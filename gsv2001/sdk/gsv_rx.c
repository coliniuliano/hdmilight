/* GSV2001 SDK: Rx port state machines (plug detection, link lock, HDCP status). */
#include "gsv_sdk.h"
#include <string.h>

/* app hooks the SDK calls into (the original wrote app RAM / called app functions) */
__attribute__((weak)) void gsv_app_status_set(int which, uint8_t val) { (void)which; (void)val; }   /* 0x08012f5c */
__attribute__((weak)) void gsv_app_rx_event(int ev, int on) { (void)ev; (void)on; }                   /* 0x08013a9c */
__attribute__((weak)) uint8_t gsv_app_hdcp_setting(void) { return 0; }                                 /* 0x08006038 */

uint32_t gsv_rx_dev_e984(gsv_port *p) { return p->idx == 0 ? GSV_PAGE(0x22) : p->idx == 1 ? GSV_PAGE(0x32) : 0; }
uint32_t gsv_rx_dev_e9a8(gsv_port *p) { return p->idx == 0 ? GSV_PAGE(0x20) : p->idx == 1 ? GSV_PAGE(0x30) : 0; }
uint32_t gsv_rx_dev_e9f0(gsv_port *p) { return p->idx == 0 ? GSV_PAGE(0x24) : p->idx == 1 ? GSV_PAGE(0x34) : 0; }
uint32_t gsv_rx_dev_e9cc(gsv_port *p) { return p->idx == 0 ? GSV_PAGE(0x25) : p->idx == 1 ? GSV_PAGE(0x35) : 0; }

void gsv_rx_disable_hpa(gsv_port *p)      /* 0x0800efd0 "Disable Rx Hpa" */
{
    uint8_t cur = 0;
    /* Gsv2k1 0x08018d68: one input, two-bit fields; the SP12CS's SDK also clears
                                         page 5 reg 0x70 bit 1 before 0xbb and page 0 reg 0xae bit 0 at the end */
    gsv_read_bits(GSV_PAGE(0x00), 0xbb, 3, 0, &cur);
    uint8_t v = cur & 0xfe;
    gsv_write_bits(GSV_PAGE(0x26), 0x74, 1, 0, 0);
    if (p->idx == 0) {
        gsv_write_bits(GSV_PAGE(0x20), 0xbe, 0x10, 4, 1);
        gsv_write_bits(GSV_PAGE(0x20), 0xbe, 0x10, 4, 0);
        if (p->hdcp->mode == 0) gsv_write_bits(GSV_PAGE(0x00), 0xbe, 1, 0, 0);
    }
    if (v != cur) {
        gsv_write_bits(GSV_PAGE(0x00), 0xb2, 3, 0, v);
        gsv_write_bits(GSV_PAGE(0x00), 0xbb, 3, 0, v);
        gsv_write_bits(GSV_PAGE(0x00), 0xb5, 3, 0, 0);
        gsv_write_bits(GSV_PAGE(0x00), 0xb6, 3, 0, 0);
        gsv_log("Port %d: Disable Rx Hpa %d...\n", p->id, v);
    }
    return;
}

static void rx_enable_hpa(gsv_port *p)     /* 0x0800f114 "Enable Rx Hpa" */
{
    uint8_t cur = 0;
    gsv_read_bits(GSV_PAGE(0x00), 0xbb, 0x0f, 0, &cur);
    uint8_t v = cur;
    if (p->id == 0) gsv_write_bits(GSV_PAGE(0x26), 0x74, 1, 0, 1);
    else if (p->id == 1) gsv_write_bits(GSV_PAGE(0x26), 0x74, 2, 1, 1);
    else return;
    if (p->hdcp->mode != 0 && p->idx == 0) gsv_write_bits(GSV_PAGE(0x00), 0xbe, 1, 0, 1);
    if (p->id == 0) v |= 1; else if (p->id == 1) v |= 2; else return;
    if (v != cur) {
        gsv_write_bits(GSV_PAGE(0x00), 0xbb, 0x0f, 0, v);
        gsv_write_bits(GSV_PAGE(0x00), 0xb2, 0x0f, 0, v);
        gsv_write_bits(GSV_PAGE(0x00), 0xb5, 0x0f, 0, 3);
        gsv_write_bits(GSV_PAGE(0x00), 0xb6, 0x0f, 0, 3);
        gsv_log("Port %d: Enable Rx Hpa %d...\n", p->id, v);
    }
}
static void rx_power_up(gsv_port *p)       /* 0x0800ba28 + 0x08011804 */
{
    rx_enable_hpa(p);
    gsv_log("---------------Rx %d Power Up------------\n", p->id + 1);
    p->rx->mode = 1; p->rx->lock_delay = 0;
}

int gsv_rx_power_down(gsv_port *p)        /* 0x0800b9e0 */
{
    gsv_rx_disable_hpa(p);
    gsv_log("--------------Rx %d Power Down-----------\n", p->id + 1);
    return 0;
}

static void rx_power_down_full(gsv_port *p)    /* 0x080117ea */
{
    gsv_rx_power_down(p);
    p->rx->mode = 0; p->rx->lock_delay = 0;
    p->hdcp->counter = 0;
}

static void rx_info_reset(gsv_port *p)         /* 0x08011828 */
{
    gsv_rx_info *r = p->rx;
    r->inactive = 1; r->u1 = 1; r->u2 = 0; r->stable = 0; r->locked = 0; r->encrypted = 0; r->plug_sub = 0;
    r->mode = 0; r->lock_delay = 0; r->audio_present = 0; r->lock_a = r->lock_b = r->lock_c = 0; r->hdmi_mode = 0;
}

static void rx_eq_reset_phy(gsv_port *p)       /* 0x0800fed4 */
{
    uint8_t v = 0;
    if (p->id == 0) gsv_read_bits(GSV_PAGE(0x02), 0x2f, 7, 0, &v);
    else if (p->id == 1) gsv_read_bits(GSV_PAGE(0x02), 0x5f, 7, 0, &v);
    gsv_write_bits(gsv_rx_dev27(p), 3, 0xff, 0, 3);
}

/* GSV2001 (Gsv2k1, Kramer image 0x080194c0): same job with other limits; the four equaliser registers 0x0c..0x0f go
 * out as one block. The SP12CS's newer SDK differs in details: it counts the timer to 10 instead of 2, uses 0 for
 * register 0x0c below 60 MHz, clears the HDCP counter on the early return, and when a restart would repeat the
 * settings already in the chip it toggles page 0x20 reg 0xd9 between f0 and a0. */
void gsv_rx_eye_width(gsv_port *p, int reset)
{
    gsv_rx_info *r = p->rx;
    uint16_t score[4] = { 0 }, width[4] = { 0 }, raw[4] = { 0 };
    uint8_t v = 0, buf[2] = { 0 }, c[4], c61; uint8_t lanes_ok = 0; bool restart = false, fail = false;
    if (reset == 0) { if (r->eq_done == 1) return; }
    else { r->eq_timer = 0; r->eq_done = 0; }
    if (r->eq_timer < 2) {
        if (r->eq_timer == 1) {
            uint32_t dev = GSV_PAGE(0x27);
            for (uint8_t lane = 0; lane < 3; lane++, dev++) {
                gsv_read_bits(dev, 0x20, 0xff, 0, &v);
                if (v == 0x10) {
                    gsv_read_block(dev, 0x92, buf, 2); raw[lane] = (uint16_t)((buf[0] << 8) | buf[1]);
                    score[lane] = raw[lane] < 0x1771 ? raw[lane] / 0x3c : 100;
                    gsv_read_bits(dev, 0x96, 0xff, 0, &v); width[lane] = v;
                    gsv_read_bits(dev, 0x3a, 0xff, 0, &v);
                    if (v == 1) {
                        gsv_read_bits(dev, 0x0e, 0xff, 0, &v);
                        if (v == 0x81) {
                            gsv_read_bits(GSV_PAGE(0x00), 0xb6, 1, 0, &v);
                            if (v != 0) { gsv_write_bits(GSV_PAGE(0x00), 0xb6, 1, 0, 0); return; }
                        }
                        r->eq_timer = 0; restart = true;
                    } else {
                        gsv_read_bits(dev, 0x0e, 0xff, 0, &v);
                        if (v == 1) { if (raw[lane] < 0xbb9 || width[lane] < 0xd) { r->eq_timer = 0; restart = true; fail = true; break; } }
                        else if (raw[lane] < 0x4b1 || width[lane] < 9) { r->eq_timer = 0; restart = true; break; }
                        lanes_ok++;
                    }
                } else {
                    gsv_read_bits(GSV_PAGE(0x00), 0xb6, 1, 0, &v);
                    if (v == 0) { gsv_write_bits(GSV_PAGE(0x00), 0xb6, 1, 0, 1); r->eq_timer = 0; restart = true; }
                    if (r->eq_timer == 0 && p->hdcp->running22 == 1) { gsv_write_bits(GSV_PAGE(0x21), 0x15, 1, 0, 1); gsv_write_bits(GSV_PAGE(0x21), 0x15, 1, 0, 0); }
                }
            }
        }
    } else lanes_ok = 3;
    if (lanes_ok == 3) {
        for (uint8_t lane = 0; lane < 3; lane++)
            if (r->eq_timer == 1) { gsv_log("Lane %d eye score  = %d\n", lane, score[lane]); gsv_log("Lane %d eye width  = %d\n", lane, width[lane]); }
        if (r->eq_timer == 1) {
            gsv_write_bits(GSV_PAGE(0x20), 0x22, 4, 2, 1);
            gsv_write_bits(GSV_PAGE(0x22), 0x48, 0x80, 7, 1);
            gsv_write_bits(GSV_PAGE(0x22), 0x48, 0x80, 7, 0);
        }
        if (r->eq_timer < 2) { r->eq_timer++; gsv_log("Port Rx %d: EQ Timer = %d\n", p->id, r->eq_timer); }
        else {
            gsv_write_bits(GSV_PAGE(0x03), 0x38, 4, 2, 1);
            gsv_write_bits(GSV_PAGE(0x03), 0x38, 0x40, 6, 1);
            r->eq_done = 1;
        }
    } else if (r->eq_timer != 0) return;
    if (r->eq_timer != 0) return;
    if (restart) { c[0] = 0x81; c[1] = fail ? 7 : 0x81; c[2] = 0x81; c[3] = 0x81; c61 = 1; }
    else {
        uint16_t pix = p->video->pixclk, tmds = p->video->tmds_clk;
        c[0] = 3; c[2] = 1;
        if (pix < 0xfa) { c[1] = 7; c[3] = 1; c61 = tmds < 0x3c ? 1 : tmds < 0xaa ? 3 : 0xf; }
        else { c[1] = 0x81; c[3] = pix < 0x1d6 ? 1 : 0x81; c61 = 0xf; }
    }
    uint32_t dev = GSV_PAGE(0x27);
    gsv_write_bits(dev, 0x3f, 0xff, 0, 0x22);
    rx_eq_reset_phy(p);
    gsv_write_bits(dev, 0x3f, 0xff, 0, 0x32);
    gsv_write_block(dev, 0x0c, c, 4);
    gsv_write_bits(dev, 0x61, 0xff, 0, c61);
    gsv_log(restart ? "Port Rx %d: Restart Adpative EQ in Enhanced Mode\n" : "Port Rx %d: Reset Adpative EQ in Fast Mode\n", p->id);
    gsv_write_bits(dev, 0x3f, 0xff, 0, 0x33);
    r->eq_timer = 1;
}


