/* YUV <-> RGB conversion ported from HyperHDR v22 YuvConverter.cpp (MIT).
 * Values are normalised 0..1 (bytes / 255). */
#pragma once
#include "hl_types.h"

typedef enum { HL_YUV_FCC = 0, HL_YUV_BT601 = 1, HL_YUV_BT709 = 2, HL_YUV_BT2020 = 3 } hl_yuv_coef_t;
typedef enum { HL_RANGE_UNKNOWN = 0, HL_RANGE_FULL = 1, HL_RANGE_LIMITED = 2 } hl_range_t;

/* Kr, Kb for a coefficient set. */
void   hl_yuv_coefs(hl_yuv_coef_t coef, float *kr, float *kb);
/* RGB -> YUV affine matrix as HyperHDR builds it (columns c0..c2, offset c3). */
hl_m34 hl_yuv_rgb2yuv_matrix(hl_range_t range, hl_yuv_coef_t coef);
/* Inverse of the above. */
hl_m34 hl_yuv_yuv2rgb_matrix(hl_range_t range, hl_yuv_coef_t coef);

hl_f3  hl_yuv_to_rgb(hl_range_t range, hl_yuv_coef_t coef, hl_f3 yuv);
hl_f3  hl_rgb_to_yuv(hl_range_t range, hl_yuv_coef_t coef, hl_f3 rgb);
