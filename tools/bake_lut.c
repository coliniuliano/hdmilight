/* Build-time tone-map LUT baker: evaluates the firmware's HDR model into an n^3 RGB table so the
 * ESP32 never has to (the 33^3 bake takes ~6 s on the P4). Compiled and run by firmware/main/CMakeLists.txt
 * with the host compiler; the output is embedded as a binary blob.
 *   bake_lut N out.bin            (uses model_calibrated.h when present, else the default model) */
#include <stdio.h>
#include <stdlib.h>
#include "hl_pipeline.h"
#if __has_include("model_calibrated.h")
#include "model_calibrated.h"
#define MODEL(m) hl_model_calibrated(m)
#else
#define MODEL(m) hl_model_default(m)
#endif

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: bake_lut N out.bin\n"); return 2; }
    int n = atoi(argv[1]);
    hl_model m; MODEL(&m);
    uint8_t *lut = hl_pipeline_build_lut(&m, n, NULL);
    if (!lut) { fprintf(stderr, "bake failed (n=%d)\n", n); return 1; }
    FILE *f = fopen(argv[2], "wb");
    if (!f || fwrite(lut, 1, (size_t)n * n * n * 3, f) != (size_t)n * n * n * 3) { fprintf(stderr, "write failed\n"); return 1; }
    fclose(f);
    return 0;
}
