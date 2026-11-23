/* Platform hooks the SDK needs: the application installs these (gsv_platform_init). The device descriptor
 * packs page (bits 0-7), 8-bit I2C address (bits 8-15) and a 16-bit-register flag (bit 16): GSV_PAGE(p). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define GSV_I2C_ADDR     0xB0u                       /* 8-bit form; 0x58 seven-bit */
#define GSV_DEV(page)    (0x00010000u | (GSV_I2C_ADDR << 8) | ((page) & 0xffu))
#define GSV_PAGE(page)   GSV_DEV(page)
#define GSV_DEV_PAGE(d)  ((d) & 0xffu)
#define GSV_DEV_ADDR(d)  (((d) >> 8) & 0xffu)

/* I2C transfer: dev8 = 8-bit device address, reg16 = (page << 8) | register, sent high byte first. 0 = ok. */
typedef int (*gsv_i2c_fn)(uint8_t dev8, uint16_t reg16, uint8_t *data, uint16_t len);

typedef struct {
    gsv_i2c_fn read;
    gsv_i2c_fn write;
} gsv_platform;

void gsv_platform_init(const gsv_platform *p);
