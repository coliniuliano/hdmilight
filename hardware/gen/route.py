"""Routing.  High-speed pairs are drawn explicitly; the slow nets go through router.Grid, one net at a time.

Layers: F = signals + parts, In1 = solid ground, In2 = power planes (3V3 with 1V2 / 1V1 / 5V islands), B = signals.
HDMI pairs: 0.20 mm tracks on a 0.40 mm pitch (about 100 ohm over In1 on JLC04161H-7628).
USB pair:   0.25 mm tracks on a 0.40 mm pitch (about 90 ohm).
"""
import math

import pcbnew

import kb
import place
import router
from router import RES

W_HDMI, W_USB, W_SIG, W_PWR = 0.20, 0.25, 0.15, 0.20
VIA = (0.5, 0.3)
PLANE_NETS = ("GND", "+3V3", "+1V2", "+1V1", "+5V")

# In2 power islands as (net, x1, y1, x2, y2); anything else on In2 is +3V3.  Later entries win.
IN2 = [
    ("+5V", 0.6, 26.9, 21.0, 33.6), ("+5V", 0.6, 33.2, 16.6, 59.4),          # west: USB-C entry and the bucks
    ("+5V", 41.0, 49.4, 50.6, 59.4),                                         # 1V1 buck input (fed by a track on B)
    ("+5V", 50.2, 57.6, 62.0, 59.4), ("+5V", 60.4, 27.6, 62.0, 59.4),        # ... on round the south-east corner and
    ("+5V", 58.9, 27.6, 62.0, 29.6),                                         # up to the HDMI-out fuse and LED buffer
    ("+1V2", 9.0, 24.5, 36.6, 26.6), ("+1V2", 33.6, 14.1, 44.4, 24.9),       # buck -> GSV2001
    ("+1V2", 9.0, 26.2, 11.5, 31.4),                                         # ... down to the buck's inductor
    ("+1V2", 30.5, 22.5, 33.9, 24.9),      # corner between strip and chip island: a wider neck, and no 3V3 crumbs
    ("+1V2", 42.3, 24.5, 49.6, 28.3),                                        # TX PLL filter and caps
    ("+1V1", 43.0, 34.85, 55.75, 43.25), ("+1V1", 51.4, 42.8, 54.2, 46.6),   # MS2130 and its feed
    ("+1V1", 50.9, 46.2, 57.6, 56.6), ("+1V1", 57.2, 49.3, 59.2, 51.5),
]
# pours on B: net -> rectangle.  MS_AVDD33 lives under the MS2130 so every analogue 3V3 pin can drop into it.
POURS = {}
ANT = (place.U1X - 10.0, place.U1Y + 16.37 - 6.0 - 0.6, place.U1X + 10.0, place.H)   # antenna keep-out, all layers


def plane_net(x, y):
    n = "+3V3"
    for net, x1, y1, x2, y2 in IN2:
        if x1 + 0.45 <= x <= x2 - 0.45 and y1 + 0.45 <= y <= y2 - 0.45:
            n = net
        elif x1 - 0.45 < x < x2 + 0.45 and y1 - 0.45 < y < y2 + 0.45:
            n = None            # too close to an island edge for a via
    return n


