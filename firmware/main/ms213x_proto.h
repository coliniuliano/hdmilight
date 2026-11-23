/* The MS2130's flash access protocol over its HID interface: 8-byte feature reports (SET_REPORT / GET_REPORT,
 * report 1). The command set is the one steve-m/ms213x_flash uses (MIT), written out again here against a
 * transport, so the same code runs on the ESP32's USB host (ms2130_usb.c) and against a model of the chip on the
 * host (tools/ms213x_proto_test). It works with the chip's ROM (534d:2130, blank flash) and with a running
 * firmware (345f:2130). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MS213X_PAGE        256
#define MS213X_FLASH_SIZE  65536

typedef struct {
    int  (*set)(void *ctx, const uint8_t rep[8]);     /* send one feature report; 0 = ok */
    int  (*get)(void *ctx, uint8_t rep[8]);           /* fetch one feature report; 0 = ok */
    void (*delay_ms)(void *ctx, unsigned ms);
    void (*progress)(void *ctx, const char *stage, int pct);   /* may be NULL */
    void *ctx;
} ms213x_io;

enum { MS213X_OK = 0, MS213X_ERR_IO = -1, MS213X_ERR_ERASE = -2, MS213X_ERR_VERIFY = -3 };

int ms213x_setup_spi(const ms213x_io *io);                                 /* route the chip's SPI pins to the flash */
int ms213x_read(const ms213x_io *io, uint8_t *buf, uint32_t len);          /* from offset 0; len is rounded up to pages: buf must hold that */
/* erase everything, write `len` bytes (the last page is padded with ff) and read them back; *bad_at = first
 * differing offset on MS213X_ERR_VERIFY */
int ms213x_program(const ms213x_io *io, const uint8_t *img, uint32_t len, uint32_t *bad_at);
