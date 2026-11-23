"""HDMI Light v3 board: the single source of truth for parts and nets.

Everything else (schematic, PCB) is generated from PARTS.  Pin numbers are the
footprint pad numbers.  Sources for the pinouts:
  GSV2001   Gscoolink product specification Rev 2.0, Table 7
  MS2130    MacroSilicon MS2130_HDMI2USB_DEMO (2022-11-12) netlist + MS2130 datasheet rev B/0 (lcsc.com/datasheet/C5905137.pdf)
  ESP32-S31 ESP32-S31-WROOM-1 datasheet v0.5, Table 3-1 and Figure 11-1
"""
from collections import OrderedDict, defaultdict

PARTS = OrderedDict()
# references given explicitly below; the automatic numbering skips them
RESERVED = {"J1", "J2", "J3", "J4", "J5", "D1", "D2", "D3", "D4", "U1", "U2", "U3", "U4", "U5", "U6", "U7", "U8", "U9", "U10"}
_counters = defaultdict(int)

FP = dict(
    R="Resistor_SMD:R_0603_1608Metric",
    C4="Capacitor_SMD:C_0402_1005Metric",
    C6="Capacitor_SMD:C_0603_1608Metric",
    C8="Capacitor_SMD:C_0805_2012Metric",
    L="Inductor_SMD:L_Changjiang_FNR3012S",
    FB="Inductor_SMD:L_0603_1608Metric",
    LED="LED_SMD:LED_0603_1608Metric",
    F="Fuse:Fuse_1206_3216Metric",
    SOT235="Package_TO_SOT_SMD:SOT-23-5",
    SOT23="Package_TO_SOT_SMD:SOT-23",
    SOT236="Package_TO_SOT_SMD:SOT-23-6",
    XTAL="Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm",
    SW="Button_Switch_SMD:SW_SPST_B3U-1000P",
    HDMI="Connector_Video:HDMI_A_Amphenol_10029449-x01xLF_Horizontal",
    USBC="Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
    TB3="Connector_Phoenix_MSTB:PhoenixContact_MSTBA_2,5_3-G-5,08_1x03_P5.08mm_Horizontal",
    HDR6="Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical",
    ESD="Package_SON:USON-10_2.5x1.0mm_P0.5mm",
    QFN64="Package_DFN_QFN:QFN-64-1EP_9x9mm_P0.5mm_EP5.7x5.7mm",
    QFN76="hdmilight:QFN-76-1EP_9x9mm_P0.4mm_EP6.3x6.3mm",
    ESP="hdmilight:ESP32-S31-WROOM-1",
    SOIC8="Package_SO:SOIC-8_3.9x4.9mm_P1.27mm",
    SOD323="Diode_SMD:D_SOD-323",
    TP="TestPoint:TestPoint_Pad_D1.0mm",
    MH="MountingHole:MountingHole_3.2mm_M3",
)


def part(prefix, value, fp, pins, sheet, mpn="", lcsc="", dnp=False, near=None, kind=None, ref=None, names=None):
    """pins: {pad: net}.  near: (ref, pad) placement hint.  names: {pad: pin name} for IC symbols."""
    if ref is None:
        while True:
            _counters[prefix] += 1
            ref = f"{prefix}{_counters[prefix]}"
            if ref not in PARTS and ref not in RESERVED:
                break
    assert ref not in PARTS, ref
    PARTS[ref] = dict(ref=ref, value=value, fp=FP.get(fp, fp), pins={str(k): v for k, v in pins.items()},
                      sheet=sheet, mpn=mpn, lcsc=lcsc, dnp=dnp, near=near, kind=kind or prefix,
                      names={str(k): v for k, v in (names or {}).items()})
    return ref


def R(value, a, b, sheet, **kw):
    return part("R", value, "R", {1: a, 2: b}, sheet, **kw)


def C(value, a, sheet, b="GND", size="C4", **kw):
    return part("C", value, size, {1: a, 2: b}, sheet, **kw)


def TP(net, sheet, **kw):
    return part("TP", net, "TP", {1: net}, sheet, **kw)


NC = None  # unconnected pad

# --------------------------------------------------------------------------- power
S = "power"
part("J", "USB-C 16P", "USBC", {
    "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND",
    "A4": "VBUS", "A9": "VBUS", "B4": "VBUS", "B9": "VBUS",
    "A5": "CC1", "B5": "CC2", "A6": "USBC_DP", "B6": "USBC_DP", "A7": "USBC_DM", "B7": "USBC_DM",
    "A8": NC, "B8": NC, "SH": "GND"}, S, mpn="TYPE-C-31-M-12", lcsc="C165948", ref="J3", kind="USBC")
R("5.1k", "CC1", "GND", S)
R("5.1k", "CC2", "GND", S)
part("F", "1.5A PTC", "F", {1: "VBUS", 2: "+5V"}, S, mpn="BSMD1206-150-16V", kind="F")
C("22uF", "+5V", S, size="C8")
C("100nF", "+5V", S, size="C6")
part("U", "USBLC6-2SC6", "SOT236", {1: "USBC_DM", 2: "GND", 3: "USBC_DP", 4: "USBC_DP", 5: "VBUS", 6: "USBC_DM"},
     S, mpn="USBLC6-2SC6", lcsc="C7519", ref="U10", kind="USBLC6")


