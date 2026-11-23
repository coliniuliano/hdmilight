/* HDMI Light v3 oneboard (revision A): pin states, the MS2130's supply and plug-detect line, and the bring-up endpoints.
 *
 *   GET  /api/board                     splitter and capture-chip state as JSON
 *   GET  /api/edid?which=source|tv|ms2130   256 bytes: what the source is shown / what each sink reported
 *   POST /api/cec?tv=on|off&follow=0|1  switch the TV on or off over CEC / LEDs dark while the TV is off (cec.c);
 *                                       bench: &send=<hex> puts one frame on the bus
 *   POST /api/ms2130?power=on|off|cycle the MS2130's 1.1 V core supply (off also drops it from USB)
 *   GET  /api/ms2130/flash?id=1         JEDEC id of the MS2130's boot flash as JSON
 *   GET  /api/ms2130/flash              the first 64 KB of that flash
 *   POST /api/ms2130/flash              body = an MS2130 image (checked, written, verified)
 *   GET  /api/reg?page=20&reg=0[&count=N]   read 1 to 256 GSV2001 registers (hex page and register) as JSON
 *   POST /api/reg?page=20&reg=46&val=84     write one GSV2001 register (the console's 'reg')
 *   POST /api/hdmi?passthrough=0|1&scale=0|1&tvhdcp=0|1&drive=0..2   bench: set the splitter up differently and restart it
 *   GET  /api/i2c                       the addresses answering on the splitter's I2C bus (the console's 'i2c')
 *   GET  /api/incident                  the last time the input stayed without a picture with a source present
 * The three SPI flash calls switch the MS2130 off while they run; tools/ms2130_flash.sh wraps them. The serial
 * console has the same three ('flashid', 'flash read', 'flash begin/commit'), driven by tools/ms2130_serial.py.
 * ms2130_usb.c adds /api/ms2130/usb for the same flash over USB, registered from here. */
#include "board.h"
#include "oneboard.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "capture.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";
_Static_assert(HL_BOARD_LED_GPIO == ONEBOARD_PIN_LED_DATA, "board.h and oneboard.h disagree about the LED strip's pin");
static bool s_ms_on = true;

static void pins(uint64_t mask, gpio_mode_t mode)
{
    const gpio_config_t c = { .pin_bit_mask = mask, .mode = mode, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE };
    gpio_config(&c);
}
#define PIN(n) (1ULL << (n))

void hl_board_init(void)
{
    /* Levels first, then directions, so nothing glitches the wrong way. */
    gpio_set_level(ONEBOARD_PIN_GSV_RESETB, 0);          /* the splitter stays in reset until its task starts */
    gpio_set_level(ONEBOARD_PIN_CEC_TX, 0);              /* Q1 off: this board never holds the CEC bus low by accident */
    gpio_set_level(ONEBOARD_PIN_STATUS_LED, 0);
    pins(PIN(ONEBOARD_PIN_GSV_RESETB) | PIN(ONEBOARD_PIN_CEC_TX) | PIN(ONEBOARD_PIN_STATUS_LED), GPIO_MODE_OUTPUT);
    /* Inputs with no pulls: the board has its own (R27 on CSB, R10 on MS_PWR_EN, R31/D5 on the MS2130's GPIO0),
     * and the flash lines belong to the MS2130 whenever it runs. */
    pins(PIN(ONEBOARD_PIN_GSV_INT) | PIN(ONEBOARD_PIN_GSV_CSB) | PIN(ONEBOARD_PIN_CEC_RX) | PIN(ONEBOARD_PIN_MS_DET) |
         PIN(ONEBOARD_PIN_FL_CS) | PIN(ONEBOARD_PIN_FL_SCLK) | PIN(ONEBOARD_PIN_FL_MOSI) | PIN(ONEBOARD_PIN_FL_MISO), GPIO_MODE_INPUT);
    /* MS2130 core supply: open drain, released = on through R10 */
    gpio_set_level(ONEBOARD_PIN_MS_PWR_EN, 1);
    pins(PIN(ONEBOARD_PIN_MS_PWR_EN), GPIO_MODE_INPUT_OUTPUT_OD);
}

