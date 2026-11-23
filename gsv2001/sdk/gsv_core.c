/* GSV2001 SDK core: register access, objects, linking, events, the per-tick dispatcher. */
#include "gsv_sdk.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gsv_platform s_pf;
gsv_port *g_gsv_port_list, *g_gsv_port_tail;
gsv_port *g_chip_state[2];

void gsv_platform_init(const gsv_platform *p) { s_pf = *p; }

__attribute__((weak)) void gsv_log(const char *fmt, ...) { (void)fmt; }
__attribute__((weak)) void gsv_delay_ms(unsigned ms) { (void)ms; }
__attribute__((weak)) void gsv_app_tx_event(int which, int on) { (void)which; (void)on; }
__attribute__((weak)) void gsv_todo(const char *what, const char *where)
{
    fprintf(stderr, "GSV_TODO: %s (in %s)\n", what, where); abort();
}

/* ---- register access ------------------------------------------------------------------------------------ */
/* 0x0800e07c / 0x0800e038: the hooks unpack the descriptor into (device address, page<<8 | reg) */
/* GSV2001: an output in pass-through mode has no Tx core (index 0xff), so the by-index descriptor helpers give 0 for
 * it. The Gsv2k1 driver checks the index before every access to the core's pages; here such an access is dropped. */
static int hook_write(uint32_t dev, uint8_t reg, uint8_t *data, uint16_t len)
{
    if (dev == 0) return 0;
    return s_pf.write(GSV_DEV_ADDR(dev), (uint16_t)((GSV_DEV_PAGE(dev) << 8) | reg), data, len);
}
static int hook_read(uint32_t dev, uint8_t reg, uint8_t *data, uint16_t len)
{
    if (dev == 0) { memset(data, 0, len); return 0; }
    return s_pf.read(GSV_DEV_ADDR(dev), (uint16_t)((GSV_DEV_PAGE(dev) << 8) | reg), data, len);
}

int gsv_write_bits(uint32_t dev, uint8_t reg, uint8_t mask, uint8_t shift, uint8_t value)   /* 0x08007800 */
{
    uint8_t v = value;
    if (mask != 0xff) {
        hook_read(dev, reg, &v, 1);
        v = (uint8_t)((v & ~mask) | ((value << shift) & mask));
    }
    return hook_write(dev, reg, &v, 1);
}

int gsv_read_bits(uint32_t dev, uint8_t reg, uint8_t mask, uint8_t shift, uint8_t *out)      /* 0x08007688 */
{
    int rc = hook_read(dev, reg, out, 1);
    *out = (uint8_t)((*out & mask) >> shift);
    return rc;
}

int gsv_read_field(uint32_t dev, uint32_t reg, uint8_t first_mask, uint8_t last_mask, uint8_t shift, int msb_first, int len, uint32_t *out)   /* 0x080075a8 */
{
    uint8_t buf[8];
    *out = 0;
    int rc = hook_read(dev, (uint8_t)reg, buf, (uint16_t)len);
    const uint8_t last = (uint8_t)(len - 1);
    if (msb_first == 1) {
        buf[0] &= first_mask; buf[last] &= last_mask;
        uint8_t sh = (uint8_t)(last * 8 - shift);
        for (uint8_t i = 0; i < last; i++) { *out |= (uint32_t)buf[i] << sh; sh = (uint8_t)(sh - 8); }
        *out |= (uint32_t)buf[last] >> shift;
    } else {
        buf[0] &= last_mask; buf[last] &= first_mask;
        uint8_t sh = (uint8_t)(last * 8 - shift);
        for (uint8_t i = last; i != 0; i--) { *out |= (uint32_t)buf[i] << sh; sh = (uint8_t)(sh - 8); }
        *out |= (uint32_t)buf[0] >> shift;
    }
    return rc;
}

int gsv_write_field(uint32_t dev, uint32_t reg, uint8_t first_mask, uint8_t last_mask, uint8_t shift, int msb_first, int len, uint32_t value)  /* 0x080076f0 */
{
    uint8_t buf[12] = { 0 };
    hook_read(dev, (uint8_t)reg, buf, (uint16_t)len);
    const int last = len - 1;
    if (msb_first == 1) {
        buf[0] = (uint8_t)((buf[0] & ~first_mask) | ((value >> ((last * 8 - shift) & 0xff)) & first_mask));
        for (int i = 1; i < last; i++) buf[i] = (uint8_t)(value >> (((len - i - 1) * 8 - shift) & 0xff));
        buf[last] = (uint8_t)((buf[last] & ~last_mask) | ((value << shift) & last_mask));
    } else {
        buf[0] = (uint8_t)((buf[0] & ~last_mask) | ((value << shift) & last_mask));
        for (int i = 1; i < last; i++) buf[i] = (uint8_t)(value >> (((len - i - 1) * 8 - shift) & 0xff));
        buf[last] = (uint8_t)((buf[last] & ~first_mask) | ((value >> ((last * 8 - shift) & 0xff)) & first_mask));
    }
    return hook_write(dev, (uint8_t)reg, buf, (uint16_t)len);
}

