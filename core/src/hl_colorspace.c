#include "hl_colorspace.h"
#include <math.h>
#include <float.h>

#ifndef HL_PI
#define HL_PI 3.14159265358979323846f
#endif

float hl_srgb_to_linear(float input)
{
    const float alpha = 1.055010718947587f;
    const float beta  = 0.003041282560128f;
    if (input < 0.f) input = 0.f;
    if (input < 12.92f * beta) return input / 12.92f;
    return powf((input + (alpha - 1.0f)) / alpha, 2.4f);
}

float hl_linear_to_srgb(float input)
{
    const float alpha = 1.055010718947587f;
    const float beta  = 0.003041282560128f;
    if (input < 0.f) input = 0.f;
    if (input < beta) return input * 12.92f;
    return alpha * powf(input, 1.0f / 2.4f) - (alpha - 1.0f);
}

hl_f3 hl_srgb_to_linear3(hl_f3 v) { return hl_f3_make(hl_srgb_to_linear(v.x), hl_srgb_to_linear(v.y), hl_srgb_to_linear(v.z)); }
hl_f3 hl_linear_to_srgb3(hl_f3 v) { return hl_f3_make(hl_linear_to_srgb(v.x), hl_linear_to_srgb(v.y), hl_linear_to_srgb(v.z)); }

float hl_bt2020_to_linear(float input)
{
    const float alpha = 1.09929682680944f;
    const float beta  = 0.018053968510807f;
    if (input < 0.f) return -hl_bt2020_to_linear(-input);
    if (input < beta * 4.5f) return input / 4.5f;
    return powf((input + alpha - 1.f) / alpha, 1.f / 0.45f);
}

float hl_linear_to_bt2020(float input)
{
    const float alpha = 1.09929682680944f;
    const float beta  = 0.018053968510807f;
    if (input < 0.f) return -hl_linear_to_bt2020(-input);
    if (input < beta) return 4.5f * input;
    return alpha * powf(input, 0.45f) - (alpha - 1.f);
}

hl_f3 hl_bt2020_to_linear3(hl_f3 v) { return hl_f3_make(hl_bt2020_to_linear(v.x), hl_bt2020_to_linear(v.y), hl_bt2020_to_linear(v.z)); }

/* Evaluated in double: near code value 1.0 the denominator C2 - C3*x^(1/M2)
 * approaches zero and float arithmetic diverges from HyperHDR's double result
 * by several 8-bit counts on out-of-gamut inputs. Soft double on the P4 is fine
 * here: this runs per grid cell or once at LUT bake. */
float hl_pq_st2084(float scale, float nonlinear_f)
{
    const double M1 = (2610.0 / 16384.0);
    const double M2 = (2523.0 / 4096.0) * 128.0;
    const double C1 = (3424.0 / 4096.0);
    const double C2 = (2413.0 / 4096.0) * 32.0;
    const double C3 = (2392.0 / 4096.0) * 32.0;
    double nonlinear = nonlinear_f;
    if (nonlinear > 0.0) {
        double xpow = pow(nonlinear, 1.0 / M2);
        double num = xpow - C1; if (num < 0.0) num = 0.0;
        double den = C2 - C3 * xpow; if (den < DBL_MIN) den = DBL_MIN;
        nonlinear = pow(num / den, 1.0 / M1);
    } else if (nonlinear < 0.0) {
        return -hl_pq_st2084(scale, -nonlinear_f);
    } else {
        return 0.f;
    }
    return (float)((double)scale * nonlinear);
}

hl_f3 hl_pq_st2084_3(float scale, hl_f3 v) { return hl_f3_make(hl_pq_st2084(scale, v.x), hl_pq_st2084(scale, v.y), hl_pq_st2084(scale, v.z)); }

float hl_hlg_inverse_oetf(float t)
{
    if (t < 0.f) return -hl_hlg_inverse_oetf(-t);
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    if (t < 0.5f) return t * t / 3.0f;
    return (expf((t - c) / a) + b) * (1.0f / 12.0f);
}

hl_f3 hl_hlg_inverse_oetf3(hl_f3 v) { return hl_f3_make(hl_hlg_inverse_oetf(v.x), hl_hlg_inverse_oetf(v.y), hl_hlg_inverse_oetf(v.z)); }

hl_f3 hl_hlg_ootf(hl_f3 v, float gamma)
{
    if (gamma == 0.f) return v;
    float luma = 0.2627f * v.x + 0.6780f * v.y + 0.0593f * v.z;
    luma = powf(luma, gamma - 1.0f);
    return hl_f3_scale(v, luma);
}

