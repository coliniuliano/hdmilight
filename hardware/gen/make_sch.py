"""Draw the schematic: one sheet per block, parts grouped by function and wired, labels only between groups.
The nets still come from design.PARTS; `python3 check_sch.py` proves the drawing matches."""
import csv
import os
from collections import OrderedDict, defaultdict

import design
from sch import G, PROJECT, PWR, ROOT, Sheet, q, uid

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(os.path.join(HERE, ".."))
SHEETS = OrderedDict([("power", "Power"), ("hdmi", "HDMI connectors"), ("gsv2001", "GSV2001 splitter"),
                      ("ms2130", "MS2130 capture"), ("esp32", "ESP32-S31"), ("led", "LED output")])


def sheet_uuid(s):
    return uid("sheet:" + s)


def sym_uuid(ref):
    return uid("sym:" + ref)


# ----------------------------------------------------------------------------- shared patterns
def bus(s, ref, nums, n=2):
    """Join several top or bottom pins of a box with one bar and one supply symbol."""
    pts = [s.out(ref, k, n) for k in nums]
    xs = sorted(p[0] for p in pts)
    y = pts[0][1]
    s.wire((xs[0], y), (xs[-1], y))
    net = s.net(ref, nums[0])
    side = s.pins[(ref, str(nums[0]))][2]
    if net in PWR and (net == "GND") == (side == "D"):
        s.pwr(net, xs[0], y)
    else:
        s.label(net, xs[0], y, side)


def pull(s, ref, x, y):
    """A pull-up or pull-down drawn on its own: rail - resistor - signal label."""
    pins = design.PARTS[ref]["pins"]
    rail = next(n for n in pins.values() if n in PWR)
    other = next(n for n in pins.values() if n != rail)
    if rail == "GND":
        s.label(other, x, y, "L")
        ex, ey = s.series(ref, x, y, left=other)
        s.wire((ex, ey), (ex + G, ey))
        s.pwr("GND", ex + G, ey)
    else:
        s.pwr(rail, x - 5.08, y)
        s.wire((x - 5.08, y), (x, y))
        ex, ey = s.series(ref, x, y, left=rail)
        s.label(other, ex, ey, "R")


def left_series(s, ref, pinref, num, far, gap=5):
    """Part in series on the left of a box pin; the far side gets a rail symbol or a label."""
    px, py = s.P(pinref, num)
    x0 = px - gap * G - 7.62
    s.wire((px, py), (x0 + 7.62, py))
    s.series(ref, x0, py, left=far)
    if far in PWR:
        s.wire((x0, py), (x0 - 5.08, py))
        s.pwr(far, x0 - 5.08, py)
    else:
        s.label(far, x0, py, "L")
    return x0, py


