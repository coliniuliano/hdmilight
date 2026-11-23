#include "hl_pipeline.h"
#include "hl_colorspace.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

void hl_pipeline_cfg_default(hl_pipeline_cfg *c)
{
    memset(c, 0, sizeof(*c));
    c->block = 8;
    c->hdr_mode = HL_HDR_AUTO;
    c->blackborder = true;
    c->sparse = false;
    hl_output_cfg_default(&c->out);
    c->lut_n = 0;
}

static int rebuild_zones(hl_pipeline *p, int hborder, int vborder)
{
    hl_zones_free(&p->zones);
    /* borders are in source pixels; scale to grid cells */
    int hb = hborder / p->cfg.block, vb = vborder / p->cfg.block;
    return hl_zones_init(&p->zones, p->leds, p->n_leds, p->gw, p->gh, hb, vb, p->cfg.sparse);
}

static void rebuild_cell_mask(hl_pipeline *p);

int hl_pipeline_init(hl_pipeline *p, const hl_pipeline_cfg *cfg, const hl_model *model,
                     const hl_led *leds, int n_leds, int src_width, int src_height)
{
    memset(p, 0, sizeof(*p));
    p->cfg = *cfg;
    p->model = *model;
    if (p->cfg.block < 1) p->cfg.block = 1;
    p->gw = src_width / p->cfg.block;
    p->gh = src_height / p->cfg.block;
    if (p->gw < 4 || p->gh < 4) return -1;
    p->n_leds = n_leds;
    p->leds = (hl_led *)malloc(sizeof(hl_led) * n_leds);
    p->grid_rgb = (uint8_t *)calloc(1, (size_t)p->gw * p->gh * 3);
    p->grid_yuv = (uint8_t *)calloc(1, (size_t)p->gw * p->gh * 3);
    p->grid_lin = (hl_f3 *)malloc(sizeof(hl_f3) * (size_t)p->gw * p->gh);
    p->led_lin = (hl_f3 *)calloc(n_leds, sizeof(hl_f3));
    p->led_srgb = (hl_f3 *)calloc(n_leds, sizeof(hl_f3));
    p->held = (hl_f3 *)calloc(n_leds, sizeof(hl_f3));
    if (!p->leds || !p->grid_rgb || !p->grid_lin || !p->led_lin || !p->led_srgb || !p->held) { hl_pipeline_free(p); return -2; }
    memcpy(p->leds, leds, sizeof(hl_led) * n_leds);
    if (rebuild_zones(p, 0, 0) != 0) { hl_pipeline_free(p); return -3; }
    hl_autohdr_init(&p->autohdr, p->cfg.hdr_mode == HL_HDR_AUTO);
    hl_blackborder_init(&p->bb, p->cfg.blackborder, 5.0f);
    if (hl_smooth_init(&p->smooth, n_leds) != 0) { hl_pipeline_free(p); return -4; }
    hl_output_init(&p->out, &p->cfg.out);
    if (p->cfg.lut_n >= 2 && !p->cfg.lut_defer) {
        p->lut = hl_pipeline_build_lut(&p->model, p->cfg.lut_n, p->cfg.yield);
        if (!p->lut) { hl_pipeline_free(p); return -5; }
    }
    p->hdr_active = (p->cfg.hdr_mode == HL_HDR_ON);
    rebuild_cell_mask(p);
    return 0;
}

uint8_t *hl_pipeline_build_lut(const hl_model *model, int n, void (*yield)(void))
{
    if (n < 2) return NULL;
    uint8_t *lut = (uint8_t *)malloc((size_t)n * n * n * 3);
    if (!lut) return NULL;
    for (int r = 0; r < n; r++) {
        hl_tonemap_build_lut_rgb_slice(model, n, r, r + 1, lut);
        if (yield) yield();
    }
    return lut;
}

void hl_pipeline_set_lut(hl_pipeline *p, uint8_t *lut, int n)
{
    free(p->lut);
    p->lut = lut; p->lut_static = NULL;
    p->cfg.lut_n = lut ? n : 0;
}

void hl_pipeline_set_lut_static(hl_pipeline *p, const uint8_t *lut, int n)
{
    free(p->lut); p->lut = NULL;
    p->lut_static = lut;
    p->cfg.lut_n = lut ? n : 0;
}

