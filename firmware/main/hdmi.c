/* The on-board HDMI splitter (GSV2001) run from the ESP32-S31 over I2C.
 *
 * All chip knowledge is in gsv2001/sdk, Gscoolink's SDK recreated from product firmware (gsv2001/sdk/gsv_sdk.h
 * says where from). This file is the platform layer and the application around it: reset, I2C, the 20 ms SDK tick
 * and EDID.
 *
 * Board wiring: input = J1, output A = the TV (J2), output B = the MS2130. Output A is a pass-through of the input;
 * output B comes from the chip's Tx core, fed through the downscaler (half width and height, same frame rate) when
 * the input is 4K, because the MS2130 takes at most 4K30. The source is shown the TV's EDID without Dolby Vision.
 *
 * The library replays a GSV2001 product's firmware exactly (make -C gsv2001 check), including this board's layout.
 * No firmware found runs an output as a pass-through on one chip, so that part (page 1 reg 9 = 2) rests on the
 * driver's code, the datasheet and this board, where it shows a 4K60 picture on a TV. */
#include "oneboard.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "gsv_app.h"

static const char *TAG = "hdmi";
#define GSV_ADDR7        (GSV_I2C_ADDR >> 1)        /* 0x58 */
#define TICK_MS          20                          /* the SP12H2's SDK tick */
#define I2C_TIMEOUT_MS   50
#define LOST_AFTER_ERRS  12                          /* failed transfers in one tick = the chip is gone */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static gsv_system        s_sys;
static SemaphoreHandle_t s_lock;                     /* s_status and the EDID copies */
static hl_hdmi_status_t  s_status;
static uint8_t           s_edid_shown[256];          /* what was last programmed into the Rx, before the SDK's fix-ups */
static bool              s_edid_from_tv;
static volatile bool     s_tv_event, s_ms_event;     /* connect events from the SDK (output A / output B) */
/* How the splitter is set up. All start from the build's options and can be changed while running (hl_hdmi_setup),
 * which starts the splitter again from its reset. */
#ifdef CONFIG_HL_ONEBOARD_TV_PASSTHROUGH
static volatile uint8_t  s_opt_passthrough = 1;      /* output A carries the input as it is, without the Tx core */
#else
static volatile uint8_t  s_opt_passthrough = 0;
#endif
#ifdef CONFIG_HL_ONEBOARD_SCALE_CAPTURE
static volatile uint8_t  s_opt_scale = 1;            /* the MS2130 gets 4K inputs downscaled to 1080p */
#else
static volatile uint8_t  s_opt_scale = 0;
#endif
#ifdef CONFIG_HL_ONEBOARD_TV_HDCP
static volatile uint8_t  s_opt_tv_hdcp = 1;          /* HDCP towards the TV when the input has it */
#else
static volatile uint8_t  s_opt_tv_hdcp = 0;          /* never try HDCP towards the TV (as output B never does) */
#endif
static volatile uint8_t  s_opt_drive = CONFIG_HL_ONEBOARD_TV_DRIVE;   /* lowest driver setting the TV output may use */
static volatile bool     s_restart;
static int               s_tick_errs;
static uint32_t          s_i2c_errs;

/* ---- SDK platform -------------------------------------------------------------------------------------- */
static int pf_read(uint8_t dev8, uint16_t reg16, uint8_t *data, uint16_t len)
{
    const uint8_t a[2] = { (uint8_t)(reg16 >> 8), (uint8_t)reg16 };
    if (dev8 == GSV_I2C_ADDR)
        for (int try = 0; try < 3; try++)                 /* the original retries three times as well */
            if (i2c_master_transmit_receive(s_dev, a, 2, data, len, I2C_TIMEOUT_MS) == ESP_OK) return 0;
    memset(data, 0, len);
    s_tick_errs++; s_i2c_errs++;
    return -1;
}

