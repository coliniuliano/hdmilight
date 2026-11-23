/* LED zone mapping and linear-light averaging, ported from HyperHDR v22
 * base/ImageColorAveraging.cpp (MIT). Each LED owns a rectangle in normalised
 * image coordinates (hmin/hmax/vmin/vmax, 0..1). */
#pragma once
#include "hl_types.h"

typedef struct {
    float hmin, hmax, vmin, vmax;   /* fractions of the (border-cropped) frame */
    int   group;                    /* >0: LEDs sharing a group get the master's colour */
    bool  disabled;
} hl_led;

typedef struct {
    int       width, height;        /* frame size the index lists were built for */
    int       hborder, vborder;     /* black-border crop applied at build time */
    int       n_leds;
    uint32_t *idx;                  /* concatenated pixel indices (pixel number, not byte offset) */
    uint32_t *start;                /* n_leds+1 offsets into idx */
    int      *group_master;         /* per LED: index of its group master, or -1 */
} hl_zones;

/* Build index lists. sparse=true samples every second pixel in x and y
 * (HyperHDR forces this above 1600 pixels per LED). Returns 0 on success. */
int  hl_zones_init(hl_zones *z, const hl_led *leds, int n_leds,
                   int width, int height, int hborder, int vborder, bool sparse);
void hl_zones_free(hl_zones *z);

/* Average an 8-bit RGB frame (3 bytes/pixel, row stride in bytes) in linear
 * light. out[n_leds] receives linear RGB 0..1. */
void hl_zones_average_rgb8(const hl_zones *z, const uint8_t *rgb, int stride_bytes, hl_f3 *out);
/* Average a frame of already-linear float colours (one hl_f3 per pixel). */
void hl_zones_average_f3(const hl_zones *z, const hl_f3 *lin, hl_f3 *out);

/* Generate a classic layout: counts per edge, clockwise from top-left, with a
 * given depth (fraction of frame the edge zones reach inward). Returns count. */
int  hl_zones_layout_classic(hl_led *out, int max, int top, int right, int bottom, int left, float depth);
