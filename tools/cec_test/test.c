/* Host test of firmware/main/cec_core.c: frames are turned into bus edges with cec_tx_build, optionally with a
 * receiver stretching the acknowledge bits, timing errors and noise, and fed back through the decoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cec_core.h"

static int fails;
static void expect(const char *what, int ok) { printf("%-70s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

typedef struct { int spec_random; int jitter; int ack; int cut_after; int bad_seg; } opts;
static const opts NOMINAL = { 0, 0, 0, -1, -1 };
static int between(int lo, int hi) { return lo + rand() % (hi - lo + 1); }

/* Put a frame on the simulated bus starting at *t and run the decoder over it. Returns frames decoded (0 or 1);
 * ack_edges counts the falling edges at which cec_rx_should_ack(my) was true. */
static int play(cec_rx *rx, const uint8_t *d, int len, opts o, int64_t *t, cec_frame *out, uint8_t my, int *ack_edges)
{
    cec_seg segs[CEC_MAX_SEGS]; int n = cec_tx_build(d, len, segs), got = 0;
    for (int i = 0; i < n; i++) {
        if (o.cut_after >= 0 && i >= o.cut_after) break;
        int low = segs[i].low_us, total = segs[i].low_us + segs[i].high_us;
        int is_ack = i > 0 && (i - 1) % 10 == 9;
        if (is_ack && o.ack) low = CEC_ACK_LOW;                       /* the receiver holds it low */
        if (o.spec_random) {                                          /* anywhere inside the specification's limits */
            if (i == 0) { low = between(3500, 3900); total = between(4300, 4700); }
            else { low = low == CEC_ONE_LOW ? between(400, 800) : between(1300, 1700); total = between(2050, 2750); }
        }
        if (i == o.bad_seg) low = 1050;                               /* neither a 0 nor a 1 */
        int j1 = o.jitter ? rand() % (2 * o.jitter + 1) - o.jitter : 0, j2 = o.jitter ? rand() % (2 * o.jitter + 1) - o.jitter : 0;
        cec_frame f;
        if (cec_rx_edge(rx, 0, *t + j1, &f)) { got++; *out = f; }
        if (ack_edges && cec_rx_should_ack(rx, my)) (*ack_edges)++;
        if (cec_rx_edge(rx, 1, *t + low + j2, &f)) { got++; *out = f; }
        *t += total;
    }
    *t += 20000;                                                      /* idle bus between frames */
    return got;
}

