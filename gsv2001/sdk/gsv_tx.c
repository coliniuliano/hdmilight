/* GSV2001 SDK: Tx port state machines (hot plug, EDID, video, HDCP). */
#include "gsv_sdk.h"
#include <string.h>

uint32_t gsv_tx_dev61(gsv_port *p) { return p->id == 4 ? GSV_PAGE(0x61) : p->id == 5 ? GSV_PAGE(0x71) : 0; }   /* 0x0800ea9c */

int gsv_tx_output_disable(gsv_port *p)   /* 0x0800c0f8 */
{
    if (p->id == 4) gsv_write_bits(GSV_PAGE(0x00), 2, 0x10, 4, 0);
    else if (p->id == 5) gsv_write_bits(GSV_PAGE(0x00), 2, 0x20, 5, 0);
    else return -1;
    return 0;
}

int gsv_tx_hdcp_cipher_disable(gsv_port *p)   /* 0x0800befc "Set Tx HDCP Cipher Disable" */
{
    p->hdcp_tx->authenticated = 0; p->hdcp_tx->ok = 0;
    gsv_write_bits(gsv_tx_dev(p), 0xb0, 0x10, 4, 0);
    gsv_write_bits(gsv_tx_dev(p), 0x97, 0x40, 6, 1);
    gsv_write_bits(gsv_tx_dev(p), 0xb0, 0x80, 7, 0);
    gsv_log("Tx Port %d: Set Tx HDCP Cipher Disable\n", p->id - 4);
    /* SDK 20210311 (0x080167f4, the same in its Gsv2k2 driver) */
    p->hdcp_tx->err_count = 0;
    gsv_write_bits(gsv_tx_dev2(p), 0x0b, 2, 1, 0);
    gsv_write_bits(gsv_tx_dev2(p), 0x28, 1, 0, 1);
    gsv_write_bits(gsv_tx_dev2(p), 0, 1, 0, 0);
    gsv_write_bits(gsv_tx_dev2(p), 0x28, 1, 0, 0);
    return 0;
}

static void tx_toggle_hpd(gsv_port *p)   /* 0x0800fbf8 */
{
    gsv_log("Port %d: Toggle Tx HPD...\n", p->id);
    gsv_write_bits(gsv_tx_dev(p), 0xef, 3, 0, 3);
    gsv_write_bits(gsv_tx_dev(p), 0xef, 0x80, 7, 0);
    gsv_write_bits(gsv_tx_dev(p), 0xef, 0x80, 7, 1);
    gsv_write_bits(gsv_tx_dev(p), 0xef, 3, 0, 0);
    gsv_log("Port %d: Toggle Tx HPD...\n", p->id);
}

void gsv_tx_hdcp_set_type(gsv_port *p, int type, int force)   /* 0x0800f984 */
{
    uint8_t en = 0, v22 = 0, cur_en = 0, cur_v22 = 0;
    if (type == 3 || type == 4) { en = 1; v22 = 0; } else if (type == 5 || type == 6) { en = 1; v22 = 1; }
    if (p->id == 4) { gsv_read_bits(GSV_PAGE(0x00), 0x95, 1, 0, &cur_v22); gsv_read_bits(GSV_PAGE(0x00), 0x94, 1, 0, &cur_en); }
    else if (p->id == 5) { gsv_read_bits(GSV_PAGE(0x00), 0x95, 2, 1, &cur_v22); gsv_read_bits(GSV_PAGE(0x00), 0x94, 2, 1, &cur_en); }
    if (cur_v22 != v22 || cur_en != en || force == 1) {
        gsv_write_bits(gsv_tx_dev(p), 0xb0, 0x10, 4, 0);
        gsv_write_bits(gsv_tx_dev2(p), 0x0b, 2, 1, 0);
        if (p->id == 4) {
            gsv_write_bits(GSV_PAGE(0x00), 0x93, 1, 0, 0); gsv_write_bits(GSV_PAGE(0x00), 0x95, 1, 0, v22);
            gsv_write_bits(GSV_PAGE(0x00), 0x94, 1, 0, en); gsv_write_bits(GSV_PAGE(0x00), 0x93, 1, 0, 1);
        } else if (p->id == 5) {
            gsv_write_bits(GSV_PAGE(0x00), 0x93, 2, 1, 0); gsv_write_bits(GSV_PAGE(0x00), 0x95, 2, 1, v22);
            gsv_write_bits(GSV_PAGE(0x00), 0x94, 2, 1, en); gsv_write_bits(GSV_PAGE(0x00), 0x93, 2, 1, 1);
        }
        tx_toggle_hpd(p);
    }
    gsv_write_bits(gsv_tx_dev(p), 0xb0, 0x10, 4, type != 0);
    gsv_write_bits(gsv_tx_dev2(p), 0x0b, 2, 1, type != 0);
}

static int tx_clear_rxid_ready(gsv_port *p)   /* 0x0800bea8 */
{
    gsv_write_bits(gsv_tx_dev2(p), 0x5d, 2, 1, 1);
    gsv_write_bits(gsv_tx_dev2(p), 0x5d, 1, 0, 1);
    gsv_log("Clear Tx RxID Ready\n");
    return 0;
}

/* ---- GSV2001 (Gsv2k1 driver, SDK 20210311) ---- */
/* Lowest of the driver's three output-driver settings (below 251 MHz, 251 to 340, above) an output may use: [0] output
 * A, [1] output B. 0 = as the driver chooses by clock. gsv_tx_plug_fsm says why a board would raise it. */
uint8_t gsv_k1_tx_drive_floor[2];
static int k1_tx_active_count(gsv_port *p)   /* 0x08019dd4: Tx ports of this chip that are past the EDID stage */
{
    int n = 0;
    for (gsv_port *q = p->chip->first_port; q && q->chip == p->chip; q = q->next)
        if (q->kind == 5 && q->idx == 0 && q->status->s6 > 5) n++;
    return n;
}
void gsv_k1_tx_hdmi_mode(gsv_port *p)        /* 0x080187f8: DVI output only when neither output has an HDMI sink */
{
    int n = 0;
    for (gsv_port *q = p->chip->first_port; q && q->chip == p->chip; q = q->next) {
        if (q->kind != 5) continue;
        if (q->idx == 0 && q->status->plug == 5) { if (q->status->hdmi_mode == 0) n++; } else n++;
    }
    gsv_write_bits(GSV_PAGE(0x40), 0xa5, 0x80, 7, 1);
    gsv_write_bits(GSV_PAGE(0x40), 0xa5, 0x40, 6, n == 2 ? 0 : 1);
}
static void k1_tx_caps(gsv_port *p)          /* 0x08018718 */
{
    uint32_t td = gsv_tx_dev(p);
    gsv_write_bits(td, 1, 1, 0, 0);
    gsv_write_bits(td, 2, 1, 0, (p->status->u10 & 1) != 0);
    gsv_write_bits(td, 1, 2, 1, 1);
    gsv_write_bits(td, 2, 2, 1, 0);
}
int gsv_tx_reset_core(gsv_port *p)     /* 0x080168e8 */
{
    uint32_t rd = GSV_PAGE(0x40), td = gsv_tx_dev(p);
    gsv_tx_hdcp_cipher_disable(p);
    gsv_tx_hdcp_set_type(p, p->hdcp_tx->type, 0);
    gsv_tx_set_tmds(p, 0);
    k1_tx_caps(p);
    if (p->idx == 0 && k1_tx_active_count(p) == 0) {
        gsv_k1_tx_hdmi_mode(p);
        gsv_write_bits(rd, 0x53, 0xff, 0, 4);
        gsv_write_bits(rd, 0x23, 0xff, 0, 0x80);
        gsv_write_bits(rd, 0x20, 0xff, 0, 0);
        gsv_write_bits(rd, 0x21, 0xff, 0, 0);
        gsv_write_bits(rd, 0x00, 0xff, 0, 0);
        gsv_write_bits(rd, 0x50, 0xff, 0, 0);
        gsv_write_bits(rd, 0x51, 0xff, 0, 0);
        gsv_write_bits(rd, 0x2b, 0xff, 0, 0);
        gsv_write_bits(rd, 0x1a, 0xff, 0, 0xb0);
        gsv_write_field(GSV_PAGE(0x42), 0x800fb, 0x7f, 0xff, 0, 1, 2, 0);
    }
    gsv_write_bits(gsv_tx_dev2(p), 0x0b, 0x40, 6, 1);
    gsv_write_bits(td, 2, 0xff, 0, 0);
    gsv_write_bits(td, 3, 0xff, 0, 0);
    gsv_write_bits(gsv_tx_dev2(p), 8, 1, 0, 1);
    gsv_write_bits(gsv_tx_dev2(p), 0x28, 1, 0, 1);
    gsv_write_field(gsv_tx_dev2(p), 0x80006, 0xff, 0xff, 0, 1, 2, 0x82);
    tx_clear_rxid_ready(p);
    /* the original also reloads the CEC registers here when CEC is enabled for the port; CEC is not in this library */
    p->video->v2 = 0x80; p->video->v3 = 0x80; p->video->colorspace = 0; p->video->flags = 0;
    p->audio->flags = 0;
    return 0;
}


