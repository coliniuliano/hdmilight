#include "ms213x_proto.h"
#include <string.h>

#define SFR_P2 0xa0
#define SFR_P3 0xb0
#define SPI_OUT ((1 << 4) | (1 << 3) | (1 << 2))
#define SPI_IN  (1 << 5)

static int set8(const ms213x_io *io, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f, uint8_t g)
{
    const uint8_t r[8] = { a, b, c, d, e, f, g, 0 };
    return io->set(io->ctx, r) ? MS213X_ERR_IO : 0;
}
static int write_reg(const ms213x_io *io, uint16_t addr, uint8_t v) { return set8(io, 0xb6, (uint8_t)(addr >> 8), (uint8_t)addr, v, 0, 0, 0); }
static int write_sfr(const ms213x_io *io, uint8_t addr, uint8_t v)  { return set8(io, 0xc6, addr, v, 0, 0, 0, 0); }
static int read_sfr(const ms213x_io *io, uint8_t addr, uint8_t *v)
{
    uint8_t r[8];
    if (set8(io, 0xc5, addr, 0, 0, 0, 0, 0) || io->get(io->ctx, r)) return MS213X_ERR_IO;
    *v = r[2];
    return 0;
}

int ms213x_setup_spi(const ms213x_io *io)
{
    uint8_t v;
    if (write_reg(io, 0xf01f, 0x00)) return MS213X_ERR_IO;                 /* SPI pin mux off while the ports are set */
    if (read_sfr(io, SFR_P3, &v) || write_sfr(io, SFR_P3, (uint8_t)((v | SPI_IN) & ~SPI_OUT))) return MS213X_ERR_IO;
    if (read_sfr(io, SFR_P2, &v) || write_sfr(io, SFR_P2, (uint8_t)(v | SPI_OUT))) return MS213X_ERR_IO;
    return write_reg(io, 0xf01f, 0x10);
}

/* one 256-byte page: the chip reads it into its buffer, then hands it over eight bytes at a time */
static int read_page(const ms213x_io *io, uint32_t addr, uint8_t *buf)
{
    if (set8(io, 0xf7, 0x01, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, MS213X_PAGE >> 8, MS213X_PAGE & 0xff)) return MS213X_ERR_IO;
    for (unsigned off = 0; off < MS213X_PAGE; off += 8)
        if (set8(io, 0xf7, 0x00, 0, 0, 0, (uint8_t)(off >> 8), (uint8_t)off) || io->get(io->ctx, buf + off)) return MS213X_ERR_IO;
    return 0;
}

/* one 256-byte page: announce it, then send it six bytes per report (the last report carries four) */
static int write_page(const ms213x_io *io, uint32_t addr, const uint8_t *page)
{
    if (set8(io, 0xf8, 0x01, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, MS213X_PAGE >> 8, MS213X_PAGE & 0xff)) return MS213X_ERR_IO;
    for (unsigned off = 0; off < MS213X_PAGE; off += 6) {
        uint8_t r[8] = { 0xf8, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        unsigned n = MS213X_PAGE - off < 6 ? MS213X_PAGE - off : 6;
        memcpy(r + 2, page + off, n);
        if (io->set(io->ctx, r)) return MS213X_ERR_IO;
    }
    return 0;
}

static void say(const ms213x_io *io, const char *stage, int pct) { if (io->progress) io->progress(io->ctx, stage, pct); }

int ms213x_read(const ms213x_io *io, uint8_t *buf, uint32_t len)
{
    for (uint32_t a = 0; a < len; a += MS213X_PAGE) {
        say(io, "reading", (int)(a * 100 / len));
        if (read_page(io, a, buf + a)) return MS213X_ERR_IO;
    }
    say(io, "reading", 100);
    return 0;
}

static int page_blank(const ms213x_io *io, uint32_t addr, int *blank)
{
    uint8_t p[MS213X_PAGE];
    if (read_page(io, addr, p)) return MS213X_ERR_IO;
    *blank = 1;
    for (unsigned i = 0; i < MS213X_PAGE; i++) if (p[i] != 0xff) { *blank = 0; break; }
    return 0;
}

int ms213x_program(const ms213x_io *io, const uint8_t *img, uint32_t len, uint32_t *bad_at)
{
    if (len == 0 || len > MS213X_FLASH_SIZE) return MS213X_ERR_IO;
    say(io, "erasing", 0);
    if (set8(io, 0xfe, 0, 0, 0, 0, 0, 0)) return MS213X_ERR_IO;
    /* The tool this follows starts writing at once. Do not rely on the chip holding the request until the erase is
     * over: wait until both ends of the flash read back blank. */
    int blank = 0;
    for (int waited = 0; !blank && waited < 20000; waited += 100) {
        int a = 0, b = 0;
        io->delay_ms(io->ctx, 100);
        if (page_blank(io, 0, &a) || page_blank(io, MS213X_FLASH_SIZE - MS213X_PAGE, &b)) return MS213X_ERR_IO;
        blank = a && b;
        say(io, "erasing", waited / 200);
    }
    if (!blank) return MS213X_ERR_ERASE;
    for (uint32_t a = 0; a < len; a += MS213X_PAGE) {
        uint8_t page[MS213X_PAGE];
        uint32_t n = len - a < MS213X_PAGE ? len - a : MS213X_PAGE;
        memset(page, 0xff, sizeof page); memcpy(page, img + a, n);
        say(io, "writing", (int)(a * 100 / len));
        if (write_page(io, a, page)) return MS213X_ERR_IO;
    }
    for (uint32_t a = 0; a < len; a += MS213X_PAGE) {
        uint8_t page[MS213X_PAGE];
        uint32_t n = len - a < MS213X_PAGE ? len - a : MS213X_PAGE;
        say(io, "verifying", (int)(a * 100 / len));
        if (read_page(io, a, page)) return MS213X_ERR_IO;
        for (uint32_t i = 0; i < n; i++)
            if (page[i] != img[a + i]) { if (bad_at) *bad_at = a + i; return MS213X_ERR_VERIFY; }
    }
    say(io, "verifying", 100);
    return 0;
}
