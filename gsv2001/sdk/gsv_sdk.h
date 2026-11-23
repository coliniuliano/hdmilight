/* Gscoolink's splitter SDK for the GSV2001, rewritten in portable C.
 *
 * Gscoolink publishes no register map or SDK for the GSV2001 (the public Product Specification Rev 2.0 has the pin
 * table and nothing else), so this library was written from Ghidra decompiles of the firmware of three shipping
 * products. No vendor firmware and no decompiled vendor code is in this repository.
 *
 *   EZCOO EZ-SP12H2 v2.06        a GSV2002 product; the framework was recovered from it (SDK "Gsv2k2", 2019-09-23)
 *   Kramer KDS-EN7               GitHub Qiuzixing/IP5000-A302, IP5000-A30V0.0.20.bin; its "Gsv2k1" driver
 *                                (SDK 20210311) is the one this library copies register sequence for register
 *                                sequence, and the one tools/emu.py replays
 *   Blustream SP12CS             cdn.blustream.co.uk/SP12CS.zip, unpacked with tools/fw2_decode.py; the same
 *                                driver a year newer, used to see where it changed (tools/emu_sp12cs.py)
 *
 * The images are not in git. To run the tools, put them in gsv2001/work/ (ignored) as
 * work/kramer/IP5000-A30V0.0.20.bin and work/sp12cs.bin.
 *
 * The framework is port objects, content blocks, state machines and register access through gsv_write_bits /
 * gsv_read_bits. Addresses in comments are the originals' (0x0800.... in the EZ-SP12H2 image, 0x0801.... /
 * 0x0802.... / 0x0803.... in Kramer's). Names come from the SDK's own debug strings where they exist.
 *
 * `make -C gsv2001 check` replays 18 scenarios against tools/emu.py and stops at the first transaction that
 * differs. Four things depart from the Kramer driver on purpose, each explained where it is written:
 * gsv_k1_init_4k60 (gsv_core.c, with what was measured), gsv_k1_tx_drive_floor (gsv_tx.c, likewise), the
 * no_hdcp flag (gsv_core.c, gsv_tx.c) and quieter logging. All four are off by default, which is why the
 * replays still match.
 *
 * The chip: I2C address 0x58, 16-bit register address (page, register). Pages 0x00 to 0x05 chip and Rx core,
 * 0x12 scaler, 0x20 to 0x29 Rx port, 0x40/0x42 Tx core, 0x60 to 0x63 and 0x70 to 0x73 outputs A and B. */
#pragma once
#include "gsv_platform.h"
#include <stddef.h>

/* ---- register access -------------------------------------------------------------------------------------- */
int gsv_write_bits(uint32_t dev, uint8_t reg, uint8_t mask, uint8_t shift, uint8_t value);          /* 0x08007800 */
int gsv_read_bits(uint32_t dev, uint8_t reg, uint8_t mask, uint8_t shift, uint8_t *out);            /* 0x08007688 */
/* multi-byte fields: `reg` packs (first mask << 16 | register); last_mask applies to the other end;
 * msb_first selects the byte order; len bytes */
int gsv_read_field(uint32_t dev, uint32_t reg, uint8_t first_mask, uint8_t last_mask, uint8_t shift, int msb_first, int len, uint32_t *out);   /* 0x080075a8 */
int gsv_write_field(uint32_t dev, uint32_t reg, uint8_t first_mask, uint8_t last_mask, uint8_t shift, int msb_first, int len, uint32_t value); /* 0x080076f0 */

extern const uint8_t gsv2k1_init_table[];
extern const uint8_t gsv2k1_rxport_init_table[];

extern const uint8_t gsv_pkt_reg[], gsv_pkt_len[], gsv_pkt_rx_reg_hdr[], gsv_pkt_rx_reg_body[];
extern const uint8_t gsv_vic_aspect[], gsv_audio_fs_table[], gsv_audio_chan_table[], gsv_fs_table[];
extern const uint8_t gsv_csc_default[], gsv_ycc_matrix_a[], gsv_ycc_matrix_b[], gsv_csc_bypass_vics[];
extern const struct gsv_csc_entry { uint8_t in_cs, out_cs, pad0, pad1; const uint8_t *matrix; } gsv_csc_table[];
int gsv_read_block(uint32_t dev, uint8_t reg, uint8_t *data, uint16_t len);     /* 0x08007578 */
int gsv_write_block(uint32_t dev, uint8_t reg, const uint8_t *data, uint16_t len);  /* 0x080076c0 */