void gsv_tx_update_mute_regs(gsv_port *p)   /* 0x0800d968 */
{
    uint8_t v2 = p->video->v2;
    bool plain = v2 == 0x80 || v2 == 0 || v2 == 0x53 || v2 == 0x40 || v2 == 0x13 || v2 == 0xff;
    /* Gsv2k1 0x0801867c: the Tx core is shared; its mute bit is left alone while both outputs run */
    if (p->idx != 0) return;
    gsv_write_bits(GSV_PAGE(0x40), 0x20, 8, 3, !plain);
    if (k1_tx_active_count(p) != 2) gsv_write_bits(GSV_PAGE(0x40), 0x4e, 8, 3, p->video->mute);
    return;
}

int gsv_tx_avmute(gsv_port *p)   /* 0x0800d8a0 "AVMUTE Enable/Disable" */
{
    if (p->id != 4 && p->id != 5) return 0;
    if (p->video->avmute == 1) {
        gsv_write_bits(gsv_rx_dev(p), 0x52, 0x80, 7, 0); gsv_write_bits(gsv_rx_dev(p), 0x52, 0x40, 6, 1);
        gsv_log("Port %d: AVMUTE Enable...\n", p->id);
    } else {
        gsv_write_bits(gsv_rx_dev(p), 0x52, 0x40, 6, 0); gsv_write_bits(gsv_rx_dev(p), 0x52, 0x80, 7, 1);
        gsv_log("Port %d: AVMUTE Disable...\n", p->id);
    }
    return 0;
}

static void tx_set_mute_av(gsv_port *p)   /* 0x08012328 "from Kfun TX SetMuteAv" */
{
    if (p->status->hdmi_mode == 0) return;
    p->video->avmute = 1;
    gsv_log("from Kfun TX SetMuteAv-->\n");
    gsv_tx_avmute(p);
    p->video->mute = 1;
    gsv_tx_update_mute_regs(p);
}

static int tx_clear_hdcp_error(gsv_port *p)   /* 0x0800bdf4 */
{
    if (p->hdcp_tx->capable22 != 0) {
        gsv_write_bits(gsv_tx_dev2(p), 0x28, 1, 0, 1);
        gsv_write_bits(gsv_tx_dev2(p), 0x0b, 0x80, 7, 1);
        gsv_write_bits(gsv_tx_dev2(p), 0x0b, 0x80, 7, 0);
        gsv_write_field(gsv_tx_dev2(p), 0x80006, 0xff, 0xff, 0, 1, 2, 0x14a);   /* SDK 20210311 (0x080166d4) */
        gsv_write_bits(gsv_tx_dev2(p), 0x28, 1, 0, 0);
    }
    gsv_write_bits(gsv_tx_dev(p), 0x97, 0x80, 7, 1);
    gsv_log("Port%d: Clear Tx Hdcp Error%d\n", p->id, p->hdcp_tx->err_count);
    return 0;
}

static void tx_status_reset(gsv_port *p)   /* 0x080118e4 */
{
    gsv_hdcp_tx *h = p->hdcp_tx; gsv_tx_status *s = p->status;
    h->step = 0; h->err_count = 0; h->u6 = 0; h->ok = 0; h->enabled = 0; h->capable22 = 0; h->authenticated = 0; h->retry = 0;
    s->tmds_locked = 0; s->u4 = 0; s->u1 = 0; s->u3 = 0; s->s6 = 0; s->edid_retry = 0; s->plug = 0; s->s9 = 0; s->hdcp_mode = 0; s->s15 = 0;
    p->video->flags = 0; p->audio->flags = 0;
    gsv_event(p, 0xb);
}
static void tx_reset_enter(gsv_port *p)   /* 0x080118ba */
{
    gsv_tx_status *s = p->status;
    s->u4 = 1; s->edid_retry = 0; s->plug = 0; s->hdcp_mode = 0; s->s6 = 0;
    gsv_rx_reset_port(p);
    p->hdcp_tx->running22 = 0;
}
static void tx_video_unlocked_enter(gsv_port *p)   /* 0x08012384 */
{
    p->video->flags = 0; p->audio->flags = 0; p->status->s6 = 0;
    gsv_tx_reset_core(p);
    gsv_tx_output_disable(p);
}
static void tx_go_reset(gsv_port *p)
{
    *p->tx_state = TXPLUG_RESET; tx_status_reset(p); tx_reset_enter(p); tx_set_mute_av(p);
}

/* SDK 20210311 (0x08017110, Gsv2k1 driver): as below, plus a DDC error check (page 0x62 reg 0x13), a retry limit of
 * five, and the output switched off when the sink cannot do HDCP at all. */
int gsv_tx_hdcp_poll(gsv_port *p)
{
    gsv_hdcp_tx *h = p->hdcp_tx;
    uint8_t cipher_on = 0, auth_on = 0, hdcp_type = 0, err = 0, st_d = 0, st_e = 0, st_14 = 0, ddc = 0;
    bool fail = false;
    if (h->step == 1) {
        gsv_write_bits(gsv_tx_dev(p), 0xb0, 0x10, 4, 0);
        gsv_write_bits(gsv_tx_dev2(p), 0x0b, 2, 1, 0);
        h->step++;
    } else if (h->step == 2) { gsv_tx_hdcp_set_type(p, h->type, 1); h->step = 0; }
    if (h->enabled == 0 && h->capable22 == 0) return 0;
    gsv_read_bits(gsv_tx_dev(p), 0xb0, 0x80, 7, &cipher_on);
    gsv_read_bits(gsv_tx_dev2(p), 0, 1, 0, &auth_on);
    h->authenticated = !(cipher_on == 0 && auth_on == 0);
    gsv_read_bits(gsv_tx_dev(p), 200, 0x0f, 0, &hdcp_type);
    gsv_read_bits(gsv_tx_dev(p), 0xc5, 0x80, 7, &err);
    gsv_read_bits(gsv_tx_dev(p), 199, 0x80, 7, &h->u6);
    if (hdcp_type == 4 && h->ok == 0) gsv_log("Port Tx %d: Tx is Running HDCP 1.4\n", p->id - 4);
    gsv_read_bits(gsv_tx_dev2(p), 0x0d, 0xff, 0, &st_d);
    gsv_read_bits(gsv_tx_dev2(p), 0x0e, 0xff, 0, &st_e);
    gsv_read_bits(gsv_tx_dev2(p), 0x14, 0xff, 0, &st_14);
    if (st_14 == 0) {
        gsv_read_bits(gsv_tx_dev2(p), 0x13, 0xff, 0, &ddc);
        if (ddc == 0x20) { fail = true; gsv_log("Port%d: Unexpected DDC Error\n", p->id); }
    }
    if (hdcp_type == 6 && auth_on == 1 && st_14 == 2) fail = true;
    else if (st_d != 0 || st_e != 0) fail = true;
    if ((st_14 == 0x80 && h->running22 == 0) || (st_14 != 0x80 && h->running22 == 1)) {
        h->running22 = st_14 >> 7;
        gsv_log(h->running22 == 1 ? "Port Tx %d: Tx is Running HDCP 2.2\n" : "Port Tx %d: Tx is Stopping HDCP 2.2\n", p->id - 4);
    }
    if (hdcp_type == 6) h->capable22 = 1;
    h->ok = ((hdcp_type == 6 && st_14 == 0x80) || hdcp_type == 4) ? 1 : 0;
    if (err != 0 || fail) {
        h->err_count++;
        tx_clear_hdcp_error(p);
        if ((p->status->hdcp_mode == 0 && h->authenticated == 1) || p->status->tmds_locked == 0) gsv_tx_hdcp_cipher_disable(p);
    } else if (p->video->mute == 1) {
        if ((st_14 & 0x81) == 0) fail = true;
        if (hdcp_type == 3 || hdcp_type == 5) err = 1;
    }
    switch (h->type) {
    case 0: if (cipher_on == 1 || auth_on == 1) gsv_tx_hdcp_cipher_disable(p); break;
    case 1:
        p->video->mute = (uint8_t)(err | fail); p->audio->mute = (uint8_t)(err | fail);
        if ((h->err_count > 4 && p->idx == 0xff) || h->enabled == 0) gsv_tx_set_tmds(p, 0);
        break;
    case 2:
        if (h->err_count == 2 && (err != 0 || fail)) { gsv_tx_hdcp_set_type(p, 4, 1); gsv_log("Port Tx %d: Tx HDCP 2.2 DownGrade to 1.4\n", p->id - 4); }
        else if (h->err_count > 4 && (cipher_on == 1 || auth_on == 1)) { gsv_tx_hdcp_cipher_disable(p); h->err_count = 5; }
        break;
    case 3:
        p->video->mute = err; p->audio->mute = err;
        if ((h->err_count > 4 && p->idx == 0xff) || h->enabled == 0) gsv_tx_set_tmds(p, 0);
        break;
    case 4: case 6: if (h->err_count > 4 && (cipher_on == 1 || auth_on == 1)) { gsv_tx_hdcp_cipher_disable(p); h->err_count = 5; } break;
    case 5: p->video->mute = fail; p->audio->mute = fail; break;
    default: break;
    }
    return 0;
}

/* 0x0800c760 "Tx is Running HDCP": poll the Tx HDCP engine */


/* SDK 20210311 (Gsv2k1 0x08016e88): the output's HDCP follows its own type setting (no application mode, no sink
 * re-check), with a retry limit of five. */
