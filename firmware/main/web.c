#include "web.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "logbuf.h"
#include "cJSON.h"
#include "app.h"
#include "net.h"
#include "netwatch.h"
#include "crashlog.h"
#include "board.h"
#include "homeassistant.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web";
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

static cJSON *read_json_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 4096) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

static esp_err_t h_status(httpd_req_t *req)
{
    hl_status_t s; hl_app_get_status(&s);
    hl_net_info_t n; hl_net_info(&n);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "uptime_s", s.uptime_s);
    cJSON_AddBoolToObject(j, "usb_connected", s.usb_connected);
    cJSON_AddNumberToObject(j, "frames_total", s.frames_total);
    cJSON_AddNumberToObject(j, "capture_w", s.cap_w);
    cJSON_AddNumberToObject(j, "capture_h", s.cap_h);
    cJSON_AddNumberToObject(j, "capture_fps", s.cap_fps);
    cJSON_AddNumberToObject(j, "fps", s.fps);
    cJSON_AddNumberToObject(j, "decode_ms", s.decode_ms);
    cJSON_AddNumberToObject(j, "pipeline_ms", s.pipeline_ms);
    cJSON_AddNumberToObject(j, "cpu0", s.cpu_pct[0]);
    cJSON_AddNumberToObject(j, "cpu1", s.cpu_pct[1]);
    cJSON_AddNumberToObject(j, "out_mode", s.out_mode);
    { char hex[8]; snprintf(hex, sizeof hex, "#%02x%02x%02x", s.out_rgb[0], s.out_rgb[1], s.out_rgb[2]); cJSON_AddStringToObject(j, "out_rgb", hex); }
    cJSON_AddBoolToObject(j, "hdr_active", s.hdr_active);
    cJSON_AddNumberToObject(j, "border_h", s.border_h);
    cJSON_AddNumberToObject(j, "border_v", s.border_v);
    cJSON_AddBoolToObject(j, "no_signal", s.no_signal);
    cJSON_AddNumberToObject(j, "free_psram", s.free_psram);
    cJSON_AddNumberToObject(j, "free_heap", s.free_heap);
    cJSON_AddNumberToObject(j, "n_leds", s.n_leds);
    cJSON_AddNumberToObject(j, "grid_w", s.grid_w);
    cJSON_AddNumberToObject(j, "grid_h", s.grid_h);
    const esp_app_desc_t *ad = esp_app_get_description();
    char build[96]; snprintf(build, sizeof build, "%s %02x%02x%02x%02x %s %s", ad->version,
                             ad->app_elf_sha256[0], ad->app_elf_sha256[1], ad->app_elf_sha256[2], ad->app_elf_sha256[3], ad->date, ad->time);   /* sha prefix: unique per image */
    cJSON_AddStringToObject(j, "build", build);
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(j, "slot", run ? run->label : "?");
    { char lr[96]; hl_netwatch_last_restart(lr, sizeof lr); cJSON_AddStringToObject(j, "last_restart", lr); }
    cJSON_AddBoolToObject(j, "crashed", hl_crash_last() != NULL);   /* details at /api/crash */
    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddStringToObject(w, "mode", n.mode);
    cJSON_AddStringToObject(w, "ip", n.ip);
    cJSON_AddStringToObject(w, "ssid", n.ssid);
    cJSON_AddNumberToObject(w, "rssi", n.rssi);
    cJSON_AddBoolToObject(w, "connected", n.connected);
    return send_json(req, j);
}

static const char *cal_names[8] = { "cal_red", "cal_green", "cal_blue", "cal_cyan", "cal_magenta", "cal_yellow", "cal_white", "cal_black" };

