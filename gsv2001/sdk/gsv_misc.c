/* GSV2001 SDK: scaler, colour-converter and audio ports. */
#include "gsv_sdk.h"
#include <string.h>

/* 0x080085fc: program the downscaler for the current input VIC / colour space */
/* GSV2001 (Gsv2k1 0x08012f0c): the same block (page 0x12) and the same VIC codes. Differences: the idle value of
 * page 1 reg 1 [2:0] is 5, more input VICs are accepted (4K 21:9 and some HD ones), and that field is 4 for an
 * interlaced input and 5 for one below 280 MHz. The result on the output is half the width and height at the same
 * frame rate; scaler->u2 gets the output VIC. */
static int scaler_apply(gsv_port *p)
{
    uint8_t v = 0, w = 0, vic = 0; gsv_port *src = p->source;
    if (g_chip_state[1] != p || p->downstream == NULL) {
        gsv_read_bits(GSV_PAGE(0x01), 1, 7, 0, &v);
        if (v == 2 || v == 3 || v == 4) gsv_write_bits(GSV_PAGE(0x01), 1, 7, 0, 5);
        return 0;
    }
    gsv_read_bits(GSV_PAGE(0x01), 1, 8, 3, &v);
    if (v == 0) gsv_write_bits(GSV_PAGE(0x01), 1, 8, 3, 1);
    switch (p->scaler->in_vic) {
    case 0x5d: case 0x67: vic = 0x5d; p->scaler->u2 = 0x20; break;      /* 2160p24 -> 1080p24 */
    case 0x5e: case 0x68: vic = 0x5e; p->scaler->u2 = 0x21; break;      /* 2160p25 */
    case 0x5f: case 0x69: vic = 0x5f; p->scaler->u2 = 0x22; break;      /* 2160p30 */
    case 0x60: case 0x6a: vic = 0x60; p->scaler->u2 = 0x1f; break;      /* 2160p50 -> 1080p50 */
    case 0x61: case 0x6b: vic = 0x61; p->scaler->u2 = 0x10; break;      /* 2160p60 -> 1080p60 */
    case 0x62: vic = 0x62; p->scaler->u2 = 0x20; break;                 /* 4096-wide timings */
    case 0x63: vic = 0x63; p->scaler->u2 = 0x21; break;
    case 0x64: vic = 0x64; p->scaler->u2 = 0x22; break;
    case 0x65: vic = 0x65; p->scaler->u2 = 0x1f; break;
    case 0x66: vic = 0x66; p->scaler->u2 = 0x10; break;
    case 0x05: p->scaler->u2 = 0x10; break;                             /* HD inputs: block code 0 */
    case 0x06: case 0x07: p->scaler->u2 = 0x02; break;
    case 0x14: p->scaler->u2 = 0x1f; break;
    case 0x15: case 0x16: p->scaler->u2 = 0x11; break;
    default: gsv_write_bits(GSV_PAGE(0x12), 0x66, 0xff, 0, 0); p->scaler->u2 = 0; return -1;
    }
    gsv_read_bits(GSV_PAGE(0x12), 0x66, 0xff, 0, &v);
    if (v != vic) { gsv_write_bits(GSV_PAGE(0x12), 0x66, 0xff, 0, vic); gsv_write_bits(GSV_PAGE(0x12), 0x68, 1, 0, 0); gsv_write_bits(GSV_PAGE(0x12), 0x68, 2, 1, 0); }
    gsv_read_bits(GSV_PAGE(0x01), 1, 7, 0, &v);
    uint8_t mode = p->scaler->colorspace == 3 ? 2 : 3;
    if (src && src->kind == 0 && src->rx->locked == 1) {
        if (src->video->interlaced == 1) mode = 4;
        else if (src->video->pixclk < 0x118) mode = 5;
    }
    if (v != mode) gsv_write_bits(GSV_PAGE(0x01), 1, 7, 0, mode);
    gsv_read_bits(GSV_PAGE(0x12), 0x78, 0x70, 4, &v);
    gsv_read_bits(GSV_PAGE(0x12), 0x78, 7, 0, &w);
    uint8_t cs = p->scaler->colorspace == 3 ? 4 : 0;
    if (v != cs || w != 0) { gsv_write_bits(GSV_PAGE(0x12), 0x78, 0x70, 4, cs); gsv_write_bits(GSV_PAGE(0x12), 0x78, 7, 0, 0); }
    return 0;
}


