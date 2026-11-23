/* MS2130 flash image check: `3c c3`, big-endian code length, header up to 0x30, code, then two 16-bit sums.
 * ms2130/tools/ms2130_image.py breaks a whole image down, including the user data past the code.
 * No ESP dependencies: tools/ms2130_image_test builds this file on the host. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define MS_CODE_OFF 0x30u

bool hl_ms2130_image_ok(const uint8_t *d, size_t len, char *why, size_t why_len)
{
    #define FAIL(...) do { if (why && why_len) snprintf(why, why_len, __VA_ARGS__); return false; } while (0)
    if (!d || len < MS_CODE_OFF + 4 || len > 65536) FAIL("image must be 52..65536 bytes, got %u", (unsigned)len);
    if (d[0] != 0x3c || d[1] != 0xc3) FAIL("no 3c c3 header (found %02x %02x)", d[0], d[1]);
    size_t code_len = ((size_t)d[2] << 8) | d[3], end = MS_CODE_OFF + code_len;
    if (code_len == 0 || end + 4 > len) FAIL("code length %u does not fit in %u bytes", (unsigned)code_len, (unsigned)len);
    uint16_t hdr = 0, code = 0;
    for (size_t i = 2; i < MS_CODE_OFF; i++) if (i < 0x0c || i > 0x0f) hdr = (uint16_t)(hdr + d[i]);
    for (size_t i = MS_CODE_OFF; i < end; i++) code = (uint16_t)(code + d[i]);
    if (hdr != (uint16_t)((d[end] << 8) | d[end + 1])) FAIL("header checksum is %04x, image says %02x%02x", hdr, d[end], d[end + 1]);
    if (code != (uint16_t)((d[end + 2] << 8) | d[end + 3])) FAIL("code checksum is %04x, image says %02x%02x", code, d[end + 2], d[end + 3]);
    if (why && why_len) snprintf(why, why_len, "code %u bytes, checksums ok", (unsigned)code_len);
    return true;
    #undef FAIL
}
