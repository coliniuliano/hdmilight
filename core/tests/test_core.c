/* Host sanity tests for the core. Build: make -C core test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hl_colorspace.h"
#include "hl_yuv.h"
#include "hl_tonemap.h"
#include "hl_zones.h"
#include "hl_smooth.h"
#include "hl_pipeline.h"
#include "hl_output.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (eps))

static void test_transfer(void)
{
    /* HyperHDR's own test vectors: sRGB 0.5 -> linear 0.214 */
    CHECK(NEAR(hl_srgb_to_linear(0.5f), 0.214f, 0.002f), "srgb->lin 0.5 = %f", hl_srgb_to_linear(0.5f));
    CHECK(NEAR(hl_linear_to_srgb(0.214f), 0.5f, 0.002f), "lin->srgb 0.214 = %f", hl_linear_to_srgb(0.214f));
    for (int i = 0; i <= 10; i++) {
        float v = i / 10.f;
        CHECK(NEAR(hl_linear_to_srgb(hl_srgb_to_linear(v)), v, 1e-4f), "srgb roundtrip %f", v);
        CHECK(NEAR(hl_linear_to_bt2020(hl_bt2020_to_linear(v)), v, 1e-4f), "bt2020 roundtrip %f", v);
    }
    /* PQ: code value 1.0 is 10000 nits -> with scale 10000/nits = 1/nits... HyperHDR calls PQ(10000/nits, x) */
    float pq1 = hl_pq_st2084(10000.0f / 10000.0f, 1.0f);
    CHECK(NEAR(pq1, 1.0f, 1e-4f), "PQ(1.0) = %f", pq1);
    /* 0.508 code value is ~100 nits (ST.2084) */
    float pq_100 = hl_pq_st2084(10000.0f, 0.508f);
    CHECK(NEAR(pq_100, 100.0f, 2.0f), "PQ(0.508)*10000 = %f (expect ~100)", pq_100);
    CHECK(hl_srgb8_to_linear16(0) == 0 && hl_srgb8_to_linear16(255) == 65535, "linear16 endpoints");
}

static void test_yuv(void)
{
    hl_f3 rgb = hl_f3_make(0.8f, 0.3f, 0.1f);
    for (int c = 0; c < 4; c++) {
        for (int r = 1; r <= 2; r++) {
            hl_f3 yuv = hl_rgb_to_yuv((hl_range_t)r, (hl_yuv_coef_t)c, rgb);
            hl_f3 back = hl_yuv_to_rgb((hl_range_t)r, (hl_yuv_coef_t)c, yuv);
            CHECK(NEAR(back.x, rgb.x, 1e-4f) && NEAR(back.y, rgb.y, 1e-4f) && NEAR(back.z, rgb.z, 1e-4f),
                  "yuv roundtrip coef %d range %d -> %f %f %f", c, r, back.x, back.y, back.z);
        }
    }
    /* BT.601 limited: white -> Y=235/255, U=V=128/255 */
    hl_f3 w = hl_rgb_to_yuv(HL_RANGE_LIMITED, HL_YUV_BT601, hl_f3_make(1, 1, 1));
    CHECK(NEAR(w.x, 235.f / 255.f, 1e-4f) && NEAR(w.y, 128.f / 255.f, 1e-4f) && NEAR(w.z, 128.f / 255.f, 1e-4f), "white limited yuv %f %f %f", w.x, w.y, w.z);
    /* Default model coefMatrix is BT.601 limited with the calibrator's coefDelta
     * (0.004, -0.002) folded in, so it sits within ~1% of the textbook matrix. */
    hl_model m; hl_model_default(&m);
    hl_m34 ref = hl_yuv_yuv2rgb_matrix(HL_RANGE_LIMITED, HL_YUV_BT601);
    CHECK(NEAR(ref.c0.x, m.coefMatrix.c0.x, 1e-3f) && NEAR(ref.c2.x, m.coefMatrix.c2.x, 0.02f) && NEAR(ref.c3.x, m.coefMatrix.c3.x, 0.02f),
          "default coefMatrix vs BT601 limited: %f/%f %f/%f %f/%f", ref.c0.x, m.coefMatrix.c0.x, ref.c2.x, m.coefMatrix.c2.x, ref.c3.x, m.coefMatrix.c3.x);
}

