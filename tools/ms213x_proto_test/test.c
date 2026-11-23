/* Host test of firmware/main/ms213x_proto.c against a model of the MS2130's HID flash commands (as documented by
 * steve-m/ms213x_flash): the page buffer, the six-bytes-per-report write, NOR-flash programming (bits only go
 * from 1 to 0, so writing without an erase shows) and an erase that takes time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms213x_proto.h"

static struct {
    uint8_t flash[MS213X_FLASH_SIZE], buf[MS213X_PAGE + 8], resp[8];
    uint8_t p2, p3, mux;
    uint32_t waddr; int wlen, wgot;        /* write in progress */
    int erase_left;                        /* reports until a started erase has finished */
    int erase_never;                       /* the erase never completes */
    long stuck_at;                         /* a cell that stays ff whatever is written (-1 = none) */
    long fail_after;                       /* transport error after this many reports (-1 = never) */
    long sets, gets;
} m;

static void tick(void) { if (!m.erase_never && m.erase_left > 0 && --m.erase_left == 0) memset(m.flash, 0xff, sizeof m.flash); }

static int dev_set(void *ctx, const uint8_t r[8])
{
    (void)ctx; m.sets++;
    if (m.fail_after >= 0 && m.sets + m.gets > m.fail_after) return -1;
    tick();
    switch (r[0]) {
    case 0xb6: if (r[1] == 0xf0 && r[2] == 0x1f) m.mux = r[3]; break;
    case 0xc5: memset(m.resp, 0, 8); m.resp[2] = r[1] == 0xa0 ? m.p2 : r[1] == 0xb0 ? m.p3 : 0; break;
    case 0xc6: if (r[1] == 0xa0) m.p2 = r[2]; else if (r[1] == 0xb0) m.p3 = r[2]; break;
    case 0xf7:
        if (r[1] == 1) {
            uint32_t a = ((uint32_t)r[2] << 16) | (r[3] << 8) | r[4]; int n = (r[5] << 8) | r[6];
            if (m.mux != 0x10 || n != MS213X_PAGE || a + n > sizeof m.flash) { fprintf(stderr, "model: bad read request\n"); exit(2); }
            /* during an erase the flash is busy: the chip returns rubbish */
            if (m.erase_left > 0) memset(m.buf, 0x5a, MS213X_PAGE); else memcpy(m.buf, m.flash + a, MS213X_PAGE);
        } else { int off = (r[5] << 8) | r[6]; memcpy(m.resp, m.buf + off, 8); }
        break;
    case 0xf8:
        if (r[1] == 1) {
            m.waddr = ((uint32_t)r[2] << 16) | (r[3] << 8) | r[4]; m.wlen = (r[5] << 8) | r[6]; m.wgot = 0;
            if (m.mux != 0x10 || m.wlen != MS213X_PAGE || m.erase_left > 0) { fprintf(stderr, "model: write started while the erase was still running\n"); exit(2); }
        } else {
            for (int i = 0; i < 6 && m.wgot < m.wlen; i++, m.wgot++)
                if ((long)(m.waddr + m.wgot) != m.stuck_at) m.flash[m.waddr + m.wgot] &= r[2 + i];   /* NOR: bits only go 1 -> 0 */
        }
        break;
    case 0xfe: m.erase_left = 400; break;
    default: fprintf(stderr, "model: unknown command %02x\n", r[0]); exit(2);
    }
    return 0;
}
static int dev_get(void *ctx, uint8_t r[8])
{
    (void)ctx; m.gets++;
    if (m.fail_after >= 0 && m.sets + m.gets > m.fail_after) return -1;
    tick(); memcpy(r, m.resp, 8); return 0;
}
static void dev_delay(void *ctx, unsigned ms) { (void)ctx; (void)ms; }

static int fails;
static void expect(const char *what, int ok) { printf("%-66s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

int main(void)
{
    static uint8_t img[MS213X_FLASH_SIZE], back[MS213X_FLASH_SIZE];
    const ms213x_io io = { dev_set, dev_get, dev_delay, NULL, NULL };
    srand(1);
    for (size_t i = 0; i < sizeof img; i++) img[i] = (uint8_t)rand();
    memset(&m, 0, sizeof m); m.fail_after = -1; m.stuck_at = -1; m.p2 = 0x01; m.p3 = 0x1d;
    for (size_t i = 0; i < sizeof m.flash; i++) m.flash[i] = (uint8_t)(i * 3);          /* an old firmware in the flash */

    expect("setup_spi returns ok", ms213x_setup_spi(&io) == 0);
    expect("setup_spi: mux on, P3 bit 5 set and bits 2..4 clear, P2 bits 2..4 set", m.mux == 0x10 && m.p3 == 0x21 && m.p2 == 0x1d);
    expect("read 64 KB returns ok", ms213x_read(&io, back, sizeof back) == 0);
    int same = 1; for (size_t i = 0; i < sizeof back; i++) if (back[i] != (uint8_t)(i * 3)) same = 0;
    expect("read returns the flash contents", same);

    uint32_t bad = 0;
    expect("program a full 64 KB image over an old one (waits for the erase)", ms213x_program(&io, img, sizeof img, &bad) == 0);
    expect("flash equals the image", memcmp(m.flash, img, sizeof img) == 0);

    const uint32_t len = 0x30 + 53167 + 4;                    /* the real image's used length: not a multiple of 256 or 6 */
    expect("program a 53,219-byte image", ms213x_program(&io, img, len, &bad) == 0);
    expect("flash holds it", memcmp(m.flash, img, len) == 0);
    int tail = 1; for (uint32_t i = len; i < sizeof m.flash; i++) if (m.flash[i] != 0xff) tail = 0;
    expect("everything after it is blank", tail);

    m.stuck_at = 40000; img[40000] = 0x12; bad = 0;
    expect("a cell that does not take its data fails verification", ms213x_program(&io, img, sizeof img, &bad) == MS213X_ERR_VERIFY);
    expect("...and the failing offset is reported", bad == 40000);
    m.stuck_at = -1;

    m.sets = m.gets = 0; m.fail_after = 3000;
    expect("a USB error part-way is reported", ms213x_program(&io, img, sizeof img, &bad) == MS213X_ERR_IO);
    m.fail_after = -1;

    m.erase_never = 1;
    expect("an erase that never finishes is reported, nothing is written", ms213x_program(&io, img, sizeof img, &bad) == MS213X_ERR_ERASE);
    m.erase_never = 0;

    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