# ----------------------------------------------------------------------------- sheets
def power():
    s = Sheet("power", "Power", "A3")
    s.text("USB-C: 5 V in, and the ESP32's USB-Serial/JTAG for flashing", 20, 22)
    s.box("J3", 45, 62, R=["A4", "A9", "B4", "B9", None, "A5", "B5", None, "A6", "B6", "A7", "B7", None, "A8", "B8"],
          B=["A1", "A12", "B1", "B12", "SH"],
          names={"A4": "VBUS", "A9": "VBUS", "B4": "VBUS", "B9": "VBUS", "A5": "CC1", "B5": "CC2", "A6": "D+", "B6": "D+",
                 "A7": "D-", "B7": "D-", "A8": "SBU1", "B8": "SBU2", "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND",
                 "SH": "SHELL"})
    px, py = s.P("J3", "A4")
    xs = px + 5.08
    for n in ("A4", "A9", "B4", "B9"):
        s.wire(s.P("J3", n), (xs, s.P("J3", n)[1]))
    s.wire((xs, py), (xs, s.P("J3", "B9")[1]))
    s.wire((xs, py), (xs + 7.62, py))
    s.label("VBUS", xs + 2.54, py, "U")
    ex, ey = s.series("F1", xs + 7.62, py, left="VBUS")
    s.wire((ex, ey), (ex + 27.94, ey))
    s.down("C1", ex + 7.62, ey)
    s.down("C2", ex + 17.78, ey)
    s.pwr("+5V", ex + 27.94, ey)
    y5, yb5 = s.P("J3", "A5")[1], s.P("J3", "B5")[1]
    s.wire(s.P("J3", "A5"), (xs + 30.48, y5))
    s.down("R1", xs + 30.48, y5)
    s.wire(s.P("J3", "B5"), (xs + 15.24, yb5))
    s.down("R2", xs + 15.24, yb5)
    for a, b, net in (("A6", "B6", "USBC_DP"), ("A7", "B7", "USBC_DM")):
        ya, yb = s.P("J3", a)[1], s.P("J3", b)[1]
        s.wire(s.P("J3", a), (xs, ya))
        s.wire(s.P("J3", b), (xs, yb), (xs, ya))
        s.label(net, xs, ya, "R")
    bus(s, "J3", ["A1", "A12", "B1", "B12", "SH"])
    s.text("ESD protection on the USB pair", 120, 52, 1.4)
    s.box("U10", 150, 80, L=[1, None, 3], R=[6, None, 4], T=[5], B=[2],
          names={1: "IO1", 2: "GND", 3: "IO2", 4: "IO2", 5: "VBUS", 6: "IO1"})
    for n in (1, 3, 6, 4, 5, 2):
        s.stub("U10", n)

    def buck(x, y, u, cin, l, rtop, rbot, couts, vout, title, en):
        s.text(title, x - 30, y - 16, 1.6)
        s.box(u, x, y, L=[4, None, 1], R=[3, None, 5], B=[2])
        ax, ay = s.P(u, 4)
        s.wire((ax, ay), (ax - 17.78, ay))
        s.pwr("+5V", ax - 17.78, ay)
        s.down(cin, ax - 10.16, ay)
        ex_, ey_ = s.P(u, 1)
        yen = ay + 20.32
        if en == "tie":
            s.wire((ex_, ey_), (ax - 2.54, ey_), (ax - 2.54, ay))
        else:
            s.wire((ex_, ey_), (ex_ - 2.54, ey_), (ex_ - 2.54, yen))
            rail, rpart, extra = en
            s.pwr(rail, ex_ - 33.02, yen)
            s.wire((ex_ - 33.02, yen), (ex_ - 27.94, yen))
            rx, ry = s.series(rpart, ex_ - 27.94, yen, left=rail)
            s.wire((rx, ry), (ex_ - 2.54, yen))
            if extra in design.PARTS:
                s.down(extra, ex_ - 12.7, yen)
            else:
                s.wire((ex_ - 2.54, yen), (ex_ - 2.54, yen + 5.08))
                s.label(extra, ex_ - 2.54, yen + 5.08, "D")
        lx, ly = s.P(u, 3)
        s.wire((lx, ly), (lx + 5.08, ly))
        rx, ry = s.series(l, lx + 5.08, ly, left=s.net(u, 3))
        n = len(couts)
        xr = rx + 12.7 * (n + 1)
        s.wire((rx, ry), (xr + 12.7, ry))
        for i, c in enumerate(couts):
            s.down(c, rx + 12.7 * (i + 1), ry)
        s.two(rtop, xr, ry + 3.81, "v", first=vout)
        s.down(rbot, xr, ry + 7.62)
        s.pwr(vout, xr + 12.7, ry)
        fx, fy = s.P(u, 5)
        yf = ry + 17.78
        s.wire((fx, fy), (fx + 2.54, fy), (fx + 2.54, yf), (xr - 6.35, yf), (xr - 6.35, ry + 7.62), (xr, ry + 7.62))
        s.stub(u, 2)

    buck(215, 45, "U6", "C3", "L1", "R3", "R4", ["C4", "C5"], "+1V2", "1.2 V for the GSV2001 (always on, comes up first)", "tie")
    buck(215, 115, "U5", "C6", "L2", "R5", "R6", ["C7", "C8"], "+3V3",
         "3.3 V (enable delayed by R7/C9 so it never leads 1.2 V)", ("+5V", "R7", "C9"))
    buck(215, 190, "U7", "C10", "L3", "R8", "R9", ["C11"], "+1V1",
         "1.1 V for the MS2130 only (ESP32 pulls MS_PWR_EN low to switch the capture chip's core off)", ("+3V3", "R10", "MS_PWR_EN"))

    s.text("2.5 V for the MS2130's analogue supply", 25, 168, 1.6)
    s.box("U8", 60, 185, L=[3], R=[2], B=[1])
    ax, ay = s.P("U8", 3)
    s.wire((ax, ay), (ax - 15.24, ay))
    s.pwr("+3V3", ax - 15.24, ay)
    s.down("C12", ax - 7.62, ay)
    bx, by = s.P("U8", 2)
    s.wire((bx, by), (bx + 15.24, by))
    s.down("C13", bx + 7.62, by)
    s.pwr("+2V5", bx + 15.24, by)
    s.stub("U8", 1)
    s.tps(30, 255, ["TP1", "TP2", "TP3", "TP4", "TP5", "TP6", "TP7", "TP8", "TP29"])
    s.text("D8 empties C9 as soon as 5 V drops, so a quick re-plug gets the full enable delay again", 100, 150, 1.4)
    s.label("EN_3V3", 110, 158, "L")
    ex, ey = s.series("D8", 110, 158, left="EN_3V3")
    s.wire((ex, ey), (ex + 5.08, ey))
    s.pwr("+5V", ex + 5.08, ey)
    s.text("1.1 V bleed: with MS_PWR_EN low the rail really falls to zero", 100, 228, 1.4)
    s.pwr("+1V1", 110, 236)
    s.wire((110, 236), (120.16, 236))
    s.down("R56", 120.16, 236)
    s.text("Mounting holes", 300, 249, 1.4)
    for i in range(4):
        s.box(f"H{i + 1}", 310 + 22 * i, 262)
    return s


