/* Host build of firmware/main/esphome_api.c with a fake light, for protocol tests against aioesphomeapi
 * (the client Home Assistant uses). Listens on 127.0.0.1 only. SIGUSR1 simulates a change made in the web UI
 * (light off, brightness 42 %). Usage: host_esphome <port> [idle_timeout_ms] */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "esphome_api.h"

static volatile bool s_on = true;
static volatile float s_bri = 0.6f;

static void get_state(void *ctx, bool *on, float *bri) { *on = s_on; *bri = s_bri; }
static void set_state(void *ctx, bool has_state, bool on, bool has_bri, float bri)
{
    if (has_bri) s_bri = bri;
    if (has_state) s_on = on;
    printf("SET has_state=%d on=%d has_bri=%d bri=%.3f\n", has_state, on, has_bri, bri);
    fflush(stdout);
}
static void on_usr1(int sig) { s_on = false; s_bri = 0.42f; }
static void log_cb(const char *m) { printf("LOG %s\n", m); fflush(stdout); }

int main(int argc, char **argv)
{
    signal(SIGUSR1, on_usr1);
    hl_esph_ops_t ops = {
        .name = "hdmilight", .friendly_name = "hdmilight", .mac = "AA:BB:CC:DD:EE:FF",
        .version = "test", .compile_time = "now", .manufacturer = "hdmilight", .model = "hdmilight-v3 (host)",
        .webserver_port = 80, .get = get_state, .set = set_state, .log = log_cb,
        .idle_timeout_ms = argc > 2 ? atoi(argv[2]) : 0,
    };
    return hl_esph_serve(&ops, argc > 1 ? (uint16_t)atoi(argv[1]) : 16053, 100) ? 1 : 0;
}