static bool near(unsigned a, unsigned b, unsigned tol) { unsigned d = a < b ? b - a : a - b; return (d & 0xffff) <= tol; }   /* 0x08014af0 */
/* 0x0800eb0c: VIC of a 4K timing from its measured geometry */
static uint8_t vic_from_timing(gsv_port *p)
{
    gsv_video *v = p->video; uint8_t vic = 0;
    if (v->pixclk <= 0x121) return 0;
    #define T(ha, ht, fr) (near(v->hactive, ha, 5) && near(v->vactive, 0x8ca, 5) && near(v->htotal, ht, 5) && near(v->vtotal, 0x870, 5) && near(v->frame_rate, fr, 3))
    if (T(0x157c, 0xf00, 0x18)) vic = 0x5d;
    if (T(0x157c, 0x1000, 0x18)) vic = 0x62;
    if (T(0x1130, 0xf00, 0x3c)) vic = 0x61;
    if (T(0x1130, 0x1000, 0x3c)) vic = 0x66;
    if (T(0xa50, 0x780, 0x32)) vic = 0x60;
    if (near(v->hactive, 0x898, 5) && near(v->vactive, 0x8ca, 5) && near(v->htotal, 0x780, 5) && near(v->vtotal, 0x870, 5) && (near(v->frame_rate, 0x3c, 3) || near(v->frame_rate, 0x78, 3))) vic = 0x61;
    if (T(0xa50, 0x800, 0x32)) vic = 0x60;
    if (near(v->hactive, 0x898, 5) && near(v->vactive, 0x8ca, 5) && near(v->htotal, 0x800, 5) && near(v->vtotal, 0x870, 5) && (near(v->frame_rate, 0x3c, 3) || near(v->frame_rate, 0x78, 3))) vic = 0x61;
    if (T(0x1130, 0xf00, 0x1e)) vic = 0x5f;
    if (T(0x1130, 0x1000, 0x1e)) vic = 0x64;
    if (T(0x14a0, 0xf00, 0x19)) vic = 0x5e;
    if (T(0x14a0, 0x1000, 0x19)) vic = 0x63;
    if (T(0x14a0, 0xf00, 0x32)) vic = 0x60;
    if (T(0x14a0, 0x1000, 0x32)) vic = 0x65;
    #undef T
    return vic;
}

extern gsv_port *g_port_rx;

void gsv_scaler_fsm_step(gsv_port *p)   /* 0x08010fa8 */
{
    gsv_port *src = p->source;
    if (src == NULL || p->scaler->enabled == 0) return;
    if (p->downstream == NULL) { p->scaler->in_vic = 0; p->scaler->colorspace = 0xf; }
    else if (src->kind == 0) {
        src->video->vic = vic_from_timing(g_port_rx);
        if (p->scaler->in_vic != src->video->vic) {
            p->scaler->dirty = 3;
            gsv_log("\t******+port->content.scaler->ScalerInVic=[%d]\r\n", p->scaler->in_vic);
            p->scaler->in_vic = src->video->vic;
            gsv_log("\t******+port->content.scaler->ScalerInVic=[%d]\r\n", p->scaler->in_vic);
        }
        if (p->scaler->colorspace != src->video->colorspace) {
            p->scaler->dirty = 3; p->scaler->colorspace = src->video->colorspace;
            gsv_log("\t******+port->content.scaler->ScalerInVic=[%d]\r\n", p->scaler->in_vic);
            gsv_log("\t******+SourcePort->content.video->timing.Vic=[%d]\r\n", src->video->vic);
            gsv_log("\t******+port->content.scaler->ColorSpace=[%d]\r\n", p->scaler->colorspace);
        }
    }
    scaler_apply(p);
}

