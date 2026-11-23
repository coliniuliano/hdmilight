#include "hl_output.h"
#include "hl_colorspace.h"
#include <math.h>
#include <string.h>
#include <float.h>

#define D 9
#define G 512

void hl_output_cfg_default(hl_output_cfg *c)
{
    static const uint8_t ideal[HL_CAL_N][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {0,255,255}, {255,0,255}, {255,255,0}, {255,255,255}, {0,0,0} };
    memset(c, 0, sizeof(*c));
    c->temp = HL_TEMP_OFF;
    c->temp_tint = hl_f3_make(1.f, 1.f, 1.f);
    c->classic = true;
    memcpy(c->cal, ideal, sizeof ideal);
    c->scale = 1.f;
    c->gamma = 1.5f;
    c->luminance = 1.f;
    c->saturation = 1.f;
    c->backlight = 0.0039f;
    c->backlight_colored = true;
    c->power_limit = 1.f;
}

static hl_f3 f3_of(const uint8_t *c) { return hl_f3_make(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f); }
static hl_f3 lerp3(hl_f3 a, hl_f3 b, float t) { return hl_f3_add(a, hl_f3_scale(hl_f3_sub(b, a), t)); }
static bool same(const uint8_t *a, const uint8_t *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

void hl_output_init(hl_output *o, const hl_output_cfg *c)
{
    static const uint8_t ideal[HL_CAL_N][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {0,255,255}, {255,0,255}, {255,255,0}, {255,255,255}, {0,0,0} };
    memset(o, 0, sizeof(*o));
    o->cfg = *c;

    switch (c->temp) {
    case HL_TEMP_WARM:    o->tint = hl_f3_make(1.f, 0.93f, 0.85f); o->has_tint = true; break;
    case HL_TEMP_NEUTRAL: o->tint = hl_f3_make(1.f, 1.f, 1.f);     o->has_tint = true; break;
    case HL_TEMP_COLD:    o->tint = hl_f3_make(0.9f, 0.95f, 1.f);  o->has_tint = true; break;
    case HL_TEMP_CUSTOM:  o->tint = c->temp_tint;                  o->has_tint = true; break;
    default: break;
    }

    const int ncal = c->classic ? 3 : HL_CAL_N;
    bool identity = true;
    for (int i = 0; i < ncal; i++) if (!same(c->cal[i], ideal[i])) identity = false;
    if (!identity && c->classic) {
        const hl_f3 r = f3_of(c->cal[0]), g = f3_of(c->cal[1]), b = f3_of(c->cal[2]);
        o->cal_matrix = hl_m3_rows(r.x, r.y, r.z, g.x, g.y, g.z, b.x, b.y, b.z);   /* transpose({r,g,b}) */
        o->cal_mode = 1;
    } else if (!identity) {
        hl_f3 t[HL_CAL_N];
        for (int i = 0; i < HL_CAL_N; i++) t[i] = f3_of(c->cal[i]);
        const hl_f3 r = t[0], g = t[1], b = t[2], cy = t[3], m = t[4], y = t[5], w = t[6], k = t[7];
        for (int iz = 0; iz < D; iz++) {
            const float tz = (float)iz / (D - 1);
            const hl_f3 k_r = lerp3(k, r, tz), g_y = lerp3(g, y, tz), b_m = lerp3(b, m, tz), c_w = lerp3(cy, w, tz);
            for (int iy = 0; iy < D; iy++) {
                const float ty = (float)iy / (D - 1);
                const hl_f3 kr_gy = lerp3(k_r, g_y, ty), bm_cw = lerp3(b_m, c_w, ty);
                for (int ix = 0; ix < D; ix++)
                    o->cal_lut[ix + iy * D + iz * D * D] = lerp3(kr_gy, bm_cw, (float)ix / (D - 1));
            }
        }
        o->cal_mode = 2;
    }

    const float sc = hl_clampf(c->scale, 0.f, 2.f);
    o->has_scale = sc != 1.f;
    o->cfg.scale = sc;

    o->has_gamma = c->gamma != 1.f;
    if (o->has_gamma) for (int i = 0; i < G; i++) o->gamma_lut[i] = powf((float)i / (G - 1), c->gamma);

    o->has_hsv = !(fabsf(c->luminance - 1.f) < 1e-5f && fabsf(c->saturation - 1.f) < 1e-5f);
    o->cfg.luminance = c->luminance < 0.f ? 0.f : c->luminance;
    o->cfg.saturation = c->saturation < 0.f ? 0.f : c->saturation;

    float bl = c->backlight >= 1.f ? 0.0039f : c->backlight;
    o->cfg.backlight = bl;
    o->has_backlight = bl != 0.f;

    const float pl = hl_clampf(c->power_limit, 0.f, 1.f);
    o->cfg.power_limit = pl;
    o->has_power = pl != 1.f;
}

float hl_output_min_backlight(const hl_output *o) { return o->has_backlight ? o->cfg.backlight : 0.f; }

static hl_f3 rgb2hsv(hl_f3 c)
{
    const float cmax = hl_f3_maxelem(c);
    const float cmin = fminf(c.x, fminf(c.y, c.z));
    const float diff = cmax - cmin;
    hl_f3 h;
    if (cmax == cmin) h.x = 0.f;
    else if (cmax == c.x) h.x = fmodf(60.f * ((c.y - c.z) / diff) + 360.f, 360.f);
    else if (cmax == c.y) h.x = fmodf(60.f * ((c.z - c.x) / diff) + 120.f, 360.f);
    else                  h.x = fmodf(60.f * ((c.x - c.y) / diff) + 240.f, 360.f);
    h.y = (cmax <= FLT_EPSILON) ? 0.f : diff / cmax;
    h.z = cmax;
    return h;
}

static hl_f3 hsv2rgb(hl_f3 h)
{
    const float c = h.z * h.y;
    const float x = c * (1.f - fabsf(fmodf(h.x / 60.f, 2.f) - 1.f));
    const float m = h.z - c;
    hl_f3 p;
    switch ((int)(h.x / 60.f)) {
    case 0:  p = hl_f3_make(c, x, 0.f); break;
    case 1:  p = hl_f3_make(x, c, 0.f); break;
    case 2:  p = hl_f3_make(0.f, c, x); break;
    case 3:  p = hl_f3_make(0.f, x, c); break;
    case 4:  p = hl_f3_make(x, 0.f, c); break;
    default: p = hl_f3_make(c, 0.f, x); break;
    }
    return hl_f3_make(p.x + m, p.y + m, p.z + m);
}

static float lut_lerp(const float *lut, float v)
{
    const float pos = v * (G - 1);
    int i0 = (int)pos, i1 = i0 + 1 < G - 1 ? i0 + 1 : G - 1;
    return lut[i0] + (lut[i1] - lut[i0]) * (pos - (float)i0);
}

static hl_f3 calibrate(const hl_output *o, hl_f3 c)
{
    if (o->cal_mode == 1) return hl_f3_clamp01(hl_m3_mul(&o->cal_matrix, c));
    const float r = c.x * (D - 1), g = c.y * (D - 1), b = c.z * (D - 1);
    const int r0 = (int)r, g0 = (int)g, b0 = (int)b;
    const int r1 = r0 + 1 < D - 1 ? r0 + 1 : D - 1, g1 = g0 + 1 < D - 1 ? g0 + 1 : D - 1, b1 = b0 + 1 < D - 1 ? b0 + 1 : D - 1;
    const float rf = r - r0, gf = g - g0, bf = b - b0;
    const hl_f3 *L = o->cal_lut;
#define AT(bb, gg, rr) L[(bb) + (gg) * D + (rr) * D * D]
    const hl_f3 c00 = lerp3(AT(b0, g0, r0), AT(b1, g0, r0), bf), c10 = lerp3(AT(b0, g1, r0), AT(b1, g1, r0), bf);
    const hl_f3 c01 = lerp3(AT(b0, g0, r1), AT(b1, g0, r1), bf), c11 = lerp3(AT(b0, g1, r1), AT(b1, g1, r1), bf);
#undef AT
    return hl_f3_clamp01(lerp3(lerp3(c00, c10, gf), lerp3(c01, c11, gf), rf));
}

void hl_output_process(const hl_output *o, hl_f3 *colors, int n)
{
    for (int i = 0; i < n; i++) {
        hl_f3 c = colors[i];
        if (o->has_tint) c = hl_f3_clamp01(hl_f3_mul(c, o->tint));
        if (o->cal_mode) c = calibrate(o, c);
        if (o->has_scale) c = hl_f3_clamp01(hl_f3_scale(c, o->cfg.scale));
        c = hl_linear_to_srgb3(hl_f3_clamp01(c));
        if (o->has_gamma) c = hl_f3_make(lut_lerp(o->gamma_lut, c.x), lut_lerp(o->gamma_lut, c.y), lut_lerp(o->gamma_lut, c.z));
        if (o->has_hsv) {
            hl_f3 h = rgb2hsv(c);
            h.z = fminf(h.z * o->cfg.luminance, 1.f);
            h.y = fminf(h.y * o->cfg.saturation, 1.f);
            c = hl_f3_clamp01(hsv2rgb(h));
        }
        if (o->has_backlight) {
            const float bl = o->cfg.backlight;
            if (o->cfg.backlight_colored) c = hl_f3_make(fmaxf(c.x, bl), fmaxf(c.y, bl), fmaxf(c.z, bl));
            else if ((c.x + c.y + c.z) / 3.f < bl) c = hl_f3_make(bl, bl, bl);
        }
        colors[i] = c;
    }
    if (o->has_power) {
        float total = 0.f;
        for (int i = 0; i < n; i++) total += colors[i].x + colors[i].y + colors[i].z;
        const float allowed = (float)n * 3.f * o->cfg.power_limit;
        if (total > 0.f && allowed < total) {
            const float s = allowed / total;
            for (int i = 0; i < n; i++) colors[i] = hl_f3_scale(colors[i], s);
        }
    }
}
