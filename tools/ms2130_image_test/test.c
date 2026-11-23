/* Host test of firmware/main/ms2130_image.c: the check that guards the MS2130 flash writer. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
bool hl_ms2130_image_ok(const uint8_t *d, size_t len, char *why, size_t why_len);

static int fails;
static void expect(const char *name, bool got, bool want, const char *why)
{
    printf("%-44s %s  (%s)\n", name, got == want ? "ok" : "FAIL", why);
    if (got != want) fails++;
}

/* a small image built to the documented layout: header, code, header sum, code sum */
static size_t synth(uint8_t *d, size_t code_len)
{
    memset(d, 0xff, 65536);
    d[0] = 0x3c; d[1] = 0xc3; d[2] = (uint8_t)(code_len >> 8); d[3] = (uint8_t)code_len;
    for (size_t i = 4; i < 0x30; i++) d[i] = (uint8_t)(i * 7);
    for (size_t i = 0; i < code_len; i++) d[0x30 + i] = (uint8_t)(i * 13 + 5);
    uint16_t h = 0, c = 0;
    for (size_t i = 2; i < 0x30; i++) if (i < 0x0c || i > 0x0f) h = (uint16_t)(h + d[i]);
    for (size_t i = 0; i < code_len; i++) c = (uint16_t)(c + d[0x30 + i]);
    size_t end = 0x30 + code_len;
    d[end] = (uint8_t)(h >> 8); d[end + 1] = (uint8_t)h; d[end + 2] = (uint8_t)(c >> 8); d[end + 3] = (uint8_t)c;
    return 65536;
}

int main(int argc, char **argv)
{
    static uint8_t d[65536], t[65536]; char why[128];
    size_t n = synth(d, 5000);
    expect("synthetic image", hl_ms2130_image_ok(d, n, why, sizeof why), true, why);
    expect("truncated to code + sums", hl_ms2130_image_ok(d, 0x30 + 5000 + 4, why, sizeof why), true, why);
    expect("truncated inside the sums", hl_ms2130_image_ok(d, 0x30 + 5000 + 3, why, sizeof why), false, why);
    memcpy(t, d, n); t[0] = 0x3d;
    expect("wrong magic", hl_ms2130_image_ok(t, n, why, sizeof why), false, why);
    memcpy(t, d, n); t[0x30 + 1234] ^= 0x01;
    expect("one code bit flipped", hl_ms2130_image_ok(t, n, why, sizeof why), false, why);
    memcpy(t, d, n); t[0x10] ^= 0x40;
    expect("one header bit flipped", hl_ms2130_image_ok(t, n, why, sizeof why), false, why);
    memcpy(t, d, n); t[0x0d] ^= 0xff;
    expect("bytes 0x0c..0x0f are outside the sum", hl_ms2130_image_ok(t, n, why, sizeof why), true, why);
    memcpy(t, d, n); t[2] = 0xff; t[3] = 0xf0;
    expect("code length past the end", hl_ms2130_image_ok(t, n, why, sizeof why), false, why);
    memset(t, 0xff, n);
    expect("blank flash", hl_ms2130_image_ok(t, n, why, sizeof why), false, why);
    expect("too short", hl_ms2130_image_ok(d, 16, why, sizeof why), false, why);
    for (int i = 1; i < argc; i++) {                      /* real images given on the command line must pass */
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); fails++; continue; }
        size_t m = fread(t, 1, sizeof t, f); fclose(f);
        expect(argv[i], hl_ms2130_image_ok(t, m, why, sizeof why), true, why);
    }
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
