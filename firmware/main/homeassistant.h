/* Home Assistant: the strip as an ESPHome-API light (on/off + brightness), announced over mDNS so HA's built-in
 * ESPHome integration discovers it. Call after hl_net_start (mDNS must be up). */
#pragma once
#include "esp_err.h"
esp_err_t hl_homeassistant_start(void);
/* The hostname changed: announce the ESPHome service under the new name. */
void hl_homeassistant_set_name(const char *name);
/* Lowest free stack the API task has had, in bytes (-1 before it starts). */
int hl_homeassistant_stack_free(void);
