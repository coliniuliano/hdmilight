#include "hl_autohdr.h"
#include <string.h>

void hl_autohdr_init(hl_autohdr *a, bool enabled)
{
    memset(a, 0, sizeof(*a));
    a->enabled = enabled;
    a->config.y = 180; a->config.u = 175; a->config.v = 160;
    a->time_to_enable_s = 30;
    a->time_to_disable_ms = 500;
    a->running = a->config;
    a->mode_sdr = true;
}

bool hl_autohdr_finalize(hl_autohdr *a, int64_t now_ms)
{
    if (!a->enabled) return !a->mode_sdr;

    /* HyperHDR: _running starts at the config thresholds and only grows; any
     * growth means the picture exceeded a threshold at least once. */
    bool triggered = (a->config.y != a->running.y || a->config.u != a->running.u ||
                      a->config.v != a->running.v) || a->triggered;

    if (triggered && !a->mode_sdr) {
        if (a->ending_ms == 0 || a->ending_ms > now_ms) a->ending_ms = now_ms;
        else if (a->ending_ms + a->time_to_disable_ms <= now_ms) a->mode_sdr = true;   /* tone mapping OFF */
    } else if (!triggered && a->mode_sdr) {
        int64_t now_s = now_ms / 1000;
        if (a->started_s == 0 || a->started_s > now_s) a->started_s = now_s;
        else if (a->started_s + a->time_to_enable_s <= now_s) a->mode_sdr = false;    /* tone mapping ON */
    }
    if (!triggered) a->ending_ms = 0; else a->started_s = 0;

    a->running = a->config;
    a->triggered = false;
    return !a->mode_sdr;
}