int gsv_tx_hdcp_cipher(gsv_port *p)
{
    gsv_hdcp_tx *h = p->hdcp_tx; uint8_t t = 0; unsigned en14 = 0, en22 = 0; bool allow = false;
    gsv_read_bits(gsv_tx_dev(p), 0xf2, 0x80, 7, &t);
    if (t == 0 || p->status->s9 == 1) return -3;
    if (p->status->hdmi_mode == 0 && h->enabled == 1) {
        if (h->type == 1 || h->type == 2) allow = h->u3 == 1; else if (h->type == 3 || h->type == 4) allow = true;
    }
    if (p->status->hdmi_mode == 1 || allow) {
        switch (h->type) {
        case 1: en14 = 1; en22 = 1; break;
        case 2: en14 = en22 = h->err_count < 5; break;
        case 3: en14 = 1; break;
        case 4: en14 = h->err_count < 5; break;
        case 5: en22 = 1; break;
        case 6: if (h->err_count < 5) en22 = 1; break;
        default: break;
        }
        if (h->step != 0) { en14 = 0; en22 = 0; }
        gsv_read_bits(gsv_tx_dev(p), 0xb0, 0x80, 7, &t);
        if (t != en14) {
            gsv_write_bits(gsv_tx_dev(p), 0x97, 0x80, 7, 1);
            gsv_write_bits(gsv_tx_dev(p), 0xb0, 0x80, 7, (uint8_t)en14);
            gsv_write_bits(gsv_tx_dev(p), 0xb0, 0x10, 4, (uint8_t)en14);
            if (en14) gsv_log("Tx Port %d: Set Tx HDCP 1.4 Cipher Enable\n", p->id - 4);
        }
        gsv_read_bits(gsv_tx_dev2(p), 0, 1, 0, &t);
        if (t != en22) {
            gsv_write_bits(gsv_tx_dev2(p), 0x0b, 0x80, 7, 1);
            gsv_write_bits(gsv_tx_dev2(p), 0x0b, 0x80, 7, 0);
            gsv_write_bits(gsv_tx_dev2(p), 0, 1, 0, (uint8_t)en22);
            gsv_write_bits(gsv_tx_dev2(p), 0x0b, 2, 1, (uint8_t)en22);
            if (en22) gsv_log("Tx Port %d: Set Tx HDCP 2.2 Cipher Enable\n", p->id - 4);
        }
    } else {
        uint8_t mute = h->u3 == 0 ? 1 : 0;
        if (p->video->mute != mute) { p->video->mute = mute; gsv_tx_update_mute_regs(p); }
    }
    return 0;
}


/* ---- Tx video / packet programming --------------------------------------------------------------------- */
void gsv_tx_depth_reg(gsv_port *p)   /* 0x0800da64 */
{
    uint8_t v = 0;
    gsv_port *src = p->source;
    if (src) {
        if (src->kind == 8 || src->kind == 9) v = 4;
        else if (p->video->depth_code == 0x1e) v = 5; else if (p->video->depth_code == 0x24) v = 6; else v = 4;
    }
    if (p->status->hdmi_mode == 0) v = 4;
    if (p->idx == 0) gsv_write_bits(GSV_PAGE(0x40), 0x53, 0x0f, 0, v);
}

void gsv_tx_audio_n_regs(gsv_port *p)   /* 0x0800d764 */
{
    /* Gsv2k1 0x080183ec: only for an output on the core; then the packet memory is re-latched */
    uint8_t v = 0;
    if (p->kind != 5 || p->idx != 0) return;
    gsv_write_bits(GSV_PAGE(0x40), 5, 0xff, 0, (uint8_t)p->audio->n_value);
    gsv_write_bits(GSV_PAGE(0x40), 6, 0xff, 0, (uint8_t)(p->audio->n_value >> 8));
    gsv_write_bits(GSV_PAGE(0x40), 7, 0x0f, 0, 0);
    gsv_read_bits(GSV_PAGE(0x42), 100, 0xff, 0, &v);
    gsv_write_bits(GSV_PAGE(0x42), 100, 0xff, 0, (uint8_t)(v + 1));
    gsv_write_bits(GSV_PAGE(0x42), 100, 0xff, 0, v);
    return;
}

/* 0x08010394: audio sample-rate register from the fs index */
static void tx_audio_fs_reg(gsv_port *p)
{
    uint8_t fs = p->audio->fs_index; unsigned i = 0;
    while (gsv_audio_fs_table[i] != fs && gsv_audio_fs_table[i] != 0xff) i += 2;
    if (gsv_audio_fs_table[i] != 0xff) gsv_write_bits(GSV_PAGE(0x40), 0x18, 0x0f, 0, fs == 8 ? 9 : gsv_audio_fs_table[i + 1]);
}
/* 0x080103f4: audio path configuration by type */
static void tx_audio_config(gsv_port *p)
{
    gsv_audio *a = p->audio; unsigned f0 = 0, f1 = 0, f2 = 0, lay = 8, ch = 3, div = 2;
    const uint32_t core = GSV_PAGE(0x40);     /* Gsv2k1 0x0801a558: the one Tx core, whatever the output's index */
    gsv_write_bits(core, 0, 6, 1, a->u0a);
    switch (a->type) {
    case 3: f0 = 0xf; gsv_write_bits(core, 2, 3, 0, 0); gsv_write_bits(core, 0x10, 0x40, 6, 0); gsv_write_bits(core, 0x10, 0x40, 6, 1); break;
    case 0: case 0x10: f0 = 0xf; break;
    case 1: f1 = 1; break;
    case 2:
        lay = a->channels; ch = a->cs_fs_ok == 1 ? 3 : 1; div = lay / (ch + 1);
        f2 = lay == 2 ? 3 : lay == 4 ? 0xf : lay == 6 ? 0x3f : 0xff;
        break;
    case 4: f2 = 0; break;
    default: break;
    }
    gsv_write_bits(core, 2, 0x3c, 2, (uint8_t)f0);
    gsv_write_bits(core, 1, 0x80, 7, (uint8_t)f1);
    gsv_write_bits(core, 0xb0, 0x0c, 2, (uint8_t)ch);
    gsv_write_bits(core, 0xb3, 0x3f, 0, (uint8_t)div);
    gsv_write_bits(core, 0xb4, 0x3f, 0, (uint8_t)lay);
    gsv_write_field(core, 0x800da, 0xff, 0xff, 0, 1, 4, f2);
    gsv_write_bits(core, 0, 0x18, 3, 0);

}
/* 0x0800d7f6: (re)start the Tx audio path */
static int tx_audio_start(gsv_port *p)
{
    if (p->status->hdcp_mode == 1 && p->status->tmds_locked != 0 && p->status->u4 != 0) {
        if (p->id == 4 || p->id == 5) {
            /* Gsv2k1 0x080184c4 */
            if (p->audio_source) gsv_write_bits(GSV_PAGE(0x40), 1, 0x40, 6, p->audio_source->id == 0 ? 0 : 1);
            gsv_write_bits(GSV_PAGE(0x40), 0, 0xe0, 5, p->audio->type == 0xff ? 0 : p->audio->type);
            tx_audio_config(p); tx_audio_fs_reg(p); gsv_tx_audio_n_regs(p);
            gsv_tx_int_config(p, 0x1800, 1);
            return 0;
        }
        return -2;
    }
    p->audio->flags &= 0xe7fd;
    gsv_tx_int_config(p, 0x1802, 0);
    return -2;
}
/* 0x0800bc5c: audio FIFO recovery */
static void tx_audio_recover(gsv_port *p)
{
    uint8_t t = 0, v = 0;
    gsv_port *src = p->audio_source;
    /* Gsv2k1 0x080163dc: only for an output on the core, and only for multi-channel audio */
    if (p->idx != 0) return;
    if (src && src->kind == 0 && src->rx->stable == 1 && src->audio->type != p->audio->type) {
        p->audio->fs_index = src->audio->fs_index; p->audio->type = src->audio->type; p->audio->n_value = src->audio->n_value;
        tx_audio_start(p);
    }
    if (p->audio->layout != 1 && !(p->status->u10 & 0x10000000)) return;
    if (p->kind == 5) gsv_read_bits(GSV_PAGE(0x03), 0xe0, 2, 1, &t);
    if (t == 1) {
        gsv_read_bits(GSV_PAGE(0x42), 100, 0xff, 0, &v);
        gsv_write_bits(GSV_PAGE(0x42), 100, 0xff, 0, (uint8_t)(v + 1));
        gsv_write_bits(GSV_PAGE(0x42), 100, 0xff, 0, v);
        gsv_log("Port%d: Audio Recover From Fifo Full\n", p->id);
        if (p->kind == 5) gsv_write_bits(GSV_PAGE(0x03), 0xe0, 4, 2, 1);
    }
    return;
}

static uint8_t csc_code(uint8_t c)   /* 0x0800efa0 */
{
    return c == 0x42 ? 0x47 : c == 0x41 ? 0x43 : c == 2 ? 7 : c == 1 ? 3 : c == 0 ? 0x40 : c;
}
/* 0x080105bc: Tx colour-space-conversion matrix for in -> out colorimetry */
static int tx_csc_matrix(gsv_port *p)
{
    uint8_t in = csc_code(p->video->v2), out = csc_code(p->video->v3);
    if (in == out || in == 0xff || out == 0xff || p->video->colorspace == 3) { gsv_write_bits(gsv_rx_dev(p), 0x2b, 0x80, 7, 0); return 0; }
    for (unsigned i = 0; gsv_csc_table[i].matrix != NULL; i++) {
        if (gsv_csc_table[i].in_cs == in && gsv_csc_table[i].out_cs == out) {
            gsv_write_block(gsv_rx_dev(p), 0x2c, gsv_csc_table[i].matrix, 0x18);
            gsv_write_bits(gsv_rx_dev(p), 0x2b, 0xff, 0, (uint8_t)(gsv_csc_table[i].matrix[0] & 0xe0));
            return 0;
        }
    }
    return 0;
}

