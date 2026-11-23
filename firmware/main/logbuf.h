/* Log ring buffer in PSRAM: everything ESP_LOG prints also lands here and is served by /api/log. */
#pragma once
#include <stddef.h>
#include <stdint.h>
void     hl_log_init(size_t capacity);
/* Copy log text written after absolute position `since` (0 = oldest kept). Returns bytes copied; *next is the
 * position to pass next time. Text older than the buffer is dropped silently. */
size_t   hl_log_read(uint64_t since, char *out, size_t cap, uint64_t *next);
