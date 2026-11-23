/* Automatic HDR tone-mapping switch, ported from HyperHDR v22
 * base/AutomaticToneMapping.cpp (MIT). Tracks the running maximum Y/U/V of the
 * signal: a raw HDR signal stays dark and desaturated (never exceeds the
 * thresholds), a bright SDR picture exceeds them. */
#pragma once
#include "hl_types.h"

typedef struct { uint8_t y, u, v; } hl_yuv_thresholds;

typedef struct {
    bool enabled;
    hl_yuv_thresholds config;       /* defaults 180/175/160 */
    int  time_to_enable_s;          /* default 30 */
    int  time_to_disable_ms;        /* default 500 */
    /* state */
    hl_yuv_thresholds running;
    bool triggered;
    bool mode_sdr;                  /* true = tone mapping off */
    int64_t started_s, ending_ms;
} hl_autohdr;

void hl_autohdr_init(hl_autohdr *a, bool enabled);
/* Feed one YUV sample (call for a subsample of pixels per frame). */
static inline void hl_autohdr_sample(hl_autohdr *a, uint8_t y, uint8_t u, uint8_t v)
{
    if (y > a->running.y) a->running.y = y;
    if (u > a->running.u) a->running.u = u;
    if (v > a->running.v) a->running.v = v;
}
/* Call once per frame. Returns true when tone mapping should be ON. */
bool hl_autohdr_finalize(hl_autohdr *a, int64_t now_ms);