static cJSON *cfg_to_json(const hl_cfg_t *c)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "top", c->top);
    cJSON_AddNumberToObject(j, "right", c->right);
    cJSON_AddNumberToObject(j, "bottom", c->bottom);
    cJSON_AddNumberToObject(j, "left", c->left);
    cJSON_AddNumberToObject(j, "depth_pct", c->depth_pct);
    cJSON_AddNumberToObject(j, "led_type", c->led_type);
    cJSON_AddNumberToObject(j, "gpio", c->gpio);
    cJSON_AddBoolToObject(j, "gpio_fixed", HL_BOARD_LED_GPIO >= 0);     /* the board wires the strip's pin: the page greys the field */
    cJSON_AddNumberToObject(j, "color_order", c->color_order);
    cJSON_AddNumberToObject(j, "brightness_pct", c->brightness_pct);
    cJSON_AddNumberToObject(j, "hdr_mode", c->hdr_mode);
    cJSON_AddBoolToObject(j, "blackborder", c->blackborder);
    cJSON_AddNumberToObject(j, "block", c->block);
    cJSON_AddNumberToObject(j, "smooth_stiffness", c->smooth_stiffness);
    cJSON_AddNumberToObject(j, "smooth_damping", c->smooth_damping);
    cJSON_AddNumberToObject(j, "no_signal_ms", c->no_signal_ms);
    cJSON_AddNumberToObject(j, "temp_preset", c->temp_preset);
    cJSON *tr = cJSON_AddArrayToObject(j, "temp_rgb");
    for (int i = 0; i < 3; i++) cJSON_AddItemToArray(tr, cJSON_CreateNumber(c->temp_rgb[i]));
    cJSON_AddBoolToObject(j, "classic", c->classic);
    for (int i = 0; i < 8; i++) {
        char hex[8]; snprintf(hex, sizeof hex, "#%02x%02x%02x", c->cal[i][0], c->cal[i][1], c->cal[i][2]);
        cJSON_AddStringToObject(j, cal_names[i], hex);
    }
    cJSON_AddNumberToObject(j, "gamma", c->gamma_x100 / 100.0);
    cJSON_AddNumberToObject(j, "luminance", c->luminance_x100 / 100.0);
    cJSON_AddNumberToObject(j, "saturation", c->saturation_x100 / 100.0);
    cJSON_AddNumberToObject(j, "scale", c->scale_x100 / 100.0);
    cJSON_AddNumberToObject(j, "backlight_pct", c->backlight_x10000 / 100.0);
    cJSON_AddBoolToObject(j, "backlight_colored", c->backlight_colored);
    cJSON_AddNumberToObject(j, "power_limit_pct", c->power_limit_pct);
    cJSON_AddStringToObject(j, "wifi_ssid", c->wifi_ssid);
    cJSON_AddStringToObject(j, "hostname", c->hostname);
    return j;
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    return send_json(req, cfg_to_json(hl_app_cfg()));
}

/* A new hostname is used at once: mDNS and the ESPHome announcement here, DHCP from the next join. */
static void hostname_changed(const char *host)
{
    hl_net_set_hostname(host);
    hl_homeassistant_set_name(host);
}

#define GET_INT(field, lo, hi) do { cJSON *v = cJSON_GetObjectItem(j, #field); if (cJSON_IsNumber(v)) { int x = v->valueint; if (x < (lo)) x = (lo); if (x > (hi)) x = (hi); c.field = x; } } while (0)
#define GET_BOOL(field) do { cJSON *v = cJSON_GetObjectItem(j, #field); if (cJSON_IsBool(v)) c.field = cJSON_IsTrue(v); } while (0)
/* a decimal in JSON stored as an integer times `mul` */
#define GET_SCALED(name, field, mul, lo, hi) do { cJSON *v = cJSON_GetObjectItem(j, name); if (cJSON_IsNumber(v)) { long x = lround(v->valuedouble * (mul)); if (x < (lo)) x = (lo); if (x > (hi)) x = (hi); c.field = x; } } while (0)