/* Tx-side helpers used by the Rx reset path live in gsv_tx.c */
int  gsv_tx_reset_core(gsv_port *p);            /* 0x0800bfa4 */
int  gsv_tx_output_disable(gsv_port *p);        /* 0x0800c0f8 */

int gsv_rx_reset_port(gsv_port *p)       /* 0x08008e38 "Rx %d Reset Port" */
{
    if (p->kind == 0 || p->kind == 1) {
        gsv_log("--------------Rx %d Reset Port------------\n", p->id + 1);
        gsv_rx_disable_hpa(p);
        gsv_rx_eye_width(p, 1);
    } else if ((p->kind == 5 || p->kind == 6) && (p->id == 4 || p->id == 5)) {
        gsv_tx_reset_core(p);
        gsv_tx_output_disable(p);
        gsv_write_bits(gsv_tx_dev(p), 0xc4, 0xff, 0, 0);
        gsv_write_bits(gsv_tx_dev(p), 0xc9, 0x0f, 0, 0xf);
    }
    return 0;
}

/* 0x08011b94: clear the downstream Tx ports' change flags */
static void rx_clear_downstream(gsv_port *p)
{
    gsv_port *cursor = NULL, *q = NULL;
    while (cursor = q, gsv_walk_downstream(p, &cursor, &q) == 0) {
        if (q->kind == 5) { q->video->flags = 0; q->audio->flags = 0; q->status->u16 = 0xffff; q->status->u18 = 0xffff; }
    }
}

static void rx_detect_check(gsv_port *p)   /* 0x08011720 */
{
    gsv_rx_info *r = p->rx;
    if (r->mode == 5) rx_power_down_full(p);
    if (r->plug_sub == 4 || r->plug_sub == 5) {
        if (r->mode == 0 && r->lock_delay > 0x0e) {     /* SDK 20210311 waits 15 passes, not 50 */
            bool connected = false;
            gsv_port *cursor = NULL, *q = NULL;
            while (gsv_walk_downstream(p, &cursor, &q) == 0) { if ((q->kind == 5 || q->kind == 6) && q->status->plug == 5) connected = true; cursor = q; }
            cursor = NULL; q = NULL;
            while (cursor = q, gsv_walk_audio_downstream(p, &cursor, &q) == 0) { if (q->kind == 0xc || q->kind == 0xe) connected = true; }
            if (connected) { rx_power_up(p); r->plug_sub = 5; }
        }
    } else if (r->mode == 1 || r->mode == 2) rx_power_down_full(p);
    else if (r->mode == 3) rx_power_up(p);
}

static void rx_read_colordepth(gsv_port *p)    /* 0x08009ea8 */
{
    uint8_t d = 0;
    gsv_read_bits(gsv_rx_dev_e9a8(p), 3, 0x30, 4, &d);
    p->video->depth_code = d == 0 ? 0x18 : d == 1 ? 0x1e : d == 2 ? 0x24 : 0;
}
static void tx_sync_polarity(gsv_port *tx)     /* 0x0800de1c */
{
    gsv_port *root = gsv_root_source(tx);
    if (root == NULL || root->rx->locked != 0) {
        const uint32_t core = GSV_PAGE(0x40);    /* Gsv2k1 0x08018b1c: the one core, whatever the output's index */
        gsv_write_bits(core, 0x1f, 2, 1, (uint8_t)(1 - tx->video->hsync_pol));
        gsv_write_bits(core, 0x1f, 1, 0, (uint8_t)(1 - tx->video->vsync_pol));
    }
}
/* GSV2001 (Gsv2k1 0x08015edc, 0x0802fd66): four more timing words, the interlace doubling on the vertical total, and
 * the colour depth passed on only as far as the sink's capability bits allow. */
static void k1_rx_read_timing_regs(gsv_port *p)   /* 0x08015edc */
{
    gsv_video *v = p->video; uint32_t t = 0;
    gsv_read_bits(GSV_PAGE(0x20), 0x0c, 0x20, 5, &v->interlaced);
    gsv_read_field(GSV_PAGE(0x20), 0x8002a, 0x1f, 0xff, 0, 1, 2, &t); v->t2a = (uint16_t)t;
    gsv_read_field(GSV_PAGE(0x20), 0x8002c, 0x1f, 0xff, 0, 1, 2, &t); v->t2c = (uint16_t)t;
    gsv_read_field(GSV_PAGE(0x20), 0x80036, 0x3f, 0xff, 0, 1, 2, &t); v->t36 = (uint16_t)((t & 0x1ffff) >> 1);
    gsv_read_field(GSV_PAGE(0x20), 0x8003a, 0x3f, 0xff, 0, 1, 2, &t); if (t & 1) t++; v->t3a = (uint16_t)((t & 0x1ffff) >> 1);
    gsv_read_field(GSV_PAGE(0x20), 0x80026, 0x3f, 0xff, 0, 1, 2, &t); v->hactive = (uint16_t)t;
    gsv_read_field(GSV_PAGE(0x20), 0x8000e, 0x1f, 0xff, 0, 1, 2, &t);
    v->vtotal = v->interlaced == 0 ? (uint16_t)t : (uint16_t)((t & 0x7fff) << 1);
    gsv_read_field(GSV_PAGE(0x20), 0x8002e, 0x3f, 0xff, 0, 1, 2, &t); v->vactive = (uint16_t)((t & 0x1ffff) >> 1);
    gsv_read_field(GSV_PAGE(0x20), 0x8000c, 0x1f, 0xff, 0, 1, 2, &t); v->htotal = (uint16_t)t;
    t = (uint32_t)((int)(1000000u * v->pixclk) / (int)((uint32_t)v->vactive * v->hactive));
    v->frame_rate = (uint16_t)(t + 1);
    gsv_read_bits(GSV_PAGE(0x20), 0x0c, 0x80, 7, &v->hsync_pol);
    gsv_read_bits(GSV_PAGE(0x20), 0x0c, 0x40, 6, &v->vsync_pol);
    gsv_read_bits(GSV_PAGE(0x22), 0x81, 1, 0, &v->three_d);
}
void gsv_k1_tx_depth_limit(gsv_port *src, gsv_port *q)   /* 0x08030470: the input's colour depth, as far as the sink can take it */
{
    gsv_video *v = src->video; uint32_t caps = q->status->u10;
    q->video->depth_code = v->depth_code;
    if (v->depth_code == 0x24 && (!(caps & 0x4000000) || (v->pixclk > 0x113 && (!(caps & 0x40) || (v->colorspace == 3 && !(caps & 8)))))) q->video->depth_code = 0x18;
    if (v->depth_code == 0x1e && (!(caps & 0x8000000) || (v->pixclk > 0x113 && (!(caps & 0x80) || (v->colorspace == 3 && !(caps & 4)))))) q->video->depth_code = 0x18;
}
static void rx_read_timing(gsv_port *p)        /* 0x0802fd66 */
{
    gsv_video *v = p->video;
    k1_rx_read_timing_regs(p);
    rx_read_colordepth(p);
    gsv_port *cursor = NULL, *q = NULL;
    while (cursor = q, gsv_walk_downstream(p, &cursor, &q) == 0) {
        if (q->kind != 5) continue;
        memcpy(&q->video->vic, &v->vic, 0xe);
        q->video->t2a = v->t2a; q->video->t2c = v->t2c; q->video->t36 = v->t36; q->video->t3a = v->t3a;
        gsv_k1_tx_depth_limit(p, q);
        tx_sync_polarity(q);
    }
}
static void rx_receiving_enter(gsv_port *p)    /* 0x08011afa -> 0x08009234 */
{
    if (p->idx != 0xff) gsv_write_bits(gsv_rx_dev_e984(p), 0x5a, 2, 1, 1);
}

void gsv_rx_plug_fsm_step(gsv_port *p)   /* 0x08010d38 */
{
    p->rx->inactive = p->idx == 0xff;
    switch (*p->plug_state) {
    case 2: case 4: case 5: case 6: case 8: case 9: gsv_rx_lock_delay(p); break;
    default: break;
    }
}

