#include "hl_smooth.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

int hl_smooth_init(hl_smooth *s, int n)
{
    memset(s, 0, sizeof(*s));
    s->n = n;
    s->target = (hl_f3 *)calloc(n, sizeof(hl_f3));
    s->current = (hl_f3 *)calloc(n, sizeof(hl_f3));
    s->velocity = (hl_f3 *)calloc(n, sizeof(hl_f3));
    if (!s->target || !s->current || !s->velocity) { hl_smooth_free(s); return -1; }
    s->stiffness = 150.f;
    s->damping = 26.f;
    s->smoothingFactor = 0.f;
    s->complete = true;
    return 0;
}

void hl_smooth_free(hl_smooth *s)
{
    free(s->target); free(s->current); free(s->velocity);
    memset(s, 0, sizeof(*s));
}

void hl_smooth_set_springiness(hl_smooth *s, float stiffness, float damping)
{
    s->stiffness = stiffness < 0.1f ? 0.1f : stiffness;
    s->damping = damping < 0.1f ? 0.1f : damping;
}

void hl_smooth_set_targets(hl_smooth *s, const hl_f3 *targets, int64_t now_ms)
{
    float delta = 0.f;
    if (!s->complete) delta = hl_clampf((float)(now_ms - s->lastUpdateMs), 0.f, 100.f);
    now_ms -= (int64_t)delta;

    if (!s->initialized) {
        /* first targets: jump (HyperHDR: current.size() != targets.size()) */
        s->initialized = true;
        s->lastUpdateMs = now_ms;
        memcpy(s->current, targets, sizeof(hl_f3) * s->n);
        memcpy(s->target, targets, sizeof(hl_f3) * s->n);
        memset(s->velocity, 0, sizeof(hl_f3) * s->n);
        s->complete = true;
    } else {
        if (s->smoothingFactor > 0.f) {
            const float inv = 1.f - s->smoothingFactor;
            for (int i = 0; i < s->n; i++)
                s->target[i] = hl_f3_add(hl_f3_scale(s->target[i], s->smoothingFactor), hl_f3_scale(targets[i], inv));
        } else {
            memcpy(s->target, targets, sizeof(hl_f3) * s->n);
        }
        s->complete = false;
    }
}

void hl_smooth_update(hl_smooth *s, int64_t now_ms, float minBrightness)
{
    if (s->complete) { s->lastUpdateMs = now_ms; return; }
    float dt = hl_clampf((float)(now_ms - s->lastUpdateMs), 0.001f, 100.0f);
    s->lastUpdateMs = now_ms;
    const float FINISH = 0.0013732906f / 10.f;
    const float VEL = 0.0005f;
    bool all_done = true;
    for (int i = 0; i < s->n; i++) {
        hl_f3 *cur = &s->current[i], *vel = &s->velocity[i];
        hl_f3 tgt = s->target[i];
        hl_f3 diff = hl_f3_sub(tgt, *cur);
        hl_f3 adiff = hl_f3_make(fabsf(diff.x), fabsf(diff.y), fabsf(diff.z));
        hl_f3 avel = hl_f3_make(fabsf(vel->x), fabsf(vel->y), fabsf(vel->z));
        if (hl_f3_maxelem(adiff) < FINISH && hl_f3_maxelem(avel) < VEL) {
            *cur = tgt; *vel = hl_f3_make(0, 0, 0);
            continue;
        }
        hl_f3 acc = hl_f3_sub(hl_f3_scale(diff, s->stiffness), hl_f3_scale(*vel, s->damping));
        *vel = hl_f3_add(*vel, hl_f3_scale(acc, dt * 0.001f));
        *cur = hl_f3_add(*cur, hl_f3_scale(*vel, dt * 0.001f));
        float *c = &cur->x, *v = &vel->x;
        for (int k = 0; k < 3; k++)
            if (c[k] < minBrightness || c[k] > 1.0f) { v[k] = 0.f; c[k] = hl_clampf(c[k], minBrightness, 1.0f); }
        all_done = false;
    }
    s->complete = all_done;
}

void hl_smooth_get(const hl_smooth *s, float minBrightness, hl_f3 *out)
{
    for (int i = 0; i < s->n; i++)
        out[i] = hl_f3_make(hl_clampf(s->current[i].x, minBrightness, 1.f),
                            hl_clampf(s->current[i].y, minBrightness, 1.f),
                            hl_clampf(s->current[i].z, minBrightness, 1.f));
}
