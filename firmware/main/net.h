/* Wi-Fi manager: station with the stored credentials, falling back to a setup
 * access point; mDNS so the UI is reachable as http://<hostname>.local. */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "config.h"

typedef struct {
    char mode[8];        /* "sta", "ap", "off" */
    char ip[16];
    char ssid[33];       /* joined or served SSID */
    int  rssi;
    bool connected;
} hl_net_info_t;

esp_err_t hl_net_start(const hl_cfg_t *cfg);
void      hl_net_info(hl_net_info_t *out);
/* Store new credentials and reconnect (returns before the result is known). */
esp_err_t hl_net_join(const char *ssid, const char *pass);
/* Kick the station side: drop and re-join if joined, start a join attempt if not. Never touches the setup AP.
 * Backstop for the event-driven retry (which stops if an attempt produces no event). */
void      hl_net_reconnect(void);
/* A usable hostname: 1 to 31 letters, digits or hyphens, no hyphen at either end. */
bool      hl_net_hostname_ok(const char *host);
/* Take a new hostname now: mDNS answers to it at once, DHCP carries it from the next join. */
void      hl_net_set_hostname(const char *host);
/* Scan; writes a JSON array string into buf. */
esp_err_t hl_net_scan_json(char *buf, size_t len);
