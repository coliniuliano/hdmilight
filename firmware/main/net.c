#include "net.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "dns_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mdns.h"

static const char *TAG = "net";
#define STA_RETRY_MAX     6          /* then fall back to the setup AP */
#define STA_RETRY_DELAY   3000

static esp_netif_t *s_sta, *s_ap;
static hl_net_info_t s_info;
static char s_ssid[33], s_pass[65], s_host[32];
static int  s_retries;
static bool s_ap_up;
static dns_server_handle_t s_dns;   /* captive portal: answers every name with 192.168.4.1 while the AP is up */
static SemaphoreHandle_t s_lock;

static void start_ap(void)
{
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) != ESP_OK) esp_read_mac(mac, ESP_MAC_BASE);   /* the P4 has no Wi-Fi MAC of its own; the C6's is only known after start */
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "hdmilight-%02x%02x", mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password, CONFIG_HL_AP_PASSWORD, sizeof ap.ap.password);
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(CONFIG_HL_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    s_ap_up = true;
    if (!s_dns) {
        dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_dns = start_dns_server(&dc);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_info.mode, "ap", sizeof s_info.mode);
    strlcpy(s_info.ssid, (char *)ap.ap.ssid, sizeof s_info.ssid);
    strlcpy(s_info.ip, "192.168.4.1", sizeof s_info.ip);
    s_info.connected = false;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "setup AP up: SSID %s, password %s, http://192.168.4.1", ap.ap.ssid,
             ap.ap.authmode == WIFI_AUTH_OPEN ? "(open)" : CONFIG_HL_AP_PASSWORD);
}

static void try_sta(void)
{
    if (!s_ssid[0]) { start_ap(); return; }
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, s_ssid, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, s_pass, sizeof sta.sta.password);
    sta.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_ap_up ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_LOGI(TAG, "connecting to %s", s_ssid);
    esp_wifi_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        try_sta();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.connected = false;
        xSemaphoreGive(s_lock);
        if (++s_retries <= STA_RETRY_MAX || s_ap_up) {
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d", d->reason, s_retries);
            vTaskDelay(pdMS_TO_TICKS(STA_RETRY_DELAY));
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "could not join %s, starting setup AP", s_ssid);
            start_ap();
            esp_wifi_connect();   /* keep trying the station side in the background */
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "AP: client " MACSTR " joined (aid %d)", MAC2STR(e->mac), e->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGW(TAG, "AP: client " MACSTR " left (aid %d, reason %d)", MAC2STR(e->mac), e->aid, e->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t *e = data;
        ESP_LOGI(TAG, "AP: client got " IPSTR, IP2STR(&e->ip));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        /* still associated but the lease is gone: not connected as far as anything above us is concerned */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.connected = false;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "lost IP address");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_retries = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_info.mode, "sta", sizeof s_info.mode);
        snprintf(s_info.ip, sizeof s_info.ip, IPSTR, IP2STR(&e->ip_info.ip));
        strlcpy(s_info.ssid, s_ssid, sizeof s_info.ssid);
        s_info.connected = true;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "connected: http://%s/  (http://%s.local/)", s_info.ip, s_host);
        if (s_ap_up) {
            /* joined after all: drop the setup AP */
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_ap_up = false;
            if (s_dns) { stop_dns_server(s_dns); s_dns = NULL; }
        }
    }
}

esp_err_t hl_net_start(const hl_cfg_t *cfg)
{
    s_lock = xSemaphoreCreateMutex();
    strlcpy(s_ssid, cfg->wifi_ssid, sizeof s_ssid);
    strlcpy(s_pass, cfg->wifi_pass, sizeof s_pass);
    strlcpy(s_host, cfg->hostname[0] ? cfg->hostname : "hdmilight", sizeof s_host);
    strlcpy(s_info.mode, "off", sizeof s_info.mode);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, s_host);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_host);
        mdns_instance_name_set("hdmilight-v3");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    return ESP_OK;
}

void hl_net_info(hl_net_info_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
    if (out->connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
    }
}

esp_err_t hl_net_join(const char *ssid, const char *pass)
{
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    strlcpy(s_pass, pass ? pass : "", sizeof s_pass);
    s_retries = 0;
    esp_wifi_disconnect();
    try_sta();
    return ESP_OK;
}

bool hl_net_hostname_ok(const char *h)
{
    size_t n = h ? strlen(h) : 0;
    if (n < 1 || n > sizeof s_host - 1 || h[0] == '-' || h[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        char ch = h[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
    }
    return true;
}

void hl_net_set_hostname(const char *host)
{
    if (!hl_net_hostname_ok(host) || !strcmp(host, s_host)) return;
    strlcpy(s_host, host, sizeof s_host);
    esp_netif_set_hostname(s_sta, s_host);
    esp_err_t err = mdns_hostname_set(s_host);
    ESP_LOGI(TAG, "hostname is now %s (http://%s.local/): %s", s_host, s_host, esp_err_to_name(err));
}

void hl_net_reconnect(void)
{
    if (!s_ssid[0]) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool joined = s_info.connected;
    xSemaphoreGive(s_lock);
    if (joined) {
        ESP_LOGW(TAG, "reconnecting to %s", s_ssid);
        esp_wifi_disconnect();   /* the DISCONNECTED event re-joins */
    } else {
        esp_err_t err = esp_wifi_connect();
        ESP_LOGW(TAG, "join attempt to %s: %s", s_ssid, esp_err_to_name(err));
    }
}

esp_err_t hl_net_scan_json(char *buf, size_t len)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) { snprintf(buf, len, "[]"); return err; }
    uint16_t n = 20;
    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);
    size_t pos = 0;
    pos += snprintf(buf + pos, len - pos, "[");
    for (int i = 0; i < n && pos < len - 80; i++) {
        /* skip duplicates (same SSID on several channels) */
        bool dup = false;
        for (int j = 0; j < i; j++) if (!strcmp((char *)recs[j].ssid, (char *)recs[i].ssid)) { dup = true; break; }
        if (dup || !recs[i].ssid[0]) continue;
        char esc[66]; size_t k = 0;
        for (const char *p = (char *)recs[i].ssid; *p && k < sizeof esc - 2; p++) { if (*p == '"' || *p == '\\') esc[k++] = '\\'; esc[k++] = *p; }
        esc[k] = 0;
        pos += snprintf(buf + pos, len - pos, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}", (pos > 1) ? "," : "", esc, recs[i].rssi,
                        recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
    }
    snprintf(buf + pos, len - pos, "]");
    return ESP_OK;
}