static void test_tonemap(void)
{
    hl_model m; hl_model_default(&m);
    /* Raw HDR grey: PQ ~0.5 code (limited ~ Y 125) is around 100 nits -> should map near mid-bright sRGB */
    hl_f3 g = hl_tonemap_yuv8(&m, 125, 128, 128);
    CHECK(g.x > 0.4f && g.x < 1.0f && NEAR(g.x, g.y, 0.08f) && NEAR(g.y, g.z, 0.08f), "HDR grey Y=125 -> %f %f %f", g.x, g.y, g.z);
    /* Monotonic in Y */
    float prev = -1.f;
    for (int y = 16; y <= 235; y += 8) {
        hl_f3 s = hl_tonemap_yuv8(&m, (uint8_t)y, 128, 128);
        CHECK(s.x >= prev - 1e-4f, "non-monotonic at Y=%d: %f < %f", y, s.x, prev);
        prev = s.x;
    }
    /* Black stays black, near-white saturates */
    hl_f3 k = hl_tonemap_yuv8(&m, 16, 128, 128);
    CHECK(k.x < 0.02f && k.y < 0.02f && k.z < 0.02f, "black -> %f %f %f", k.x, k.y, k.z);
    hl_f3 wht = hl_tonemap_yuv8(&m, 235, 128, 128);
    CHECK(wht.x > 0.95f, "white -> %f", wht.x);
    /* RGB entry agrees with YUV entry for the same colour */
    hl_f3 a = hl_tonemap_rgb8(&m, 200, 100, 50);
    hl_f3 yuv = hl_rgb_to_yuv(m.signal.range, m.coef, hl_f3_make(200 / 255.f, 100 / 255.f, 50 / 255.f));
    hl_f3 b = hl_tonemap_yuv8(&m, (uint8_t)lroundf(yuv.x * 255), (uint8_t)lroundf(yuv.y * 255), (uint8_t)lroundf(yuv.z * 255));
    CHECK(NEAR(a.x, b.x, 0.03f) && NEAR(a.y, b.y, 0.03f) && NEAR(a.z, b.z, 0.03f), "rgb vs yuv entry: %f %f %f vs %f %f %f", a.x, a.y, a.z, b.x, b.y, b.z);
    /* LUT agrees with direct evaluation at grid points */
    int n = 17;
    uint8_t *lut = malloc((size_t)n * n * n * 3);
    hl_tonemap_build_lut_rgb(&m, n, lut);
    uint8_t o[3];
    hl_tonemap_lut_rgb8(lut, n, 255, 0, 0, o);
    hl_f3 d = hl_tonemap_rgb8(&m, 255, 0, 0);
    CHECK(abs(o[0] - (int)lroundf(d.x * 255)) <= 1, "lut red %d vs %f", o[0], d.x * 255);
    free(lut);
}

static void test_zones(void)
{
    enum { W = 40, H = 30 };
    static uint8_t img[W * H * 3];
    /* left half red, right half green */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = img + (y * W + x) * 3;
        if (x < W / 2) { p[0] = 255; p[1] = 0; p[2] = 0; } else { p[0] = 0; p[1] = 255; p[2] = 0; }
    }
    hl_led leds[2] = { { 0.f, 0.5f, 0.f, 1.f, 0, false }, { 0.5f, 1.f, 0.f, 1.f, 0, false } };
    hl_zones z;
    CHECK(hl_zones_init(&z, leds, 2, W, H, 0, 0, false) == 0, "zones init");
    hl_f3 out[2];
    hl_zones_average_rgb8(&z, img, W * 3, out);
    CHECK(NEAR(out[0].x, 1.f, 1e-3f) && NEAR(out[0].y, 0.f, 1e-3f), "left zone %f %f %f", out[0].x, out[0].y, out[0].z);
    CHECK(NEAR(out[1].y, 1.f, 1e-3f) && NEAR(out[1].x, 0.f, 1e-3f), "right zone %f %f %f", out[1].x, out[1].y, out[1].z);
    CHECK(z.start[1] - z.start[0] == (W / 2) * H, "left zone pixel count %u", z.start[1] - z.start[0]);
    hl_zones_free(&z);

    hl_led layout[64];
    int n = hl_zones_layout_classic(layout, 64, 10, 6, 10, 6, 0.1f);
    CHECK(n == 32, "classic layout count %d", n);
    CHECK(NEAR(layout[0].hmin, 0.f, 1e-6f) && NEAR(layout[9].hmax, 1.f, 1e-6f), "top edge span");
}