def buck(ref, vout, rtop, rbot, en_net, sheet, cout=2):
    lx, fb = f"LX_{vout[1:]}", f"FB_{vout[1:]}"
    part("U", "SY8089AAAC", "SOT235", {1: en_net, 2: "GND", 3: lx, 4: "+5V", 5: fb}, sheet,
         mpn="SY8089AAAC", lcsc="C78988", ref=ref, kind="BUCK",
         names={1: "EN", 2: "GND", 3: "LX", 4: "IN", 5: "FB"})
    C("10uF", "+5V", sheet, size="C6", near=(ref, 4))
    part("L", "2.2uH", "L", {1: lx, 2: vout}, sheet, mpn="SWPA3015S2R2MT", lcsc="C43389", near=(ref, 3))
    R(rtop, vout, fb, sheet, near=(ref, 5))
    R(rbot, fb, "GND", sheet, near=(ref, 5))
    for _ in range(cout):
        C("22uF", vout, sheet, size="C8", near=(ref, 3))


# 1.2 V first (always on); 3.3 V is held off by an RC on EN so it never leads 1.2 V (GSV2001 Figure 7).
buck("U6", "+1V2", "10k", "10k", "+5V", S)
buck("U5", "+3V3", "100k", "22k", "EN_3V3", S)
R("100k", "+5V", "EN_3V3", S, near=("U5", 1))
C("1uF", "EN_3V3", S, size="C6", near=("U5", 1))
# 1.1 V feeds only the MS2130; the ESP32 can cut it to reset the capture chip.
buck("U7", "+1V1", "10k", "12k", "MS_PWR_EN", S, cout=1)      # 0.6 V x (1 + 10/12) = 1.10 V
R("10k", "+3V3", "MS_PWR_EN", S, near=("U7", 1))
part("U", "XC6206P252MR", "SOT23", {1: "GND", 2: "+2V5", 3: "+3V3"}, S, mpn="XC6206P252MR", lcsc="C545580", ref="U8",
     kind="LDO", names={1: "GND", 2: "VOUT", 3: "VIN"})
C("1uF", "+3V3", S, size="C6", near=("U8", 3))
C("1uF", "+2V5", S, size="C6", near=("U8", 2))
for n in ("+5V", "+3V3", "+1V2", "+1V1", "+2V5", "GND", "GND", "GND"):
    TP(n, S)

# --------------------------------------------------------------------------- HDMI connectors
S = "hdmi"
HDMI_NAMES = {1: "D2+", 2: "D2S", 3: "D2-", 4: "D1+", 5: "D1S", 6: "D1-", 7: "D0+", 8: "D0S", 9: "D0-", 10: "CK+",
              11: "CKS", 12: "CK-", 13: "CEC", 14: "UTIL", 15: "SCL", 16: "SDA", 17: "DGND", 18: "+5V", 19: "HPD",
              "SH": "SHELL"}


def hdmi(ref, pfx, v5, hpd, scl, sda):
    part("J", "HDMI-A", "HDMI", {
        1: f"{pfx}_2P", 2: "GND", 3: f"{pfx}_2N", 4: f"{pfx}_1P", 5: "GND", 6: f"{pfx}_1N",
        7: f"{pfx}_0P", 8: "GND", 9: f"{pfx}_0N", 10: f"{pfx}_CP", 11: "GND", 12: f"{pfx}_CN",
        13: "CEC", 14: f"{pfx}_UTIL", 15: scl, 16: sda, 17: "GND", 18: v5, 19: hpd, "SH": "GND"},
        S, mpn="10029449-111RLF", ref=ref, kind="HDMI", names=HDMI_NAMES)


hdmi("J1", "RXA", "RX_5V", "RX_HPD_C", "RX_SCL", "RX_SDA")      # input, from the source
hdmi("J2", "TXA", "HDMI_OUT_5V", "TXA_HPD", "TXA_SCL", "TXA_SDA")  # output, to the TV


def esd(ref, a, b, near):
    # RClamp0524P flow-through: 1/10, 2/9, 4/7, 5/6 are the same line; 3 and 8 are ground.
    part("D", "RClamp0524P", "ESD", {1: f"{a}P", 10: f"{a}P", 2: f"{a}N", 9: f"{a}N", 3: "GND", 8: "GND",
                                     4: f"{b}P", 7: f"{b}P", 5: f"{b}N", 6: f"{b}N"}, S, mpn="RClamp0524PATCT",
         lcsc="C40960", ref=ref, kind="ESD4", near=near)


esd("D1", "RXA_2", "RXA_1", ("J1", 3))
esd("D2", "RXA_0", "RXA_C", ("J1", 9))
esd("D3", "TXA_2", "TXA_1", ("J2", 3))
esd("D4", "TXA_0", "TXA_C", ("J2", 9))