void hl_ms2130_power(bool on)
{
    if (on != s_ms_on) ESP_LOGW(TAG, "MS2130 core supply %s", on ? "on" : "off");
    gpio_set_level(ONEBOARD_PIN_MS_PWR_EN, on ? 1 : 0);
    s_ms_on = on;
}
bool hl_ms2130_powered(void) { return s_ms_on; }
void hl_ms2130_cycle(int off_ms) { hl_ms2130_power(false); vTaskDelay(pdMS_TO_TICKS(off_ms)); hl_ms2130_power(true); }

void hl_board_start(void)
{
    ESP_LOGI(TAG, "HDMI Light v3 oneboard (revision A): MS_PWR_EN reads %d (1 = MS2130 core on), GSV INT reads %d",
             gpio_get_level(ONEBOARD_PIN_MS_PWR_EN), gpio_get_level(ONEBOARD_PIN_GSV_INT));
    hl_hdmi_start();
    hl_cec_start();
}

bool hl_board_leds_blank(void) { return hl_cec_leds_blank(); }

/* ---- the capture chip's flash over the serial console (tools/ms2130_serial.py) -------------------------- *
 * The console takes printable lines of at most 95 characters, so the image travels as hex:
 *   flash read                 -> "FL <offset> <32 bytes>" per line, then "FL end <bytes> <crc32>"
 *   flash begin <bytes>        -> "FL ready <bytes>"
 *   fd <offset> <hex bytes>    -> "FL ok <next offset>"   (in order; repeating a block is harmless)
 *   flash commit <crc32>       -> "FL done <message>" after the image was checked, written and verified
 * Every failure is "FL error <why>". Nothing is erased unless the whole image arrived with the right checksum. */
static uint8_t *s_fl_buf;
static size_t s_fl_len, s_fl_got;

static uint32_t fl_crc32(const uint8_t *p, size_t n)       /* zlib's CRC-32 */
{
    uint32_t c = 0xffffffffu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u))); }
    return ~c;
}

static int fl_hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void fl_drop(void) { free(s_fl_buf); s_fl_buf = NULL; s_fl_len = s_fl_got = 0; }

static void con_flash_read(void)
{
    uint8_t *b = heap_caps_malloc(MS_FLASH_IMAGE_MAX, MALLOC_CAP_SPIRAM);
    if (!b) { printf("FL error out of memory\n"); return; }
    esp_err_t e = hl_msflash_read(b, MS_FLASH_IMAGE_MAX);
    if (e != ESP_OK) printf("FL error %s\n", e == ESP_ERR_NOT_FOUND ? "no flash answering" : esp_err_to_name(e));
    else {
        char line[80];
        for (size_t off = 0; off < MS_FLASH_IMAGE_MAX; off += 32) {
            int n = snprintf(line, sizeof line, "FL %06x ", (unsigned)off);
            for (int i = 0; i < 32; i++) n += snprintf(line + n, sizeof line - n, "%02x", b[off + i]);
            printf("%s\n", line);
            if ((off & 0x1ff) == 0x1e0) vTaskDelay(1);     /* let the other tasks on this core run */
        }
        printf("FL end %u %08lx\n", (unsigned)MS_FLASH_IMAGE_MAX, (unsigned long)fl_crc32(b, MS_FLASH_IMAGE_MAX));
    }
    free(b);
}

