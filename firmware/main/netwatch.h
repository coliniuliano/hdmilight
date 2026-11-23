/* Network watchdog: keeps re-joining Wi-Fi whenever the network is gone or stale (never restarts for that), and
 * restarts only if the board's own web server is wedged, which otherwise needs someone at the plug on a board
 * updated only over Wi-Fi. See netwatch.c. */
#pragma once
#include <stddef.h>
#include "esp_err.h"

esp_err_t hl_netwatch_start(void);
/* Why the board last restarted: the reset reason plus, if the watchdog did it, its reason (empty if none). */
void hl_netwatch_last_restart(char *buf, size_t len);