# input side: sink-style pull-ups off the source's own 5 V
R("47k", "RX_5V", "RX_SCL", S, near=("J1", 15))
R("47k", "RX_5V", "RX_SDA", S, near=("J1", 16))
R("1k", "RX_HPD", "RX_HPD_C", S, near=("J1", 19))
C("1uF", "RX_5V", S, size="C6", near=("J1", 18))
# output side: source pull-ups off our fused 5 V
part("F", "0.5A PTC", "F", {1: "+5V", 2: "HDMI_OUT_5V"}, S, mpn="JK-nSMD050-30", kind="F", near=("J2", 18))
C("100nF", "HDMI_OUT_5V", S, size="C6", near=("J2", 18))
R("1.8k", "HDMI_OUT_5V", "TXA_SCL", S, near=("J2", 15))
R("1.8k", "HDMI_OUT_5V", "TXA_SDA", S, near=("J2", 16))
R("100k", "TXA_HPD", "GND", S, near=("J2", 19))   # revision B: take this to an ESP32 pin through a divider too (it can be 5 V)
# ARC: the utility line runs straight from the TV side to the source side (legacy single-ended ARC).
R("0R", "RXA_UTIL", "TXA_UTIL", S, near=("J2", 14))
# CEC: one copper bus straight across both connectors, so TV and source keep talking with this board unpowered,
# and nothing here may load it then.  The ESP32 listens through 1M (its pin clamp cannot load the bus) and pulls
# low through Q1.  There is no pull-up of our own: the TV and the source each have one, and without a series
# diode ours would load the bus whenever this board is off.  The GSV2001's CEC pin is undocumented, and unknown
# when unpowered, so it reaches the bus only through 100k: that keeps the pin defined (CEC idles high) without
# letting it load the bus.  The ESP32 does all CEC work; to try the chip's own CEC, replace R18 with 0R.
R("100k", "GSV_CEC", "CEC", S)
part("Q", "2N7002", "SOT23", {1: "CEC_TX", 2: "GND", 3: "CEC"}, S, mpn="2N7002", lcsc="C8545", kind="NMOS",
     names={1: "G", 2: "S", 3: "D"})
R("100k", "CEC_TX", "GND", S)
R("1M", "CEC", "CEC_RX", S)
for n in ("CEC", "RX_5V", "RX_HPD", "RX_SCL", "RX_SDA", "HDMI_OUT_5V", "TXA_HPD", "TXA_SCL", "TXA_SDA", "RXA_UTIL"):
    TP(n, S)

# --------------------------------------------------------------------------- GSV2001 splitter
S = "gsv2001"
GSV = OrderedDict([
    (1, ("TVDD", "+3V3")), (2, ("MPLL_PVDD", "GSV_MPLL_PVDD")), (3, ("XTALI", "GSV_XTALI")), (4, ("XTALO", "GSV_XTALO")),
    (5, ("DVDD", "+1V2")), (6, ("TXB_AVDD", "+1V2")), (7, ("TXB_CN", "TXB_CN")), (8, ("TXB_CP", "TXB_CP")),
    (9, ("TXB_0N", "TXB_0N")), (10, ("TXB_0P", "TXB_0P")), (11, ("TXB_AVDD", "+1V2")), (12, ("TXB_1N", "TXB_1N")),
    (13, ("TXB_1P", "TXB_1P")), (14, ("TXB_2N", "TXB_2N")), (15, ("TXB_2P", "TXB_2P")), (16, ("TXB_AVDD", "+1V2")),
    (17, ("TXB_HPD", "TXB_HPD")), (18, ("TXB_SDA", "TXB_SDA")), (19, ("TXB_SCL", "TXB_SCL")),
    (20, ("TXB_UTILITY", "TXB_UTIL")), (21, ("TXA_SDA", "TXA_SDA")), (22, ("TXA_SCL", "TXA_SCL")),
    (23, ("TXA_HPD", "TXA_HPD")), (24, ("TX_PVDD", "GSV_TX_PVDD")), (25, ("TXA_AVDD", "+1V2")), (26, ("TVDD", "+3V3")),
    (27, ("TXA_CN", "TXA_CN")), (28, ("TXA_CP", "TXA_CP")), (29, ("TXA_0N", "TXA_0N")), (30, ("TXA_0P", "TXA_0P")),
    (31, ("TXA_AVDD", "+1V2")), (32, ("TXA_1N", "TXA_1N")), (33, ("TXA_1P", "TXA_1P")), (34, ("TXA_2N", "TXA_2N")),
    (35, ("TXA_2P", "TXA_2P")), (36, ("TXA_AVDD", "+1V2")), (37, ("DVDD", "+1V2")), (38, ("DVDDIO", "+3V3")),
    (39, ("TXA_AVDD", "+1V2")), (40, ("TXA_AVDD", "+1V2")), (41, ("DVDDIO", "+3V3")), (42, ("TXA_AVDD", "+1V2")),
    (43, ("DVDD", "+1V2")), (44, ("AUD_D0", "GSV_AUD_D0")), (45, ("AUD_D1", NC)), (46, ("AUD_D2", NC)),
    (47, ("AUD_D3", NC)), (48, ("AUD_D4", NC)), (49, ("AUD_D5", NC)), (50, ("AUD_SCLK", NC)), (51, ("AUD_MCLK", NC)),
    (52, ("I2C_SDA", "I2C_SDA")), (53, ("I2C_SCL", "I2C_SCL")), (54, ("CSB", "GSV_CSB")), (55, ("RESETB", "GSV_RESETB")),
    (56, ("INT", "GSV_INT")), (57, ("AVMUTE", "GSV_AVMUTE")), (58, ("DVDD", "+1V2")), (59, ("RX_AVDD", "+1V2")),
    (60, ("TVDD", "+3V3")), (61, ("RXA_CN", "RXA_CN")), (62, ("RXA_CP", "RXA_CP")), (63, ("RXA_0N", "RXA_0N")),
    (64, ("RXA_0P", "RXA_0P")), (65, ("RX_AVDD", "+1V2")), (66, ("RXA_1N", "RXA_1N")), (67, ("RXA_1P", "RXA_1P")),
    (68, ("RXA_2N", "RXA_2N")), (69, ("RXA_2P", "RXA_2P")), (70, ("RX_AVDD", "+1V2")), (71, ("REXT400", "GSV_REXT")),
    (72, ("CEC", "GSV_CEC")), (73, ("RX_HPD", "RX_HPD")), (74, ("RX_SDA", "RX_SDA")), (75, ("RX_SCL", "RX_SCL")),
    (76, ("RX_5V", "RX_5V")), (77, ("EP", "GND")),
])
part("U", "GSV2001", "QFN76", {k: v[1] for k, v in GSV.items()}, S, mpn="GSV2001", lcsc="C7422483", ref="U2",
     kind="IC", names={k: v[0] for k, v in GSV.items()})
