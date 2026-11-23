/* Runtime configuration, persisted in NVS. Defaults come from Kconfig. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define HL_CFG_VERSION 4

typedef enum { HL_LED_TYPE_WS2812 = 0, HL_LED_TYPE_SK6812_RGBW = 1, HL_LED_TYPE_WS2811 = 2 } hl_led_type_t;
typedef enum { HL_ORDER_GRB = 0, HL_ORDER_RGB, HL_ORDER_BRG, HL_ORDER_RBG, HL_ORDER_GBR, HL_ORDER_BGR } hl_color_order_t;

typedef struct {
    uint16_t version;
    /* layout */
    uint16_t top, right, bottom, left;
    uint8_t  depth_pct;
    /* strip */
    uint8_t  led_type;          /* hl_led_type_t */
    uint8_t  gpio;
    uint8_t  brightness_pct;
    /* pipeline */
    uint8_t  hdr_mode;          /* 0 auto, 1 on, 2 off */
    uint8_t  blackborder;
    uint8_t  block;
    uint8_t  lut_n;             /* unused: the LUT is baked at build time; kept so the NVS layout (version 1) is unchanged */
    uint16_t smooth_stiffness;  /* HyperHDR default 150 */
    uint16_t smooth_damping;    /* HyperHDR default 26 */
    uint16_t no_signal_ms;
    /* network */
    char     wifi_ssid[33];
    char     wifi_pass[65];
    char     hostname[32];
    /* colour (added in v2; a v1 blob is migrated by copying the prefix) */
    uint8_t  temp_preset;       /* hl_temp_preset */
    uint8_t  temp_rgb[3];       /* custom tint, percent */
    uint8_t  classic;           /* 1 = red/green/blue targets only */
    uint8_t  cal[8][3];         /* red, green, blue, cyan, magenta, yellow, white, black */
    uint16_t gamma_x100, luminance_x100, saturation_x100, scale_x100;
    uint16_t backlight_x10000;
    uint8_t  backlight_colored;
    uint8_t  power_limit_pct;
    /* output override (added in v3) */
    uint8_t  out_mode;          /* 0 live picture, 1 off, 2 solid colour */
    uint8_t  out_rgb[3];
    /* v4 */
    uint8_t  color_order;       /* hl_color_order_t: byte order the strip expects */
} hl_cfg_t;

void      hl_cfg_defaults(hl_cfg_t *c);
esp_err_t hl_cfg_load(hl_cfg_t *c);       /* defaults if nothing stored */
esp_err_t hl_cfg_save(const hl_cfg_t *c);
int       hl_cfg_led_count(const hl_cfg_t *c);