int gsv_tx_set_colordepth(gsv_port *p)   /* 0x08010688 */
{
    uint8_t d = p->video->colordepth;
    if (p->video->colordepth_in < 5 && d < 4) {
        gsv_write_bits(gsv_rx_dev(p), 0x23, 0x30, 4, d);
        gsv_write_bits(gsv_rx_dev(p), 0x23, 0x0c, 2, d);
        gsv_write_bits(gsv_rx_dev(p), 0x23, 0xc0, 6, 2);
        return 0;
    }
    return -2;
}

/* 0x0800ff44 "Y1Y0": program the AVI infoframe and the Tx colour pipeline from the port's video content */
int gsv_tx_set_colorspace(gsv_port *p, const uint8_t *avi_in, int log)
{
    if (p->id != 4 && p->id != 5) return 0;
    uint8_t avi[0x20]; memcpy(avi, avi_in, gsv_pkt_len[0]);
    gsv_video *v = p->video; uint8_t out_cs, in_cs = 0;
    static const uint8_t ycc_codes[] = { 0x41, 0x42, 0x43, 0x47, 0x4b, 0x4f, 0x57, 0x5b, 1, 2, 3, 7, 0xb, 0xf, 0x1b, 0x17 };
    #define IS_YCC(c) (memchr(ycc_codes, (c), sizeof ycc_codes) != NULL)
    if (p->status->hdmi_mode == 0) {
        in_cs = v->colorspace; out_cs = 0;
        v->v3 = (v->v2 == 0x40 || v->v2 == 0 || v->v2 == 0x80 || v->v2 == 0xff) ? v->v2 : 0x40;
    } else {
        if (IS_YCC(v->v2)) in_cs = 2;
        out_cs = v->colorspace;
        if (v->colorspace != 3) { if (IS_YCC(v->v3)) { if (v->colorspace == 0) out_cs = 2; } else out_cs = 0; }
        if (out_cs == 0) { avi[8] &= 0x3f; avi[6] = (uint8_t)((avi[6] & 0xf3) | ((v->v3 >> 6) << 2)); }
        else { avi[6] &= 0xf3; avi[8] = (uint8_t)((avi[8] & 0x3f) | ((v->v3 >> 6) << 6)); }
    }
    #undef IS_YCC
    gsv_write_bits(gsv_rx_dev(p), 0x20, 8, 3, in_cs != 0);
    if (p->source->kind == 8 && (avi[6] & 3) == 1) avi[4] &= 0xfc;
    tx_csc_matrix(p);
    if (v->vic < 0x6c) v->aspect = gsv_vic_aspect[v->vic];
    if (v->aspect == 1) { avi[5] = (uint8_t)((avi[5] & 0xcf) | 0x10); gsv_write_bits(gsv_rx_dev(p), 0x22, 2, 1, 0); }
    else if (v->aspect == 2) { avi[5] = (uint8_t)((avi[5] & 0xcf) | 0x20); gsv_write_bits(gsv_rx_dev(p), 0x22, 2, 1, 1); }
    else { avi[5] &= 0xcf; gsv_write_bits(gsv_rx_dev(p), 0x22, 2, 1, 0); }
    gsv_tx_set_colordepth(p);
    gsv_write_bits(gsv_rx_dev(p), 0x20, 0xf0, 4, (p->source->kind == 0 && p->source->video->colorspace == 1) ? 1 : 0);
    uint8_t ycc_sel;
    switch (out_cs) {
    case 0: avi[4] &= 0x1f; ycc_sel = 0; break;
    case 1: avi[4] = (uint8_t)((avi[4] & 0x1f) | 0x20); ycc_sel = 3; break;
    case 2: avi[4] = (uint8_t)((avi[4] & 0x1f) | 0x40); ycc_sel = 1; break;
    case 3: avi[4] = (uint8_t)((avi[4] & 0x1f) | 0x60); ycc_sel = 1; break;
    default: avi[4] = (uint8_t)((avi[4] & 0x1f) | 0x60); ycc_sel = 0; break;
    }
    gsv_write_bits(gsv_rx_dev(p), 0x21, 0xc0, 6, ycc_sel);
    gsv_write_bits(gsv_tx_dev42(p), 0x1f, 0xff, 0, 0x82);
    gsv_write_bits(gsv_tx_dev42(p), 0x20, 0xff, 0, avi[1]);
    gsv_write_bits(gsv_tx_dev42(p), 0x21, 0xff, 0, avi[2]);
    gsv_tx_int_config(p, 1, 2);
    gsv_write_block(gsv_tx_dev42(p), gsv_pkt_reg[0], avi + 3, (uint16_t)(gsv_pkt_len[0] - 3));
    gsv_tx_int_config(p, 1, log);
    if (log) gsv_log("Port%d: Y1Y0 = %d\n", p->id, v->colorspace);
    return 0;
}

extern uint8_t g_cfg_scaler_txa, g_cfg_scaler_txb;   /* app DIP flags, gsv_app.c */

/* 0x0800db38: load a packet of `type` into the Tx packet RAM and enable/disable it */
int gsv_tx_packet_config(gsv_port *p, int type, const uint8_t *data, int enable)
{
    if (p->id != 4 && p->id != 5) return 0;
    static const uint16_t bits[] = { 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0, 0, 0, 0, 0x2000, 0x4000 };
    unsigned bit = type < 16 ? bits[type] : 0;
    switch (type) {
    case 0: gsv_tx_set_colorspace(p, data, enable); break;
    case 1: case 2: case 3: case 4: case 5: case 6: case 8: case 9:
        gsv_tx_int_config(p, bit, 2);
        gsv_write_block(gsv_tx_dev42(p), gsv_pkt_reg[type], data, gsv_pkt_len[type]);
        gsv_tx_int_config(p, bit, enable);
        break;
    case 7: gsv_tx_int_config(p, 0x80, p->status->hdmi_mode == 0 ? 0 : enable); break;
    case 10: {
        gsv_audio *a = p->audio;
        gsv_write_bits(gsv_rx_dev(p), 2, 0x40, 6, 1);
        gsv_write_bits(gsv_rx_dev(p), 0x15, 3, 0, a->ca);
        gsv_write_bits(gsv_rx_dev(p), 0x15, 4, 2, a->lsv_dm);
        gsv_write_bits(gsv_rx_dev(p), 0x15, 0x38, 3, a->lfep);
        gsv_write_bits(gsv_rx_dev(p), 0x18, 0x30, 4, a->cs_wl);
        gsv_write_bits(gsv_rx_dev(p), 0x16, 0xff, 0, a->cs_byte1);
        gsv_write_bits(gsv_rx_dev(p), 0x17, 0x0f, 0, a->cs_fs);
        gsv_write_bits(gsv_rx_dev(p), 0x19, 0x0f, 0, a->cs_byte4);
        gsv_write_bits(gsv_rx_dev(p), 0x1a, 0x40, 6, a->cs_fs_ok);
        tx_audio_fs_reg(p);
        gsv_write_bits(gsv_rx_dev(p), 0x1a, 0x20, 5, 0);
        break; }
    case 0xe:
        if ((p->id == 4 && g_cfg_scaler_txa == 0) || (p->id == 5 && g_cfg_scaler_txb == 0)) {
            gsv_tx_int_config(p, bit, 2);
            gsv_write_block(gsv_rx_dev(p), gsv_pkt_reg[type], data, gsv_pkt_len[type]);
            gsv_tx_int_config(p, bit, enable);
        }
        break;
    case 0xf:
        gsv_tx_int_config(p, bit, 2);
        gsv_write_block(gsv_rx_dev(p), gsv_pkt_reg[type], data, gsv_pkt_len[type]);
        gsv_tx_int_config(p, bit, enable);
        break;
    default: return -4;
    }
    return 0;
}

/* 0x08011120: when the Tx is fed through the scaler/colour ports, let them override VIC / colour space */
static void tx_apply_scaler_color(gsv_port *tx, int type, uint8_t *pkt)
{
    bool vic_done = false;
    for (gsv_port *q = tx->source; q && (q->kind == 8 || q->kind == 9); q = q->source) {
        if (type != 0) continue;
        if (q->kind == 8) {
            gsv_scaler_fsm_step(q);
            if (!vic_done) {
                if (q->scaler->u2 != 0) { pkt[7] = q->scaler->u2; tx->video->vic = q->scaler->u2; gsv_log("\t******SourcePort->content.scaler->ScalerOutVic = [%d]\r\n", q->scaler->u2); vic_done = true; }
                if (q->scaler->colorspace == 3) { tx->video->colorspace = 2; pkt[4] = (uint8_t)((pkt[4] & 0x1f) | 0x40); }
            }
        } else {
            gsv_color_fsm_step(q);
            if (!vic_done && q->color->vic != 0) { pkt[7] = q->color->vic; tx->video->vic = q->color->vic; gsv_log("\t******SourcePort->content.color->ColorInVic = [%d]\r\n", q->color->vic); vic_done = true; }
            pkt[4] = (uint8_t)((pkt[4] & 0x1f) | (q->color->u4 << 5));
            pkt[5] = (uint8_t)((pkt[5] & 0x3f) | (q->color->u5 << 6));
            pkt[6] = (uint8_t)((pkt[6] & 0x8f) | (((q->color->u5 & 0x1f) >> 2) << 4));
            pkt[6] = (uint8_t)((pkt[6] & 0xf3) | ((q->color->u5 >> 6) << 2));
            tx->video->colorspace = q->color->u4; tx->video->v2 = q->color->u5;
        }
    }
}