class R:
    """Board + grid, kept in step."""

    def __init__(self, B):
        self.B = B
        self.G = router.Grid(place.W, place.H)
        self.padcells = {}      # (layer, cell index) -> pad centre
        self.padlist = {}       # net -> [(ref, num, (x, y), layers, cells)]
        self.pre = set()        # (ref, pad) already tied to its plane by explicit geometry
        self.viapos = set()
        self.extra = {}         # net -> grid cells of hand-drawn copper that later pads may join
        self._raster_pads()
        G = self.G
        m = 0.35
        for l in (0, 1):
            G.block(-1, -1, place.W + 1, m)
            G.block(-1, place.H - m, place.W + 1, place.H + 1)
            G.block(-1, -1, m, place.H + 1)
            G.block(place.W - m, -1, place.W + 1, place.H + 1)
        G.block(*ANT)
        nx1, ny1, nx2, ny2 = place.NOTCH                # the antenna cut-out is a board edge too
        G.block(nx1 - m, ny1 - m, nx2 + m, ny2 + 1)

    def _raster_pads(self):
        G, B = self.G, self.B
        for ref, fp in B.fps.items():
            for pad in fp.Pads():
                bb = pad.GetBoundingBox()
                x1, y1, x2, y2 = (kb.ToMM(v) for v in (bb.GetLeft(), bb.GetTop(), bb.GetRight(), bb.GetBottom()))
                c = pad.GetPosition()
                cx, cy = kb.ToMM(c.x), kb.ToMM(c.y)
                name = pad.GetNetname()
                th = pad.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH)
                layers = (0, 1) if th else (0,)
                if not pad.IsOnLayer(pcbnew.F_Cu) and not th:
                    continue        # paste-only apertures
                net = G.nid(name) if name else -1
                if th and pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                    G.block(x1 - 0.3, y1 - 0.3, x2 + 0.3, y2 + 0.3)
                    continue
                G.rect(layers, x1, y1, x2, y2, net, via_block=True)
                if name:
                    cells = []
                    for l in layers:
                        for iy in range(int(math.ceil((y1 + 0.04) / RES)), int(math.floor((y2 - 0.04) / RES)) + 1):
                            for ix in range(int(math.ceil((x1 + 0.04) / RES)), int(math.floor((x2 - 0.04) / RES)) + 1):
                                cells.append((l, ix, iy))
                                self.padcells[(l, iy * G.nx + ix)] = (cx, cy)
                    if not cells:   # pad narrower than the grid: use the nearest cell
                        ix, iy = G.cell(cx, cy)
                        cells = [(l, ix, iy) for l in layers]
                        for l in layers:
                            self.padcells[(l, iy * G.nx + ix)] = (cx, cy)
                    self.padlist.setdefault(name, []).append((ref, pad.GetNumber(), (cx, cy), layers, cells, th))
        # mounting holes
        for ref in ("H1", "H2", "H3", "H4"):
            c = B.fps[ref].GetPosition()
            G.block(kb.ToMM(c.x) - 3.3, kb.ToMM(c.y) - 3.3, kb.ToMM(c.x) + 3.3, kb.ToMM(c.y) + 3.3)

    # ---- drawing (board and grid together)
    def track(self, pts, net, w, layer="F"):
        self.B.track(pts, net, w, layer)
        l = 0 if layer == "F" else 1
        n = self.G.nid(net)
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            self.G.seg(l, x1, y1, x2, y2, w / 2, n)

    def via(self, x, y, net, d=VIA[0], h=VIA[1]):
        self.viapos.add((round(x, 2), round(y, 2)))
        ix, iy = self.G.cell(x, y)
        if abs(ix * RES - x) < 1e-6 and abs(iy * RES - y) < 1e-6:
            self.G.ownvia.setdefault(self.G.nid(net), set()).add(iy * self.G.nx + ix)
        self.B.via(x, y, net, d, h)
        self.G.circle((0, 1), x, y, d / 2, self.G.nid(net))

    def trunk(self, pts, net, w, layer="B"):
        """A hand-drawn track on grid points that the path-finder may connect to anywhere along its length."""
        self.track(pts, net, w, layer)
        l = 0 if layer == "F" else 1
        cells = self.extra.setdefault(net, set())
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            n = int(round(max(abs(x2 - x1), abs(y2 - y1)) / RES))
            for k in range(n + 1):
                ix, iy = self.G.cell(x1 + (x2 - x1) * k / max(n, 1), y1 + (y2 - y1) * k / max(n, 1))
                cells.add((l, iy * self.G.nx + ix))

    def reserve(self, pts, net, w, layer="F"):
        """Keep a corridor for a net without drawing copper: the path-finder will use it, others cannot."""
        l = 0 if layer == "F" else 1
        n = self.G.nid(net)
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            self.G.seg(l, x1, y1, x2, y2, w / 2, n, copper=False)

    def gnd_via(self, x, y):
        self.via(x, y, "GND")


def jog(x0, y0, x1, y1, xj, early=0.0):
    """Horizontal run from (x0, y0) to (x1, y1) with one 45-degree jog starting at xj (in travel direction)."""
    d = 1 if x1 > x0 else -1
    dy = y1 - y0
    xs = xj - d * early
    if abs(dy) < 1e-6:
        return [(x0, y0), (x1, y1)]
    return [(x0, y0), (xs, y0), (xs + d * abs(dy), y1), (x1, y1)]


