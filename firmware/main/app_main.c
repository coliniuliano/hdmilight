/* hdmilight-v3: MS2130 MJPEG over USB -> hardware JPEG decode -> hl_core pipeline -> WS281x.
 *
 * Core 0: USB host, UVC driver, decode+pipeline task.
 * Core 1: LED output task at CONFIG_HL_OUTPUT_HZ.
 * Runtime configuration lives in NVS and is edited through the web UI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "logbuf.h"
#include "crashlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/jpeg_decode.h"

#include "hl_pipeline.h"
#if __has_include("model_calibrated.h")
#include "model_calibrated.h"
#endif
#include "capture.h"
#include "leds.h"
#include "config.h"
#include "app.h"
#include "board.h"
#if CONFIG_HL_WIFI
#include "net.h"
#include "web.h"
#include "homeassistant.h"
#include "netwatch.h"
#endif

static const char *TAG = "hdmilight";

#define FRAME_W   CONFIG_HL_CAPTURE_WIDTH
#define FRAME_H   CONFIG_HL_CAPTURE_HEIGHT
#define PAD16(v)  (((v) + 15u) & ~15u)
#define MAX_LEDS  512

static hl_cfg_t           s_cfg;
static hl_pipeline        s_pipe;
static bool               s_pipe_ok;
static SemaphoreHandle_t  s_lock;            /* pipeline + config + LED driver */
static int                s_n_leds;
static hl_led             s_leds[MAX_LEDS];
static uint8_t            s_rgb[MAX_LEDS * 3];  /* last colours written to the strip */
static volatile int64_t   s_last_frame_ms;
static volatile bool      s_no_signal;
static hl_status_t        s_stat;            /* rolling stats, updated by the decode task */
static hl_model           s_model;
extern const uint8_t      lut_baked_start[] asm("_binary_lut_baked_bin_start");   /* baked at build, see CMakeLists.txt */
extern const uint8_t      lut_baked_end[]   asm("_binary_lut_baked_bin_end");
static SemaphoreHandle_t  s_frame_lock;      /* newest JPEG for the web preview */
static uint8_t           *s_jpeg;            /* PSRAM, FRAME_W*FRAME_H*2 bytes */
static size_t             s_jpeg_len;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void mark_healthy(void);

static void output_cfg(const hl_cfg_t *c, hl_output_cfg *o)
{
    hl_output_cfg_default(o);
    o->temp = (hl_temp_preset)c->temp_preset;
    o->temp_tint = hl_f3_make(c->temp_rgb[0] / 100.f, c->temp_rgb[1] / 100.f, c->temp_rgb[2] / 100.f);
    o->classic = c->classic;
    memcpy(o->cal, c->cal, sizeof o->cal);
    o->gamma = c->gamma_x100 / 100.f;
    o->luminance = c->luminance_x100 / 100.f;
    o->saturation = c->saturation_x100 / 100.f;
    o->scale = c->scale_x100 / 100.f;
    o->backlight = c->backlight_x10000 / 10000.f;
    o->backlight_colored = c->backlight_colored;
    o->power_limit = c->power_limit_pct / 100.f;
}

/* ---- pipeline / strip (re)build from config; caller holds s_lock ---------- */
static int build_pipeline(const hl_cfg_t *c)
{
    if (s_pipe_ok) { hl_pipeline_free(&s_pipe); s_pipe_ok = false; }
    s_n_leds = hl_zones_layout_classic(s_leds, MAX_LEDS, c->top, c->right, c->bottom, c->left, c->depth_pct / 100.0f);
    hl_pipeline_cfg pc; hl_pipeline_cfg_default(&pc);
    pc.block = c->block;
    pc.hdr_mode = c->hdr_mode == 1 ? HL_HDR_ON : c->hdr_mode == 2 ? HL_HDR_OFF : HL_HDR_AUTO;
    pc.blackborder = c->blackborder;
    pc.lut_n = CONFIG_HL_TONEMAP_LUT_N;
    pc.lut_defer = true;          /* the LUT is baked at build time and lives in flash */
    pc.edge_only = true;
    output_cfg(c, &pc.out);
#if __has_include("model_calibrated.h")
    hl_model_calibrated(&s_model);   /* HyperHDR LUT calibration of this splitter + grabber (tools/bestresult_to_c.py) */
#else
    hl_model_default(&s_model);      /* HyperHDR's default HDR model; run the calibration to replace it */
#endif
    int rc = hl_pipeline_init(&s_pipe, &pc, &s_model, s_leds, s_n_leds, FRAME_W, FRAME_H);
    if (rc == 0) {
        s_pipe_ok = true;
        hl_smooth_set_springiness(&s_pipe.smooth, c->smooth_stiffness, c->smooth_damping);
        const size_t n = CONFIG_HL_TONEMAP_LUT_N;
        /* The table is baked into flash at build time but used from PSRAM: 4800 random lookups per frame miss the
         * flash cache constantly, and PSRAM is far cheaper to hit. One 108 KB copy at init. */
        size_t bytes = n * n * n * 3;
        uint8_t *lut = ((size_t)(lut_baked_end - lut_baked_start) == bytes) ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM) : NULL;
        if (lut) { memcpy(lut, lut_baked_start, bytes); hl_pipeline_set_lut(&s_pipe, lut, (int)n); }
        else if ((size_t)(lut_baked_end - lut_baked_start) == bytes) hl_pipeline_set_lut_static(&s_pipe, lut_baked_start, (int)n);
        else ESP_LOGE(TAG, "baked LUT size mismatch (%u bytes), tone-mapping from the model", (unsigned)(lut_baked_end - lut_baked_start));
    }
    ESP_LOGI(TAG, "pipeline rc=%d: %d LEDs, grid %dx%d, hdr %d, lut %d^3", rc, s_n_leds, s_pipe.gw, s_pipe.gh, c->hdr_mode, CONFIG_HL_TONEMAP_LUT_N);
    return rc;
}

