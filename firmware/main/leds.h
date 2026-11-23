/* WS281x / SK6812 output through the RMT backend of espressif/led_strip.
 * Reconfigurable at runtime (the web UI changes GPIO, count and type). */
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

esp_err_t hl_leds_init(int gpio, int count, hl_led_type_t type, hl_color_order_t order);
void      hl_leds_deinit(void);
void      hl_leds_set_brightness(int pct);                  /* 1..100, applied in hl_leds_write */
esp_err_t hl_leds_write(const uint8_t *rgb, int count);      /* blocking refresh */
void      hl_leds_selftest(void);                            /* red, green, blue, then a single-pixel chase */
