/* HDMI Light v3 oneboard (revision A): the board's pin map and the pieces that share it.
 * tools/check_oneboard_pins.py checks every ONEBOARD_PIN_* below against the board's netlist (hardware/gen/design.py),
 * so the two cannot drift apart. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "ms2130.h"

/* GSV2001 splitter */
#define ONEBOARD_PIN_GSV_SDA      40
#define ONEBOARD_PIN_GSV_SCL      39
#define ONEBOARD_PIN_GSV_RESETB   38     /* high = run; R24 holds it low until driven */
#define ONEBOARD_PIN_GSV_INT      23     /* not used: the SDK polls */
#define ONEBOARD_PIN_GSV_CSB      24     /* R27 pulls it low, and the chip answers on I2C that way; left as an input */
/* CEC: one bus across both connectors, listened to through 1M, pulled low through Q1 */
#define ONEBOARD_PIN_CEC_RX       22
#define ONEBOARD_PIN_CEC_TX       21     /* high = bus low */
#define ONEBOARD_PIN_STATUS_LED   20     /* D6, high = on */
/* MS2130 capture chip */
#define ONEBOARD_PIN_MS_PWR_EN    25     /* R10 pulls it high (1.1 V core on); drive low to switch the core off */
#define ONEBOARD_PIN_MS_DET       7      /* through R58 to the MS2130's GPIO0; floating = demo-board wiring (status LED) */
#define ONEBOARD_PIN_FL_CS        5      /* the MS2130's boot flash through R45..R48: floating unless programming it */
#define ONEBOARD_PIN_FL_SCLK      4
#define ONEBOARD_PIN_FL_MOSI      3
#define ONEBOARD_PIN_FL_MISO      2
#define ONEBOARD_PIN_LED_DATA     6

/* ---- MS2130 supply (oneboard.c) ---- */
void hl_ms2130_power(bool on);        /* off = 1.1 V core off (the chip's USB drops); on = released to the pull-up */
bool hl_ms2130_powered(void);
void hl_ms2130_cycle(int off_ms);     /* off, wait, on */

/* ---- splitter (hdmi.c) ---- */
typedef struct {
    bool     found;               /* answers on I2C */
    uint8_t  i2c_addr;            /* 7-bit */
    uint32_t ticks, i2c_errors;
    uint16_t tick_ms;             /* how long the last SDK tick took */
    bool     int_level;
    /* input */
    bool     rx_5v, rx_locked, rx_hdmi, rx_encrypted;
    uint16_t rx_w, rx_h, rx_fps, rx_pixclk;
    uint8_t  rx_vic, rx_colorspace, rx_depth;
    /* outputs: A = the TV (J2), B = the MS2130 */
    struct { uint8_t plug; bool connected, hdmi, hdcp, scaled; } tx[2];
    bool     edid_from_tv;        /* the source sees a copy of the TV's EDID (else the built-in default) */
} hl_hdmi_status_t;

int       hl_hdmi_i2c_scan(char *out, size_t len);                       /* addresses that answer; returns how many, -1 before the bus is up */
esp_err_t hl_hdmi_reg(uint8_t page, uint8_t reg, uint8_t *val, bool write);   /* one GSV2001 register */
void hl_hdmi_start(void);
/* Bench: change how the splitter is set up and start it again. -1 leaves a setting. passthrough: output A (TV) is a
 * pass-through of the input instead of taking the Tx core; scale: 4K inputs are downscaled for the MS2130; tv_hdcp: 0
 * never tries HDCP towards the TV; drive: the lowest of the driver's three output-driver settings the TV output may
 * use (0 = the driver's own choice by clock). Not stored: a restart of the board goes back to the build's options. */
void hl_hdmi_setup(int passthrough, int scale, int tv_hdcp, int drive);
void hl_hdmi_get_setup(int *passthrough, int *scale, int *tv_hdcp, int *drive);
void hl_hdmi_get_status(hl_hdmi_status_t *s);
/* 256 bytes each: 0 = what the source is shown, 1 = read from the TV, 2 = read from the MS2130. Returns 256 or 0. */
size_t hl_hdmi_get_edid(int which, uint8_t *out);

/* ---- the TV's power state and CEC (cec.c) ---- */
typedef struct {
    const char *tv;               /* from its hot-plug: "on", "going off", "off", "unknown" (splitter not running) */
    const char *tv_cec;           /* what CEC traffic says: "on", "standby", "unknown" */
    bool     follow;              /* LEDs go dark while the TV is off */
    uint32_t frames, sent;        /* CEC frames heard (through the splitter), commands sent */
    char     last[56];            /* the last frame heard, hex */
} hl_cec_status_t;
void hl_cec_start(void);
void hl_cec_get(hl_cec_status_t *st);
void hl_cec_set_follow(bool follow);          /* stored in NVS */
bool hl_cec_send(const uint8_t *d, int len);  /* bench: one frame through Q1, as given */
bool hl_cec_tv_power(bool on);                /* Image View On / Standby to the TV, through Q1 */
void hl_cec_frame_in(const uint8_t *d, int len);   /* a frame the splitter's CEC engine heard (hdmi.c) */
bool hl_cec_leds_blank(void);                 /* follow is on and the TV has been off for 0.5 s */
/* hdmi.c: how long the TV output's hot-plug has been low, in ms: 0 = high (TV on), -1 = splitter not running */
int  hl_hdmi_tv_off_ms(void);
/* hdmi.c: the last "input stuck" record (a source present, no picture for 20 s), kept in NVS; returns its length */
int  hl_hdmi_incident(char *out, size_t n);

/* ---- MS2130 boot flash (msflash.c): all of these switch the MS2130's core off for the duration ---- */
esp_err_t hl_msflash_id(uint8_t id[3]);                              /* JEDEC id; ESP_ERR_NOT_FOUND if nothing answers */
esp_err_t hl_msflash_read(uint8_t *dst, size_t len);                 /* from offset 0 */
esp_err_t hl_msflash_program(const uint8_t *img, size_t len, char *msg, size_t msg_len);   /* erase, write, verify */