static void con_flash_data(const char *off_s, const char *hex)
{
    size_t off = strtoul(off_s, NULL, 16), chars = strlen(hex), n = chars / 2;
    uint8_t tmp[48];
    if (!s_fl_buf) { printf("FL error no transfer open: 'flash begin <bytes>' first\n"); return; }
    if ((chars & 1) || !n || n > sizeof tmp || off > s_fl_got || off + n > s_fl_len) {
        printf("FL error bad block at %06x (expecting %06x of %06x)\n", (unsigned)off, (unsigned)s_fl_got, (unsigned)s_fl_len); return;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = fl_hex(hex[2 * i]), lo = fl_hex(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) { printf("FL error not hex at %06x\n", (unsigned)off); return; }
        tmp[i] = (uint8_t)(hi << 4 | lo);
    }
    memcpy(s_fl_buf + off, tmp, n);
    if (off + n > s_fl_got) s_fl_got = off + n;
    printf("FL ok %06x\n", (unsigned)s_fl_got);
}

static void con_flash(int argc, char **argv)
{
    if (!strcmp(argv[1], "read")) con_flash_read();
    else if (!strcmp(argv[1], "begin") && argc >= 3) {
        size_t len = strtoul(argv[2], NULL, 10);
        fl_drop();
        if (!len || len > MS_FLASH_IMAGE_MAX) { printf("FL error the length must be 1 to %d bytes\n", MS_FLASH_IMAGE_MAX); return; }
        if (!(s_fl_buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM))) { printf("FL error out of memory\n"); return; }
        s_fl_len = len;
        printf("FL ready %u\n", (unsigned)len);
    } else if (!strcmp(argv[1], "commit") && argc >= 3) {
        uint32_t want = (uint32_t)strtoul(argv[2], NULL, 16);
        if (!s_fl_buf) printf("FL error no transfer open: 'flash begin <bytes>' first\n");
        else if (s_fl_got != s_fl_len) printf("FL error only %u of %u bytes arrived: nothing was written\n", (unsigned)s_fl_got, (unsigned)s_fl_len);
        else if (fl_crc32(s_fl_buf, s_fl_len) != want) { printf("FL error checksum mismatch: nothing was written\n"); fl_drop(); }
        else {
            char msg[160] = "";
            esp_err_t e = hl_msflash_program(s_fl_buf, s_fl_len, msg, sizeof msg);
            printf("FL %s %s\n", e == ESP_OK ? "done" : "error", msg);
            if (e == ESP_OK) fl_drop();                    /* kept after a failure, so 'flash commit' can be repeated */
        }
    } else if (!strcmp(argv[1], "abort")) { fl_drop(); printf("FL ok 000000\n"); }
    else printf("flash read | begin <bytes> | commit <crc32> | abort   (use tools/ms2130_serial.py)\n");
}

/* ---- serial console ------------------------------------------------------------------------------------ */
void hl_board_console_help(void)
{
    printf("oneboard commands:\n"
           "  usb                    USB devices on the host port (the capture chip)\n"
           "  msusb read|status      read the capture chip's flash over USB / state of that job\n"
           "  board                  splitter, input, outputs, capture chip, CEC\n"
           "  pins                   levels of the board's control pins\n"
           "  i2c                    scan the splitter's I2C bus\n"
           "  reg <page> <reg> [val] read or write one GSV2001 register (hex)\n"
           "  hdmi [pt scale tvhdcp drive]  bench: TV output as pass-through (1) or on the Tx core (0), 4K scaled for\n"
           "                         capture, HDCP towards the TV, lowest driver setting (0 to 2) for the TV output;\n"
           "                         restarts the splitter. Not stored\n"
           "  ms on|off|cycle        the capture chip's core supply\n"
           "  flashid                JEDEC id of the capture chip's flash over SPI (switches it off meanwhile)\n"
           "  flash read|begin|commit  the capture chip's flash over SPI, as hex lines: tools/ms2130_serial.py\n"
           "  tv on|off              switch the TV on or off over CEC\n"
           "  tv follow on|off       LEDs fade out while the TV is off\n");
}

