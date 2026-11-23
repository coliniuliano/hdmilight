#include "cec_core.h"
#include <string.h>

/* accepted low times, microseconds (the specification's limits with a little margin for interrupt latency) */
#define START_LOW_MIN 3350          /* specification: 3500..3900 */
#define START_LOW_MAX 4050
#define ONE_LOW_MIN   300           /* 400..800 */
#define ONE_LOW_MAX   950
#define ZERO_LOW_MIN  1150          /* 1300..1700 */
#define ZERO_LOW_MAX  1850
#define HIGH_MAX      2600          /* longest high time inside a frame (a "1" at the slowest allowed bit rate) */

void cec_rx_init(cec_rx *rx) { memset(rx, 0, sizeof *rx); }

static void drop(cec_rx *rx) { if (rx->in_frame) rx->errors++; rx->in_frame = false; }

bool cec_rx_edge(cec_rx *rx, int level, int64_t t, cec_frame *out)
{
    if (!level) {                                   /* falling edge: a bit begins */
        if (rx->in_frame && t - rx->t_rise > HIGH_MAX) drop(rx);          /* the sender stopped in the middle */
        rx->t_fall = t;
        return false;
    }
    int64_t low = t - rx->t_fall;                   /* rising edge: the low time says what the bit was */
    rx->t_rise = t;
    if (low >= START_LOW_MIN && low <= START_LOW_MAX) {
        if (rx->in_frame) rx->errors++;             /* a start bit in the middle of a frame: begin again */
        rx->in_frame = true; rx->bit = 0; rx->cur = 0; rx->eom = false; rx->f.len = 0; rx->f.acked = 0;
        return false;
    }
    if (!rx->in_frame) return false;
    int v;
    if (low >= ONE_LOW_MIN && low <= ONE_LOW_MAX) v = 1;
    else if (low >= ZERO_LOW_MIN && low <= ZERO_LOW_MAX) v = 0;
    else { drop(rx); return false; }
    if (rx->bit < 8) { rx->cur = (uint8_t)((rx->cur << 1) | v); rx->bit++; return false; }
    if (rx->bit == 8) { rx->eom = v; rx->bit = 9; return false; }
    /* acknowledge bit: the block is complete */
    rx->f.data[rx->f.len] = rx->cur;
    if (v == 0) rx->f.acked |= (uint16_t)(1u << rx->f.len);
    rx->f.len++;
    rx->bit = 0; rx->cur = 0;
    if (rx->eom || rx->f.len == CEC_MAX_BLOCKS) {
        rx->in_frame = false; rx->frames++;
        *out = rx->f;
        return true;
    }
    return false;
}

bool cec_rx_should_ack(const cec_rx *rx, uint8_t my_addr)
{
    if (!rx->in_frame || rx->bit != 9 || my_addr >= CEC_ADDR_NONE) return false;
    uint8_t hdr = rx->f.len ? rx->f.data[0] : rx->cur;       /* in the first block the header is still in `cur` */
    return (hdr & 0x0f) == my_addr && (hdr >> 4) != my_addr;
}

int cec_tx_build(const uint8_t *data, int len, cec_seg *segs)
{
    int n = 0;
    segs[n++] = (cec_seg){ CEC_START_LOW, CEC_START_TOTAL - CEC_START_LOW, CEC_CHK_NONE };
    for (int b = 0; b < len; b++) {
        for (int i = 0; i < 10; i++) {
            int v = i < 8 ? (data[b] >> (7 - i)) & 1 : i == 8 ? (b == len - 1) : 1;      /* data, end-of-message, acknowledge */
            uint16_t low = v ? CEC_ONE_LOW : CEC_ZERO_LOW;
            /* While this sender's own address goes out, a "1" that reads back low means another initiator with a
             * lower address is sending too and has priority. */
            uint8_t chk = (b == 0 && i < 4 && v) ? CEC_CHK_ARB : CEC_CHK_NONE;
            segs[n++] = (cec_seg){ low, (uint16_t)(CEC_BIT_TOTAL - low), chk };
        }
    }
    return n;
}

bool cec_tx_matches(const cec_frame *heard, const uint8_t *sent, int len)
{
    return heard->len == len && memcmp(heard->data, sent, (size_t)len) == 0;
}

