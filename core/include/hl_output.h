/* LED output colour processing, ported from HyperHDR v22 infinite-color-engine/InfiniteProcessing.cpp (MIT):
 * temperature, colour calibration, scale, linear->sRGB, user gamma, luminance/saturation, backlight, power limit. */
#pragma once
#include "hl_types.h"

typedef enum { HL_TEMP_OFF = 0, HL_TEMP_COLD, HL_TEMP_NEUTRAL, HL_TEMP_WARM, HL_TEMP_CUSTOM } hl_temp_preset;
enum { HL_CAL_RED = 0, HL_CAL_GREEN, HL_CAL_BLUE, HL_CAL_CYAN, HL_CAL_MAGENTA, HL_CAL_YELLOW, HL_CAL_WHITE, HL_CAL_BLACK, HL_CAL_N };

typedef struct {
    hl_temp_preset temp;
    hl_f3   temp_tint;          /* HL_TEMP_CUSTOM: per-channel multiplier */
    bool    classic;            /* true: red/green/blue targets only (matrix); false: all eight (3D LUT) */
    uint8_t cal[HL_CAL_N][3];   /* what the strip should show for each ideal colour */
    float   scale;              /* 0..2, applied in linear light */
    float   gamma;              /* HyperHDR default 1.5 */
    float   luminance, saturation;
    float   backlight;          /* minimum level 0..1 (0 = off); HyperHDR default 0.0039 */
    bool    backlight_colored;  /* per channel floor, else grey below the threshold */
    float   power_limit;        /* 0..1 of the strip's full-white sum */
} hl_output_cfg;

typedef struct {
    hl_output_cfg cfg;
    int    cal_mode;            /* 0 none, 1 matrix, 2 lut */
    hl_m3  cal_matrix;
    hl_f3  cal_lut[9 * 9 * 9];
    bool   has_tint, has_gamma, has_hsv, has_scale, has_power, has_backlight;
    hl_f3  tint;
    float  gamma_lut[512];
} hl_output;

void  hl_output_cfg_default(hl_output_cfg *c);
void  hl_output_init(hl_output *o, const hl_output_cfg *c);
/* Linear RGB in, nonlinear sRGB out (in place), all LEDs at once (the power limit is global). */
void  hl_output_process(const hl_output *o, hl_f3 *colors, int n);
/* Floor the smoothing output must keep (0 when backlight is off). */
float hl_output_min_backlight(const hl_output *o);