bool hl_board_console(int argc, char **argv)
{
    if (!strcmp(argv[0], "board")) {
        hl_hdmi_status_t s; hl_hdmi_get_status(&s);
        hl_cec_status_t c; hl_cec_get(&c);
        if (!s.found) printf("splitter: NOT FOUND on I2C (try 'i2c')\n");
        else {
            printf("splitter: running, tick %u ms, %lu I2C errors\n", s.tick_ms, (unsigned long)s.i2c_errors);
            printf("input: %s", !s.rx_5v ? "no source\n" : !s.rx_locked ? "source present, not locked\n" : "");
            if (s.rx_5v && s.rx_locked) printf("%ux%u, VIC %u%s\n", s.rx_w, s.rx_h, s.rx_vic, s.rx_encrypted ? ", HDCP" : "");
            static const char *nm[2] = { "TV", "capture chip" };
            for (int i = 0; i < 2; i++)
                printf("output to %s: %s%s%s (state %u)\n", nm[i], s.tx[i].connected ? "connected" : "not connected", s.tx[i].scaled ? ", downscaled" : "",
                       s.tx[i].hdcp ? ", HDCP" : "", s.tx[i].plug);
            printf("EDID shown to the source: %s\n", s.edid_from_tv ? "the TV's" : "built-in default");
        }
        printf("capture chip: core %s, USB %s\n", hl_ms2130_powered() ? "on" : "off", hl_capture_connected() ? "streaming" : "not streaming");
        printf("TV: %s (CEC says %s), LEDs %s it; %lu CEC frames heard, last %s; %lu commands sent\n", c.tv, c.tv_cec,
               c.follow ? "follow" : "ignore", (unsigned long)c.frames, c.last[0] ? c.last : "-", (unsigned long)c.sent);
    } else if (!strcmp(argv[0], "usb")) {
        char buf[256]; int n = hl_msusb_list(buf, sizeof buf);
        if (n < 0) printf("busy (a flash job is running)\n"); else if (n == 0) printf("no USB device\n"); else printf("%s", buf);
    } else if (!strcmp(argv[0], "msusb") && argc >= 2 && !strcmp(argv[1], "read")) {
        esp_err_t e = hl_msusb_start(NULL, 0);
        printf(e == ESP_OK ? "reading: 'msusb status' shows progress; fetch it with tools/ms2130_flash.sh usb-status, or /api/ms2130/usb/dump\n" : "not started: %s\n", esp_err_to_name(e));
    } else if (!strcmp(argv[0], "msusb")) {
        hl_msusb_job_t j; hl_msusb_status(&j);
        static const char *st[] = { "idle", "running", "done", "failed" };
        printf("%s %s %d %%  %s\n", st[j.state], j.stage, j.pct, j.msg);
    } else if (!strcmp(argv[0], "pins")) {
        /* inputs only: RESETB is an output-only pin, which always reads back 0 */
        printf("GSV INT %d, CSB %d | MS_PWR_EN %d, MS GPIO0 %d | CEC bus %d\n", gpio_get_level(ONEBOARD_PIN_GSV_INT), gpio_get_level(ONEBOARD_PIN_GSV_CSB),
               gpio_get_level(ONEBOARD_PIN_MS_PWR_EN), gpio_get_level(ONEBOARD_PIN_MS_DET), gpio_get_level(ONEBOARD_PIN_CEC_RX));
    } else if (!strcmp(argv[0], "i2c")) {
        char buf[200]; int n = hl_hdmi_i2c_scan(buf, sizeof buf);
        if (n < 0) printf("the I2C bus is not up yet\n"); else if (n == 0) printf("nothing answers\n"); else printf("%d answering: %s\n", n, buf);
    } else if (!strcmp(argv[0], "reg") && argc >= 3) {
        uint8_t page = (uint8_t)strtoul(argv[1], NULL, 16), reg = (uint8_t)strtoul(argv[2], NULL, 16), val = argc >= 4 ? (uint8_t)strtoul(argv[3], NULL, 16) : 0;
        esp_err_t e = hl_hdmi_reg(page, reg, &val, argc >= 4);
        if (e != ESP_OK) printf("failed: %s\n", esp_err_to_name(e)); else printf("page %02x reg %02x %s %02x\n", page, reg, argc >= 4 ? "<-" : "=", val);
    } else if (!strcmp(argv[0], "ms") && argc >= 2) {
        if (!strcmp(argv[1], "on")) hl_ms2130_power(true); else if (!strcmp(argv[1], "off")) hl_ms2130_power(false); else hl_ms2130_cycle(500);
        printf("capture chip core %s\n", hl_ms2130_powered() ? "on" : "off");
    } else if (!strcmp(argv[0], "fd") && argc >= 3) {
        con_flash_data(argv[1], argv[2]);
    } else if (!strcmp(argv[0], "flash") && argc >= 2) {
        con_flash(argc, argv);
    } else if (!strcmp(argv[0], "hdmi")) {
        int pt = argc >= 2 ? atoi(argv[1]) : -1, sc = argc >= 3 ? atoi(argv[2]) : -1, th = argc >= 4 ? atoi(argv[3]) : -1,
            dr = argc >= 5 ? atoi(argv[4]) : -1;
        if (argc >= 2) hl_hdmi_setup(pt, sc, th, dr);
        hl_hdmi_get_setup(&pt, &sc, &th, &dr);
        printf("splitter set-up: passthrough %d, scale %d, tvhdcp %d, drive %d%s\n", pt, sc, th, dr, argc >= 2 ? " (restarting it)" : "");
    } else if (!strcmp(argv[0], "flashid")) {
        uint8_t id[3] = { 0 }; esp_err_t e = hl_msflash_id(id);
        printf("JEDEC id %02x %02x %02x%s\n", id[0], id[1], id[2], e == ESP_OK ? "" : ": no flash answering");
    } else if (!strcmp(argv[0], "tv") && argc >= 2) {
        if (!strcmp(argv[1], "on") || !strcmp(argv[1], "off")) hl_cec_tv_power(!strcmp(argv[1], "on"));
        else if (!strcmp(argv[1], "follow") && argc >= 3) hl_cec_set_follow(!strcmp(argv[2], "on"));
        else return false;
        hl_cec_status_t c; hl_cec_get(&c);
        printf("TV %s, LEDs %s it\n", c.tv, c.follow ? "follow" : "ignore");
    } else return false;
    return true;
}

