/* Frame -> LED colours. Ties the ported HyperHDR stages together:
 *   RGB888 frame -> block-average to a small grid -> (auto)HDR tone map per
 *   cell -> linear light -> zone averages -> output processing (sRGB, gamma, calibration) -> smoothing.
 *
 * Deviation from HyperHDR: HyperHDR tone-maps every source pixel and averages
 * the results; we average 8-bit RGB inside each grid cell first, then tone-map
 * the cell. That keeps the pow() count at a few thousand per frame. */
#pragma once
#include "hl_types.h"
#include "hl_tonemap.h"
#include "hl_zones.h"
#include "hl_autohdr.h"
#include "hl_blackborder.h"
#include "hl_smooth.h"
#include "hl_output.h"

typedef enum { HL_HDR_AUTO = 0, HL_HDR_ON, HL_HDR_OFF } hl_hdr_mode;

typedef struct {
    int   block;             /* grid cell size in source pixels (8 -> 80x60 from 640x480) */
    hl_hdr_mode hdr_mode;
    bool  blackborder;
    bool  sparse;            /* HyperHDR "sparse processing" for zone sampling */
    int   lut_n;             /* 0 = evaluate the model per cell; else bake an n^3 LUT */
    void (*yield)(void);     /* optional; called between LUT slices so an RTOS idle task can run */
    bool  lut_defer;         /* don't bake the LUT in init; the caller bakes it later with hl_pipeline_build_lut/_set_lut */
    bool  edge_only;         /* only average the cells the zones and the border detector read */
    hl_output_cfg out;       /* LED colour processing */
} hl_pipeline_cfg;

typedef struct {
    hl_pipeline_cfg cfg;
    hl_model   model;
    hl_led    *leds; int n_leds;
    hl_zones   zones;
    hl_autohdr autohdr;
    hl_blackborder bb;
    hl_smooth  smooth;
    hl_output  out;
    /* scratch */
    int        gw, gh;       /* grid size */
    uint8_t   *grid_rgb;     /* gw*gh*3 averaged 8-bit RGB */
    uint8_t   *grid_yuv;     /* gw*gh*3 averaged 8-bit YUV when the source was YUV (else unused) */
    bool       grid_has_yuv;
    uint8_t   *cell_mask;    /* gw*gh, 1 = computed; NULL = all cells */
    int        cells_used;
    hl_f3     *grid_lin;     /* gw*gh linear colours after tone map */
    hl_f3     *led_lin;      /* n_leds */
    hl_f3     *led_srgb;     /* n_leds targets handed to smoothing */
    hl_f3     *held;         /* n_leds last values sent (anti-flicker hold) */
    uint8_t   *lut;          /* optional baked tone-map LUT, owned (malloc) */
    const uint8_t *lut_static; /* or a read-only one (e.g. in flash), not freed */
    bool       hdr_active;
    uint32_t   frames;
} hl_pipeline;

void hl_pipeline_cfg_default(hl_pipeline_cfg *c);
/* leds are copied. Returns 0 on success. */
int  hl_pipeline_init(hl_pipeline *p, const hl_pipeline_cfg *cfg, const hl_model *model,
                      const hl_led *leds, int n_leds, int src_width, int src_height);
void hl_pipeline_free(hl_pipeline *p);
/* Bake an n^3 tone-map LUT for a model without touching a pipeline (slow: ~6 s for n=33 on an ESP32-P4).
 * Returns a malloc'd buffer of n*n*n*3 bytes, or NULL. */
uint8_t *hl_pipeline_build_lut(const hl_model *model, int n, void (*yield)(void));
/* Install (or clear with NULL) a baked LUT; takes ownership and frees any previous one. Until a LUT is
 * installed, HDR frames are tone-mapped per cell directly from the model. */
void hl_pipeline_set_lut(hl_pipeline *p, uint8_t *lut, int n);
/* Use a read-only LUT the caller keeps alive (e.g. a flash-embedded table baked at build time). */
void hl_pipeline_set_lut_static(hl_pipeline *p, const uint8_t *lut, int n);
/* Feed one decoded frame (8-bit RGB, 3 bytes/pixel). Updates smoothing targets. */
void hl_pipeline_frame_rgb8(hl_pipeline *p, const uint8_t *rgb, int width, int height, int stride_bytes, int64_t now_ms);
/* Same for the ESP32-P4 JPEG decoder's packed 4:2:0 output: 1.5 bytes/pixel, every line is [C Y Y] triplets, the
 * chroma byte being U on even-indexed lines and V on odd-indexed lines (one U and one V per 2x2 block). Full-range
 * BT.601 (JFIF). stride_bytes = width * 3 / 2. Half the memory traffic of RGB888 and one YUV->RGB per cell. */
void hl_pipeline_frame_yuv420p4(hl_pipeline *p, const uint8_t *buf, int width, int height, int stride_bytes, int64_t now_ms);
/* Replace the output colour processing at runtime. */
void hl_pipeline_set_output(hl_pipeline *p, const hl_output_cfg *c);
/* Advance smoothing and fetch 8-bit output. Call at the LED refresh rate. A LED only changes when its colour
 * moved by more than half an 8-bit step (HyperHDR's anti-flicker filter). */
void hl_pipeline_output(hl_pipeline *p, int64_t now_ms, uint8_t *rgb_out /* n_leds*3 */);