def hdmi():
    s = Sheet("hdmi", "HDMI connectors", "A3")
    order = [1, 3, None, 4, 6, None, 7, 9, None, 10, 12, None, 13, 14, 15, 16, 18, 19]
    gnd = [2, 5, 8, 11, 17, "SH"]
    # ---- input
    s.text("HDMI in (from the source)", 20, 22)
    s.box("J1", 0, 0, R=order, B=gnd, at=(1, 70, 40))
    x = 70
    for d, first in (("D1", 1), ("D2", 7)):
        s.box(d, 0, 0, L=[10, 9, 8, 7, 6], R=[1, 2, 3, 4, 5], at=(10, x + 20.32, s.P("J1", first)[1]))
    for jp, d, dp in ((1, "D1", 10), (3, "D1", 9), (4, "D1", 7), (6, "D1", 6), (7, "D2", 10), (9, "D2", 9), (10, "D2", 7), (12, "D2", 6)):
        s.wire(s.P("J1", jp), s.P(d, dp))
    for d in ("D1", "D2"):
        for n in (1, 2, 4, 5):
            s.stub(d, n, 3)
        s.stub(d, 3, 1)
        s.stub(d, 8, 1)
    s.stub("J1", 13, 4)
    s.stub("J1", 14, 4)
    for pin, r, dx in ((15, "R11", 66.04), (16, "R12", 86.36)):
        px, py = s.P("J1", pin)
        s.wire((px, py), (px + dx + 7.62, py))
        s.up(r, px + dx, py, rail="RX_5V")
        s.label(s.net("J1", pin), px + dx + 7.62, py, "R")
    px, py = s.P("J1", 18)
    s.wire((px, py), (px + 116.84, py))
    s.down("C14", px + 109.22, py)
    s.label("RX_5V", px + 116.84, py, "R")
    px, py = s.P("J1", 19)
    s.wire((px, py), (px + 7.62, py))
    s.label("RX_HPD_C", px + 2.54, py, "D")
    ex, ey = s.series("R13", px + 7.62, py, left="RX_HPD_C")
    s.label("RX_HPD", ex, ey, "R")
    bus(s, "J1", gnd)
    # ---- output
    s.text("HDMI out (to the TV)", 250, 22)
    s.box("J2", 0, 0, L=order, B=gnd, at=(1, 340, 40))
    x = 340
    for d, first in (("D3", 1), ("D4", 7)):
        s.box(d, 0, 0, L=[1, 2, 3, 4, 5], R=[10, 9, 8, 7, 6], at=(10, x - 20.32, s.P("J2", first)[1]))
    for jp, d, dp in ((1, "D3", 10), (3, "D3", 9), (4, "D3", 7), (6, "D3", 6), (7, "D4", 10), (9, "D4", 9), (10, "D4", 7), (12, "D4", 6)):
        s.wire(s.P("J2", jp), s.P(d, dp))
    for d in ("D3", "D4"):
        for n in (1, 2, 4, 5):
            s.stub(d, n, 3)
        s.stub(d, 3, 1)
        s.stub(d, 8, 1)
    s.stub("J2", 13, 4)
    s.stub("J2", 14, 4)
    for pin, r, dx in ((15, "R14", 66.04), (16, "R15", 86.36)):
        px, py = s.P("J2", pin)
        s.wire((px, py), (px - dx - 7.62, py))
        s.up(r, px - dx, py, rail="HDMI_OUT_5V")
        s.label(s.net("J2", pin), px - dx - 7.62, py, "L")
    px, py = s.P("J2", 18)
    s.wire((px, py), (px - 124.46, py))
    s.down("C15", px - 109.22, py)
    s.label("HDMI_OUT_5V", px - 116.84, py, "D")
    s.series("F2", px - 132.08, py, left="+5V")
    s.pwr("+5V", px - 132.08, py)
    px, py = s.P("J2", 19)
    s.wire((px, py), (px - 20.32, py))
    s.down("R16", px - 10.16, py)
    s.label("TXA_HPD", px - 20.32, py, "L")
    bus(s, "J2", gnd)
    # ---- links between the two connectors
    s.text("ARC: the utility line runs straight from the TV side to the source side (R17)", 150, 172, 1.4)
    s.label("RXA_UTIL", 170, 180, "L")
    ex, ey = s.series("R17", 170, 180, left="RXA_UTIL")
    s.label("TXA_UTIL", ex, ey, "R")
    s.text("R55 NOT FITTED: hot-plug pull-up to the source's 5 V, only if the GSV2001's RX_HPD output proves to be open-drain", 150, 156, 1.4)
    s.label("RX_5V", 170, 163, "L")
    ex, ey = s.series("R55", 170, 163, left="RX_5V")
    s.label("RX_HPD_C", ex, ey, "R")
    s.text("eARC experiment: C70 would AC-couple the two hot-plug pins. NOT FITTED, untested.", 150, 194, 1.4)
    s.label("RX_HPD_C", 170, 202, "L")
    ex, ey = s.series("C70", 170, 202, left="RX_HPD_C")
    s.label("TXA_HPD", ex, ey, "R")
    s.text("CEC: one copper bus across both connectors. The ESP32 listens through 1M (CEC_RX) and pulls low through Q1 (CEC_TX);", 100, 218, 1.4)
    s.text("the GSV2001's CEC pin sees the bus only through R18 (100k). Nothing here loads the bus when the board is off.", 100, 222, 1.4)
    s.label("CEC", 150, 245, "L")
    s.wire((150, 245), (210.82, 245))
    s.up("R18", 160.02, 245, rail="GSV_CEC")
    s.box("Q1", 0, 0, L=[1], T=[3], B=[2], at=(3, 185.42, 250.08))
    s.wire((185.42, 245), (185.42, 250.08))
    gx, gy = s.P("Q1", 1)
    s.wire((gx, gy), (gx - 17.78, gy))
    s.label("CEC_TX", gx - 17.78, gy, "L")
    s.down("R19", gx - 7.62, gy)
    s.stub("Q1", 2)
    ex, ey = s.series("R20", 210.82, 245, left="CEC")
    s.label("CEC_RX", ex, ey, "R")
    s.tps(30, 135, ["TP9", "TP10", "TP11", "TP12", "TP13", "TP18"], title="Test pads, input side")
    s.tps(250, 135, ["TP14", "TP15", "TP16", "TP17"], title="Test pads, output side")
    return s