for p in (1, 26, 60, 38, 41):
    C("100nF", "+3V3", S, near=("U2", p))
# one per pin group; pins 11, 31 and 65 sit between TMDS pairs and reach the 1.2 V plane through their own vias
for p in (5, 16, 25, 36, 39, 42, 58, 70):
    C("100nF", "+1V2", S, near=("U2", p))
C("10uF", "+3V3", S, size="C6", near=("U2", 1))
C("10uF", "+1V2", S, size="C6", near=("U2", 37))
C("10uF", "+1V2", S, size="C6", near=("U2", 65))
part("FB", "120R@100MHz", "FB", {1: "+1V2", 2: "GSV_MPLL_PVDD"}, S, mpn="BLM18PG121SN1D", lcsc="C14709", near=("U2", 2))
C("100nF", "GSV_MPLL_PVDD", S, near=("U2", 2))
C("1uF", "GSV_MPLL_PVDD", S, size="C6", near=("U2", 2))
part("FB", "120R@100MHz", "FB", {1: "+1V2", 2: "GSV_TX_PVDD"}, S, mpn="BLM18PG121SN1D", lcsc="C14709", near=("U2", 24))
C("100nF", "GSV_TX_PVDD", S, near=("U2", 24))
C("1uF", "GSV_TX_PVDD", S, size="C6", near=("U2", 24))
part("Y", "27MHz 12pF", "XTAL", {1: "GSV_XTALI", 2: "GND", 3: "GSV_XTALO", 4: "GND"}, S, mpn="X322527MOB4SI",
     lcsc="C70595", near=("U2", 3), kind="XTAL")
C("15pF", "GSV_XTALI", S, near=("U2", 3))
C("15pF", "GSV_XTALO", S, near=("U2", 4))
R("200R 1%", "GSV_REXT_M", "GSV_REXT", S, near=("U2", 71))    # REXT = 400 ohm: two 200R in series (R21 + R50)
R("4.7k", "+3V3", "I2C_SDA", S)
R("4.7k", "+3V3", "I2C_SCL", S)
R("10k", "GSV_RESETB", "GND", S, near=("U2", 55))
R("10k", "+3V3", "GSV_INT", S, near=("U2", 56))
R("10k", "+3V3", "GSV_CSB", S, dnp=True, near=("U2", 54))
R("10k", "GSV_CSB", "GND", S, near=("U2", 54))
for n in ("GSV_RESETB", "GSV_INT", "GSV_CSB", "GSV_AVMUTE", "GSV_AUD_D0", "TXB_UTIL", "TXB_HPD", "TXB_SCL", "TXB_SDA"):
    TP(n, S)

