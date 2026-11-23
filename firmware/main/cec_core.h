/* HDMI-CEC without the hardware: the bit decoder (bus edges in, frames out), the bit timings of a frame to send,
 * and what the traffic says about the TV's power state. No ESP dependencies: cec.c drives it from a GPIO interrupt
 * and a timer, tools/cec_test runs it on the host.
 *
 * CEC is one open-drain wire. Every bit starts with a falling edge; the length of the low time is the value:
 * start bit 3.7 ms low of 4.5 ms, "1" 0.6 ms low of 2.4 ms, "0" 1.5 ms low of 2.4 ms. A frame is a start bit and
 * up to 16 blocks of 8 data bits (MSB first), an end-of-message bit and an acknowledge bit, which the sender
 * leaves as "1" and the addressed receiver stretches into a "0". The first block is (initiator << 4) | destination;
 * destination 15 is everyone. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CEC_MAX_BLOCKS   16
#define CEC_ADDR_TV      0
#define CEC_ADDR_FREE    14        /* "specific use": the address this board takes when it is allowed to transmit */
#define CEC_ADDR_NONE    15        /* unregistered: listens only (also the broadcast destination) */

/* nominal times in microseconds */
#define CEC_START_LOW    3700
#define CEC_START_TOTAL  4500
#define CEC_ONE_LOW      600
#define CEC_ZERO_LOW     1500
#define CEC_BIT_TOTAL    2400
#define CEC_SAMPLE_AT    1050      /* after the falling edge: where a bit is read */
#define CEC_ACK_LOW      1500      /* how long a receiver holds the acknowledge bit low */
#define CEC_FREE_NEW_US  (7 * CEC_BIT_TOTAL)    /* bus idle time before a new initiator may send */
#define CEC_FREE_RETRY_US (3 * CEC_BIT_TOTAL)   /* ...before retrying a failed frame */

typedef struct {
    uint8_t  data[CEC_MAX_BLOCKS];
    uint8_t  len;
    uint16_t acked;               /* bit n: block n had its acknowledge bit pulled low */
} cec_frame;

static inline uint8_t cec_src(const cec_frame *f) { return f->data[0] >> 4; }
static inline uint8_t cec_dst(const cec_frame *f) { return f->data[0] & 0x0f; }

/* ---- receive ---- */
typedef struct {
    bool      in_frame;
    int64_t   t_fall, t_rise;     /* time of the last falling / rising edge */
    uint8_t   bit;                /* 0..7 data, 8 end-of-message, 9 acknowledge */
    uint8_t   cur;
    bool      eom;
    cec_frame f;
    uint32_t  frames, errors;     /* counters */
} cec_rx;

void cec_rx_init(cec_rx *rx);
/* Feed every edge of the bus: level = the level after the edge (0 low, 1 high), t_us = when. Returns true when a
 * frame is complete, copied to *out. */
bool cec_rx_edge(cec_rx *rx, int level, int64_t t_us, cec_frame *out);
/* Right after a falling edge: is this the acknowledge bit of a block addressed to `my_addr` by someone else?
 * If so the caller holds the bus low for CEC_ACK_LOW. */
bool cec_rx_should_ack(const cec_rx *rx, uint8_t my_addr);

/* ---- transmit ---- */
typedef struct {
    uint16_t low_us, high_us;     /* drive low for low_us, then release for high_us */
    uint8_t  check;               /* CEC_CHK_*: read the bus CEC_SAMPLE_AT after the falling edge */
} cec_seg;
enum { CEC_CHK_NONE = 0, CEC_CHK_ARB = 1 /* a low here means another initiator won: stop */ };
#define CEC_MAX_SEGS (1 + CEC_MAX_BLOCKS * 10)
/* the bit timings for a frame; returns the number of segments */
int cec_tx_build(const uint8_t *data, int len, cec_seg *segs);
/* did the bus carry exactly what was sent (as the receiver heard it)? */
bool cec_tx_matches(const cec_frame *heard, const uint8_t *sent, int len);

/* ---- what the traffic means ---- */
typedef enum { CEC_TV_UNKNOWN = 0, CEC_TV_ON, CEC_TV_STANDBY } cec_tv_state;
typedef struct {
    cec_tv_state tv;
    uint8_t      my_addr;         /* CEC_ADDR_NONE while only listening */
    uint8_t      last_op_from_tv;
    uint32_t     heard;           /* frames seen */
} cec_policy;

void cec_policy_init(cec_policy *p);
/* Take a frame from the bus. Returns true if `reply` should be sent (only ever when my_addr is a real address and
 * the frame was addressed to it). *changed is set when the TV state changed. */
bool cec_policy_frame(cec_policy *p, const cec_frame *f, cec_frame *reply, bool *changed);
const char *cec_tv_state_name(cec_tv_state s);
