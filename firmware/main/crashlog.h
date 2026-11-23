/* Crash capture that survives the reboot, for a board updated only over Wi-Fi (no serial console to read).
 *
 * A link-time wrap of esp_panic_handler (every panic, abort() and watchdog panic goes through it) saves the reason,
 * the abort() message, all CPU registers and 64 words of the crashing stack into .noinit RAM, which a software or
 * panic reset leaves alone. The last 4 KiB of log text are mirrored there continuously. On the next boot the record is
 * moved to normal RAM and served by /api/crash; tools/crash_decode.py turns the addresses into source lines using the
 * matching ELF. A power cut clears it. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HL_CRASH_LOG_CAP   4096
#define HL_CRASH_STACK_WDS 64

typedef struct {
    uint32_t elf_sha;              /* first 4 bytes of the crashed image's ELF SHA-256 (match it to the ELF) */
    int32_t  core, exception;
    uint32_t reason_ptr, desc_ptr; /* addresses of the reason/description strings (look them up in the ELF) */
    char     abort_msg[96];        /* "abort() was called at PC ..." when it was an abort */
    uint32_t addr;                 /* faulting instruction */
    uint32_t mepc, ra, sp, gp, tp, mstatus, mcause, mtval;
    uint32_t a[8], s[12], t[7];
    uint32_t stack[HL_CRASH_STACK_WDS];
    uint32_t stack_words;          /* 0 if the stack pointer was not safe to read */
    uint32_t log_len;
    char     log[HL_CRASH_LOG_CAP]; /* the last log text before the crash, oldest first */
} hl_crash_report_t;

/* Call first thing in app_main, before hl_log_init. */
void hl_crash_init(void);
/* Mirror log text into the .noinit ring (called by logbuf for every line). */
void hl_crash_log_append(const char *s, size_t n);
/* The crash that caused this boot, or NULL. */
const hl_crash_report_t *hl_crash_last(void);