static void test_smooth(void)
{
    hl_smooth s; hl_smooth_init(&s, 1);
    hl_f3 a = hl_f3_make(0, 0, 0), b = hl_f3_make(1, 1, 1);
    hl_smooth_set_targets(&s, &a, 0);
    hl_smooth_set_targets(&s, &b, 10);
    hl_f3 c;
    float prev = 0.f; int overshoot = 0;
    for (int64_t t = 20; t <= 1500; t += 20) {
        hl_smooth_update(&s, t, 0.f);
        hl_smooth_get(&s, 0.f, &c);
        if (c.x > 1.0001f) overshoot++;
        if (t == 160) CHECK(c.x > 0.3f && c.x < 1.0f, "mid-transition at 150ms: %f", c.x);
        prev = c.x;
    }
    CHECK(NEAR(prev, 1.f, 1e-3f), "converged to target: %f", prev);
    CHECK(overshoot == 0, "clamped, no overshoot");
    CHECK(s.complete, "animation marked complete");
    hl_smooth_free(&s);
}

static void test_pipeline(void)
{
    enum { W = 640, H = 480 };
    uint8_t *img = malloc((size_t)W * H * 3);
    /* top third blue, rest dark grey */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = img + ((size_t)y * W + x) * 3;
        if (y < H / 3) { p[0] = 10; p[1] = 20; p[2] = 240; } else { p[0] = 128; p[1] = 128; p[2] = 128; }
    }
    hl_led leds[32];
    int n = hl_zones_layout_classic(leds, 32, 10, 6, 10, 6, 0.08f);
    hl_pipeline_cfg cfg; hl_pipeline_cfg_default(&cfg);
    cfg.hdr_mode = HL_HDR_OFF; cfg.blackborder = false; cfg.out.gamma = 1.f;
    hl_model m; hl_model_default(&m);
    hl_pipeline p;
    CHECK(hl_pipeline_init(&p, &cfg, &m, leds, n, W, H) == 0, "pipeline init");
    hl_pipeline_frame_rgb8(&p, img, W, H, W * 3, 1000);
    uint8_t out[32 * 3];
    for (int64_t t = 1020; t <= 2000; t += 20) hl_pipeline_output(&p, t, out);
    /* top LEDs blue, bottom LEDs grey */
    CHECK(out[2] > 200 && out[0] < 40, "top LED 0 = %d %d %d", out[0], out[1], out[2]);
    int bi = 10 + 6 + 3;   /* a bottom LED */
    CHECK(abs(out[bi * 3] - 128) <= 2 && abs(out[bi * 3 + 2] - 128) <= 2, "bottom LED = %d %d %d", out[bi * 3], out[bi * 3 + 1], out[bi * 3 + 2]);
    /* HyperHDR's default output gamma 1.5: 128 -> 255 * 0.502^1.5 = 91 */
    hl_output_cfg oc; hl_output_cfg_default(&oc);
    hl_pipeline_set_output(&p, &oc);
    hl_pipeline_frame_rgb8(&p, img, W, H, W * 3, 2000);
    for (int64_t t = 2020; t <= 3000; t += 20) hl_pipeline_output(&p, t, out);
    CHECK(abs(out[bi * 3] - 91) <= 1, "gamma 1.5 bottom LED = %d", out[bi * 3]);

    /* HDR on: PQ code 128 is ~90 nits against a 229-nit white, so mid grey
     * comes out brighter than pass-through (the "raw HDR looks dim" fix). */
    cfg.hdr_mode = HL_HDR_ON;
    hl_pipeline p2;
    CHECK(hl_pipeline_init(&p2, &cfg, &m, leds, n, W, H) == 0, "pipeline init hdr");
    hl_pipeline_frame_rgb8(&p2, img, W, H, W * 3, 1000);
    uint8_t out2[32 * 3];
    for (int64_t t = 1020; t <= 2000; t += 20) hl_pipeline_output(&p2, t, out2);
    CHECK(out2[bi * 3] > out[bi * 3], "hdr tone map lifted mid grey: %d vs %d", out2[bi * 3], out[bi * 3]);
    /* LUT path matches direct within a few counts */
    cfg.lut_n = 33;
    hl_pipeline p3;
    CHECK(hl_pipeline_init(&p3, &cfg, &m, leds, n, W, H) == 0, "pipeline init lut");
    hl_pipeline_frame_rgb8(&p3, img, W, H, W * 3, 1000);
    uint8_t out3[32 * 3];
    for (int64_t t = 1020; t <= 2000; t += 20) hl_pipeline_output(&p3, t, out3);
    CHECK(abs(out3[bi * 3] - out2[bi * 3]) <= 6, "lut vs direct: %d vs %d", out3[bi * 3], out2[bi * 3]);
    hl_pipeline_free(&p); hl_pipeline_free(&p2); hl_pipeline_free(&p3);
    free(img);
}