def hdmi_port(r, conn, esd_hi, esd_lo, chip, pins, xj_conn, xj_chip, pfx):
    """One HDMI port: connector pad -> ESD (straight through the part) -> chip pad, for the four pairs."""
    B = r.B
    cpin = {"2P": 1, "2N": 3, "1P": 4, "1N": 6, "0P": 7, "0N": 9, "CP": 10, "CN": 12}
    for name in ("2", "1", "0", "C"):
        esd = esd_hi if name in ("2", "1") else esd_lo
        ys = {}
        for pol in "PN":
            net = f"{pfx}_{name}{pol}"
            pads = [(n, B.pad(esd, n)) for n in range(1, 11) if B.padnet(esd, n) == net]
            cx, cy = B.pad(conn, cpin[name + pol])
            # ESD pad nearest the connector, and the one nearest the chip
            pads.sort(key=lambda t: abs(t[1][0] - cx))
            (ea, eb) = pads[0][1], pads[1][1]
            ux, uy = B.pad(chip, pins[name + pol])
            ys[pol] = (net, (cx, cy), ea, eb, (ux, uy))
        for pol in "PN":
            net, c, ea, eb, u = ys[pol]
            other = ys["N" if pol == "P" else "P"]
            # connector -> ESD: the pair closes from 1.0 mm to 0.5 mm pitch
            r.track(jog(c[0], c[1], ea[0], ea[1], xj_conn), net, W_HDMI)
            r.track([ea, eb], net, W_HDMI)
            # ESD -> chip: one jog near the chip; the line on the outside of the bend starts 0.1 mm early
            dy = u[1] - eb[1]
            outer = (dy > 0) == (eb[1] > other[3][1])
            r.track(jog(eb[0], eb[1], u[0], u[1], xj_chip, 0.1 if outer else 0.0), net, W_HDMI)


CP_EXTRA = 1.35     # mm; each mm adds 1.41 mm to TXB_CP


def txb(r):
    """Splitter output B to the MS2130.  The two QFNs face the same way round, so the eight lines cannot reach the
    MS2130 in order on one layer: they drop to B at U2, run south as a bus and come back up one row at a time."""
    B = r.B
    v1 = {"CN": (37.3, 25.5), "CP": (38.0, 25.5), "0N": (38.45, 26.5), "0P": (39.15, 26.5),
          "1N": (39.85, 26.5), "1P": (40.55, 26.5), "2N": (40.75, 25.5), "2P": (41.45, 25.5)}
    lane = {"CN": 37.45, "CP": 37.85, "0N": 38.6, "0P": 39.0, "1N": 40.0, "1P": 40.4, "2N": 41.3, "2P": 41.7}
    u2 = {"CN": 7, "CP": 8, "0N": 9, "0P": 10, "1N": 12, "1P": 13, "2N": 14, "2P": 15}
    u3 = {"CN": 46, "CP": 49, "0N": 53, "0P": 54, "1N": 57, "1P": 58, "2N": 61, "2P": 62}
    for k in u2:
        net = "TXB_" + k
        px, py = B.pad("U2", u2[k])
        vx, vy = v1[k]
        d = abs(vx - px)
        r.track([(px, py), (px, vy - 0.15 - d), (vx, vy - 0.15), (vx, vy)], net, W_HDMI)
        r.via(vx, vy, net)
        lx = lane[k]
        d = abs(lx - vx)
        tx, ty = B.pad("U3", u3[k])
        if k == "CN":
            # clock N: its pad is round the corner on the north side of U3
            ex, ey = 37.05, 33.9
            r.track([(vx, vy), (vx, vy + 0.4), (lx, vy + 0.4 + d), (lx, ey - 0.8), (ex, ey - 0.4), (ex, ey)], net,
                    W_HDMI, "B")
            r.via(ex, ey, net)
            r.track([(ex, ey), (tx - 0.3, ey), (tx, ey + 0.3), (tx, ty)], net, W_HDMI)
            continue
        if k == "CP":
            # clock P: its pad is the first one round the corner from clock N's, which makes this half 1.9 mm
            # shorter.  Run the lane CP_EXTRA further south and come back up at 45 degrees to make the two equal.
            ey = ty + CP_EXTRA
            r.track([(vx, vy), (vx, vy + 0.4), (lx, vy + 0.4 + d), (lx, ey)], net, W_HDMI, "B")
            r.via(lx, ey, net)
            r.track([(lx, ey), (lx + CP_EXTRA, ty), (tx, ty)], net, W_USB)
            continue
        if k.endswith("N"):
            ex, ey = lx - 0.2, ty
        else:
            ex, ey = lx + 0.2, ty + 0.15
        dd = abs(ex - lx)
        r.track([(vx, vy), (vx, vy + 0.4), (lx, vy + 0.4 + d), (lx, ey - 0.5 - dd), (ex, ey - 0.5), (ex, ey)], net,
                W_HDMI, "B")
        r.via(ex, ey, net)
        if ey != ty:
            r.track([(ex, ey), (ex + 0.15, ty), (tx, ty)], net, W_USB)
        else:
            r.track([(ex, ey), (tx, ty)], net, W_USB)
    for x, y in ((36.5, 26.3), (42.3, 27.6), (36.3, 33.2)):
        r.gnd_via(x, y)