static int pf_write(uint8_t dev8, uint16_t reg16, uint8_t *data, uint16_t len)
{
    uint8_t buf[2 + 256];
    if (dev8 == GSV_I2C_ADDR && len <= 256) {
        buf[0] = (uint8_t)(reg16 >> 8); buf[1] = (uint8_t)reg16;
        memcpy(buf + 2, data, len);
        for (int try = 0; try < 3; try++)
            if (i2c_master_transmit(s_dev, buf, (size_t)len + 2, I2C_TIMEOUT_MS) == ESP_OK) return 0;
    }
    s_tick_errs++; s_i2c_errs++;
    return -1;
}

static const gsv_platform s_platform = { pf_read, pf_write };

/* the SDK's debug output (the original printed it on its UART): one log line per call */
void gsv_log(const char *fmt, ...)
{
    char b[160];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    char *s = b;
    while (*s == '\t' || *s == '\r' || *s == '\n' || *s == ' ') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0;
    if (n) ESP_LOGI("gsv", "%s", s);
}

void gsv_delay_ms(unsigned ms) { vTaskDelay(pdMS_TO_TICKS(ms) + 1); }

/* a path of the original that was never ported: say so and carry on (the LEDs must not stop for it) */
void gsv_todo(const char *what, const char *where)
{
    ESP_LOGE("gsv", "unported SDK path reached: %s (in %s)", what, where);
}

/* ---- what happened before a stuck input -------------------------------------------------------------------
 * The splitter's input once stayed unlocked for minutes with a source present, the source restarting HDCP every 2.4 s,
 * until the splitter was restarted (2026-11-21, after the board started with the TV off). The cause is not known.
 * So the splitter task keeps a short history of the events that could matter, and when a source has been present for
 * STUCK_MS without a picture it writes that history and the chip's input registers to NVS once, readable with
 * GET /api/incident even after a restart. */
#define STUCK_MS  20000
#define HIST_N    24
static struct { uint32_t ms; char txt[44]; } s_hist[HIST_N];
static int s_hist_n;                                   /* total added; the ring holds the last HIST_N */
static SemaphoreHandle_t s_hist_lock;
static void hist(const char *fmt, ...)
{
    if (!s_hist_lock) return;
    char t[44]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    xSemaphoreTake(s_hist_lock, portMAX_DELAY);
    int i = s_hist_n++ % HIST_N;
    s_hist[i].ms = (uint32_t)(esp_timer_get_time() / 1000); strlcpy(s_hist[i].txt, t, sizeof s_hist[i].txt);
    xSemaphoreGive(s_hist_lock);
}
static int hist_dump(char *out, size_t n)
{
    int k = 0;
    xSemaphoreTake(s_hist_lock, portMAX_DELAY);
    int first = s_hist_n > HIST_N ? s_hist_n - HIST_N : 0;
    for (int j = first; j < s_hist_n && k < (int)n; j++)
        k += snprintf(out + k, n - k, "%8.1f s  %s\n", s_hist[j % HIST_N].ms / 1000.0, s_hist[j % HIST_N].txt);
    xSemaphoreGive(s_hist_lock);
    return k;
}

