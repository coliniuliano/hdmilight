/* The TV's power state, and switching the TV on and off (oneboard design).
 *
 * Whether the TV is on comes from its hot-plug line: the test TV drops hot-plug about 0.4 s after going to
 * standby and raises it again when it wakes, and hdmi.c tracks it for the TV output (tv_hotplug_track). CEC adds
 * what the TV says on the bus, as far as it can be heard.
 *
 * Hearing CEC: the frames come from the splitter's own CEC engine, which hdmi.c drains every tick and hands to
 * hl_cec_frame_in. On revision A the ESP32's receive pin (IO22, through R20) does not see the bus, so it is not used.
 * The engine is not given an address: it hears broadcasts, which is what a TV sends when it wakes.
 *
 * Sending CEC: through Q1 (IO21 high = bus low), timed by a hardware timer. The receive pin cannot be read, so a
 * frame goes out without waiting for a quiet bus or checking arbitration, and nothing tells whether it was
 * acknowledged; the TV's hot-plug shows whether a Standby or Image View On worked. Both are sent from logical
 * address 4 (playback device 1), which is what was tested on the board's TV. */
#include "oneboard.h"
#include "cec_core.h"
#include <string.h>
#include <stdio.h>
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "cec";
#define TV_OFF_AFTER_MS  500       /* hot-plug low for this long = the TV is off (the TV drops it about 0.4 s after standby) */
#define MY_ADDR          4         /* playback device 1: the initiator of the frames this board sends */

static QueueHandle_t     s_frames;
static gptimer_handle_t  s_timer;
static SemaphoreHandle_t s_tx_done, s_txm, s_lock;

/* transmit state, owned by the timer interrupt while a frame is going out */
typedef enum { PH_IDLE, PH_LOW, PH_HIGH } phase_t;
static cec_seg           s_segs[CEC_MAX_SEGS];
static volatile int      s_nseg, s_seg;
static volatile phase_t  s_phase;

static struct {
    cec_tv_state cec_tv;           /* what CEC traffic says */
    bool     follow;
    uint32_t frames, sent;
    char     last[3 * CEC_MAX_BLOCKS + 8];
} s_pub;

static void bus_low(bool low) { gpio_set_level(ONEBOARD_PIN_CEC_TX, low ? 1 : 0); }

static void alarm_at(uint64_t count)
{
    const gptimer_alarm_config_t a = { .alarm_count = count, .flags = { .auto_reload_on_alarm = false } };
    gptimer_set_alarm_action(s_timer, &a);
}

/* timer interrupt: the next step of the frame being sent */
static bool on_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *ev, void *ctx)
{
    (void)timer; (void)ctx;
    BaseType_t hp = pdFALSE;
    const uint64_t t = ev->alarm_value;
    switch (s_phase) {
    case PH_LOW:                                   /* the low time of this bit is over: release the bus */
        bus_low(false); s_phase = PH_HIGH; alarm_at(t + s_segs[s_seg].high_us);
        break;
    case PH_HIGH:
        if (++s_seg < s_nseg) { bus_low(true); s_phase = PH_LOW; alarm_at(t + s_segs[s_seg].low_us); break; }
        s_phase = PH_IDLE; gptimer_set_alarm_action(s_timer, NULL);
        xSemaphoreGiveFromISR(s_tx_done, &hp);
        break;
    default:
        gptimer_set_alarm_action(s_timer, NULL);
        break;
    }
    return hp == pdTRUE;
}

/* put one frame on the wire; false if the timer did not finish */
static bool play(const uint8_t *d, int len)
{
    xSemaphoreTake(s_txm, portMAX_DELAY);
    s_nseg = cec_tx_build(d, len, s_segs); s_seg = 0;
    xSemaphoreTake(s_tx_done, 0);
    uint64_t now;
    s_phase = PH_LOW;
    bus_low(true);
    gptimer_get_raw_count(s_timer, &now);
    alarm_at(now + s_segs[0].low_us);
    bool ok = xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(600)) == pdTRUE;          /* 16 blocks take under 400 ms */
    if (!ok) { gptimer_set_alarm_action(s_timer, NULL); bus_low(false); s_phase = PH_IDLE; ESP_LOGE(TAG, "transmit timer did not finish"); }
    xSemaphoreGive(s_txm);
    return ok;
}

static void note_frame(const cec_frame *f)
{
    char hex[sizeof s_pub.last]; int n = 0;
    for (int i = 0; i < f->len; i++) n += snprintf(hex + n, sizeof hex - n, "%s%02x", i ? ":" : "", f->data[i]);
    ESP_LOGI(TAG, "heard %s", hex);
    xSemaphoreTake(s_lock, portMAX_DELAY); strlcpy(s_pub.last, hex, sizeof s_pub.last); s_pub.frames++; xSemaphoreGive(s_lock);
}

