/* Board support. Dev kits (external HDMI splitter + USB grabber) need none. The HDMI Light v3 oneboard (revision A,
 * CONFIG_HL_BOARD_ONEBOARD) carries the splitter (GSV2001) and the grabber (MS2130), and the ESP32-S31 runs both:
 * oneboard.c (pins, MS2130 supply, HTTP endpoints), hdmi.c (GSV2001 over I2C), msflash.c (MS2130 boot flash). */
#pragma once
#include "sdkconfig.h"
#include "esp_err.h"

/* The LED strip's data pin where the board wires it (oneboard: IO6 through the level shifter U9; every other pin goes
 * somewhere else, to the flash lines or the splitter's reset for instance), or -1 where it is the user's choice. */
#if CONFIG_HL_BOARD_ONEBOARD
#define HL_BOARD_LED_GPIO 6
#else
#define HL_BOARD_LED_GPIO (-1)
#endif

#if CONFIG_HL_BOARD_ONEBOARD
void      hl_board_init(void);                 /* first thing in app_main: every board pin to its safe state */
void      hl_board_start(void);                /* start the splitter task; call once logging and NVS are up */
esp_err_t hl_board_register_http(void *httpd); /* /api/board, /api/edid, /api/cec, /api/ms2130, /api/ms2130/flash */
bool      hl_board_leds_blank(void);           /* the TV is known to be in standby and the LEDs are set to follow it */
bool      hl_board_console(int argc, char **argv);   /* serial console commands of this board; false = not one of them */
void      hl_board_console_help(void);
#else
#include <stdbool.h>
static inline void hl_board_init(void) {}
static inline void hl_board_start(void) {}
static inline bool hl_board_leds_blank(void) { return false; }
static inline bool hl_board_console(int argc, char **argv) { (void)argc; (void)argv; return false; }
static inline void hl_board_console_help(void) {}
#endif
