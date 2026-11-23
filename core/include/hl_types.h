/* hdmilight-v3 core: shared small types.
 * Portable C11, no platform dependencies. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { float x, y, z; } hl_f3;

static inline hl_f3 hl_f3_make(float x, float y, float z) { hl_f3 r = { x, y, z }; return r; }
static inline hl_f3 hl_f3_add(hl_f3 a, hl_f3 b) { return hl_f3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline hl_f3 hl_f3_sub(hl_f3 a, hl_f3 b) { return hl_f3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline hl_f3 hl_f3_mul(hl_f3 a, hl_f3 b) { return hl_f3_make(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline hl_f3 hl_f3_scale(hl_f3 a, float s) { return hl_f3_make(a.x * s, a.y * s, a.z * s); }
static inline float hl_f3_maxelem(hl_f3 a) { float m = a.x > a.y ? a.x : a.y; return m > a.z ? m : a.z; }
static inline float hl_clampf(float v, float lo, float hi) { if (v != v) return lo; /* NaN */ return v < lo ? lo : (v > hi ? hi : v); }
static inline hl_f3 hl_f3_clamp01(hl_f3 a) { return hl_f3_make(hl_clampf(a.x, 0.f, 1.f), hl_clampf(a.y, 0.f, 1.f), hl_clampf(a.z, 0.f, 1.f)); }

/* 3x3 matrix stored as three COLUMN vectors, matching HyperHDR's linalg layout
 * (mul(M, v) = c0*v.x + c1*v.y + c2*v.z). */
typedef struct { hl_f3 c0, c1, c2; } hl_m3;
static inline hl_f3 hl_m3_mul(const hl_m3 *m, hl_f3 v)
{
    return hl_f3_add(hl_f3_add(hl_f3_scale(m->c0, v.x), hl_f3_scale(m->c1, v.y)), hl_f3_scale(m->c2, v.z));
}
/* Build from a row-major list (the way HyperHDR's matrix({...}) helper does). */
static inline hl_m3 hl_m3_rows(float m00, float m01, float m02,
                               float m10, float m11, float m12,
                               float m20, float m21, float m22)
{
    hl_m3 r;
    r.c0 = hl_f3_make(m00, m10, m20);
    r.c1 = hl_f3_make(m01, m11, m21);
    r.c2 = hl_f3_make(m02, m12, m22);
    return r;
}

/* Affine 3x4: four column vectors; the fourth is the offset (HyperHDR double4x4 with (v,1)). */
typedef struct { hl_f3 c0, c1, c2, c3; } hl_m34;
static inline hl_f3 hl_m34_mul(const hl_m34 *m, hl_f3 v)
{
    return hl_f3_add(hl_m3_mul((const hl_m3 *)m, v), m->c3);
}