static void cec_task(void *arg)
{
    (void)arg;
    cec_policy pol; cec_policy_init(&pol);              /* my_addr stays CEC_ADDR_NONE: it never asks to reply */
    while (1) {
        cec_frame f, reply; bool changed = false;
        if (xQueueReceive(s_frames, &f, portMAX_DELAY) != pdTRUE) continue;
        note_frame(&f);
        cec_policy_frame(&pol, &f, &reply, &changed);
        if (changed) {
            ESP_LOGI(TAG, "CEC says the TV is %s", cec_tv_state_name(pol.tv));
            xSemaphoreTake(s_lock, portMAX_DELAY); s_pub.cec_tv = pol.tv; xSemaphoreGive(s_lock);
        }
    }
}

/* ---- public ------------------------------------------------------------------------------------------- */
void hl_cec_frame_in(const uint8_t *d, int len)
{
    if (!s_frames || len < 1) return;
    cec_frame f = { 0 };
    f.len = (uint8_t)(len > CEC_MAX_BLOCKS ? CEC_MAX_BLOCKS : len);
    memcpy(f.data, d, f.len);
    xQueueSend(s_frames, &f, 0);
}

bool hl_cec_send(const uint8_t *d, int len)
{
    if (!s_txm || len < 1 || len > CEC_MAX_BLOCKS) return false;
    bool ok = play(d, len);
    if (ok) { xSemaphoreTake(s_lock, portMAX_DELAY); s_pub.sent++; xSemaphoreGive(s_lock); }
    return ok;
}

bool hl_cec_tv_power(bool on)
{
    if (!s_txm) return false;
    const uint8_t d[2] = { (uint8_t)((MY_ADDR << 4) | CEC_ADDR_TV), on ? 0x04 : 0x36 };     /* Image View On / Standby */
    bool ok = play(d, 2);
    vTaskDelay(pdMS_TO_TICKS(60));
    ok = play(d, 2) && ok;                         /* twice: no acknowledge can be read, and both are harmless repeated */
    ESP_LOGI(TAG, "sent %s to the TV%s", on ? "Image View On" : "Standby", ok ? "" : " (transmit timer failed)");
    if (ok) { xSemaphoreTake(s_lock, portMAX_DELAY); s_pub.sent++; xSemaphoreGive(s_lock); }
    return ok;
}

static void load_settings(void)
{
    nvs_handle_t h; uint8_t v;
    if (nvs_open("hdmilight", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u8(h, "cec_follow", &v) == ESP_OK) s_pub.follow = v;
    nvs_close(h);
}

void hl_cec_set_follow(bool follow)
{
    xSemaphoreTake(s_lock, portMAX_DELAY); s_pub.follow = follow; xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open("hdmilight", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "cec_follow", follow); nvs_commit(h); nvs_close(h); }
    ESP_LOGI(TAG, "LEDs %s the TV", follow ? "follow" : "ignore");
}

void hl_cec_get(hl_cec_status_t *st)
{
    const int off_ms = hl_hdmi_tv_off_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->tv = off_ms < 0 ? "unknown" : off_ms >= TV_OFF_AFTER_MS ? "off" : off_ms > 0 ? "going off" : "on";
    st->tv_cec = cec_tv_state_name(s_pub.cec_tv);
    st->follow = s_pub.follow; st->frames = s_pub.frames; st->sent = s_pub.sent;
    strlcpy(st->last, s_pub.last, sizeof st->last);
    xSemaphoreGive(s_lock);
}

bool hl_cec_leds_blank(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY); bool follow = s_pub.follow; xSemaphoreGive(s_lock);
    return follow && hl_hdmi_tv_off_ms() >= TV_OFF_AFTER_MS;
}

void hl_cec_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_txm = xSemaphoreCreateMutex();
    s_tx_done = xSemaphoreCreateBinary();
    s_frames = xQueueCreate(8, sizeof(cec_frame));
    s_pub.follow = true;
    load_settings();
    bus_low(false);
    const gptimer_config_t tc = { .clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000 };
    const gptimer_event_callbacks_t cbs = { .on_alarm = on_alarm };
    if (gptimer_new_timer(&tc, &s_timer) != ESP_OK || gptimer_register_event_callbacks(s_timer, &cbs, NULL) != ESP_OK ||
        gptimer_enable(s_timer) != ESP_OK || gptimer_start(s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer setup failed: cannot switch the TV");
        vSemaphoreDelete(s_txm); s_txm = NULL;
    }
    xTaskCreatePinnedToCore(cec_task, "cec", 4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "TV state from its hot-plug%s; CEC heard through the splitter", s_pub.follow ? ", LEDs follow it" : "");
}