hl_f3 hl_bt2020_to_bt709(hl_f3 a)
{
    return hl_f3_make( 1.6605f * a.x - 0.5876f * a.y - 0.0728f * a.z,
                      -0.1246f * a.x + 1.1329f * a.y - 0.0083f * a.z,
                      -0.0182f * a.x - 0.1006f * a.y + 1.1187f * a.z);
}

/* Matrices from HyperHDR ColorSpace.h, row-major as listed there. Only the sRGB pair is needed: the BT.2020
 * primaries conversion goes through hl_bt2020_to_bt709's fixed coefficients, as HyperHDR's does. */
static const hl_m3 *m_srgb_to_xyz(void)
{
    static hl_m3 m; static int init = 0;
    if (!init) { m = hl_m3_rows(0.4124564f, 0.3575761f, 0.1804375f,
                                0.2126729f, 0.7151522f, 0.0721750f,
                                0.0193339f, 0.1191920f, 0.9503041f); init = 1; }
    return &m;
}
static const hl_m3 *m_xyz_to_srgb(void)
{
    /* inverse of the above (standard sRGB D65) */
    static hl_m3 m; static int init = 0;
    if (!init) { m = hl_m3_rows( 3.2404542f, -1.5371385f, -0.4985314f,
                                -0.9692660f,  1.8760108f,  0.0415560f,
                                 0.0556434f, -0.2040259f,  1.0572252f); init = 1; }
    return &m;
}

hl_f3 hl_srgb_to_xyz(hl_f3 rgb) { return hl_m3_mul(m_srgb_to_xyz(), rgb); }
hl_f3 hl_xyz_to_srgb(hl_f3 xyz) { return hl_m3_mul(m_xyz_to_srgb(), xyz); }

hl_f3 hl_xyz_to_lab(hl_f3 xyz)
{
    float x = xyz.x / 95.047f, y = xyz.y / 100.0f, z = xyz.z / 108.883f;
    x = (x > 0.008856f) ? cbrtf(x) : (7.787f * x + 16.0f / 116.0f);
    y = (y > 0.008856f) ? cbrtf(y) : (7.787f * y + 16.0f / 116.0f);
    z = (z > 0.008856f) ? cbrtf(z) : (7.787f * z + 16.0f / 116.0f);
    return hl_f3_make((116.0f * y) - 16.f, 500.f * (x - y), 200.f * (y - z));
}

hl_f3 hl_lab_to_xyz(hl_f3 lab)
{
    float y = (lab.x + 16.0f) / 116.0f;
    float x = lab.y / 500.0f + y;
    float z = y - lab.z / 200.0f;
    float x3 = x * x * x, y3 = y * y * y, z3 = z * z * z;
    return hl_f3_make(((x3 > 0.008856f) ? x3 : ((x - 16.0f / 116.0f) / 7.787f)) * 95.047f,
                      ((y3 > 0.008856f) ? y3 : ((y - 16.0f / 116.0f) / 7.787f)) * 100.0f,
                      ((z3 > 0.008856f) ? z3 : ((z - 16.0f / 116.0f) / 7.787f)) * 108.883f);
}

hl_f3 hl_lab_to_lch(hl_f3 lab)
{
    float c = sqrtf(lab.y * lab.y + lab.z * lab.z);
    float h = atan2f(lab.z, lab.y);
    if (h > 0.f) h = (h / HL_PI) * 180.0f;
    else         h = 360.0f - (fabsf(h) / HL_PI) * 180.0f;
    return hl_f3_make(lab.x, c, h);
}

hl_f3 hl_lch_to_lab(hl_f3 lch)
{
    /* Verbatim HyperHDR quirk: hue < 360 gets +360; harmless for cos/sin. */
    if (lch.z > 360.0f) lch.z -= 360.0f;
    else if (lch.z < 360.0f) lch.z += 360.0f;
    float h = lch.z * HL_PI / 180.0f;
    return hl_f3_make(lch.x, cosf(h) * lch.y, sinf(h) * lch.y);
}

hl_f3 hl_xyz_to_lch(hl_f3 xyz) { return hl_lab_to_lch(hl_xyz_to_lab(xyz)); }
hl_f3 hl_lch_to_xyz(hl_f3 lch) { return hl_lab_to_xyz(hl_lch_to_lab(lch)); }

uint16_t hl_srgb8_to_linear16(uint8_t v)
{
    static uint16_t table[256];
    static int init = 0;
    if (!init) {
        for (int i = 0; i < 256; i++) {
            float lin = hl_srgb_to_linear((float)i / 255.0f);
            table[i] = (uint16_t)(lin * 65535.0f + 0.5f);
        }
        init = 1;
    }
    return table[v];
}
