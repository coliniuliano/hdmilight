/* Spring-damper colour smoothing, ported from HyperHDR v22
 * infinite-color-engine/InfiniteHybridRgbInterpolator.cpp (MIT), its default
 * smoothing mode: stiffness 150, damping 26, semi-implicit Euler, 50 Hz. */
#pragma once
#include "hl_types.h"

typedef struct {
    int    n;
    hl_f3 *target, *current, *velocity;
    float  stiffness, damping, smoothingFactor;
    int64_t lastUpdateMs;
    bool   complete;
    bool   initialized;
} hl_smooth;

int  hl_smooth_init(hl_smooth *s, int n_leds);
void hl_smooth_free(hl_smooth *s);
void hl_smooth_set_springiness(hl_smooth *s, float stiffness, float damping);
/* New per-LED targets (any 0..1 colour space; HyperHDR feeds nonlinear sRGB). */
void hl_smooth_set_targets(hl_smooth *s, const hl_f3 *targets, int64_t now_ms);
/* Advance the physics to now_ms. Call at the output rate (e.g. 50 Hz). */
void hl_smooth_update(hl_smooth *s, int64_t now_ms, float min_brightness);
/* Current colours, clamped to [min_brightness, 1]. */
void hl_smooth_get(const hl_smooth *s, float min_brightness, hl_f3 *out);
