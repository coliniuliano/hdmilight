/* Application core: pipeline + capture + LEDs, and the API the web UI uses. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "config.h"

typedef struct {
    uint32_t uptime_s;
    bool     usb_connected;
    uint32_t frames_total;      /* USB frames received */
    int      cap_w, cap_h;      /* negotiated USB video mode, 0 while no stream is open */
    float    cap_fps;
    float    fps;               /* decoded frames per second, last window */
    float    decode_ms, pipeline_ms;
    uint8_t  cpu_pct[2];        /* busy % per core over the last stats window */
    bool     hdr_active;
    int      border_h, border_v;   /* -1 = unknown */
    bool     no_signal;
    uint32_t free_psram, free_heap;
    int      n_leds;
    int      grid_w, grid_h;
    uint8_t  out_mode;
    uint8_t  out_rgb[3];
} hl_status_t;

void      hl_app_get_status(hl_status_t *s);
/* Copy of what the strip was last told to show (3 bytes per LED). Returns count. */
int       hl_app_get_leds(uint8_t *rgb, int max_leds);
const hl_cfg_t *hl_app_cfg(void);
/* Validate, save to NVS, and apply. Layout/strip changes rebuild the pipeline and the driver. */
esp_err_t hl_app_apply_config(const hl_cfg_t *c);
/* Copy of the newest JPEG frame from the dongle (already MJPEG, no re-encode). Returns bytes copied, 0 if none. */
size_t    hl_app_copy_frame(uint8_t *dst, size_t cap);
void      hl_app_selftest(void);
void      hl_console_start(void);          /* console.c: serial commands for bring-up (CONFIG_HL_CONSOLE) */
/* Output override: 0 live picture, 1 off, 2 solid colour. Persisted. */
esp_err_t hl_app_set_output(int mode, const uint8_t rgb[3]);