/* The packed 4:2:0 path must land on the same grid colours as the RGB path (within rounding + chroma averaging). */
static void test_pipeline_yuv420p4(void)
{
    const int W = 64, H = 32, B = 8;
    static uint8_t rgb[64 * 32 * 3];
    static uint8_t yuv[64 * 32 * 3 / 2] __attribute__((aligned(4)));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *c = rgb + ((size_t)y * W + x) * 3;
            c[0] = (uint8_t)(x * 4); c[1] = (uint8_t)(y * 8); c[2] = (uint8_t)(255 - x * 3);
        }
    /* pack as the P4 decoder does: per line [C Y Y]..., U on even lines, V on odd, chroma from the 2x2 block */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x += 2) {
            float su = 0, sv = 0; int yy, xx;
            for (yy = (y & ~1); yy < (y & ~1) + 2; yy++)
                for (xx = x; xx < x + 2; xx++) {
                    const uint8_t *c = rgb + ((size_t)yy * W + xx) * 3;
                    su += -0.168736f * c[0] - 0.331264f * c[1] + 0.5f * c[2] + 128.f;
                    sv += 0.5f * c[0] - 0.418688f * c[1] - 0.081312f * c[2] + 128.f;
                }
            uint8_t *t = yuv + (size_t)y * W * 3 / 2 + (size_t)x * 3 / 2;
            t[0] = (uint8_t)lroundf(((y & 1) ? sv : su) / 4.f);
            for (int k = 0; k < 2; k++) {
                const uint8_t *c = rgb + ((size_t)y * W + x + k) * 3;
                t[1 + k] = (uint8_t)lroundf(0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]);
            }
        }
    hl_led leds[4]; int n = hl_zones_layout_classic(leds, 4, 1, 1, 1, 1, 0.5f);
    hl_pipeline_cfg cfg; hl_pipeline_cfg_default(&cfg); cfg.block = B; cfg.blackborder = false; cfg.hdr_mode = HL_HDR_OFF;
    hl_model m; hl_model_default(&m);
    hl_pipeline a, b;
    CHECK(hl_pipeline_init(&a, &cfg, &m, leds, n, W, H) == 0, "init rgb");
    CHECK(hl_pipeline_init(&b, &cfg, &m, leds, n, W, H) == 0, "init yuv");
    hl_pipeline_frame_rgb8(&a, rgb, W, H, W * 3, 0);
    hl_pipeline_frame_yuv420p4(&b, yuv, W, H, W * 3 / 2, 0);
    int maxd = 0;
    for (int i = 0; i < a.gw * a.gh * 3; i++) { int d = abs((int)a.grid_rgb[i] - (int)b.grid_rgb[i]); if (d > maxd) maxd = d; }
    /* the word-wise filter must equal a plain byte loop exactly */
    int exact = 1;
    for (int gy = 0; gy < b.gh; gy++) for (int gx = 0; gx < b.gw; gx++) {
        unsigned sy = 0, su = 0, sv = 0;
        for (int y = 0; y < B; y++) for (int t = 0; t < B / 2; t++) {
            const uint8_t *tr = yuv + (size_t)(gy * B + y) * W * 3 / 2 + (size_t)gx * B * 3 / 2 + t * 3;
            sy += tr[1] + tr[2]; if (y & 1) sv += tr[0]; else su += tr[0];
        }
        const uint8_t *q = b.grid_yuv + ((size_t)gy * b.gw + gx) * 3;
        if (q[0] != (sy + B * B / 2) / (B * B) || q[1] != (su + 8) / 16 || q[2] != (sv + 8) / 16) exact = 0;
    }
    CHECK(exact, "4:2:0 box filter differs from the reference loop");
    CHECK(maxd <= 3, "yuv420p4 vs rgb8 grid differ by %d", maxd);
    CHECK(b.grid_has_yuv && !a.grid_has_yuv, "grid_has_yuv flags");
    hl_pipeline_free(&a); hl_pipeline_free(&b);
}