void gsv_rx_plug_fsm(gsv_port *p)        /* 0x08006d08 */
{
    gsv_rx_info *r = p->rx;
    bool to_detect = false, to_pulldown = false;
    switch (*p->plug_state) {
    default:
        if (r->inactive == 0) {
            if (r->plug_sub == 5 && r->cable_5v != 0) {
                if (r->mode == 0 || r->mode == 2 || r->mode == 5) { rx_clear_downstream(p); to_detect = true; }
                else if (r->lock_b == 0) to_detect = true;
            } else {
                rx_clear_downstream(p); gsv_rx_reset_port(p); r->stable = 0;
                *p->plug_state = RXPLUG_PULL_DOWN_HPD; rx_power_down_full(p);
            }
        } else {
            rx_clear_downstream(p); r->stable = 0; *p->plug_state = RXPLUG_RESET;
            rx_info_reset(p); gsv_rx_reset_port(p); rx_power_down_full(p);
        }
        break;
    case RXPLUG_DEFAULT:
        *p->plug_state = RXPLUG_RESET; rx_info_reset(p); gsv_rx_reset_port(p); rx_power_down_full(p); break;
    case RXPLUG_DETECT:
        if (r->cable_5v == 0) to_pulldown = true;
        else if (r->inactive == 0) {
            if (r->plug_sub == 5) { if (r->mode == 1 && r->lock_b != 0) *p->plug_state = RXPLUG_PLUGGED; else rx_detect_check(p); }
            else to_pulldown = true;
        } else { *p->plug_state = RXPLUG_RESET; rx_info_reset(p); gsv_rx_reset_port(p); rx_power_down_full(p); }
        break;
    case RXPLUG_INFO_UPDATE:
        if (r->lock_b == 0) { *p->plug_state = RXPLUG_DETECT; rx_detect_check(p); }
        else { *p->plug_state = RXPLUG_STABLE; r->stable = 1; }
        break;
    case RXPLUG_INPUT_LOCK:
        if (r->inactive == 0) {
            if (r->lock_b == 0) { *p->plug_state = RXPLUG_DETECT; rx_detect_check(p); }
            else if (r->locked == 0) *p->plug_state = RXPLUG_PLUGGED;
            else if (r->plug_sub == 5 && r->mode == 1) *p->plug_state = RXPLUG_READ_TIMING;   /* 0x080121c4 is a no-op */
            else { *p->plug_state = RXPLUG_PULL_DOWN_HPD; rx_power_down_full(p); }
        } else { rx_clear_downstream(p); *p->plug_state = RXPLUG_RESET; rx_info_reset(p); gsv_rx_reset_port(p); rx_power_down_full(p); }
        break;
    case RXPLUG_PLUGGED:
        if (r->lock_b == 0) { *p->plug_state = RXPLUG_DETECT; rx_detect_check(p); }
        else if (r->plug_sub == 5 && r->mode == 1) { if (r->locked == 1) *p->plug_state = RXPLUG_INPUT_LOCK; }
        else { *p->plug_state = RXPLUG_PULL_DOWN_HPD; rx_power_down_full(p); }
        break;
    case RXPLUG_PULL_DOWN_HPD:
        if (r->cable_5v == 0 || r->plug_sub != 5) { if (r->plug_sub != 5) gsv_event(p, 0xc); }   /* 0x08011866 */
        else { *p->plug_state = RXPLUG_DETECT; rx_detect_check(p); }
        break;
    case RXPLUG_READ_TIMING:
        if (r->lock_b == 0) { *p->plug_state = RXPLUG_DETECT; rx_detect_check(p); }
        else { *p->plug_state = RXPLUG_INFO_UPDATE; rx_read_timing(p); }
        break;
    case RXPLUG_RESET:
        if (r->inactive == 0) { *p->plug_state = RXPLUG_PULL_DOWN_HPD; rx_power_down_full(p); }
        break;
    }
    if (to_pulldown) { *p->plug_state = RXPLUG_PULL_DOWN_HPD; rx_power_down_full(p); }
    if (to_detect) { r->stable = 0; *p->plug_state = RXPLUG_DETECT; rx_detect_check(p); }
}

static void rx_read_5v(gsv_port *p)       /* 0x0800f244 */
{
    uint8_t v[4] = { 0 };
    if (p->id == 0) gsv_read_bits(GSV_PAGE(0x03), 0x10, 2, 1, v);
    else if (p->id == 1) gsv_read_bits(GSV_PAGE(0x03), 0x14, 2, 1, v);
    p->rx->cable_5v = v[0];
}



