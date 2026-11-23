/* Network watchdog. Keeps the board reachable without ever restarting it over a network problem: with the
 * router down (or the TV used offline) the LEDs keep running and the board keeps trying to rejoin, indefinitely.
 *
 * Every CHECK_PERIOD_S, once the board has been up SETTLE_S:
 *  - joined: ping the gateway (exercises the whole P4 -> C6 -> router path). If it stopped answering for
 *    PING_FAIL_LIMIT checks in a row (3 min), drop and re-join Wi-Fi, again every 3 min while it lasts.
 *  - not joined: start a join attempt every NO_NET_KICK_S. net.c already retries every 3 s on each disconnect
 *    event; this covers the cases where that chain stops (an attempt that produces no event, associated without
 *    an address).
 *  - always: fetch /favicon.ico from our own web server over loopback. That path does not touch Wi-Fi, so a
 *    network outage cannot fail it; failing means the board itself is wedged (e.g. out of sockets). Only this
 *    restarts the board, after HTTP_FAIL_LIMIT checks in a row (2 min).
 * Checks only count failures after passing once since boot ("armed"): a gateway that ignores ping, or a missing
 * loopback, is never mistaken for a fault. The restart reason is saved and shown by /api/status afterwards. */
#include "netwatch.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "nvs.h"
#include "ping/ping_sock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "net.h"
#include "homeassistant.h"

static const char *TAG = "netwatch";
#define SETTLE_S         90
#define CHECK_PERIOD_S   30
#define PING_FAIL_LIMIT  6      /* x 30 s: re-join */
#define HTTP_FAIL_LIMIT  4      /* x 30 s: restart */
#define NO_NET_KICK_S    60
#define NVS_NS           "hl_diag"

static char s_last[96];

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "crash (panic)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT:      return "external reset";
    default:               return "other";
    }
}

void hl_netwatch_last_restart(char *buf, size_t len) { strlcpy(buf, s_last, len); }

/* Read and clear the reason saved before a watchdog restart, combine with the hardware reset reason. */
static void load_last_restart(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    char wd[64] = "";
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        size_t l = sizeof wd;
        if (nvs_get_str(h, "wd_reason", wd, &l) == ESP_OK) { nvs_erase_key(h, "wd_reason"); nvs_commit(h); }
        else wd[0] = 0;
        nvs_close(h);
    }
    /* the saved reason only explains this boot if this boot was a software restart */
    if (wd[0] && rr == ESP_RST_SW) snprintf(s_last, sizeof s_last, "network watchdog: %s", wd);
    else snprintf(s_last, sizeof s_last, "%s", reset_name(rr));
    ESP_LOGW(TAG, "last restart: %s", s_last);
}

static void restart_for(const char *why)
{
    ESP_LOGE(TAG, "restarting: %s", why);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, "wd_reason", why); nvs_commit(h); nvs_close(h); }
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the log reach the console and the ring */
    esp_restart();
}

/* ---- gateway ping ------------------------------------------------------- */
static SemaphoreHandle_t s_ping_done;
static void ping_end(esp_ping_handle_t hdl, void *arg) { xSemaphoreGive(s_ping_done); }

/* 1 = a reply came back, 0 = no reply, -1 = could not ping (no gateway, no resources: not counted as a failure) */
static int ping_gateway(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.gw.addr == 0) return -1;
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.count = 3; pc.interval_ms = 300; pc.timeout_ms = 1000; pc.data_size = 16;
    pc.target_addr.type = IPADDR_TYPE_V4;
    pc.target_addr.u_addr.ip4.addr = ip.gw.addr;
    esp_ping_callbacks_t cb = { .on_ping_end = ping_end };
    esp_ping_handle_t ph;
    if (esp_ping_new_session(&pc, &cb, &ph) != ESP_OK) return -1;
    xSemaphoreTake(s_ping_done, 0);
    esp_ping_start(ph);
    bool ended = xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(6000)) == pdTRUE;
    uint32_t replies = 0;
    esp_ping_get_profile(ph, ESP_PING_PROF_REPLY, &replies, sizeof replies);
    if (!ended) esp_ping_stop(ph);
    esp_ping_delete_session(ph);
    return replies > 0 ? 1 : 0;
}