static uint8_t csc_code(uint8_t c) { return c == 0x42 ? 0x47 : c == 0x41 ? 0x43 : c == 2 ? 7 : c == 1 ? 3 : c == 0 ? 0x40 : c; }   /* 0x0800efa0 */

/* 0x08007dfc: program the colour converter block for the current input/output */
static int color_apply(gsv_port *p)
{
    gsv_color *c = p->color; uint8_t t = 0, t2 = 0; unsigned want;
    if (g_chip_state[1] == p && p->downstream != NULL) {
        gsv_read_bits(GSV_PAGE(0x12), 0x66, 0xff, 0, &t);
        if (c->vic != t) gsv_write_bits(GSV_PAGE(0x12), 0x66, 0xff, 0, c->vic);
        gsv_read_bits(GSV_PAGE(0x01), 1, 8, 3, &t);
        want = (c->colorspace == 0 && c->u4 != 0) ? 0 : 1;
        if (t != want) gsv_write_bits(GSV_PAGE(0x01), 1, 8, 3, (uint8_t)want);
        if (want == 0) {
            uint8_t in = csc_code(c->u3), out = csc_code(c->u5);
            const uint8_t *m = gsv_csc_default; unsigned i = 0;
            for (; gsv_csc_table[i].matrix != NULL; i++) if (gsv_csc_table[i].in_cs == in && gsv_csc_table[i].out_cs == out) { m = gsv_csc_table[i].matrix; break; }
            if (gsv_csc_table[i].out_cs == 0xff) { if (t == 0) gsv_write_bits(GSV_PAGE(0x01), 1, 8, 3, 1); }
            else {
                uint8_t cur[0x18]; gsv_read_block(GSV_PAGE(0x12), 0x20, cur, 0x18);
                if (memcmp(cur, m, 0x18) != 0) { gsv_write_block(GSV_PAGE(0x12), 0x20, m, 0x18); gsv_write_bits(GSV_PAGE(0x12), 0x38, 3, 0, (uint8_t)((m[0] & 0x7f) >> 5)); }
            }
        }
        gsv_read_bits(GSV_PAGE(0x01), 1, 7, 0, &t);
        if (c->u4 == 3 && c->colorspace != 3) want = 1; else if (c->u4 == 3 || c->colorspace != 3) want = 4; else want = 0;
        if (t != want) gsv_write_bits(GSV_PAGE(0x01), 1, 7, 0, (uint8_t)want);
        gsv_read_bits(GSV_PAGE(0x12), 0x78, 0x70, 4, &t);
        gsv_read_bits(GSV_PAGE(0x12), 0x78, 7, 0, &t2);
        unsigned a = 0, b = 0;
        if (c->u4 == 3 && c->colorspace != 3) b = 3; else if (c->u4 != 3 && c->colorspace == 3) a = 4;
        if (t != a || t2 != b) { gsv_write_bits(GSV_PAGE(0x12), 0x78, 0x70, 4, (uint8_t)a); gsv_write_bits(GSV_PAGE(0x12), 0x78, 7, 0, (uint8_t)b); }
        want = ((c->u4 == 3 && c->colorspace != 3) || (c->u4 != 3 && c->colorspace == 3)) ? 1 : 0;
        for (unsigned i = 0; gsv_csc_bypass_vics[i] != 0xff; i++) if (c->vic == gsv_csc_bypass_vics[i]) { want = 0; break; }
        gsv_read_bits(GSV_PAGE(0x12), 0x68, 1, 0, &t);
        if (t != want) gsv_write_bits(GSV_PAGE(0x12), 0x68, 1, 0, (uint8_t)want);
        gsv_port *src = p->source;
        if (want == 1 && src->kind == 0 && src->idx != 0xff) {
            uint32_t s1 = 0, s2 = 0;
            struct { uint32_t rreg, wreg; uint8_t mask; int shift; } tab[] = {
                { 0x8000c, 0x80054, 0x1f, 0 }, { 0x80026, 0x80052, 0x3f, 0 }, { 0x80028, 0x80056, 0x1f, 0 }, { 0x8002a, 0x80058, 0x1f, 0 },
                { 0x8002c, 0x8005a, 0x1f, 0 }, { 0x8002e, 0x8005c, 0x3f, 1 }, { 0x8000e, 0x8005e, 0x1f, 0 }, { 0x80032, 0x80060, 0x3f, 1 },
                { 0x80036, 0x80062, 0x3f, 1 }, { 0x8003a, 0x80064, 0x3f, 2 },
            };
            for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) {
                gsv_read_field(gsv_rx_dev_e9a8(src), tab[i].rreg, tab[i].mask, 0xff, 0, 1, 2, &s1);
                if (tab[i].shift == 1) s1 >>= 1; else if (tab[i].shift == 2) { if (s1 & 1) s1++; s1 >>= 1; }
                gsv_read_field(GSV_PAGE(0x12), tab[i].wreg, 0x1f, 0xff, 0, 1, 2, &s2);
                if (s2 != s1) gsv_write_field(GSV_PAGE(0x12), tab[i].wreg, 0x1f, 0xff, 0, 1, 2, s1);
            }
        }
        return 0;
    }
    gsv_read_bits(GSV_PAGE(0x01), 1, 7, 0, &t);
    if (t == 0 || t == 1) gsv_write_bits(GSV_PAGE(0x01), 1, 7, 0, 4);
    return 0;
}