static void build_strip(const hl_cfg_t *c)
{
    hl_leds_init(c->gpio, s_n_leds, (hl_led_type_t)c->led_type, (hl_color_order_t)c->color_order);
    hl_leds_set_brightness(c->brightness_pct);
}

/* ---- app API (web UI) ---------------------------------------------------- */
const hl_cfg_t *hl_app_cfg(void) { return &s_cfg; }

void hl_app_get_status(hl_status_t *s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *s = s_stat;
    s->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s->usb_connected = hl_capture_connected();
    s->frames_total = hl_capture_frames_total();
    if (!hl_capture_format(&s->cap_w, &s->cap_h, &s->cap_fps)) { s->cap_w = s->cap_h = 0; s->cap_fps = 0; }
    s->hdr_active = s_pipe_ok && s_pipe.hdr_active;
    s->border_h = (s_pipe_ok && !s_pipe.bb.current.unknown) ? s_pipe.bb.current.horizontalSize : -1;
    s->border_v = (s_pipe_ok && !s_pipe.bb.current.unknown) ? s_pipe.bb.current.verticalSize : -1;
    s->no_signal = s_no_signal;
    s->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s->free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s->n_leds = s_n_leds;
    s->out_mode = s_cfg.out_mode; memcpy(s->out_rgb, s_cfg.out_rgb, 3);
    s->grid_w = s_pipe_ok ? s_pipe.gw : 0;
    s->grid_h = s_pipe_ok ? s_pipe.gh : 0;
    xSemaphoreGive(s_lock);
}

int hl_app_get_leds(uint8_t *rgb, int max_leds)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_n_leds < max_leds ? s_n_leds : max_leds;
    memcpy(rgb, s_rgb, (size_t)n * 3);
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t hl_app_apply_config(const hl_cfg_t *in)
{
    hl_cfg_t fixed = *in; const hl_cfg_t *c = &fixed;
    if (HL_BOARD_LED_GPIO >= 0) fixed.gpio = HL_BOARD_LED_GPIO;   /* the one place every config change passes through */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool layout = c->top != s_cfg.top || c->right != s_cfg.right || c->bottom != s_cfg.bottom || c->left != s_cfg.left ||
                  c->depth_pct != s_cfg.depth_pct || c->block != s_cfg.block ||
                  c->hdr_mode != s_cfg.hdr_mode || c->blackborder != s_cfg.blackborder;
    bool strip = layout || c->gpio != s_cfg.gpio || c->led_type != s_cfg.led_type || c->color_order != s_cfg.color_order;
    s_cfg = *c;
    int rc = 0;
    if (layout) rc = build_pipeline(&s_cfg);
    else if (s_pipe_ok) {
        hl_smooth_set_springiness(&s_pipe.smooth, c->smooth_stiffness, c->smooth_damping);
        hl_output_cfg oc; output_cfg(c, &oc);
        hl_pipeline_set_output(&s_pipe, &oc);
    }
    if (strip) build_strip(&s_cfg); else hl_leds_set_brightness(c->brightness_pct);
    xSemaphoreGive(s_lock);
    hl_cfg_save(&s_cfg);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

size_t hl_app_copy_frame(uint8_t *dst, size_t cap)
{
    if (!s_frame_lock) return 0;
    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    size_t n = s_jpeg_len <= cap ? s_jpeg_len : 0;
    if (n) memcpy(dst, s_jpeg, n);
    xSemaphoreGive(s_frame_lock);
    return n;
}

esp_err_t hl_app_set_output(int mode, const uint8_t rgb[3])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.out_mode = (uint8_t)mode;
    if (rgb) memcpy(s_cfg.out_rgb, rgb, 3);
    hl_cfg_t c = s_cfg;
    xSemaphoreGive(s_lock);
    return hl_cfg_save(&c);
}

