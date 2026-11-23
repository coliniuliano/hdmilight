/* Smoke test of the oneboard set-up on a GSV2001: the application layer (app/gsv_app.c) with gsv_chip_type =
 * GSV2001 against a plain register model (what is written is read back; events preset what a source and two sinks
 * would make the chip report, the same values ../gsv2001/tools/emu.py uses). Not a replay: no firmware builds this
 * layout. It checks that the SDK gets where it should: output A in pass-through and running, output B fed through
 * the scaler for a 4K60 source with a 1080p60 infoframe, and nothing addressed to a device that does not exist. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gsv_sdk.h"
#include "gsv_app.h"

static uint8_t reg[0x10000]; static int bad_dev, writes;
static void set(unsigned page, unsigned r, uint8_t v) { reg[(page << 8) | r] = v; }
static uint8_t get(unsigned page, unsigned r) { return reg[(page << 8) | r]; }
static int rd(uint8_t dev, uint16_t r, uint8_t *d, uint16_t n) { if (dev != 0xb0) { bad_dev++; memset(d, 0, n); return 0; } for (unsigned i = 0; i < n; i++) d[i] = reg[(uint16_t)(r + i)]; return 0; }
static int wr(uint8_t dev, uint16_t r, uint8_t *d, uint16_t n) { if (dev != 0xb0) { bad_dev++; return 0; } for (unsigned i = 0; i < n; i++) reg[(uint16_t)(r + i)] = d[i]; writes++; return 0; }
static const gsv_platform pf = { rd, wr };
void gsv_log(const char *fmt, ...) { (void)fmt; }
void gsv_todo(const char *what, const char *where) { fprintf(stderr, "TODO reached: %s in %s\n", what, where); exit(1); }
void gsv_delay_ms(unsigned ms) { (void)ms; }

static void ev_lock_4k60(void)
{
    set(0x20, 0x00, 0xb8); set(0x20, 0x03, 0x00); set(0x02, 0x39, 0x0b); set(0x02, 0x3a, 0x9a); set(0x02, 0x2f, 0x00);
    for (unsigned l = 0; l < 3; l++) { set(0x27 + l, 0x20, 0x10); set(0x27 + l, 0x92, 0x17); set(0x27 + l, 0x93, 0x70); set(0x27 + l, 0x96, 0x20); set(0x27 + l, 0x3a, 0); set(0x27 + l, 0x0e, 0); }
    set(0x20, 0x26, 0x11); set(0x20, 0x27, 0x30); set(0x20, 0x2e, 0x11); set(0x20, 0x2f, 0x94);
    set(0x20, 0x0c, 0x0f); set(0x20, 0x0d, 0x00); set(0x20, 0x0e, 0x08); set(0x20, 0x0f, 0x70);
    static const uint8_t hdr[3] = { 0x82, 0x02, 0x0d }, body[5] = { 0x00, 0x28, 0x00, 97, 0x00 };
    unsigned sum = 0;
    for (unsigned i = 0; i < 3; i++) { set(0x24, 0xe0 + i, hdr[i]); sum += hdr[i]; }
    for (unsigned i = 0; i < 5; i++) { set(0x24, 0x01 + i, body[i]); sum += body[i]; }
    set(0x24, 0x00, (uint8_t)(0u - sum)); set(0x22, 0x19, 0x40);
    set(0x03, 0x35, 0x20); set(0x03, 0x38, 0x22);       /* HDMI mode, lock status */
}
static void ev_sink(unsigned page)          /* a 1080p HDMI sink on Tx A (0x60) or Tx B (0x70) */
{
    uint8_t e[256] = { 0 };
    static const uint8_t dtd[18] = { 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0xc4, 0x8e, 0x21, 0x00, 0x00, 0x1e };
    static const uint8_t cea[15] = { 0x43, 0x10, 0x04, 0x01,  0x23, 0x09, 0x07, 0x07,  0x66, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x78 };   /* video, audio, HDMI vendor block */
    memcpy(e, "\x00\xff\xff\xff\xff\xff\xff\x00\x1e\x6d", 10); e[0x12] = 1; e[0x13] = 3; e[0x14] = 0x80;
    memcpy(e + 0x36, dtd, 18); e[0x7e] = 1;
    unsigned s = 0; for (unsigned i = 0; i < 0x7f; i++) s += e[i]; e[0x7f] = (uint8_t)(0u - s);
    memcpy(e + 0x80, "\x02\x03\x13\x70", 4); memcpy(e + 0x84, cea, 15);
    s = 0; for (unsigned i = 0x80; i < 0xff; i++) s += e[i]; e[0xff] = (uint8_t)(0u - s);
    set(page, 0xf1, 0x01); set(page, 0xc5, 0x10); set(page, 0xc8, 0x04); set(page, 0x1c, 0x01); set(page, 0x1d, 0x0f);
    for (unsigned i = 0; i < 256; i++) set(page + 1, i, e[i]);
    set(0x02, 0x8c, 0x81); set(0x02, 0x8d, 0x00); set(0x02, 0x8e, 0x01); set(0x02, 0x8f, 0x00);
    set(0x02, 0x90, 0x02); set(0x02, 0x91, 0xe6); set(0x02, 0x92, 0x00); set(0x02, 0x9b, 0x08);
}
#define CHECK(cond, what) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", what); fails++; } } while (0)

