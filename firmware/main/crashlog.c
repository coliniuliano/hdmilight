#include "crashlog.h"
#include <string.h>
#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"

static const char *TAG = "crashlog";
#define MAGIC_LOG   0x484c4c47u   /* "HLLG": log ring initialised */
#define MAGIC_CRASH 0x484c4352u   /* "HLCR": a panic was captured */

/* Everything here lives in .noinit: not cleared at startup, kept across software and panic resets. */
typedef struct {
    uint32_t log_magic;
    uint32_t log_pos;              /* total bytes ever appended (ring index = pos % cap) */
    char     log[HL_CRASH_LOG_CAP];
    uint32_t crash_magic;
    uint32_t elf_sha;
    hl_crash_report_t r;           /* r.log is filled at the next boot, from the ring */
} store_t;
static __NOINIT_ATTR store_t s_st;

static hl_crash_report_t *s_last;  /* heap copy for this boot, if the previous one crashed */

void hl_crash_log_append(const char *s, size_t n)
{
    if (s_st.log_magic != MAGIC_LOG) return;
    for (size_t i = 0; i < n; i++) s_st.log[(s_st.log_pos + i) % HL_CRASH_LOG_CAP] = s[i];
    s_st.log_pos += n;
}

const hl_crash_report_t *hl_crash_last(void) { return s_last; }

void hl_crash_init(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    bool warm = rr != ESP_RST_POWERON && rr != ESP_RST_BROWNOUT && rr != ESP_RST_UNKNOWN;
    if (warm && s_st.crash_magic == MAGIC_CRASH) {
        s_last = heap_caps_calloc(1, sizeof *s_last, MALLOC_CAP_SPIRAM);
        if (s_last) {
            *s_last = s_st.r;
            if (s_st.log_magic == MAGIC_LOG) {   /* linearise the ring, oldest first */
                uint32_t len = s_st.log_pos < HL_CRASH_LOG_CAP ? s_st.log_pos : HL_CRASH_LOG_CAP;
                uint32_t start = s_st.log_pos - len;
                for (uint32_t i = 0; i < len; i++) s_last->log[i] = s_st.log[(start + i) % HL_CRASH_LOG_CAP];
                s_last->log_len = len;
            }
        }
    }
    /* fresh state for this boot */
    s_st.crash_magic = 0;
    s_st.log_magic = MAGIC_LOG;
    s_st.log_pos = 0;
    const uint8_t *sha = esp_app_get_description()->app_elf_sha256;
    s_st.elf_sha = ((uint32_t)sha[0] << 24) | ((uint32_t)sha[1] << 16) | ((uint32_t)sha[2] << 8) | sha[3];
    if (s_last)
        ESP_LOGE(TAG, "previous boot crashed: core %d, mepc 0x%08lx, ra 0x%08lx, mcause 0x%lx%s%s (details at /api/crash)",
                 (int)s_last->core, (unsigned long)s_last->mepc, (unsigned long)s_last->ra, (unsigned long)s_last->mcause,
                 s_last->abort_msg[0] ? ", " : "", s_last->abort_msg);
}

/* ---- the panic hook: runs in a crashed system, so IRAM only, no flash, no locks, no libc beyond ROM ---- */
void __real_esp_panic_handler(panic_info_t *info);
extern char *g_panic_abort_details;

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    hl_crash_report_t *r = &s_st.r;
    r->elf_sha = s_st.elf_sha;
    r->core = info->core;
    r->exception = info->exception;
    r->reason_ptr = (uint32_t)info->reason;
    r->desc_ptr = (uint32_t)info->description;
    r->addr = (uint32_t)info->addr;
    r->abort_msg[0] = 0;
    const char *ad = g_panic_abort_details;
    if (ad && esp_ptr_internal(ad)) {
        size_t i = 0;
        for (; i < sizeof r->abort_msg - 1 && ad[i]; i++) r->abort_msg[i] = ad[i];
        r->abort_msg[i] = 0;
    }
    const RvExcFrame *f = (const RvExcFrame *)info->frame;
    r->stack_words = 0;
    if (f && esp_ptr_internal(f)) {
        r->mepc = f->mepc; r->ra = f->ra; r->sp = f->sp; r->gp = f->gp; r->tp = f->tp;
        r->mstatus = f->mstatus; r->mcause = f->mcause; r->mtval = f->mtval;
        r->a[0] = f->a0; r->a[1] = f->a1; r->a[2] = f->a2; r->a[3] = f->a3;
        r->a[4] = f->a4; r->a[5] = f->a5; r->a[6] = f->a6; r->a[7] = f->a7;
        r->s[0] = f->s0; r->s[1] = f->s1; r->s[2] = f->s2; r->s[3] = f->s3; r->s[4] = f->s4; r->s[5] = f->s5;
        r->s[6] = f->s6; r->s[7] = f->s7; r->s[8] = f->s8; r->s[9] = f->s9; r->s[10] = f->s10; r->s[11] = f->s11;
        r->t[0] = f->t0; r->t[1] = f->t1; r->t[2] = f->t2; r->t[3] = f->t3; r->t[4] = f->t4; r->t[5] = f->t5;
        r->t[6] = f->t6;
        /* stack words, only if the whole window is internal RAM (a bad sp would fault again here) */
        const uint32_t *sp = (const uint32_t *)(f->sp & ~3u);
        if (esp_ptr_internal(sp) && esp_ptr_internal(sp + HL_CRASH_STACK_WDS - 1)) {
            for (int i = 0; i < HL_CRASH_STACK_WDS; i++) r->stack[i] = sp[i];
            r->stack_words = HL_CRASH_STACK_WDS;
        }
    }
    s_st.crash_magic = MAGIC_CRASH;
    __real_esp_panic_handler(info);
}
