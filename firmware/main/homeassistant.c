/* Glue between the ESPHome API server (esphome_api.c) and the app.
 *
 * "Off" is output mode 1 (the UI's Off). "On" restores the last other mode: live picture, or the solid colour if
 * that was on. Brightness is the global brightness cap (brightness_pct). Changes made in the web UI reach HA
 * through the server's poll. */
#include "homeassistant.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esphome_api.h"
#include "app.h"

static const char *TAG = "homeassistant";
#define API_PORT 6053
#define POLL_MS  250

static int  s_last_on_mode;          /* 0 live, 2 solid */
static char s_name[32], s_mac[18], s_mac_txt[13], s_compiled[32];
static hl_esph_ops_t s_ops;
static TaskHandle_t  s_task;

int hl_homeassistant_stack_free(void) { return s_task ? (int)uxTaskGetStackHighWaterMark(s_task) : -1; }

static void get_state(void *ctx, bool *on, float *bri)
{
    const hl_cfg_t *c = hl_app_cfg();
    if (c->out_mode != 1) s_last_on_mode = c->out_mode;
    *on = c->out_mode != 1;
    *bri = c->brightness_pct / 100.0f;
}

static void set_state(void *ctx, bool has_state, bool on, bool has_bri, float bri)
{
    if (has_bri) {
        int pct = (int)lroundf(bri * 100);
        pct = pct < 1 ? 1 : pct > 100 ? 100 : pct;
        hl_cfg_t c = *hl_app_cfg();
        if (c.brightness_pct != pct) { c.brightness_pct = pct; hl_app_apply_config(&c); }
    }
    if (has_state) {
        int mode = hl_app_cfg()->out_mode;
        if (!on && mode != 1) hl_app_set_output(1, NULL);
        else if (on && mode == 1) hl_app_set_output(s_last_on_mode, NULL);
    }
}

static void log_cb(const char *msg) { ESP_LOGI(TAG, "%s", msg); }

static void api_task(void *arg)
{
    /* hl_esph_serve only returns if it cannot open its listening socket (e.g. no free sockets at that moment):
     * keep trying rather than leaving HA without the device until the next reboot */
    for (;;) {
        hl_esph_serve(&s_ops, API_PORT, POLL_MS);
        ESP_LOGE(TAG, "API server could not start, retrying in 5 s");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void hl_homeassistant_set_name(const char *name)
{
    if (!name[0] || !strcmp(name, s_name)) return;
    strlcpy(s_name, name, sizeof s_name);       /* s_ops points here: the next API client sees the new name */
    mdns_service_instance_name_set("_esphomelib", "_tcp", s_name);
    mdns_service_txt_item_set("_esphomelib", "_tcp", "friendly_name", s_name);
}

esp_err_t hl_homeassistant_start(void)
{
    const hl_cfg_t *c = hl_app_cfg();
    const esp_app_desc_t *ad = esp_app_get_description();
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_BASE);
    snprintf(s_mac, sizeof s_mac, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    snprintf(s_mac_txt, sizeof s_mac_txt, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    snprintf(s_compiled, sizeof s_compiled, "%s %s", ad->date, ad->time);
    strlcpy(s_name, c->hostname[0] ? c->hostname : "hdmilight", sizeof s_name);
    const char *const_target = "hdmilight-v3 (" CONFIG_IDF_TARGET ")";
    s_last_on_mode = c->out_mode != 1 ? c->out_mode : 0;

    s_ops = (hl_esph_ops_t){
        .name = s_name, .friendly_name = s_name, .mac = s_mac,
        .version = ad->version, .compile_time = s_compiled,
        .manufacturer = "hdmilight", .model = const_target,
        .webserver_port = 80,
        .get = get_state, .set = set_state, .log = log_cb,
    };

    /* what HA's ESPHome integration looks for: _esphomelib._tcp with a "mac" TXT record */
    mdns_txt_item_t txt[] = {
        { "friendly_name", s_name },
        { "version", ad->version },
        { "mac", s_mac_txt },
        { "platform", "ESP32" },
        { "board", CONFIG_IDF_TARGET },
        { "network", "wifi" },
    };
    esp_err_t err = mdns_service_add(s_name, "_esphomelib", "_tcp", API_PORT, txt, sizeof txt / sizeof txt[0]);
    if (err != ESP_OK) ESP_LOGW(TAG, "mDNS announce failed (%s); HA can still add it by IP, port %d", esp_err_to_name(err), API_PORT);

    if (xTaskCreatePinnedToCore(api_task, "esphome_api", 8192, NULL, 3, &s_task, 1) != pdPASS) return ESP_FAIL;
    ESP_LOGI(TAG, "ESPHome API on port %d as %s (%s)", API_PORT, s_name, s_mac);
    return ESP_OK;
}