/* ---- HTTP ---------------------------------------------------------------------------------------------- */
static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

static bool query(httpd_req_t *req, const char *key, char *val, size_t n)
{
    char q[64];
    return httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, key, val, n) == ESP_OK;
}

static esp_err_t h_board(httpd_req_t *req)
{
    hl_hdmi_status_t st; hl_hdmi_get_status(&st);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "board", "hdmilight-v3 oneboard, revision A");
    cJSON *g = cJSON_AddObjectToObject(j, "splitter");
    cJSON_AddBoolToObject(g, "found", st.found);
    cJSON_AddNumberToObject(g, "ticks", st.ticks);
    cJSON_AddNumberToObject(g, "tick_ms", st.tick_ms);
    cJSON_AddNumberToObject(g, "i2c_errors", st.i2c_errors);
    cJSON_AddBoolToObject(g, "int_pin", st.int_level);
    cJSON_AddBoolToObject(g, "edid_from_tv", st.edid_from_tv);
    cJSON *in = cJSON_AddObjectToObject(g, "input");
    cJSON_AddBoolToObject(in, "cable_5v", st.rx_5v);
    cJSON_AddBoolToObject(in, "locked", st.rx_locked);
    cJSON_AddBoolToObject(in, "hdmi", st.rx_hdmi);
    cJSON_AddBoolToObject(in, "hdcp", st.rx_encrypted);
    cJSON_AddNumberToObject(in, "width", st.rx_w);
    cJSON_AddNumberToObject(in, "height", st.rx_h);
    cJSON_AddNumberToObject(in, "frame_rate", st.rx_fps);
    cJSON_AddNumberToObject(in, "pixel_clock", st.rx_pixclk);
    cJSON_AddNumberToObject(in, "vic", st.rx_vic);
    cJSON_AddNumberToObject(in, "colorspace", st.rx_colorspace);
    cJSON_AddNumberToObject(in, "depth_code", st.rx_depth);
    static const char *names[2] = { "output_a_tv", "output_b_ms2130" };
    for (int i = 0; i < 2; i++) {
        cJSON *o = cJSON_AddObjectToObject(g, names[i]);
        cJSON_AddBoolToObject(o, "connected", st.tx[i].connected);
        cJSON_AddNumberToObject(o, "plug_state", st.tx[i].plug);
        cJSON_AddBoolToObject(o, "hdmi", st.tx[i].hdmi);
        cJSON_AddBoolToObject(o, "hdcp", st.tx[i].hdcp);
        cJSON_AddBoolToObject(o, "downscaled", st.tx[i].scaled);
    }
    cJSON *m = cJSON_AddObjectToObject(j, "ms2130");
    cJSON_AddBoolToObject(m, "core_power", hl_ms2130_powered());
    cJSON_AddBoolToObject(m, "pwr_en_pin", gpio_get_level(ONEBOARD_PIN_MS_PWR_EN));
    cJSON_AddBoolToObject(m, "usb_streaming", hl_capture_connected());
    cJSON_AddNumberToObject(m, "usb_frames", hl_capture_frames_total());
    cJSON_AddBoolToObject(m, "gpio0_pin", gpio_get_level(ONEBOARD_PIN_MS_DET));
    hl_cec_status_t cs; hl_cec_get(&cs);
    cJSON *c = cJSON_AddObjectToObject(j, "cec");
    cJSON_AddStringToObject(c, "tv", cs.tv);
    cJSON_AddStringToObject(c, "tv_cec", cs.tv_cec);
    cJSON_AddBoolToObject(c, "follow", cs.follow);
    cJSON_AddNumberToObject(c, "frames", cs.frames);
    cJSON_AddNumberToObject(c, "sent", cs.sent);
    cJSON_AddStringToObject(c, "last_frame", cs.last);
    return send_json(req, j);
}