# --------------------------------------------------------------------------- MS2130 capture
S = "ms2130"
MS = OrderedDict([
    (1, ("DVDD33", "+3V3")), (2, ("AVDD33_USB", "MS_AVDD33")), (3, ("DM", "USB_DM")), (4, ("DP", "USB_DP")),
    (5, ("AVDD33_USB", "MS_AVDD33")), (6, ("AVDD11_USB", "+1V1")), (7, ("USB_REXT", "MS_USB_REXT")),
    (8, ("AVDD33_USB", "MS_AVDD33")), (9, ("SSTXM", NC)), (10, ("AVDD11_USB", "+1V1")), (11, ("SSTXP", NC)),
    (12, ("AVDD11_USB", "+1V1")), (13, ("SSRXM", NC)), (14, ("SSRXP", NC)), (15, ("AVSS", "GND")),
    (16, ("DVDD33", "+3V3")), (17, ("DVSS", "GND")), (18, ("DVDD11", "MS_DVDD11")), (19, ("AVSS", "GND")),
    (20, ("AVDD25", "+2V5")), (21, ("XTIN", "MS_XTIN")), (22, ("XTOUT", "MS_XTOUT")), (23, ("AVDD25", "+2V5")),
    (24, ("AVDD11", "+1V1")), (25, ("AVSS", "GND")), (26, ("RXDDCSCL", "TXB_SCL")), (27, ("RXDDCSDA", "TXB_SDA")),
    (28, ("HDMIRXDET", "MS_RXDET")), (29, ("HDMIRXHPD", "MS_HPD")), (30, ("DVSS", "GND")), (31, ("DVDD33", "+3V3")),
    (32, ("DVSS", "GND")), (33, ("DVDD33", "+3V3")), (34, ("DVSS", "GND")), (35, ("SPIMOSI", "MS_MOSI")),
    (36, ("SPISCLK", "MS_SCLK")), (37, ("SPIMISO", "MS_MISO")), (38, ("SPICS", "MS_CS")), (39, ("DVDD11", "MS_DVDD11")),
    (40, ("DVSS", "GND")), (41, ("DVDD33", "+3V3")), (42, ("GPIO0", "MS_GPIO0")), (43, ("GPIO1", "MS_GPIO1")),
    (44, ("MCU_SEL", NC)), (45, ("HDMIRX_REXT", "MS_RX_REXT")), (46, ("HDMIRXCN", "TXB_CN")), (47, ("AVDD25", "+2V5")),
    (48, ("AVDD33", "MS_AVDD33")), (49, ("HDMIRXCP", "TXB_CP")), (50, ("AVSS", "GND")), (51, ("AVDD11", "+1V1")),
    (52, ("AVDD33", "MS_AVDD33")), (53, ("HDMIRX0N", "TXB_0N")), (54, ("HDMIRX0P", "TXB_0P")), (55, ("AVDD11", "+1V1")),
    (56, ("AVDD33", "MS_AVDD33")), (57, ("HDMIRX1N", "TXB_1N")), (58, ("HDMIRX1P", "TXB_1P")), (59, ("AVDD11", "+1V1")),
    (60, ("AVDD33", "MS_AVDD33")), (61, ("HDMIRX2N", "TXB_2N")), (62, ("HDMIRX2P", "TXB_2P")), (63, ("DVO_D7", NC)),
    (64, ("DVSS", "GND")), (65, ("EP", "GND")),
])
part("U", "MS2130", "QFN64", {k: v[1] for k, v in MS.items()}, S, mpn="MS2130", lcsc="C5905137", ref="U3", kind="IC",
     names={k: v[0] for k, v in MS.items()})
part("FB", "120R@100MHz", "FB", {1: "+3V3", 2: "MS_AVDD33"}, S, mpn="BLM18PG121SN1D", lcsc="C14709", near=("U3", 56))
part("FB", "120R@100MHz", "FB", {1: "+1V1", 2: "MS_DVDD11"}, S, mpn="BLM18PG121SN1D", lcsc="C14709", near=("U3", 39))
for p in (2, 8, 48, 52, 56, 60):
    C("100nF", "MS_AVDD33", S, near=("U3", p))
C("10uF", "MS_AVDD33", S, size="C6", near=("U3", 56))
for p in (1, 16, 31, 33, 41):
    C("100nF", "+3V3", S, near=("U3", p))
for p in (20, 23, 47):
    C("100nF", "+2V5", S, near=("U3", p))
for p in (6, 10, 24, 51, 55, 59):
    C("100nF", "+1V1", S, near=("U3", p))
C("10uF", "+1V1", S, size="C6", near=("U3", 55))
for p in (18, 39):
    C("100nF", "MS_DVDD11", S, near=("U3", p))
C("10uF", "MS_DVDD11", S, size="C6", near=("U3", 39))
part("Y", "24MHz 12pF", "XTAL", {1: "MS_XTIN", 2: "GND", 3: "MS_XTOUT", 4: "GND"}, S, mpn="X322524MOB4SI",
     lcsc="C70590", near=("U3", 21), kind="XTAL")