int main(void)
{
    cec_rx rx; cec_frame f; int64_t t = 1000000; int n;
    cec_rx_init(&rx); srand(7);

    const uint8_t standby[] = { 0x0f, 0x36 };                         /* TV -> everyone: standby */
    n = play(&rx, standby, 2, NOMINAL, &t, &f, CEC_ADDR_NONE, NULL);
    expect("broadcast frame decodes", n == 1 && f.len == 2 && !memcmp(f.data, standby, 2));
    expect("...with no acknowledge", f.acked == 0);

    const uint8_t ask[] = { 0xe0, 0x8f };                             /* 14 -> TV: give power status */
    opts acked = NOMINAL; acked.ack = 1;
    n = play(&rx, ask, 2, acked, &t, &f, CEC_ADDR_NONE, NULL);
    expect("directed frame decodes with both blocks acknowledged", n == 1 && f.len == 2 && !memcmp(f.data, ask, 2) && f.acked == 3);
    n = play(&rx, ask, 2, NOMINAL, &t, &f, CEC_ADDR_NONE, NULL);
    expect("the same frame with nobody answering shows no acknowledge", n == 1 && f.acked == 0);

    uint8_t big[16]; for (int i = 0; i < 16; i++) big[i] = (uint8_t)(0x4f + i * 37);
    n = play(&rx, big, 16, NOMINAL, &t, &f, CEC_ADDR_NONE, NULL);
    expect("a 16-block frame decodes", n == 1 && f.len == 16 && !memcmp(f.data, big, 16));

    int ok = 1;
    for (int k = 0; k < 400; k++) {
        opts o = NOMINAL; o.spec_random = 1; o.jitter = 40; o.ack = k & 1;
        uint8_t d[4] = { (uint8_t)(k * 17 | 0x0f), (uint8_t)(k * 29), (uint8_t)(k * 53), (uint8_t)k };
        if (o.ack) d[0] = (uint8_t)((d[0] & 0xf0) | 4);
        if (play(&rx, d, 1 + k % 4, o, &t, &f, CEC_ADDR_NONE, NULL) != 1 || f.len != 1 + k % 4 || memcmp(f.data, d, f.len)) ok = 0;
    }
    expect("400 frames with every bit anywhere in the specified limits, +/-40 us jitter", ok);

    uint32_t errs = rx.errors;
    opts bad = NOMINAL; bad.bad_seg = 7;
    n = play(&rx, standby, 2, bad, &t, &f, CEC_ADDR_NONE, NULL);
    expect("a bit that is neither 0 nor 1 drops the frame", n == 0 && rx.errors == errs + 1);
    opts cut = NOMINAL; cut.cut_after = 13;
    n = play(&rx, standby, 2, cut, &t, &f, CEC_ADDR_NONE, NULL);
    expect("a frame that stops half-way yields nothing", n == 0);
    n = play(&rx, standby, 2, NOMINAL, &t, &f, CEC_ADDR_NONE, NULL);
    expect("...and the next frame decodes", n == 1 && f.len == 2 && !memcmp(f.data, standby, 2));

    int e;
    const uint8_t to_me[] = { 0x0e, 0x8f };                           /* TV -> 14 */
    e = 0; play(&rx, to_me, 2, acked, &t, &f, 14, &e);
    expect("a frame for this address is acknowledged at its two acknowledge bits only", e == 2);
    const uint8_t to_other[] = { 0x04, 0x8f };
    e = 0; play(&rx, to_other, 2, acked, &t, &f, 14, &e);
    expect("a frame for another address is not acknowledged", e == 0);
    e = 0; play(&rx, standby, 2, NOMINAL, &t, &f, 14, &e);
    expect("a broadcast is not acknowledged", e == 0);
    const uint8_t own[] = { 0xee };
    e = 0; play(&rx, own, 1, NOMINAL, &t, &f, 14, &e);
    expect("this board's own poll of its address is not acknowledged by itself", e == 0);
    e = 0; play(&rx, to_me, 2, acked, &t, &f, CEC_ADDR_NONE, &e);
    expect("a listen-only board acknowledges nothing", e == 0);

    cec_seg segs[CEC_MAX_SEGS];
    n = cec_tx_build(ask, 2, segs);
    int arb = 0; for (int i = 0; i < n; i++) arb += segs[i].check == CEC_CHK_ARB;
    expect("two blocks are a start bit and 20 bits", n == 21 && segs[0].low_us == 3700 && segs[0].high_us == 800);
    expect("arbitration is checked on the 1 bits of the initiator address (e = 1110)", arb == 3 && segs[1].check && segs[2].check && segs[3].check && !segs[4].check);
    expect("end-of-message is 0 on the first block and 1 on the last", segs[9].low_us == 1500 && segs[19].low_us == 600);
    expect("the sender leaves both acknowledge bits as 1", segs[10].low_us == 600 && segs[20].low_us == 600);
    expect("cec_tx_matches", cec_tx_matches(&(cec_frame){ { 0xe0, 0x8f }, 2, 3 }, ask, 2) && !cec_tx_matches(&(cec_frame){ { 0xe0, 0x8e }, 2, 3 }, ask, 2));

    /* what the traffic means */
    cec_policy p; cec_frame r; bool ch;
    #define FR(...) (&(cec_frame){ { __VA_ARGS__ }, (uint8_t)sizeof((uint8_t[]){ __VA_ARGS__ }), 0 })
    cec_policy_init(&p);
    expect("state starts unknown", p.tv == CEC_TV_UNKNOWN);
    cec_policy_frame(&p, FR(0x44), &r, &ch);
    expect("a poll changes nothing", p.tv == CEC_TV_UNKNOWN && !ch);
    cec_policy_frame(&p, FR(0x40, 0x04), &r, &ch);
    expect("a source sending Image View On to the TV: on", p.tv == CEC_TV_ON && ch);
    cec_policy_frame(&p, FR(0x0f, 0x36), &r, &ch);
    expect("the TV broadcasting Standby: standby", p.tv == CEC_TV_STANDBY && ch);
    cec_policy_frame(&p, FR(0x0f, 0x85), &r, &ch);
    expect("the TV asking for the active source: on", p.tv == CEC_TV_ON);
    cec_policy_frame(&p, FR(0x40, 0x36), &r, &ch);
    expect("a source sending Standby to the TV: standby", p.tv == CEC_TV_STANDBY);
    cec_policy_frame(&p, FR(0x04, 0x90, 0x00), &r, &ch);
    expect("the TV reporting power status on: on", p.tv == CEC_TV_ON);
    cec_policy_frame(&p, FR(0x04, 0x90, 0x01), &r, &ch);
    expect("...standby: standby", p.tv == CEC_TV_STANDBY);
    cec_policy_frame(&p, FR(0x04, 0x90, 0x02), &r, &ch);
    expect("...waking up: on", p.tv == CEC_TV_ON);
    cec_policy_frame(&p, FR(0x04, 0x90, 0x03), &r, &ch);
    expect("...going to standby: standby", p.tv == CEC_TV_STANDBY);
    cec_policy_frame(&p, FR(0x4f, 0x82, 0x10, 0x00), &r, &ch);
    expect("a source announcing itself active does not by itself mean the TV is on", p.tv == CEC_TV_STANDBY);
    cec_policy_frame(&p, FR(0x45, 0x36), &r, &ch);
    cec_policy_frame(&p, FR(0x0f, 0x80, 0x00, 0x00, 0x10, 0x00), &r, &ch);
    expect("Standby between two other devices is ignored; a routing change from the TV: on", p.tv == CEC_TV_ON);
    expect("listen-only: never a reply", !cec_policy_frame(&p, FR(0x0e, 0x8f), &r, &ch) && !cec_policy_frame(&p, FR(0x0f, 0x8f), &r, &ch));
    p.my_addr = CEC_ADDR_FREE;
    expect("with an address: Give Power Status is answered with on", cec_policy_frame(&p, FR(0x0e, 0x8f), &r, &ch) && r.len == 3 && r.data[0] == 0xe0 && r.data[1] == 0x90 && r.data[2] == 0);
    expect("...Get CEC Version with 1.4", cec_policy_frame(&p, FR(0x0e, 0x9f), &r, &ch) && r.len == 3 && r.data[1] == 0x9e && r.data[2] == 5);
    expect("...Give Physical Address with Feature Abort", cec_policy_frame(&p, FR(0x0e, 0x83), &r, &ch) && r.len == 4 && r.data[0] == 0xe0 && r.data[1] == 0x00 && r.data[2] == 0x83);
    expect("...a request for someone else is not answered", !cec_policy_frame(&p, FR(0x04, 0x8f), &r, &ch));
    expect("...a broadcast is not answered", !cec_policy_frame(&p, FR(0x0f, 0x83), &r, &ch));
    expect("...Standby addressed to this board is not answered", !cec_policy_frame(&p, FR(0x0e, 0x36), &r, &ch));

    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