void hl_app_selftest(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hl_leds_selftest();
    xSemaphoreGive(s_lock);
}

/* ---- decode + pipeline (core 0) ---------------------------------------- */
static void decode_task(void *arg)
{
    jpeg_decoder_handle_t dec = NULL;
    jpeg_decode_engine_cfg_t eng = { .timeout_ms = 100 };
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&eng, &dec));

    size_t out_cap = 0;
    jpeg_decode_memory_alloc_cfg_t ocfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    uint8_t *out = jpeg_alloc_decoder_mem(PAD16(FRAME_W) * PAD16(FRAME_H) * 3 / 2, &ocfg, &out_cap);   /* packed 4:2:0 */
    size_t in_cap = 0;
    jpeg_decode_memory_alloc_cfg_t icfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    uint8_t *in = jpeg_alloc_decoder_mem((size_t)FRAME_W * FRAME_H * 2, &icfg, &in_cap);
    if (!out || !in) { ESP_LOGE(TAG, "decoder buffers alloc failed"); vTaskDelete(NULL); }

    /* Decode to packed 4:2:0 (1.5 bytes/pixel) instead of RGB888: half the PSRAM traffic for the box filter, and the
     * YUV->RGB conversion happens once per grid cell instead of once per pixel. The P4's 4:2:2 MJPEG -> 4:2:0 path
     * needs chip rev >= 3.0 (CONFIG_ESP32P4_REV_MIN). */
    const jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_YUV420,
    };

    uint32_t frames = 0, decode_fail = 0;
    int64_t last_report = now_ms(), decode_us_acc = 0, pipe_us_acc = 0;
    bool info_logged = false;
    /* CPU load per core = time not spent in that core's idle task (run-time counters are esp_timer microseconds) */
    TaskHandle_t idle[2] = { xTaskGetIdleTaskHandleForCore(0), xTaskGetIdleTaskHandleForCore(1) };
    uint32_t idle_prev[2] = { 0, 0 }; int64_t cpu_prev_us = esp_timer_get_time();
    for (int c = 0; c < 2; c++) { TaskStatus_t ts; vTaskGetInfo(idle[c], &ts, pdFALSE, eRunning); idle_prev[c] = ts.ulRunTimeCounter; }

    while (1) {
        hl_capture_frame_t f;
        if (!hl_capture_take_newest(&f, pdMS_TO_TICKS(2000))) {
            if (!hl_capture_connected()) ESP_LOGW(TAG, "no grabber");
            continue;
        }
        /* Decode at most CONFIG_HL_CAPTURE_FPS per second: after a hot-plug the dongle has been seen streaming
         * 60 fps against a 30 fps request, and decoding every frame just burns CPU. */
        static int64_t last_decode_us;
        int64_t tnow = esp_timer_get_time();
        if (tnow - last_decode_us < (1000000 * 3) / (4 * CONFIG_HL_CAPTURE_FPS)) { hl_capture_release(&f); continue; }   /* 3/4 of a frame period: tolerates jitter, halves a doubled rate */
        last_decode_us = tnow;
        const uvc_host_frame_t *fr = f.frame;
        size_t len = (fr->data_len <= in_cap) ? hl_jpeg_trim(fr->data, fr->data_len) : 0;
        if (len == 0) {
            static uint32_t bad;
            if (++bad == 1 || bad % 100 == 0) ESP_LOGW(TAG, "invalid/oversized MJPEG frame: %u bytes (x%" PRIu32 ")", (unsigned)fr->data_len, bad);
            hl_capture_release(&f);
            continue;
        }
        memcpy(in, fr->data, len);
        hl_capture_release(&f);

        if (!info_logged) {
#if !CONFIG_HL_WIFI
            mark_healthy();
#endif
            jpeg_decode_picture_info_t info;
            if (jpeg_decoder_get_info(in, len, &info) == ESP_OK) {
                ESP_LOGI(TAG, "first MJPEG frame: %" PRIu32 "x%" PRIu32 " %s, %u bytes", info.width, info.height,
                         info.sample_method == JPEG_DOWN_SAMPLING_YUV444 ? "4:4:4" : info.sample_method == JPEG_DOWN_SAMPLING_YUV422 ? "4:2:2" :
                         info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? "4:2:0" : "gray", (unsigned)len);
                info_logged = true;
                if (info.width != FRAME_W || info.height != FRAME_H) ESP_LOGW(TAG, "frame size differs from configured %dx%d", FRAME_W, FRAME_H);
            }
        }

        int64_t t0 = esp_timer_get_time();
        uint32_t out_len = 0;
        esp_err_t err = jpeg_decoder_process(dec, &dcfg, in, len, out, out_cap, &out_len);
        int64_t t1 = esp_timer_get_time();
        if (err != ESP_OK) {
            if (++decode_fail % 30 == 1) ESP_LOGW(TAG, "jpeg decode failed: %s (x%" PRIu32 ")", esp_err_to_name(err), decode_fail);
            continue;
        }
        if (out_len < (uint32_t)FRAME_W * FRAME_H * 3 / 2) { ESP_LOGW(TAG, "short decode: %" PRIu32 " bytes", out_len); continue; }

        /* keep the newest good JPEG for the web preview */
        if (s_jpeg && xSemaphoreTake(s_frame_lock, 0) == pdTRUE) {
            memcpy(s_jpeg, in, len); s_jpeg_len = len;
            xSemaphoreGive(s_frame_lock);
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_pipe_ok) hl_pipeline_frame_yuv420p4(&s_pipe, out, FRAME_W, FRAME_H, FRAME_W * 3 / 2, now_ms());
        s_last_frame_ms = now_ms();
        xSemaphoreGive(s_lock);
        int64_t t2 = esp_timer_get_time();

        frames++;
        decode_us_acc += (t1 - t0);
        pipe_us_acc += (t2 - t1);
        int64_t now = now_ms();
        if (now - last_report >= 5000) {
            float fps = frames * 1000.0f / (now - last_report);
            float dms = frames ? decode_us_acc / 1000.0f / frames : 0.f, pms = frames ? pipe_us_acc / 1000.0f / frames : 0.f;
            int64_t cpu_now_us = esp_timer_get_time(); uint8_t cpu[2];
            for (int c = 0; c < 2; c++) {
                TaskStatus_t ts; vTaskGetInfo(idle[c], &ts, pdFALSE, eRunning);
                uint32_t idle_us = ts.ulRunTimeCounter - idle_prev[c]; idle_prev[c] = ts.ulRunTimeCounter;
                int64_t el = cpu_now_us - cpu_prev_us;
                cpu[c] = (uint8_t)(el > 0 && idle_us < el ? 100 - (100 * idle_us) / el : 0);
            }
            cpu_prev_us = cpu_now_us;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_stat.fps = fps; s_stat.decode_ms = dms; s_stat.pipeline_ms = pms; s_stat.cpu_pct[0] = cpu[0]; s_stat.cpu_pct[1] = cpu[1];
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "%.1f fps (usb %" PRIu32 " total), decode %.1f ms, pipeline %.1f ms, cpu %u%%/%u%%, hdr=%d, border h%d v%d, free psram %u",
                     fps, hl_capture_frames_total(), dms, pms, cpu[0], cpu[1], (int)s_pipe.hdr_active,
                     s_pipe.bb.current.unknown ? -1 : s_pipe.bb.current.horizontalSize,
                     s_pipe.bb.current.unknown ? -1 : s_pipe.bb.current.verticalSize,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            frames = 0; decode_us_acc = pipe_us_acc = 0; last_report = now;
        }
    }
}