/* ---- device descriptors --------------------------------------------------------------------------------- */
static uint32_t by_idx(gsv_port *p, uint8_t page) { return p->idx == 0 ? GSV_PAGE(page) : p->idx == 1 ? GSV_PAGE(page + 0x10) : 0; }
static uint32_t by_rxid(gsv_port *p, uint8_t page) { return p->id == 0 ? GSV_PAGE(page) : p->id == 1 ? GSV_PAGE(page + 0x10) : 0; }
static uint32_t by_txid(gsv_port *p, uint8_t page) { return p->id == 4 ? GSV_PAGE(page) : p->id == 5 ? GSV_PAGE(page + 0x10) : 0; }
uint32_t gsv_rx_dev(gsv_port *p)      { return by_idx(p, 0x40); }   /* 0x0800ea78 */
uint32_t gsv_tx_dev42(gsv_port *p)    { return by_idx(p, 0x42); }   /* 0x0800eadc */
uint32_t gsv_rx_dev21(gsv_port *p)    { return by_idx(p, 0x21); }   /* 0x0800e960 */
uint32_t gsv_rx_dev26(gsv_port *p)    { return by_idx(p, 0x26); }   /* 0x0800ea34 */
uint32_t gsv_rx_dev27(gsv_port *p)    { return by_rxid(p, 0x27); }  /* 0x0800ea14 */
uint32_t gsv_tx_dev(gsv_port *p)      { return by_txid(p, 0x60); }  /* 0x0800eabc */
uint32_t gsv_tx_dev2(gsv_port *p)     { return by_txid(p, 0x62); }  /* 0x0800ea58 */

/* ---- lifecycle ------------------------------------------------------------------------------------------ */
int gsv_chip_create(gsv_chip *c, uint8_t a, uint8_t b, uint32_t *dev_base, gsv_port *first, uint32_t x)   /* 0x08006590 */
{
    (void)x;
    c->c0 = a; c->c1 = b; c->dev_base = dev_base; c->first_port = first; c->state = NULL;
    return 0;
}

/* 0x080067b0: clear the port and attach the content blocks its kind needs (the original bump-allocates them) */
static void port_alloc(gsv_port *p)
{
    p->idx = p->b77 = 0xff; if (p->bypass != 1) p->bypass = 0xff; if (p->no_hdcp != 1) p->no_hdcp = 0xff;
    p->flags = p->plug_state = p->st10 = p->rx_state = p->st18 = p->hdcp_state = p->st20 = p->tx_state = p->st28 = p->st2c = p->st30 = NULL;
    p->rx = NULL; p->status = NULL; p->hdcp = NULL; p->hdcp_tx = NULL; p->video = NULL; p->audio = NULL; p->scaler = NULL; p->color = NULL;
    p->chain = NULL; p->p58 = NULL; p->source = p->downstream2 = p->downstream = p->audio_source = p->audio_downstream2 = p->audio_downstream = NULL;
    memset(&p->mem, 0, sizeof p->mem);
    uint8_t *st = p->mem.st;
    #define FLAGS   p->flags = p->mem.flags
    #define STATE(f, n) p->f = &st[n]
    switch (p->kind) {
    case 0:
        FLAGS; STATE(plug_state, 0); STATE(st10, 1); STATE(rx_state, 2); STATE(st18, 3); STATE(hdcp_state, 4); STATE(st20, 5);
        p->rx = &p->mem.rx; p->hdcp = &p->mem.hdcp; p->video = &p->mem.video; p->audio = &p->mem.audio; break;
    case 1:
        FLAGS; STATE(plug_state, 0); STATE(st10, 1); STATE(rx_state, 2); STATE(st18, 3);
        p->rx = &p->mem.rx; p->video = &p->mem.video; break;
    case 2:
        FLAGS; STATE(rx_state, 2); STATE(st18, 3); p->rx = &p->mem.rx; p->video = &p->mem.video; p->audio = &p->mem.audio; break;
    case 3:
        FLAGS; STATE(rx_state, 2); STATE(st18, 3); p->rx = &p->mem.rx; p->video = &p->mem.video; break;
    case 4:
        FLAGS; p->audio = &p->mem.audio; break;
    case 5:
        FLAGS; STATE(tx_state, 6); STATE(st28, 7); STATE(st2c, 8); STATE(st30, 9); STATE(hdcp_state, 4); STATE(st20, 5);
        p->status = &p->mem.status; p->hdcp_tx = &p->mem.hdcp_tx; p->video = &p->mem.video; p->audio = &p->mem.audio; break;
    case 6:
        FLAGS; STATE(tx_state, 6); STATE(st28, 7); STATE(st2c, 8); STATE(st30, 9);
        p->status = &p->mem.status; p->video = &p->mem.video; p->audio = &p->mem.audio; break;
    case 7:
        FLAGS; STATE(st2c, 8); STATE(st30, 9); p->status = &p->mem.status; p->video = &p->mem.video; p->audio = &p->mem.audio; break;
    case 8:  FLAGS; p->scaler = &p->mem.scaler; break;
    case 9:  FLAGS; p->color = &p->mem.color; break;
    case 0xb: FLAGS; STATE(st2c, 8); STATE(st30, 9); p->status = &p->mem.status; p->video = &p->mem.video; break;
    case 0xc: FLAGS; p->audio = &p->mem.audio; break;
    default: break;
    }
    #undef FLAGS
    #undef STATE
}