/* ---- web server self-check ---------------------------------------------- */
/* 1 = answered, 0 = did not (including: no socket available, which is itself the failure being watched).
 * The request must carry at least one header: esp_http_server mishandles a header-less request ("GET / HTTP/1.0
 * \r\n\r\n") by pushing everything after the method back into the session's pending buffer once it is done
 * (cb_no_body pauses at last.at, which still points at the URL), then parsing that leftover as a second request:
 * "400 Bad Request" plus send errors in the log on every check. We also half-close after the request and read the
 * reply to EOF: the server then sees a clean FIN and closes the session itself, whereas closing with unread
 * reply bytes makes lwIP send an RST, which the server logs as a recv/send error. */
static int http_selfcheck(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(80), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int ok = 0;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        static const char req[] = "GET /favicon.ico HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        if (send(fd, req, sizeof req - 1, 0) == (int)(sizeof req - 1)) {
            shutdown(fd, SHUT_WR);   /* request complete: the server answers, then reads EOF and closes */
            char resp[16] = { 0 }, sink[64];
            int got = 0, total = 0;
            for (;;) {   /* each recv waits at most 5 s; the reply (a 204) is under 100 bytes */
                char *dst = got < 12 ? resp + got : sink;
                int r = recv(fd, dst, got < 12 ? 12 - got : (int)sizeof sink, 0);
                if (r <= 0) break;   /* 0 = server closed after answering; < 0 = timeout or error */
                if (got < 12) got += r;
                if ((total += r) > 4096) break;   /* not our 204: stop rather than drain forever */
            }
            ok = got >= 12 && strncmp(resp, "HTTP/1.", 7) == 0;   /* any HTTP status: the server is serving */
        }
    }
    close(fd);
    return ok;
}

static void netwatch_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(SETTLE_S * 1000));
    bool ping_armed = false, http_armed = false;
    int ping_fail = 0, http_fail = 0;
    int64_t no_net_since = 0, last_kick = 0;
    for (unsigned round = 0;; round++) {
        if (round % 10 == 0)   /* every 5 min: how close the network tasks have come to their stack limits */
            ESP_LOGI(TAG, "stack headroom: netwatch %u B, esphome_api %d B, free heap %u B",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL), hl_homeassistant_stack_free(),
                     (unsigned)esp_get_free_heap_size());
        hl_net_info_t ni; hl_net_info(&ni);
        int64_t now = esp_timer_get_time() / 1000000;
        if (ni.connected) {
            no_net_since = 0;
            int p = ping_gateway();
            if (p == 1) { if (!ping_armed) ESP_LOGI(TAG, "gateway answers ping: watching it"); ping_armed = true; ping_fail = 0; }
            else if (p == 0 && ping_armed) { ping_fail++; ESP_LOGW(TAG, "gateway did not answer (%d/%d)", ping_fail, PING_FAIL_LIMIT); }
            if (ping_armed && ping_fail >= PING_FAIL_LIMIT) {
                ESP_LOGW(TAG, "gateway unreachable for 3 min: re-joining Wi-Fi");
                ping_fail = 0;
                hl_net_reconnect();
            }
        } else {
            /* no network: never restart for this (LEDs keep working offline); just make sure attempts continue */
            if (!no_net_since) no_net_since = now;
            if (now - no_net_since >= NO_NET_KICK_S && now - last_kick >= NO_NET_KICK_S) {
                ESP_LOGW(TAG, "no network for %d s: join attempt", (int)(now - no_net_since));
                last_kick = now;
                hl_net_reconnect();
            }
        }
        /* the web server serves the setup AP too, so check it in every mode */
        int h = http_selfcheck();
        if (h) { if (!http_armed) ESP_LOGI(TAG, "web server answers: watching it"); http_armed = true; http_fail = 0; }
        else if (http_armed) { http_fail++; ESP_LOGW(TAG, "web server did not answer (%d/%d)", http_fail, HTTP_FAIL_LIMIT); }
        if (http_armed && http_fail >= HTTP_FAIL_LIMIT) restart_for("web server not answering for 2 min");
        vTaskDelay(pdMS_TO_TICKS(CHECK_PERIOD_S * 1000));
    }
}

esp_err_t hl_netwatch_start(void)
{
    load_last_restart();
    s_ping_done = xSemaphoreCreateBinary();
    if (!s_ping_done) return ESP_ERR_NO_MEM;
    return xTaskCreatePinnedToCore(netwatch_task, "netwatch", 4096, NULL, 2, NULL, 1) == pdPASS ? ESP_OK : ESP_FAIL;
}
