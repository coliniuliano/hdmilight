/* USB UVC capture: opens the first UVC device (MS2130 dongle) as MJPEG and
 * queues frames. Based on ESP-IDF's examples/peripherals/usb/host/uvc plus
 * the settings the P4 + MacroSilicon capture projects converged on. */
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "usb/uvc_host.h"

typedef struct {
    uvc_host_stream_hdl_t stream;
    const uvc_host_frame_t *frame;
} hl_capture_frame_t;

/* Start USB host + UVC driver + a task that (re)opens the stream forever. */
void hl_capture_start(void);
/* Newest queued frame (older ones are returned to the driver). */
bool hl_capture_take_newest(hl_capture_frame_t *out, TickType_t timeout_ticks);
void hl_capture_release(const hl_capture_frame_t *f);
bool hl_capture_connected(void);
/* The USB video mode the card agreed to (always MJPEG). False, with zeros, while no stream is open. */
bool hl_capture_format(int *w, int *h, float *fps);
uint32_t hl_capture_frames_total(void);

/* Some cards pad MJPEG frames after EOI; return the bounded JPEG length or 0. */
static inline size_t hl_jpeg_trim(const uint8_t *data, size_t size)
{
    if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) return 0;
    for (size_t end = size; end >= 4; --end)
        if (data[end - 2] == 0xff && data[end - 1] == 0xd9) return end;
    return 0;
}
