/* Minimal ESPHome native API server (plaintext, port 6053) exposing one light with on/off and brightness.
 * Home Assistant's built-in ESPHome integration finds it through mDNS (_esphomelib._tcp) and adds it with one
 * click; no broker or add-on involved.
 *
 * Portable: plain BSD sockets and no ESP-IDF calls, so the same file builds on the host for protocol tests
 * (tools/esphome_test). The ESP glue lives in homeassistant.c.
 *
 * Protocol reference: esphome/components/api/api.proto (frame = 0x00, varint length, varint type, protobuf). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name;            /* node name (hostname), max 31 chars */
    const char *friendly_name;
    const char *mac;             /* "AA:BB:CC:DD:EE:FF" */
    const char *version;         /* shown as the device's software version */
    const char *compile_time;
    const char *manufacturer;
    const char *model;
    uint32_t    webserver_port;  /* > 0: HA links to http://<host>:<port> */
    /* current light state; brightness 0..1 */
    void (*get)(void *ctx, bool *on, float *brightness);
    /* command from HA; only the fields with has_* set are meant */
    void (*set)(void *ctx, bool has_state, bool on, bool has_brightness, float brightness);
    void *ctx;
    void (*log)(const char *msg);   /* optional */
    int   idle_timeout_ms;          /* drop a client silent this long; 0 = 90 s (HA pings every 20 s) */
} hl_esph_ops_t;

/* Listen on `port` and serve forever (returns only if the socket cannot be set up, with -1).
 * Polls ops->get every poll_ms and pushes changes to subscribed clients.
 * Resource use is bounded: 1 listening socket + at most 3 clients. A client silent for idle_timeout_ms is dropped;
 * when all slots are taken the stalest client is evicted for the newcomer, so HA can always reconnect after a
 * Wi-Fi blip left a half-open connection behind. Socket errors never spin: the loop sleeps poll_ms first. */
int hl_esph_serve(const hl_esph_ops_t *ops, uint16_t port, int poll_ms);