/* ---- LED output (core 1) ------------------------------------------------ */
static void led_task(void *arg)
{
    static hl_f3 black[MAX_LEDS];
    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_HL_OUTPUT_HZ);
    TickType_t last = xTaskGetTickCount();
    bool dark = false;
    while (1) {
        int64_t now = now_ms();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_pipe_ok) {
            bool tv_off = hl_board_leds_blank();      /* oneboard build with CEC following on: the TV is in standby */
            if (tv_off || now - s_last_frame_ms > s_cfg.no_signal_ms) {
                if (!dark) {
                    if (tv_off) ESP_LOGW(TAG, "the TV is in standby, fading out");
                    else ESP_LOGW(TAG, "no signal for %u ms, fading out", s_cfg.no_signal_ms);
                    dark = true;
                }
                hl_smooth_set_targets(&s_pipe.smooth, black, now);
            } else if (dark) {
                ESP_LOGI(TAG, "signal back");
                dark = false;
            }
            s_no_signal = dark;
            hl_pipeline_output(&s_pipe, now, s_rgb);
        }
        if (s_cfg.out_mode == 1) memset(s_rgb, 0, (size_t)s_n_leds * 3);
        else if (s_cfg.out_mode == 2) for (int i = 0; i < s_n_leds; i++) memcpy(s_rgb + i * 3, s_cfg.out_rgb, 3);
        xSemaphoreGive(s_lock);
        hl_leds_write(s_rgb, s_n_leds);   /* the RMT transfer takes a few ms: never under the lock the decode task needs */
        vTaskDelayUntil(&last, period);
    }
}