/* ---- GSV2001 (Gsv2k1 driver, SDK 20210311) ---- */
void gsv_k1_pll_kick(gsv_port *p)        /* 0x08019360: restart the PLL's frequency tracking when its two counters disagree */
{
    uint32_t a = 0, b = 0; uint8_t base, kick;
    if (p->id == 0) { base = 0x33; kick = 0x62; } else if (p->id == 4 || p->id == 5) { base = 0x93; kick = 0x52; } else return;
    gsv_read_field(GSV_PAGE(0x02), 0x80000u | base, 3, 0xff, 0, 1, 3, &a);
    gsv_read_field(GSV_PAGE(0x02), 0x80000u | (uint8_t)(base + 3), 3, 0xff, 0, 1, 3, &b);
    if ((b < a ? a - b : b - a) > 10) {
        gsv_write_bits(GSV_PAGE(0x05), kick, 0x40, 6, 1);
        gsv_write_bits(GSV_PAGE(0x05), kick, 0x40, 6, 0);
    }
}
static int k1_rx_check_lock(gsv_port *p)   /* 0x08015828 */
{
    gsv_rx_info *r = p->rx; uint8_t a = 0, b = 0, unlock = 0;
    gsv_read_bits(GSV_PAGE(0x20), 0, 0x80, 7, &r->lock_b);
    gsv_read_bits(GSV_PAGE(0x20), 0, 0x08, 3, &r->lock_a);
    gsv_read_bits(GSV_PAGE(0x20), 0, 0x10, 4, &r->lock_c);
    if (r->lock_b == 0) gsv_k1_pll_kick(p);
    gsv_read_bits(GSV_PAGE(0x03), 0x11, 1, 0, &unlock);
    if (unlock) { gsv_write_bits(GSV_PAGE(0x03), 0x11, 4, 2, 1); r->lock_b = 0; r->eq_done = 0; r->audio_present = 0; }
    gsv_read_bits(GSV_PAGE(0x03), 0x38, 1, 0, &b);
    gsv_read_bits(GSV_PAGE(0x03), 0x38, 0x10, 4, &a);
    if (b == 1 || a == 1) { r->lock_a = 0; r->lock_c = 0; gsv_write_bits(GSV_PAGE(0x03), 0x38, 4, 2, 1); gsv_write_bits(GSV_PAGE(0x03), 0x38, 0x40, 6, 1); }
    if (unlock) { gsv_log("Port %d: TMDS PLL Unlock ST triggers EQ ReRun\n", p->id); gsv_rx_eye_width(p, 1); }
    if (r->lock_a == 0 || r->lock_c == 0 || r->lock_b == 0) r->locked = 0;
    return 0;
}
static void k1_audio_type(gsv_port *p)     /* 0x08014bc0 */
{
    gsv_audio *au = p->audio; uint8_t c = 0;
    gsv_read_bits(GSV_PAGE(0x22), 0x19, 4, 2, &c);
    if (c == 1) { au->fs = 1; au->type = 0; }
    else {
        gsv_read_bits(GSV_PAGE(0x22), 0x19, 8, 3, &c);
        if (c == 1) { au->fs = 7; au->type = 2; }
        else {
            gsv_read_bits(GSV_PAGE(0x22), 0x19, 0x10, 4, &c);
            if (c == 1) { au->fs = 9; au->type = 4; }
            else {
                gsv_read_bits(GSV_PAGE(0x22), 0x19, 0x20, 5, &c);
                if (c == 1) { au->fs = 1; au->type = 3; } else { au->fs = 0xc; au->type = 0xff; }
            }
        }
    }
    gsv_read_bits(GSV_PAGE(0x22), 100, 0x40, 6, &c);
    if (c != 1) gsv_write_bits(GSV_PAGE(0x22), 100, 0x40, 6, 1);
}
static void k1_flag(uint16_t *flags, uint16_t *changed, uint16_t bit, unsigned on)
{
    if ((on != 0) != ((*flags & bit) != 0)) *changed |= bit;
    if (on) *flags |= bit; else *flags &= (uint16_t)~bit;
}
int gsv_rx_lock_delay(gsv_port *p)   /* 0x08014cdc: as below with one input, whole-byte flag reads and a 16-pass delay */
{
    gsv_rx_info *r = p->rx; gsv_video *vd = p->video; gsv_audio *au = p->audio;
    uint8_t v = 0, c = 0, b = 0;
    rx_read_5v(p);
    if (r->cable_5v == 1) {
        if (r->mode == 0 && r->lock_delay < 0x10) { r->lock_delay++; gsv_log("Port Rx %d: Lock Delay = %d\n", p->id, r->lock_delay); }
    } else { r->lock_delay = 0; r->locked = 0; vd->flags = 0; au->flags = 0; }
    if (p->idx == 0xff || r->mode != 1) {
        r->stable = 0; r->locked = 0; r->encrypted = 0; r->audio_present = 0; r->lock_a = r->lock_b = r->lock_c = 0; r->eq_done = 0; r->hdmi_mode = 0;
        vd->v2 = 0xff; vd->v3 = 0xff;
    } else {
        k1_rx_check_lock(p);
        gsv_read_bits(GSV_PAGE(0x20), 0, 0x20, 5, &r->hdmi_mode);
        gsv_read_bits(GSV_PAGE(0x20), 3, 0x0f, 0, &vd->colordepth);
        gsv_read_bits(GSV_PAGE(0x20), 3, 0x30, 4, &v);
        uint32_t clk = 0;
        gsv_read_field(GSV_PAGE(0x02), 0x80039, 0x0f, 0xff, 0, 1, 2, &clk);
        gsv_read_bits(GSV_PAGE(0x02), 0x2f, 7, 0, &c);
        clk /= ((1u << c) * 5) & 0xff;
        if (vd->tmds_clk != clk) {
            uint32_t d = clk < vd->tmds_clk ? vd->tmds_clk - clk : clk - vd->tmds_clk;
            vd->tmds_clk = (uint16_t)clk;
            if (d > 5 || vd->tmds_clk < 0xb) { r->lock_a = 0; r->lock_c = 0; r->lock_b = 0; r->eq_done = 0; vd->pixclk = 0; }
        }
        if (r->lock_b == 1) {
            if (v == 1) clk = (clk << 2) / 5; else if (v == 2) clk = (clk << 1) / 3;
            vd->pixclk = (uint16_t)clk;
            if (r->lock_a == 1 && r->eq_done == 1 && r->lock_c == 1) {
                if (r->locked == 0) {
                    vd->flags = 0; vd->vic = 0; au->flags = 0;
                    gsv_write_bits(GSV_PAGE(0x22), 0x5a, 4, 2, 1);
                    gsv_write_bits(GSV_PAGE(0x22), 0x5a, 4, 2, 0);
                }
                r->locked = 1;
            } else { r->locked = 0; gsv_rx_eye_width(p, 0); }
        }
        if (r->locked == 1) {
            gsv_read_bits(GSV_PAGE(0x22), 0x18, 0x80, 7, &c);
            if (c == 1) {
                gsv_read_bits(GSV_PAGE(0x20), 0x7a, 1, 0, &c);
                if (c == 1) {
                    gsv_write_bits(GSV_PAGE(0x20), 0x7a, 2, 1, 0); gsv_write_bits(GSV_PAGE(0x20), 0x7a, 1, 0, 0);
                    gsv_write_bits(GSV_PAGE(0x22), 0x5a, 4, 2, 1); gsv_write_bits(GSV_PAGE(0x22), 0x5a, 4, 2, 0);
                    r->audio_present = 0;
                } else {
                    gsv_read_bits(GSV_PAGE(0x22), 4, 1, 0, &r->audio_present);
                    if (r->audio_present) {
                        gsv_read_bits(GSV_PAGE(0x22), 0x17, 0x1e, 0, &c);
                        if (c != 0) { gsv_write_bits(GSV_PAGE(0x22), 0x5a, 4, 2, 1); gsv_write_bits(GSV_PAGE(0x22), 0x5a, 4, 2, 0); }
                        k1_audio_type(p);
                        uint8_t want = (au->type == 2 || au->type == 4) ? 0 : 1;
                        gsv_read_bits(GSV_PAGE(0x22), 0x62, 0x80, 7, &c);
                        if (want != c) gsv_write_bits(GSV_PAGE(0x22), 0x62, 0x80, 7, want);
                        gsv_read_bits(GSV_PAGE(0x20), 0, 2, 1, &c);
                        if (c == 0) {
                            au->channels = 2;
                            gsv_read_bits(GSV_PAGE(0x22), 0x87, 0x18, 3, &c);
                            want = (au->type == 2 || au->type == 4) ? 1 : 0;
                            if (c != want) gsv_write_bits(GSV_PAGE(0x22), 0x87, 0x18, 3, want);
                        } else {
                            gsv_read_bits(GSV_PAGE(0x22), 0x19, 0x80, 7, &c);
                            if (c == 1) {
                                gsv_read_bits(GSV_PAGE(0x24), 0x1d, 0xff, 0, &c);
                                au->channels = (uint8_t)((c & 7) + 1);
                                gsv_read_bits(GSV_PAGE(0x22), 0x65, 0x3f, 0, &c);
                                want = (uint8_t)(((c - 1) & 0x1ff) >> 1);
                                if (au->type == 3 || au->type == 2 || au->type == 4) want = 3;
                                gsv_read_bits(GSV_PAGE(0x22), 0x87, 0x18, 3, &c);
                                if (c != want) gsv_write_bits(GSV_PAGE(0x22), 0x87, 0x18, 3, want);
                            }
                        }
                    }
                }
            }
        }
    }
    if (r->hdmi_mode == 0 && r->locked != 0) { vd->colorspace = 0; vd->v2 = 0x80; }
    if (r->hdmi_mode == 0 || r->locked == 0) return 0;
    gsv_read_bits(GSV_PAGE(0x22), 0x19, 0xff, 0, &b);
    k1_flag(&vd->flags, &r->video_changed, 0x0001, b & 0x40);
    k1_flag(&au->flags, &r->audio_changed, 0x0002, b & 0x80);
    gsv_read_bits(GSV_PAGE(0x22), 0x18, 0xff, 0, &b);
    k1_flag(&vd->flags, &r->video_changed, 0x0008, b & 0x01);
    k1_flag(&vd->flags, &r->video_changed, 0x0100, b & 0x02);
    k1_flag(&vd->flags, &r->video_changed, 0x0200, b & 0x04);
    k1_flag(&au->flags, &r->audio_changed, 0x0004, b & 0x08);
    k1_flag(&vd->flags, &r->video_changed, 0x0010, b & 0x10);
    k1_flag(&vd->flags, &r->video_changed, 0x0020, b & 0x20);
    k1_flag(&vd->flags, &r->video_changed, 0x0040, b & 0x40);
    k1_flag(&au->flags, &r->audio_changed, 0x1000, b & 0x80);
    gsv_read_bits(GSV_PAGE(0x22), 0x17, 0xff, 0, &b);
    k1_flag(&vd->flags, &r->video_changed, 0x0080, b & 0x01);
    k1_flag(&vd->flags, &r->video_changed, 0x2000, b & 0x40);
    gsv_read_bits(GSV_PAGE(0x22), 0x65, 0x80, 7, &b);
    k1_flag(&au->flags, &r->audio_changed, 0x0400, b);
    gsv_read_bits(GSV_PAGE(0x03), 0x36, 0x20, 5, &vd->avmute);
    gsv_read_bits(GSV_PAGE(0x03), 0x35, 0x20, 5, &r->hdmi_mode);
    if (au->type == 0xff || r->audio_present == 0) au->flags = 0;
    return 0;
}


/* ---- Rx main state machine (0x08007444) --------------------------------------------------------------- */
static int rx_set_bcaps(gsv_port *p)      /* 0x0800b8f4 "Set BCaps/Status REC mode" */
{
    if (p->idx == 0) {
        gsv_write_bits(gsv_rx_dev26(p), 0x40, 0xff, 0, 0x80);
        gsv_write_field(gsv_rx_dev26(p), 0x80041, 0xff, 0xff, 0, 1, 2, 0);
        gsv_log(p->hdcp->downstream_count == 0 ? "Port %d: Set BCaps/Status REC mode\n" : "Port %d: Set BCaps/Status REP mode\n", p->id);
    }
    return 0;
}
static void rx_hdcp_reset_rec(gsv_port *p)   /* 0x080116bc */
{
    p->hdcp->u1 = 0; p->hdcp->downstream_count = 0; p->hdcp->u3 = 0; p->hdcp->u4 = 0;
    rx_set_bcaps(p);
    gsv_log("Port %d: HDCP Reset to REC Mode\n", p->id);
}
static void rx_reset_all(gsv_port *p)    /* 0x08011bda */
{
    /* Gsv2k1 0x08016120: back to input 0, no source selected */
    p->idx = 0;
    gsv_write_bits(GSV_PAGE(0x00), 0x01, 0x70, 4, 0);
    gsv_rx_disable_hpa(p);     /* 0x0800b8c4 */
    memset(p->video, 0, sizeof *p->video);
    memset(p->audio, 0, sizeof *p->audio);
    rx_hdcp_reset_rec(p);
    gsv_event(p, 10);                                          /* 0x08011708 */
}

void gsv_rx_fsm_step(gsv_port *p) { (void)p; }   /* 0x08010e4c: every state calls an empty function */

static void rx_audio_intpt(gsv_port *p);
void gsv_rx_fsm(gsv_port *p)             /* 0x08007444 */
{
    gsv_rx_info *r = p->rx;
    bool to_reset = false, to_freerun = false, to_receiving = false;
    switch (*p->rx_state) {
    case RX_DEFAULT: *p->rx_state = RX_RESET; rx_reset_all(p); break;
    case RX_DETECT:
        if (r->inactive == 1) { *p->rx_state = RX_RESET; rx_reset_all(p); }
        else if ((r->locked == 0 && r->u1 != 0) || r->u2 != 0) *p->rx_state = RX_FREERUN;   /* 0x08011b14 -> no-op */
        else if (r->locked == 1) { *p->rx_state = RX_RECEIVING; rx_receiving_enter(p); }
        break;
    case RX_FREERUN:
        if (r->u1 == 0) *p->rx_state = RX_DETECT;
        else if (r->inactive == 1) { *p->rx_state = RX_RESET; rx_reset_all(p); }
        else if (r->locked != 0 && r->u2 == 0) { *p->rx_state = RX_RECEIVING; rx_receiving_enter(p); }
        break;
    case RX_RECEIVING:
        if (r->inactive == 0) {
            if (r->u2 == 0) {
                if (r->locked == 0) { if (r->u1 == 0) { if (r->u2 == 0) to_reset = true; else to_receiving = true; } else to_freerun = true; }
                else to_receiving = true;
            } else to_freerun = true;
        } else to_reset = true;
        break;
    default: if (r->inactive == 0) *p->rx_state = RX_DETECT; break;
    }
    if (to_receiving) { gsv_intpt_dispatch(p); rx_audio_intpt(p); }
    if (to_freerun) *p->rx_state = RX_FREERUN;
    if (to_reset) { *p->rx_state = RX_RESET; rx_reset_all(p); }
}

