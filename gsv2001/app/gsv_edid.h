/* EDID handling, after the EZ-SP12H2's application: the built-in EDID and the fix-ups applied before one goes to the Rx. */
#pragma once
#include <stdint.h>

extern const uint8_t gsv_edid_builtin[256];        /* shown to the source while no TV's EDID is known */
extern const uint8_t gsv_edid_hf_vsdb[12];

void gsv_edid_fix_checksums(uint8_t *edid);        /* 0x0800e0f8: stamp our manufacturer id and recompute both checksums */
void gsv_edid_set_name(uint8_t *edid, const char *name);   /* not in the original: the monitor name a source shows, up to 13 characters */
void gsv_edid_strip_dolby_vision(uint8_t *edid);   /* 0x0800e6a8: swap the Dolby Vision VSVDB for the HF-VSDB */
