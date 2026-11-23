/* HDR -> sRGB tone mapping. Port of the per-entry function HyperHDR v22 uses to
 * generate its 256^3 LUT (LutCalibrator.cpp: hdr_to_srgb, doToneMapping,
 * CreateLutWorker::run), driven by the calibrator's BestResult model instead
 * of a 50 MB table.
 *
 * Upstream: awawa-dev/HyperHDR v22.0.0.0, MIT; the notice is in core/LICENSE.HyperHDR. tools/refcheck compiles
 * HyperHDR's own version of this arithmetic and diffs it against this port. */
#pragma once
#include "hl_types.h"
#include "hl_yuv.h"
#include "hl_colorspace.h"

#define HL_LCH_MAX 32

typedef struct { float x, y, z, w; } hl_f4;   /* LCH correction entry: L scale, C scale, hue delta, hue key */

typedef struct {
    hl_f4 low[HL_LCH_MAX];  int n_low;
    hl_f4 mid[HL_LCH_MAX];  int n_mid;
    hl_f4 high[HL_LCH_MAX]; int n_high;
} hl_lch_lists;

typedef struct {
    hl_range_t range;        /* signal.range */
    float yRange, upYLimit, downYLimit, yShift;
    bool  isSourceP010;
} hl_signal;

/* Mirror of HyperHDR BestResult (include/lut-calibrator/BestResult.h). */
typedef struct {
    hl_yuv_coef_t coef;
    hl_m34   coefMatrix;          /* YUV(0..1) -> RGB, columns as serialized double4x4 */
    int      coloredAspectMode;   /* 0..3 */
    hl_f3    colorAspectFirst, colorAspectSecond;
    hl_f3    aspect;
    int      bt2020Range;
    int      altConvert;
    hl_m3    altPrimariesToSrgb;  /* columns as serialized double3x3 */
    hl_gamma_t gamma;
    float    gammaHLG;
    float    nits;
    bool     lchEnabled;
    hl_lch_lists lch;
    hl_signal signal;
} hl_model;

/* HyperHDR's built-in default model (LutCalibrator::CreateDefaultLut). */
void  hl_model_default(hl_model *m);

/* yuv normalised 0..1; UV bytes used for the "white" test exactly as HyperHDR does. */
hl_f3 hl_hdr_to_srgb(const hl_model *m, hl_f3 yuv, uint8_t u8, uint8_t v8);

/* LUT "phase 1": input is YUV bytes from the grabber (HDR, YUV formats). */
hl_f3 hl_tonemap_yuv8(const hl_model *m, uint8_t y, uint8_t u, uint8_t v);
/* LUT "phase 0": input is RGB bytes (HDR, MJPEG/RGB formats). This is the path
 * for a hardware-decoded MJPEG frame. */
hl_f3 hl_tonemap_rgb8(const hl_model *m, uint8_t r, uint8_t g, uint8_t b);

/* Optional coarse LUT (n^3 entries, nearest) for RGB8 input, to take the pow()
 * calls off the per-pixel path. out must hold n*n*n*3 bytes. */
void  hl_tonemap_build_lut_rgb(const hl_model *m, int n, uint8_t *out);
/* Same, for red indices [r0, r1) only, so callers can yield between slices. */
void  hl_tonemap_build_lut_rgb_slice(const hl_model *m, int n, int r0, int r1, uint8_t *out);
void  hl_tonemap_lut_rgb8(const uint8_t *lut, int n, uint8_t r, uint8_t g, uint8_t b, uint8_t out[3]);
