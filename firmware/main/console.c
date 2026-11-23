/* A small serial console for bring-up: the same questions the web page answers, without needing Wi-Fi.
 *
 * Lines are read from UART0 (the USB-to-UART port of a dev kit) and from the chip's USB Serial/JTAG port; answers
 * go to the log output, which reaches both. Type "help". Both ports work on the oneboard (its bring-up was done over
 * the USB Serial/JTAG port). */
#include "sdkconfig.h"
#if CONFIG_HL_CONSOLE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "soc/soc_caps.h"
#include "driver/uart.h"
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "board.h"
#include "capture.h"
#if CONFIG_HL_WIFI
#include "net.h"
#endif

static const char *TAG = "console";
#define HL_LINE_MAX 96

static void cmd_help(void)
{
    printf("commands:\n"
           "  status                 frames, USB grabber, Wi-Fi, memory\n"
#if HL_BOARD_LED_GPIO < 0
           "  led <gpio>             move the LED strip's data pin (saved)\n"
#endif
           "  ledtest                run the strip self-test\n"
#if CONFIG_HL_WIFI
           "  wifi <ssid> [password] join a network (saved)\n"
#endif
           "  reboot\n");
    hl_board_console_help();
}

static void cmd_status(void)
{
    hl_status_t s; hl_app_get_status(&s);
    const hl_cfg_t *c = hl_app_cfg();
    printf("up %lu s, %d LEDs on GPIO %u, output %s\n", (unsigned long)s.uptime_s, s.n_leds, c->gpio, s.out_mode == 0 ? "live" : s.out_mode == 1 ? "off" : "solid colour");
    printf("USB grabber %s, %lu frames, %.1f fps, decode %.1f ms, pipeline %.1f ms%s\n", s.usb_connected ? "streaming" : "not streaming",
           (unsigned long)s.frames_total, s.fps, s.decode_ms, s.pipeline_ms, s.no_signal ? ", no signal" : "");
#if CONFIG_HL_WIFI
    hl_net_info_t n; hl_net_info(&n);
    printf("Wi-Fi %s \"%s\" %s%s\n", n.mode, n.ssid, n.ip, n.connected ? "" : " (not connected)");
#endif
    printf("free: %u KB PSRAM, %u KB internal\n", (unsigned)(s.free_psram / 1024), (unsigned)(s.free_heap / 1024));
}

static void run(char *line)
{
    char *argv[6]; int argc = 0;
    for (char *t = strtok(line, " \t"); t && argc < 6; t = strtok(NULL, " \t")) argv[argc++] = t;
    if (!argc) return;
    if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) cmd_help();
    else if (!strcmp(argv[0], "status")) cmd_status();
    else if (!strcmp(argv[0], "led") && argc >= 2) {
#if HL_BOARD_LED_GPIO >= 0
        printf("the LED strip is wired to GPIO %d on this board: the pin cannot be moved\n", HL_BOARD_LED_GPIO);
#else
        hl_cfg_t c = *hl_app_cfg(); c.gpio = (uint8_t)atoi(argv[1]);
        printf("LED data pin GPIO %u: %s\n", c.gpio, hl_app_apply_config(&c) == ESP_OK ? "applied and saved" : "failed");
#endif
    } else if (!strcmp(argv[0], "ledtest")) { hl_app_selftest(); printf("done\n"); }
#if CONFIG_HL_WIFI
    else if (!strcmp(argv[0], "wifi") && argc >= 2) {
        hl_cfg_t c = *hl_app_cfg();
        strlcpy(c.wifi_ssid, argv[1], sizeof c.wifi_ssid); strlcpy(c.wifi_pass, argc >= 3 ? argv[2] : "", sizeof c.wifi_pass);
        hl_app_apply_config(&c); hl_net_join(c.wifi_ssid, c.wifi_pass);
        printf("joining \"%s\": 'status' shows the result\n", c.wifi_ssid);
    }
#endif
    else if (!strcmp(argv[0], "reboot")) { printf("rebooting\n"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
    else if (!hl_board_console(argc, argv)) printf("unknown command: %s (try 'help')\n", argv[0]);
}

/* one character from either port into that port's line; a finished line runs */
static void feed(char *line, int *len, char ch)
{
    if (ch == '\r' || ch == '\n') {
        if (*len) { putchar('\n'); line[*len] = 0; *len = 0; run(line); }
        printf("> "); fflush(stdout);
    } else if (ch == 0x08 || ch == 0x7f) {
        if (*len) { (*len)--; printf("\b \b"); fflush(stdout); }
    } else if (ch >= 0x20 && ch < 0x7f && *len < HL_LINE_MAX - 1) {
        line[(*len)++] = ch; putchar(ch); fflush(stdout);
    }
}

static void console_task(void *arg)
{
    (void)arg;
    static char l_uart[HL_LINE_MAX], l_usj[HL_LINE_MAX];
    int n_uart = 0, n_usj = 0;
    bool uart_ok = uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0) == ESP_OK;
    bool usj_ok = false;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    usb_serial_jtag_driver_config_t uc = { .rx_buffer_size = 256, .tx_buffer_size = 256 };
    usj_ok = usb_serial_jtag_driver_install(&uc) == ESP_OK;
#endif
    ESP_LOGI(TAG, "type 'help' (UART0%s%s)", uart_ok ? "" : " unavailable", usj_ok ? " or the USB Serial/JTAG port" : "");
    if (!uart_ok && !usj_ok) vTaskDelete(NULL);
    while (1) {
        uint8_t ch; bool got = false;
        if (uart_ok) while (uart_read_bytes(UART_NUM_0, &ch, 1, 0) == 1) { feed(l_uart, &n_uart, (char)ch); got = true; }
#if SOC_USB_SERIAL_JTAG_SUPPORTED
        if (usj_ok) while (usb_serial_jtag_read_bytes(&ch, 1, 0) == 1) { feed(l_usj, &n_usj, (char)ch); got = true; }
#endif
        if (!got) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void hl_console_start(void) { xTaskCreatePinnedToCore(console_task, "console", 8192, NULL, 3, NULL, 1); }
#else
void hl_console_start(void) {}
#endif
