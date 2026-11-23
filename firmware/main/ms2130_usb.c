/* The MS2130's boot flash read and written over USB, from the ESP32 as the host.
 *
 * The chip takes flash commands as HID feature reports on its HID interface (ms213x_proto.c), both from its ROM
 * (534d:2130, which is what a blank flash boots into) and from a running firmware (345f:2130). On the oneboard
 * design the chip's USB goes only to the ESP32, so this is how its blank flash gets loaded. A job runs in its own
 * task; the HTTP side only starts it and polls.
 *
 *   GET  /api/ms2130/usb            job state: {state, write, stage, pct, message, device, have_dump}
 *   POST /api/ms2130/usb?op=read    read the 64 KB flash        -> then GET /api/ms2130/usb/dump
 *   POST /api/ms2130/usb?op=write   body = an MS2130 image: checked, then erase + write + verify
 * tools/ms2130_flash.sh usb-read / usb-write wraps these.
 *
 * Part of the oneboard build only. On the oneboard (2026-11-20) a read of the programmed chip returned the image
 * byte for byte while the capture kept running. A blank chip never enumerates, so the first programming goes over
 * SPI (msflash.c). The write has never been run. */
#include "ms2130.h"
#include "ms213x_proto.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "oneboard.h"

static const char *TAG = "msusb";
#define XFER_TIMEOUT_US  (10 * 1000 * 1000)     /* the erase command may hold its request until the flash is done */
#define HID_IF_DEFAULT   4

static const struct { uint16_t vid, pid; const char *name; } s_known[] = {
    { 0x345f, 0x2130, "MS2130" }, { 0x534d, 0x2130, "MS2130 ROM (blank or invalid flash)" },
};

static SemaphoreHandle_t        s_lock;          /* s_job */
static hl_msusb_job_t           s_job;
static uint8_t                 *s_buf;           /* 64 KB in PSRAM: the image to write, or the dump read */
static size_t                   s_len;
static usb_host_client_handle_t s_client;
static usb_device_handle_t      s_dev;
static usb_transfer_t          *s_xfer;
static volatile bool            s_xfer_done;
static uint8_t                  s_hid_if, s_mps0;

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) { (void)msg; (void)arg; }
static bool ensure_client(void);
/* connect / disconnect notices pile up in the client's small queue between uses: empty it */
static void drain_events(void) { for (int i = 0; i < 8 && usb_host_client_handle_events(s_client, 0) == ESP_OK; i++) { } }
static void xfer_cb(usb_transfer_t *t) { (void)t; s_xfer_done = true; }

/* one HID feature report (report 1, 8 bytes) to or from the HID interface, as a control transfer on endpoint 0 */
static int report(bool in, const uint8_t *out8, uint8_t *in8)
{
    usb_setup_packet_t *sp = (usb_setup_packet_t *)s_xfer->data_buffer;
    sp->bmRequestType = in ? 0xa1 : 0x21;                 /* class, interface */
    sp->bRequest = in ? 0x01 : 0x09;                      /* GET_REPORT / SET_REPORT */
    sp->wValue = (3 << 8) | 1;                            /* feature report 1 */
    sp->wIndex = s_hid_if;
    sp->wLength = 8;
    if (!in) memcpy(s_xfer->data_buffer + sizeof *sp, out8, 8);
    s_xfer->num_bytes = (int)sizeof *sp + (in ? usb_round_up_to_mps(8, s_mps0) : 8);
    s_xfer->device_handle = s_dev;
    s_xfer->bEndpointAddress = 0;
    s_xfer->callback = xfer_cb;
    s_xfer_done = false;
    if (usb_host_transfer_submit_control(s_client, s_xfer) != ESP_OK) return -1;
    int64_t t0 = esp_timer_get_time();
    while (!s_xfer_done) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(20));
        if (esp_timer_get_time() - t0 > XFER_TIMEOUT_US) { ESP_LOGE(TAG, "control transfer timed out"); return -1; }
    }
    if (s_xfer->status != USB_TRANSFER_STATUS_COMPLETED) { ESP_LOGE(TAG, "control transfer status %d", (int)s_xfer->status); return -1; }
    if (in) {
        if (s_xfer->actual_num_bytes < (int)sizeof *sp + 8) return -1;
        memcpy(in8, s_xfer->data_buffer + sizeof *sp, 8);
    }
    return 0;
}
static int  io_set(void *ctx, const uint8_t r[8]) { (void)ctx; return report(false, r, NULL); }
static int  io_get(void *ctx, uint8_t r[8]) { (void)ctx; return report(true, NULL, r); }
static void io_delay(void *ctx, unsigned ms) { (void)ctx; vTaskDelay(pdMS_TO_TICKS(ms)); }
static void io_progress(void *ctx, const char *stage, int pct)
{
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_job.stage, stage, sizeof s_job.stage); s_job.pct = pct;
    xSemaphoreGive(s_lock);
}