void gsv_port_create(gsv_chip *c, gsv_port *p, int16_t id, uint8_t kind)   /* 0x080065a8 */
{
    if (!g_gsv_port_list) g_gsv_port_list = p;
    p->chip = c; p->id = id; p->kind = kind;
    port_alloc(p);
    if (g_gsv_port_tail) g_gsv_port_tail->next = p;
    g_gsv_port_tail = p;
    p->next = NULL;
}

static void rx_port_init_a(gsv_port *p) { *p->plug_state = 1; }                        /* 0x08006cdc */
static void rx_port_init_b(gsv_port *p) { *p->rx_state = 1; /* 0x08011b22 is empty */ } /* 0x08006cf6 */
static void tx_port_init(gsv_port *p)                                                    /* 0x08006ce4 -> 0x0800d532 */
{
    *p->tx_state = 1;
    return;
}
static int port_init(gsv_port *p)                                                        /* 0x08006754 */
{
    switch (p->kind) {
    case 0: case 1: rx_port_init_a(p); rx_port_init_b(p); break;
    case 2: case 3: rx_port_init_b(p); break;
    case 5: case 6: tx_port_init(p); break;
    default: break;
    }
    return 0;
}
int gsv_ports_init_all(void)                                                             /* 0x08006ae4 */
{
    for (gsv_port *p = g_gsv_port_list; p; p = p->next) port_init(p);
    return 0;
}

void gsv_tx_read_edid_dump(gsv_port *p);
/* ---- events (0x0800785c "AvEvent...") ------------------------------------------------------------------ */
int gsv_event(gsv_port *p, int ev)
{
    switch (ev) {
    case 0: case 1: case 2: case 3: case 4: case 0xe: break;
    case 7: {                                               /* a sink's video is about to start: HDMI or DVI, from the source */
        gsv_port *src = gsv_root_source(p);
        if (src && src->kind == 0) {
            if (src->rx->locked == 1) p->status->hdmi_mode = (p->status->s9 == 1 || p->status->spa != 0) ? src->rx->hdmi_mode : 0;
            else p->status->hdmi_mode = 1;
            if (p->kind == 5) { gsv_k1_tx_hdmi_mode(p); p->video->flags &= 0xfffe; }     /* 0x08030516 */
        }
        break;
    }
    case 5:
        gsv_log("AvEventPortDownStreamDisconnected[%d]\r\n", p->id - 3);
        gsv_app_tx_event(p->id - 3, 0);                     /* 0x08013d7c, + 0x08012f10 for TxB */
        break;
    case 6:
        gsv_log("AvEventPortDownStreamConnected[%d]\r\n", p->id - 3);
        gsv_app_tx_event(p->id - 3, 1);
        break;
    case 8: gsv_app_rx_event(0, 0); break;                  /* 0x08013a9c */
    case 9: gsv_app_rx_event(0, 1); break;
    case 10: p->hdcp->mode = 1; break;
    case 0xb:                                               /* a Tx was reset: its HDCP policy. 2 = try 2.2, then 1.4 */
        p->hdcp_tx->type = (p->no_hdcp == 1) ? 0 : 2;   /* 0 = never: with type 2 the Gsv2k1 driver re-tries
                                                               every pass, for ever, on a sink that does not answer HDCP */
        p->hdcp_tx->u3 = 0;
        break;
    case 0xc:
        if (p->rx->plug_sub != 5) {
            gsv_log("\tRx %d Generated Edid Start: \r\n", p->id + 1);   /* 0x08006ca8 */
            p->rx->plug_sub = 5;
        }
        break;
    case 0xd: p->status->u10 = 0xffffffff; break;
    case 0xf: gsv_tx_read_edid_dump(p); break;
    default: return -3;
    }
    return 0;
}

/* ---- linking (0x080065ec and helpers) ------------------------------------------------------------------- */
/* 0x08011484 / 0x080112a8: depth-first walk of the downstream tree below `root` using (downstream, downstream2)
 * or the audio equivalents; *cursor is the previously returned port, *out receives the next one. */
