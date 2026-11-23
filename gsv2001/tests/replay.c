/* Replay check for the GSV2001 variant: the rewritten SDK against traces of the Gsv2k1 driver inside Kramer's KDS-EN7
 * firmware (../gsv2001/tools/emu.py). Same idea as replay_idle.c; the system is one Rx, Tx A, Tx B.
 *   replay_k1 TRACE init            chip init only
 *   replay_k1 TRACE N               build, then N passes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "gsv_sdk.h"
#include "gsv_app.h"

/* the sink EDID of ../gsv2001/tools/emu.py (sink_edid), also given to the input in the "edid@" runs */
static const uint8_t k1_test_edid[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x1e, 0x6d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x03, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0xc4, 0x8e, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xb2,
    0x02, 0x03, 0x13, 0x70, 0x43, 0x10, 0x04, 0x01, 0x23, 0x09, 0x07, 0x07, 0x66, 0x03, 0x0c, 0x00,
    0x10, 0x00, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9,
};


int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: replay_k1 TRACE init|N\n"); return 2; }
    harness_load(argv[1]);
    gsv_platform_init(&harness_platform);
    static uint32_t dev0; static gsv_chip chip; static gsv_port rx0, txa, txb, scaler;
    int scale_on = -1, scale_off = -1;                  /* "scaler@ON,OFF": passes at which output B goes through the scaler and back */
    if (argc > 3) sscanf(argv[3], "scaler@%d,%d", &scale_on, &scale_off);
    if (argc > 4 && !strcmp(argv[4], "passthrough-a")) txa.bypass = 1;      /* output A as a pass-through of the input */
    if (argc > 6 && !strcmp(argv[6], "nohdcp-b")) txb.no_hdcp = 1;          /* never try HDCP on output B */
    int edid_at = -2;                                   /* "edid@N": the input gets an EDID after the build and again in pass N */
    if (argc > 5) sscanf(argv[5], "edid@%d", &edid_at);
    static uint8_t edid[256];
    dev0 = GSV_PAGE(0x00);
    gsv_chip_create(&chip, 1, 0, &dev0, &rx0, 0);
    gsv_port_create(&chip, &rx0, GSV_ID_RX0, GSV_KIND_RX);
    gsv_port_create(&chip, &txa, GSV_ID_TXA, GSV_KIND_TX);
    gsv_port_create(&chip, &txb, GSV_ID_TXB, GSV_KIND_TX);
    if (scale_on >= 0) gsv_port_create(&chip, &scaler, GSV_ID_SCALER, GSV_KIND_SCALER);
    g_port_rx = &rx0; g_port_txa = &txa; g_port_txb = &txb;      /* the app's globals, which parts of the Tx code read */
    if (!strcmp(argv[2], "init")) { gsv_chip_init(&chip); harness_done(); return 0; }
    harness_mark("build");
    gsv_chip_init(&chip);
    gsv_ports_init_all();
    gsv_port_link(&rx0, &txa, 2);
    gsv_port_link(&rx0, &txb, 2);
    if (edid_at != -2) { memcpy(edid, k1_test_edid, 256); gsv_rx_set_edid(&rx0, edid); }
    int ticks = atoi(argv[2]); char m[32];
    for (int t = 0; t < ticks; t++) {
        snprintf(m, sizeof m, "tick %d", t);
        harness_mark(m);
        if (t == scale_on) { gsv_port_link(&rx0, &scaler, 0); gsv_port_link(&scaler, &txb, 0); }
        if (t == edid_at) { memcpy(edid, k1_test_edid, 256); gsv_rx_set_edid(&rx0, edid); }
        if (t == scale_off) gsv_port_link(&rx0, &txb, 2);
        gsv_fsm_run();
    }
    harness_done();
    /* where the SDK ended up (the same state the firmware was in, since every transfer matched) */
    printf("    input: locked %d stable %d hdmi %d encrypted %d vic %u\n", rx0.rx->locked, rx0.rx->stable, rx0.rx->hdmi_mode, rx0.rx->encrypted, rx0.video->vic);
    gsv_port *tx[2] = { &txa, &txb };
    for (int i = 0; i < 2; i++)
        printf("    output %c: state %d index %d hdmi %d s6 %d s9 %d clock %u vic %u mute %d | hdcp: type %d sink14 %d sink22 %d authenticated %d ok %d errors %d\n", 'A' + i,
               *tx[i]->tx_state, tx[i]->idx, tx[i]->status->hdmi_mode, tx[i]->status->s6, tx[i]->status->s9, tx[i]->video->pixclk, tx[i]->video->vic, tx[i]->video->mute,
               tx[i]->hdcp_tx->type, tx[i]->hdcp_tx->enabled, tx[i]->hdcp_tx->capable22, tx[i]->hdcp_tx->authenticated, tx[i]->hdcp_tx->ok, tx[i]->hdcp_tx->err_count);
    return 0;
}
