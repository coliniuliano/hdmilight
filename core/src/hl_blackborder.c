#include "hl_blackborder.h"
#include <string.h>
#include <math.h>

void hl_blackborder_init(hl_blackborder *b, bool enabled, float threshold_percent)
{
    memset(b, 0, sizeof(*b));
    b->enabled = enabled;
    int t = (int)ceilf((threshold_percent / 100.0f) * 255.0f);
    b->threshold = (uint8_t)(t < 0 ? 0 : (t > 255 ? 255 : t));
    b->unknownSwitchCnt = 600;
    b->borderSwitchCnt = 50;
    b->maxInconsistentCnt = 10;
    b->blurRemoveCnt = 1;
    b->mode = HL_BB_DEFAULT;
    b->current.unknown = true;
    b->previous.unknown = true;
}

static inline bool is_black(const hl_blackborder *b, const uint8_t *rgb, int stride, int x, int y)
{
    const uint8_t *p = rgb + (size_t)y * stride + (size_t)x * 3;
    return p[0] < b->threshold && p[1] < b->threshold && p[2] < b->threshold;
}

static bool border_eq(const hl_border *a, const hl_border *o)
{
    if (a->unknown) return o->unknown;
    return !o->unknown && a->horizontalSize == o->horizontalSize && a->verticalSize == o->verticalSize;
}

static hl_border detect_default(const hl_blackborder *b, const uint8_t *rgb, int width, int height, int stride)
{
    int w33 = width / 3, h33 = height / 3, w66 = w33 * 2, h66 = h33 * 2;
    int xc = width / 2, yc = height / 2;
    int fx = -1, fy = -1;
    int W = width - 1, H = height - 1;
    for (int x = 0; x < w33; ++x)
        if (!is_black(b, rgb, stride, W - x, yc) || !is_black(b, rgb, stride, x, h33) || !is_black(b, rgb, stride, x, h66)) { fx = x; break; }
    for (int y = 0; y < h33; ++y)
        if (!is_black(b, rgb, stride, xc, H - y) || !is_black(b, rgb, stride, w33, y) || !is_black(b, rgb, stride, w66, y)) { fy = y; break; }
    hl_border r;
    r.unknown = (fx == -1 || fy == -1);
    r.horizontalSize = fy;
    r.verticalSize = fx;
    return r;
}

static hl_border detect_letterbox(const hl_blackborder *b, const uint8_t *rgb, int width, int height, int stride)
{
    int w25 = width / 4, h33 = height / 3, w75 = w25 * 3, xc = width / 2;
    int fy = -1, H = height - 1;
    for (int y = 0; y < h33; ++y)
        if (!is_black(b, rgb, stride, xc, y) || !is_black(b, rgb, stride, w25, y) || !is_black(b, rgb, stride, w75, y) ||
            !is_black(b, rgb, stride, w25, H - y) || !is_black(b, rgb, stride, w75, H - y)) { fy = y; break; }
    hl_border r;
    r.unknown = (fy == -1);
    r.horizontalSize = fy;
    r.verticalSize = 0;
    return r;
}

static bool update_border(hl_blackborder *b, hl_border n)
{
    if (border_eq(&n, &b->previous)) {
        ++b->consistentCnt;
        b->inconsistentCnt = 0;
    } else {
        ++b->inconsistentCnt;
        if (b->inconsistentCnt <= b->maxInconsistentCnt) return false;
        b->previous = n;
        b->consistentCnt = 0;
    }
    if (border_eq(&b->current, &n)) { b->inconsistentCnt = 0; return false; }
    bool changed = false;
    if (n.unknown) {
        if (b->consistentCnt == b->unknownSwitchCnt) { b->current = n; changed = true; }
    } else {
        if (b->current.unknown || b->consistentCnt == b->borderSwitchCnt) { b->current = n; changed = true; }
    }
    return changed;
}

bool hl_blackborder_process_rgb8(hl_blackborder *b, const uint8_t *rgb, int width, int height, int stride_bytes)
{
    hl_border img = { true, 0, 0 };
    if (!b->enabled) { b->current = img; return true; }
    img = (b->mode == HL_BB_LETTERBOX) ? detect_letterbox(b, rgb, width, height, stride_bytes)
                                       : detect_default(b, rgb, width, height, stride_bytes);
    if (img.horizontalSize > 0) img.horizontalSize += (int)b->blurRemoveCnt;
    if (img.verticalSize > 0)   img.verticalSize   += (int)b->blurRemoveCnt;
    return update_border(b, img);
}