def gsv2001():
    s = Sheet("gsv2001", "GSV2001 splitter", "A2")
    L = [69, 68, 67, 66, 64, 63, 62, 61, None, 76, 73, 74, 75, 72, None, 52, 53, 54, 55, 56, 57, None,
         None, None, None, 3, None, None, 4, None, None, None, None, None, 71]
    R = [35, 34, 33, 32, 30, 29, 28, 27, None, 23, 21, 22, None, 15, 14, 13, 12, 10, 9, 8, 7, None, 17, 18, 19, 20, None,
         44, 45, 46, 47, 48, 49, 50, 51]
    v33 = [1, 26, 60, 38, 41]
    v12 = [5, 37, 43, 58, 6, 11, 16, 25, 31, 36, 39, 40, 42, 59, 65, 70]
    s.box("U2", 300, 215, L=L, R=R, T=v33 + [None] + v12 + [None, 2, None, None, 24], B=[77])
    bus(s, "U2", v33)
    bus(s, "U2", v12)
    s.stub("U2", 2, 3)
    s.stub("U2", 24, 3)
    s.stub("U2", 77)
    for n in L[:8] + L[9:14] + L[15:21]:
        s.stub("U2", n, 3)
    for n in R:
        if n:
            s.stub("U2", n, 3)
    # crystal
    xi, xo = s.P("U2", 3), s.P("U2", 4)
    s.box("Y1", 0, 0, R=[1, None, None, 3], B=[2, 4], names={1: "X1", 3: "X2", 2: "GND", 4: "GND"}, at=(1, xi[0] - 33.02, xi[1]))
    s.wire(xi, s.P("Y1", 1))
    s.wire(xo, s.P("Y1", 3))
    bus(s, "Y1", [2, 4])
    tx = xi[0] - 10.16
    s.wire((tx, xi[1]), (tx, xi[1] - 5.08))
    s.series("C36", tx - 7.62, xi[1] - 5.08, left="GND")
    s.pwr("GND", tx - 7.62, xi[1] - 5.08)
    s.wire((tx, xo[1]), (tx, xo[1] + 5.08))
    s.series("C37", tx - 7.62, xo[1] + 5.08, left="GND")
    s.pwr("GND", tx - 7.62, xo[1] + 5.08)
    s.text("27 MHz crystal", xi[0] - 62, xi[1] - 12, 1.4)
    px, py = s.P("U2", 71)
    s.wire((px, py), (px - 22.86, py))
    s.series("R21", px - 30.48, py, left="GSV_REXT_M")
    s.wire((px - 30.48, py), (px - 43.18, py))
    s.series("R50", px - 50.8, py, left="+3V3")
    s.wire((px - 50.8, py), (px - 55.88, py))
    s.pwr("+3V3", px - 55.88, py)
    s.text("REXT: 400 ohm to 3.3 V, made from two 200 ohm", s.P("U2", 71)[0] - 80, s.P("U2", 71)[1] + 7, 1.4)
    # supply filters and decoupling
    s.bank(60, 50, "GSV_MPLL_PVDD", ["C32", "C33"], feed=("FB1", "+1V2"), title="PLL supply (pin 2)")
    s.bank(60, 90, "GSV_TX_PVDD", ["C34", "C35"], feed=("FB2", "+1V2"), title="TX PLL supply (pin 24)")
    s.bank(430, 50, "+3V3", ["C16", "C17", "C18", "C19", "C20", "C29"], title="3.3 V decoupling (TVDD, DVDDIO)")
    s.bank(430, 90, "+1V2", ["C21", "C22", "C23", "C24", "C25", "C26", "C27", "C28", "C30", "C31"],
           title="1.2 V decoupling (DVDD, RX/TX AVDD)")
    s.text("Control pulls. CSB is undocumented (low = I2C on the sister chip): R27 pulls it down, R26 is NOT FITTED.", 35, 132, 1.4)
    for i, r in enumerate(("R22", "R23", "R25", "R26", "R27", "R24", "R53")):
        pull(s, r, 60, 145 + 12.7 * i)
    s.tps(420, 370, ["TP19", "TP20", "TP21", "TP22", "TP23", "TP24", "TP25", "TP26", "TP27"], pitch=19.05)
    s.text("RX from the HDMI input, TXA to the TV at full rate, TXB (downscaled to 1080p) to the MS2130.", 200, 30, 1.6)
    return s


