#include "gsv_app.h"
#include <string.h>

gsv_port *g_port_rx, *g_port_txa, *g_port_txb, *g_port_scaler;
uint8_t g_cfg_scaler_txa, g_cfg_scaler_txb;
static gsv_system *s_sys;

const char *gsv_app_edid_name;
uint8_t gsv_app_hdcp_setting(void) { return 0; }

/* The original keeps one status byte per input (RAM 0x20000097): the Rx HDCP state machine stores the input's HDCP
 * state there (0 none, 1 = HDCP 1.4, 2 = HDCP 2.2; 0x08012f5c) and the Tx cipher reads it back (0x08012f1c) to
 * encrypt each output the way the input is encrypted. */
static uint8_t s_input_status[1];
void gsv_app_status_set(int which, uint8_t val) { if (which == 1) s_input_status[0] = val; }
uint8_t gsv_app_setting(int idx) { return idx == 1 ? s_input_status[0] : 0xff; }

void gsv_system_init(gsv_system *s)   /* 0x080106f4 app_init, object part */
{
    s_sys = s; g_cfg_scaler_txa = s->scaler_txa; g_cfg_scaler_txb = s->scaler_txb;
    s_input_status[0] = 0;
    s->dev0 = GSV_PAGE(0x00);
    gsv_chip_create(&s->chip, 1, 0, &s->dev0, &s->rx0, 0);
    /* One input and one Tx core. Output A carries the input as it is (pass-through,
     * so the TV gets 4K60 / HDR untouched); output B takes the Tx core, which the route update below feeds from the
     * scaler when the input is 4K and scaler_txb is set (the datasheet's 4K + 1080p example). */
    s->txa.bypass = s->passthrough_txa ? 1 : 0;
    s->txa.no_hdcp = s->no_hdcp_txa ? 1 : 0;
    s->txb.no_hdcp = 1;                                 /* output B's sink is the capture chip: it has no HDCP */
    gsv_port_create(&s->chip, &s->rx0, GSV_ID_RX0, GSV_KIND_RX);
    gsv_port_create(&s->chip, &s->txa, GSV_ID_TXA, GSV_KIND_TX);
    gsv_port_create(&s->chip, &s->txb, GSV_ID_TXB, GSV_KIND_TX);
    gsv_port_create(&s->chip, &s->scaler, GSV_ID_SCALER, GSV_KIND_SCALER);
    g_port_rx = &s->rx0; g_port_txa = &s->txa; g_port_txb = &s->txb; g_port_scaler = &s->scaler;
    gsv_chip_init(&s->chip);
    gsv_ports_init_all();
    gsv_port_link(&s->rx0, &s->txa, 2);
    gsv_port_link(&s->rx0, &s->txb, 2);
    gsv_app_set_edid(s, gsv_edid_builtin, 0);
}

void gsv_app_set_edid(gsv_system *s, const uint8_t *edid, int strip_dv)
{
    memcpy(s->edid, edid, 256);
    if (strip_dv) gsv_edid_strip_dolby_vision(s->edid);
    gsv_edid_fix_checksums(s->edid);
    if (gsv_app_edid_name && gsv_app_edid_name[0]) gsv_edid_set_name(s->edid, gsv_app_edid_name);
    gsv_rx_set_edid(&s->rx0, s->edid);
}

/* 0x08007b40: 1 = direct, 3 = through the scaler when the input is 4K and the DIP switch asks for it */
static int route_wanted(gsv_system *s, gsv_port *rx, gsv_port *tx)
{
    (void)s;
    if (rx->kind != 0 || tx->kind != 5) return 0;
    if (tx->status->plug != 5 || tx->status->hdcp_mode != 1) return 0;
    int want = 1;
    bool big = rx->video->pixclk > 0x121;
    if ((big && rx->video->colorspace == 3) || (big && rx->video->hactive > 0xeff && rx->video->vactive > 0x86f)) {
        if (g_cfg_scaler_txa == 1 && tx->id == 4) want = 3;
        if (g_cfg_scaler_txb == 1 && tx->id == 5) want = 3;
    }
    return want;
}
/* 0x08007b04 */
static int route_current(gsv_port *rx, gsv_port *tx, gsv_port *scaler, gsv_port *color)
{
    if (tx->source == rx) return 1;
    if (tx->source == color) return color->source == rx ? 2 : 0;
    if (tx->source == scaler) return scaler->source == rx ? 3 : 0;
    return 0;
}

int gsv_app_route_update(gsv_system *s)   /* 0x080079dc "TxBConnectStyle" */
{
    if (s->chip.c0 != 1) return -4;
    /* only output B is routed: direct, or through the scaler for 4K */
    if (s->rx0.rx->locked != 1) return 0;
    int w = route_wanted(s, &s->rx0, &s->txb);
    if (w == 2) w = 1;
    if (w != 0 && route_current(&s->rx0, &s->txb, &s->scaler, &s->color) != w) {
        gsv_log("\t******TxBConnectStyle=[%d]\r\n", w);
        gsv_route_set(&s->rx0, &s->txb, &s->color, &s->scaler, w);
    }
    return 0;
}