/* open the first MS2130 on the bus and find its HID interface */
static bool open_device(char *why, size_t why_len)
{
    uint8_t addrs[8]; int n = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) != ESP_OK || n == 0) { snprintf(why, why_len, "no USB device is connected"); return false; }
    for (int i = 0; i < n; i++) {
        usb_device_handle_t dev;
        if (usb_host_device_open(s_client, addrs[i], &dev) != ESP_OK) continue;
        const usb_device_desc_t *dd;
        if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK)
            for (size_t k = 0; k < sizeof s_known / sizeof s_known[0]; k++)
                if (dd->idVendor == s_known[k].vid && dd->idProduct == s_known[k].pid) {
                    s_dev = dev; s_mps0 = dd->bMaxPacketSize0 ? dd->bMaxPacketSize0 : 64; s_hid_if = HID_IF_DEFAULT;
                    const usb_config_desc_t *cd;
                    if (usb_host_get_active_config_descriptor(dev, &cd) == ESP_OK) {
                        int off = 0; const usb_standard_desc_t *d = (const usb_standard_desc_t *)cd;
                        while ((d = usb_parse_next_descriptor_of_type(d, cd->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off)) != NULL) {
                            const usb_intf_desc_t *id = (const usb_intf_desc_t *)d;
                            if (id->bInterfaceClass == USB_CLASS_HID) { s_hid_if = id->bInterfaceNumber; break; }
                        }
                    }
                    xSemaphoreTake(s_lock, portMAX_DELAY); s_job.vid = dd->idVendor; s_job.pid = dd->idProduct; xSemaphoreGive(s_lock);
                    ESP_LOGI(TAG, "%s at address %u (%04x:%04x), HID interface %u", s_known[k].name, addrs[i], dd->idVendor, dd->idProduct, s_hid_if);
                    return true;
                }
        usb_host_device_close(s_client, dev);
    }
    snprintf(why, why_len, "no MS2130 among the %d USB device%s connected (looking for 345f:2130 or 534d:2130)", n, n == 1 ? "" : "s");
    return false;
}

static void finish(bool ok, const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_job.state = ok ? HL_MSUSB_DONE : HL_MSUSB_FAILED;
    s_job.have_dump = ok && !s_job.write;
    strlcpy(s_job.msg, msg, sizeof s_job.msg);
    xSemaphoreGive(s_lock);
    if (ok) ESP_LOGW(TAG, "%s", msg); else ESP_LOGE(TAG, "%s", msg);
}

static void job_task(void *arg)
{
    (void)arg;
    char msg[160];
    bool write = s_job.write;
    if (!ensure_client()) { finish(false, "could not register with the USB host"); vTaskDelete(NULL); }
    drain_events();
    if (!s_xfer && usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 64, 0, &s_xfer) != ESP_OK) { s_xfer = NULL; finish(false, "out of memory for the USB transfer"); vTaskDelete(NULL); }
    if (!open_device(msg, sizeof msg)) { finish(false, msg); vTaskDelete(NULL); }

    const ms213x_io io = { io_set, io_get, io_delay, io_progress, NULL };
    int r = ms213x_setup_spi(&io);
    uint32_t bad = 0;
    if (r == 0) r = write ? ms213x_program(&io, s_buf, (uint32_t)s_len, &bad) : ms213x_read(&io, s_buf, MS213X_FLASH_SIZE);
    usb_host_device_close(s_client, s_dev); s_dev = NULL;

    if (r == MS213X_ERR_IO) finish(false, "USB transfer failed (the chip may have dropped off the bus); nothing can be said about the flash contents");
    else if (r == MS213X_ERR_ERASE) finish(false, "the flash did not read back blank after the erase command; nothing was written");
    else if (r == MS213X_ERR_VERIFY) { snprintf(msg, sizeof msg, "verify failed at offset 0x%05x: write it again", (unsigned)bad); finish(false, msg); }
    else if (!write) {
        char why[96]; bool ok = hl_ms2130_image_ok(s_buf, MS213X_FLASH_SIZE, why, sizeof why);
        snprintf(msg, sizeof msg, "read 65536 bytes; %s%s", ok ? "valid MS2130 image: " : "not a valid image: ", why);
        finish(true, msg);
    } else {
        hl_ms2130_cycle(500);                    /* restart the chip from the new image */
        snprintf(msg, sizeof msg, "wrote and verified %u bytes; the MS2130's core supply was cycled to restart it", (unsigned)s_len);
        finish(true, msg);
    }
    vTaskDelete(NULL);
}

