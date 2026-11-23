/* The MS2130's boot flash (U4, SPI NOR) written from the ESP32 through the series resistors R45..R48.
 *
 * The MS2130 is the bus master of that flash whenever it runs, so its 1.1 V core is switched off first and the
 * four ESP32 pins are only driven for the duration. What the MS2130's own SPI pins do with the core off is
 * undocumented; on the oneboard they do not drive (2026-11-20: the flash answered, and a blank one was written and
 * verified this way). If they ever did, the 220R resistors would lose and the JEDEC id read 00 or ff: this code then
 * stops before erasing anything. */
#include "oneboard.h"
#include <string.h>
#include <stdio.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "msflash";
#define FL_HOST     SPI2_HOST
#define FL_HZ       1000000          /* through 220R: no need to hurry 64 KB */
#define FL_CHUNK    256
#define CMD_WREN 0x06
#define CMD_RDSR 0x05
#define CMD_WRSR 0x01
#define CMD_READ 0x03
#define CMD_PP   0x02
#define CMD_SE   0x20                /* 4 KB sector erase: every 25-series part has it */
#define CMD_RDID 0x9f

static SemaphoreHandle_t s_lock;
static spi_device_handle_t s_dev;
static uint8_t *s_tx, *s_rx;
static bool s_was_on;

static void pins_float(void)
{
    const gpio_config_t in = {
        .pin_bit_mask = (1ULL << ONEBOARD_PIN_FL_CS) | (1ULL << ONEBOARD_PIN_FL_SCLK) | (1ULL << ONEBOARD_PIN_FL_MOSI) | (1ULL << ONEBOARD_PIN_FL_MISO),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&in);
}

static esp_err_t fl_begin(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_tx) s_tx = heap_caps_malloc(FL_CHUNK + 4, MALLOC_CAP_DMA);
    if (!s_rx) s_rx = heap_caps_malloc(FL_CHUNK + 4, MALLOC_CAP_DMA);
    if (!s_tx || !s_rx) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
    s_was_on = hl_ms2130_powered();
    hl_ms2130_power(false);
    vTaskDelay(pdMS_TO_TICKS(300));                 /* R56 bleeds the 1.1 V rail in a few tens of ms */
    const spi_bus_config_t bus = {
        .mosi_io_num = ONEBOARD_PIN_FL_MOSI, .miso_io_num = ONEBOARD_PIN_FL_MISO, .sclk_io_num = ONEBOARD_PIN_FL_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = FL_CHUNK + 4,
    };
    esp_err_t err = spi_bus_initialize(FL_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        const spi_device_interface_config_t dev = { .clock_speed_hz = FL_HZ, .mode = 0, .spics_io_num = ONEBOARD_PIN_FL_CS, .queue_size = 1 };
        err = spi_bus_add_device(FL_HOST, &dev, &s_dev);
        if (err != ESP_OK) spi_bus_free(FL_HOST);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI setup: %s", esp_err_to_name(err));
        pins_float();
        if (s_was_on) hl_ms2130_power(true);
        xSemaphoreGive(s_lock);
    }
    return err;
}

static void fl_end(void)
{
    spi_bus_remove_device(s_dev); s_dev = NULL;
    spi_bus_free(FL_HOST);
    pins_float();                                    /* the MS2130 owns the bus again */
    if (s_was_on) hl_ms2130_power(true);
    xSemaphoreGive(s_lock);
}

/* one chip-select low period: n bytes out of s_tx, n bytes into s_rx */
static esp_err_t xfer(size_t n)
{
    spi_transaction_t t = { .length = n * 8, .tx_buffer = s_tx, .rx_buffer = s_rx };
    return spi_device_polling_transmit(s_dev, &t);
}
static esp_err_t cmd1(uint8_t c) { s_tx[0] = c; return xfer(1); }
static esp_err_t status(uint8_t *sr) { s_tx[0] = CMD_RDSR; s_tx[1] = 0; esp_err_t e = xfer(2); *sr = s_rx[1]; return e; }
static void set_addr(uint8_t c, uint32_t a) { s_tx[0] = c; s_tx[1] = (uint8_t)(a >> 16); s_tx[2] = (uint8_t)(a >> 8); s_tx[3] = (uint8_t)a; }