/* edge_only must give the full grid's LED targets and border, including across a letterbox change. */
static void pack_yuv420p4(const uint8_t *rgb, uint8_t *yuv, int W, int H)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x += 2) {
            float su = 0, sv = 0;
            for (int yy = (y & ~1); yy < (y & ~1) + 2; yy++)
                for (int xx = x; xx < x + 2; xx++) {
                    const uint8_t *c = rgb + ((size_t)yy * W + xx) * 3;
                    su += -0.168736f * c[0] - 0.331264f * c[1] + 0.5f * c[2] + 128.f;
                    sv += 0.5f * c[0] - 0.418688f * c[1] - 0.081312f * c[2] + 128.f;
                }
            uint8_t *t = yuv + (size_t)y * W * 3 / 2 + (size_t)x * 3 / 2;
            t[0] = (uint8_t)lroundf(((y & 1) ? sv : su) / 4.f);
            for (int k = 0; k < 2; k++) {
                const uint8_t *c = rgb + ((size_t)y * W + x + k) * 3;
                t[1 + k] = (uint8_t)lroundf(0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]);
            }
        }
}

static void test_pipeline_edge_only(void)
{
    const int W = 640, H = 480;
    static uint8_t rgb[640 * 480 * 3];
    static uint8_t yuv[640 * 480 * 3 / 2] __attribute__((aligned(4)));
    hl_led leds[128]; int n = hl_zones_layout_classic(leds, 128, 30, 17, 30, 17, 0.08f);
    hl_pipeline_cfg cfg; hl_pipeline_cfg_default(&cfg); cfg.block = 8; cfg.blackborder = true; cfg.hdr_mode = HL_HDR_OFF;
    hl_model m; hl_model_default(&m);
    hl_pipeline full, edge;
    cfg.edge_only = false; CHECK(hl_pipeline_init(&full, &cfg, &m, leds, n, W, H) == 0, "init full");
    cfg.edge_only = true;  CHECK(hl_pipeline_init(&edge, &cfg, &m, leds, n, W, H) == 0, "init edge_only");
    CHECK(edge.cells_used > 0 && edge.cells_used < edge.gw * edge.gh, "edge_only computed %d of %d cells", edge.cells_used, edge.gw * edge.gh);
    int worst_cells = edge.cells_used; const int init_cells = edge.cells_used;
    float md = 0; int border_mismatch = 0;
    for (int f = 0; f < 140; f++) {
        const int bar = (f >= 70) ? 60 : 0;   /* 60-row letterbox bars from frame 70 on */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint8_t *c = rgb + ((size_t)y * W + x) * 3;
                const bool black = y < bar || y >= H - bar;
                c[0] = black ? 0 : (uint8_t)((x + f * 3) & 255); c[1] = black ? 0 : (uint8_t)((y * 2 + f) & 255); c[2] = black ? 0 : (uint8_t)((x ^ y) & 255);
            }
        pack_yuv420p4(rgb, yuv, W, H);
        hl_pipeline_frame_yuv420p4(&full, yuv, W, H, W * 3 / 2, (uint32_t)f * 33);
        hl_pipeline_frame_yuv420p4(&edge, yuv, W, H, W * 3 / 2, (uint32_t)f * 33);
        if (full.bb.current.horizontalSize != edge.bb.current.horizontalSize || full.bb.current.verticalSize != edge.bb.current.verticalSize ||
            full.bb.current.unknown != edge.bb.current.unknown) border_mismatch++;
        for (int i = 0; i < n; i++) {
            md = fmaxf(md, fabsf(full.led_srgb[i].x - edge.led_srgb[i].x));
            md = fmaxf(md, fabsf(full.led_srgb[i].y - edge.led_srgb[i].y));
            md = fmaxf(md, fabsf(full.led_srgb[i].z - edge.led_srgb[i].z));
        }
        if (edge.cells_used > worst_cells) worst_cells = edge.cells_used;
    }
    CHECK(md == 0.f, "edge_only LED targets differ from full grid by %g", md);
    CHECK(border_mismatch == 0, "edge_only border differed on %d frames", border_mismatch);
    CHECK(full.bb.current.horizontalSize > 0, "letterbox not detected (h=%d)", full.bb.current.horizontalSize);
    printf("edge_only: %d..%d of %d cells (%.0f%% at worst)\n", edge.cells_used, worst_cells, edge.gw * edge.gh, 100.0 * worst_cells / (edge.gw * edge.gh));
    hl_pipeline auto_hdr; cfg.hdr_mode = HL_HDR_AUTO;
    CHECK(hl_pipeline_init(&auto_hdr, &cfg, &m, leds, n, W, H) == 0, "init edge_only auto");
    CHECK(auto_hdr.cells_used > init_cells && auto_hdr.cells_used <= init_cells + 300, "auto-HDR lattice adds %d cells", auto_hdr.cells_used - init_cells);
    hl_pipeline_free(&auto_hdr);
    hl_pipeline_free(&full); hl_pipeline_free(&edge);
}

