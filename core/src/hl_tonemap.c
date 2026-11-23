#include "hl_tonemap.h"
#include <math.h>
#include <string.h>

void hl_model_default(hl_model *m)
{
    memset(m, 0, sizeof(*m));
    m->coef = HL_YUV_BT601;
    /* double4x4{ {c0},{c1},{c2},{c3} } from CreateDefaultLut */
    m->coefMatrix.c0 = hl_f3_make(1.16438356164f, 1.16438356164f, 1.16438356164f);
    m->coefMatrix.c1 = hl_f3_make(-4.43119136738e-17f, -0.387076923077f, 2.02178571429f);
    m->coefMatrix.c2 = hl_f3_make(1.58691964286f, -0.821942994505f, -1.77247654695e-16f);
    m->coefMatrix.c3 = hl_f3_make(-0.869630789302f, 0.53382122535f, -1.08791650359f);
    m->coloredAspectMode = 3;
    m->colorAspectFirst  = hl_f3_make(1.08301468119f, 1.00959155269f, 1.10863646026f);
    m->colorAspectSecond = hl_f3_make(1.06441799482f, 1.00700595321f, 1.06831049137f);
    m->aspect = hl_f3_make(0.9925f, 1.f, 1.035f);
    m->bt2020Range = 0;
    m->altConvert = 1;
    m->altPrimariesToSrgb.c0 = hl_f3_make(1.62995144161f, -0.159968936426f, -0.0191389994477f);
    m->altPrimariesToSrgb.c1 = hl_f3_make(-0.556261837808f, 1.17281602107f, -0.104271698501f);
    m->altPrimariesToSrgb.c2 = hl_f3_make(-0.0736896038051f, -0.0128470846442f, 1.12341069795f);
    m->gamma = HL_GAMMA_PQ;
    m->gammaHLG = 0.f;
    m->lchEnabled = false;
    m->nits = 229.625687f;
    m->signal.range = HL_RANGE_LIMITED;
    m->signal.yRange = 0.858824f;
    m->signal.upYLimit = 0.572549f;
    m->signal.downYLimit = 0.062745f;
    m->signal.yShift = 0.062745f;
    m->signal.isSourceP010 = false;
}

/* --- LCH correction (doToneMapping) ------------------------------------ */

/* Find the bracketing pair in a hue-sorted (descending) list and interpolate. */
static void lch_lookup(const hl_f4 *list, int n, float hue, hl_f3 *corr)
{
    corr->x = corr->y = corr->z = 0.f;
    if (n < 2) return;
    hl_f4 last = list[0];
    for (int i = 1; i < n; last = list[i], i++) {
        const hl_f4 cur = list[i];
        if (last.w >= hue && hue >= cur.w) {
            float lastAsp = last.w - hue;
            float curAsp = hue - cur.w;
            float prop = 1.f - (lastAsp / (lastAsp + curAsp));
            if (last.x > 0.f && cur.x > 0.f) corr->x = prop * last.x + (1.f - prop) * cur.x;
            if (last.y > 0.f && cur.y > 0.f) corr->y = prop * last.y + (1.f - prop) * cur.y;
            corr->z += prop * last.z + (1.f - prop) * cur.z;
            break;
        }
    }
}