/* GSV2001 registers over HTTP: how the 4K60 lock fix was found (gsv2001/sdk/gsv_core.c) and how the lane eye
 * figures are read. */
static bool reg_args(httpd_req_t *req, uint8_t *page, uint8_t *reg)
{
    char v[8];
    if (!query(req, "page", v, sizeof v)) return false;
    *page = (uint8_t)strtoul(v, NULL, 16);
    if (!query(req, "reg", v, sizeof v)) return false;
    *reg = (uint8_t)strtoul(v, NULL, 16);
    return true;
}

static esp_err_t h_reg_get(httpd_req_t *req)
{
    uint8_t page, reg; char v[8]; int count = 1;
    if (!reg_args(req, &page, &reg)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "page=<hex>&reg=<hex>[&count=1..256]");
    if (query(req, "count", v, sizeof v)) count = atoi(v);
    if (count < 1 || count > 256 || reg + count > 256) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "count 1..256, within the page");
    char hex[2 * 256 + 1]; esp_err_t e = ESP_OK;
    for (int i = 0; i < count && e == ESP_OK; i++) {
        uint8_t val = 0;
        e = hl_hdmi_reg(page, (uint8_t)(reg + i), &val, false);
        snprintf(hex + 2 * i, 3, "%02x", val);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", e == ESP_OK);
    cJSON_AddNumberToObject(j, "page", page);
    cJSON_AddNumberToObject(j, "reg", reg);
    if (e == ESP_OK) cJSON_AddStringToObject(j, "hex", hex); else cJSON_AddStringToObject(j, "error", esp_err_to_name(e));
    return send_json(req, j);
}