static void test_output(void)
{
    hl_output_cfg c; hl_output_cfg_default(&c);
    hl_output o;
    /* defaults minus gamma/backlight: plain linear -> sRGB */
    c.gamma = 1.f; c.backlight = 0.f;
    hl_output_init(&o, &c);
    hl_f3 v[2] = { hl_f3_make(0.214f, 0.214f, 0.214f), hl_f3_make(1.f, 0.f, 0.f) };
    hl_output_process(&o, v, 2);
    CHECK(NEAR(v[0].x, 0.5f, 0.002f) && NEAR(v[1].x, 1.f, 1e-5f) && v[1].y == 0.f, "pass-through %f %f", v[0].x, v[1].y);
    /* classic calibration: a weak-red strip wants more red for red */
    hl_output_cfg_default(&c); c.gamma = 1.f; c.backlight = 0.f;
    c.cal[HL_CAL_RED][1] = 25;
    hl_output_init(&o, &c);
    v[0] = hl_f3_make(0.f, 1.f, 0.f); hl_output_process(&o, v, 1);
    CHECK(o.cal_mode == 1 && v[0].x > 0.3f && NEAR(v[0].y, 1.f, 1e-5f), "matrix cal %f %f", v[0].x, v[0].y);
    /* eight-colour LUT with identity targets except white: only white moves */
    hl_output_cfg_default(&c); c.gamma = 1.f; c.backlight = 0.f; c.classic = false;
    c.cal[HL_CAL_WHITE][2] = 200;
    hl_output_init(&o, &c);
    v[0] = hl_f3_make(1.f, 1.f, 1.f); v[1] = hl_f3_make(1.f, 0.f, 0.f); hl_output_process(&o, v, 2);
    CHECK(o.cal_mode == 2 && NEAR(v[0].z, hl_linear_to_srgb(200 / 255.f), 1e-3f) && NEAR(v[1].x, 1.f, 1e-5f) && v[1].y == 0.f, "lut cal %f %f %f", v[0].z, v[1].x, v[1].y);
    /* backlight: coloured floors each channel, grey replaces dark colours */
    hl_output_cfg_default(&c); c.gamma = 1.f; c.backlight = 0.1f;
    hl_output_init(&o, &c); v[0] = hl_f3_make(0.f, 0.f, 0.f); hl_output_process(&o, v, 1);
    CHECK(NEAR(v[0].x, 0.1f, 1e-6f), "coloured backlight %f", v[0].x);
    CHECK(NEAR(hl_output_min_backlight(&o), 0.1f, 1e-6f), "min backlight");
    /* power limit: three full-white LEDs at 50 % come out at half */
    hl_output_cfg_default(&c); c.gamma = 1.f; c.backlight = 0.f; c.power_limit = 0.5f;
    hl_output_init(&o, &c);
    hl_f3 w[3] = { hl_f3_make(1, 1, 1), hl_f3_make(1, 1, 1), hl_f3_make(1, 1, 1) }; hl_output_process(&o, w, 3);
    CHECK(NEAR(w[1].y, 0.5f, 1e-5f), "power limit %f", w[1].y);
    /* saturation gain 0 greys out */
    hl_output_cfg_default(&c); c.gamma = 1.f; c.backlight = 0.f; c.saturation = 0.f;
    hl_output_init(&o, &c); v[0] = hl_f3_make(1.f, 0.f, 0.f); hl_output_process(&o, v, 1);
    CHECK(NEAR(v[0].x, 1.f, 1e-5f) && NEAR(v[0].y, 1.f, 1e-5f), "saturation 0 -> white %f %f", v[0].x, v[0].y);
}

int main(void)
{
    test_transfer();
    test_yuv();
    test_tonemap();
    test_zones();
    test_smooth();
    test_pipeline();
    test_pipeline_yuv420p4();
    test_pipeline_edge_only();
    test_output();
    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("all core tests passed\n");
    return 0;
}