static esp_err_t h_config_post(httpd_req_t *req)
{
    cJSON *j = read_json_body(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    hl_cfg_t c = *hl_app_cfg();
    GET_INT(top, 0, 300); GET_INT(right, 0, 300); GET_INT(bottom, 0, 300); GET_INT(left, 0, 300);
    GET_INT(depth_pct, 2, 50);
    GET_INT(led_type, 0, 2);
    if (HL_BOARD_LED_GPIO < 0) { GET_INT(gpio, 0, 63); }
    GET_INT(color_order, 0, 5);
    GET_INT(brightness_pct, 1, 100);
    GET_INT(hdr_mode, 0, 2);
    GET_BOOL(blackborder);
    GET_INT(block, 2, 40);
    GET_INT(smooth_stiffness, 1, 2000);
    GET_INT(smooth_damping, 1, 500);
    GET_INT(no_signal_ms, 200, 60000);
    GET_INT(temp_preset, 0, 4);
    cJSON *tr = cJSON_GetObjectItem(j, "temp_rgb");
    if (cJSON_IsArray(tr) && cJSON_GetArraySize(tr) == 3)
        for (int i = 0; i < 3; i++) { int x = cJSON_GetArrayItem(tr, i)->valueint; c.temp_rgb[i] = x < 0 ? 0 : x > 200 ? 200 : x; }
    GET_BOOL(classic);
    for (int i = 0; i < 8; i++) {
        cJSON *v = cJSON_GetObjectItem(j, cal_names[i]); unsigned r, g, b;
        if (cJSON_IsString(v) && sscanf(v->valuestring, "#%02x%02x%02x", &r, &g, &b) == 3) { c.cal[i][0] = r; c.cal[i][1] = g; c.cal[i][2] = b; }
    }
    GET_SCALED("gamma", gamma_x100, 100, 10, 500);
    GET_SCALED("luminance", luminance_x100, 100, 0, 300);
    GET_SCALED("saturation", saturation_x100, 100, 0, 300);
    GET_SCALED("scale", scale_x100, 100, 0, 200);
    GET_SCALED("backlight_pct", backlight_x10000, 100, 0, 5000);
    GET_BOOL(backlight_colored);
    GET_INT(power_limit_pct, 1, 100);
    cJSON *hn = cJSON_GetObjectItem(j, "hostname");
    bool renamed = false;
    if (cJSON_IsString(hn)) {
        if (!hl_net_hostname_ok(hn->valuestring)) { cJSON_Delete(j); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "hostname: letters, digits and hyphens only"); }
        renamed = strcmp(c.hostname, hn->valuestring) != 0;
        strlcpy(c.hostname, hn->valuestring, sizeof c.hostname);
    }
    cJSON_Delete(j);
    if (hl_cfg_led_count(&c) < 1 || hl_cfg_led_count(&c) > 512) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "led count 1..512");
    esp_err_t err = hl_app_apply_config(&c);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    if (renamed) hostname_changed(c.hostname);
    return send_json(req, cfg_to_json(hl_app_cfg()));
}

