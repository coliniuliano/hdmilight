/* MS2130 capture chip helpers of the oneboard build: the flash image check (ms2130_image.c) and reading or
 * writing the chip's boot flash over USB (ms2130_usb.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define MS_FLASH_IMAGE_MAX 65536

/* header + both checksums of an MS2130 flash image; `why` gets the reason or a one-line summary */
bool hl_ms2130_image_ok(const uint8_t *img, size_t len, char *why, size_t why_len);

typedef struct {
    enum { HL_MSUSB_IDLE = 0, HL_MSUSB_RUNNING, HL_MSUSB_DONE, HL_MSUSB_FAILED } state;
    bool     write;               /* the current or last job: write (else read) */
    char     stage[12];           /* "erasing", "writing", "verifying", "reading" */
    int      pct;
    char     msg[160];            /* result of the last job */
    uint16_t vid, pid;            /* the device the last job talked to */
    bool     have_dump;           /* a finished read is waiting at /api/ms2130/usb/dump */
} hl_msusb_job_t;

/* Start a job in the background: read the 64 KB flash, or (img != NULL) erase, write and verify an image, which
 * must pass hl_ms2130_image_ok. ESP_ERR_INVALID_STATE while another job runs. */
esp_err_t hl_msusb_start(const uint8_t *img, size_t len);
void      hl_msusb_status(hl_msusb_job_t *j);
size_t    hl_msusb_dump(const uint8_t **data);          /* the last finished read, 0 if none */
esp_err_t hl_msusb_register_http(void *httpd);          /* /api/ms2130/usb, /api/ms2130/usb/dump */
/* one line per connected USB device ("addr 1  345f:2130"); returns how many, -1 while a job is running */
int       hl_msusb_list(char *out, size_t len);