static void do_tone_mapping(const hl_lch_lists *m, hl_f3 *p)
{
    hl_f3 a = hl_xyz_to_lch(hl_f3_scale(hl_srgb_to_xyz(*p), 100.f));
    if (a.y < 0.1f) return;

    hl_f3 cHigh, cMid, cLow;
    lch_lookup(m->high, m->n_high, a.z, &cHigh);
    lch_lookup(m->mid,  m->n_mid,  a.z, &cMid);
    lch_lookup(m->low,  m->n_low,  a.z, &cLow);

    hl_f3 aHigh = a;
    if (cHigh.x > 0.f) aHigh.x *= cHigh.x;
    if (cHigh.y > 0.f) aHigh.y *= cHigh.y;
    aHigh.z += cHigh.z;
    hl_f3 pHigh = hl_xyz_to_srgb(hl_f3_scale(hl_lch_to_xyz(aHigh), 1.f / 100.f));

    hl_f3 aLow = a;
    if (cLow.x > 0.f) aLow.x *= cLow.x;
    if (cLow.y > 0.f) aLow.y *= cLow.y;
    aLow.z += cLow.z;
    hl_f3 pLow = hl_xyz_to_srgb(hl_f3_scale(hl_lch_to_xyz(aLow), 1.f / 100.f));

    float mx = hl_f3_maxelem(pLow); float mh = hl_f3_maxelem(pHigh); if (mh > mx) mx = mh;

    if (128.0f / 255.0f >= mx) {
        if (cLow.x > 0.f) a.x *= cLow.x;
        if (cLow.y > 0.f) a.y *= cLow.y;
        a.z += cLow.z;
    } else if (192.0f / 255.0f >= mx) {
        float lenLow = fabsf((128.0f / 255.0f) - mx);
        float lenMid = fabsf((192.0f / 255.0f) - mx);
        float aspectMid = (1.0f - lenMid / (lenLow + lenMid));
        if (cLow.x > 0.f && cMid.x > 0.f) a.x *= cMid.x * aspectMid + cLow.x * (1.f - aspectMid);
        if (cLow.y > 0.f && cMid.y > 0.f) a.y *= cMid.y * aspectMid + cLow.y * (1.f - aspectMid);
        a.z += cMid.z * aspectMid + cLow.z * (1.f - aspectMid);
    } else {
        float lenMid = fabsf((192.0f / 255.0f) - mx);
        float lenHigh = fabsf(1.0f - mx);
        float aspectHigh = (1.0f - lenHigh / (lenMid + lenHigh));
        if (cMid.x > 0.f && cHigh.x > 0.f) a.x *= cHigh.x * aspectHigh + cMid.x * (1.f - aspectHigh);
        if (cMid.y > 0.f && cHigh.y > 0.f) a.y *= cHigh.y * aspectHigh + cMid.y * (1.f - aspectHigh);
        a.z += cHigh.z * aspectHigh + cMid.z * (1.f - aspectHigh);
    }
    *p = hl_xyz_to_srgb(hl_f3_scale(hl_lch_to_xyz(a), 1.f / 100.f));
}

/* --- hdr_to_srgb --------------------------------------------------------- */

hl_f3 hl_hdr_to_srgb(const hl_model *m, hl_f3 yuv, uint8_t u8, uint8_t v8)
{
    hl_f3 srgb = hl_f3_make(0, 0, 0);
    bool white = true;

    /* P010 unpack is not ported: our source is 8-bit MJPEG. Treated as PQ. */

    if (m->gamma == HL_GAMMA_SRGB || m->gamma == HL_GAMMA_BT2020_IN_SRGB) {
        /* CapturedColors::correctYRange */
        yuv.x = ((yuv.x - m->signal.downYLimit) / (m->signal.upYLimit - m->signal.downYLimit)) * m->signal.yRange + m->signal.yShift;
    }

    if (m->signal.range == HL_RANGE_LIMITED) {
        const float low = 16.0f / 255.0f;
        yuv.x = (yuv.x < low) ? low : (yuv.x - low) * m->aspect.x + low;
    } else {
        yuv.x *= m->aspect.x;
    }

    if (u8 != v8 || u8 != 128) {
        const float mid = 128.0f / 256.0f;
        yuv.y = (yuv.y - mid) * m->aspect.y + mid;
        yuv.z = (yuv.z - mid) * m->aspect.z + mid;
        white = false;
    }

    hl_f3 a = hl_m34_mul(&m->coefMatrix, yuv);
    hl_f3 e = hl_f3_make(0, 0, 0);

    switch (m->gamma) {
    case HL_GAMMA_PQ:
    case HL_GAMMA_P010:
        e = hl_pq_st2084_3(10000.0f / m->nits, a);
        break;
    case HL_GAMMA_HLG:
        e = hl_f3_scale(hl_hlg_ootf(hl_hlg_inverse_oetf3(a), m->gammaHLG), m->nits);
        break;
    case HL_GAMMA_SRGB:
        srgb = a;
        break;
    case HL_GAMMA_BT2020_IN_SRGB:
        e = hl_srgb_to_linear3(a);
        break;
    case HL_GAMMA_PQ_IN_SRGB:
        a = hl_linear_to_srgb3(a);
        e = hl_pq_st2084_3(10000.0f / m->nits, a);
        break;
    }

    if (m->gamma != HL_GAMMA_SRGB) {
        if (m->altConvert) srgb = hl_m3_mul(&m->altPrimariesToSrgb, e);
        else               srgb = hl_bt2020_to_bt709(e);
        srgb = hl_linear_to_srgb3(srgb);
    }

    if (m->bt2020Range) {
        srgb = hl_bt2020_to_linear3(srgb);
        srgb = hl_linear_to_srgb3(srgb);
    }

    if (m->coloredAspectMode) {
        const hl_f3 f = m->colorAspectFirst, s = m->colorAspectSecond;
        if (m->coloredAspectMode == 1) {
            if (!white) srgb = hl_f3_mul(srgb, f);
            else        srgb = hl_f3_scale(srgb, (f.x + f.y + f.z) / 3.0f);
        } else if (m->coloredAspectMode == 2) {
            if (!white) srgb = hl_f3_mul(srgb, hl_f3_scale(hl_f3_add(f, s), 0.5f));
            else        srgb = hl_f3_scale(srgb, (f.x + f.y + f.z + s.x + s.y + s.z) / 6.0f);
        } else {
            if (!white) srgb = hl_f3_mul(srgb, s);
            else        srgb = hl_f3_scale(srgb, (s.x + s.y + s.z) / 3.0f);
        }
    }

    /* Upstream calls clamp01() here but discards its return value, so the
     * LCH correction sees the unclamped value. The LUT worker clamps when it
     * rounds to bytes; hl_f3_clamp01 below is that step. */
    if (m->lchEnabled) do_tone_mapping(&m->lch, &srgb);

    return hl_f3_clamp01(srgb);
}