static esp_err_t h_output_post(httpd_req_t *req)
{
    cJSON *j = read_json_body(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    const hl_cfg_t *cur = hl_app_cfg();
    int mode = cur->out_mode; uint8_t rgb[3]; memcpy(rgb, cur->out_rgb, 3);
    cJSON *m = cJSON_GetObjectItem(j, "mode"); if (cJSON_IsNumber(m)) mode = m->valueint < 0 ? 0 : m->valueint > 2 ? 2 : m->valueint;
    cJSON *v = cJSON_GetObjectItem(j, "rgb"); unsigned r, g, b;
    if (cJSON_IsString(v) && sscanf(v->valuestring, "#%02x%02x%02x", &r, &g, &b) == 3) { rgb[0] = r; rgb[1] = g; rgb[2] = b; }
    cJSON_Delete(j);
    if (hl_app_set_output(mode, rgb) != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    cJSON *o = cJSON_CreateObject(); char hex[8]; snprintf(hex, sizeof hex, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
    cJSON_AddNumberToObject(o, "mode", mode); cJSON_AddStringToObject(o, "rgb", hex);
    return send_json(req, o);
}

static esp_err_t h_leds(httpd_req_t *req)
{
    static uint8_t rgb[512 * 3];
    int n = hl_app_get_leds(rgb, 512);
    /* compact: hex string, 6 chars per LED */
    char *s = malloc(n * 6 + 32);
    if (!s) return httpd_resp_send_500(req);
    int p = sprintf(s, "{\"n\":%d,\"hex\":\"", n);
    for (int i = 0; i < n; i++) p += sprintf(s + p, "%02x%02x%02x", rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    sprintf(s + p, "\"}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

static esp_err_t h_wifi_post(httpd_req_t *req)
{
    cJSON *j = read_json_body(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *ssid = cJSON_GetObjectItem(j, "ssid"), *pass = cJSON_GetObjectItem(j, "pass"), *hn = cJSON_GetObjectItem(j, "hostname");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) { cJSON_Delete(j); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid"); }
    if (cJSON_IsString(hn) && !hl_net_hostname_ok(hn->valuestring)) { cJSON_Delete(j); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "hostname: letters, digits and hyphens only"); }
    hl_cfg_t c = *hl_app_cfg();
    /* no password sent: keep the stored one for the network already configured, none for a different one */
    if (cJSON_IsString(pass)) strlcpy(c.wifi_pass, pass->valuestring, sizeof c.wifi_pass);
    else if (strcmp(c.wifi_ssid, ssid->valuestring)) c.wifi_pass[0] = 0;
    strlcpy(c.wifi_ssid, ssid->valuestring, sizeof c.wifi_ssid);
    bool renamed = cJSON_IsString(hn) && strcmp(c.hostname, hn->valuestring);
    if (cJSON_IsString(hn)) strlcpy(c.hostname, hn->valuestring, sizeof c.hostname);
    cJSON_Delete(j);
    hl_app_apply_config(&c);   /* saves */
    if (renamed) hostname_changed(c.hostname);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "ssid", c.wifi_ssid);
    cJSON_AddStringToObject(r, "hostname", c.hostname);
    esp_err_t sent = send_json(req, r);
    vTaskDelay(pdMS_TO_TICKS(300));     /* let the answer leave before the link drops */
    hl_net_join(c.wifi_ssid, c.wifi_pass);
    return sent;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    static char buf[2048];
    if (hl_net_scan_json(buf, sizeof buf) != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_frame(httpd_req_t *req)
{
    static uint8_t *buf;
    const size_t cap = (size_t)CONFIG_HL_CAPTURE_WIDTH * CONFIG_HL_CAPTURE_HEIGHT * 2;
    if (!buf) buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_500(req);
    size_t n = hl_app_copy_frame(buf, cap);
    if (!n) { httpd_resp_set_status(req, "204 No Content"); return httpd_resp_send(req, NULL, 0); }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)buf, n);
}

/* The crash that caused this boot (crashlog.c), for tools/crash_decode.py. {"present":false} if none. */
static esp_err_t h_crash(httpd_req_t *req)
{
    const hl_crash_report_t *r = hl_crash_last();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "present", r != NULL);
    if (r) {
        char hx[16];
#define HEX(name, v) do { snprintf(hx, sizeof hx, "0x%08lx", (unsigned long)(v)); cJSON_AddStringToObject(j, name, hx); } while (0)
        HEX("elf_sha", r->elf_sha);
        cJSON_AddNumberToObject(j, "core", r->core);
        cJSON_AddNumberToObject(j, "exception", r->exception);
        HEX("reason_ptr", r->reason_ptr); HEX("desc_ptr", r->desc_ptr);
        cJSON_AddStringToObject(j, "abort", r->abort_msg);
        HEX("addr", r->addr); HEX("mepc", r->mepc); HEX("ra", r->ra); HEX("sp", r->sp); HEX("gp", r->gp); HEX("tp", r->tp);
        HEX("mstatus", r->mstatus); HEX("mcause", r->mcause); HEX("mtval", r->mtval);
        cJSON *regs = cJSON_AddObjectToObject(j, "regs");
        char nm[8];
        for (int i = 0; i < 8; i++)  { snprintf(nm, sizeof nm, "a%d", i); snprintf(hx, sizeof hx, "0x%08lx", (unsigned long)r->a[i]); cJSON_AddStringToObject(regs, nm, hx); }
        for (int i = 0; i < 12; i++) { snprintf(nm, sizeof nm, "s%d", i); snprintf(hx, sizeof hx, "0x%08lx", (unsigned long)r->s[i]); cJSON_AddStringToObject(regs, nm, hx); }
        for (int i = 0; i < 7; i++)  { snprintf(nm, sizeof nm, "t%d", i); snprintf(hx, sizeof hx, "0x%08lx", (unsigned long)r->t[i]); cJSON_AddStringToObject(regs, nm, hx); }
        cJSON *st = cJSON_AddArrayToObject(j, "stack");
        for (uint32_t i = 0; i < r->stack_words; i++) { snprintf(hx, sizeof hx, "0x%08lx", (unsigned long)r->stack[i]); cJSON_AddItemToArray(st, cJSON_CreateString(hx)); }
#undef HEX
        char *log = malloc(r->log_len + 1);
        if (log) { memcpy(log, r->log, r->log_len); log[r->log_len] = 0; cJSON_AddStringToObject(j, "log", log); free(log); }
    }
    return send_json(req, j);
}

static esp_err_t h_favicon(httpd_req_t *req) { httpd_resp_set_status(req, "204 No Content"); return httpd_resp_send(req, NULL, 0); }

static esp_err_t h_selftest(httpd_req_t *req)
{
    hl_app_selftest();
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* Captive portal: every unknown URL (including the phones' connectivity probes such as /generate_204 and
 * /hotspot-detect.html) is redirected to the UI, which makes the phone pop its "sign in to network" page.
 * iOS requires a body, a bare redirect is not enough. */
static esp_err_t h_404_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "<a href=\"/\">hdmilight</a>", HTTPD_RESP_USE_STRLEN);
}

/* Firmware update over Wi-Fi: POST the raw app image. Written to the other OTA slot, then reboot; if the new
 * image never reports healthy the bootloader rolls back (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). */
static void reboot_task(void *arg);
static esp_err_t h_update(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no image / no slot");
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(part, req->content_len, &ota);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    static char *buf; if (!buf) buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!buf) { esp_ota_abort(ota); return httpd_resp_send_500(req); }
    int left = req->content_len, got_total = 0;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < 16384 ? left : 16384);
        if (r <= 0) { esp_ota_abort(ota); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "receive failed"); }
        if ((err = esp_ota_write(ota, buf, r)) != ESP_OK) { esp_ota_abort(ota); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err)); }
        left -= r; got_total += r;
    }
    if ((err = esp_ota_end(ota)) != ESP_OK || (err = esp_ota_set_boot_partition(part)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    ESP_LOGW(TAG, "update: %d bytes written to %s, rebooting", got_total, part->label);
    char msg[96]; snprintf(msg, sizeof msg, "{\"ok\":true,\"bytes\":%d,\"slot\":\"%s\",\"note\":\"rebooting\"}", got_total, part->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, msg);
    xTaskCreate(reboot_task, "reboot", 4096, NULL, 5, NULL);   /* esp_restart runs every shutdown handler on this stack */
    return ESP_OK;
}

/* Log tail: GET /api/log?since=<pos> returns text after that absolute position plus X-Log-Next for the next call. */
static esp_err_t h_log(httpd_req_t *req)
{
    char q[48] = {0}; uint64_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) { char v[24]; if (httpd_query_key_value(q, "since", v, sizeof v) == ESP_OK) since = strtoull(v, NULL, 10); }
    static char *buf; const size_t cap = 32768;
    if (!buf) buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_500(req);
    uint64_t next = 0; size_t n = hl_log_read(since, buf, cap, &next);
    char hdr[24]; snprintf(hdr, sizeof hdr, "%llu", (unsigned long long)next);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Log-Next", hdr);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-Log-Next");
    return httpd_resp_send(req, buf, n);
}