static int walk_next(gsv_port *root, gsv_port **cursor, gsv_port **out, int audio)
{
    #define DN(q)  (audio ? (q)->audio_downstream : (q)->downstream)
    #define DN2(q) (audio ? (q)->audio_downstream2 : (q)->downstream2)
    gsv_port *q = DN(root);
    for (;;) {
        if (!q) return -3;
        if (*cursor == NULL && q) break;
        if (DN(q) == NULL) {
            if (*cursor == q) { *cursor = NULL; q = DN2(q); } else q = DN2(q);
        } else {
            if (*cursor == q) *cursor = NULL;
            if (walk_next(q, cursor, out, audio) == 0) {
                if (*out) return 0;
                *cursor = NULL; q = DN2(q);
            } else q = DN2(q);
        }
    }
    *out = q;
    return 0;
    #undef DN
    #undef DN2
}
/* 0x080114f8: variant used by the HDCP code (same traversal, different bookkeeping of the result) */
static int walk_next2(gsv_port *root, gsv_port **cursor, gsv_port **out)
{
    int rc = -3;
    gsv_port *q = root->downstream;
    for (;;) {
        for (;;) {
            for (;;) {
                if (!q) return rc;
                if (q->downstream == NULL) break;
                if (walk_next2(q, cursor, out) == 0) {
                    if (*out) return 0;
                    *cursor = NULL; rc = 0; q = q->downstream2;
                } else q = q->downstream2;
            }
            if (*cursor != q) break;
            *cursor = NULL; q = q->downstream2;
            if (!q) return -3;
            rc = 0;
        }
        if (*cursor == NULL && q) break;
        q = q->downstream2;
    }
    *out = q;
    return 0;
}
int gsv_walk_downstream(gsv_port *root, gsv_port **cursor, gsv_port **out) { return walk_next2(root, cursor, out); }
/* 0x0801131c: same traversal over the audio links */
int gsv_walk_audio_downstream(gsv_port *root, gsv_port **cursor, gsv_port **out)
{
    int rc = -3;
    gsv_port *q = root->audio_downstream;
    for (;;) {
        for (;;) {
            for (;;) {
                if (!q) return rc;
                if (q->audio_downstream == NULL) break;
                if (gsv_walk_audio_downstream(q, cursor, out) == 0) {
                    if (*out) return 0;
                    *cursor = NULL; rc = 0; q = q->audio_downstream2;
                } else q = q->audio_downstream2;
            }
            if (*cursor != q) break;
            *cursor = NULL; q = q->audio_downstream2;
            if (!q) return -3;
            rc = 0;
        }
        if (*cursor == NULL && q) break;
        q = q->audio_downstream2;
    }
    *out = q;
    return 0;
}
gsv_port *gsv_root_source(gsv_port *p)       /* 0x08011562 */
{
    gsv_port *r = NULL;
    for (gsv_port *q = p->source; q; q = q->source) r = q;
    return r;
}
gsv_port *gsv_audio_root_source(gsv_port *p) /* 0x08011386 */
{
    gsv_port *r = NULL;
    for (gsv_port *q = p->audio_source; q; q = q->audio_source) r = q;
    return r;
}
int gsv_read_block(uint32_t dev, uint8_t reg, uint8_t *data, uint16_t len) { return hook_read(dev, reg, data, len); }            /* 0x08007578 */
int gsv_write_block(uint32_t dev, uint8_t reg, const uint8_t *data, uint16_t len) { return hook_write(dev, reg, (uint8_t *)data, len); }   /* 0x080076c0 */
__attribute__((weak)) uint8_t gsv_app_setting(int idx) { (void)idx; return 0; }

static void unlink_video(gsv_port *p)    /* 0x0800e1f8 */
{
    gsv_port *cursor = NULL, *q = p, *hit;
    while (q->source) {
        q = q->source;
        if (q->downstream == p) q->downstream = p->downstream2;
        if (q->downstream2 == p) q->downstream2 = p->downstream2;
    }
    for (;;) {
        hit = NULL;
        if (walk_next(q, &cursor, &hit, 0) != 0) break;
        if (hit->downstream == p) hit->downstream = p->downstream2;
        if (hit->downstream2 == p) hit->downstream2 = p->downstream2;
        cursor = hit;
    }
    p->source = NULL; p->downstream2 = NULL;
}
static void unlink_audio(gsv_port *p)    /* 0x0800e190 */
{
    gsv_port *cursor = NULL, *q = p, *hit;
    while (q->audio_source) {
        q = q->audio_source;
        if (q->audio_downstream == p) q->audio_downstream = p->audio_downstream2;
        if (q->audio_downstream2 == p) q->audio_downstream2 = p->audio_downstream2;
    }
    for (;;) {
        hit = NULL;
        if (walk_next(q, &cursor, &hit, 1) != 0) break;
        if (hit->audio_downstream == p) hit->audio_downstream = p->audio_downstream2;
        if (hit->audio_downstream2 == p) hit->audio_downstream2 = p->audio_downstream2;
        cursor = hit;
    }
    p->audio_source = NULL; p->audio_downstream2 = NULL;
}

