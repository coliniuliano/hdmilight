#include "gsv_edid.h"
#include <string.h>

static uint8_t block_sum(const uint8_t *b) { uint8_t s = 0; for (unsigned i = 0; i < 0x7f; i++) s = (uint8_t)(s + b[i]); return (uint8_t)-s; }

/* The original branded the EDID as its own product here: manufacturer id "MEC" and "MEC" over the first three
 * characters of the monitor-name descriptor. This board is not that product, so it stamps its own id and leaves
 * the name to gsv_edid_builtin (or to gsv_edid_set_name, which the application calls after this).
 *
 * "HLV" is a placeholder, not an assigned id: it is absent from the UEFI PNP ID registry (checked 2026-11-22),
 * but nobody has registered it to this project either. Anyone selling a board should register their own. */
void gsv_edid_fix_checksums(uint8_t *edid)
{
    edid[8] = 0x21; edid[9] = 0x96;                                   /* manufacturer id "HLV" */
    edid[0x7f] = block_sum(edid);
    if (edid[0x7e]) edid[0xff] = block_sum(edid + 0x80);
}

void gsv_edid_set_name(uint8_t *edid, const char *name)
{
    for (unsigned i = 0; i < 4; i++) {
        uint8_t *d = edid + 0x36 + i * 0x12;
        if (d[0] != 0 || d[1] != 0 || d[2] != 0 || d[3] != 0xfc || d[4] != 0) continue;       /* the monitor-name descriptor */
        size_t n = strlen(name);
        if (n > 13) n = 13;
        memcpy(d + 5, name, n);
        if (n < 13) { d[5 + n] = 0x0a; memset(d + 6 + n, 0x20, 12 - n); }                    /* line feed, then spaces */
        edid[0x7f] = block_sum(edid);
        return;
    }
}

void gsv_edid_strip_dolby_vision(uint8_t *edid)
{
    if (edid[0x7e] == 0 || edid[0x80] != 2) return;
    uint8_t *cea = edid + 0x80;
    for (unsigned i = 4; i < cea[2]; i = (i + (cea[i] & 0x1f) + 1) & 0xffff) {
        if ((cea[i] & 0xe0) != 0xe0 || cea[i + 1] != 1) continue;        /* vendor-specific video data block */
        if (cea[i + 2] != 0x46 || cea[i + 3] != 0xd0 || cea[i + 4] != 0x00) return;   /* Dolby OUI 00-D0-46 */
        uint8_t len = cea[i] & 0x1f, ver = cea[i + 5] >> 5;
        if (ver == 0 && len == 0x19) {
            memcpy(cea + i, gsv_edid_hf_vsdb, 12);
            memmove(cea + i + 12, cea + i + 26, 0x65 - i);
            memset(edid + 0xf1, 0, 14);
            cea[2] = (uint8_t)(cea[2] - 14);
        } else if (ver == 1 && len == 0xe) {
            memcpy(cea + i, gsv_edid_hf_vsdb, 12);
            memmove(cea + i + 12, cea + i + 15, 0x70 - i);
            edid[0xfc] = edid[0xfd] = edid[0xfe] = 0;
            cea[2] = (uint8_t)(cea[2] - 3);
        } else if ((ver == 1 && len == 0xb) || (ver == 2 && len == 0xb && (cea[i + 7] & 3))) {
            memcpy(cea + i, gsv_edid_hf_vsdb, 12);
        } else return;
        edid[0xff] = block_sum(edid + 0x80);
        return;
    }
}
