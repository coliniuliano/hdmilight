/* Host simulator: run the exact core pipeline on JPEG frames captured from the
 * MS2130 on a Mac (see capture_mac.sh), print per-LED colours, and optionally
 * write a PPM "strip" image per frame for eyeballing.
 *
 *   host_sim [--hdr auto|on|off] [--block N] [--out dir] frame1.jpg frame2.jpg ...
 *
 * Frames are fed at 30 fps of simulated time; smoothing runs at 50 Hz. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "third_party/stb_image.h"
#include "hl_pipeline.h"
#if __has_include("model_calibrated.h")
#include "model_calibrated.h"
#define MODEL(m) hl_model_calibrated(m)
#else
#define MODEL(m) hl_model_default(m)
#endif

static void write_strip(const char *dir, int idx, const uint8_t *rgb, int n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/strip_%04d.ppm", dir, idx);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int cell = 16, h = 32;
    fprintf(f, "P6\n%d %d\n255\n", n * cell, h);
    for (int y = 0; y < h; y++)
        for (int i = 0; i < n; i++)
            for (int x = 0; x < cell; x++) fwrite(rgb + i * 3, 1, 3, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    hl_pipeline_cfg cfg; hl_pipeline_cfg_default(&cfg);
    const char *out_dir = NULL;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "--hdr") && argi + 1 < argc) {
            const char *m = argv[++argi];
            cfg.hdr_mode = !strcmp(m, "on") ? HL_HDR_ON : !strcmp(m, "off") ? HL_HDR_OFF : HL_HDR_AUTO;
        } else if (!strcmp(argv[argi], "--block") && argi + 1 < argc) {
            cfg.block = atoi(argv[++argi]);
        } else if (!strcmp(argv[argi], "--out") && argi + 1 < argc) {
            out_dir = argv[++argi];
        } else {
            fprintf(stderr, "unknown option %s\n", argv[argi]);
            return 2;
        }
        argi++;
    }
    if (argi >= argc) { fprintf(stderr, "usage: host_sim [--hdr auto|on|off] [--block N] [--out dir] frames...\n"); return 2; }

    hl_led leds[256];
    int n = hl_zones_layout_classic(leds, 256, 30, 17, 30, 17, 0.08f);
    hl_model model; MODEL(&model);
    hl_pipeline p;
    int inited = 0;
    uint8_t out[256 * 3];
    int64_t t = 1000;

    for (int i = argi; i < argc; i++) {
        int w, h, ch;
        uint8_t *img = stbi_load(argv[i], &w, &h, &ch, 3);
        if (!img) { fprintf(stderr, "%s: %s\n", argv[i], stbi_failure_reason()); continue; }
        if (!inited) {
            if (hl_pipeline_init(&p, &cfg, &model, leds, n, w, h) != 0) { fprintf(stderr, "pipeline init failed for %dx%d\n", w, h); return 1; }
            inited = 1;
            printf("pipeline: %dx%d, grid %dx%d, %d LEDs, hdr=%d, lut=%d\n", w, h, p.gw, p.gh, n, cfg.hdr_mode, cfg.lut_n);
        }
        hl_pipeline_frame_rgb8(&p, img, w, h, w * 3, t);
        for (int k = 0; k < 2; k++) { t += 16; hl_pipeline_output(&p, t, out); }   /* ~33 ms of 50 Hz output */
        stbi_image_free(img);
        printf("%s: hdr=%d border h%d v%d | top0 %3d %3d %3d | right0 %3d %3d %3d | bottom0 %3d %3d %3d | left0 %3d %3d %3d\n",
               argv[i], (int)p.hdr_active,
               p.bb.current.unknown ? -1 : p.bb.current.horizontalSize, p.bb.current.unknown ? -1 : p.bb.current.verticalSize,
               out[0], out[1], out[2], out[30 * 3], out[30 * 3 + 1], out[30 * 3 + 2],
               out[47 * 3], out[47 * 3 + 1], out[47 * 3 + 2], out[77 * 3], out[77 * 3 + 1], out[77 * 3 + 2]);
        if (out_dir) write_strip(out_dir, i - argi, out, n);
    }
    if (inited) hl_pipeline_free(&p);
    return 0;
}