C("18pF", "MS_XTIN", S, near=("U3", 21))
C("18pF", "MS_XTOUT", S, near=("U3", 22))
R("1M", "MS_XTIN", "MS_XTOUT", S, near=("U3", 21))
R("200R 1%", "MS_USB_REXT", "GND", S, near=("U3", 7))
R("1.5k", "MS_RX_REXT", "MS_REXT_M", S, near=("U3", 45))        # REXT = 1.6k: 1.5k + 100R (R30 + R51)
part("U", "W25Q80DVSNIG", "SOIC8", {1: "MS_CS", 2: "MS_MISO", 3: "+3V3", 4: "GND", 5: "MS_MOSI", 6: "MS_SCLK",
                                     7: "+3V3", 8: "+3V3"}, S, mpn="W25Q80DVSNIG", lcsc="C401655", ref="U4",
     kind="FLASH", names={1: "CS", 2: "DO", 3: "WP", 4: "GND", 5: "DI", 6: "CLK", 7: "HOLD", 8: "VCC"}, near=("U3", 36))
C("100nF", "+3V3", S, near=("U4", 8))
part("D", "LED red", "LED", {1: "MS_GPIO0", 2: "MS_LED_A"}, S, kind="LED", near=("U3", 42))
R("1.5k", "+3V3", "MS_LED_A", S, near=("U3", 42))
R("0R", "MS_GPIO1", "GND", S, dnp=True, near=("U3", 43))
# link to the splitter's second output
R("4.7k", "+3V3", "TXB_SCL", S, near=("U3", 26))
R("4.7k", "+3V3", "TXB_SDA", S, near=("U3", 27))
R("1k", "MS_HPD", "TXB_HPD", S, near=("U3", 29))
R("1k", "+5V", "MS_RXDET", S, near=("U3", 28))

# --------------------------------------------------------------------------- ESP32-S31
S = "esp32"
ESPN = {1: "GND", 2: "3V3", 3: "EN", 4: "IO2", 5: "IO3", 6: "IO4", 7: "IO5", 8: "IO0", 9: "IO1", 10: "IO6", 11: "IO7",
        12: "IO35", 13: "IO33/USB_D-", 14: "IO34/USB_D+", 15: "IO20", 16: "IO21", 17: "IO22", 18: "IO23", 19: "IO24",
        20: "IO25", 21: "IO36", 22: "IO37", 23: "IO38", 24: "IO39", 25: "IO40", 26: "IO60", 27: "IO61", 28: "IO42",
        29: "IO43", 30: "IO44", 31: "IO45", 32: "IO46", 33: "IO47", 34: "IO54", 35: "IO55", 36: "RX0", 37: "TX0",
        38: "IO56", 39: "IO57", 40: "GND", 41: "GND", 42: "IO8", 43: "IO9", 44: "IO10", 45: "IO11", 46: "IO12",
        47: "IO13", 48: "IO14", 49: "IO15", 50: "IO16", 51: "IO17", 52: "IO18", 53: "IO19", 54: "DM", 55: "DP",
        56: "IO48", 57: "IO49", 58: "IO50", 59: "IO51", 60: "IO52", 61: "IO53"}
ESPNET = {1: "GND", 2: "+3V3", 3: "ESP_EN", 40: "GND", 41: "GND",
          13: "ESP_USB_DM", 14: "ESP_USB_DP", 54: "USB_DM", 55: "USB_DP",
          # Pads 15..20 (IO20..IO25) are the SDIO pads: they have their own supply, which can be 3.3 V or 1.8 V and
          # whose default is not documented (ESP-IDF source says 3.3 V).  They carry only signals that still work, or
          # do not matter much, at 1.8 V.  GSV2001 reset and I2C are on IO38..IO40, which are on a plain 3.3 V supply.
          15: "ESP_LED", 16: "CEC_TX", 17: "CEC_RX", 18: "GSV_INT", 19: "GSV_CSB", 20: "MS_PWR_EN",
          23: "GSV_RESETB", 24: "I2C_SCL", 25: "I2C_SDA",
          27: "ESP_BOOT", 36: "ESP_RX0", 37: "ESP_TX0", 10: "LED_DATA",
          4: "FL_MISO", 5: "FL_MOSI", 6: "FL_SCLK", 7: "FL_CS", 22: "ESP_IO37",
          11: "MS_DET"}       # IO7: USB plug-detect for the MS2130 (see R58)
part("U", "ESP32-S31-WROOM-1-N16R16V", "ESP", {k: ESPNET.get(k, NC) for k in ESPN}, S,
     mpn="ESP32-S31-WROOM-1-N16R16V", ref="U1", kind="IC", names=ESPN)
