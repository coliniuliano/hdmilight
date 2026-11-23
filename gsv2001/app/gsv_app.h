/* The application layer around the SDK for the oneboard: ports, routing and the EDID shown to the source (after the
 * EZ-SP12H2's app_init / app_route_update). */
#pragma once
#include "gsv_sdk.h"
#include "gsv_edid.h"

typedef struct {
    gsv_chip  chip;
    uint32_t  dev0;
    gsv_port  rx0, txa, txb, scaler, color;    /* color is never created: only a place-holder for gsv_route_set */
    uint8_t   scaler_txa, scaler_txb;      /* DIP: downscale this output when the input is 4K */
    uint8_t   passthrough_txa;             /* output A is a pass-through of the input instead of taking the Tx core */
    uint8_t   no_hdcp_txa;                 /* never try HDCP on output A either (output B never does) */
    uint8_t   edid[256];                   /* what the Rx presents to the source */
} gsv_system;

extern gsv_port *g_port_rx, *g_port_txa, *g_port_txb, *g_port_scaler;

void gsv_system_init(gsv_system *s);                     /* what app_init does before its main loop: objects, links, default EDID */
extern const char *gsv_app_edid_name;                   /* not in the original: overrides the monitor name in the built-in EDID (NULL or empty = keep "HDMI Light") */
void gsv_app_set_edid(gsv_system *s, const uint8_t *edid, int strip_dv);   /* fix up and program an EDID into the Rx (edid_select strips Dolby Vision when a downscale DIP is on) */
int  gsv_app_route_update(gsv_system *s);                /* 0x080079dc, once per SDK tick */