static esp_err_t h_reg_post(httpd_req_t *req)
{
    uint8_t page, reg, val; char v[8];
    if (!reg_args(req, &page, &reg) || !query(req, "val", v, sizeof v)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "page=<hex>&reg=<hex>&val=<hex>");
    val = (uint8_t)strtoul(v, NULL, 16);
    esp_err_t e = hl_hdmi_reg(page, reg, &val, true);
    ESP_LOGW(TAG, "GSV2001 page %02x reg %02x <- %02x over HTTP: %s", page, reg, val, esp_err_to_name(e));
    uint8_t back = 0;
    if (e == ESP_OK) e = hl_hdmi_reg(page, reg, &back, false);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", e == ESP_OK);
    char hex[3]; snprintf(hex, sizeof hex, "%02x", back);
    if (e == ESP_OK) cJSON_AddStringToObject(j, "reads_back", hex); else cJSON_AddStringToObject(j, "error", esp_err_to_name(e));
    return send_json(req, j);
}

/* POST /api/hdmi?passthrough=0|1&scale=0|1&tvhdcp=0|1&drive=0..2: set the splitter up differently and start it again
 * (bench); with no argument, only report the set-up. */
static esp_err_t h_hdmi(httpd_req_t *req)
{
    char v[8]; int pt = -1, sc = -1, th = -1, dr = -1;
    if (query(req, "passthrough", v, sizeof v)) pt = atoi(v);
    if (query(req, "scale", v, sizeof v)) sc = atoi(v);
    if (query(req, "tvhdcp", v, sizeof v)) th = atoi(v);
    if (query(req, "drive", v, sizeof v)) dr = atoi(v);
    if (pt >= 0 || sc >= 0 || th >= 0 || dr >= 0) {
        ESP_LOGW(TAG, "splitter set-up over HTTP: passthrough %d scale %d tvhdcp %d drive %d", pt, sc, th, dr);
        hl_hdmi_setup(pt, sc, th, dr);
    }
    hl_hdmi_get_setup(&pt, &sc, &th, &dr);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "passthrough", pt); cJSON_AddBoolToObject(j, "scale", sc); cJSON_AddBoolToObject(j, "tvhdcp", th);
    cJSON_AddNumberToObject(j, "drive", dr);
    return send_json(req, j);
}

static esp_err_t h_i2c(httpd_req_t *req)
{
    char buf[200]; int n = hl_hdmi_i2c_scan(buf, sizeof buf);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "answering", n);
    cJSON_AddStringToObject(j, "addresses", n > 0 ? buf : "");
    return send_json(req, j);
}

static esp_err_t h_edid(httpd_req_t *req)
{
    char v[12] = "source"; query(req, "which", v, sizeof v);
    uint8_t e[256];
    if (!hl_hdmi_get_edid(!strcmp(v, "tv") ? 1 : !strcmp(v, "ms2130") ? 2 : 0, e)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "splitter not running");
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)e, sizeof e);
}

static esp_err_t h_ms_power(httpd_req_t *req)
{
    char v[8] = "";
    if (!query(req, "power", v, sizeof v)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "power=on|off|cycle");
    if (!strcmp(v, "on")) hl_ms2130_power(true);
    else if (!strcmp(v, "off")) hl_ms2130_power(false);
    else if (!strcmp(v, "cycle")) hl_ms2130_cycle(500);
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "power=on|off|cycle");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "core_power", hl_ms2130_powered());
    return send_json(req, j);
}

/* GET /api/incident: the last time the input stayed without a picture while a source was present (hdmi.c) */
static esp_err_t h_incident(httpd_req_t *req)
{
    char *t = malloc(2000); if (!t) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    hl_hdmi_incident(t, 2000);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t e = httpd_resp_sendstr(req, t); free(t); return e;
}