def usb_hs(r):
    """ESP32-S31 high-speed USB to the MS2130: down inside the module's pad ring, east on B, up at U3."""
    B = r.B
    dm, dp = B.pad("U1", 54), B.pad("U1", 55)
    vx = dm[0] + 1.7
    for p, net in ((dm, "USB_DM"), (dp, "USB_DP")):
        r.track([p, (vx, p[1])], net, W_USB)
        r.via(vx, p[1], net)
    m3, m4 = B.pad("U3", 3), B.pad("U3", 4)
    vm, vp = (m3[0] - 0.5, 45.3), (m4[0] + 0.3, 44.6)
    ym, yp = 45.0, 44.6
    # B layer: both lines jog south together, then run east
    r.track([(vx, dm[1]), (vx + 0.8, dm[1]), (vx + 0.8 + (ym - dm[1]), ym), (vm[0] - 0.3, ym), vm], "USB_DM", W_USB, "B")
    x0 = vx + 0.8 - 0.22
    r.track([(vx, dp[1]), (x0, dp[1]), (x0 + (yp - dp[1]), yp), vp], "USB_DP", W_USB, "B")
    r.via(*vm, "USB_DM")
    r.via(*vp, "USB_DP")
    r.track([m3, (m3[0], 44.3), (vm[0], 44.8), vm], "USB_DM", W_USB)
    r.track([m4, (m4[0], 44.15), (vp[0], 44.45), vp], "USB_DP", W_USB)
    for x, y in ((vx, dp[1] - 1.0), (vx, dm[1] + 1.0), (47.6, 46.4)):
        r.gnd_via(x, y)


def u2_power(r):
    """GSV2001: 1V2 pins drop straight into the 1V2 island through vias between the pads and the exposed pad;
    the exposed pad gets nine ground vias."""
    B = r.B
    cx, cy = place.U2X, place.U2Y
    groups = [(58, 59), (65,), (70,), (5, 6), (11,), (16,), (25,), (31,), (36, 37), (39, 40), (42, 43)]
    for g in groups:
        pts = [B.pad("U2", n) for n in g]
        mx, my = sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts)
        # move 0.9 mm from the pad row towards the chip centre
        if abs(mx - cx) > abs(my - cy):
            vx, vy = mx + (0.9 if mx < cx else -0.9), my
        else:
            vx, vy = mx, my + (0.9 if my < cy else -0.9)
        for p in pts:
            if abs(mx - cx) > abs(my - cy):
                r.track([p, (p[0] + (0.45 if mx < cx else -0.45), p[1]), (vx, vy)], "+1V2", 0.2)
            else:
                r.track([p, (p[0], p[1] + (0.45 if my < cy else -0.45)), (vx, vy)], "+1V2", 0.2)
        r.via(vx, vy, "+1V2")
        r.pre.update(("U2", str(n)) for n in g)
    for dx in (-2, 0, 2):
        for dy in (-2, 0, 2):
            r.via(cx + dx, cy + dy, "GND", 0.6, 0.3)
    for dx in (-1.8, 0, 1.8):
        for dy in (-1.8, 0, 1.8):
            r.via(place.U3X + dx, place.U3Y + dy, "GND", 0.6, 0.3)
    r.pre.update({("U2", "77"), ("U3", "65")})
    u3_power(r)