def ms2130():
    s = Sheet("ms2130", "MS2130 capture", "A2")
    L = [62, 61, 58, 57, 54, 53, 49, 46, None, 26, 27, None, 29, None, 28, None, 45, None, None, None, 21, None, None, 22]
    R = [4, 3, None, 7, None, None, None, 38, 36, 35, 37] + [None] * 14 + [42, None, None, 43, None, None, 9, 11, 13, 14, 44, 63]
    v33, a33, v25, v11, d11 = [1, 16, 31, 33, 41], [2, 5, 8, 48, 52, 56, 60], [20, 23, 47], [6, 10, 12, 24, 51, 55, 59], [18, 39]
    gnd = [15, 17, 19, 25, 30, 32, 34, 40, 50, 64, 65]
    s.box("U3", 290, 215, L=L, R=R, T=v33 + [None] + a33 + [None] + v25 + [None] + v11 + [None] + d11, B=gnd)
    for grp in (v33, a33, v25, v11, d11, gnd):
        bus(s, "U3", grp)
    for n in L[:8] + [26, 27]:
        s.stub("U3", n, 3)
    left_series(s, "R35", "U3", 29, "TXB_HPD")
    left_series(s, "R36", "U3", 28, "+5V", gap=8)
    px, py = s.P("U3", 45)
    s.wire((px, py), (px - 35.56, py))
    s.series("R30", px - 43.18, py, left="MS_REXT_M")
    s.wire((px - 43.18, py), (px - 55.88, py))
    s.series("R51", px - 63.5, py, left="GND")
    s.wire((px - 63.5, py), (px - 66.04, py))
    s.pwr("GND", px - 66.04, py)
    # crystal
    xi, xo = s.P("U3", 21), s.P("U3", 22)
    s.box("Y2", 0, 0, R=[1, None, None, 3], B=[2, 4], names={1: "X1", 3: "X2", 2: "GND", 4: "GND"}, at=(1, xi[0] - 35.56, xi[1]))
    s.wire(xi, s.P("Y2", 1))
    s.wire(xo, s.P("Y2", 3))
    bus(s, "Y2", [2, 4])
    s.two("R28", xi[0] - 7.62, xi[1] + 3.81, "v", first="MS_XTIN")
    tx = xi[0] - 17.78
    s.wire((tx, xi[1]), (tx, xi[1] - 5.08))
    s.series("C63", tx - 7.62, xi[1] - 5.08, left="GND")
    s.pwr("GND", tx - 7.62, xi[1] - 5.08)
    s.wire((tx, xo[1]), (tx, xo[1] + 5.08))
    s.series("C64", tx - 7.62, xo[1] + 5.08, left="GND")
    s.pwr("GND", tx - 7.62, xo[1] + 5.08)
    s.text("24 MHz crystal", xi[0] - 64, xi[1] - 14, 1.4)
    # right side
    s.stub("U3", 4, 3)
    s.stub("U3", 3, 3)
    px, py = s.P("U3", 7)
    s.wire((px, py), (px + 5.08, py))
    s.wire((px + 5.08, py), (px + 15.24, py))
    ex, ey = s.series("R29", px + 15.24, py, left="MS_USB_REXT")
    s.wire((ex, ey), (ex + 2.54, ey))
    s.pwr("GND", ex + 2.54, ey)
    cs = s.P("U3", 38)
    s.box("U4", 0, 0, L=[1, 6, 5, 2], R=[8, 7, 3], B=[4], at=(1, cs[0] + 60.96, cs[1]))
    for a, b in ((38, 1), (36, 6), (35, 5), (37, 2)):
        s.wire(s.P("U3", a), s.P("U4", b))
    bx = s.P("U4", 8)[0] + 5.08
    for n in (8, 7, 3):
        s.wire(s.P("U4", n), (bx, s.P("U4", n)[1]))
    y8 = s.P("U4", 8)[1]
    s.wire((bx, s.P("U4", 3)[1]), (bx, y8 - 5.08))
    s.pwr("+3V3", bx, y8 - 5.08)
    s.wire((bx, y8), (bx + 10.16, y8))
    s.down("C65", bx + 10.16, y8)
    s.stub("U4", 4)
    s.text("Boot flash (holds the MS2130 firmware)", cs[0] + 50, cs[1] - 12, 1.4)
    yb = s.P("U3", 37)[1] + 10.16
    for i, (pin, r) in enumerate(((38, "R45"), (36, "R46"), (35, "R47"), (37, "R48"))):
        x = cs[0] + 10.16 + 10.16 * i
        s.wire((x, s.P("U3", pin)[1]), (x, yb))
        s.down(r, x, yb, to=s.net(r, 2))
    s.text("R45-R48: ESP32 access to the flash for in-circuit programming.", cs[0] + 52, yb + 6, 1.3)
    s.text("The ESP32 keeps FL_* floating while the MS2130 is powered.", cs[0] + 52, yb + 9, 1.3)
    px, py = s.P("U3", 42)
    s.wire((px, py), (px + 5.08, py))
    s.wire((px + 5.08, py), (px + 12.7, py))
    ex, ey = s.series("D5", px + 12.7, py, left="MS_GPIO0")
    s.wire((ex, ey), (ex + 12.7, ey))
    ex, ey = s.series("R31", ex + 12.7, ey, left="MS_LED_A")
    s.wire((ex, ey), (ex + 2.54, ey))
    s.pwr("+3V3", ex + 2.54, ey)
    px, py = s.P("U3", 43)
    s.wire((px, py), (px + 5.08, py))
    s.wire((px + 5.08, py), (px + 12.7, py))
    ex, ey = s.series("R32", px + 12.7, py, left="MS_GPIO1")
    s.wire((ex, ey), (ex + 2.54, ey))
    s.pwr("GND", ex + 2.54, ey)
    s.text("R32: GPIO1 strap, NOT FITTED", px + 30, py + 1, 1.3)
    # supplies
    s.bank(70, 50, "MS_AVDD33", ["C38", "C39", "C40", "C41", "C42", "C43", "C44"], feed=("FB3", "+3V3"),
           title="Analogue 3.3 V, filtered from the 3.3 V rail")
    s.bank(70, 90, "MS_DVDD11", ["C60", "C61", "C62"], feed=("FB4", "+1V1"), title="Digital 1.1 V, filtered from the 1.1 V rail")
    s.bank(420, 50, "+3V3", ["C45", "C46", "C47", "C48", "C49"], title="Digital 3.3 V decoupling")
    s.bank(420, 90, "+1V1", ["C53", "C54", "C55", "C56", "C57", "C58", "C59"], title="Analogue 1.1 V decoupling")
    s.bank(180, 90, "+2V5", ["C50", "C51", "C52"], title="2.5 V decoupling")
    s.text("DDC pull-ups for the link from the GSV2001's output B", 35, 140, 1.4)
    pull(s, "R33", 60, 152)
    pull(s, "R34", 60, 164.7)
    s.text("Hot-plug to the GSV2001 stays low while the MS2130 core is off", 35, 176, 1.4)
    pull(s, "R52", 60, 186)
    s.text("USB plug detect (datasheet option): the ESP32 is the host, so it drives GPIO0 itself through R58.", 40, 200, 1.4)
    s.text("ESP32 pin floating = demo-board wiring (D5 status LED). High = host present, low = absent.", 40, 203.5, 1.4)
    s.label("MS_DET", 60, 211, "L")
    ex, ey = s.series("R58", 60, 211, left="MS_DET")
    s.label("MS_GPIO0", ex, ey, "R")
    s.text("HDMI input comes from the GSV2001's output B (TXB_*), USB 2.0 goes to the ESP32.", 200, 30, 1.6)
    return s