static inline uint8_t to_u8(float v)
{
    int i = (int)lroundf(v * 255.0f);
    return (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i));
}

hl_f3 hl_tonemap_yuv8(const hl_model *m, uint8_t y, uint8_t u, uint8_t v)
{
    hl_f3 yuv = hl_f3_make(y / 255.0f, u / 255.0f, v / 255.0f);
    return hl_hdr_to_srgb(m, yuv, u, v);
}

hl_f3 hl_tonemap_rgb8(const hl_model *m, uint8_t r, uint8_t g, uint8_t b)
{
    /* CreateLutWorker phase 0: RGB -> YUV with the model's range/coef, then the same path. */
    hl_f3 rgb = hl_f3_make(r / 255.0f, g / 255.0f, b / 255.0f);
    hl_f3 yuv = hl_rgb_to_yuv(m->signal.range, m->coef, rgb);
    return hl_hdr_to_srgb(m, yuv, to_u8(yuv.y), to_u8(yuv.z));
}

void hl_tonemap_build_lut_rgb(const hl_model *m, int n, uint8_t *out)
{
    hl_tonemap_build_lut_rgb_slice(m, n, 0, n, out);
}

void hl_tonemap_build_lut_rgb_slice(const hl_model *m, int n, int r0, int r1, uint8_t *out)
{
    for (int r = r0; r < r1; r++)
        for (int g = 0; g < n; g++)
            for (int b = 0; b < n; b++) {
                uint8_t R = (uint8_t)((r * 255) / (n - 1));
                uint8_t G = (uint8_t)((g * 255) / (n - 1));
                uint8_t B = (uint8_t)((b * 255) / (n - 1));
                hl_f3 s = hl_tonemap_rgb8(m, R, G, B);
                uint8_t *o = out + 3 * (((size_t)r * n + g) * n + b);
                o[0] = to_u8(s.x); o[1] = to_u8(s.y); o[2] = to_u8(s.z);
            }
}

void hl_tonemap_lut_rgb8(const uint8_t *lut, int n, uint8_t r, uint8_t g, uint8_t b, uint8_t out[3])
{
    int ri = (r * (n - 1) + 127) / 255;
    int gi = (g * (n - 1) + 127) / 255;
    int bi = (b * (n - 1) + 127) / 255;
    const uint8_t *o = lut + 3 * (((size_t)ri * n + gi) * n + bi);
    out[0] = o[0]; out[1] = o[1]; out[2] = o[2];
}