/* write the history and the input's registers to NVS ("incident"), once per stuck spell */
static void incident_capture(uint32_t stuck_ms, uint32_t hdcp_restarts)
{
    char *t = malloc(1800); if (!t) return;
    int k = snprintf(t, 1800, "input unlocked for %lu s with a source present; HDCP restarted %lu times meanwhile\n"
                     "TV output %s, source shown %s EDID\n", (unsigned long)(stuck_ms / 1000), (unsigned long)hdcp_restarts,
                     s_sys.txa.status->plug == 5 ? "connected" : "not connected", s_edid_from_tv ? "the TV's" : "the built-in");
    static const struct { uint8_t page, reg, n; } regs[] = {
        { 0x20, 0x00, 4 }, { 0x20, 0x46, 1 }, { 0x27, 0x92, 5 }, { 0x28, 0x92, 5 }, { 0x29, 0x92, 5 },
        { 0x03, 0x10, 2 }, { 0x03, 0x30, 16 }, { 0x03, 0x50, 16 }, { 0x00, 0xb0, 16 }, { 0x21, 0x00, 8 },
    };
    for (unsigned r = 0; r < sizeof regs / sizeof regs[0] && k < 1700; r++) {
        k += snprintf(t + k, 1800 - k, "%02x.%02x:", regs[r].page, regs[r].reg);
        for (int i = 0; i < regs[r].n; i++) { uint8_t v = 0; gsv_read_bits(GSV_PAGE(regs[r].page), (uint8_t)(regs[r].reg + i), 0xff, 0, &v); k += snprintf(t + k, 1800 - k, " %02x", v); }
        k += snprintf(t + k, 1800 - k, "\n");
    }
    k += snprintf(t + k, 1800 - k, "history (board uptime):\n");
    hist_dump(t + k, 1800 - k);
    nvs_handle_t h;
    if (nvs_open("hdmilight", NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, "incident", t); nvs_commit(h); nvs_close(h); }
    ESP_LOGE(TAG, "input stuck: details saved, GET /api/incident\n%s", t);
    free(t);
}

int hl_hdmi_incident(char *out, size_t n)
{
    nvs_handle_t h; size_t len = n;
    if (nvs_open("hdmilight", NVS_READONLY, &h) != ESP_OK) return snprintf(out, n, "none recorded\n");
    esp_err_t e = nvs_get_str(h, "incident", out, &len); nvs_close(h);
    return e == ESP_OK ? (int)strlen(out) : snprintf(out, n, "none recorded\n");
}

/* output connect / disconnect: which = port id - 3, so 1 = output A (TV), 2 = output B (MS2130) */
void gsv_app_tx_event(int which, int on)
{
    ESP_LOGI(TAG, "output %c (%s) %s", which == 1 ? 'A' : 'B', which == 1 ? "TV" : "MS2130", on ? "connected" : "disconnected");
    if (which == 1) s_tv_event = on; else if (which == 2) s_ms_event = on;
    hist("output %c %s", which == 1 ? 'A' : 'B', on ? "connected" : "disconnected");
}

void gsv_app_rx_event(int ev, int on)
{
    (void)ev;
    ESP_LOGI(TAG, "HDMI input %s", on ? "plugged" : "lost");
    hist("input %s", on ? "plugged" : "lost");
}