/* ---- policy -------------------------------------------------------------------------------------------- */
enum {
    OP_FEATURE_ABORT = 0x00, OP_IMAGE_VIEW_ON = 0x04, OP_TEXT_VIEW_ON = 0x0d, OP_SET_MENU_LANGUAGE = 0x32, OP_STANDBY = 0x36,
    OP_USER_CONTROL_PRESSED = 0x44, OP_GIVE_OSD_NAME = 0x46, OP_ROUTING_CHANGE = 0x80, OP_ACTIVE_SOURCE = 0x82,
    OP_GIVE_PHYSICAL_ADDRESS = 0x83, OP_REQUEST_ACTIVE_SOURCE = 0x85, OP_SET_STREAM_PATH = 0x86, OP_GIVE_VENDOR_ID = 0x8c,
    OP_GIVE_POWER_STATUS = 0x8f, OP_REPORT_POWER_STATUS = 0x90, OP_GET_MENU_LANGUAGE = 0x91, OP_CEC_VERSION = 0x9e,
    OP_GET_CEC_VERSION = 0x9f, OP_ABORT = 0xff,
};

void cec_policy_init(cec_policy *p) { memset(p, 0, sizeof *p); p->my_addr = CEC_ADDR_NONE; }

const char *cec_tv_state_name(cec_tv_state s) { return s == CEC_TV_ON ? "on" : s == CEC_TV_STANDBY ? "standby" : "unknown"; }

bool cec_policy_frame(cec_policy *p, const cec_frame *f, cec_frame *reply, bool *changed)
{
    cec_tv_state before = p->tv;
    bool send = false;
    if (changed) *changed = false;
    p->heard++;
    if (f->len < 2) return false;                         /* a poll: only an address being tested */
    uint8_t src = cec_src(f), dst = cec_dst(f), op = f->data[1];

    if (op == OP_STANDBY && (dst == CEC_ADDR_TV || dst == CEC_ADDR_NONE)) {
        p->tv = CEC_TV_STANDBY;                           /* the TV, or everything, is being switched off */
    } else if (src == CEC_ADDR_TV) {
        p->last_op_from_tv = op;
        if (op == OP_REPORT_POWER_STATUS && f->len >= 3)
            p->tv = (f->data[2] == 0 || f->data[2] == 2) ? CEC_TV_ON : CEC_TV_STANDBY;   /* 0 on, 1 standby, 2 waking, 3 going to standby */
        else if (op == OP_ROUTING_CHANGE || op == OP_ACTIVE_SOURCE || op == OP_REQUEST_ACTIVE_SOURCE || op == OP_SET_STREAM_PATH ||
                 op == OP_SET_MENU_LANGUAGE || op == OP_USER_CONTROL_PRESSED)
            p->tv = CEC_TV_ON;                            /* things only an awake TV says */
    } else if (dst == CEC_ADDR_TV && (op == OP_IMAGE_VIEW_ON || op == OP_TEXT_VIEW_ON)) {
        p->tv = CEC_TV_ON;                                /* a source waking the TV */
    }

    /* requests addressed to this board, once it has an address */
    if (p->my_addr < CEC_ADDR_NONE && dst == p->my_addr && src != p->my_addr && reply) {
        reply->len = 0; reply->acked = 0;
        reply->data[0] = (uint8_t)((p->my_addr << 4) | src);
        switch (op) {
        case OP_GIVE_POWER_STATUS: reply->data[1] = OP_REPORT_POWER_STATUS; reply->data[2] = 0; reply->len = 3; break;
        case OP_GET_CEC_VERSION:   reply->data[1] = OP_CEC_VERSION; reply->data[2] = 0x05; reply->len = 3; break;       /* 1.4 */
        case OP_GIVE_PHYSICAL_ADDRESS: case OP_GIVE_OSD_NAME: case OP_GIVE_VENDOR_ID: case OP_GET_MENU_LANGUAGE:
            /* this board is not a source or a display and has no place in the TV's device list */
            reply->data[1] = OP_FEATURE_ABORT; reply->data[2] = op; reply->data[3] = 0; reply->len = 4; break;          /* unrecognised opcode */
        case OP_ABORT:
            reply->data[1] = OP_FEATURE_ABORT; reply->data[2] = op; reply->data[3] = 4; reply->len = 4; break;          /* refused */
        default: break;
        }
        send = reply->len != 0;
    }
    if (changed) *changed = p->tv != before;
    return send;
}