void gsv_color_fsm_step(gsv_port *p)   /* 0x08010e8a */
{
    gsv_port *src = p->source; gsv_color *c = p->color;
    if (src == NULL) return;
    if (p->downstream == NULL) { c->vic = 0; c->colorspace = 0xf; c->u4 = 0xf; c->u3 = 0xff; c->u5 = 0xff; }
    else if (src->kind == 0) {
        if (c->vic != src->video->vic) { c->dirty = 3; c->vic = src->video->vic; }
        if (c->colorspace != src->video->colorspace) { c->dirty = 3; c->colorspace = src->video->colorspace; }
        if (c->u3 != src->video->v2) { c->dirty = 3; c->u3 = src->video->v2; }
        uint8_t out = c->colorspace == 3 ? 2 : 3;
        if (c->u4 != out) { c->dirty = 3; c->u4 = out; }
        uint8_t col = c->u3;   /* decompiler: uninitialised unless the source carries a vendor frame */
        if ((src->video->flags & 0x2000) && src->video->colorspace != 0) col = c->u3;
        if (c->u5 != col) { c->dirty = 3; c->u5 = col; }
    }
    color_apply(p);
}

void gsv_audio_fsm_step(gsv_port *p)   /* 0x08010d2c -> 0x08007d84 */
{
    uint8_t cur = 0, want = 0xff;
    gsv_port *src = p->audio_source;
    if (src) {
        if (src->kind == 0) { if (src->rx->audio_present != 0) want = (src->audio->layout == 1 || src->audio->layout == 0) ? 0 : 0xef; }
        else if (src->kind == 5) want = 0xef;
    }
    if (p->audio->mute == 1) want = 0xff;
    if (p->id == 8) {
        gsv_read_bits(GSV_PAGE(0x00), 0x9d, 0xff, 0, &cur);
        if (want != cur) gsv_write_bits(GSV_PAGE(0x00), 0x9d, 0xff, 0, want);
    }
}