def u3_power(r):
    """MS2130 pins that never leave the package outline.

    Ground pins run straight into the exposed pad; the ones at the corners, which would miss it, are joined
    diagonally (15+17, 32+34) or taken to a via outside (50, 64).
    Supply pins drop through 0.45 mm vias between the pad row and the exposed pad, staggered on two radii so that
    neighbours on different rails keep their clearance.  Inner: 1V1 into its In2 island.  Outer: analogue 3V3 into
    the pour on B; 2V5 and digital 1V1 are picked up by tracks on B.
    """
    B = r.B
    cx, cy = place.U3X, place.U3Y

    def inward(n, rad):
        px, py = B.pad("U3", n)
        if abs(px - cx) > abs(py - cy):
            return (px, py), (cx + (rad if px > cx else -rad), py)
        return (px, py), (px, cy + (rad if py > cy else -rad))

    for n in (19, 25, 30, 40):
        p, q = inward(n, 2.6)
        r.track([p, q], "GND", 0.2)
    p15, p17, p32, p34 = (B.pad("U3", n) for n in (15, 17, 32, 34))
    r.track([p15, (p15[0], p15[1] - 0.5), (cx + 2.65, cy + 3.35), (cx + 2.65, cy + 2.6)], "GND", 0.2)
    r.track([p17, (p15[0] - 0.2, p17[1])], "GND", 0.2)
    r.track([p34, (p34[0], p34[1] + 0.5), (cx + 2.65, cy - 3.35), (cx + 2.65, cy - 2.6)], "GND", 0.2)
    r.track([p32, (p34[0] - 0.2, p32[1])], "GND", 0.2)
    p50, p64 = B.pad("U3", 50), B.pad("U3", 64)
    r.track([p50, (44.9, p50[1]), (44.6, p50[1] + 0.3), (44.2, p50[1] + 0.3)], "GND", 0.15)
    r.via(44.2, p50[1] + 0.3, "GND")
    r.track([p64, (45.6, p64[1])], "GND", 0.2)
    r.via(45.6, p64[1], "GND")
    r.pre.update(("U3", str(n)) for n in (15, 17, 19, 25, 30, 32, 34, 40, 50, 64))

    inner = {6: "+1V1", 10: "+1V1", 12: "+1V1", 24: "+1V1", 51: "+1V1", 55: "+1V1", 59: "+1V1"}
    outer = {2: "MS_AVDD33", 5: "MS_AVDD33", 8: "MS_AVDD33", 52: "MS_AVDD33", 56: "MS_AVDD33", 60: "MS_AVDD33",
             20: "+2V5", 23: "+2V5", 18: "MS_DVDD11", 39: "MS_DVDD11"}
    for table, rad in ((inner, 3.25), (outer, 3.65)):
        for n, net in table.items():
            p, q = inward(n, rad)
            r.track([p, q], net, 0.2)
            r.via(q[0], q[1], net, 0.45, 0.2)
            if net in ("+1V1", "MS_AVDD33"):
                r.pre.add(("U3", str(n)))
    # pin 48 (analogue 3V3) is too close to the corner for a via: it runs to its capacitor instead
    r.track([B.pad("U3", 48), B.pad("C40", 1)], "MS_AVDD33", 0.2)
    # pin 47 (2V5) is boxed in by the clock stub outside and pin 51's via inside: its via sits off-axis
    p47 = B.pad("U3", 47)
    v47 = (round(p47[0] + 0.4, 1), round(cy - 3.25, 1))
    r.track([p47, (p47[0], v47[1] - 0.4), v47], "+2V5", 0.2)
    r.via(v47[0], v47[1], "+2V5", 0.45, 0.2)


def buck_nodes(r):
    """Switch nodes: one short fat track from the regulator's LX pin into the inductor pad."""
    for u, l, net in (("U6", "L1", "LX_1V2"), ("U5", "L2", "LX_3V3"), ("U7", "L3", "LX_1V1")):
        a, b = r.B.pad(u, 3), r.B.pad(l, 1)
        r.track([a, (b[0], a[1])], net, 0.5)


def u2_north(r):
    """GSV2001 control pins fan out to their test pads and the debug header without crossing."""
    B = r.B
    for i, (pin, tp) in enumerate(((57, "TP22"), (56, "TP20"), (55, "TP19"), (54, "TP21"))):
        net = B.padnet("U2", pin)
        px, py = B.pad("U2", pin)
        tx, ty = B.pad(tp, 1)
        y0 = 13.9 - 0.4 * i
        d = px - tx
        r.track([(px, py), (px, y0), (tx, y0 - d), (tx, ty)], net, W_SIG)
    for pin, hdr, y0 in ((53, 5, 11.5), (52, 6, 12.3)):
        net = B.padnet("U2", pin)
        px, py = B.pad("U2", pin)
        tx, ty = B.pad("J5", hdr)
        r.track([(px, py), (px, y0), (tx, y0 - (tx - px)), (tx, ty)], net, W_SIG)
    r.track([B.pad("U2", 44), B.pad("TP23", 1)], "GSV_AUD_D0", W_SIG)
    # TXB HPD / DDC leave the south-east corner as a fan, clear of the TXB via cluster
    for pin, yt in ((19, 24.65), (18, 24.95), (17, 25.05)):
        px, py = B.pad("U2", pin)
        r.reserve([(px, py), (px, yt), (43.6, yt + 43.6 - px)], B.padnet("U2", pin), W_SIG)


def escapes(r):
    """Short straight stubs out of every routed QFN pin, so a neighbour's track cannot fence a pin in."""
    B = r.B
    skip2 = {5, 6, 11, 16, 25, 31, 36, 37, 39, 40, 42, 43, 58, 59, 65, 70, 77, 17, 18, 19}
    skip3 = {2, 5, 6, 8, 10, 12, 18, 20, 23, 24, 39, 47, 48, 51, 52, 55, 56, 59, 60, 15, 17, 19, 25, 30, 32, 34, 40,
             50, 64, 65, 3, 4}
    for ref, cx, cy, n, skip, end in (("U2", place.U2X, place.U2Y, 76, skip2, 5.45), ("U3", place.U3X, place.U3Y, 64, skip3, 5.35)):
        for k in range(1, n + 1):
            net = B.padnet(ref, k)
            if not net or k in skip or net.startswith(("RXA_", "TXA_", "TXB_")) and net[-1] in "PN" and net[-2] in "012C":
                continue
            px, py = B.pad(ref, k)
            if abs(px - cx) > abs(py - cy):
                q = (cx + (end if px > cx else -end), py)
            else:
                q = (px, cy + (end if py > cy else -end))
            r.reserve([(px, py), q], net, 0.2 if net in PLANE_NETS else 0.15)


