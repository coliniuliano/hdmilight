/* USB UVC capture for the MS2130 dongle.
 *
 * Settings follow the P4 + MacroSilicon projects that stream these cards
 * (netube99/sskvm, 93percentai/esp32-MS2109, wxffxx/ExoAnchor): three explicitly
 * sized PSRAM frame buffers, stop -> drain -> close ordering, reopen on a 10 s
 * stall, and a log of the card's advertised modes on connect.
 *
 * Verified on hardware (Mac + ESP32-P4-Module-DEV-KIT, 2026-11-01): on a USB 2.0
 * link the MS2130 streams MJPEG over a BULK endpoint (EP 0x83, 512 B packets) with
 * a negotiated payload of 15360 bytes = 30 full packets, so mid-frame payloads
 * never end with a short packet. The stock usb_host_uvc bulk state machine only
 * finds payload boundaries by short packets and leaves the 12-byte payload headers
 * inside the JPEG; firmware/patches/usb_host_uvc sizes each URB to the negotiated
 * payload and treats a full URB as a boundary too. urb_size is therefore left 0. */
#include "capture.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"

#if CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE < 4096
#error "MacroSilicon capture cards have >1 KiB configuration descriptors: set CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE >= 4096"
#endif

static const char *TAG = "capture";
#define USB_HOST_PRIORITY   15
#define FRAME_QUEUE_LEN     3
#define FRAME_MAX_BYTES     (CONFIG_HL_CAPTURE_FRAME_KB * 1024)
#define STALL_TIMEOUT_US    (10 * 1000 * 1000)

static QueueHandle_t      s_queue;
static SemaphoreHandle_t  s_gate;        /* serialises frame_cb against stop/drain */
static bool               s_accept;
static uvc_host_stream_hdl_t s_stream;
static volatile bool      s_connected;
static volatile int64_t   s_last_frame_us;
static uint32_t           s_frames_total;
static volatile uint16_t  s_fmt_w, s_fmt_h;      /* the mode the card agreed to; 0 until a stream is open */
static volatile float     s_fmt_fps;
static volatile bool      s_gone;        /* the device left the bus since the stream was opened */

/* Oneboard, seen 2026-11-20 (every one of 13 power-ups of the MS2130's core): the MS2130 enumerates about 1.4 s after
 * its core supply comes up, leaves the bus by itself about 0.24 s later (with or without a stream start) and is back
 * for good about 1.4 s after that. A start inside that window fails, so the stream is only started once the device
 * has stayed for this long. An MS2130 that was already running (an ESP32 restart) does not do this. */
#if CONFIG_HL_BOARD_ONEBOARD
#define SETTLE_MS 500
#else
#define SETTLE_MS 0
#endif

static bool frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    /* Runs in the UVC driver task: never decode here. */
    if (xSemaphoreTake(s_gate, 0) != pdTRUE) return true;
    hl_capture_frame_t f = { .stream = s_stream, .frame = frame };
    bool keep = s_accept && xQueueSendToBack(s_queue, &f, 0) == pdPASS;
    xSemaphoreGive(s_gate);
    if (keep) { s_last_frame_us = esp_timer_get_time(); s_frames_total++; }
    return !keep;   /* true = driver takes the buffer back immediately */
}

static void stream_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    static unsigned buffer_errors;
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB transfer error %s", esp_err_to_name(event->transfer_error.error));
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        if (s_connected) ESP_LOGW(TAG, "grabber disconnected");
        s_gone = true;
        s_connected = false;
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        if (++buffer_errors == 1 || buffer_errors % 100 == 0)
            ESP_LOGW(TAG, "frame buffer %s (%u total)",
                     event->type == UVC_HOST_FRAME_BUFFER_OVERFLOW ? "overflow: raise HL_CAPTURE_FRAME_KB" : "underflow: decode too slow",
                     buffer_errors);
        break;
    default:
        break;
    }
}

static void driver_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) return;
    ESP_LOGI(TAG, "UVC device connected: addr %u, %u modes", event->device_connected.dev_addr, (unsigned)event->device_connected.frame_info_num);
}

static uvc_host_stream_config_t s_cfg = {
    .event_cb = stream_cb,
    .frame_cb = frame_cb,
    .user_ctx = NULL,
    .usb = { .vid = UVC_HOST_ANY_VID, .pid = UVC_HOST_ANY_PID, .uvc_stream_index = 0 },
    .vs_format = {
        .h_res = CONFIG_HL_CAPTURE_WIDTH,
        .v_res = CONFIG_HL_CAPTURE_HEIGHT,
        .fps = CONFIG_HL_CAPTURE_FPS,
        .format = UVC_VS_FORMAT_MJPEG,
    },
    .advanced = {
        .number_of_frame_buffers = FRAME_QUEUE_LEN,
        .frame_size = FRAME_MAX_BYTES,          /* bounded, not the card's dwMaxVideoFrameSize */
        .frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .number_of_urbs = 3,
        .urb_size = 0,       /* bulk: the (patched) driver sizes each URB to the negotiated payload; isoc: driver default */
    },
};

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void drain_queue(void)
{
    hl_capture_frame_t f;
    while (xQueueReceive(s_queue, &f, 0) == pdPASS)
        uvc_host_frame_return(f.stream, (uvc_host_frame_t *)f.frame);
}