/* 0x08012686: carry a packet (type/bit) from the source port to a Tx */
void gsv_tx_apply_from_source(gsv_port *src, gsv_port *tx, uint8_t *buf, unsigned bit, int type, int audio, int force)
{
    uint16_t *sflags = audio ? &src->audio->flags : &src->video->flags;
    uint16_t *tflags = audio ? &tx->audio->flags : &tx->video->flags;
    if ((*sflags & bit) == 0 && force != 1) { gsv_tx_int_config(tx, bit, 0); *tflags &= (uint16_t)~bit; return; }
    if (force == 0 || force == 1) { gsv_rx_read_packet(src, type, buf); gsv_parse_packet(src, type, buf); }
    tx_apply_scaler_color(tx, type, buf);
    gsv_parse_packet(tx, type, buf);
    gsv_tx_packet_config(tx, type, buf, 1);
    *tflags |= (uint16_t)bit;
}

/* 0x0800de7c: Tx colour pipeline registers after a content change */
static int tx_csc_apply(gsv_port *p)
{
    gsv_video *v = p->video; uint8_t t = 0; const uint8_t *m = NULL; unsigned ycc;
    if (p->status->hdmi_mode == 0) {
        ycc = v->colorspace != 0;
        gsv_read_bits(gsv_rx_dev(p), 0x20, 8, 3, &t);
        if (t != ycc) gsv_write_bits(gsv_rx_dev(p), 0x20, 8, 3, (uint8_t)ycc);
        gsv_read_bits(gsv_rx_dev(p), 0x21, 0xc0, 6, &t);
        if (t != 0) gsv_write_bits(gsv_rx_dev(p), 0x21, 0xc0, 6, 0);
        unsigned conv = 0;
        if (v->colorspace == 2 || v->colorspace == 1) { conv = 1; m = (v->v2 == 0x42 || v->v2 == 0x47) ? gsv_ycc_matrix_a : gsv_ycc_matrix_b; }
        gsv_read_bits(gsv_rx_dev(p), 0x2b, 0x80, 7, &t);
        if (t != conv) gsv_write_bits(gsv_rx_dev(p), 0x2b, 0x80, 7, (uint8_t)conv);
        if (conv == 1) {
            gsv_read_bits(gsv_rx_dev(p), 0x2d, 0xff, 0, &t);
            if (m[1] != t) { gsv_write_block(gsv_rx_dev(p), 0x2c, m, 0x18); gsv_write_bits(gsv_rx_dev(p), 0x2b, 0x60, 5, (uint8_t)((m[0] & 0x7f) >> 5)); }
        }
    }
    if (p->status->hdmi_mode == 0 || v->pixclk > 0x1c2) {
        gsv_read_bits(gsv_rx_dev(p), 0x53, 0x0f, 0, &t);
        if (t != 4) gsv_write_bits(gsv_rx_dev(p), 0x53, 0x0f, 0, 4);
    }
    return 0;
}

/* 0x080123c4: in the stable state, follow the source's video content changes */
static int tx_stable_video_update(gsv_port *p)
{
    gsv_port *root = gsv_root_source(p);
    if (!root) return -3;
    gsv_port *src = p->source; uint8_t buf[40]; bool refresh = false; uint8_t me = 0;
    if (p->status->s6 < 0x3d) p->status->s6++;      /* SDK 20210311 counts to 61 */
    uint16_t diff = root->video->flags ^ p->video->flags;
    if (root == src) {
        if ((root->video->flags & 1) && root->video->colorspace != p->video->colorspace) { p->video->colorspace = root->video->colorspace; p->video->v2 = root->video->v2; diff |= 1; }
    } else {
        if (p->idx == 0) me = 1; else if (p->idx == 1) me = 2;
        if (src->kind == 8) { if (src->scaler->dirty & me) { src->scaler->dirty &= (uint8_t)~me; refresh = true; } }
        else if (src->kind == 9 && (src->color->dirty & me)) { src->color->dirty &= (uint8_t)~me; refresh = true; }
    }
    if (diff & 1) gsv_tx_apply_from_source(root, p, buf, 1, 0, 0, 0);
    else if (refresh) gsv_tx_apply_from_source(root, p, buf, 1, 0, 0, 1);
    if (diff & 8)      gsv_tx_apply_from_source(root, p, buf, 8, 3, 0, 0);
    if (diff & 0x2000) gsv_tx_apply_from_source(root, p, buf, 0x2000, 0xe, 0, 0);
    if (diff & 0x100)  gsv_tx_apply_from_source(root, p, buf, 0x100, 8, 0, 0);
    if (diff & 0x200)  gsv_tx_apply_from_source(root, p, buf, 0x200, 9, 0, 0);
    if (diff & 0x10)   gsv_tx_apply_from_source(root, p, buf, 0x10, 4, 0, 0);
    if (diff & 0x20)   gsv_tx_apply_from_source(root, p, buf, 0x20, 5, 0, 0);
    if (diff & 0x40)   gsv_tx_apply_from_source(root, p, buf, 0x40, 6, 0, 0);
    if (diff & 0x80) {
        if ((root->video->flags & 0x80) == 0) { p->video->flags &= 0xff7f; buf[0] = 0; gsv_tx_packet_config(p, 7, buf, 1); }
        else {
            uint8_t d = 0;
            gsv_read_bits(gsv_rx_dev_e9a8(root), 3, 0x30, 4, &d);        /* 0x08009ea8 on the root */
            root->video->depth_code = d == 0 ? 0x18 : d == 1 ? 0x1e : d == 2 ? 0x24 : 0;
            gsv_k1_tx_depth_limit(root, p);
            gsv_tx_depth_reg(p);
            p->video->flags |= 0x80; buf[0] = 1; gsv_tx_packet_config(p, 7, buf, 1);
        }
    }
    if (root->rx->hdmi_mode == 0) {
        if (src->kind == 8) { src->scaler->colorspace = 0; p->video->colorspace = 0; p->video->v2 = 0x80; }
        else if (src->kind == 9) { src->color->colorspace = 0; src->color->u3 = 0x80; p->video->colorspace = 3; p->video->v2 = 0x42; }
        else { p->video->colorspace = 0; p->video->v2 = 0x80; }
    }
    tx_csc_apply(p);
    return 0;
}

/* 0x080121d0: in the stable state, follow the audio source's changes */
static int tx_stable_audio_update(gsv_port *p)
{
    if (p->status->hdmi_mode == 0) { p->audio->flags = 0; return 0; }
    gsv_port *root = gsv_audio_root_source(p);
    if (!root) return -3;
    uint8_t buf[40];
    uint16_t diff = root->audio->flags ^ p->audio->flags;
    if (diff & 0x400) gsv_tx_apply_from_source(root, p, buf, 0x400, 10, 1, 0);
    if (diff & 2)     gsv_tx_apply_from_source(root, p, buf, 2, 1, 1, 0);
    if (diff & 4)     gsv_tx_apply_from_source(root, p, buf, 4, 2, 1, 0);
    if (diff) { uint8_t mute = p->audio->mute; memcpy(p->audio, root->audio, sizeof *p->audio); p->audio->mute = mute; tx_audio_start(p); }
    tx_audio_recover(p);
    return 0;
}

/* 0x080122e8 */
static void tx_hdcp_bksv_check(gsv_port *p)
{
    if (p->hdcp_tx->authenticated != 0) {
        if (p->hdcp_tx->u6 == 1) {   /* 0x0800bd98 "Clear Tx Bksv Ready" */
            gsv_write_bits(gsv_tx_dev(p), 0x97, 0x40, 6, 1);
            p->hdcp_tx->u6 = 0;
            gsv_write_bits(gsv_tx_dev(p), 0xb0, 0x10, 4, 1);
            gsv_log("Clear Tx Bksv Ready\n");
        }
    }
}
static void tx_clear_mute_av(gsv_port *p)   /* 0x0801228c */
{
    if (p->status->hdmi_mode == 0) return;
    p->video->avmute = 0;
    gsv_log("from Kfun TX ClearMuteAv-->\n");
    gsv_tx_avmute(p);
    p->video->mute = 0;
    gsv_tx_update_mute_regs(p);
}
/* 0x08011888: enable the Tx core */
static void tx_enable_core(gsv_port *p)
{
    gsv_tx_output_disable(p);
    gsv_port *root = gsv_root_source(p);
    if (root && root->rx->locked == 1) {
        root = gsv_root_source(p);   /* 0x0800de1c */
        if (root == NULL || root->rx->locked != 0) {
            const uint32_t core = GSV_PAGE(0x40);     /* Gsv2k1 0x08018b1c */
            gsv_write_bits(core, 0x1f, 2, 1, (uint8_t)(1 - p->video->hsync_pol));
            gsv_write_bits(core, 0x1f, 1, 0, (uint8_t)(1 - p->video->vsync_pol));
        }
    }
}