static esp_err_t wait_idle(int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 2) {
        uint8_t sr;
        if (status(&sr) != ESP_OK) return ESP_FAIL;
        if (!(sr & 1)) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t read_id(uint8_t id[3])
{
    memset(s_tx, 0, 4); s_tx[0] = CMD_RDID;
    esp_err_t e = xfer(4);
    if (e != ESP_OK) return e;
    memcpy(id, s_rx + 1, 3);
    bool none = (id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00) || (id[0] == 0xff && id[1] == 0xff && id[2] == 0xff);
    return none ? ESP_ERR_NOT_FOUND : ESP_OK;
}

static esp_err_t read_block(uint32_t addr, uint8_t *dst, size_t n)
{
    memset(s_tx, 0, n + 4); set_addr(CMD_READ, addr);
    esp_err_t e = xfer(n + 4);
    if (e == ESP_OK) memcpy(dst, s_rx + 4, n);
    return e;
}

esp_err_t hl_msflash_id(uint8_t id[3])
{
    esp_err_t e = fl_begin();
    if (e != ESP_OK) return e;
    e = read_id(id);
    fl_end();
    ESP_LOGI(TAG, "JEDEC id %02x %02x %02x%s", id[0], id[1], id[2], e == ESP_OK ? "" : " (no flash answering)");
    return e;
}

esp_err_t hl_msflash_read(uint8_t *dst, size_t len)
{
    esp_err_t e = fl_begin();
    if (e != ESP_OK) return e;
    uint8_t id[3];
    e = read_id(id);
    for (size_t off = 0; e == ESP_OK && off < len; off += FL_CHUNK)
        e = read_block((uint32_t)off, dst + off, len - off < FL_CHUNK ? len - off : FL_CHUNK);
    fl_end();
    return e;
}

esp_err_t hl_msflash_program(const uint8_t *img, size_t len, char *msg, size_t msg_len)
{
    #define SAY(...) do { if (msg && msg_len) snprintf(msg, msg_len, __VA_ARGS__); } while (0)
    char why[96];
    if (!hl_ms2130_image_ok(img, len, why, sizeof why)) { SAY("not an MS2130 image: %s", why); return ESP_ERR_INVALID_ARG; }
    esp_err_t e = fl_begin();
    if (e != ESP_OK) { SAY("SPI setup failed: %s", esp_err_to_name(e)); return e; }
    uint8_t id[3], sr = 0;
    e = read_id(id);
    if (e != ESP_OK) {
        SAY("no flash answering (id %02x %02x %02x): the MS2130 may be driving the bus with its core off; nothing was erased", id[0], id[1], id[2]);
        goto out;
    }
    ESP_LOGW(TAG, "programming %u bytes into flash %02x %02x %02x", (unsigned)len, id[0], id[1], id[2]);
    if ((e = status(&sr)) != ESP_OK) { SAY("status read failed"); goto out; }
    if (sr & 0x7c) {                                 /* block-protect bits set: clear them */
        cmd1(CMD_WREN); s_tx[0] = CMD_WRSR; s_tx[1] = 0; xfer(2);
        if ((e = wait_idle(200)) != ESP_OK) { SAY("could not clear the flash's write protection (status %02x)", sr); goto out; }
    }
    for (uint32_t a = 0; a < len; a += 4096) {
        cmd1(CMD_WREN); set_addr(CMD_SE, a); xfer(4);
        if ((e = wait_idle(3000)) != ESP_OK) { SAY("sector erase at 0x%05x: %s", (unsigned)a, esp_err_to_name(e)); goto out; }
    }
    for (uint32_t a = 0; a < len; a += FL_CHUNK) {
        size_t n = len - a < FL_CHUNK ? len - a : FL_CHUNK;
        bool blank = true;
        for (size_t i = 0; i < n; i++) if (img[a + i] != 0xff) { blank = false; break; }
        if (blank) continue;
        cmd1(CMD_WREN); set_addr(CMD_PP, a); memcpy(s_tx + 4, img + a, n); xfer(n + 4);
        if ((e = wait_idle(200)) != ESP_OK) { SAY("page program at 0x%05x: %s", (unsigned)a, esp_err_to_name(e)); goto out; }
    }
    for (uint32_t a = 0; a < len; a += FL_CHUNK) {
        size_t n = len - a < FL_CHUNK ? len - a : FL_CHUNK;
        uint8_t back[FL_CHUNK];
        if ((e = read_block(a, back, n)) != ESP_OK) { SAY("read-back at 0x%05x failed", (unsigned)a); goto out; }
        for (size_t i = 0; i < n; i++)
            if (back[i] != img[a + i]) {
                SAY("verify failed at 0x%05x: wrote %02x, read %02x", (unsigned)(a + i), img[a + i], back[i]);
                e = ESP_ERR_INVALID_CRC; goto out;
            }
    }
    SAY("wrote and verified %u bytes (flash %02x %02x %02x; %s)", (unsigned)len, id[0], id[1], id[2], why);
out:
    fl_end();
    if (e == ESP_OK) ESP_LOGW(TAG, "%s", msg ? msg : "done"); else ESP_LOGE(TAG, "%s", msg ? msg : "failed");
    return e;
    #undef SAY
}
