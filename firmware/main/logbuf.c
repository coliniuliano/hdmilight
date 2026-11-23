#include "logbuf.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "crashlog.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static char              *s_buf;
static size_t             s_cap;
static uint64_t           s_pos;      /* absolute number of bytes ever written */
static SemaphoreHandle_t  s_mtx;
static vprintf_like_t     s_prev;

static int log_vprintf(const char *fmt, va_list ap)
{
    char line[256];
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    if (n > 0) hl_crash_log_append(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);   /* survives a crash */
    if (n > 0 && s_buf && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
        size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
        for (size_t i = 0; i < len; i++) s_buf[(s_pos + i) % s_cap] = line[i];
        s_pos += len;
        xSemaphoreGive(s_mtx);
    }
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void hl_log_init(size_t capacity)
{
    s_buf = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (!s_buf) return;
    s_cap = capacity;
    s_mtx = xSemaphoreCreateMutex();
    s_prev = esp_log_set_vprintf(log_vprintf);
}

size_t hl_log_read(uint64_t since, char *out, size_t cap, uint64_t *next)
{
    if (!s_buf || cap == 0) { if (next) *next = s_pos; return 0; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint64_t oldest = s_pos > s_cap ? s_pos - s_cap : 0;
    if (since < oldest) since = oldest;
    if (since > s_pos) since = s_pos;
    size_t n = (size_t)(s_pos - since);
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) out[i] = s_buf[(since + i) % s_cap];
    if (next) *next = since + n;
    xSemaphoreGive(s_mtx);
    return n;
}