C("22uF", "+3V3", S, size="C8", near=("U1", 2))
C("100nF", "+3V3", S, size="C6", near=("U1", 2))
R("10k", "+3V3", "ESP_EN", S, near=("U1", 3))
C("1uF", "ESP_EN", S, size="C6", near=("U1", 3))
part("SW", "RESET", "SW", {1: "ESP_EN", 2: "GND"}, S, mpn="B3U-1000P", lcsc="C231329", kind="SW")
R("10k", "+3V3", "ESP_BOOT", S, near=("U1", 27))
part("SW", "BOOT", "SW", {1: "ESP_BOOT", 2: "GND"}, S, mpn="B3U-1000P", lcsc="C231329", kind="SW")
R("22R", "USBC_DM", "ESP_USB_DM", S, near=("U1", 13))
R("22R", "USBC_DP", "ESP_USB_DP", S, near=("U1", 14))
part("D", "LED white", "LED", {1: "GND", 2: "ESP_LED_A"}, S, kind="LED")
R("100R", "ESP_LED", "ESP_LED_A", S)
R("470R", "ESP_TX0", "DBG_TX", S, near=("U1", 37))
part("J", "DEBUG", "HDR6", {1: "GND", 2: "+3V3", 3: "DBG_TX", 4: "ESP_RX0", 5: "I2C_SCL", 6: "I2C_SDA"}, S, ref="J5",
     kind="HDR", names={1: "GND", 2: "3V3", 3: "TX", 4: "RX", 5: "SCL", 6: "SDA"})

# --------------------------------------------------------------------------- LED strip output
S = "led"
part("U", "74AHCT1G125", "SOT235", {1: "GND", 2: "LED_DATA", 3: "GND", 4: "LED_DATA_5V", 5: "+5V"}, S,
     mpn="SN74AHCT1G125DBVR", lcsc="C7484", ref="U9", kind="BUF", names={1: "OE", 2: "A", 3: "GND", 4: "Y", 5: "VCC"})
C("100nF", "+5V", S, size="C6", near=("U9", 5))
R("100R", "LED_DATA_5V", "LED_DOUT", S, near=("U9", 4))
part("D", "PESD5V0S1BA", "SOD323", {1: "LED_DOUT", 2: "GND"}, S, mpn="PESD5V0S1BA", lcsc="C5261083", kind="TVS")
R("10k", "LED_BI", "GND", S)
part("J", "LED 3P 5.08", "TB3", {1: "GND", 2: "LED_DOUT", 3: "LED_BI"}, S, mpn="MSTBA 2,5/3-G-5,08", ref="J4",
     kind="TB", names={1: "GND", 2: "DATA", 3: "BI"})
TP("LED_DATA", S)

# --------------------------------------------------------------------------- mechanical
for i in range(1, 5):
    part("H", "M3", "MH", {}, "power", ref=f"H{i}", kind="MH")


# --------------------------------------------------------------------------- later additions
# (kept at the end so that earlier reference designators do not move)
# ESP32 access to the MS2130's boot flash, so it can be written in circuit (first programming and updates).
# The ESP32 must leave these four pins floating whenever the MS2130 is powered (MS_PWR_EN high).
S = "ms2130"
for sig in ("CS", "SCLK", "MOSI", "MISO"):
    R("220R", f"MS_{sig}", f"FL_{sig}", S)
# eARC experiment: eARC is a differential signal on HDMI pins 14 (utility) and 19 (hot-plug).  Pin 14 already runs
# through R17; this capacitor would AC-couple the two hot-plug pins so the pair is complete.  Not fitted: untested,
# and it puts a second path on hot-plug detect.
S = "hdmi"
C("1uF", "RX_HPD_C", S, b="TXA_HPD", size="C6", dnp=True)
# GPIO37 is a strapping pin (JTAG source) with no internal pull; the datasheet says it must not float.
# With the eFuses as shipped it is ignored, so this pull-up only matters if EFUSE_JTAG_SEL_ENABLE is ever burned.
S = "esp32"
R("10k", "+3V3", "ESP_IO37", S, near=("U1", 22))

# The two reference resistors are built from JLCPCB basic values (see R21 and R30 above).
R("200R 1%", "+3V3", "GSV_REXT_M", "gsv2001", near=("U2", 71))
R("100R", "MS_REXT_M", "GND", "ms2130", near=("U3", 45))

# ---- additions from the pre-order design review
# pins that would otherwise float
R("100k", "TXB_HPD", "GND", "ms2130", near=("U3", 29))          # MS2130 hot-plug output while its core is off
R("100k", "TXB_UTIL", "GND", "gsv2001", near=("U2", 20))
R("10k", "LED_DATA", "GND", "led", near=("U9", 2))              # no garbage to the strip before the ESP32 drives it
# HDMI input hot-plug: if the GSV2001's RX_HPD output turns out to be open-drain, fit this (1k to the source's 5 V).
R("1k", "RX_5V", "RX_HPD_C", "hdmi", dnp=True, near=("J1", 19))
# 3V3 must not lead 1V2: this diode empties the enable delay capacitor as soon as 5 V drops, so a quick re-plug
# gets the same delay as a cold start.
part("D", "1N4148WS", "SOD323", {1: "+5V", 2: "EN_3V3"}, "power", mpn="1N4148WS", lcsc="C2128", kind="DIODE",
     near=("U5", 1))