/* With app rollback enabled a freshly updated image must declare itself healthy, or the bootloader boots the previous
 * image at the next reset.
 * With Wi-Fi, healthy = the network is up (joined the stored network, or the setup AP is serving): updates arrive over
 * Wi-Fi, so an image without it could only be replaced by cable. An updated image with no network after
 * HEALTHY_NET_WAIT_S restarts on purpose and the bootloader rolls back.
 * Without Wi-Fi, healthy = a frame decoded, or 60 s of uptime (a boot with no dongle attached is not a failure). */
#define HEALTHY_NET_WAIT_S 180

static bool pending_verify(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void mark_healthy(void)
{
    static bool done;
    if (done) return;
    done = true;
    if (pending_verify()) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "update confirmed on %s", esp_ota_get_running_partition()->label);
    }
}

static void healthy_timer_task(void *arg)
{
#if CONFIG_HL_WIFI
    if (pending_verify()) {
        for (int s = 0; s < HEALTHY_NET_WAIT_S; s++) {
            hl_net_info_t ni;
            hl_net_info(&ni);
            if (ni.connected || strcmp(ni.mode, "ap") == 0) {
                mark_healthy();
                vTaskDelete(NULL);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGE(TAG, "updated image has no network after %d s: restarting so the bootloader rolls back", HEALTHY_NET_WAIT_S);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
#else
    vTaskDelay(pdMS_TO_TICKS(60000));
    mark_healthy();
#endif
    vTaskDelete(NULL);
}

void app_main(void)
{
    hl_board_init();            /* oneboard build: every board pin to its safe state before anything else */
    hl_crash_init();            /* before anything logs: picks up the previous boot's crash, if any */
    hl_log_init(64 * 1024);
    ESP_LOGI(TAG, "hdmilight-v3 starting (%s)", CONFIG_IDF_TARGET);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init(); }
    ESP_ERROR_CHECK(err);
    hl_cfg_load(&s_cfg);
    s_lock = xSemaphoreCreateMutex();
    s_frame_lock = xSemaphoreCreateMutex();
    s_jpeg = heap_caps_malloc((size_t)FRAME_W * FRAME_H * 2, MALLOC_CAP_SPIRAM);
    if (!s_jpeg) ESP_LOGW(TAG, "no PSRAM for the frame preview buffer");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = build_pipeline(&s_cfg);
    build_strip(&s_cfg);
    xSemaphoreGive(s_lock);
    if (rc != 0) ESP_LOGE(TAG, "pipeline init failed; fix the layout in the web UI");

#if CONFIG_HL_WIFI
    hl_net_start(&s_cfg);   /* P4: through the ESP32-C6 (esp_wifi_remote + esp_hosted); S31: its own radio */
    hl_web_start();
    hl_homeassistant_start();   /* ESPHome API light, discovered by Home Assistant */
    hl_netwatch_start();        /* restart if the board is up but unreachable */
#endif

    s_last_frame_ms = now_ms();
    hl_capture_start();
    hl_board_start();           /* oneboard build: run the on-board splitter (no-op on dev kits) */
    hl_console_start();

    /* 4 KiB: on an update's first boot this task writes otadata and logs "update confirmed"; that ESP_LOGW goes
     * through logbuf (288 B frame) and newlib's _vfprintf_r (1056 B) into a possibly contended UART lock. With 2 KiB
     * it overflowed (hardware stack guard, core 1, caught by crashlog on 2026-11-07) whenever another task was
     * logging at that moment, e.g. Home Assistant reconnecting 9 s after boot. */
    xTaskCreate(healthy_timer_task, "healthy", 4096, NULL, 1, NULL);
    xTaskCreatePinnedToCore(decode_task, "decode", 8192, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(led_task, "leds", 4096, NULL, 12, NULL, 1);
}