/* 0x0800c13c: Tx interrupt enables. mask16 selects interrupts; mode 1 enables, 0 disables, 2 only sets the
 * mask bits in the chip's 16-bit interrupt mask register (dev42 0xfb/0xfc) */
/* GSV2001 SDK (0x08010e1e): the Tx interrupt enables. One shadow word on page 0x42 and the enable bits on page 0x40,
 * for either output. mode 0 = disable, 1 = enable, 2 = only mark in the shadow word. */
void gsv_tx_int_config(gsv_port *p, unsigned mask16, int mode)
{
    static const struct { uint16_t bit, shadow; uint8_t reg, mask, shift; } t[] = {
        { 1u << 0, 0x002, 0x50, 0x02, 1 }, { 1u << 1, 0x008, 0x50, 0x08, 3 }, { 1u << 2, 0x020, 0x51, 0x08, 3 },
        { 1u << 3, 0x004, 0x50, 0x04, 2 }, { 1u << 6, 0x100, 0x51, 0x20, 5 }, { 1u << 8, 0x010, 0x50, 0x10, 4 },
        { 1u << 9, 0x001, 0x50, 0x01, 0 }, { 1u << 4, 0x040, 0x51, 0x10, 4 }, { 1u << 5, 0x080, 0x51, 0x10, 4 },
        { 1u << 7, 0x200, 0x51, 0x04, 2 }, { 1u << 12, 0, 0x51, 0x01, 0 }, { 1u << 11, 0, 0x51, 0x02, 1 },
        { 1u << 13, 0x400, 0x51, 0x40, 6 }, { 1u << 14, 0x800, 0x51, 0x80, 7 },
    };
    if (p->idx == 0xff) return;
    const bool mark = mode == 2; const uint8_t on = mode == 1;
    uint32_t v = 0;
    gsv_read_field(GSV_PAGE(0x42), 0x800fb, 0x7f, 0xff, 0, 1, 2, &v);
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        if (!(mask16 & t[i].bit)) continue;
        if (mark) v |= t[i].shadow;
        else { v &= ~(uint32_t)t[i].shadow & 0xffff; gsv_write_bits(GSV_PAGE(0x40), t[i].reg, t[i].mask, t[i].shift, on); }
    }
    gsv_write_field(GSV_PAGE(0x42), 0x800fb, 0x7f, 0xff, 0, 1, 2, v);
}


void gsv_tx_set_tmds(gsv_port *p, unsigned on)    /* 0x0800fb30 "Set TMDS to %d" */
{
    uint8_t f2 = (on == 1 && p->kind == 5) ? 0xff : 0x70;
    gsv_write_bits(gsv_tx_dev(p), 0xd1, 0x10, 4, (uint8_t)on);
    gsv_write_bits(gsv_tx_dev(p), 0xd1, 0x20, 5, (uint8_t)on);
    gsv_write_bits(gsv_tx_dev(p), 0xd1, 0x40, 6, (uint8_t)on);
    gsv_write_bits(gsv_tx_dev(p), 0xd1, 0x80, 7, (uint8_t)on);
    gsv_write_bits(gsv_tx_dev(p), 0xf2, 0xff, 0, f2);
    gsv_log(on ? "Port %d: Set TMDS to 1...\n" : "Port %d: Set TMDS to 0...\n", p->id);
}

int gsv_tx_reset_core(gsv_port *p);
void gsv_rx_disable_hpa(gsv_port *p);


/* GSV2001 SDK (0x0801246c). The source selects are not the GSV2002's: each output has a two-bit field in page 1
 * reg 9 (bits 1:0 Tx A, 5:4 Tx B), and page 1 reg 5 bits 2:0 and page 0 reg 8 bits 6:4 are written for either output.
 * `what` is gsv_port_link's: 0 video, 1 audio, 2 both. */
/* GSV2001 output modes (Gsv2k1 0x08013244, datasheet 1.3.3 and figure 3). The chip has one Tx core (page 0x40) that
 * both outputs can take their picture from; the core is fed by the input (page 1 reg 5 [2:0] = 0) or by the scaler /
 * colour block (2). Page 1 reg 9 has two bits per output (1:0 Tx A, 5:4 Tx B): the driver writes 0 for an output it
 * has connected (index 0) and 2 for one it has not (index 0xff), which it then treats as carrying the input as it is
 * (locked when the input is stable, clock = the input's TMDS clock, core registers left alone). That is the
 * datasheet's example: the 4K input passed to one output, 1080p from the scaler on the other.
 * tx->bypass = 1 before linking asks for that pass-through mode; no firmware found sets it up itself (their apps
 * never need it), so the value 2 = pass-through is read from the driver's code, not seen in use. */