# 1V1 bleed, so switching the MS2130 core off (MS_PWR_EN low) really takes the rail to zero
R("1k", "+1V1", "GND", "power", near=("U7", 3))
R("100k", "ESP_RX0", "+3V3", "esp32", near=("J5", 4))           # console input does not float with nothing plugged in
# MS2130 datasheet, "USB plug detection circuit": a design that powers the chip while no host is attached should tell
# it on GPIO0 (high = host present).  Here the host is the ESP32, so the ESP32 gives that signal itself, through 470R.
# Left floating, GPIO0 is exactly MacroSilicon's demo board (status LED D5), which is what the stock firmware expects.
R("470R", "MS_DET", "MS_GPIO0", "ms2130", near=("U3", 42))
TP("MS_PWR_EN", "power")      # short to ground to hold the MS2130 core off (e.g. to program U4 with a clip)

# --------------------------------------------------------------------------- JLCPCB / LCSC part numbers
# Checked against JLCPCB's assembly library with gen/jlc.py.  "basic" parts carry no feeder fee.
# Not available there: U1 (ESP32-S31-WROOM-1, not in the library) and U2 (GSV2001, listed but out of stock).
JLC = {
    ("R", "0R"): "C21189", ("R", "22R"): "C23345", ("R", "100R"): "C22775", ("R", "200R 1%"): "C8218",
    ("R", "220R"): "C22962", ("R", "470R"): "C23179", ("R", "1k"): "C21190", ("R", "1.5k"): "C22843",
    ("R", "1.8k"): "C4177", ("R", "4.7k"): "C23162", ("R", "5.1k"): "C23186", ("R", "10k"): "C25804",
    ("R", "12k"): "C22790", ("R", "22k"): "C31850", ("R", "27k"): "C22967", ("R", "47k"): "C25819",
    ("R", "100k"): "C25803", ("R", "1M"): "C22935",
    ("C4", "100nF"): "C1525", ("C4", "15pF"): "C1548", ("C4", "18pF"): "C1549",
    ("C6", "100nF"): "C14663", ("C6", "1uF"): "C15849", ("C6", "10uF"): "C19702", ("C8", "22uF"): "C45783",
    ("FB", "120R@100MHz"): "C14709", ("L", "2.2uH"): "C43389",
    ("F", "1.5A PTC"): "C883133", ("F", "0.5A PTC"): "C720075",
    ("LED", "LED red"): "C2286", ("LED", "LED white"): "C2290",
    ("XTAL", "27MHz 12pF"): "C70595", ("XTAL", "24MHz 12pF"): "C70590",
    ("ESD4", "RClamp0524P"): "C40960", ("DIODE", "1N4148WS"): "C2128", ("TVS", "PESD5V0S1BA"): "C5261083",
    ("USBC", "USB-C 16P"): "C165948", ("HDMI", "HDMI-A"): "C427307", ("USBLC6", "USBLC6-2SC6"): "C7519",
    ("BUCK", "SY8089AAAC"): "C78988", ("LDO", "XC6206P252MR"): "C545580", ("NMOS", "2N7002"): "C8545",
    ("IC", "MS2130"): "C5905137", ("FLASH", "W25Q80DVSNIG"): "C401655", ("SW", "RESET"): "C231329",
    ("SW", "BOOT"): "C231329", ("BUF", "74AHCT1G125"): "C7484",
    ("TB", "LED 3P 5.08"): "C441205", ("HDR", "DEBUG"): "C37208",
}
BASIC = {"C21189", "C23345", "C22775", "C8218", "C22962", "C23179", "C21190", "C22843", "C4177", "C23162", "C23186",
         "C25804", "C22790", "C31850", "C22967", "C25819", "C25803", "C22935", "C1525", "C1548", "C1549", "C14663",
         "C15849", "C19702", "C45783", "C14709", "C2286", "C2290", "C8545", "C2128"}
NOT_ASSEMBLED = {"U1": "not in JLCPCB's library: fit by hand with hot air",
                 "U2": "GSV2001 is out of stock at LCSC/JLCPCB: fit by hand with hot air",
                 "J4": "through-hole header KF2EDGR-5.08-3P: solder by hand; the mating plug is KF2EDGK-5.08-3P",
                 "J5": "through-hole: solder by hand"}
# Parts JLCPCB could fit but that you would rather solder yourself (to save an extended-part fee): ref -> note.
# They leave the JLCPCB BOM and placement file, are listed in fab/parts_not_assembled.csv, and keep their paste.
HAND_FIT = {}
_SIZE = {"Capacitor_SMD:C_0402_1005Metric": "C4", "Capacitor_SMD:C_0603_1608Metric": "C6", "Capacitor_SMD:C_0805_2012Metric": "C8"}
for _p in PARTS.values():
    _k = _SIZE.get(_p["fp"], _p["kind"])
    _p["lcsc"] = JLC.get((_k, _p["value"]), "" if _p["kind"] in ("TP", "MH") or _p["ref"] in ("U1", "U2") else _p["lcsc"])


def nets():
    out = defaultdict(list)
    for p in PARTS.values():
        for pad, net in p["pins"].items():
            if net:
                out[net].append((p["ref"], pad))
    return out


if __name__ == "__main__":
    n = nets()
    print(len(PARTS), "parts,", len(n), "nets")
    for name, pins in sorted(n.items()):
        if len(pins) < 2:
            print("  single-pin net:", name, pins)