def esp32():
    s = Sheet("esp32", "ESP32-S31", "A3")
    nc = [8, 9, 11, 12, 21, 22, 26, 28, 29, 30, 31, 32, 33, 34, 35, 38, 39] + list(range(42, 54)) + list(range(56, 62))
    L = [3] + [None] * 6 + [27] + [None] * 5 + [13, None, None, 14, None, None, 54, 55, None, None, 37, None, None, 36, None] + nc[:17]
    R = [15, 16, 17, 18, 19, 20, None, 23, 24, None, None, 25, None, None, 10, None, 4, 5, 6, 7, None] + nc[17:]
    s.box("U1", 200, 160, L=L, R=R, T=[2], B=[1, 40, 41], minw=50)
    s.stub("U1", 2)
    bus(s, "U1", [1, 40, 41])
    for pin, r, c, sw, note in ((3, "R37", "C68", "SW1", "Reset"), (27, "R38", None, "SW2", "Boot (hold while resetting to flash)")):
        px, py = s.P("U1", pin)
        s.wire((px, py), (px - 33.02, py))
        s.up(r, px - 10.16, py)
        if c:
            s.down(c, px - 22.86, py)
        s.two(sw, px - 36.83, py, "h", first="GND")
        s.wire((px - 40.64, py), (px - 43.18, py))
        s.pwr("GND", px - 43.18, py)
        s.text(note, px - 100, py + 1.5, 1.4)
    left_series(s, "R39", "U1", 13, "USBC_DM")
    left_series(s, "R40", "U1", 14, "USBC_DP")
    s.stub("U1", 54, 3)
    s.stub("U1", 55, 3)
    left_series(s, "R42", "U1", 37, "DBG_TX")
    s.stub("U1", 36, 3)
    for n in (16, 17, 18, 19, 20, 23, 24, 25, 10, 4, 5, 6, 7, 11):
        s.stub("U1", n, 3)
    px, py = s.P("U1", 15)
    s.wire((px, py), (px + 5.08, py))
    s.wire((px + 5.08, py), (px + 12.7, py))
    ex, ey = s.series("R41", px + 12.7, py, left="ESP_LED")
    s.wire((ex, ey), (ex + 12.7, ey))
    ex, ey = s.series("D6", ex + 12.7, ey, left="ESP_LED_A")
    s.wire((ex, ey), (ex + 2.54, ey))
    s.pwr("GND", ex + 2.54, ey)
    s.bank(300, 45, "+3V3", ["C66", "C67"], title="Module supply")
    s.text("GPIO37 is a strapping pin (JTAG source) with no internal pull: R49 keeps it from floating.", 20, 252, 1.4)
    pull(s, "R49", 40, 262)
    s.text("R57 keeps the console input from floating with nothing plugged into J5.", 150, 252, 1.4)
    pull(s, "R57", 170, 262)
    s.text("Debug header", 318, 148, 1.4)
    s.box("J5", 335, 165, L=[1, 2, 3, 4, 5, 6])
    for n in range(1, 7):
        s.stub("J5", n, 3)
    s.text("USB HS host (DM/DP, pads under the module) goes to the MS2130; IO33/IO34 are USB-Serial/JTAG to the USB-C.", 20, 22, 1.5)
    s.text("FL_* reach the MS2130 boot flash through R45-R48: leave floating unless programming it.", 20, 27, 1.5)
    s.text("IO20-IO25 are SDIO pads on their own supply (3.3 V by default per ESP-IDF, undocumented): only signals that survive 1.8 V are on them.", 20, 32, 1.5)
    return s