void hl_pipeline_free(hl_pipeline *p)
{
    hl_zones_free(&p->zones);
    hl_smooth_free(&p->smooth);
    free(p->leds); free(p->grid_rgb); free(p->grid_yuv); free(p->cell_mask); free(p->grid_lin); free(p->led_lin); free(p->led_srgb); free(p->held); free(p->lut);
    memset(p, 0, sizeof(*p));
}

/* Cells the LED zones read, the black-border detector's scan lines (rows/columns at 1/3, 1/2, 2/3, a third of the
 * way in; letterbox mode: columns at 1/4, 1/2, 3/4) and, for automatic HDR detection, every 4th cell of the
 * interior so a bright picture centre is still seen. Rebuilt with the zones. */
static void rebuild_cell_mask(hl_pipeline *p)
{
    if (!p->cfg.edge_only) { free(p->cell_mask); p->cell_mask = NULL; p->cells_used = p->gw * p->gh; return; }
    const int gw = p->gw, gh = p->gh, cells = gw * gh;
    if (!p->cell_mask) p->cell_mask = (uint8_t *)malloc((size_t)cells);
    if (!p->cell_mask) { p->cells_used = cells; return; }
    memset(p->cell_mask, 0, (size_t)cells);
    const hl_zones *z = &p->zones;
    for (int i = 0; i < z->n_leds; i++)
        for (uint32_t k = z->start[i]; k < z->start[i + 1]; k++)
            if (z->idx[k] < (uint32_t)cells) p->cell_mask[z->idx[k]] = 1;
    if (p->cfg.blackborder) {
        const int w33 = gw / 3, h33 = gh / 3, xc = gw / 2, yc = gh / 2, w25 = gw / 4;
        const bool lb = p->bb.mode == HL_BB_LETTERBOX;
        const int rows[3] = { h33, yc, h33 * 2 }, cols[3] = { lb ? w25 : w33, xc, lb ? w25 * 3 : w33 * 2 };
        for (int k = 0; k < 3; k++) {
            if (!lb) for (int x = 0; x < gw; x++) { if (x < w33 || x >= gw - w33) p->cell_mask[rows[k] * gw + x] = 1; }
            for (int y = 0; y < gh; y++) { if (y < h33 || y >= gh - h33) p->cell_mask[y * gw + cols[k]] = 1; }
        }
    }
    if (p->cfg.hdr_mode == HL_HDR_AUTO)
        for (int y = 2; y < gh; y += 4) for (int x = 2; x < gw; x += 4) p->cell_mask[y * gw + x] = 1;
    int n = 0;
    for (int i = 0; i < cells; i++) n += p->cell_mask[i];
    p->cells_used = n;
}

/* Box-average the source into the grid (integer, gamma-encoded RGB). */
static void downsample(hl_pipeline *p, const uint8_t *rgb, int width, int height, int stride)
{
    const int B = p->cfg.block;
    const int gw = p->gw, gh = p->gh;
    (void)height;
    for (int gy = 0; gy < gh; gy++) {
        for (int gx = 0; gx < gw; gx++) {
            if (p->cell_mask && !p->cell_mask[gy * gw + gx]) continue;
            uint32_t sr = 0, sg = 0, sb = 0;
            for (int y = 0; y < B; y++) {
                const uint8_t *row = rgb + (size_t)(gy * B + y) * stride + (size_t)gx * B * 3;
                for (int x = 0; x < B; x++) { sr += row[0]; sg += row[1]; sb += row[2]; row += 3; }
            }
            uint32_t n = (uint32_t)B * B;
            uint8_t *o = p->grid_rgb + ((size_t)gy * gw + gx) * 3;
            o[0] = (uint8_t)((sr + n / 2) / n);
            o[1] = (uint8_t)((sg + n / 2) / n);
            o[2] = (uint8_t)((sb + n / 2) / n);
        }
    }
    (void)width;
}

/* Box-average the ESP32-P4 decoder's packed 4:2:0 layout into the grid: Y from every byte pair, U from the
 * chroma byte of even-indexed lines, V from odd-indexed lines. Produces the YUV grid and, through full-range
 * BT.601 (JFIF), the RGB grid the rest of the pipeline works on. (A row-major 32-bit variant measured slower on the
 * ESP32-P4 than this plain loop; the frame read itself is about half of the cost.) */