/* POST /api/cec?tv=on|off and/or follow=0|1 */
static esp_err_t h_cec(httpd_req_t *req)
{
    char v[8]; bool any = false; char hx[40];
    if (query(req, "send", hx, sizeof hx)) {                /* bench: send=<hex bytes>, e.g. 4004 */
        uint8_t d[16]; int len = 0; unsigned b;
        for (const char *c = hx; c[0] && c[1] && len < 16 && sscanf(c, "%2x", &b) == 1; c += 2) d[len++] = (uint8_t)b;
        if (!hl_cec_send(d, len)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "send=<2 to 32 hex digits>");
        any = true;
    }
    if (query(req, "follow", v, sizeof v)) { hl_cec_set_follow(v[0] == '1'); any = true; }
    if (query(req, "tv", v, sizeof v)) {
        if (strcmp(v, "on") && strcmp(v, "off")) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tv=on|off");
        hl_cec_tv_power(!strcmp(v, "on")); any = true;
    }
    if (!any) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tv=on|off and/or follow=0|1");
    hl_cec_status_t cs; hl_cec_get(&cs);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "tv", cs.tv);
    cJSON_AddBoolToObject(j, "follow", cs.follow);
    return send_json(req, j);
}

static esp_err_t h_flash_get(httpd_req_t *req)
{
    char v[4];
    if (query(req, "id", v, sizeof v)) {
        uint8_t id[3] = { 0 };
        esp_err_t e = hl_msflash_id(id);
        char hex[8]; snprintf(hex, sizeof hex, "%02x%02x%02x", id[0], id[1], id[2]);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", e == ESP_OK);
        cJSON_AddStringToObject(j, "jedec", hex);
        if (e != ESP_OK) cJSON_AddStringToObject(j, "error", e == ESP_ERR_NOT_FOUND ? "no flash answering: the MS2130 may be driving the bus with its core off" : esp_err_to_name(e));
        return send_json(req, j);
    }
    uint8_t *buf = heap_caps_malloc(MS_FLASH_IMAGE_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_500(req);
    esp_err_t e = hl_msflash_read(buf, MS_FLASH_IMAGE_MAX), r;
    if (e == ESP_OK) {
        httpd_resp_set_type(req, "application/octet-stream");
        r = httpd_resp_send(req, (const char *)buf, MS_FLASH_IMAGE_MAX);
    } else {
        r = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, e == ESP_ERR_NOT_FOUND ? "no flash answering" : esp_err_to_name(e));
    }
    free(buf);
    return r;
}

static esp_err_t h_flash_post(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > MS_FLASH_IMAGE_MAX) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body = an MS2130 flash image, up to 65536 bytes");
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_500(req);
    for (int got = 0; got < len; ) {
        int r = httpd_req_recv(req, (char *)buf + got, len - got);
        if (r <= 0) { free(buf); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "receive failed"); }
        got += r;
    }
    char msg[160] = "";
    esp_err_t e = hl_msflash_program(buf, (size_t)len, msg, sizeof msg);
    free(buf);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", e == ESP_OK);
    cJSON_AddStringToObject(j, "message", msg);
    if (e != ESP_OK) httpd_resp_set_status(req, e == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error");
    return send_json(req, j);
}

esp_err_t hl_board_register_http(void *httpd)
{
    const httpd_uri_t uris[] = {
        { "/api/board",        HTTP_GET,  h_board,      NULL },
        { "/api/edid",         HTTP_GET,  h_edid,       NULL },
        { "/api/cec",          HTTP_POST, h_cec,        NULL },
        { "/api/incident",     HTTP_GET,  h_incident,   NULL },
        { "/api/ms2130",       HTTP_POST, h_ms_power,   NULL },
        { "/api/ms2130/flash", HTTP_GET,  h_flash_get,  NULL },
        { "/api/ms2130/flash", HTTP_POST, h_flash_post, NULL },
        { "/api/reg",          HTTP_GET,  h_reg_get,    NULL },
        { "/api/reg",          HTTP_POST, h_reg_post,   NULL },
        { "/api/i2c",          HTTP_GET,  h_i2c,        NULL },
        { "/api/hdmi",         HTTP_POST, h_hdmi,       NULL },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        esp_err_t e = httpd_register_uri_handler((httpd_handle_t)httpd, &uris[i]);
        if (e != ESP_OK) { ESP_LOGE(TAG, "registering %s: %s", uris[i].uri, esp_err_to_name(e)); return e; }
    }
    return hl_msusb_register_http(httpd);            /* /api/ms2130/usb: the capture chip's flash over USB */
}