def led():
    s = Sheet("led", "LED output", "A4")
    s.text("WS281x data: 3.3 V from the ESP32 buffered to 5 V. The strip has its own supply; only data and ground leave here.", 15, 20, 1.5)
    s.box("U9", 100, 90, L=[2, None, None, 1], R=[4], T=[5], B=[3])
    px, py = s.P("U9", 2)
    s.wire((px, py), (px - 15.24, py))
    s.label("LED_DATA", px - 15.24, py, "L")
    s.down("R54", px - 7.62, py)
    ox, oy = s.P("U9", 1)
    s.wire((ox, oy), (ox - 5.08, oy), (ox - 5.08, oy + 5.08))
    s.pwr("GND", ox - 5.08, oy + 5.08)
    vx, vy = s.out("U9", 5, 2)
    s.pwr("+5V", vx, vy)
    s.wire((vx, vy), (vx + 12.7, vy))
    s.down("C69", vx + 12.7, vy)
    s.stub("U9", 3)
    yx, yy = s.P("U9", 4)
    s.wire((yx, yy), (yx + 5.08, yy))
    nx, ny = s.series("R43", yx + 5.08, yy, left="LED_DATA_5V")
    s.box("J4", 0, 0, L=[2, None, None, 3, None, None, 1], at=(2, nx + 40.64, ny))
    s.wire((nx, ny), s.P("J4", 2))
    s.down("D7", nx + 7.62, ny)
    bx, by = s.P("J4", 3)
    s.wire((bx, by), (bx - 17.78, by))
    s.down("R44", bx - 17.78, by)
    gx, gy = s.P("J4", 1)
    s.wire((gx, gy), (gx - 5.08, gy), (gx - 5.08, gy + 5.08))
    s.pwr("GND", gx - 5.08, gy + 5.08)
    s.text("BI = WS2815 backup data in, held low through R44 (10k, so a mis-wired strip cannot short)", nx + 10, ny - 12, 1.3)
    s.tps(40, 160, ["TP28"])
    return s


