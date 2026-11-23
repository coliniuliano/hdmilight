/* Replay harness: the rewritten SDK is driven exactly like the emulated firmware (tools/emu.py) and every I2C
 * transaction is checked against the recorded trace; reads return the recorded bytes. */
#pragma once
#include <stdint.h>
#include "gsv_platform.h"
void harness_load(const char *trace_path);
void harness_mark(const char *text);          /* must match the trace's "# text" lines in order */
void harness_done(void);                      /* fails if trace lines remain */
extern const gsv_platform harness_platform;