static int gsv2k1_link_apply(gsv_port *src, gsv_port *dst, int what)
{
    if ((dst->id == 4 || dst->id == 5) && dst->bypass == 1) {
        gsv_write_bits(GSV_PAGE(0x01), 9, dst->id == 4 ? 0x03 : 0x30, dst->id == 4 ? 0 : 4, 2);
        if (what != 1) { dst->status->tmds_locked = 0; dst->video->flags = 0; }
        dst->audio->flags = 0;
        gsv_tx_int_config(dst, 0xffff, 0);
        if (src->id < 4) g_chip_state[0] = src;
        return 0;                                           /* index stays 0xff */
    }
    if (dst->kind == 5 && what != 1) {
        if (dst->status->s6 < 0x1a) dst->status->s6 = 0; else dst->status->hdcp_mode = 2;
        if (dst->idx != 0xff) gsv_tx_reset_core(dst);
    } else if (dst->id == 0x14 || dst->id == 0x18) {
        int16_t other = dst->id == 0x14 ? 0x18 : 0x14;
        uint8_t c1 = dst->chip->c1;
        for (gsv_port *q = dst->chip->first_port; q && q->chip->c1 == c1; q = q->next) {
            if (q->source && q->source->id == other) q->source = NULL;
            if (q->downstream && q->downstream->id == other) q->downstream = NULL;
            if (q->downstream2 && q->downstream2->id == other) q->downstream2 = q->downstream2->downstream2;
        }
        for (gsv_port *q = dst->chip->first_port; q && q->chip->c1 == c1; q = q->next)
            if (q->id == other) { q->source = NULL; q->downstream = NULL; q->downstream2 = NULL; }
    }
    if (dst->id == 4 || dst->id == 5) {
        dst->idx = 0;
        if (what == 1) { dst->audio->flags = 0; gsv_tx_int_config(dst, 0x1800, 0); }
        else { dst->status->tmds_locked = 0; dst->video->flags = 0; dst->audio->flags = 0; gsv_tx_int_config(dst, 0xffff, 0); }
    }
    const uint16_t id = (uint16_t)dst->id, sid = (uint16_t)src->id;
    if (id == 10) {
        gsv_write_bits(GSV_PAGE(0x00), 0x9d, 0xff, 0, 0xff);
    } else if (id == 4 || id == 5) {
        const uint8_t m9 = id == 4 ? 0x03 : 0x30, s9 = id == 4 ? 0 : 4;
        gsv_write_bits(GSV_PAGE(0x01), 9, m9, s9, 0);
        if (sid == 0) gsv_write_bits(GSV_PAGE(0x01), 5, 7, 0, 0);
        else if (sid == 0x14 || sid == 0x18) gsv_write_bits(GSV_PAGE(0x01), 5, 7, 0, 2);
        else if (sid == 0x1c) gsv_write_bits(GSV_PAGE(0x01), 5, 7, 0, 4);
        uint8_t cur = 0; int v = 1;
        gsv_read_bits(GSV_PAGE(0x00), 8, 0x70, 4, &cur);
        if (sid == 0) { if (what == 0) v = -1; }
        else if (sid == 10) { gsv_write_bits(GSV_PAGE(0x00), 0x9d, 0xff, 0, 0xff); gsv_write_bits(GSV_PAGE(0x00), 10, 0x70, 4, 0); v = 5; }
        else if (cur == 5) v = -1;
        if (v >= 0) gsv_write_bits(GSV_PAGE(0x00), 8, 0x70, 4, (uint8_t)v);
    } else if (id == 8) {
        gsv_write_bits(GSV_PAGE(0x00), 0x9d, 0xff, 0, 0xff);
        uint8_t sel = sid == 0 ? 1 : (sid == 4 || sid == 5) ? 4 : 0;
        if (sid == 4 || sid == 5) GSV_TODO("GSV2001 audio from a Tx (0x0801244a)");
        gsv_write_bits(GSV_PAGE(0x00), 10, 0x70, 4, sel);
    } else if (id == 0x14 || id == 0x18) {
        if (sid == 0) gsv_write_bits(GSV_PAGE(0x01), 4, 3, 0, 0);
        else if (sid == 0x1c) gsv_write_bits(GSV_PAGE(0x01), 4, 3, 0, 3);
        /* the SP12CS's newer SDK also clears page 0x12 reg 0x66 here */
    }
    if (sid < 4) g_chip_state[0] = src;
    if (id == 0x14 || id == 0x18) g_chip_state[1] = dst;
    return 0;
}