/* ---- sink EDID (0x0800d554, 0x08010c0c, 0x08006c08) --------------------------------------------------- */
static int tx_ddc_fail_check(gsv_port *p);
static void tx_read_bksv(gsv_port *p);
/* read the sink's EDID over DDC into edid[len]; returns 0 ok, -1 failed, -2 already connected */
static int tx_read_edid(gsv_port *p, uint8_t *edid, int len)
{
    uint8_t hpd = 0, st = 0; uint16_t tries = 0; int8_t sum = 0;
    while (hpd == 0 && tries < 3000) {
        gsv_read_bits(gsv_tx_dev(p), 0xc5, 0x10, 4, &hpd);
        if (hpd) break;
        gsv_read_bits(gsv_tx_dev(p), 200, 0x0f, 0, &st);
        if (st == 1) { if (tx_ddc_fail_check(p) == 1) { gsv_log("Port%d: ReRead Edid Failure\n", p->id); break; } }
        tries++;
    }
    uint8_t ok = 0;
    if (hpd == 1) {
        gsv_read_block(gsv_tx_dev61(p), 0x00, edid, (uint16_t)len);
        for (unsigned i = 0; i < 0x7f; i++) sum = (int8_t)(sum + edid[i]);
        if (edid[0x7f] == (uint8_t)-sum) {
            if (edid[0x7e] == 1) { sum = 0; for (unsigned i = 0x80; i < 0xff; i++) sum = (int8_t)(sum + edid[i]); if (edid[0xff] == (uint8_t)-sum) ok = 1; }
            else ok = 1;
        }
    }
    if (!ok) {
        gsv_log("Port%d: Read Edid Failure\n", p->id);
        memset(edid, 0, 0x100);
        gsv_write_bits(gsv_tx_dev(p), 0xc9, 0x0f, 0, 0xf);
        gsv_write_bits(gsv_tx_dev(p), 0xc9, 0x10, 4, 0);
        gsv_write_bits(gsv_tx_dev(p), 0xc9, 0x10, 4, 1);
        return -1;
    }
    if (p->status->plug == 5) return -2;
    if (p->hdcp_tx->enabled == 0) { gsv_write_bits(gsv_tx_dev(p), 3, 1, 0, 1); tx_read_bksv(p); gsv_write_bits(gsv_tx_dev(p), 3, 1, 0, 0); }
    p->status->plug = 5;
    return 0;
}
static unsigned edid_find_spa_tx(const uint8_t *edid, uint8_t *spa)   /* 0x080113a8 */
{
    spa[0] = spa[1] = 0xff;
    if (edid[0x7e] == 0) return 1;
    if (edid[0x80] != 2 || edid[0x81] != 3) return 1;
    unsigned end = edid[0x82] < 4 ? 0xff : (uint8_t)(edid[0x82] + 0x80);
    for (unsigned i = 0x84; i < 0xff; i = (i + (edid[i] & 0x1f) + 1) & 0xff) {
        if (end <= i) return 1;
        if ((edid[i] & 0xe0) == 0x60) {
            if (edid[i + 1] == 0x03 && edid[i + 2] == 0x0c && edid[i + 3] == 0x00) { spa[0] = edid[i + 4]; spa[1] = edid[i + 5]; gsv_log("SPA Found =%02x,%02x\n", spa[0], spa[1]); return (i + 4) & 0xff; }
            return 1;
        }
    }
    return 1;
}
uint8_t gsv_tx_edid[2][256];
/* 0x08010c0c "Spa[0]..": read the sink EDID, derive HDMI mode and physical address */
static gsv_port *tx_find_rx_source(gsv_port *p);
/* SDK 20210311 (0x0802edf0, Gsv2k1 driver): checks the EDID header, hands the physical address to the CEC block
 * (page 0x41 reg 0x80), and raises the EDID event only when the EDID changed or the input is still powered down. */
static int tx_edid_manage(gsv_port *p)
{
    uint8_t spa[4] = { 0 }; gsv_tx_status *st = p->status;
    if (st->plug == 5) return 0;
    if (p->kind == 5) {
        uint8_t *edid = gsv_tx_edid[p->id == 5];
        tx_read_edid(p, edid, 0x100);
        if (edid[6] != 0xff || edid[7] != 0) { gsv_log("Port %d: Edid Checksum Fail, Abort\n", p->id); st->hdcp_mode = 2; return -1; }
        unsigned off = edid_find_spa_tx(edid, spa);
        st->spa = (off < 0x83 || off > 0xfd) ? 0 : (uint16_t)((spa[0] << 8) | spa[1]);
        gsv_write_field(GSV_PAGE(0x41), 0x80080, 0xff, 0xff, 0, 1, 2, st->spa);
        st->hdmi_mode = ((spa[0] == 0 && spa[1] == 0) || off < 0x82 || off > 0xfe) ? 0 : 1;
        if (edid[0x7f] == st->u0a[0] && edid[0xff] == st->u0a[1]) {
            gsv_port *src = tx_find_rx_source(p);
            if (src && src->kind == 0 && src->rx->mode == 0) gsv_event(p, 0xf);
        } else { st->u0a[0] = edid[0x7f]; st->u0a[1] = edid[0xff]; gsv_event(p, 0xf); }
    }
    gsv_k1_tx_hdmi_mode(p);
    k1_tx_caps(p);
    return 0;
}
void gsv_tx_read_edid_dump(gsv_port *p)   /* 0x08006c08: event 0xf */
{
    uint8_t *edid = gsv_tx_edid[p->id == 5];
    tx_read_edid(p, edid, 0x100);
    gsv_log("Tx %d Read Edid Start: \n", p->id - 3);
    for (unsigned i = 0; i < 0x100; i++) { gsv_log("%0.2x ", edid[i]); if ((i & 0xf) == 0xf) gsv_log("\r\n"); }
    gsv_log("\r\n");
    p->status->u10 = 0xffffffff;
}

/* ---- Tx plug state machine (0x0800cc80) ----------------------------------------------------------------- */
static gsv_port *tx_find_rx_source(gsv_port *p)   /* 0x0800f22c */
{
    for (gsv_port *q = p->source; q; q = q->source) if (q->kind == 0) return q;
    return NULL;
}
static void tx_toggle_tmds_out(gsv_port *p)   /* 0x0800fd98 */
{
    /* Gsv2k1 0x08019ec0: one Tx PLL; not for Tx A while both outputs are running */
    if (k1_tx_active_count(p) == 2 && p->id == 4) return;
    gsv_write_bits(GSV_PAGE(0x02), 0x86, 1, 0, 1); gsv_write_bits(GSV_PAGE(0x02), 0x86, 1, 0, 0);
    return;
}
static int tx_clock_check(gsv_port *p, uint8_t v)   /* 0x0800fc8c */
{
    (void)p; uint8_t r = 0;
    if (v == 0 || v == 0xff) {
        gsv_read_bits(GSV_PAGE(0x02), 1, 0xff, 0, &r);
        gsv_write_bits(GSV_PAGE(0x02), 1, 0xff, 0, r == 0 ? 0x0f : 0);
        return 1;
    }
    return 0;
}
static void tx_reset_phy(gsv_port *p)   /* 0x0800f3c8 */
{
    /* Gsv2k1 0x080192bc: the shared Tx core is reset only while no output is running */
    if (k1_tx_active_count(p) == 0) {
        gsv_write_bits(GSV_PAGE(0x40), 0xa6, 0x20, 5, 1); gsv_write_bits(GSV_PAGE(0x40), 0xa6, 0x20, 5, 0);
        gsv_write_bits(GSV_PAGE(0x40), 0x4f, 1, 0, 1);    gsv_write_bits(GSV_PAGE(0x40), 0x4f, 1, 0, 0);
    }
    gsv_write_bits(gsv_tx_dev(p), 0xf4, 0x10, 4, 0); gsv_write_bits(gsv_tx_dev(p), 0xf4, 0x10, 4, 1);
    return;
}
/* 0x0800fdf4: one DDC transaction through the Tx DDC master. write=0 reads one byte (returned), write!=0 writes */
static uint8_t tx_ddc_xfer(gsv_port *p, int write, uint8_t addr, uint8_t offset, uint8_t value)
{
    uint8_t busy = 0; uint8_t n = 0;
    gsv_write_bits(gsv_tx_dev(p), 0x1e, 0xff, 0, write == 0 ? (uint8_t)(addr | 1) : (uint8_t)(addr & 0xfe));
    gsv_write_bits(gsv_tx_dev(p), 0x20, 0xff, 0, offset);
    if (write != 0) gsv_write_bits(gsv_tx_dev(p), 0x21, 0xff, 0, value);
    gsv_write_bits(gsv_tx_dev(p), 0x22, 1, 0, 1);
    for (n = 0; n < 0x18; n++) { gsv_read_bits(gsv_tx_dev(p), 0x1c, 1, 0, &busy); if (busy == 1) break; }
    if (write == 1 || busy == 0) return 0xff;
    uint8_t r = 0; gsv_read_bits(gsv_tx_dev(p), 0x1d, 0xff, 0, &r);
    return r;
}
static int tx_ddc_fail_check(gsv_port *p)   /* 0x0800fce4 */
{
    uint32_t st = 0; uint8_t f = 0;
    gsv_write_bits(gsv_tx_dev(p), 0xaa, 1, 0, 0);
    gsv_write_bits(gsv_tx_dev(p), 0xaa, 1, 0, 1);
    gsv_read_field(gsv_tx_dev(p), 0x800ab, 7, 0xff, 0, 1, 2, &st);
    gsv_read_bits(gsv_tx_dev(p), 0xc5, 8, 3, &f);
    if (f == 1) gsv_write_bits(gsv_tx_dev(p), 0x97, 0x80, 7, 1);
    return (f == 1 || st == 0xf0) ? 1 : 0;
}
static void tx_read_bksv(gsv_port *p)   /* 0x0800f28c */
{
    uint8_t bksv[8]; uint8_t ones = 0;
    /* SDK 20210311 (0x08019004): stop the cipher first, and ask the sink for its
                                         HDCP 2.2 version byte (DDC 0x74 offset 0x50) right here */
    gsv_write_bits(gsv_tx_dev(p), 0xb0, 0x80, 7, 0);
    gsv_write_bits(gsv_tx_dev2(p), 0, 1, 0, 0);
    for (uint8_t i = 0; i < 5; i++) bksv[i] = tx_ddc_xfer(p, 0, 0x74, i, 0);
    static uint8_t last[5];           /* the original's RAM 0x08019134 -> one copy for both outputs: it logs only a
                                         changed value, and updates just the first byte that differs */
    for (uint8_t i = 0; i < 5; i++)
        if (last[i] != bksv[i]) {
            last[i] = bksv[i];
            gsv_log("Port %d: BKSV = %x,%x,%x,%x,%x\n", p->id, bksv[0], bksv[1], bksv[2], bksv[3], bksv[4]);
            break;
        }
    p->hdcp_tx->capable22 = tx_ddc_xfer(p, 0, 0x74, 0x50, 0) == 4;
    for (uint8_t i = 0; i < 5; i++) for (uint8_t b = 0; b < 8; b++) if ((bksv[i] >> b) & 1) ones++;
    p->hdcp_tx->enabled = ones == 20;
    if (!p->hdcp_tx->enabled) p->hdcp_tx->capable22 = 0;
    if (p->hdcp_tx->enabled) gsv_log("Port Tx %d: Tx is HDCP 1.4 Capable\n", p->id - 3);
    if (p->hdcp_tx->capable22) gsv_log("Port Tx %d: Tx is HDCP 2.2 Capable\n", p->id - 3);
    return;
}