/* ---- finding the chip ---------------------------------------------------------------------------------- */
static void reset_pulse(void)
{
    gpio_set_level(ONEBOARD_PIN_GSV_RESETB, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(ONEBOARD_PIN_GSV_RESETB, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void scan_bus(void)
{
    char line[160]; int n = 0, found = 0;
    for (int a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(s_bus, a, 20) == ESP_OK) { found++; if (n < (int)sizeof line - 6) n += snprintf(line + n, sizeof line - n, " 0x%02x", a); }
    if (found) ESP_LOGW(TAG, "I2C devices answering:%s", line);
    else ESP_LOGW(TAG, "nothing answers on I2C (SDA IO%d, SCL IO%d): check the 27 MHz crystal Y1, the 1.2 V and 3.3 V rails and U2's centre pad",
                  ONEBOARD_PIN_GSV_SDA, ONEBOARD_PIN_GSV_SCL);
}

/* Reset the chip and look for it at 0x58. R27 holds CSB low, which is what it needs to answer on I2C. */
static bool find_chip(void)
{
    reset_pulse();
    if (i2c_master_probe(s_bus, GSV_ADDR7, I2C_TIMEOUT_MS) == ESP_OK) {
        ESP_LOGI(TAG, "GSV2001 answers at 0x%02x", GSV_ADDR7);
        return true;
    }
    scan_bus();
    return false;
}

/* ---- EDID: show the source what the TV can do ---------------------------------------------------------- */
static bool edid_valid(const uint8_t *e)
{
    static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (memcmp(e, hdr, 8)) return false;
    uint8_t s = 0; for (int i = 0; i < 128; i++) s = (uint8_t)(s + e[i]);
    if (s) return false;
    if (e[0x7e]) { s = 0; for (int i = 128; i < 256; i++) s = (uint8_t)(s + e[i]); if (s) return false; }
    return true;
}

/* The SDK keeps two blocks of the sink's EDID. A TV with more extension blocks than one is presented as base block
 * plus its first extension, which is where the CEA data block collection and the HDMI blocks are. */
static void edid_clamp_to_two_blocks(uint8_t *e)
{
    if (e[0x7e] <= 1) return;
    e[0x7e] = 1;
    uint8_t s = 0; for (int i = 0; i < 127; i++) s = (uint8_t)(s + e[i]);
    e[0x7f] = (uint8_t)-s;
}

static void edid_follow_tv(void)
{
    gsv_port *tv = &s_sys.txa;
    if (!s_tv_event || tv->status->plug != 5 || tv->status->hdcp_mode != 1) return;   /* the SP12H2's own "output ready" test */
    uint8_t e[256];
    memcpy(e, gsv_tx_edid[0], 256);
    if (!edid_valid(e)) {
        static bool said;
        if (!said) { said = true; ESP_LOGW(TAG, "the TV's EDID did not read cleanly: the source keeps the built-in one"); }
        return;
    }
    edid_clamp_to_two_blocks(e);
    if (s_sys.scaler_txa || s_sys.scaler_txb) gsv_edid_strip_dolby_vision(e);   /* only a downscaled output cannot pass it (SP12H2 behaviour) */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool same = s_edid_from_tv && memcmp(e, s_edid_shown, 256) == 0;
    if (!same) { memcpy(s_edid_shown, e, 256); s_edid_from_tv = true; }
    xSemaphoreGive(s_lock);
    if (same) return;
    ESP_LOGI(TAG, "showing the source the TV's EDID (%c%c%c, %u extension block%s)",
             '@' + ((e[8] >> 2) & 0x1f), '@' + (((e[8] & 3) << 3) | (e[9] >> 5)), '@' + (e[9] & 0x1f), e[0x7e], e[0x7e] == 1 ? "" : "s");
    hist("source shown the TV's EDID");
    memcpy(s_sys.edid, e, 256);
    gsv_rx_set_edid(&s_sys.rx0, s_sys.edid);
}

/* ---- status -------------------------------------------------------------------------------------------- */
static void snapshot(bool found, uint16_t tick_ms)
{
    hl_hdmi_status_t st = { .found = found, .i2c_addr = GSV_ADDR7, .tick_ms = tick_ms, .i2c_errors = s_i2c_errs };
    st.int_level = gpio_get_level(ONEBOARD_PIN_GSV_INT);
    if (found) {
        const gsv_port *rx = &s_sys.rx0;
        st.rx_5v = rx->rx->cable_5v; st.rx_locked = rx->rx->locked == 1; st.rx_hdmi = rx->rx->hdmi_mode; st.rx_encrypted = rx->rx->encrypted == 1;
        st.rx_w = rx->video->hactive; st.rx_h = rx->video->vactive; st.rx_fps = rx->video->frame_rate; st.rx_pixclk = rx->video->pixclk;
        st.rx_vic = rx->video->vic; st.rx_colorspace = rx->video->colorspace; st.rx_depth = rx->video->depth_code;
        const gsv_port *tx[2] = { &s_sys.txa, &s_sys.txb };
        for (int i = 0; i < 2; i++) {
            st.tx[i].plug = tx[i]->status->plug;
            st.tx[i].connected = tx[i]->status->plug == 5;
            st.tx[i].hdmi = tx[i]->status->hdmi_mode;
            st.tx[i].hdcp = tx[i]->hdcp_tx->authenticated == 1;
            st.tx[i].scaled = tx[i]->source == &s_sys.scaler;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st.ticks = found ? s_status.ticks + 1 : 0;
    st.edid_from_tv = s_edid_from_tv;
    s_status = st;
    xSemaphoreGive(s_lock);
}

void hl_hdmi_get_status(hl_hdmi_status_t *s)
{
    if (!s_lock) { memset(s, 0, sizeof *s); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *s = s_status;
    xSemaphoreGive(s_lock);
}

size_t hl_hdmi_get_edid(int which, uint8_t *out)
{
    if (!s_lock) return 0;
    hl_hdmi_status_t st; hl_hdmi_get_status(&st);
    if (!st.found) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* the sink copies are written by the splitter task without this lock: a read during a hot-plug can be torn */
    memcpy(out, which == 1 ? gsv_tx_edid[0] : which == 2 ? gsv_tx_edid[1] : s_sys.edid, 256);
    xSemaphoreGive(s_lock);
    return 256;
}

/* ---- for the serial console: poke at the bus while the SDK runs (the I2C driver serialises access) ---------- */
int hl_hdmi_i2c_scan(char *out, size_t len)
{
    int n = 0, found = 0;
    out[0] = 0;
    if (!s_bus) return -1;
    for (int a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(s_bus, a, 20) == ESP_OK) { found++; if (n < (int)len - 6) n += snprintf(out + n, len - n, "%s0x%02x", n ? " " : "", a); }
    return found;
}

esp_err_t hl_hdmi_reg(uint8_t page, uint8_t reg, uint8_t *val, bool write)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    const uint8_t w[3] = { page, reg, *val };
    return write ? i2c_master_transmit(s_dev, w, 3, I2C_TIMEOUT_MS) : i2c_master_transmit_receive(s_dev, w, 2, val, 1, I2C_TIMEOUT_MS);
}

/* ---- CEC through the splitter's own engine -----------------------------------------------------------------
 * The GSV2001 has a CEC engine on page 0x41, laid out like Analog Devices' ADV76xx one (worked out from the CEC code in
 * Kramer's GSV2001 driver). Its pin reaches the bus through R18 (100k), so it listens; with no logical address set
 * it keeps broadcasts. 0x4e bits 1:0 = 1 switches it on; 0x49 bits 0..2 = receive buffer n holds a frame; 0x26 has
 * a 2-bit arrival stamp per buffer (oldest first); a buffer is 17 bytes at 0x15 / 0x27 / 0x38, the last one & 0x1f
 * the length; pulsing 0x4a bit n frees buffer n. */
static void cec_engine_start(void) { gsv_write_bits(GSV_PAGE(0x41), 0x4e, 0x03, 0, 1); }
static void cec_engine_poll(void)
{
    uint8_t ready = 0;
    if (gsv_read_bits(GSV_PAGE(0x41), 0x49, 0x07, 0, &ready) != 0) return;
    while (ready) {
        uint8_t st = 0; gsv_read_bits(GSV_PAGE(0x41), 0x26, 0x3f, 0, &st);
        int pick = -1; uint8_t best = 0xff;
        for (int n = 0; n < 3; n++) {
            uint8_t stamp = (st >> (2 * n)) & 3;
            if ((ready & (1 << n)) && stamp && stamp < best) { best = stamp; pick = n; }
        }
        if (pick < 0) for (int n = 0; n < 3; n++) if (ready & (1 << n)) { pick = n; break; }
        static const uint8_t base[3] = { 0x15, 0x27, 0x38 };
        uint8_t b[17];
        for (int i = 0; i < 17; i++) gsv_read_bits(GSV_PAGE(0x41), (uint8_t)(base[pick] + i), 0xff, 0, &b[i]);
        int len = b[16] & 0x1f;
        if (len > 0 && len <= 16) hl_cec_frame_in(b, len);
        gsv_write_bits(GSV_PAGE(0x41), 0x4a, (uint8_t)(1 << pick), (uint8_t)pick, 1);
        gsv_write_bits(GSV_PAGE(0x41), 0x4a, (uint8_t)(1 << pick), (uint8_t)pick, 0);
        ready &= (uint8_t)~(1 << pick);
    }
}

/* the TV output's hot-plug, for the TV's power state (cec.c) */
static volatile int64_t s_tv_low_since = -1;      /* ms; -1 while the splitter is not running, 0 while hot-plug is high */
int hl_hdmi_tv_off_ms(void)
{
    const int64_t t = s_tv_low_since;
    return t < 0 ? -1 : t == 0 ? 0 : (int)(esp_timer_get_time() / 1000 - t);
}
static void tv_hotplug_track(void)
{
    uint8_t hpd = 0;
    if (gsv_read_bits(GSV_PAGE(0x60), 0xc5, 0x10, 4, &hpd) != 0) return;       /* the driver's own hot-plug bit */
    if (hpd) s_tv_low_since = 0;
    else if (s_tv_low_since <= 0) s_tv_low_since = esp_timer_get_time() / 1000;
}

/* ---- the task ------------------------------------------------------------------------------------------ */
static void status_led(bool found, bool locked)
{
    /* solid = picture locked; slow blink = running, no picture; fast blink = splitter not found */
    int64_t ms = esp_timer_get_time() / 1000;
    gpio_set_level(ONEBOARD_PIN_STATUS_LED, locked ? 1 : found ? (ms / 500) & 1 : (ms / 100) & 1);
}

static void hdmi_task(void *arg)
{
    const i2c_master_bus_config_t bus = {
        .i2c_port = -1, .sda_io_num = ONEBOARD_PIN_GSV_SDA, .scl_io_num = ONEBOARD_PIN_GSV_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = false },          /* R33/R34 on the board */
    };
    const i2c_device_config_t dev = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = GSV_ADDR7, .scl_speed_hz = CONFIG_HL_ONEBOARD_I2C_HZ };
    if (i2c_new_master_bus(&bus, &s_bus) != ESP_OK || i2c_master_bus_add_device(s_bus, &dev, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus setup failed: the splitter is not being run");
        vTaskDelete(NULL);
    }
    gsv_platform_init(&s_platform);

    while (1) {
        s_tv_low_since = -1;                      /* unknown until the splitter runs again */
        while (!find_chip()) {
            snapshot(false, 0);
            for (int i = 0; i < 30; i++) { status_led(false, false); vTaskDelay(pdMS_TO_TICKS(100)); }
        }
        /* start the SDK from scratch (also after the chip was lost: its port list is global) */
        g_gsv_port_list = g_gsv_port_tail = NULL; g_chip_state[0] = g_chip_state[1] = NULL;
        memset(&s_sys, 0, sizeof s_sys);
        s_sys.scaler_txa = 0;                     /* the TV gets the picture untouched */
        s_sys.scaler_txb = s_opt_scale;
        s_sys.passthrough_txa = s_opt_passthrough;
        s_sys.no_hdcp_txa = !s_opt_tv_hdcp;
        s_restart = false;
        s_tv_event = s_ms_event = false; s_tick_errs = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY); s_edid_from_tv = false; xSemaphoreGive(s_lock);
        gsv_k1_init_4k60 = 1;                     /* without it a 4K60 HDCP 2.2 input keeps losing vertical lock (gsv_core.c) */
        gsv_k1_tx_drive_floor[0] = s_opt_drive;   /* output A (the HDMI connector): gsv_tx.c says what was measured */
        gsv_app_edid_name = CONFIG_HL_ONEBOARD_EDID_NAME;   /* what a source calls this board while no TV's EDID is passed on */
        gsv_system_init(&s_sys);                  /* chip init tables, ports, links, built-in EDID */
        cec_engine_start();
        if (s_tick_errs) { ESP_LOGE(TAG, "%d I2C transfers failed during chip init", s_tick_errs); }
        ESP_LOGI(TAG, "splitter running (GSV2001): output A %s, output B %s", s_sys.passthrough_txa ? "passes the input through" : "on the Tx core",
                 s_sys.scaler_txb ? "downscaled for 4K inputs" : "not scaled");
        ESP_LOGI(TAG, "TV output: %s, driver setting at least %d", s_opt_tv_hdcp ? "HDCP when the input has it" : "no HDCP", s_opt_drive);

        hist("splitter started");
        TickType_t last = xTaskGetTickCount();
        bool was_locked = false, was_5v = false, was_enc = false, captured = false;
        int64_t unlocked_since = 0; uint32_t hdcp_restarts = 0;
        while (1) {
            int64_t t0 = esp_timer_get_time();
            s_tick_errs = 0;
            edid_follow_tv();
            gsv_fsm_run();
            gsv_app_route_update(&s_sys);
            cec_engine_poll();
            tv_hotplug_track();
            uint16_t took = (uint16_t)((esp_timer_get_time() - t0) / 1000);
            if (s_restart) { ESP_LOGW(TAG, "splitter set-up changed: starting it again"); break; }
            if (s_tick_errs >= LOST_AFTER_ERRS) { ESP_LOGE(TAG, "the splitter stopped answering (%d failed transfers in one tick): starting over", s_tick_errs); break; }
            snapshot(true, took);
            bool locked = s_sys.rx0.rx->locked == 1;
            if (locked != was_locked) {
                was_locked = locked;
                if (locked) ESP_LOGI(TAG, "input locked: %ux%u, %u Hz, VIC %u, colour space %u%s", s_sys.rx0.video->hactive, s_sys.rx0.video->vactive,
                                     s_sys.rx0.video->frame_rate, s_sys.rx0.video->vic, s_sys.rx0.video->colorspace, s_sys.rx0.rx->encrypted == 1 ? ", HDCP" : "");
                else ESP_LOGI(TAG, "input unlocked");
                hist(locked ? "input locked %ux%u %u Hz" : "input unlocked", s_sys.rx0.video->hactive, s_sys.rx0.video->vactive, s_sys.rx0.video->frame_rate);
            }
            const bool v5 = s_sys.rx0.rx->cable_5v, enc = s_sys.rx0.rx->encrypted == 1;
            if (v5 != was_5v) { was_5v = v5; hist(v5 ? "source 5 V on" : "source 5 V off"); }
            if (enc != was_enc) { was_enc = enc; if (enc) hdcp_restarts++; if (!captured && hdcp_restarts <= 3) hist(enc ? "input HDCP on" : "input HDCP off"); }
            /* a source present without a picture for STUCK_MS: keep what led to it */
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (locked || !v5) { unlocked_since = 0; hdcp_restarts = 0; captured = false; }
            else if (!unlocked_since) unlocked_since = now_ms;
            else if (!captured && now_ms - unlocked_since >= STUCK_MS) { captured = true; incident_capture((uint32_t)(now_ms - unlocked_since), hdcp_restarts); }
            status_led(true, locked);
            /* a tick that overran its 20 ms still yields, so a slow bus cannot starve lower-priority work */
            if (xTaskGetTickCount() - last >= pdMS_TO_TICKS(TICK_MS)) { vTaskDelay(1); last = xTaskGetTickCount(); }
            else vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
        }
    }
}

void hl_hdmi_setup(int passthrough, int scale, int tv_hdcp, int drive)
{
    if (tv_hdcp >= 0) s_opt_tv_hdcp = tv_hdcp != 0;
    if (passthrough >= 0) s_opt_passthrough = passthrough != 0;
    if (scale >= 0) s_opt_scale = scale != 0;
    if (drive >= 0 && drive <= 2) s_opt_drive = (uint8_t)drive;
    s_restart = true;
}

void hl_hdmi_get_setup(int *passthrough, int *scale, int *tv_hdcp, int *drive)
{
    *passthrough = s_opt_passthrough; *scale = s_opt_scale; *tv_hdcp = s_opt_tv_hdcp; *drive = s_opt_drive;
}

void hl_hdmi_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_hist_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(hdmi_task, "hdmi", 8192, NULL, 6, NULL, 1);
}