static void reboot_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); }
static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(reboot_task, "reboot", 4096, NULL, 5, NULL);   /* esp_restart runs every shutdown handler on this stack */
    return ESP_OK;
}

esp_err_t hl_web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 32;
    cfg.stack_size = 8192;
    cfg.core_id = 1;              /* network serving on core 1; capture/decode/pipeline own core 0 */
    cfg.lru_purge_enable = true;
    httpd_handle_t h = NULL;
    esp_err_t err = httpd_start(&h, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err)); return err; }
    const httpd_uri_t uris[] = {
        { "/",              HTTP_GET,  h_index,       NULL },
        { "/api/status",    HTTP_GET,  h_status,      NULL },
        { "/api/config",    HTTP_GET,  h_config_get,  NULL },
        { "/api/config",    HTTP_POST, h_config_post, NULL },
        { "/api/leds",      HTTP_GET,  h_leds,        NULL },
        { "/api/frame.jpg", HTTP_GET,  h_frame,       NULL },
        { "/api/wifi",      HTTP_POST, h_wifi_post,   NULL },
        { "/api/scan",      HTTP_GET,  h_scan,        NULL },
        { "/api/selftest",  HTTP_POST, h_selftest,    NULL },
        { "/api/output",    HTTP_POST, h_output_post, NULL },
        { "/api/reboot",    HTTP_POST, h_reboot,      NULL },
        { "/api/update",    HTTP_POST, h_update,      NULL },
        { "/api/log",       HTTP_GET,  h_log,         NULL },
        { "/favicon.ico",   HTTP_GET,  h_favicon,     NULL },
        { "/api/crash",     HTTP_GET,  h_crash,       NULL },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(h, &uris[i]);
#if CONFIG_HL_BOARD_ONEBOARD
    hl_board_register_http(h);          /* the oneboard design's endpoints, including the capture chip's flash */
#endif
    httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, h_404_redirect);
    ESP_LOGI(TAG, "http server up");
    return ESP_OK;
}