int gsv_port_link(gsv_port *src, gsv_port *dst, int what)   /* 0x080065ec */
{
    if (what == 0 || what == 2) {
        switch (src->kind) {
        case 0: case 2: case 5: case 7: case 8:
            if (src->kind == 8) src->scaler->enabled = 1;
            /* fallthrough */
        case 9:
            if (src->kind == 9) src->color->enabled = 1;
            /* fallthrough */
        case 1: case 3: case 6: case 10: case 0xb:
            unlink_video(dst);
            if (src->downstream && src->downstream != dst) dst->downstream2 = src->downstream;
            src->downstream = dst;
            dst->source = src;
            if (dst->kind == 5) { dst->video->flags = 0; gsv_event(src, 4); }
            break;
        default: break;
        }
    }
    if (what == 1 || what == 2) {
        switch (src->kind) {
        case 0: case 2: case 4: case 5: case 7: case 0xc:
            unlink_audio(dst);
            if (src->audio_downstream && src->audio_downstream != dst) dst->audio_downstream2 = src->audio_downstream;
            src->audio_downstream = dst;
            dst->audio_source = src;
            if (dst->kind == 5) dst->audio->flags = 0;
            break;
        default: break;
        }
    }
    if (what == 0 || what == 2) {
        switch (src->kind) {
        case 0: case 1: case 2: case 3: case 5: case 6: case 7: case 8: case 9: case 10: case 0xb:
            if (!src->chain) dst->chain = src->chain;
            else { gsv_port *q = src->chain; while (q->chain) q = q->chain; dst->chain = q; }
            break;
        case 4: case 0xc: src->chain = NULL; break;
        default: break;
        }
    }
    return gsv2k1_link_apply(src, dst, what);
}

void gsv_route_set(gsv_port *rx, gsv_port *tx, gsv_port *color, gsv_port *scaler, int mode)   /* 0x08007be8 */
{
    if (mode == 1) {
        if (tx->source != rx) { gsv_port_link(rx, tx, 2); gsv_log("New Route: Rx%d-Tx%d\n", rx->id + 1, tx->id - 3); }
    } else if (mode == 2) {
        bool a = color->source != rx; if (a) gsv_port_link(rx, color, 0);
        bool b = tx->source != color; if (b) gsv_port_link(color, tx, 0);
        if (a || b) gsv_log("New Route: Rx%d-Color-Tx%d\n", rx->id + 1, tx->id - 3);
    } else if (mode == 3) {
        bool a = scaler->source != rx; if (a) gsv_port_link(rx, scaler, 0);
        bool b = tx->source != scaler; if (b) gsv_port_link(scaler, tx, 0);
        if (a || b) gsv_log("New Route: Rx%d-Scaler-Tx%d\n", rx->id + 1, tx->id - 3);
    }
}

/* ---- chip init (0x08006746 / 0x08008d0c / 0x0800f2f4) -------------------------------------------------- */
void gsv_rx_pll_init(void)
{
    uint8_t v = 0, saved = 0;
    gsv_read_bits(GSV_PAGE(0x01), 0x05, 0x07, 0, &saved);
    gsv_write_bits(GSV_PAGE(0x01), 0x05, 0x07, 0, 4);
    uint16_t tries = 0;      /* GSV2001 SDK (0x0800f25c) only: it gives up instead of waiting for ever */
    for (bool ok = false; !ok;) {
        for (uint8_t i = 0; i < 0x0f; i++) gsv_read_bits(GSV_PAGE(0x02), 0x9c, 0xff, 0, &v);
        while (v != 0x20 && tries < 0x6000) { gsv_read_bits(GSV_PAGE(0x02), 0x9c, 0xff, 0, &v); tries++; }
        gsv_read_bits(GSV_PAGE(0x02), 0x91, 0xff, 0, &v);
        if (v < 0x81 || v > 0x95) {
            gsv_write_bits(GSV_PAGE(0x01), 0xc5, 0xff, 0, 0xcc);
            gsv_write_bits(GSV_PAGE(0x01), 0xc6, 0xff, 0, 0x01);
            gsv_write_bits(GSV_PAGE(0x01), 0xc5, 0xff, 0, 0x0c);
        } else ok = true;
        if (++tries > 0x4fff) break;
    }
    gsv_write_bits(GSV_PAGE(0x01), 0x05, 0x07, 0, saved);
}

uint8_t gsv_k1_init_4k60;      /* 0 = the Kramer driver's init exactly (what the traces replay); 1 = add the SP12CS's 20.46=84 */
/* GSV2001 SDK chip init (0x0800f340 in the SP12CS firmware) */
void gsv_set_i2c_addresses(gsv_chip *c)
{
    const uint32_t base = *c->dev_base;
    gsv_write_bits(GSV_PAGE(0x00), 0xff, 0x80, 7, 0);
    gsv_write_bits(GSV_PAGE(0x00), 0xff, 0x80, 7, 1);
    for (int i = 0; i < 100; i++) gsv_write_bits(GSV_PAGE(0x00), 0xff, 0x80, 7, 0);   /* its settle time: 100 more writes */
    for (const uint8_t *t = gsv2k1_init_table; *t != 0xff; t += 3) gsv_write_bits(base | t[0], t[1], 0xff, 0, t[2]);
    gsv_rx_pll_init();
    for (const uint8_t *t = gsv2k1_rxport_init_table; *t != 0xff; t += 2) gsv_write_bits(GSV_PAGE(0x27), t[0], 0xff, 0, t[1]);
    /* Not in the Kramer driver: page 0x20 reg 0x46 = 0x84, which the year-newer SDK in the SP12CS (a 4K60 splitter)
     * has in its init table after 20.24. Found on the oneboard, 2026-11-20: with the Kramer value (0x80) a 594 MHz
     * input with HDCP 2.2 loses vertical lock (page 0x20 reg 0 bit 4) about three quarters of the time although the
     * picture is fine, and the driver then re-plugs the input every few seconds; with 0x84 the bit stays set. */
    if (gsv_k1_init_4k60) gsv_write_bits(GSV_PAGE(0x20), 0x46, 0xff, 0, 0x84);
    c->state = g_chip_state;
    g_chip_state[0] = NULL;
}