/* ---- objects --------------------------------------------------------------------------------------------- */
extern uint8_t  gsv_k1_init_4k60;            /* GSV2001: chip init adds the newer SDK's page 0x20 reg 0x46 = 0x84 (gsv_core.c) */
extern uint8_t  gsv_k1_tx_drive_floor[2];    /* GSV2001: lowest output-driver setting (0..2) output A / B may use (gsv_tx.c) */

typedef struct gsv_port gsv_port;

typedef struct {
    uint8_t   c0, c1;
    uint32_t *dev_base;      /* -> GSV_PAGE(0) descriptor; page offsets are ORed into it */
    gsv_port *first_port;
    gsv_port **state;        /* g_chip_state: [0] active Rx port, [1] active scaler/colour port */
} gsv_chip;

/* port kinds (gsv_port.kind) */
enum { GSV_KIND_RX = 0, GSV_KIND_RX1 = 1, GSV_KIND_TX = 5, GSV_KIND_TX6 = 6, GSV_KIND_SCALER = 8, GSV_KIND_COLOR = 9, GSV_KIND_AUDIO = 0xc };
/* port ids (gsv_port.id): Rx 0/1, TxA 4, TxB 5, audio 8, scaler 0x14, colour 0x18 */
enum { GSV_ID_RX0 = 0, GSV_ID_RX1 = 1, GSV_ID_TXA = 4, GSV_ID_TXB = 5, GSV_ID_AUDIO = 8, GSV_ID_SCALER = 0x14, GSV_ID_COLOR = 0x18 };

/* Rx port state machine states (strings: PlugRx*) */
enum { RXPLUG_DEFAULT = 1, RXPLUG_DETECT, RXPLUG_INFO_UPDATE, RXPLUG_INPUT_LOCK, RXPLUG_PLUGGED, RXPLUG_PULL_DOWN_HPD, RXPLUG_READ_TIMING, RXPLUG_RESET, RXPLUG_STABLE };
enum { RX_DEFAULT = 1, RX_DETECT, RX_FREERUN, RX_RECEIVING, RX_RESET };
/* Tx port state machine states (strings: PlugTx*) */
enum { TXPLUG_DEFAULT = 1, TXPLUG_DEFAULT_EDID, TXPLUG_EDID, TXPLUG_EDID_ERROR, TXPLUG_EDID_MANAGE, TXPLUG_ENABLE_TX_CORE, TXPLUG_HDCP,
       TXPLUG_HPD_ANTI_DITHER, TXPLUG_RESET, TXPLUG_STABLE, TXPLUG_TRANSMIT_VIDEO, TXPLUG_VIDEO_UNLOCKED };

/* content blocks (sizes are the original allocation sizes; field names by offset where the role is not yet known) */
typedef struct {                /* port+0x34, 0x1e bytes: Rx link status */
    uint8_t inactive;           /* +0  set when port->idx == 0xff */
    uint8_t u1, u2;
    uint8_t stable;             /* +3 */
    uint8_t locked;             /* +4  video locked */
    uint8_t encrypted;          /* +5  input is HDCP encrypted */
    uint8_t plug_sub;           /* +6  5 = plugged */
    uint8_t cable_5v;           /* +7  +5V detected */
    uint8_t mode;               /* +8  0/2/5 */
    uint8_t lock_delay;         /* +9 */
    uint8_t lock_a, lock_b, lock_c;   /* +0xa, +0xb, +0xc */
    uint8_t audio_present;      /* +0xd */
    uint8_t eq_done;            /* +0xe */
    uint8_t u0f;
    uint16_t video_changed;     /* +0x10 bit mask of changed video flags */
    uint16_t audio_changed;     /* +0x12 */
    uint8_t hdmi_mode;          /* +0x14 */
    uint8_t u15;
    uint16_t eq_timer;          /* +0x16 */
    uint8_t u18[6];
} gsv_rx_info;

