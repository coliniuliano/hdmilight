#include <string.h>
#include <stdlib.h>
#include "leds.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "leds";
static led_strip_handle_t s_strip;
static int  s_count;
static bool s_rgbw;
static int  s_brightness = 100;

esp_err_t hl_leds_init(int gpio, int count, hl_led_type_t type, hl_color_order_t order)
{
    hl_leds_deinit();
    led_strip_config_t cfg = {
        .strip_gpio_num = gpio,
        .max_leds = count,
        .flags = { .invert_out = false },
    };
    cfg.led_model = type == HL_LED_TYPE_SK6812_RGBW ? LED_MODEL_SK6812 : type == HL_LED_TYPE_WS2811 ? LED_MODEL_WS2811 : LED_MODEL_WS2812;
    s_rgbw = type == HL_LED_TYPE_SK6812_RGBW;
    static const uint8_t pos[][3] = { { 1, 0, 2 }, { 0, 1, 2 }, { 1, 2, 0 }, { 0, 2, 1 }, { 2, 0, 1 }, { 2, 1, 0 } };   /* r,g,b byte index per hl_color_order_t */
    const uint8_t *o = pos[order < 6 ? order : 0];
    cfg.color_component_format = (led_color_component_format_t){ .format = { .r_pos = o[0], .g_pos = o[1], .b_pos = o[2], .w_pos = 3, .reserved = 0, .bytes_per_color = 1, .num_components = s_rgbw ? 4 : 3 } };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        /* The largest DMA buffer the RMT driver accepts (2 x 4095 B). led_strip's default is 64 symbols, i.e. the
         * encoder ISR must refill every 40 us for the whole frame; a late ISR breaks the frame mid-strip and the
         * LEDs after the break show shifted data (the flicker seen on dark edges). 2046 symbols = 1.3 ms slack. */
        .mem_block_symbols = 2046,
        .flags = { .with_dma = true },
    };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT with DMA failed (%s), retrying without DMA", esp_err_to_name(err));
        rmt.flags.with_dma = false;
        err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    }
    if (err != ESP_OK) { ESP_LOGE(TAG, "led_strip init failed: %s", esp_err_to_name(err)); s_strip = NULL; return err; }
    s_count = count;
    led_strip_clear(s_strip);
    static const char *names[] = { "GRB", "RGB", "BRG", "RBG", "GBR", "BGR" };
    ESP_LOGI(TAG, "%d LEDs on GPIO %d, order %s%s", count, gpio, names[order < 6 ? order : 0], s_rgbw ? "W" : "");
    return ESP_OK;
}

void hl_leds_deinit(void)
{
    if (s_strip) { led_strip_clear(s_strip); led_strip_del(s_strip); s_strip = NULL; }
    s_count = 0;
}

void hl_leds_set_brightness(int pct)
{
    s_brightness = pct < 1 ? 1 : (pct > 100 ? 100 : pct);
}

static inline uint32_t scale(uint8_t v) { return ((uint32_t)v * (uint32_t)s_brightness) / 100; }

esp_err_t hl_leds_write(const uint8_t *rgb, int count)
{
    if (!s_strip) return ESP_ERR_INVALID_STATE;
    if (count > s_count) count = s_count;
    for (int i = 0; i < count; i++) {
        uint32_t r = scale(rgb[i * 3]), g = scale(rgb[i * 3 + 1]), b = scale(rgb[i * 3 + 2]);
        if (s_rgbw) {
            uint32_t w = r < g ? (r < b ? r : b) : (g < b ? g : b);   /* common component to the white die */
            led_strip_set_pixel_rgbw(s_strip, i, r - w, g - w, b - w, w);
        } else {
            led_strip_set_pixel(s_strip, i, r, g, b);
        }
    }
    return led_strip_refresh(s_strip);
}

void hl_leds_selftest(void)
{
    if (!s_strip || s_count <= 0) return;
    const uint8_t cols[3][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 } };
    uint8_t *buf = calloc((size_t)s_count * 3, 1);
    if (!buf) return;
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < s_count; i++) { buf[i * 3] = cols[c][0]; buf[i * 3 + 1] = cols[c][1]; buf[i * 3 + 2] = cols[c][2]; }
        hl_leds_write(buf, s_count);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    for (int i = 0; i < s_count; i++) {
        memset(buf, 0, (size_t)s_count * 3);
        buf[i * 3] = buf[i * 3 + 1] = buf[i * 3 + 2] = 255;
        hl_leds_write(buf, s_count);
        vTaskDelay(pdMS_TO_TICKS(1500 / s_count + 1));
    }
    memset(buf, 0, (size_t)s_count * 3);
    hl_leds_write(buf, s_count);
    free(buf);
}