def vbus(r):
    """USB-C supply to the fuse: a 0.3 mm neck at each pair of VBUS pins (they sit 0.2 mm from their
    neighbours), 0.5 mm from there on, with the two sides of the connector joined on B."""
    B = r.B
    ys = sorted({round(B.pad("J3", n)[1], 2) for n in ("A4", "A9", "B4", "B9")})
    px = B.pad("J3", "A4")[0]
    f1 = B.pad("F1", 1)
    top, bot = ys[0], ys[-1]
    r.track([(px, top), (8.2, top)], "VBUS", 0.3)
    r.track([(8.2, top), (8.6, top - 0.4), (8.6, f1[1]), f1], "VBUS", 0.5)
    r.track([(px, bot), (8.2, bot)], "VBUS", 0.3)
    r.track([(8.2, bot), (8.6, bot)], "VBUS", 0.5)
    r.via(8.6, 43.4, "VBUS", 0.6, 0.3)
    r.via(8.6, bot, "VBUS", 0.6, 0.3)
    r.track([(8.6, 43.4), (8.6, bot)], "VBUS", 0.5, "B")
    for n in ("A4", "A9", "B4", "B9"):
        r.pre.add(("J3", n))
    r.pre.add(("F1", "1"))


def earc(r):
    """The two eARC lines (HDMI pins 14 and 19) cross the board as a 100 ohm pair on B along the north edge.
    Pin 14 is the ARC link through R17; the pin 19 line ends at C70, which is not fitted."""
    r.trunk([(11.4, 5.0), (12.0, 5.6), (61.6, 5.6), (62.2, 5.0)], "RXA_UTIL", W_HDMI)
    r.trunk([(10.6, 6.0), (63.0, 6.0)], "RX_HPD_C", W_HDMI)


def guards(r):
    """Keep slow tracks and vias away from the high-speed pairs (about three gaps clear on the same layer)."""
    G = r.G
    j1, j2 = place.J1Y, place.J2Y
    for box, layer in (((10.6, j1 - 1.35, 29.4, j1 + 5.35), 0), ((29.4, place.U2Y - 3.0, 32.9, place.U2Y + 1.4), 0),
                       ((48.9, j2 - 5.35, 66.4, j2 + 1.35), 0),
                       ((36.6, 26.0, 42.4, 42.4), 1), ((34.5, 44.0, 47.6, 45.6), 1)):
        G.block_tracks(*box, layers=(layer,))
        G.block_vias(*box)


def power_links(r):
    """5 V to the east half of the board: on B under the module to the 1V1 buck's In2 island, which carries on
    as a strip round the south-east corner and up the east side (HDMI-out fuse, detect resistor, LED buffer).
    In2 stays 3V3 under the module, so the ESP32's supply is never cut off from the rest of the plane, and B stays
    free in the south-east for the flash bus."""
    for x, y in ((15.4, 45.4), (15.4, 46.2), (42.2, 52.3), (42.2, 53.1)):
        r.via(x, y, "+5V", 0.6, 0.3)
    r.trunk([(15.4, 45.4), (15.4, 50.6), (42.2, 50.6), (42.2, 53.1)], "+5V", 0.8)
    f2 = r.B.pad("F2", 1)
    r.via(59.7, 28.5, "+5V", 0.6, 0.3)
    r.track([(59.7, 28.5), f2], "+5V", 0.4)
    r.pre.add(("F2", "1"))
    # analogue 3V3 of the MS2130: a spine on B just outside the via ring, with a spur to each pin's via
    r.trunk([(47.5, 36.8), (47.5, 43.1), (51.3, 43.1)], "MS_AVDD33", 0.3)
    for y in (36.8, 38.8, 40.8):
        r.track([(47.5, y), (47.9, y)], "MS_AVDD33", 0.25, "B")
    for x in (48.3, 49.8, 51.3):
        r.track([(x, 43.1), (x, 42.7)], "MS_AVDD33", 0.25, "B")