typedef struct {                /* port+0x38, 0x1c bytes: Tx status */
    uint8_t hdmi_mode;          /* +0  sink is HDMI (1) or DVI (0) */
    uint8_t u1;
    uint8_t tmds_locked;        /* +2 */
    uint8_t u3, u4, u5;
    uint8_t s6;                 /* +6 stage counter */
    uint8_t edid_retry;         /* +7 */
    uint8_t plug;               /* +8  0..5, 5 = connected */
    uint8_t s9;                 /* +9 */
    uint8_t u0a[2];
    uint16_t spa;               /* +0xc  CEC physical address from the sink EDID */
    uint8_t u0e[2];
    uint32_t u10;               /* +0x10 */
    uint8_t hdcp_mode;          /* +0x14 */
    uint8_t s15;                /* +0x15 */
    uint16_t u16, u18;          /* +0x16, +0x18 (set to 0xffff when the source resets) */
    uint8_t u1a[2];
} gsv_tx_status;

typedef struct {                /* port+0x3c, 8 bytes: Rx HDCP */
    uint8_t mode;               /* +0  1..4 */
    uint8_t u1;
    uint8_t downstream_count;   /* +2 */
    uint8_t u3, u4;
    uint8_t counter;            /* +5 */
    uint8_t u6;
    uint8_t running22;          /* +7 */
} gsv_hdcp_rx;

typedef struct {                /* port+0x40, 12 bytes: Tx HDCP */
    uint8_t enabled;            /* +0 */
    uint8_t authenticated;      /* +1 */
    uint8_t type;               /* +2  0..6 */
    uint8_t u3;
    uint8_t ok;                 /* +4 */
    uint8_t step;               /* +5 */
    uint8_t u6;
    uint8_t err_count;          /* +7 */
    uint8_t u8;
    uint8_t capable22;          /* +9 */
    uint8_t running22;          /* +0xa */
    uint8_t retry;              /* +0xb */
} gsv_hdcp_tx;

typedef struct {                /* port+0x44, 0x22 bytes: video content */
    uint8_t depth_code;         /* +0  0x18/0x1e/0x24 for 8/10/12 bit */
    uint8_t colorspace;         /* +1  AVI Y1Y0 (0 RGB, 1 4:2:2, 2 4:4:4, 3 4:2:0) */
    uint8_t v2, v3;             /* +2 colorimetry code in / out */
    uint8_t aspect;             /* +4 */
    uint8_t avmute;             /* +5 */
    uint8_t u6;
    uint8_t mute;               /* +7 */
    uint16_t pixclk;            /* +8 */
    uint16_t tmds_clk;          /* +0xa */
    uint8_t three_d;            /* +0xc */
    uint8_t three_d_fmt;        /* +0xd */
    uint8_t u0e[2];
    uint8_t vic;                /* +0x10 */
    uint8_t hsync_pol;          /* +0x11 */
    uint8_t vsync_pol;          /* +0x12 */
    uint8_t interlaced;         /* +0x13 */
    /* The next four names are swapped against what the chip reports: htotal holds the active width,
     * vtotal the active height, hactive the horizontal total and vactive the vertical total. The code uses them rightly. */
    uint16_t htotal;            /* +0x14 */
    uint16_t vtotal;            /* +0x16 */
    uint16_t hactive;           /* +0x18 */
    uint16_t vactive;           /* +0x1a */
    uint16_t frame_rate;        /* +0x1c */
    uint16_t flags;             /* +0x1e  bit per packet type received/applied */
    uint8_t colordepth_in;      /* +0x20 */
    uint8_t colordepth;         /* +0x21 */
    uint16_t t2a, t2c, t36, t3a;  /* GSV2001 driver only: four more timing words (page 0x20 reg 0x2a, 0x2c, 0x36 / 2, 0x3a / 2 rounded up) */
} gsv_video;