int main(void)
{
    static gsv_system sys; int fails = 0;
    set(0x02, 0x9c, 0x20); set(0x02, 0x91, 0x90);       /* Rx PLL calibrated */
    gsv_platform_init(&pf);
    sys.scaler_txb = 1;
    sys.passthrough_txa = 1;
    gsv_system_init(&sys);
    CHECK(sys.txa.idx == 0xff && (get(0x01, 0x09) & 0x03) == 2, "output A is not set to pass-through after init");
    CHECK(sys.txb.idx == 0 && (get(0x01, 0x09) & 0x30) == 0, "output B is not on the Tx core after init");
    for (int t = 0; t < 400; t++) {
        if (t == 1) set(0x03, 0x10, 0x02);              /* +5 V from a source */
        if (t == 55) ev_lock_4k60();
        if (t == 60) ev_sink(0x60);
        if (t == 64) ev_sink(0x70);
        gsv_fsm_run();
        gsv_app_route_update(&sys);
    }
    CHECK(sys.rx0.rx->locked == 1 && sys.rx0.rx->stable == 1, "input not locked and stable");
    CHECK(sys.rx0.video->vic == 0x61, "input VIC is not 97 (2160p60)");
    CHECK(*sys.txa.tx_state == TXPLUG_STABLE && sys.txa.status->tmds_locked == 1, "output A (TV, pass-through) not running");
    CHECK(sys.txa.idx == 0xff && (get(0x01, 0x09) & 0x03) == 2, "output A left pass-through mode");
    CHECK(*sys.txb.tx_state == TXPLUG_STABLE && sys.txb.status->tmds_locked == 1, "output B (capture) not running");
    CHECK(sys.txb.source == &sys.scaler && sys.scaler.source == &sys.rx0, "output B is not routed through the scaler");
    CHECK((get(0x01, 0x05) & 7) == 2 && (get(0x01, 0x04) & 3) == 0, "Tx core source is not the scaler fed by the input");
    CHECK(get(0x12, 0x66) == 0x61 && (get(0x01, 0x01) & 7) == 3, "scaler not programmed for 2160p60 RGB");
    CHECK(sys.scaler.scaler->u2 == 0x10 && get(0x42, 0x26) == 0x10, "output B's AVI infoframe does not say 1080p60 (VIC 16)");
    CHECK(sys.txa.status->hdmi_mode == 1 && sys.txb.status->hdmi_mode == 1, "an output is not in HDMI mode");
    CHECK(sys.txa.video->mute == 0 && sys.txb.video->mute == 0, "an output is muted");
    CHECK(sys.txa.video->pixclk == 594, "output A does not carry the input's 594 MHz");
    CHECK(bad_dev == 0, "transfers addressed to a device other than 0xB0");
    if (fails) return 1;
    printf("ok: GSV2001 board set-up: output A pass-through, output B through the scaler (VIC 97 -> 16), %d writes\n", writes);
    return 0;
}