/* ---- Rx HDCP status (0x0801204c and callees) ----------------------------------------------------------- */
static int rx_hdcp_status(gsv_port *p, uint8_t *out)   /* 0x08009a3c "Rx is encrypted with HDCP" */
{
    uint8_t running22 = 0, bd_hi = 0, bd_lo = 0, t = 0;
    uint32_t hi_bit, lo_bit, want_hi, want_lo;
    out[0] = out[1] = 0;
    gsv_read_bits(GSV_PAGE(0x03), 0x5f, 1, 0, &out[0]);
    gsv_read_bits(GSV_PAGE(0x03), 0x39, 0x10, 4, &out[1]);
    gsv_read_bits(GSV_PAGE(0x03), 0x5f, 2, 1, &p->hdcp->u4);
    gsv_read_bits(GSV_PAGE(0x03), 0x39, 0x20, 5, &p->rx->encrypted);
    gsv_read_bits(GSV_PAGE(0x03), 0x3a, 2, 1, &running22);
    if (out[1] != 0) {
        if (running22 == 0) { if (p->rx->encrypted == 1) gsv_log("Port Rx %d: Rx is encrypted with HDCP 1.4\n", p->id); }
        else if (running22 == 1 && p->rx->encrypted == 1) gsv_log("Port Rx %d: Rx is encrypted with HDCP 2.2\n", p->id);
    }
    gsv_read_bits(GSV_PAGE(0x00), 0xbd, 0xc0, 6, &bd_hi);
    gsv_read_bits(GSV_PAGE(0x00), 0xbd, 0x0c, 2, &bd_lo);
    hi_bit = bd_hi & 1; lo_bit = bd_lo & 1; bd_hi &= 2; bd_lo &= 2;
    want_lo = lo_bit;
    switch (p->hdcp->mode) {
    case 1: want_hi = 0; break;
    case 2: {
        want_hi = 0; want_lo = 0;
        gsv_port *cursor = NULL, *q = NULL;
        while (gsv_walk_downstream(p, &cursor, &q) == 0) {
            if (q->kind == 5) {
                if (q->status->hdmi_mode == 0) { if (q->status->plug == 5) want_hi = 1; }
                else if (q->hdcp_tx->enabled == 1) {
                    uint8_t a = 0, b = 0;
                    gsv_read_bits(gsv_tx_dev(q), 0xcd, 2, 1, &a);
                    gsv_read_bits(gsv_tx_dev(q), 0xcd, 1, 0, &b);
                    if (q->hdcp_tx->capable22 == 0 && (a == 0 || b == 0)) want_hi = 1;
                }
            }
            cursor = q;
        }
        break; }
    case 3: want_hi = 1; want_lo = 0; break;
    case 4: want_hi = 1; want_lo = 1; break;
    default: want_hi = hi_bit; break;
    }
    if (want_hi != hi_bit || want_lo != lo_bit) {
        p->rx->mode = 5;
        gsv_write_bits(GSV_PAGE(0x00), 0xbd, 0xc0, 6, (uint8_t)(want_hi | bd_hi));
        gsv_write_bits(GSV_PAGE(0x00), 0xbd, 0x0c, 2, (uint8_t)(want_lo | bd_lo));
    }
    if (out[1] != 0) gsv_write_bits(GSV_PAGE(0x03), 0x39, 0x40, 6, 1);

    bool r22 = running22 == 1;
    if ((bool)p->hdcp->running22 != r22) {
        p->hdcp->running22 = r22;
        gsv_log(r22 ? "Port Rx %d: Rx is Running HDCP 2.2\n" : "Port Rx %d: Rx is Stopping HDCP 2.2\n", p->id);
        if (r22) {                              /* Gsv2k1 takes this flag from page 0x21 reg 0x4e..0x4f */
            uint32_t w = 0;
            gsv_read_field(GSV_PAGE(0x21), 0x8004e, 0xff, 0xff, 0, 1, 2, &w);
            p->hdcp->u6 = w != 0;
        }
    }
    gsv_read_bits(gsv_rx_dev21(p), 7, 1, 0, &t);
    uint8_t want = (uint8_t)(1 - p->rx->locked);
    if (want != t) gsv_write_bits(gsv_rx_dev21(p), 7, 1, 0, want);
    if (p->rx->encrypted == 0 && p->hdcp->running22 == 0) gsv_app_status_set(1, 0);
    else if (p->rx->encrypted == 1 && p->hdcp->running22 == 0) gsv_app_status_set(1, 1);
    else if (p->rx->encrypted == 1 && p->hdcp->running22 == 1) gsv_app_status_set(1, 2);
    else gsv_app_status_set(1, 0);
    return 0;
}

static void rx_hdcp_poll(gsv_port *p)     /* 0x08011632 */
{
    if (p->kind != 0 || p->hdcp->mode == 0 || p->chain != NULL) return;
    uint8_t st[4] = { 0 };
    if (rx_hdcp_status(p, st) == 0 && st[1] != 0) gsv_event(p, p->rx->encrypted == 0 ? 3 : 2);
    if (st[0] != 0) gsv_write_bits(GSV_PAGE(0x03), 0x5f, 4, 2, 1);          /* 0x08009260 */
    if (p->rx->encrypted == 1 && p->rx->eq_done == 1 && p->rx->locked == 0) p->hdcp->counter++;
    if (p->hdcp->counter > 9) { p->rx->mode = 5; p->hdcp->counter = 0; }
}

static void rx_count_downstream_hdcp(gsv_port *p)   /* 0x080115de */
{
    gsv_port *cursor = NULL, *q = NULL;
    p->hdcp->downstream_count = 0;
    while (cursor = q, gsv_walk_downstream(p, &cursor, &q) == 0) {
        if (q->kind == 5) { gsv_tx_hdcp_poll(q); if (q->hdcp_tx->enabled == 1 || q->hdcp_tx->capable22 == 1) p->hdcp->downstream_count++; }
    }
}

void gsv_tx_update_mute_regs(gsv_port *p);   /* 0x0800d968, gsv_tx.c */

static void rx_hdcp_propagate(gsv_port *p)   /* 0x08012064 "Input Hdcp Off,Disable Tx HDCP" */
{
    gsv_port *cursor = NULL, *q = NULL;
    p->hdcp->downstream_count = 0;
    /* SDK 20210311 (0x080302b4): once input and output are stable for 60 passes */
    while (cursor = q, gsv_walk_downstream(p, &cursor, &q) == 0)
        if (q->kind == 5 && *p->plug_state == 9 && *q->tx_state == 10 && q->status->s6 > 0x3b) gsv_tx_hdcp_cipher(q);
    return;
}

void gsv_rx_hdcp_fsm_step(gsv_port *p)   /* 0x0801204c */
{
    rx_hdcp_poll(p);
    rx_count_downstream_hdcp(p);
    rx_hdcp_propagate(p);
}

/* ---- Rx interrupt handling (0x08011c08 and callees) -------------------------------------------------- */
/* 0x0800b0e4: read the video-side interrupt flags into f[18] */
static int rx_intpt_read(gsv_port *p, uint8_t *f)
{
    uint8_t t = 0;
    memset(f, 0, 0x12);
    /* Gsv2k1 0x080159bc: the same flags from two block reads, page 3 reg 0x30.. and 0x50.. */
    uint8_t a[15] = { 0 }, b[27] = { 0 };
    if (p->idx != 0) return -3;
    gsv_read_block(GSV_PAGE(0x03), 0x30, a, 15);
    gsv_read_block(GSV_PAGE(0x03), 0x50, b, 27);
    #define BIT(x, n) (((x) >> (n)) & 1)
    f[0] = (a[6] & 0x10) || p->video->avmute != BIT(a[6], 5);
    f[1] = (a[5] & 0x10) || p->rx->hdmi_mode != BIT(a[5], 5);
    f[2] = (a[8] & 0x01) || p->rx->lock_a != BIT(a[8], 1);
    f[3] = (a[8] & 0x10) || p->rx->lock_c != BIT(a[8], 5);
    f[4] = (a[9] & 0x01) || p->video->three_d != BIT(a[9], 1);
    f[5] = (b[8] & 0x30) != 0;
    f[6] = (b[11] & 0x03) != 0;
    f[7] = (b[11] & 0x30) != 0;
    f[8] = (b[5] & 0x03) != 0;
    if ((a[0] & 1) == 0 && (b[0] & 1) == 0) {
        if (p->video->flags & 1) { gsv_read_bits(GSV_PAGE(0x24), 0, 0xff, 0, &t); if (p->rx->u18[0] != t) f[9] = 1; }
    } else f[9] = 1;
    f[10] = (a[1] & 0x01) || (b[1] & 0x01);
    f[11] = (a[13] & 0x10) || (b[26] & 0x10);
    f[12] = (a[1] & 0x10) || (b[1] & 0x10);
    f[13] = (a[2] & 0x01) || (b[2] & 0x01);
    f[14] = (a[3] & 0x01) || (b[3] & 0x01);
    f[15] = (a[3] & 0x10) || (b[3] & 0x10);
    f[16] = (a[4] & 0x01) || (b[4] & 0x01);
    if (a[5] & 1) f[17] = 1;
    else { t = BIT(a[5], 1); f[17] = ((p->video->flags & 0x80) != 0) != (t != 0); }
    #undef BIT
    static const char *k1names[] = { "AvMute", "HdmiModeChg", "DeRegenLck", 0, "Vid3dDet", "NewTmds", "BadTmdsClk", "DeepClrChg", "PktErr", "AvIfValid",
                                     "SpdValid", "HdrValid", "MsValid", "VsValid", "Isrc1Valid", "Isrc2Valid", "GamutValid", "GcValid" };
    for (int i = 0; i < 18; i++) if (f[i] == 1 && k1names[i]) gsv_log("Port%d: Intpt->%s\n", p->id, k1names[i]);
    return 0;
}
/* 0x08009284: acknowledge the flags read above */
static void rx_intpt_clear(uint8_t *f)
{
    if (f[0]) gsv_write_bits(GSV_PAGE(0x03), 0x36, 0x40, 6, 1);
    if (f[1]) gsv_write_bits(GSV_PAGE(0x03), 0x35, 0x40, 6, 1);
    if (f[4]) gsv_write_bits(GSV_PAGE(0x03), 0x39, 4, 2, 1);
    if (f[5]) gsv_write_bits(GSV_PAGE(0x03), 0x58, 0x40, 6, 1);
    if (f[6]) gsv_write_bits(GSV_PAGE(0x03), 0x5b, 4, 2, 1);
    if (f[7]) gsv_write_bits(GSV_PAGE(0x03), 0x5b, 0x40, 6, 1);
    if (f[8]) gsv_write_bits(GSV_PAGE(0x03), 0x55, 4, 2, 1);
    if (f[9]) { gsv_write_bits(GSV_PAGE(0x03), 0x30, 4, 2, 1); gsv_write_bits(GSV_PAGE(0x03), 0x50, 4, 2, 1); }
    if (f[10]) { gsv_write_bits(GSV_PAGE(0x03), 0x31, 4, 2, 1); gsv_write_bits(GSV_PAGE(0x03), 0x51, 4, 2, 1); }
    if (f[11]) { gsv_write_bits(GSV_PAGE(0x03), 0x3d, 0x40, 6, 1); gsv_write_bits(GSV_PAGE(0x03), 0x6a, 0x40, 6, 1); }
    if (f[12]) { gsv_write_bits(GSV_PAGE(0x03), 0x31, 0x40, 6, 1); gsv_write_bits(GSV_PAGE(0x03), 0x51, 0x40, 6, 1); }
    if (f[13]) { gsv_write_bits(GSV_PAGE(0x03), 0x32, 4, 2, 1); gsv_write_bits(GSV_PAGE(0x03), 0x52, 4, 2, 1); }
    if (f[14]) { gsv_write_bits(GSV_PAGE(0x03), 0x33, 4, 2, 1); gsv_write_bits(GSV_PAGE(0x03), 0x53, 4, 2, 1); }
    if (f[15]) { gsv_write_bits(GSV_PAGE(0x03), 0x33, 0x40, 6, 1); gsv_write_bits(GSV_PAGE(0x03), 0x53, 0x40, 6, 1); }
    if (f[16]) { gsv_write_bits(GSV_PAGE(0x03), 0x34, 4, 2, 1); gsv_write_bits(GSV_PAGE(0x03), 0x54, 4, 2, 1); }
    if (f[17]) gsv_write_bits(GSV_PAGE(0x03), 0x35, 4, 2, 1);
}
static void rx_read_avmute(gsv_port *p)   { gsv_read_bits(gsv_rx_dev_e9a8(p), 0, 1, 0, &p->video->avmute); }    /* 0x08009a18 */
static void rx_read_hdmi_mode(gsv_port *p) { gsv_read_bits(gsv_rx_dev_e9a8(p), 0, 0x20, 5, &p->rx->hdmi_mode); } /* 0x08009efe */