typedef struct {                /* port+0x48, 0x18 bytes: audio content */
    uint32_t n_value;           /* +0  24-bit N from the Rx */
    uint16_t flags;             /* +4 */
    uint8_t mute;               /* +6 */
    uint8_t fs;                 /* +7 */
    uint8_t type;               /* +8  0 LPCM, 2 HBR, 3 DSD, 4 DST, 0x10 unknown, 0xff none */
    uint8_t layout;             /* +9 */
    uint8_t u0a;
    uint8_t cs_fs_ok;           /* +0xb */
    uint8_t ca, lsv_dm, lfep;   /* +0xc .. +0xe from the audio infoframe */
    uint8_t cs_byte1;           /* +0xf */
    uint8_t cs_fs;              /* +0x10 */
    uint8_t channels;           /* +0x11 */
    uint8_t fs_index;           /* +0x12 */
    uint8_t cs_wl;              /* +0x13 */
    uint8_t cs_byte4;           /* +0x14 */
    uint8_t u15[3];
} gsv_audio;

typedef struct { uint8_t enabled, in_vic, u2, colorspace, dirty; } gsv_scaler;             /* port+0x4c, 5 bytes */
typedef struct { uint8_t enabled, vic, colorspace, u3, u4, u5, dirty; } gsv_color;         /* port+0x50, 7 bytes */

struct gsv_port {
    gsv_chip *chip;             /* +0x00 */
    uint8_t   kind;             /* +0x04 */
    int16_t   id;               /* +0x06 */
    uint8_t  *flags;            /* +0x08 (2 bytes) */
    uint8_t  *plug_state;       /* +0x0c */
    uint8_t  *st10;             /* +0x10 */
    uint8_t  *rx_state;         /* +0x14 */
    uint8_t  *st18;             /* +0x18 */
    uint8_t  *hdcp_state;       /* +0x1c */
    uint8_t  *st20;             /* +0x20 */
    uint8_t  *tx_state;         /* +0x24 */
    uint8_t  *st28, *st2c, *st30;
    gsv_rx_info   *rx;          /* +0x34 */
    gsv_tx_status *status;      /* +0x38 */
    gsv_hdcp_rx   *hdcp;        /* +0x3c */
    gsv_hdcp_tx   *hdcp_tx;     /* +0x40 */
    gsv_video     *video;       /* +0x44 */
    gsv_audio     *audio;       /* +0x48 */
    gsv_scaler    *scaler;      /* +0x4c */
    gsv_color     *color;       /* +0x50 */
    gsv_port *chain;            /* +0x54 */
    void     *p58;              /* +0x58 */
    gsv_port *source;           /* +0x5c  upstream video port */
    gsv_port *downstream2;      /* +0x60  sibling list of ports fed by the same source */
    gsv_port *downstream;       /* +0x64 */
    gsv_port *audio_source;     /* +0x68 */
    gsv_port *audio_downstream2;/* +0x6c */
    gsv_port *audio_downstream; /* +0x70 */
    uint8_t   idx;              /* +0x74  Rx: 0xff until plugged; Tx: 0 = TxA, 1 = TxB */
    uint8_t   bypass;           /* +0x75 unused by the original (0xff). Here, GSV2001 only: 1 = this Tx is a pass-through of the input */
    uint8_t   no_hdcp;          /* +0x76 unused by the original. Here, GSV2001 only: 1 = never try HDCP on this Tx (its sink cannot answer) */
    uint8_t   b77;
    gsv_port *next;             /* +0x78 */
    /* storage for the content blocks (the original bump-allocates them from a 0x238-byte pool) */
    struct {
        uint8_t flags[2], st[11];
        gsv_rx_info rx; gsv_tx_status status; gsv_hdcp_rx hdcp; gsv_hdcp_tx hdcp_tx; gsv_video video; gsv_audio audio;
        gsv_scaler scaler; gsv_color color;
    } mem;
};

/* globals the SDK keeps (RAM 0x200002d0.., 0x20000bd0) */
extern gsv_port *g_gsv_port_list, *g_gsv_port_tail;
extern gsv_port *g_chip_state[2];