void gsv_tx_fsm_step(gsv_port *p)   /* 0x08010da4 */
{
    switch (*p->tx_state) {
    case 3: case 4: case 5: case 8: case 9: case 0xc: gsv_tx_plug_fsm(p); break;
    case 6: gsv_event(p, 6); gsv_tx_plug_fsm(p); break;
    case 7: case 10: gsv_tx_plug_fsm(p); gsv_tx_hdcp_poll(p); break;
    case 0xb: if (p->status->s6 == 0) gsv_event(p, 7); gsv_tx_plug_fsm(p); gsv_tx_hdcp_poll(p); break;
    default: break;
    }
}

/* GSV2001 (Gsv2k1 0x080175a8): the same job as below. Differences: one Tx PLL register set for both outputs, the
 * output divider in page 2 reg 0xec, scrambling decided by the pixel clock (above 340 MHz), drive strength set per
 * clock range, SCDC written only when the sink is HDMI 2.0 capable, and the shared page 0x40 mute bits left alone
 * while both outputs are active. */
int gsv_tx_plug_fsm(gsv_port *p)
{
    gsv_tx_status *s = p->status; gsv_video *vd = p->video; gsv_audio *au = p->audio;
    uint8_t a = 0, b = 0; uint32_t pix = 0, tmp = 0; bool locked = false;
    uint32_t td = gsv_tx_dev(p);
    gsv_port *rx = tx_find_rx_source(p);
    if (s->hdcp_mode != 2 && s->hdcp_mode != 3) {
        gsv_read_bits(td, 0xf1, 1, 0, &a);
        if (p->id == 4) {
            gsv_read_bits(GSV_PAGE(0x03), 0x91, 0x10, 4, &b);
            if (b == 1) { gsv_write_bits(GSV_PAGE(0x03), 0x91, 0x40, 6, 1); a = 0; }
            gsv_read_bits(GSV_PAGE(0x00), 0x90, 1, 0, &b);
            if (b != a) gsv_write_bits(GSV_PAGE(0x00), 0x90, 1, 0, a);
        } else if (p->id == 5) {
            gsv_read_bits(GSV_PAGE(0x03), 0x95, 0x10, 4, &b);
            if (b == 1) { gsv_write_bits(GSV_PAGE(0x03), 0x95, 0x40, 6, 1); a = 0; }
            gsv_read_bits(GSV_PAGE(0x00), 0x90, 2, 1, &b);
            if (b != a) gsv_write_bits(GSV_PAGE(0x00), 0x90, 2, 1, a);
        }
        s->hdcp_mode = a;
    }
    if (s->hdcp_mode != 0 && rx) {
        if (p->idx == 0) {
            if (s->s6 < 6) { gsv_read_bits(GSV_PAGE(0x02), 0x8c, 0x80, 7, &a); if (a == 0 && rx->rx->lock_b == 1) gsv_k1_pll_kick(p); }
            else { gsv_read_bits(GSV_PAGE(0x02), 0x9b, 8, 3, &a); if (a == 0) tx_toggle_tmds_out(p); }
            if (a == 1 && rx->rx->stable == 1) locked = true;
        } else if (p->idx == 0xff && rx->rx->stable == 1) locked = true;
    }
    if (s->tmds_locked == 1 && !locked) { gsv_tx_set_tmds(p, 0); gsv_log("Port %d: PllUnlock\n", p->id); }
    if (locked) {
        if (p->idx == 0) {
            gsv_read_field(GSV_PAGE(0x02), 0x80090, 3, 0xff, 0, 1, 3, &pix);
            if (tx_clock_check(p, (uint8_t)pix) == 1) gsv_read_field(GSV_PAGE(0x02), 0x80090, 3, 0xff, 0, 1, 3, &pix);
            pix >>= 8;
            gsv_read_bits(GSV_PAGE(0x02), 0x8c, 0x1f, 0, &a); pix /= a;
            gsv_read_field(GSV_PAGE(0x02), 0x8008d, 7, 0xff, 0, 1, 2, &tmp); pix = tmp * pix;
            if (p->id == 4) gsv_read_bits(GSV_PAGE(0x02), 0xec, 0x70, 4, &a);
            else if (p->id == 5) gsv_read_bits(GSV_PAGE(0x02), 0xec, 7, 0, &a);
            else a = 0;
            pix /= (1u << a) * 5;
        } else if (p->idx == 0xff) pix = rx->video->tmds_clk;
        if (s->s6 > 5 && vd->pixclk != pix) locked = false;
        vd->pixclk = (uint16_t)pix;
        b = vd->pixclk > 0x154;                                   /* scrambled (HDMI 2.0) above 340 MHz */
        gsv_read_bits(td, 0xf1, 0x20, 5, &a);
        if (a != b) { gsv_write_bits(td, 0xf1, 0x10, 4, 1); gsv_write_bits(td, 0xf1, 0x20, 5, b); }
        gsv_read_bits(td, 0xf1, 0x80, 7, &a);
        if (a != b) { gsv_write_bits(td, 0xf1, 0x80, 7, b); gsv_write_bits(td, 0xf1, 0x40, 6, 1); }
        gsv_read_bits(td, 3, 1, 0, &a);
        if (a == 0) gsv_write_bits(td, 3, 1, 0, 1);
    }
    if (s->s6 == 1) { tx_reset_phy(p); gsv_k1_tx_hdmi_mode(p); }
    if (s->s6 == 5) {
        static const uint8_t drive[3][5] = { { 0xf0, 0xaf, 0x40, 0x0c, 0x03 }, { 0xfe, 0x0f, 0x47, 0xb6, 0x03 }, { 0xfe, 0x5f, 0x47, 0xbe, 0x1f } };
        /* The driver's three settings, by clock. Not in either firmware: a floor per output. On the oneboard
         * (2026-11-21, a capture card on output A, counting locks after a PHY reset) the first setting gave no
         * picture at 74 and 148 MHz and about four in five at 185, the other two every time at 74, 148, 185 and
         * 296 MHz; the bit that decides is reg 0xd5 bits 5:4 (0 in the first setting, 3 in the others). Output B,
         * a few centimetres of track to the capture chip, works with the first. */
        unsigned band = vd->pixclk >= 0x155 ? 2 : vd->pixclk >= 0xfb ? 1 : 0;
        if (band < gsv_k1_tx_drive_floor[p->id == 5]) band = gsv_k1_tx_drive_floor[p->id == 5];
        const uint8_t *d = drive[band];
        gsv_log("Port %d: Tx Pixel Frequency = %d\n", p->id, vd->pixclk);
        s->s6++;
        gsv_write_bits(td, 0xd1, 0xff, 0, d[0]);
        gsv_write_bits(td, 0xd3, 0xff, 0, d[1]);
        gsv_write_bits(td, 0xd4, 0xff, 0, d[2]);
        gsv_write_bits(td, 0xd5, 0xff, 0, d[3]);
        gsv_write_bits(td, 0xd6, 0xff, 0, d[4]);
        gsv_write_bits(gsv_tx_dev2(p), 0x28, 1, 0, 1);
        gsv_write_bits(gsv_tx_dev2(p), 0x28, 1, 0, 0);
        gsv_write_bits(td, 2, 1, 0, 0);
        if ((s->u10 & 1) == 0) { gsv_read_bits(td, 0xf2, 0x80, 7, &a); if (a == 0) gsv_tx_set_tmds(p, 1); }
        else {
            gsv_tx_set_tmds(p, 0);
            tx_ddc_xfer(p, 1, 0xa8, 0x01, 1);                     /* SCDC: source version, then scrambling + clock ratio */
            tx_ddc_xfer(p, 1, 0xa8, 0x20, b == 1 ? 3 : 0);
            gsv_tx_set_tmds(p, 1);
        }
        tx_toggle_tmds_out(p);
    }
    if (s->tmds_locked == 0 && locked) { gsv_write_bits(td, 0xf4, 0x10, 4, 0); gsv_write_bits(td, 0xf4, 0x10, 4, 1); }
    s->tmds_locked = locked;
    if (s->tmds_locked == 0) { vd->flags = 0; au->flags = 0; }
    else {
        if (vd->pixclk < 0xf && vd->pixclk != 0) { vd->colordepth_in = 1; vd->colordepth = 1; } else { vd->colordepth_in = 0; vd->colordepth = 0; }
        if (p->idx == 0) {
            if (s->s6 == 0 && k1_tx_active_count(p) == 1 && rx && rx->kind == 0) { vd->flags = rx->video->flags; au->flags = rx->audio->flags; }
            if (s->s6 == 1 && rx && rx->kind == 0 && rx->rx->stable == 1 && rx->rx->hdmi_mode == 0 && s->hdmi_mode == 1 && (vd->flags & 1) == 0) {
                uint8_t avi[0x20] = { 0 }; avi[1] = 2; avi[2] = 0xd;           /* default AVI infoframe header */
                gsv_tx_set_colorspace(p, avi, 1);
            }
            if (s->s6 > 4 && k1_tx_active_count(p) != 2) {
                gsv_read_bits(GSV_PAGE(0x40), 0x4e, 8, 3, &a);
                if (vd->mute != a) gsv_write_bits(GSV_PAGE(0x40), 0x4e, 8, 3, vd->mute);
                gsv_read_bits(GSV_PAGE(0x40), 0x1a, 0x20, 5, &a);
                uint8_t amute;
                if (au->mute == 1) amute = 1;
                else if (p->audio_source == NULL || p->audio_source->kind != 4) amute = (au->type == 2 || au->type == 4 || (au->flags & 0x400)) ? 0 : 1;
                else amute = 0;
                if (a != amute) gsv_write_bits(GSV_PAGE(0x40), 0x1a, 0x20, 5, amute);
            }
            if (s->s6 > 4) {
                uint8_t m = au->mute == 1;
                gsv_read_bits(GSV_PAGE(0x40), 0x1a, 0x20, 5, &a);
                if (a != m) {
                    /* With a source that sends no audio the block above sets this bit and this one clears it again, on
                     * every pass, as the original does. Keep the writes; say it once per value, not 50 times a second. */
                    static uint8_t said[2] = { 0xff, 0xff };
                    gsv_write_bits(GSV_PAGE(0x40), 0x1a, 0x20, 5, m);
                    if (said[p->id == 5] != m) { said[p->id == 5] = m; gsv_log("--------port:%d mute:%d------\n", p->id, m); }
                }
            }
        }
    }
    gsv_read_bits(td, 0xc5, 0x10, 4, &a);
    if (s->s9 == 0) {
        if (a == 1) { if (s->plug == 0) s->plug = 1; }
        else if (s->hdcp_mode == 1 && s->plug == 0) {
            if (tx_ddc_fail_check(p) == 1) { gsv_log("Port %d: DDC Fail, Abort\n", p->id); s->edid_retry = 0; tx_toggle_hpd(p); }
            gsv_read_bits(td, 0xc9, 0x0f, 0, &a);
            if (a == 0) {
                gsv_log("Port %d: Edid Fail, Retry\n", p->id);
                gsv_write_bits(td, 0x97, 0x80, 7, 1);
                tx_toggle_hpd(p);
                if (s->edid_retry != 0xff) s->edid_retry++;
                if (s->edid_retry > 200) { s->edid_retry = 0; s->plug = 2; }
            }
        }
    } else s->plug = 5;
    /* The original asks a connected sink for its BKSV on every pass until it is HDCP capable. A no_hdcp output's sink
     * never is (the MS2130), so that was 14 DDC transfers a pass for ever; the read inside the EDID read stays. */
    if (s->plug == 5 && p->hdcp_tx->enabled == 0 && p->no_hdcp != 1) tx_read_bksv(p);
    return 0;
}