void gsv_tx_depth_reg(gsv_port *tx);      /* 0x0800da64, gsv_tx.c */
int  gsv_tx_avmute(gsv_port *tx);         /* 0x0800d8a0 */

/* 0x08009f24: read one packet from the Rx packet RAM into pkt[] (31 bytes max) */
int gsv_rx_read_packet(gsv_port *p, int type, uint8_t *pkt)
{
    uint8_t t = 0; int rc = 0;
    switch (type) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 8: case 9:
        gsv_read_block(gsv_rx_dev_e9f0(p), gsv_pkt_rx_reg_hdr[type], pkt, 3);
        gsv_read_block(gsv_rx_dev_e9f0(p), gsv_pkt_rx_reg_body[type], pkt + 3, (uint16_t)(gsv_pkt_len[type] - 3));
        break;
    case 10:
        gsv_read_bits(gsv_rx_dev_e9a8(p), 0, 2, 1, pkt + 5);
        gsv_read_block(gsv_rx_dev_e984(p), gsv_pkt_rx_reg_hdr[type], pkt, 5);
        gsv_read_bits(GSV_PAGE(0x03), 0x37, 0x10, 4, &t);
        if (t) gsv_write_bits(GSV_PAGE(0x03), 0x37, 0x40, 6, 1);
        break;
    case 0xe:
        gsv_read_block(gsv_rx_dev_e9cc(p), gsv_pkt_rx_reg_hdr[type], pkt + 1, 2);
        gsv_read_block(gsv_rx_dev_e9cc(p), gsv_pkt_rx_reg_body[type], pkt + 3, (uint16_t)(gsv_pkt_len[type] - 3));
        pkt[0] = 0x87;
        break;
    default: rc = -4; break;
    }
    if (type == 0) p->rx->u18[0] = pkt[3];
    return rc;
}

/* 0x08012778: interpret a received packet into the port's video/audio content */
void gsv_parse_packet(gsv_port *p, int type, uint8_t *pkt)
{
    gsv_video *v = p->video; gsv_audio *a = p->audio;
    switch (type) {
    case 0: {   /* AVI infoframe */
        uint8_t vic = pkt[7], ext = 0, cs = 0;
        if (vic) v->vic = vic;
        gsv_log("\t###RxPort->content.video->timing.Vic = [%d]\r\n", g_gsv_port_list ? g_gsv_port_list->video->vic : 0);
        gsv_log("\t###port->content.video->timing.Vic Value = [%d]\r\n", vic);
        uint8_t colorimetry;
        if ((pkt[4] >> 5) == 0) colorimetry = (uint8_t)((pkt[6] & 0x0f) >> 2);
        else { ext = (uint8_t)((pkt[6] & 0x7f) >> 4); cs = (uint8_t)(pkt[5] >> 6); colorimetry = (uint8_t)(pkt[8] >> 6); }
        v->colorspace = (uint8_t)(pkt[4] >> 5);
        if (v->colorspace != 0 && cs == 0) cs = 2;
        v->v2 = (uint8_t)(cs + colorimetry * 0x40);
        if (cs == 3) v->v2 = (uint8_t)(v->v2 + ext * 4);
        if (v->v2 == 0 && v->colorspace != 0) v->v2 = 0x42;
        v->v3 = v->v2;
        uint8_t pr = (uint8_t)((pkt[5] & 0x3f) >> 4);
        if (pr == 1) v->aspect = 1; else if (pr == 2) v->aspect = 2;
        else if (v->vic < 0x6c) v->aspect = gsv_vic_aspect[v->vic]; else v->aspect = 0;
        v->colordepth = (uint8_t)(pkt[8] & 0x0f);
        break; }
    case 1:     /* audio infoframe */
        if ((pkt[4] & 7) == 0) {
            if (a->fs == 0xc) {
                switch (a->type) { case 2: a->fs = 7; break; case 3: a->fs = 1; break; case 4: a->fs = 9; break; default: a->fs = 1; break; }
            }
            if (pkt[7] < 0x20) { a->channels = gsv_audio_chan_table[pkt[7]]; pkt[4] = (uint8_t)((pkt[4] & 0xf8) | (a->channels & 7)); }
        } else a->channels = (uint8_t)((pkt[4] & 7) + 1);
        if (pkt[4] >> 4) a->layout = (uint8_t)(pkt[4] >> 4);
        break;
    case 9:     /* vendor specific infoframe: 3D / 4K VICs */
        v->three_d = (uint8_t)((pkt[7] & 0x7f) >> 6);
        v->three_d_fmt = (uint8_t)(pkt[8] >> 4);
        if (pkt[7] & 0x20) { switch (pkt[8]) { case 1: v->vic = 0x5f; break; case 2: v->vic = 0x5e; break; case 3: v->vic = 0x5d; break; case 4: v->vic = 0x62; break; default: break; } }
        break;
    case 10: {  /* audio channel status */
        a->ca = pkt[0] & 3; a->lsv_dm = (uint8_t)((pkt[0] & 7) >> 2); a->lfep = (uint8_t)((pkt[0] & 0x3f) >> 3);
        a->cs_byte1 = pkt[1]; a->cs_fs = pkt[2] & 0x0f; a->cs_wl = (uint8_t)((pkt[3] & 0x3f) >> 4); a->cs_byte4 = pkt[4] & 0x0f; a->cs_fs_ok = pkt[5];
        a->layout = (a->ca & 2) == 0 ? 1 : 0xff;
        if (pkt[2] >> 4) a->channels = (uint8_t)(pkt[2] >> 4);
        for (unsigned i = 0; gsv_fs_table[i + 1] != 0xff; i += 3) {
            if (gsv_fs_table[i + 1] == (pkt[3] & 0x0f)) {
                if (a->fs_index != gsv_fs_table[i]) {
                    a->fs_index = gsv_fs_table[i];
                    static const char *fs[] = { "32KHz", "44KHz", "48KHz", "88KHz", "96KHz", "176KHz", "192KHz", "HBR", "Audio From Stream" };
                    if (a->fs_index < 9) gsv_log("Port%d: %s Detected\n", p->id, fs[a->fs_index]);
                }
                break;
            }
        }
        break; }
    default: break;
    }
}

/* 0x08011f74: a packet of `type` (flag bit `bit`) changed on the Rx: re-read it and push it to every Tx */
static void rx_propagate_packet(gsv_port *p, uint8_t *buf, unsigned bit, int type, int audio)
{
    gsv_port *cursor = NULL, *q = NULL;
    if (audio == 0) {
        p->rx->video_changed &= (uint16_t)~bit;
        if (p->video->flags & bit) { gsv_rx_read_packet(p, type, buf); gsv_parse_packet(p, type, buf); }
    } else {
        p->rx->audio_changed &= (uint16_t)~bit;
        if (p->audio->flags & bit) { gsv_rx_read_packet(p, type, buf); gsv_parse_packet(p, type, buf); }
    }
    while (gsv_walk_downstream(p, &cursor, &q) == 0) {
        if (q->kind == 5) gsv_tx_apply_from_source(p, q, buf, bit, type, audio, 0);
        cursor = q; q = NULL;
    }
}