/* ---- lifecycle ------------------------------------------------------------------------------------------ */
int  gsv_chip_create(gsv_chip *c, uint8_t a, uint8_t b, uint32_t *dev_base, gsv_port *first, uint32_t x); /* 0x08006590 */
void gsv_port_create(gsv_chip *c, gsv_port *p, int16_t id, uint8_t kind);                      /* 0x080065a8 */
int  gsv_chip_init(gsv_chip *c);                                                               /* 0x08006746 */
void gsv_set_i2c_addresses(gsv_chip *c);                                                       /* 0x08008d0c */
void gsv_rx_pll_init(void);                                                                    /* 0x0800f2f4 */
int  gsv_ports_init_all(void);                                                                 /* 0x08006ae4 */
int  gsv_port_link(gsv_port *src, gsv_port *dst, int what);  /* 0 video, 1 audio, 2 both */    /* 0x080065ec */
void gsv_route_set(gsv_port *rx, gsv_port *tx, gsv_port *color, gsv_port *scaler, int mode);   /* 0x08007be8 */
int  gsv_fsm_run(void);                                                                        /* 0x08006b00 */
int  gsv_event(gsv_port *p, int ev);                          /* AvEvent... */                 /* 0x0800785c */
/* depth-first walk of the video tree below root: *cursor = last returned port (NULL to start), *out = next one;
 * returns 0 while there are ports (0x080114f8) */
int  gsv_walk_downstream(gsv_port *root, gsv_port **cursor, gsv_port **out);
int  gsv_walk_audio_downstream(gsv_port *root, gsv_port **cursor, gsv_port **out);   /* 0x0801131c */
gsv_port *gsv_root_source(gsv_port *p);        /* 0x08011562: top of the video chain, NULL if none */
gsv_port *gsv_audio_root_source(gsv_port *p);  /* 0x08011386 */
uint8_t gsv_app_setting(int idx);              /* app settings bytes (0x08012f1c); idx 1 = HDCP mode */

/* ---- device descriptors for a port's register blocks ---------------------------------------------------- */
uint32_t gsv_rx_dev(gsv_port *p);        /* 0x0800ea78  page 0x40/0x50 by idx */
uint32_t gsv_tx_dev(gsv_port *p);        /* 0x0800eabc  page 0x60/0x70 by id */
uint32_t gsv_tx_dev2(gsv_port *p);       /* 0x0800ea58  page 0x62/0x72 by id */
uint32_t gsv_tx_dev42(gsv_port *p);      /* 0x0800eadc  page 0x42/0x52 by idx */
uint32_t gsv_rx_dev21(gsv_port *p);      /* 0x0800e960  page 0x21/0x31 by idx */
uint32_t gsv_rx_dev26(gsv_port *p);      /* 0x0800ea34  page 0x26/0x36 by idx */
uint32_t gsv_rx_dev27(gsv_port *p);      /* 0x0800ea14  page 0x27/0x37 by id */
uint32_t gsv_rx_dev_e984(gsv_port *p);   /* 0x0800e984 */
uint32_t gsv_rx_dev_e9a8(gsv_port *p);   /* 0x0800e9a8 */
uint32_t gsv_rx_dev_e9f0(gsv_port *p);   /* 0x0800e9f0 */
uint32_t gsv_rx_dev_e9cc(gsv_port *p);   /* 0x0800e9cc page 0x25/0x35 */
uint32_t gsv_tx_dev61(gsv_port *p);      /* 0x0800ea9c page 0x61/0x71 */