/* ---- Tx HDCP / stable state machine (0x08006f48 "AvFsmPlugTx*") ------------------------------------- */
uint8_t gsv_app_hdcp_setting(void);
extern gsv_port *g_port_rx;

/* SDK 20210311 (0x0800ca7c): the Tx plug state machine as Gscoolink ships it. (Kramer's build also holds output B in
 * reset until its application allows it; that gate is left out.) */
void gsv_tx_hdcp_fsm(gsv_port *p)
{
    gsv_tx_status *s = p->status; gsv_hdcp_tx *h = p->hdcp_tx;
    bool to_default_edid = false, unlock = false, edid_check = false, follow = false;
    switch (*p->tx_state) {
    default:
        if (s->tmds_locked == 0) {
            if (s->hdcp_mode != 1) {
                if (s->hdcp_mode == 2) { s->hdcp_mode = 1; *p->tx_state = TXPLUG_VIDEO_UNLOCKED; tx_video_unlocked_enter(p); }
                else { gsv_event(p, 5); tx_go_reset(p); }
            }
        } else { tx_enable_core(p); *p->tx_state = TXPLUG_TRANSMIT_VIDEO; }
        break;
    case TXPLUG_DEFAULT: tx_go_reset(p); break;
    case TXPLUG_DEFAULT_EDID: *p->tx_state = TXPLUG_EDID_MANAGE; break;
    case TXPLUG_EDID:
        if (s->hdcp_mode == 1) {
            if (s->plug == 2) { if (s->s9 == 0) { *p->tx_state = TXPLUG_EDID_ERROR; gsv_event(p, 0xe); } else if (s->s9 == 1) to_default_edid = true; else edid_check = true; }
            else edid_check = true;
        } else tx_go_reset(p);
        break;
    case TXPLUG_EDID_ERROR: if (s->hdcp_mode != 1) tx_go_reset(p); break;
    case TXPLUG_EDID_MANAGE:
        if (s->hdcp_mode == 1) { if (s->tmds_locked == 1 && s->plug == 5) *p->tx_state = TXPLUG_TRANSMIT_VIDEO; else tx_edid_manage(p); }
        else { gsv_event(p, 5); tx_go_reset(p); }
        break;
    case TXPLUG_ENABLE_TX_CORE: *p->tx_state = TXPLUG_EDID; break;
    case TXPLUG_HDCP:
        if (s->hdcp_mode == 1) {
            if (h->ok == 0 && h->running22 == 0) { if (s->tmds_locked == 0) unlock = true; else tx_hdcp_bksv_check(p); }
            else { *p->tx_state = TXPLUG_STABLE; tx_clear_mute_av(p); }
        } else if (s->hdcp_mode == 2) { s->hdcp_mode = 1; unlock = true; }
        else { gsv_event(p, 5); tx_go_reset(p); }
        break;
    case TXPLUG_HPD_ANTI_DITHER:
        if (s->s15 == 0) { if (s->hdcp_mode != 1) tx_go_reset(p); }
        else { *p->tx_state = TXPLUG_ENABLE_TX_CORE; tx_enable_core(p); }
        break;
    case TXPLUG_RESET:
        if (s->hdcp_mode == 1) { *p->tx_state = TXPLUG_HPD_ANTI_DITHER; s->s15 = 1; } else s->hdcp_mode = 0;
        break;
    case TXPLUG_STABLE:
        if (s->hdcp_mode == 1) {
            if (s->tmds_locked == 0) unlock = true;
            else { tx_stable_video_update(p); tx_stable_audio_update(p); tx_hdcp_bksv_check(p); }
        } else if (s->hdcp_mode == 2) { s->hdcp_mode = 1; unlock = true; }
        else { gsv_event(p, 5); tx_go_reset(p); }
        break;
    case TXPLUG_TRANSMIT_VIDEO:
        if (s->hdcp_mode == 1) {
            if (s->s6 < 5) follow = true;
            else if (h->authenticated == 1) *p->tx_state = TXPLUG_HDCP;
            else if (h->authenticated == 0) { *p->tx_state = TXPLUG_STABLE; tx_clear_mute_av(p); }
            else follow = true;
        } else if (s->hdcp_mode == 2) { s->hdcp_mode = 1; unlock = true; }
        else { gsv_event(p, 5); tx_go_reset(p); }
        break;
    }
    if (follow) { if (s->tmds_locked == 0) unlock = true; else { tx_stable_video_update(p); tx_stable_audio_update(p); } }
    if (edid_check && s->plug != 0) { if (s->s9 == 1) to_default_edid = true; else if (s->s9 == 0) *p->tx_state = TXPLUG_EDID_MANAGE; }
    if (unlock) { *p->tx_state = TXPLUG_VIDEO_UNLOCKED; tx_video_unlocked_enter(p); }
    if (to_default_edid) { *p->tx_state = TXPLUG_DEFAULT_EDID; gsv_event(p, 0xd); s->hdmi_mode = 1; gsv_k1_tx_hdmi_mode(p); }
}


