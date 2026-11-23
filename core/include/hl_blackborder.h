/* Black border detection, ported from HyperHDR v22 blackborder/ (MIT):
 * "default" 3-line detector plus the hysteresis processor. */
#pragma once
#include "hl_types.h"

typedef struct {
    bool unknown;
    int  horizontalSize;   /* rows of border at top/bottom */
    int  verticalSize;     /* columns of border at left/right */
} hl_border;

typedef enum { HL_BB_DEFAULT = 0, HL_BB_LETTERBOX } hl_bb_mode;

typedef struct {
    bool enabled;
    uint8_t threshold;          /* 8-bit; HyperHDR default 5% -> 13 */
    unsigned unknownSwitchCnt;  /* 600 */
    unsigned borderSwitchCnt;   /* 50 */
    unsigned maxInconsistentCnt;/* 10 */
    unsigned blurRemoveCnt;     /* 1 */
    hl_bb_mode mode;
    /* state */
    hl_border current, previous;
    unsigned consistentCnt, inconsistentCnt;
} hl_blackborder;

void hl_blackborder_init(hl_blackborder *b, bool enabled, float threshold_percent);
/* Run on an 8-bit RGB frame. Returns true when the applied border changed. */
bool hl_blackborder_process_rgb8(hl_blackborder *b, const uint8_t *rgb, int width, int height, int stride_bytes);