def fl_bus(r):
    """Flash-access lines (ESP32 IO2..IO5 to R45..R48) as one bundle on B: a via beside each module pad, east
    under the MS2130's south capacitors, north between the LED parts and the terminal, then one line under each
    resistor to a via above its pad.  The module pins are assigned so the four never cross."""
    B = r.B
    order = ("FL_CS", "FL_SCLK", "FL_MOSI", "FL_MISO")                 # module pads 7, 6, 5, 4 (north to south)
    vw = ((36.9, 45.8), (36.9, 46.5), (36.9, 47.8), (36.9, 49.1))      # vias beside the module pads
    fan = (((37.6, 45.8), (38.9, 47.1)), ((37.6, 46.5), (38.5, 47.4)), ((37.5, 47.8), (37.6, 47.7)),
           ((37.2, 49.1), (38.3, 48.0)))
    for i, net in enumerate(order):
        pad = B.pad("U1", 7 - i)
        res = B.pad("R%d" % (45 + i), 2)
        assert B.padnet("U1", 7 - i) == net and B.padnet("R%d" % (45 + i), 2) == net, net
        vx, vy = vw[i]
        knee = (round(vx - abs(vy - pad[1]) - 0.3, 2), pad[1])
        r.track([pad, knee, (knee[0] + abs(vy - pad[1]), vy), (vx, vy)] if abs(vy - pad[1]) > 0.2 else
                [pad, (vx - 0.7, pad[1]), (vx - 0.7 + abs(vy - pad[1]), vy), (vx, vy)], net, W_SIG)
        r.via(vx, vy, net)
        y = 47.1 + 0.3 * i          # east along here
        x = 66.3 + 0.3 * i          # north along here
        yt = 35.8 + 0.3 * i         # east again above the terminal's top pad
        rx = round(res[0], 1)
        r.trunk([(vx, vy), fan[i][0], fan[i][1], (x - 0.4, y), (x, y - 0.4), (x, yt + 0.4), (x + 0.4, yt),
                 (rx - 0.4, yt), (rx, yt - 0.4), (rx, 30.3)], net, W_SIG)
        r.via(rx, 30.3, net)
        r.track([(rx, 30.3), res], net, W_SIG)


def cec_tap(r):
    """The CEC tap's own links: R18 (to the GSV2001 pin), R20 (to the ESP32 input), the test pad and Q1's drain
    are one small node, wired here so the path-finder only has to bring the bus to it."""
    B = r.B
    a, b, tp, q = B.pad("R18", 2), B.pad("R20", 1), B.pad("TP9", 1), B.pad("Q1", 3)
    for ref, pad in (("R18", 2), ("R20", 1), ("TP9", 1), ("Q1", 3)):
        assert B.padnet(ref, pad) == "CEC", (ref, pad)
    r.track([a, b], "CEC", W_SIG)
    r.track([a, (tp[0] - 0.6, a[1]), (tp[0] - 0.2, a[1] - 0.4)], "CEC", W_SIG)
    r.track([(tp[0] + 0.1, tp[1] + 0.2), (q[0], tp[1] + 0.2 + q[0] - tp[0] - 0.1), q], "CEC", W_SIG)


def supply_outputs(r):
    """Where each supply enters its plane: wide copper and several vias instead of one thin track and one via.
    1V2 carries about 0.7 A, 3V3 about 1 A at peaks, and the fused 5 V feeds the whole board."""
    B = r.B
    # 1V2: inductor - output capacitors, three vias into the In2 island between them
    l, c4, c5 = B.pad("L1", 2), B.pad("C4", 1), B.pad("C5", 1)
    r.track([(l[0], c4[1]), c4, c5], "+1V2", 0.8)
    r.track([(l[0], 28.5), (10.9, 28.5)], "+1V2", 0.5)
    for y in (28.5, 29.3, 30.1):
        r.via(10.9, y, "+1V2")
    r.track([(10.9, 29.3), (10.9, 30.1)], "+1V2", 0.5)
    r.pre.update({("L1", "2"), ("C4", "1"), ("C5", "1")})
    # 3V3: inductor - first output capacitor, three vias into the plane
    l, c7 = B.pad("L2", 2), B.pad("C7", 1)
    r.track([(l[0], c7[1]), c7], "+3V3", 0.8)
    for y in (27.3, 28.1):
        r.track([(l[0], y), (25.1, y)], "+3V3", 0.5)
    for y in (27.3, 28.1, 28.9):
        r.via(25.1, y, "+3V3")
    r.pre.update({("L2", "2"), ("C7", "1")})
    # 5 V after the input fuse: four vias into the west In2 island
    f = B.pad("F1", 2)
    r.track([f, (f[0], 38.5)], "+5V", 0.8)
    r.track([(12.0, 38.5), (12.8, 38.5)], "+5V", 0.5)
    r.track([f, (13.5, f[1])], "+5V", 0.5)
    r.track([(13.5, 39.5), (13.5, 40.3)], "+5V", 0.5)
    for x, y in ((12.0, 38.5), (12.8, 38.5), (13.5, 39.5), (13.5, 40.3)):
        r.via(x, y, "+5V")
    r.pre.add(("F1", "2"))
    # the module's 3V3 pin straight to its bulk capacitor, which gets two vias of its own
    p2, c = B.pad("U1", 2), B.pad("C66", 1)
    r.track([p2, (36.3, p2[1]), (36.3 + c[1] - p2[1], c[1]), c, (39.7, c[1])], "+3V3", 0.5)
    r.track([(39.7, 52.6), (39.7, 53.3)], "+3V3", 0.5)
    for y in (52.6, 53.3):
        r.via(39.7, y, "+3V3")
    r.pre.update({("U1", "2"), ("C66", "1")})


