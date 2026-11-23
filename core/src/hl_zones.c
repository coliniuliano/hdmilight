#include "hl_zones.h"
#include "hl_colorspace.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int iround(float v) { return (int)lroundf(v); }   /* qRound: half away from zero */
static int imin(int a, int b) { return a < b ? a : b; }

int hl_zones_init(hl_zones *z, const hl_led *leds, int n_leds,
                  int width, int height, int hborder, int vborder, bool sparse)
{
    memset(z, 0, sizeof(*z));
    if (width <= 2 * vborder || height <= 2 * hborder) return -1;
    z->width = width; z->height = height;
    z->hborder = hborder; z->vborder = vborder;
    z->n_leds = n_leds;

    const int xOffset = vborder, actualWidth = width - 2 * vborder;
    const int yOffset = hborder, actualHeight = height - 2 * hborder;

    /* first pass: count */
    size_t total = 0;
    for (int i = 0; i < n_leds; i++) {
        const hl_led *l = &leds[i];
        if (l->disabled) continue;
        if ((l->hmax - l->hmin) < 1e-6f || (l->vmax - l->vmin) < 1e-6f) continue;
        int minX = xOffset + iround(actualWidth * l->hmin);
        int maxX = xOffset + iround(actualWidth * l->hmax);
        int minY = yOffset + iround(actualHeight * l->vmin);
        int maxY = yOffset + iround(actualHeight * l->vmax);
        minX = imin(minX, xOffset + actualWidth - 1); if (minX == maxX) maxX++;
        minY = imin(minY, yOffset + actualHeight - 1); if (minY == maxY) maxY++;
        int endY = imin(maxY, yOffset + actualHeight), endX = imin(maxX, xOffset + actualWidth);
        int ny = abs(endY - minY), nx = abs(endX - minX);
        bool sp = sparse || ((size_t)ny * nx > 1600);
        int inc = sp ? 2 : 1;
        total += (size_t)((ny + inc - 1) / inc) * ((nx + inc - 1) / inc);
    }

    z->idx = (uint32_t *)malloc(sizeof(uint32_t) * (total ? total : 1));
    z->start = (uint32_t *)malloc(sizeof(uint32_t) * (n_leds + 1));
    z->group_master = (int *)malloc(sizeof(int) * (n_leds ? n_leds : 1));
    if (!z->idx || !z->start || !z->group_master) { hl_zones_free(z); return -2; }

    size_t pos = 0;
    for (int i = 0; i < n_leds; i++) {
        const hl_led *l = &leds[i];
        z->start[i] = (uint32_t)pos;
        z->group_master[i] = -1;
        if (l->group > 0) {
            for (int j = 0; j < i; j++)
                if (leds[j].group == l->group) { z->group_master[i] = (z->group_master[j] >= 0) ? z->group_master[j] : j; break; }
        }
        if (l->disabled) continue;
        if ((l->hmax - l->hmin) < 1e-6f || (l->vmax - l->vmin) < 1e-6f) continue;
        int minX = xOffset + iround(actualWidth * l->hmin);
        int maxX = xOffset + iround(actualWidth * l->hmax);
        int minY = yOffset + iround(actualHeight * l->vmin);
        int maxY = yOffset + iround(actualHeight * l->vmax);
        minX = imin(minX, xOffset + actualWidth - 1); if (minX == maxX) maxX++;
        minY = imin(minY, yOffset + actualHeight - 1); if (minY == maxY) maxY++;
        int endY = imin(maxY, yOffset + actualHeight), endX = imin(maxX, xOffset + actualWidth);
        int ny = abs(endY - minY), nx = abs(endX - minX);
        bool sp = sparse || ((size_t)ny * nx > 1600);
        int inc = sp ? 2 : 1;
        for (int y = minY; y < endY; y += inc)
            for (int x = minX; x < endX; x += inc)
                z->idx[pos++] = (uint32_t)(y * width + x);
    }
    z->start[n_leds] = (uint32_t)pos;
    return 0;
}

void hl_zones_free(hl_zones *z)
{
    free(z->idx); free(z->start); free(z->group_master);
    memset(z, 0, sizeof(*z));
}

void hl_zones_average_rgb8(const hl_zones *z, const uint8_t *rgb, int stride_bytes, hl_f3 *out)
{
    const int w = z->width;
    for (int i = 0; i < z->n_leds; i++) {
        uint32_t s = z->start[i], e = z->start[i + 1];
        if (e == s) { out[i] = hl_f3_make(0, 0, 0); continue; }
        uint64_t sr = 0, sg = 0, sb = 0;
        for (uint32_t k = s; k < e; k++) {
            uint32_t p = z->idx[k];
            const uint8_t *px = rgb + (size_t)(p / w) * stride_bytes + (size_t)(p % w) * 3;
            sr += hl_srgb8_to_linear16(px[0]);
            sg += hl_srgb8_to_linear16(px[1]);
            sb += hl_srgb8_to_linear16(px[2]);
        }
        float n = (float)(e - s) * 65535.0f;
        out[i] = hl_f3_make(sr / n, sg / n, sb / n);
    }
    for (int i = 0; i < z->n_leds; i++)
        if (z->group_master[i] >= 0) out[i] = out[z->group_master[i]];
}

void hl_zones_average_f3(const hl_zones *z, const hl_f3 *lin, hl_f3 *out)
{
    for (int i = 0; i < z->n_leds; i++) {
        uint32_t s = z->start[i], e = z->start[i + 1];
        if (e == s) { out[i] = hl_f3_make(0, 0, 0); continue; }
        hl_f3 acc = hl_f3_make(0, 0, 0);
        for (uint32_t k = s; k < e; k++) acc = hl_f3_add(acc, lin[z->idx[k]]);
        out[i] = hl_f3_scale(acc, 1.0f / (float)(e - s));
    }
    for (int i = 0; i < z->n_leds; i++)
        if (z->group_master[i] >= 0) out[i] = out[z->group_master[i]];
}

int hl_zones_layout_classic(hl_led *out, int max, int top, int right, int bottom, int left, float depth)
{
    int n = 0;
    /* top: left -> right */
    for (int i = 0; i < top && n < max; i++, n++) {
        out[n] = (hl_led){ (float)i / top, (float)(i + 1) / top, 0.f, depth, 0, false };
    }
    /* right: top -> bottom */
    for (int i = 0; i < right && n < max; i++, n++) {
        out[n] = (hl_led){ 1.f - depth, 1.f, (float)i / right, (float)(i + 1) / right, 0, false };
    }
    /* bottom: right -> left */
    for (int i = 0; i < bottom && n < max; i++, n++) {
        out[n] = (hl_led){ 1.f - (float)(i + 1) / bottom, 1.f - (float)i / bottom, 1.f - depth, 1.f, 0, false };
    }
    /* left: bottom -> top */
    for (int i = 0; i < left && n < max; i++, n++) {
        out[n] = (hl_led){ 0.f, depth, 1.f - (float)(i + 1) / left, 1.f - (float)i / left, 0, false };
    }
    return n;
}