int gsv_intpt_dispatch(gsv_port *p)   /* 0x08011c08 */
{
    uint8_t f[0x12]; uint8_t buf[40];
    gsv_port *cursor, *q;
    if (p->rx->stable == 0) return 0;
    int rc = rx_intpt_read(p, f);
    rx_intpt_clear(f);
    /* SDK 20210311 (0x0802fe34): nothing is acted on in a pass that also reports a
                                             lock change, a new TMDS clock or a bad clock */
    if (f[2] || f[3] || f[5] || f[6]) return 0;
    if (rc == 0) {
        if (f[0]) {
            rx_read_avmute(p);
            cursor = NULL;
            while (q = NULL, gsv_walk_downstream(p, &cursor, &q) == 0) {
                if (q->kind == 5) { q->video->avmute = p->video->avmute; gsv_log("from Intpt.AvMute-->\n"); gsv_tx_avmute(q); q->video->flags |= 0x80; }
                buf[0] = 1; gsv_tx_packet_config(q, 7, buf, 1);
                cursor = q;
            }
        }
        if (f[1]) {
            rx_read_hdmi_mode(p);
            cursor = NULL;
            while (q = NULL, gsv_walk_downstream(p, &cursor, &q) == 0) {
                if (q->kind == 5 && q->status->plug == 0) { q->status->hdmi_mode = p->rx->hdmi_mode; gsv_k1_tx_hdmi_mode(q); }
                cursor = q;
            }
        }
        if (f[4]) { k1_rx_read_timing_regs(p); }
        if (f[7]) {
            rx_read_colordepth(p);
            cursor = NULL;
            while (q = NULL, gsv_walk_downstream(p, &cursor, &q) == 0) {
                if (q->kind == 5) { gsv_k1_tx_depth_limit(p, q); gsv_tx_depth_reg(q); }
                cursor = q;
            }
        }
        if (f[9]  || (p->rx->video_changed & 0x0001)) rx_propagate_packet(p, buf, 0x0001, 0, 0);
        if (f[10] || (p->rx->video_changed & 0x0008)) rx_propagate_packet(p, buf, 0x0008, 3, 0);
        if (f[11] || (p->rx->video_changed & 0x2000)) rx_propagate_packet(p, buf, 0x2000, 0xe, 0);
        if (f[12] || (p->rx->video_changed & 0x0100)) rx_propagate_packet(p, buf, 0x0100, 8, 0);
        if (f[13] || (p->rx->video_changed & 0x0200)) rx_propagate_packet(p, buf, 0x0200, 9, 0);
        if (f[14] || (p->rx->video_changed & 0x0010)) rx_propagate_packet(p, buf, 0x0010, 4, 0);
        if (f[15] || (p->rx->video_changed & 0x0020)) rx_propagate_packet(p, buf, 0x0020, 5, 0);
        if (f[16] || (p->rx->video_changed & 0x0040)) rx_propagate_packet(p, buf, 0x0040, 6, 0);
        if (f[17] || (p->rx->video_changed & 0x0080)) {
            p->rx->video_changed &= 0xff7f;
            if (p->video->flags & 0x80) { rx_read_avmute(p); rx_read_colordepth(p); }
            cursor = NULL;
            while (q = NULL, gsv_walk_downstream(p, &cursor, &q) == 0) {
                if (q->kind == 5) {
                    if ((p->video->flags & 0x80) == 0) { q->video->flags &= 0xff7f; buf[0] = 0; gsv_tx_packet_config(q, 7, buf, 1); }
                    else {
                        gsv_k1_tx_depth_limit(p, q);
                        gsv_tx_depth_reg(q);
                        q->video->avmute = p->video->avmute; gsv_log("from Intpt.GcValid-->\n"); gsv_tx_avmute(q);
                        q->video->flags |= 0x80; buf[0] = 1; gsv_tx_packet_config(q, 7, buf, 1);
                    }
                }
                cursor = q;
            }
        }
    }
    return 0;
}

/* ---- Rx audio interrupt handling (0x08011948 and callees) ------------------------------------------ */
static void rx_audio_type_detect(gsv_port *p)   /* 0x0800a048 */
{
    uint8_t t = 0;
    gsv_read_bits(gsv_rx_dev_e984(p), 0x19, 4, 2, &t);
    if (t == 1) { p->audio->type = 0; return; }
    gsv_read_bits(gsv_rx_dev_e984(p), 0x19, 8, 3, &t);
    if (t == 1) { p->audio->type = 2; return; }
    gsv_read_bits(gsv_rx_dev_e984(p), 0x19, 0x10, 4, &t);
    if (t == 1) { p->audio->type = 4; return; }
    gsv_read_bits(gsv_rx_dev_e984(p), 0x19, 0x20, 5, &t);
    p->audio->type = t == 1 ? 3 : 0x10;
}
/* 0x080094e4: audio interrupt flags into f[16] */
static int rx_audio_intpt_read(gsv_port *p, uint8_t *f)
{
    uint8_t t = 0; int rc = -3;
    memset(f, 0, 0x10);
    if (p->rx->audio_present != 0) {
        rc = 0;
        rx_audio_type_detect(p);
        gsv_read_bits(GSV_PAGE(0x03), 0x37, 1, 0, &f[1]);
        gsv_read_bits(GSV_PAGE(0x03), 0x36, 1, 0, &f[0]);
        if (f[0] == 0) {
            gsv_read_bits(GSV_PAGE(0x03), 0x36, 2, 1, &t);
            if (p->audio->mute == 1) f[0] = 0;
            else if (t == 1 && p->audio->cs_fs_ok == 0) f[0] = 1;
            else if (t == 0 && p->audio->cs_fs_ok == 1) f[0] = 1;
        }
        gsv_read_bits(GSV_PAGE(0x03), 0x37, 0x10, 4, &f[2]);
        if (f[2] == 0) gsv_read_bits(GSV_PAGE(0x03), 0x59, 0x10, 4, &f[2]);
        gsv_read_bits(GSV_PAGE(0x03), 0x55, 0x10, 4, &f[3]);
        if (f[3] == 0) { gsv_read_bits(GSV_PAGE(0x03), 0x55, 0x20, 5, &t); if (t == 1) f[3] = 1; }
        gsv_read_bits(GSV_PAGE(0x03), 0x34, 0x10, 4, &f[4]);
        if (f[4] == 0) gsv_read_bits(GSV_PAGE(0x03), 0x56, 1, 0, &f[4]);
        gsv_read_bits(GSV_PAGE(0x03), 0x56, 0x10, 4, &f[5]);
        if (f[5] == 0) { gsv_read_bits(GSV_PAGE(0x03), 0x56, 0x20, 5, &t); if (t == 1) f[5] = 1; }
        gsv_read_bits(GSV_PAGE(0x03), 0x57, 1, 0, &f[6]);
        if (f[6] == 0) { gsv_read_bits(GSV_PAGE(0x03), 0x57, 2, 1, &t); if (t == 1) f[6] = 1; }
        gsv_read_bits(GSV_PAGE(0x03), 0x57, 0x10, 4, &f[7]);
        if (f[7] == 0) { gsv_read_bits(GSV_PAGE(0x03), 0x57, 0x20, 5, &t); if (t == 1) f[7] = 1; }
        gsv_read_bits(GSV_PAGE(0x03), 0x54, 0x10, 4, &f[8]);
        if (f[8] == 0) { gsv_read_bits(GSV_PAGE(0x03), 0x54, 0x20, 5, &t); if (t == 1) f[8] = 1; }
        gsv_read_bits(GSV_PAGE(0x03), 0x5a, 0x10, 4, &f[9]);
        if (f[9] == 0) { gsv_read_bits(GSV_PAGE(0x03), 0x5a, 0x20, 5, &t); if (t == 1) f[9] = 1; }
        gsv_read_bits(GSV_PAGE(0x03), 0x58, 1, 0, &f[10]);
        if (f[10] == 0) { gsv_read_bits(GSV_PAGE(0x03), 0x58, 2, 1, &t); if (t == 1) f[10] = 1; }
        gsv_read_bits(GSV_PAGE(0x03), 0x59, 1, 0, &f[11]);
        if (f[11] == 0) { gsv_read_bits(GSV_PAGE(0x03), 0x59, 2, 1, &t); if (t == 1) f[11] = 1; }
        gsv_read_bits(GSV_PAGE(0x03), 0x58, 0x10, 4, &f[12]);
        if (f[12] == 0) { gsv_read_bits(GSV_PAGE(0x03), 0x58, 0x20, 5, &t); if (t == 1) f[12] = 1; }
        gsv_read_bits(GSV_PAGE(0x03), 0x5a, 1, 0, &f[13]);
        if (f[13] == 0) { gsv_read_bits(GSV_PAGE(0x03), 0x5a, 2, 1, &t); if (t == 1) f[13] = 1; }
        gsv_read_bits(GSV_PAGE(0x03), 0x30, 0x10, 4, &f[14]);
        if (f[14] == 0) gsv_read_bits(GSV_PAGE(0x03), 0x50, 0x10, 4, &f[14]);
        gsv_read_bits(GSV_PAGE(0x03), 0x32, 0x10, 4, &f[15]);
        if (f[15] == 0) gsv_read_bits(GSV_PAGE(0x03), 0x52, 0x10, 4, &f[15]);
        if (p->rx->locked == 0 || p->audio->mute == 1) f[2] = 0;
    }
    static const char *names[] = { "AudChanMode", "InternMute", "CsDataValid", "NChange", "CtsThresh", "AudFifoOv", "AudFifoUn", "AudioPktErr", "AudModeChg", 0, 0, 0, "AudSampChg", "AudPrtyErr", "AudIfValid", "AcpValid" };
    for (int i = 0; i < 16; i++) if (f[i] == 1 && names[i]) gsv_log("Port%d: Intpt->%s\n", p->id, names[i]);
    return rc;
}
static void rx_audio_intpt_clear(uint8_t *f)   /* 0x080090a4 */
{
    if (f[0]) gsv_write_bits(GSV_PAGE(0x03), 0x36, 4, 2, 1);
    if (f[1]) gsv_write_bits(GSV_PAGE(0x03), 0x37, 4, 2, 1);
    if (f[2]) { gsv_write_bits(GSV_PAGE(0x03), 0x37, 0x40, 6, 1); gsv_write_bits(GSV_PAGE(0x03), 0x59, 0x40, 6, 1); }
    if (f[3]) gsv_write_bits(GSV_PAGE(0x03), 0x55, 0x40, 6, 1);
    if (f[4]) { gsv_write_bits(GSV_PAGE(0x03), 0x34, 0x40, 6, 1); gsv_write_bits(GSV_PAGE(0x03), 0x56, 4, 2, 1); }
    if (f[5]) gsv_write_bits(GSV_PAGE(0x03), 0x56, 0x40, 6, 1);
    if (f[6]) gsv_write_bits(GSV_PAGE(0x03), 0x57, 4, 2, 1);
    if (f[7]) gsv_write_bits(GSV_PAGE(0x03), 0x57, 0x40, 6, 1);
    if (f[8]) gsv_write_bits(GSV_PAGE(0x03), 0x54, 0x40, 6, 1);
    if (f[9]) gsv_write_bits(GSV_PAGE(0x03), 0x5a, 0x40, 6, 1);
    if (f[10]) gsv_write_bits(GSV_PAGE(0x03), 0x58, 4, 2, 1);
    if (f[11]) gsv_write_bits(GSV_PAGE(0x03), 0x59, 4, 2, 1);
    if (f[12]) gsv_write_bits(GSV_PAGE(0x03), 0x58, 0x40, 6, 1);
    if (f[13]) gsv_write_bits(GSV_PAGE(0x03), 0x5a, 4, 2, 1);
    if (f[14]) { gsv_write_bits(GSV_PAGE(0x03), 0x30, 0x40, 6, 1); gsv_write_bits(GSV_PAGE(0x03), 0x50, 0x40, 6, 1); }
    if (f[15]) { gsv_write_bits(GSV_PAGE(0x03), 0x32, 0x40, 6, 1); gsv_write_bits(GSV_PAGE(0x03), 0x52, 0x40, 6, 1); }
}
static int rx_read_n(gsv_port *p)   /* 0x08009e38: the audio N value, when channel status is valid */
{
    uint8_t t = 0;
    if (g_chip_state[0] != p) return -3;
    gsv_read_bits(GSV_PAGE(0x03), 0x34, 0x20, 5, &t);
    if (t == 1) { gsv_read_field(gsv_rx_dev_e9a8(p), 0x80009, 0x0f, 0xff, 0, 1, 3, &p->audio->n_value); if (p->audio->n_value != 0) return 0; }
    return -1;
}
void gsv_tx_audio_n_regs(gsv_port *tx);   /* 0x0800d764, gsv_tx.c */
/* 0x08008ef0: post-process the audio flags */
static void rx_audio_post(gsv_port *p, uint8_t *f)
{
    if (f[2]) {
        uint8_t fs = p->audio->fs, b3 = 1, b4 = 0, b2 = 1, b1 = 0;
        if (fs == 1) gsv_write_bits(gsv_rx_dev_e984(p), 3, 0x80, 7, 1);
        else if (fs == 6) b1 = 1; else if (fs == 7) b4 = 1; else if (fs == 0xc) { b2 = 0; b3 = 0; }
        gsv_write_bits(gsv_rx_dev_e984(p), 1, 8, 3, b3);
        gsv_write_bits(gsv_rx_dev_e984(p), 1, 0x10, 4, b4);
        gsv_write_bits(gsv_rx_dev_e984(p), 1, 4, 2, b2);
        gsv_write_bits(gsv_rx_dev_e984(p), 1, 2, 1, b1);
    }
    if (f[9]) { gsv_write_bits(gsv_rx_dev_e984(p), 0x3f, 0x20, 5, 1); gsv_write_bits(gsv_rx_dev_e984(p), 0x3f, 0x20, 5, 0); }
    if (f[3] || f[4] || f[5] || f[6]) { gsv_write_bits(gsv_rx_dev_e9a8(p), 0x7a, 2, 1, 1); gsv_write_bits(gsv_rx_dev_e9a8(p), 0x7a, 1, 0, 1); }
}
static void rx_audio_intpt(gsv_port *p)   /* 0x08011948 */
{
    uint8_t f[0x10]; uint8_t buf[40]; bool reset_flags = false;
    gsv_port *cursor, *q;
    if (p->rx->stable == 0) return;
    int rc = rx_audio_intpt_read(p, f);
    rx_audio_intpt_clear(f);
    if (rc == 0) {
        if (f[0]) { f[14] = 1; f[4] = 1; f[2] = 1; }
        if (f[2] || f[4]) rx_read_n(p);
        if (f[9]) {
            reset_flags = true; rx_audio_type_detect(p);
            cursor = NULL;
            while (q = NULL, gsv_walk_audio_downstream(p, &cursor, &q) == 0) { q->audio->type = p->audio->type; cursor = q; }
        }
        if (f[12]) {
            reset_flags = true; rx_read_n(p);
            cursor = NULL;
            while (q = NULL, gsv_walk_audio_downstream(p, &cursor, &q) == 0) { q->audio->n_value = p->audio->n_value; cursor = q; }
        }
        if (f[15] || (p->rx->audio_changed & 4))     rx_propagate_packet(p, buf, 4, 2, 1);
        if (f[2]  || (p->rx->audio_changed & 0x400)) rx_propagate_packet(p, buf, 0x400, 10, 1);
        if (f[14] || (p->rx->audio_changed & 2))     rx_propagate_packet(p, buf, 2, 1, 1);
        if (f[3]) {
            rx_read_n(p);
            cursor = NULL;
            while (q = NULL, gsv_walk_audio_downstream(p, &cursor, &q) == 0) { q->audio->n_value = p->audio->n_value; gsv_tx_audio_n_regs(q); cursor = q; }
        }
    }
    if (reset_flags) {
        cursor = NULL;
        while (q = NULL, gsv_walk_audio_downstream(p, &cursor, &q) == 0) { q->audio->flags = 0; cursor = q; }
    }
    rx_audio_post(p, f);
}

