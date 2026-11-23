#include "hl_yuv.h"

void hl_yuv_coefs(hl_yuv_coef_t coef, float *kr, float *kb)
{
    switch (coef) {
    case HL_YUV_FCC:    *kr = 0.3f;    *kb = 0.11f;   break;
    case HL_YUV_BT709:  *kr = 0.2126f; *kb = 0.0722f; break;
    case HL_YUV_BT2020: *kr = 0.2627f; *kb = 0.0593f; break;
    case HL_YUV_BT601:
    default:            *kr = 0.2990f; *kb = 0.1140f; break;
    }
}

hl_m34 hl_yuv_rgb2yuv_matrix(hl_range_t range, hl_yuv_coef_t coef)
{
    float Kr, Kb;
    hl_yuv_coefs(coef, &Kr, &Kb);
    const float Kg = 1.0f - Kr - Kb;
    const float Cr = 0.5f / (1.0f - Kb);
    const float Cb = 0.5f / (1.0f - Kr);
    float scaleY = 1.0f, addY = 0.0f, scaleUV = 1.0f, addUV = 128.0f / 255.0f;
    if (range == HL_RANGE_LIMITED) {
        scaleY = 219.0f / 255.0f;
        addY = 16.0f / 255.0f;
        scaleUV = 224.0f / 255.0f;
    }
    hl_m34 m;
    m.c0 = hl_f3_make(Kr * scaleY, -Kr * Cr * scaleUV, (1.f - Kr) * Cb * scaleUV);
    m.c1 = hl_f3_make(Kg * scaleY, -Kg * Cr * scaleUV, -Kg * Cb * scaleUV);
    m.c2 = hl_f3_make(Kb * scaleY, (1.f - Kb) * Cr * scaleUV, -Kb * Cb * scaleUV);
    m.c3 = hl_f3_make(addY, addUV, addUV);
    return m;
}

/* Inverse of an affine 3x4: A^-1 and -A^-1 b. */
static hl_m34 affine_inverse(const hl_m34 *m)
{
    /* A columns: c0 c1 c2 -> a[row][col] */
    float a[3][3] = {
        { m->c0.x, m->c1.x, m->c2.x },
        { m->c0.y, m->c1.y, m->c2.y },
        { m->c0.z, m->c1.z, m->c2.z },
    };
    float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
              - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
              + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    float id = 1.0f / det;
    float inv[3][3];
    inv[0][0] =  (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * id;
    inv[0][1] = -(a[0][1] * a[2][2] - a[0][2] * a[2][1]) * id;
    inv[0][2] =  (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
    inv[1][0] = -(a[1][0] * a[2][2] - a[1][2] * a[2][0]) * id;
    inv[1][1] =  (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id;
    inv[1][2] = -(a[0][0] * a[1][2] - a[0][2] * a[1][0]) * id;
    inv[2][0] =  (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * id;
    inv[2][1] = -(a[0][0] * a[2][1] - a[0][1] * a[2][0]) * id;
    inv[2][2] =  (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
    hl_m34 r;
    r.c0 = hl_f3_make(inv[0][0], inv[1][0], inv[2][0]);
    r.c1 = hl_f3_make(inv[0][1], inv[1][1], inv[2][1]);
    r.c2 = hl_f3_make(inv[0][2], inv[1][2], inv[2][2]);
    hl_f3 nb = hl_f3_scale(m->c3, -1.f);
    r.c3 = hl_m3_mul((const hl_m3 *)&r, nb);
    return r;
}

hl_m34 hl_yuv_yuv2rgb_matrix(hl_range_t range, hl_yuv_coef_t coef)
{
    hl_m34 f = hl_yuv_rgb2yuv_matrix(range, coef);
    return affine_inverse(&f);
}

hl_f3 hl_yuv_to_rgb(hl_range_t range, hl_yuv_coef_t coef, hl_f3 yuv)
{
    hl_m34 m = hl_yuv_yuv2rgb_matrix(range, coef);
    return hl_m34_mul(&m, yuv);
}

hl_f3 hl_rgb_to_yuv(hl_range_t range, hl_yuv_coef_t coef, hl_f3 rgb)
{
    hl_m34 m = hl_yuv_rgb2yuv_matrix(range, coef);
    return hl_m34_mul(&m, rgb);
}
