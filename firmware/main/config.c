#include "config.h"
#include "board.h"
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"       /* git-ignored local file, see wifi_secrets.h.example */
#endif
#ifndef HL_WIFI_SSID
#define HL_WIFI_SSID ""
#define HL_WIFI_PASS ""
#endif
#ifndef HL_HOSTNAME
#define HL_HOSTNAME "hdmilight"
#endif
#include "sdkconfig.h"
#include <string.h>
#include <stddef.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "config";
#define NS  "hdmilight"
#define KEY "cfg"

void hl_cfg_defaults(hl_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = HL_CFG_VERSION;
    c->top = CONFIG_HL_LED_TOP; c->right = CONFIG_HL_LED_RIGHT; c->bottom = CONFIG_HL_LED_BOTTOM; c->left = CONFIG_HL_LED_LEFT;
    c->depth_pct = CONFIG_HL_ZONE_DEPTH_PCT;
#if CONFIG_HL_LED_SK6812_RGBW
    c->led_type = HL_LED_TYPE_SK6812_RGBW;
#elif CONFIG_HL_LED_WS2811
    c->led_type = HL_LED_TYPE_WS2811;
#else
    c->led_type = HL_LED_TYPE_WS2812;
#endif
    c->gpio = HL_BOARD_LED_GPIO >= 0 ? HL_BOARD_LED_GPIO : CONFIG_HL_LED_GPIO;
    c->brightness_pct = CONFIG_HL_BRIGHTNESS_PCT;
#if CONFIG_HL_HDR_ON
    c->hdr_mode = 1;
#elif CONFIG_HL_HDR_OFF
    c->hdr_mode = 2;
#else
    c->hdr_mode = 0;
#endif
    c->blackborder = CONFIG_HL_BLACKBORDER;
    c->block = CONFIG_HL_GRID_BLOCK;
    c->smooth_stiffness = 150;
    c->smooth_damping = 26;
    c->no_signal_ms = CONFIG_HL_NO_SIGNAL_MS;
    static const uint8_t ideal[8][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {0,255,255}, {255,0,255}, {255,255,0}, {255,255,255}, {0,0,0} };
    memcpy(c->cal, ideal, sizeof ideal);
    c->temp_rgb[0] = c->temp_rgb[1] = c->temp_rgb[2] = 100;
    c->classic = 1;
    c->gamma_x100 = 150; c->luminance_x100 = 100; c->saturation_x100 = 100; c->scale_x100 = 100;
    c->backlight_x10000 = 39; c->backlight_colored = 1;
    c->power_limit_pct = 100;
    c->out_mode = 0; c->out_rgb[0] = c->out_rgb[1] = c->out_rgb[2] = 255;
    c->color_order = HL_ORDER_GRB;
    strlcpy(c->hostname, HL_HOSTNAME, sizeof c->hostname);
    strlcpy(c->wifi_ssid, HL_WIFI_SSID, sizeof c->wifi_ssid);
    strlcpy(c->wifi_pass, HL_WIFI_PASS, sizeof c->wifi_pass);
}

esp_err_t hl_cfg_load(hl_cfg_t *c)
{
    hl_cfg_defaults(c);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) { ESP_LOGI(TAG, "no stored config, using defaults"); return ESP_OK; }
    hl_cfg_t stored = { 0 };
    size_t len = sizeof stored;
    err = nvs_get_blob(h, KEY, &stored, &len);
    nvs_close(h);
    /* each version appended fields: copy the stored prefix, keep the defaults for the rest */
    const size_t prefix[] = { 0, offsetof(hl_cfg_t, temp_preset), offsetof(hl_cfg_t, out_mode), offsetof(hl_cfg_t, color_order), sizeof(hl_cfg_t) };
    if (err == ESP_OK && stored.version >= 1 && stored.version <= HL_CFG_VERSION && len <= sizeof stored) {
        memcpy(c, &stored, len < prefix[stored.version] ? len : prefix[stored.version]);
        c->version = HL_CFG_VERSION;
        ESP_LOGI(TAG, "loaded config from NVS (v%u)", stored.version);
        if (!c->wifi_ssid[0] && HL_WIFI_SSID[0]) {   /* nothing entered in the UI yet: use the local secrets file */
            strlcpy(c->wifi_ssid, HL_WIFI_SSID, sizeof c->wifi_ssid);
            strlcpy(c->wifi_pass, HL_WIFI_PASS, sizeof c->wifi_pass);
        }
    } else {
        ESP_LOGW(TAG, "stored config unusable (%s, len %u, v%u), using defaults", esp_err_to_name(err), (unsigned)len, stored.version);
    }
    if (HL_BOARD_LED_GPIO >= 0) c->gpio = HL_BOARD_LED_GPIO;      /* whatever was stored: the strip is wired to one pin */
    return ESP_OK;
}

esp_err_t hl_cfg_save(const hl_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, KEY, c, sizeof *c);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved config: %s", esp_err_to_name(err));
    return err;
}

int hl_cfg_led_count(const hl_cfg_t *c)
{
    return c->top + c->right + c->bottom + c->left;
}