int gsv_chip_init(gsv_chip *c) { gsv_set_i2c_addresses(c); return 0; }

/* ---- per-tick dispatcher (0x08006b00) ------------------------------------------------------------------ */
static void rx_plug_state_log(gsv_port *p, uint8_t prev)     /* 0x080131a4 */
{
    static const char *n[] = { 0, "PlugRxDefault", "PlugRxDetect", "PlugRxInfoUpdate", "PlugRxInputLock", "PlugRxPlugged", "PlugRxPullDownHpd", "PlugRxReadTiming", "PlugRxReset", "PlugRxStable" };
    uint8_t s = *p->plug_state;
    if (s == prev) return;
    if (s == 4) gsv_event(p, 9);
    if (s == 6 && p->rx->cable_5v == 0) gsv_event(p, 8);
    if (s >= 1 && s <= 9) gsv_log("Port%d: %s\n", p->id, n[s]);
}
static void rx_state_log(gsv_port *p, uint8_t prev)          /* 0x080134f4 */
{
    static const char *n[] = { 0, "RxDefault", "RxDetect", "RxFreerun", "RxReceiving", "RxReset" };
    uint8_t s = *p->rx_state;
    if (s != prev && s >= 1 && s <= 5) gsv_log("Port%d: %s\n", p->id, n[s]);
}
static void rx_hdcp_state_log(gsv_port *p, uint8_t prev)     /* 0x08012fc4 */
{
    static const char *n[] = { 0, "Hdcp2p2State", "HdcpDefault", "HdcpDisable", "HdcpFail", "HdcpReAuthentication", "HdcpReadBksv", "HdcpReceiverMode", "HdcpRepeaterMode", "HdcpRepeaterReset", "HdcpRequestSinkHdcp", "HdcpStartAuthentication", "HdcpUpstreamConnected" };
    uint8_t s = *p->hdcp_state;
    if (s != prev && s >= 1 && s <= 12) gsv_log("Port%d: %s\n", p->id, n[s]);
}
static void tx_plug_state_log(gsv_port *p, uint8_t prev)     /* 0x08013324 */
{
    static const char *n[] = { 0, "PlugTxDefault", "PlugTxDefaultEdid", "PlugTxEdid", "PlugTxEdidError", "PlugTxEdidManage", "PlugTxEnableTxCore", "PlugTxHdcp", "PlugTxHpdAntiDither", "PlugTxReset", "PlugTxStable", "PlugTxTransmitVideo", "PlugTxVideoUnlocked" };
    uint8_t s = *p->tx_state;
    if (s != prev && s >= 1 && s <= 12) gsv_log("Port%d: %s\n", p->id, n[s]);
}

int gsv_fsm_run(void)
{
    for (gsv_port *p = g_gsv_port_list; p; p = p->next) {
        uint8_t prev;
        switch (p->kind) {
        case 0: case 1:
            prev = *p->plug_state; gsv_rx_plug_fsm_step(p); gsv_rx_plug_fsm(p); rx_plug_state_log(p, prev);
            prev = *p->rx_state;   gsv_rx_fsm_step(p);      gsv_rx_fsm(p);      rx_state_log(p, prev);
            prev = *p->hdcp_state; gsv_rx_hdcp_fsm_step(p); rx_hdcp_state_log(p, prev);
            break;
        case 2: case 3: gsv_rx_fsm_step(p); gsv_rx_fsm(p); break;
        case 5:
            prev = *p->tx_state; gsv_tx_fsm_step(p); gsv_tx_hdcp_fsm(p); tx_plug_state_log(p, prev); break;
        case 6: gsv_tx_fsm_step(p); gsv_tx_hdcp_fsm(p); break;
        case 8: gsv_scaler_fsm_step(p); break;
        case 9: gsv_color_fsm_step(p); break;
        case 10: GSV_TODO("kind 10 fsm"); break;
        case 0xc: gsv_audio_fsm_step(p); break;
        default: break;
        }
    }
    return 0;
}
