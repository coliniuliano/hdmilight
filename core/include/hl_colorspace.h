/* Colour science ported from HyperHDR v22 infinite-color-engine/ColorSpace.cpp (MIT).
 * Float instead of double: the ESP32-P4 FPU is single precision. */
#pragma once
#include "hl_types.h"

typedef enum { HL_GAMMA_PQ = 0, HL_GAMMA_HLG, HL_GAMMA_SRGB, HL_GAMMA_BT2020_IN_SRGB, HL_GAMMA_PQ_IN_SRGB, HL_GAMMA_P010 } hl_gamma_t;

float  hl_srgb_to_linear(float v);
float  hl_linear_to_srgb(float v);
hl_f3  hl_srgb_to_linear3(hl_f3 v);
hl_f3  hl_linear_to_srgb3(hl_f3 v);

float  hl_bt2020_to_linear(float v);
float  hl_linear_to_bt2020(float v);
hl_f3  hl_bt2020_to_linear3(hl_f3 v);

/* PQ ST.2084 EOTF; scale is 10000/nits in HyperHDR usage. */
float  hl_pq_st2084(float scale, float nonlinear);
hl_f3  hl_pq_st2084_3(float scale, hl_f3 nonlinear);

float  hl_hlg_inverse_oetf(float t);
hl_f3  hl_hlg_inverse_oetf3(hl_f3 v);
hl_f3  hl_hlg_ootf(hl_f3 v, float gamma);

hl_f3  hl_bt2020_to_bt709(hl_f3 a);     /* linear light, fixed coefficients */
hl_f3  hl_srgb_to_xyz(hl_f3 rgb);        /* linear sRGB -> XYZ */
hl_f3  hl_xyz_to_srgb(hl_f3 xyz);

hl_f3  hl_xyz_to_lab(hl_f3 xyz);
hl_f3  hl_lab_to_xyz(hl_f3 lab);
hl_f3  hl_lab_to_lch(hl_f3 lab);
hl_f3  hl_lch_to_lab(hl_f3 lch);
hl_f3  hl_xyz_to_lch(hl_f3 xyz);
hl_f3  hl_lch_to_xyz(hl_f3 lch);

/* 8-bit sRGB -> 16-bit linear (0..65535), table based, as HyperHDR's
 * InfiniteProcessing::srgbNonlinearToLinear(byte3). */
uint16_t hl_srgb8_to_linear16(uint8_t v);