/* ---- EDID into the Rx (0x08010924 / 0x0800bbc0 / 0x0800ba70 / 0x0800945c) ---------------------------- */
int gsv_rx_write_edid(gsv_port *p, const uint8_t *edid, int len)   /* "Rx%d Writes EDID RAM1" */
{
    gsv_log("Rx%d Writes EDID RAM1, [255]=0x%x .\n", p->id + 1, edid[0xff]);
    gsv_write_bits(GSV_PAGE(0x26), 0x7a, 3, 0, 0);
    gsv_write_bits(GSV_PAGE(0x26), 0x7a, 0x70, 4, 0);
    gsv_write_block(GSV_PAGE(0x10), 0x00, edid, 0x100);
    if (len > 0x100) { gsv_write_bits(GSV_PAGE(0x26), 0x7a, 0x70, 4, 1); gsv_write_block(GSV_PAGE(0x10), 0x00, edid + 0x100, 0x100); }
    return 0;
}
/* 0x080113a8: find the CEC physical address in an EDID; returns its offset, or 1 when absent */
static unsigned edid_find_spa(const uint8_t *edid, uint8_t *spa)
{
    spa[0] = spa[1] = 0xff;
    if (edid[0x7e] == 0) return 1;
    if (edid[0x80] != 2 || edid[0x81] != 3) return 1;
    unsigned end = edid[0x82] < 4 ? 0xff : (uint8_t)(edid[0x82] + 0x80);
    for (unsigned i = 0x84; i < 0xff; i = (i + (edid[i] & 0x1f) + 1) & 0xff) {
        if (end <= i) return 1;
        if ((edid[i] & 0xe0) == 0x60) {
            if (edid[i + 1] == 0x03 && edid[i + 2] == 0x0c && edid[i + 3] == 0x00) {
                spa[0] = edid[i + 4]; spa[1] = edid[i + 5];
                gsv_log("SPA Found =%02x,%02x\n", spa[0], spa[1]);
                return (i + 4) & 0xff;
            }
            return 1;
        }
    }
    return 1;
}
/* 0x08011582: fill the first free nibble of the physical address with this port's position */
static void edid_assign_spa(gsv_port *p, uint8_t *spa, unsigned pos)
{
    if (pos == 0) pos = ((uint8_t)p->id + 1 - (uint8_t)p->chip->first_port->id) & 0xff;
    if ((spa[1] & 0x0f) == 0) {
        if ((spa[1] & 0xf0) == 0) {
            if ((spa[0] & 0x0f) == 0) { if ((spa[0] & 0xf0) == 0) spa[0] = (uint8_t)((pos & 0x0f) << 4); else spa[0] |= (uint8_t)pos; }
            else spa[1] = (uint8_t)((pos & 0x0f) << 4);
        } else spa[1] |= (uint8_t)pos;
    }
}
static int rx_edid_enable(gsv_port *p)   /* 0x0800945c */
{
    if (p->id == 0) { gsv_write_bits(GSV_PAGE(0x26), 0x7d, 1, 0, 0); gsv_write_bits(GSV_PAGE(0x26), 0x77, 4, 2, 1); gsv_write_bits(GSV_PAGE(0x26), 0x74, 1, 0, 1); }
    else if (p->id == 1) { gsv_write_bits(GSV_PAGE(0x26), 0x7d, 2, 1, 0); gsv_write_bits(GSV_PAGE(0x26), 0x77, 4, 2, 1); gsv_write_bits(GSV_PAGE(0x26), 0x74, 2, 1, 1); }
    return 0;
}
void gsv_rx_set_edid(gsv_port *p, uint8_t *edid)   /* 0x08010924 "Rx %d Setting Edid Start" */
{
    uint8_t spa[4] = { 0 };
    gsv_log("\tRx %d Setting Edid Start: \r\n", p->id + 1);
    rx_power_down_full(p);
    edid_find_spa(edid, spa);
    edid_assign_spa(p, spa, 0);
    gsv_rx_write_edid(p, edid, 0x100);          /* 0x08011f48 */
    rx_edid_enable(p);
    p->rx->plug_sub = 5;
}