def root():
    s = (f'(kicad_sch (version 20231120) (generator "hdmilight_gen") (generator_version "2")\n  (uuid {q(ROOT)})\n  (paper "A4")\n'
         '  (title_block (title "HDMI Light v3") (rev "A"))\n  (lib_symbols)\n')
    s += ('  (text "HDMI in -> GSV2001 -> HDMI out (4K60 to the TV)\\nGSV2001 output B (1080p) -> MS2130 -> USB 2.0 -> ESP32-S31 -> LED data\\n'
          'USB-C: 5 V in and ESP32 programming\\n\\nProject, build and ordering notes: README.md" '
          '(at 30 150 0) (effects (font (size 2 2)) (justify left top)) (uuid ' + q(uid("roottext")) + '))\n')
    for i, (sname, title) in enumerate(SHEETS.items()):
        x, y = 30 + (i % 3) * 80, 40 + (i // 3) * 50
        s += (f'  (sheet (at {x} {y}) (size 60 30) (fields_autoplaced yes) (stroke (width 0.1524) (type solid)) (fill (color 0 0 0 0.0))\n'
              f'    (uuid {q(sheet_uuid(sname))})\n'
              f'    (property "Sheetname" {q(title)} (at {x} {y - 0.7} 0) (effects (font (size 1.27 1.27)) (justify left bottom)))\n'
              f'    (property "Sheetfile" {q(sname + ".kicad_sch")} (at {x} {y + 30.6} 0) (effects (font (size 1.27 1.27)) (justify left top)))\n'
              f'    (instances (project {q(PROJECT)} (path {q("/" + ROOT)} (page {q(i + 2)}))))\n  )\n')
    s += '  (sheet_instances (path "/" (page "1")))\n)\n'
    return s


def build():
    sheets = [power(), hdmi(), gsv2001(), ms2130(), esp32(), led()]
    libs = OrderedDict()
    seen = []
    for s in sheets:
        open(os.path.join(OUT, s.name + ".kicad_sch"), "w").write(s.render())
        seen += s.placed
        for k, v in s.libs.items():
            libs.setdefault(k, v.replace('(symbol "hl:', '(symbol "'))
    missing = [r for r in design.PARTS if r not in seen]
    dup = [r for r in set(seen) if seen.count(r) > 1]
    assert not missing and not dup, ("not drawn:", missing, "drawn twice:", dup)
    open(os.path.join(OUT, PROJECT + ".kicad_sch"), "w").write(root())
    os.makedirs(os.path.join(OUT, "lib"), exist_ok=True)
    open(os.path.join(OUT, "lib", "hl.kicad_sym"), "w").write(
        '(kicad_symbol_lib (version 20231120) (generator "hdmilight_gen")\n' + "".join(libs.values()) + ')\n')
    open(os.path.join(OUT, "sym-lib-table"), "w").write(
        '(sym_lib_table\n  (version 7)\n  (lib (name "hl")(type "KiCad")(uri "${KIPRJMOD}/lib/hl.kicad_sym")(options "")(descr "HDMI Light symbols"))\n)\n')


def bom():
    rows = defaultdict(list)
    for p in design.PARTS.values():
        if p["kind"] in ("TP", "MH"):
            continue
        rows[(p["value"], p["fp"].split(":")[1], p["mpn"], p["lcsc"], "DO NOT FIT" if p["dnp"] else "")].append(p["ref"])
    with open(os.path.join(OUT, "bom.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Qty", "References", "Value", "Footprint", "MPN", "LCSC", "Fit"])
        for (val, fp, mpn, lcsc, dnp), refs in sorted(rows.items(), key=lambda kv: kv[1][0]):
            w.writerow([len(refs), " ".join(refs), val, fp, mpn, lcsc, dnp])


if __name__ == "__main__":
    build()
    bom()
    print("schematic and BOM written to", OUT)