static void downsample_yuv420p4(hl_pipeline *p, const uint8_t *buf, int stride)
{
    const int B = p->cfg.block, half = B / 2;
    const int gw = p->gw, gh = p->gh;
    const uint32_t ny = (uint32_t)B * B, nc = (uint32_t)half * half;
    for (int gy = 0; gy < gh; gy++) {
        for (int gx = 0; gx < gw; gx++) {
            if (p->cell_mask && !p->cell_mask[gy * gw + gx]) continue;
            uint32_t sy = 0, su = 0, sv = 0;
            for (int y = 0; y < B; y++) {
                const uint8_t *row = buf + (size_t)(gy * B + y) * stride + (size_t)gx * B * 3 / 2;
                uint32_t sc = 0;
                for (int t = 0; t < half; t++) { sc += row[0]; sy += row[1] + row[2]; row += 3; }
                if (y & 1) sv += sc; else su += sc;
            }
            const int Y = (int)((sy + ny / 2) / ny), U = (int)((su + nc / 2) / nc), V = (int)((sv + nc / 2) / nc);
            uint8_t *q = p->grid_yuv + ((size_t)gy * gw + gx) * 3;
            q[0] = (uint8_t)Y; q[1] = (uint8_t)U; q[2] = (uint8_t)V;
            const float fy = (float)Y, fu = (float)U - 128.f, fv = (float)V - 128.f;
            uint8_t *o = p->grid_rgb + ((size_t)gy * gw + gx) * 3;
            o[0] = (uint8_t)hl_clampf(fy + 1.402f * fv + 0.5f, 0, 255);
            o[1] = (uint8_t)hl_clampf(fy - 0.344136f * fu - 0.714136f * fv + 0.5f, 0, 255);
            o[2] = (uint8_t)hl_clampf(fy + 1.772f * fu + 0.5f, 0, 255);
        }
    }
}

static bool grid_detect(hl_pipeline *p, int64_t now_ms);
static void grid_finish(hl_pipeline *p, int64_t now_ms);

void hl_pipeline_frame_rgb8(hl_pipeline *p, const uint8_t *rgb, int width, int height, int stride_bytes, int64_t now_ms)
{
    if (width / p->cfg.block < p->gw || height / p->cfg.block < p->gh) return;
    downsample(p, rgb, width, height, stride_bytes);
    p->grid_has_yuv = false;
    if (grid_detect(p, now_ms)) downsample(p, rgb, width, height, stride_bytes);   /* zones moved: refill */
    grid_finish(p, now_ms);
}

void hl_pipeline_frame_yuv420p4(hl_pipeline *p, const uint8_t *buf, int width, int height, int stride_bytes, int64_t now_ms)
{
    if (width / p->cfg.block < p->gw || height / p->cfg.block < p->gh || (p->cfg.block & 1)) return;
    downsample_yuv420p4(p, buf, stride_bytes);
    p->grid_has_yuv = true;
    if (grid_detect(p, now_ms)) downsample_yuv420p4(p, buf, stride_bytes);
    grid_finish(p, now_ms);
}

/* HDR detection and black borders. Returns true when the border moved and the cell mask changed. */
static bool grid_detect(hl_pipeline *p, int64_t now_ms)
{
    const int cells = p->gw * p->gh;
    bool rebuilt = false;

    /* Automatic HDR detection on the grid. With a YUV source the decoder's own values are used; with RGB they are
     * re-derived with BT.601 full range (the JFIF convention the hardware decoder reverses). */
    if (p->cfg.hdr_mode == HL_HDR_AUTO) {
        for (int i = 0; i < cells; i += 2) {
            if (p->cell_mask && !p->cell_mask[i]) continue;
            if (p->grid_has_yuv) {
                const uint8_t *q = p->grid_yuv + (size_t)i * 3;
                hl_autohdr_sample(&p->autohdr, q[0], q[1], q[2]);
                continue;
            }
            const uint8_t *c = p->grid_rgb + (size_t)i * 3;
            float r = c[0], g = c[1], b = c[2];
            float y = 0.299f * r + 0.587f * g + 0.114f * b;
            float u = -0.168736f * r - 0.331264f * g + 0.5f * b + 128.f;
            float v = 0.5f * r - 0.418688f * g - 0.081312f * b + 128.f;
            hl_autohdr_sample(&p->autohdr, (uint8_t)hl_clampf(y, 0, 255), (uint8_t)hl_clampf(u, 0, 255), (uint8_t)hl_clampf(v, 0, 255));
        }
        p->hdr_active = hl_autohdr_finalize(&p->autohdr, now_ms);
    } else {
        p->hdr_active = (p->cfg.hdr_mode == HL_HDR_ON);
    }

    /* Black border detection on the grid, then rebuild zones if it moved. */
    if (p->cfg.blackborder) {
        if (hl_blackborder_process_rgb8(&p->bb, p->grid_rgb, p->gw, p->gh, p->gw * 3)) {
            const hl_border *cb = &p->bb.current;
            int hb = cb->unknown ? 0 : cb->horizontalSize;
            int vb = cb->unknown ? 0 : cb->verticalSize;
            hl_zones_free(&p->zones);
            if (hl_zones_init(&p->zones, p->leds, p->n_leds, p->gw, p->gh, hb, vb, p->cfg.sparse) != 0)
                hl_zones_init(&p->zones, p->leds, p->n_leds, p->gw, p->gh, 0, 0, p->cfg.sparse);
            rebuild_cell_mask(p);
            rebuilt = p->cell_mask != NULL;
        }
    }
    return rebuilt;
}