def module_ground(r):
    """The module's nine ground pads: tied into a grid with the twelve ground vias of Espressif's land pattern
    between them.  They carry heat to the middle of the module when it is soldered and give the USB pair, whose
    pads are right beside them, a ground return."""
    B = r.B
    pts = sorted({B.pad("U1", 41, k) for k in range(9)})
    xs, ys = sorted({p[0] for p in pts}), sorted({p[1] for p in pts})
    for y in ys:
        r.track([(xs[0], y), (xs[-1], y)], "GND", 0.3)
    for x in xs:
        r.track([(x, ys[0]), (x, ys[-1])], "GND", 0.3)
    for y in ys:
        for a, b in zip(xs, xs[1:]):
            r.via((a + b) / 2, y, "GND", 0.45, 0.2)
    for x in xs:
        for a, b in zip(ys, ys[1:]):
            r.via(x, (a + b) / 2, "GND", 0.45, 0.2)
    r.pre.add(("U1", "41"))


def hdmi_grounds(r):
    """Ground pins that sit between TMDS pads: connector shield pins get a via behind the pad row, and each ESD
    array gets one between its two pairs on the chip side."""
    B = r.B
    for conn, d in (("J1", -1), ("J2", 1)):
        for n in (2, 5, 8, 11, 17):
            px, py = B.pad(conn, n)
            vx = px + d * 1.65
            r.track([(px, py), (vx, py)], "GND", 0.25)
            r.via(vx, py, "GND")
            r.pre.add((conn, str(n)))
    for esd, d in (("D1", 1), ("D2", 1), ("D3", -1), ("D4", -1)):
        a, b = B.pad(esd, 3), B.pad(esd, 8)
        far = a if (a[0] - b[0]) * d > 0 else b
        near = b if far is a else a
        vx = far[0] + d * 0.95
        r.track([near, far, (vx, far[1])], "GND", 0.2)
        r.via(vx, far[1], "GND", 0.45, 0.2)
        r.pre.update({(esd, "3"), (esd, "8")})


def zones(B):
    w, h = place.W, place.H
    full = [(0.3, 0.3), (w - 0.3, 0.3), (w - 0.3, h - 0.3), (0.3, h - 0.3)]
    B.zone("GND", "In1", full, clearance=0.2, thermal=False)
    B.zone("+3V3", "In2", full, clearance=0.25, thermal=False)
    for i, (net, x1, y1, x2, y2) in enumerate(IN2):
        B.zone(net, "In2", [(x1, y1), (x2, y1), (x2, y2), (x1, y2)], clearance=0.25, prio=1 + i, thermal=False)
    for net, (x1, y1, x2, y2) in POURS.items():
        B.zone(net, "B", [(x1, y1), (x2, y1), (x2, y2), (x1, y2)], clearance=0.2, prio=5, thermal=False)
    B.zone("GND", "F", full, clearance=0.25, minw=0.2)
    B.zone("GND", "B", full, clearance=0.25, minw=0.2)
    x1, y1, x2, y2 = ANT
    B.keepout([(x1, y1), (x2, y1), (x2, y2), (x1, y2)])


def route_all(B, slow=True):
    r = R(B)
    u2rx = {"CN": 61, "CP": 62, "0N": 63, "0P": 64, "1N": 66, "1P": 67, "2N": 68, "2P": 69}
    u2tx = {"CN": 27, "CP": 28, "0N": 29, "0P": 30, "1N": 32, "1P": 33, "2N": 34, "2P": 35}
    hdmi_port(r, "J1", "D1", "D2", "U2", u2rx, 10.1, 29.6, "RXA")
    hdmi_port(r, "J2", "D3", "D4", "U2", u2tx, 67.9, 48.7, "TXA")
    txb(r)
    usb_hs(r)
    u2_power(r)
    hdmi_grounds(r)
    buck_nodes(r)
    power_links(r)
    supply_outputs(r)
    module_ground(r)
    fl_bus(r)
    vbus(r)
    earc(r)
    guards(r)
    u2_north(r)
    cec_tap(r)
    escapes(r)
    if slow:
        import nets
        nets.route_nets(r)
    zones(B)
    B.fill()
    return r