static void stream_task(void *arg)
{
    while (1) {
        ESP_LOGI(TAG, "opening UVC stream %ux%u MJPEG @%d...", s_cfg.vs_format.h_res, s_cfg.vs_format.v_res, CONFIG_HL_CAPTURE_FPS);
        s_gone = false;
        esp_err_t err = uvc_host_stream_open(&s_cfg, pdMS_TO_TICKS(5000), &s_stream);
        usb_host_lib_info_t present = { 0 };
        usb_host_lib_info(&present);
        if (err != ESP_OK && present.num_devices == 0) {      /* nothing on the bus: not a frame-rate mismatch */
            static bool said;
            if (!said) { said = true; ESP_LOGW(TAG, "no USB grabber on the host port (%s): waiting for one", esp_err_to_name(err)); }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (err != ESP_OK && s_cfg.vs_format.fps != 0) {
            /* The driver matches fps exactly against the descriptor; fall back to the card default. */
            ESP_LOGW(TAG, "no %d fps mode (%s), retrying at card default", CONFIG_HL_CAPTURE_FPS, esp_err_to_name(err));
            float save = s_cfg.vs_format.fps;
            s_cfg.vs_format.fps = 0;
            err = uvc_host_stream_open(&s_cfg, pdMS_TO_TICKS(5000), &s_stream);
            s_cfg.vs_format.fps = save;
        }
        if (err != ESP_OK) {
            usb_host_lib_info_t info = { 0 };
            usb_host_lib_info(&info);
            ESP_LOGW(TAG, "open failed: %s (USB devices enumerated: %d)", esp_err_to_name(err), info.num_devices);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        uvc_host_stream_format_t fmt;
        if (uvc_host_stream_format_get(s_stream, &fmt) == ESP_OK) {
            ESP_LOGI(TAG, "negotiated %ux%u @%.1f fmt=%d", fmt.h_res, fmt.v_res, fmt.fps, fmt.format);
            s_fmt_w = fmt.h_res; s_fmt_h = fmt.v_res; s_fmt_fps = fmt.fps;
        }

        xSemaphoreTake(s_gate, portMAX_DELAY);
        s_accept = true;
        xSemaphoreGive(s_gate);
        s_last_frame_us = esp_timer_get_time();

        for (int waited = 0; waited < SETTLE_MS && !s_gone; waited += 50) vTaskDelay(pdMS_TO_TICKS(50));
        err = s_gone ? ESP_ERR_NOT_FINISHED : uvc_host_stream_start(s_stream);
        if (err == ESP_ERR_NOT_FINISHED) {
            ESP_LOGI(TAG, "grabber left the bus right after enumerating (it does after a power-up): waiting for it to come back");
        } else if (err == ESP_OK) {
            s_connected = true;
            ESP_LOGI(TAG, "streaming");
            while (s_connected) {
                vTaskDelay(pdMS_TO_TICKS(500));
                if (esp_timer_get_time() - s_last_frame_us > STALL_TIMEOUT_US) {
                    ESP_LOGW(TAG, "no frames for 10 s, reopening stream");
                    break;
                }
            }
        } else {
            ESP_LOGE(TAG, "stream start failed: %s", esp_err_to_name(err));
        }

        /* stop first (frames still arrive until then), then gate callbacks, then return every buffer */
        if (s_gone) vTaskDelay(pdMS_TO_TICKS(100));    /* the driver paused the stream itself; there is nothing to send a stop to */
        else uvc_host_stream_stop(s_stream);
        xSemaphoreTake(s_gate, portMAX_DELAY);
        s_accept = false;
        drain_queue();
        xSemaphoreGive(s_gate);
        s_connected = false;
        err = uvc_host_stream_close(s_stream);
        if (err != ESP_OK) ESP_LOGW(TAG, "close: %s", esp_err_to_name(err));
        s_stream = NULL;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void hl_capture_start(void)
{
    s_queue = xQueueCreate(FRAME_QUEUE_LEN, sizeof(hl_capture_frame_t));
    s_gate = xSemaphoreCreateMutex();
    const usb_host_config_t host_config = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, USB_HOST_PRIORITY, NULL, 0);
    const uvc_host_driver_config_t drv = {
        .driver_task_stack_size = 5 * 1024,
        .driver_task_priority = USB_HOST_PRIORITY + 1,
        .xCoreID = 0,
        .create_background_task = true,
        .event_cb = driver_cb,
    };
    ESP_ERROR_CHECK(uvc_host_install(&drv));
    xTaskCreatePinnedToCore(stream_task, "uvc_stream", 4096, NULL, USB_HOST_PRIORITY - 2, NULL, 0);
    ESP_LOGI(TAG, "USB host up; control transfer limit %d, frame buffers %d x %d KiB", CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE, FRAME_QUEUE_LEN, CONFIG_HL_CAPTURE_FRAME_KB);
}

/* Returns the newest queued frame, returning any older ones to the driver.
 * Blocks up to timeout_ticks for the first one. */
bool hl_capture_take_newest(hl_capture_frame_t *out, TickType_t timeout_ticks)
{
    hl_capture_frame_t f;
    if (xQueueReceive(s_queue, &f, timeout_ticks) != pdPASS) return false;
    hl_capture_frame_t newer;
    while (xQueueReceive(s_queue, &newer, 0) == pdPASS) {
        uvc_host_frame_return(f.stream, (uvc_host_frame_t *)f.frame);
        f = newer;
    }
    *out = f;
    return true;
}

void hl_capture_release(const hl_capture_frame_t *f)
{
    uvc_host_frame_return(f->stream, (uvc_host_frame_t *)f->frame);
}

bool hl_capture_connected(void) { return s_connected; }

bool hl_capture_format(int *w, int *h, float *fps)
{
    *w = s_fmt_w; *h = s_fmt_h; *fps = s_fmt_fps;
    return s_connected && s_fmt_w;
}
uint32_t hl_capture_frames_total(void) { return s_frames_total; }