/* Tone map into linear light, average the zones, hand the targets to smoothing. */
static void grid_finish(hl_pipeline *p, int64_t now_ms)
{
    const int cells = p->gw * p->gh;
    for (int i = 0; i < cells; i++) {
        if (p->cell_mask && !p->cell_mask[i]) continue;
        const uint8_t *c = p->grid_rgb + (size_t)i * 3;
        if (p->hdr_active) {
            const uint8_t *lut = p->lut ? p->lut : p->lut_static;
            if (lut) {
                uint8_t o[3];
                hl_tonemap_lut_rgb8(lut, p->cfg.lut_n, c[0], c[1], c[2], o);
                p->grid_lin[i] = hl_f3_make(hl_srgb8_to_linear16(o[0]) * (1.f / 65535.f), hl_srgb8_to_linear16(o[1]) * (1.f / 65535.f), hl_srgb8_to_linear16(o[2]) * (1.f / 65535.f));
            } else {
                p->grid_lin[i] = hl_srgb_to_linear3(hl_tonemap_rgb8(&p->model, c[0], c[1], c[2]));
            }
        } else {
            p->grid_lin[i] = hl_f3_make(hl_srgb8_to_linear16(c[0]) * (1.f / 65535.f), hl_srgb8_to_linear16(c[1]) * (1.f / 65535.f), hl_srgb8_to_linear16(c[2]) * (1.f / 65535.f));
        }
    }

    hl_zones_average_f3(&p->zones, p->grid_lin, p->led_lin);
    memcpy(p->led_srgb, p->led_lin, sizeof(hl_f3) * (size_t)p->n_leds);
    hl_output_process(&p->out, p->led_srgb, p->n_leds);
    hl_smooth_set_targets(&p->smooth, p->led_srgb, now_ms);
    p->frames++;
}

void hl_pipeline_set_output(hl_pipeline *p, const hl_output_cfg *c)
{
    p->cfg.out = *c;
    hl_output_init(&p->out, c);
}

void hl_pipeline_output(hl_pipeline *p, int64_t now_ms, uint8_t *rgb_out)
{
    const float floor = hl_output_min_backlight(&p->out);
    hl_smooth_update(&p->smooth, now_ms, floor);
    for (int i = 0; i < p->n_leds; i++) {
        hl_f3 c = hl_f3_make(hl_clampf(p->smooth.current[i].x, floor, 1.f),
                             hl_clampf(p->smooth.current[i].y, floor, 1.f),
                             hl_clampf(p->smooth.current[i].z, floor, 1.f));
        hl_f3 *h = &p->held[i];
        if (hl_f3_maxelem(hl_f3_make(fabsf(c.x - h->x), fabsf(c.y - h->y), fabsf(c.z - h->z))) * 255.f > 0.49f) *h = c;
        rgb_out[i * 3 + 0] = (uint8_t)lroundf(h->x * 255.f);
        rgb_out[i * 3 + 1] = (uint8_t)lroundf(h->y * 255.f);
        rgb_out[i * 3 + 2] = (uint8_t)lroundf(h->z * 255.f);
    }
}