/* ---- state machines (one call per SDK tick each) -------------------------------------------------------- */
void gsv_rx_plug_fsm_step(gsv_port *p);  /* 0x08010d38 */
void gsv_rx_plug_fsm(gsv_port *p);       /* 0x08006d08 */
void gsv_rx_fsm_step(gsv_port *p);       /* 0x08010e4c */
void gsv_rx_fsm(gsv_port *p);            /* 0x08007444 */
void gsv_rx_hdcp_fsm_step(gsv_port *p);  /* 0x0801204c */
void gsv_tx_fsm_step(gsv_port *p);       /* 0x08010da4 */
int  gsv_tx_plug_fsm(gsv_port *p);       /* 0x0800cc80 */
void gsv_tx_hdcp_fsm(gsv_port *p);       /* 0x08006f48 */
int  gsv_tx_hdcp_poll(gsv_port *p);      /* 0x0800c760 "Tx is Running HDCP" */
void gsv_scaler_fsm_step(gsv_port *p);   /* 0x08010fa8 */
void gsv_color_fsm_step(gsv_port *p);    /* 0x08010e8a */
void gsv_audio_fsm_step(gsv_port *p);    /* 0x08010d2c */
int  gsv_rx_lock_delay(gsv_port *p);     /* 0x0800a0fc "Lock Delay" */
void gsv_rx_eye_width(gsv_port *p, int reset);  /* 0x0800f4b8 */
int  gsv_rx_reset_port(gsv_port *p);     /* 0x08008e38 */
void gsv_rx_disable_hpa(gsv_port *p);    /* 0x0800efd0 */
int  gsv_rx_power_down(gsv_port *p);     /* 0x0800b9e0 */
void gsv_k1_tx_hdmi_mode(gsv_port *p);                         /* Gsv2k1 0x080187f8 */
void gsv_k1_tx_depth_limit(gsv_port *src, gsv_port *q);         /* Gsv2k1 0x08030470 */
void gsv_k1_pll_kick(gsv_port *p);                             /* Gsv2k1 0x08019360 */
void gsv_tx_set_tmds(gsv_port *p, unsigned on);                /* 0x0800fb30 */
int  gsv_tx_hdcp_cipher_disable(gsv_port *p);                  /* 0x0800befc */
void gsv_tx_hdcp_set_type(gsv_port *p, int type, int force);   /* 0x0800f984 */
int  gsv_tx_hdcp_cipher(gsv_port *p);                          /* 0x0800c460 */
int  gsv_intpt_dispatch(gsv_port *p);                          /* 0x08011c08 */
int  gsv_tx_set_colorspace(gsv_port *p, const uint8_t *avi, int log);  /* 0x0800ff44 */
int  gsv_tx_set_colordepth(gsv_port *p);                            /* 0x08010688 */
int  gsv_tx_packet_config(gsv_port *p, int type, const uint8_t *data, int enable);   /* 0x0800db38 */
void gsv_tx_update_mute_regs(gsv_port *p);                          /* 0x0800d968 */
int  gsv_tx_output_disable(gsv_port *p);                            /* 0x0800c0f8 */
int  gsv_tx_reset_core(gsv_port *p);                                /* 0x0800bfa4 */
void gsv_tx_int_config(gsv_port *p, unsigned mask16, int mode);     /* 0x0800c13c */
void gsv_tx_apply_from_source(gsv_port *src, gsv_port *tx, uint8_t *buf, unsigned bit, int type, int audio, int force);  /* 0x08012686 */
int  gsv_rx_write_edid(gsv_port *p, const uint8_t *edid, int len);  /* 0x0800bbc0 */
extern uint8_t gsv_tx_edid[2][256];      /* 0x20000e58: the sink EDIDs read on Tx A / Tx B */
void gsv_rx_set_edid(gsv_port *p, uint8_t *edid);                   /* 0x08010924 "Rx %d Setting Edid Start" */
void gsv_parse_packet(gsv_port *p, int type, uint8_t *pkt);         /* 0x08012778 */
int  gsv_rx_read_packet(gsv_port *p, int type, uint8_t *pkt);       /* 0x08009f24 */

/* application callbacks (weak defaults do nothing) */
void gsv_app_rx_event(int ev, int on);          /* Rx plugged / lost (0x08013a9c) */
void gsv_app_tx_event(int which, int on);       /* Tx downstream connected / disconnected (0x08013d7c) */
void gsv_app_status_set(int which, uint8_t val);/* HDCP status for the app's status report (0x08012f5c) */
uint8_t gsv_app_hdcp_setting(void);             /* app setting byte: HDCP bypass (0x08006038) */

/* debug output of the SDK (the original printed over UART); the application may route it anywhere */
void gsv_log(const char *fmt, ...);
/* settle time after the chip's soft reset (the original busy-waits); the weak default does nothing */
void gsv_delay_ms(unsigned ms);
/* reached a path not yet ported: abort loudly so the replay diff points at it */
void gsv_todo(const char *what, const char *where);
#define GSV_TODO(what) gsv_todo(what, __func__)