esp_err_t hl_msusb_start(const uint8_t *img, size_t len)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    char why[96];
    if (img && !hl_ms2130_image_ok(img, len, why, sizeof why)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_job.state == HL_MSUSB_RUNNING) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    if (!s_buf) s_buf = heap_caps_malloc(MS213X_FLASH_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
    memset(&s_job, 0, sizeof s_job);
    s_job.state = HL_MSUSB_RUNNING; s_job.write = img != NULL;
    strlcpy(s_job.stage, "starting", sizeof s_job.stage);
    if (img) { memcpy(s_buf, img, len); s_len = len; }
    xSemaphoreGive(s_lock);
    if (xTaskCreatePinnedToCore(job_task, "msusb", 6144, NULL, 5, NULL, 0) != pdPASS) { finish(false, "could not start the job task"); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

void hl_msusb_status(hl_msusb_job_t *j)
{
    if (!s_lock) { memset(j, 0, sizeof *j); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY); *j = s_job; xSemaphoreGive(s_lock);
}

size_t hl_msusb_dump(const uint8_t **data)
{
    hl_msusb_job_t j; hl_msusb_status(&j);
    if (!j.have_dump) return 0;
    *data = s_buf;
    return MS213X_FLASH_SIZE;
}

static bool ensure_client(void)
{
    if (s_client) return true;
    const usb_host_client_config_t cc = { .is_synchronous = false, .max_num_event_msg = 5,
                                          .async = { .client_event_callback = client_event_cb, .callback_arg = NULL } };
    if (usb_host_client_register(&cc, &s_client) != ESP_OK) { s_client = NULL; return false; }
    return true;
}

int hl_msusb_list(char *out, size_t len)
{
    out[0] = 0;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    hl_msusb_job_t j; hl_msusb_status(&j);
    if (j.state == HL_MSUSB_RUNNING || !ensure_client()) return -1;
    drain_events();
    uint8_t addrs[8]; int n = 0, w = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) != ESP_OK) return 0;
    for (int i = 0; i < n; i++) {
        usb_device_handle_t dev; const usb_device_desc_t *dd;
        if (usb_host_device_open(s_client, addrs[i], &dev) != ESP_OK) continue;
        if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK && w < (int)len - 24)
            w += snprintf(out + w, len - w, "addr %u  %04x:%04x\n", addrs[i], dd->idVendor, dd->idProduct);
        usb_host_device_close(s_client, dev);
    }
    return n;
}

/* ---- HTTP ---------------------------------------------------------------------------------------------- */
static esp_err_t send_status(httpd_req_t *req, const char *err)
{
    hl_msusb_job_t j; hl_msusb_status(&j);
    static const char *st[] = { "idle", "running", "done", "failed" };
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", st[j.state]);
    cJSON_AddBoolToObject(o, "write", j.write);
    cJSON_AddStringToObject(o, "stage", j.stage);
    cJSON_AddNumberToObject(o, "pct", j.pct);
    cJSON_AddStringToObject(o, "message", j.msg);
    char dev[12]; snprintf(dev, sizeof dev, "%04x:%04x", j.vid, j.pid);
    cJSON_AddStringToObject(o, "device", j.vid ? dev : "");
    cJSON_AddBoolToObject(o, "have_dump", j.have_dump);
    if (err) cJSON_AddStringToObject(o, "error", err);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

static esp_err_t h_get(httpd_req_t *req) { return send_status(req, NULL); }

static esp_err_t h_post(httpd_req_t *req)
{
    char q[32] = "", op[8] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "op", op, sizeof op) != ESP_OK ||
        (strcmp(op, "read") && strcmp(op, "write"))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "op=read or op=write");
    esp_err_t e;
    if (!strcmp(op, "read")) e = hl_msusb_start(NULL, 0);
    else {
        int len = req->content_len;
        if (len <= 0 || len > MS_FLASH_IMAGE_MAX) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body = an MS2130 flash image, up to 65536 bytes");
        uint8_t *img = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (!img) return httpd_resp_send_500(req);
        for (int got = 0; got < len; ) {
            int r = httpd_req_recv(req, (char *)img + got, len - got);
            if (r <= 0) { free(img); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "receive failed"); }
            got += r;
        }
        e = hl_msusb_start(img, (size_t)len);
        free(img);
    }
    if (e == ESP_ERR_INVALID_ARG) { httpd_resp_set_status(req, "400 Bad Request"); return send_status(req, "not a valid MS2130 image (header or checksums): nothing was started"); }
    if (e == ESP_ERR_INVALID_STATE) { httpd_resp_set_status(req, "409 Conflict"); return send_status(req, "a job is already running"); }
    if (e != ESP_OK) { httpd_resp_set_status(req, "500 Internal Server Error"); return send_status(req, esp_err_to_name(e)); }
    return send_status(req, NULL);
}

static esp_err_t h_dump(httpd_req_t *req)
{
    const uint8_t *d; size_t n = hl_msusb_dump(&d);
    if (!n) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no finished read: POST /api/ms2130/usb?op=read first");
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)d, n);
}

esp_err_t hl_msusb_register_http(void *httpd)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    const httpd_uri_t uris[] = {
        { "/api/ms2130/usb",      HTTP_GET,  h_get,  NULL },
        { "/api/ms2130/usb",      HTTP_POST, h_post, NULL },
        { "/api/ms2130/usb/dump", HTTP_GET,  h_dump, NULL },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        esp_err_t e = httpd_register_uri_handler((httpd_handle_t)httpd, &uris[i]);
        if (e != ESP_OK) { ESP_LOGE(TAG, "registering %s: %s", uris[i].uri, esp_err_to_name(e)); return e; }
    }
    return ESP_OK;
}
