"""A small schematic-drawing layer over KiCad's file format: symbols drawn by this file, parts placed by
function, wires between pins.  Nets come from design.PARTS; any pin a sheet forgets gets a labelled stub, and
check_sch.py compares KiCad's netlist with design.py, so a drawing mistake shows up as a mismatch."""
import uuid
from collections import OrderedDict

import design

NS = uuid.UUID("6f1d3c52-2a55-4c1e-9a1e-0c4e5b7a9d10")
PROJECT = "hdmilight"
G = 2.54
H = 1.27
PWR = ("GND", "+5V", "+3V3", "+1V2", "+1V1", "+2V5")
FONT = "(effects (font (size 1.27 1.27)))"


def uid(key):
    return str(uuid.uuid5(NS, key))


ROOT = uid("root")


def q(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def sn(v):
    return round(round(v / H) * H, 2)


def poly(pts, fill="none", w=0.254):
    return ("(polyline (pts " + " ".join(f"(xy {x:.3f} {y:.3f})" for x, y in pts) +
            f") (stroke (width {w}) (type default)) (fill (type {fill})))")


GLYPH = {  # vertical drawing, pin 1 at the top; (shapes, pin length)
    "R": ([[(-1.016, 2.54), (1.016, 2.54), (1.016, -2.54), (-1.016, -2.54), (-1.016, 2.54)]], 1.27),
    "C": ([[(-2.032, 0.635), (2.032, 0.635)], [(-2.032, -0.635), (2.032, -0.635)]], 3.175),
    "L": ([[(0, 2.54), (1.2, 2.2), (1.2, 1.2), (0, 0.85), (1.2, 0.5), (1.2, -0.5), (0, -0.85), (1.2, -1.2), (1.2, -2.2),
            (0, -2.54)]], 1.27),
    "FB": ([[(-1.016, 2.54), (1.016, 2.54), (1.016, -2.54), (-1.016, -2.54), (-1.016, 2.54)], [(-1.6, -1.6), (1.6, 1.6)]], 1.27),
    "F": ([[(-0.762, 2.54), (0.762, 2.54), (0.762, -2.54), (-0.762, -2.54), (-0.762, 2.54)], [(0, 2.54), (0, -2.54)]], 1.27),
    "D": ([[(-1.27, -1.27), (1.27, -1.27), (0, 1.27), (-1.27, -1.27)], [(-1.27, 1.27), (1.27, 1.27)]], 2.54),
    "SW": ([[(0, 2.54), (0, 1.5)], [(0, -2.54), (0, -1.5)], [(0, -1.5), (1.3, 1.3)]], 1.27),
}
KIND_GLYPH = {"R": "R", "C": "C", "L": "L", "FB": "FB", "F": "F", "LED": "D", "TVS": "D", "SW": "SW"}


class Sheet:
    def __init__(self, name, title, paper="A3"):
        self.name, self.title, self.paper = name, title, paper
        self.libs = OrderedDict()
        self.items = []
        self.wires = []
        self.pins = OrderedDict()     # (ref, num) -> (x, y, side)
        self.marks = set()            # points carrying a label, power symbol or no-connect
        self.npwr = 0
        self.placed = []
        self.named = set()            # nets that carry a label or supply symbol on this sheet
        self.tags = []                # (x, y, net) of every label and supply symbol

    # ---- library symbols
    def _lib2(self, glyph, o, flip):
        name = f"{glyph}_{o}{'f' if flip else ''}"
        if name in self.libs:
            return name
        shapes, plen = GLYPH[glyph]
        t = (lambda x, y: (x, y)) if o == "v" else (lambda x, y: (-y, x))
        g = " ".join(poly([t(*p) for p in s]) for s in shapes)
        n1, n2 = ("2", "1") if flip else ("1", "2")
        (x1, y1), (x2, y2) = t(0, 3.81), t(0, -3.81)
        a1, a2 = (270, 90) if o == "v" else (0, 180)
        s = (f'    (symbol {q("hl:" + name)} (pin_numbers hide) (pin_names (offset 0) hide) (in_bom yes) (on_board yes)\n'
             f'      (property "Reference" {q(glyph[0])} (at 0 0 0) {FONT})\n      (property "Value" {q(name)} (at 0 0 0) {FONT})\n'
             f'      (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
             f'      (property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
             f'      (symbol {q(name + "_0_1")} {g})\n      (symbol {q(name + "_1_1")}\n'
             f'        (pin passive line (at {x1:.2f} {y1:.2f} {a1}) (length {plen}) (name "~" {FONT}) (number {q(n1)} {FONT}))\n'
             f'        (pin passive line (at {x2:.2f} {y2:.2f} {a2}) (length {plen}) (name "~" {FONT}) (number {q(n2)} {FONT})))\n    )\n')
        self.libs[name] = s
        return name

    def _libpwr(self, net):
        name = "PWR_" + net.replace("+", "P")
        if name in self.libs:
            return name
        if net == "GND":
            g = " ".join(poly(p) for p in ([(0, 0), (0, -1.27)], [(-1.27, -1.27), (1.27, -1.27)], [(-0.762, -1.778), (0.762, -1.778)],
                                           [(-0.254, -2.286), (0.254, -2.286)]))
        else:
            g = poly([(0, 0), (0, 1.27)]) + " " + poly([(-1.27, 1.27), (1.27, 1.27)])
        self.libs[name] = (
            f'    (symbol {q("hl:" + name)} (power) (pin_numbers hide) (pin_names (offset 0) hide) (in_bom yes) (on_board yes)\n'
            f'      (property "Reference" "#PWR" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'      (property "Value" {q(net)} (at 0 0 0) {FONT})\n'
            f'      (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'      (property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'      (symbol {q(name + "_0_1")} {g})\n      (symbol {q(name + "_1_1")}\n'
            f'        (pin power_in line (at 0 0 0) (length 0) (name {q(net)} {FONT}) (number "1" {FONT})))\n    )\n')
        return name

    # ---- placing parts
    def _inst(self, ref, lib, x, y, refpos, valpos, nums, just="left"):
        p = design.PARTS[ref]
        self.placed.append(ref)
        path = "/" + ROOT + "/" + uid("sheet:" + self.name)
        s = (f'  (symbol (lib_id {q("hl:" + lib)}) (at {x:.2f} {y:.2f} 0) (unit 1) '
             f'(in_bom {"no" if p["kind"] in ("TP", "MH") else "yes"}) (on_board yes) (dnp {"yes" if p["dnp"] else "no"}) '
             f'(uuid {q(uid("sym:" + ref))})\n'
             f'    (property "Reference" {q(ref)} (at {refpos[0]:.2f} {refpos[1]:.2f} 0) (effects (font (size 1.27 1.27)) (justify {just})))\n'
             f'    (property "Value" {q(p["value"])} (at {valpos[0]:.2f} {valpos[1]:.2f} 0) '
             f'(effects (font (size 1.0 1.0)) (justify {just})))\n'
             f'    (property "Footprint" {q(p["fp"])} (at {x:.2f} {y:.2f} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
             f'    (property "Datasheet" "" (at {x:.2f} {y:.2f} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n')
        for k, v in (("MPN", p["mpn"]), ("LCSC", p["lcsc"])):
            if v:
                s += f'    (property {q(k)} {q(v)} (at {x:.2f} {y:.2f} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
        for num in nums:
            s += f'    (pin {q(num)} (uuid {q(uid("pin:" + ref + ":" + num))}))\n'
        s += f'    (instances (project {q(PROJECT)} (path {q(path)} (reference {q(ref)}) (unit 1))))\n  )\n'
        self.items.append(s)

    def net(self, ref, num):
        return design.PARTS[ref]["pins"][str(num)]

    def two(self, ref, x, y, o="v", first=None):
        """Two-pin part centred at (x, y).  first: the net that goes on the top (v) or left (h) pin."""
        x, y = sn(x), sn(y)
        p = design.PARTS[ref]
        flip = first is not None and p["pins"]["1"] != first
        if first is not None:
            assert first in (p["pins"]["1"], p["pins"]["2"]), (ref, first, p["pins"])
        lib = self._lib2(KIND_GLYPH.get(p["kind"], "R"), o, flip)
        a, b = ("2", "1") if flip else ("1", "2")
        if o == "v":
            self.pins[(ref, a)] = (x, sn(y - 3.81), "U")
            self.pins[(ref, b)] = (x, sn(y + 3.81), "D")
            self._inst(ref, lib, x, y, (x + 2.3, y - 1.0), (x + 2.3, y + 1.0), ("1", "2"))
        else:
            self.pins[(ref, a)] = (sn(x - 3.81), y, "L")
            self.pins[(ref, b)] = (sn(x + 3.81), y, "R")
            self._inst(ref, lib, x, y, (x, y - 2.0), (x, y + 3.4), ("1", "2"), just="bottom")
        return self.pins[(ref, a)][:2], self.pins[(ref, b)][:2]

    def box(self, ref, x, y, L=(), R=(), T=(), B=(), names=None, at=None, minw=0):
        """An IC or connector as a box.  Pin lists hold pad numbers, None leaves a gap.  at=(pin, x, y) puts
        that pin's end at the given point."""
        p = design.PARTS[ref]
        nm = dict(p["names"])
        nm.update({str(k): v for k, v in (names or {}).items()})
        L, R, T, B = ([None if v is None else str(v) for v in s] for s in (L, R, T, B))
        rows = max(len(L), len(R), 1)
        cols = max(len(T), len(B), 0)
        ln = max([len(nm.get(v, "")) for v in L if v] + [0])
        rn = max([len(nm.get(v, "")) for v in R if v] + [0])
        w = max((ln + rn) * 0.62 + 5, cols * G / 2 + G, minw / 2, 5.08)
        w = sn(int(w / G + 0.999) * G)
        h = sn((rows + 1) * H + (3.81 if T or B else 0))
        pos = {}
        for side, lst, in (("L", L), ("R", R)):
            for i, v in enumerate(lst):
                if v:
                    pos[v] = ((-w - G) if side == "L" else (w + G), (rows - 1) * H - i * G, side)
        for side, lst in (("U", T), ("D", B)):
            for i, v in enumerate(lst):
                if v:
                    pos[v] = (-(len(lst) - 1) * H + i * G, (h + G) if side == "U" else (-h - G), side)
        if at:
            px, py, _ = pos[str(at[0])]
            x, y = at[1] - px, at[2] + py
        x, y = sn(x), sn(y)
        ang = {"L": 0, "R": 180, "U": 270, "D": 90}
        s = (f'    (symbol {q("hl:" + ref)} (pin_names (offset 0.508)) (in_bom yes) (on_board yes)\n'
             f'      (property "Reference" {q(ref.rstrip("0123456789"))} (at 0 0 0) {FONT})\n      (property "Value" {q(ref)} (at 0 0 0) {FONT})\n'
             f'      (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
             f'      (property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
             f'      (symbol {q(ref + "_0_1")} (rectangle (start {-w:.2f} {h:.2f}) (end {w:.2f} {-h:.2f}) (stroke (width 0.254) (type default)) (fill (type background))))\n'
             f'      (symbol {q(ref + "_1_1")}\n')
        for v, (px, py, side) in pos.items():
            s += (f'        (pin passive line (at {px:.2f} {py:.2f} {ang[side]}) (length 2.54) (name {q(nm.get(v) or "~")} {FONT}) '
                  f'(number {q(v)} {FONT}))\n')
            self.pins[(ref, v)] = (sn(x + px), sn(y - py), side)
        s += '      )\n    )\n'
        self.libs[ref] = s
        self._inst(ref, ref, x, y, (x - w, y - h - 2.6 - (5 if T else 0)), (x - w, y - h - 1.0 - (5 if T else 0)), list(pos))
        return x, y, w, h

    def tp(self, ref, x, y):
        """Test pad with its pin at (x, y), drawn hanging below."""
        if "TP" not in self.libs:
            self.libs["TP"] = (
                '    (symbol "hl:TP" (pin_numbers hide) (pin_names (offset 0) hide) (in_bom yes) (on_board yes)\n'
                f'      (property "Reference" "TP" (at 0 0 0) {FONT})\n      (property "Value" "TP" (at 0 0 0) {FONT})\n'
                '      (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
                '      (property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
                '      (symbol "TP_0_1" (circle (center 0 -2.54) (radius 0.762) (stroke (width 0.254) (type default)) (fill (type none))))\n'
                f'      (symbol "TP_1_1" (pin passive line (at 0 0 270) (length 1.778) (name "~" {FONT}) (number "1" {FONT})))\n    )\n')
        x, y = sn(x), sn(y)
        self.pins[(ref, "1")] = (x, y, "U")
        self._inst(ref, "TP", x, y, (x + 1.5, y + 2.2), (x + 1.5, y + 3.6), ("1",))
        design.PARTS[ref]  # exists
        return x, y

    # ---- connections
    def P(self, ref, num):
        return self.pins[(ref, str(num))][:2]

    def wire(self, *pts):
        pts = [(sn(x), sn(y)) for x, y in pts]
        for a, b in zip(pts, pts[1:]):
            if a != b:
                assert a[0] == b[0] or a[1] == b[1], ("diagonal wire", a, b)
                self.wires.append((a, b))
        return pts[-1]

    def label(self, net, x, y, side="R"):
        x, y = sn(x), sn(y)
        self.named.add(net)
        self.tags.append((x, y, net))
        ang, just = {"R": (0, "left"), "L": (180, "right"), "U": (90, "left"), "D": (270, "right")}[side]
        self.items.append(f'  (global_label {q(net)} (shape passive) (at {x:.2f} {y:.2f} {ang}) (fields_autoplaced yes) '
                          f'(effects (font (size 1.27 1.27)) (justify {just})) (uuid {q(uid(f"lbl:{self.name}:{net}:{x}:{y}"))}))\n')
        self.marks.add((x, y))
        return x, y

    def pwr(self, net, x, y):
        """Power symbol at a point: ground hangs down, rails point up."""
        x, y = sn(x), sn(y)
        if net not in PWR:
            return self.label(net, x, y, "U" if net != "GND" else "D")
        lib = self._libpwr(net)
        self.named.add(net)
        self.tags.append((x, y, net))
        self.npwr += 1
        ref = f"#PWR{self.name[:2].upper()}{self.npwr:03d}"
        vy = y + 3.6 if net == "GND" else y - 2.6
        path = "/" + ROOT + "/" + uid("sheet:" + self.name)
        self.items.append(
            f'  (symbol (lib_id {q("hl:" + lib)}) (at {x:.2f} {y:.2f} 0) (unit 1) (in_bom yes) (on_board yes) (dnp no) '
            f'(uuid {q(uid("pwr:" + self.name + ref))})\n'
            f'    (property "Reference" {q(ref)} (at {x:.2f} {y:.2f} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'    (property "Value" {q(net)} (at {x:.2f} {vy:.2f} 0) (effects (font (size 1.0 1.0))))\n'
            f'    (property "Footprint" "" (at {x:.2f} {y:.2f} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'    (property "Datasheet" "" (at {x:.2f} {y:.2f} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'    (pin "1" (uuid {q(uid("pwrpin:" + self.name + ref))}))\n'
            f'    (instances (project {q(PROJECT)} (path {q(path)} (reference {q(ref)}) (unit 1))))\n  )\n')
        self.marks.add((x, y))
        return x, y

    def nc(self, ref, num):
        x, y = self.P(ref, num)
        self.items.append(f'  (no_connect (at {x:.2f} {y:.2f}) (uuid {q(uid(f"nc:{ref}:{num}"))}))\n')
        self.marks.add((x, y))

    def text(self, s, x, y, size=1.6):
        self.items.append(f'  (text {q(s)} (at {sn(x):.2f} {sn(y):.2f} 0) (effects (font (size {size} {size})) (justify left bottom)) '
                          f'(uuid {q(uid(f"txt:{self.name}:{x}:{y}"))}))\n')

    def out(self, ref, num, n=2):
        """Point n grid steps straight out of a box pin (and the wire to it)."""
        x, y, side = self.pins[(ref, str(num))]
        dx, dy = {"L": (-1, 0), "R": (1, 0), "U": (0, -1), "D": (0, 1)}[side]
        return self.wire((x, y), (x + dx * n * G, y + dy * n * G))

    def stub(self, ref, num, n=2, net=None):
        """Wire out of a pin ending in its net: a power symbol for rails, a label otherwise, a cross if unconnected."""
        net = net or self.net(ref, num)
        if net is None:
            return self.nc(ref, num)
        x, y, side = self.pins[(ref, str(num))]
        ex, ey = self.out(ref, num, n)
        if net in PWR and side in ("U", "D") and (net == "GND") == (side == "D"):
            return self.pwr(net, ex, ey)
        return self.label(net, ex, ey, side)

    # ---- patterns
    def up(self, ref, x, y, rail=None):
        """Vertical part standing on the point (x, y); its top goes to a rail."""
        p = design.PARTS[ref]["pins"]
        here = None
        rail = rail or next(n for n in p.values() if n in PWR)
        self.two(ref, x, y - 3.81, "v", first=rail)
        return self.pwr(rail, x, y - 7.62)

    def down(self, ref, x, y, to="GND"):
        """Vertical part hanging from the point (x, y); its bottom goes to ground (or another net)."""
        p = design.PARTS[ref]["pins"]
        first = p["1"] if p["2"] == to else p["2"]
        self.two(ref, x, y + 3.81, "v", first=first)
        if to in PWR:
            return self.pwr(to, x, y + 7.62)
        return self.label(to, x, y + 7.62, "D")

    def series(self, ref, x, y, left):
        """Horizontal part with its left pin at (x, y); returns the right pin's point."""
        self.two(ref, x + 3.81, y, "h", first=left)
        return sn(x + 7.62), sn(y)

    def bank(self, x, y, rail, refs, pitch=12.7, feed=None, title=None):
        """Decoupling capacitors hanging from one rail wire.  feed=(ferrite ref, source rail) puts a filter in front."""
        x, y = sn(x), sn(y)
        if title:
            self.text(title, x - 5, y - 9, 1.4)
        x0 = x
        if feed:
            fb, src = feed
            self.pwr(src, x - 12.7, y)
            self.wire((x - 12.7, y), (x - 10.16, y))
            self.series(fb, x - 10.16, y, left=src)
            self.wire((x - 2.54, y), (x, y))
            self.label(rail, x + pitch * (len(refs) - 1) + 5.08, y, "R")
            self.wire((x + pitch * (len(refs) - 1), y), (x + pitch * (len(refs) - 1) + 5.08, y))
        else:
            self.pwr(rail, x - 5.08, y)
            self.wire((x - 5.08, y), (x, y))
        self.wire((x, y), (x + pitch * (len(refs) - 1), y))
        for i, ref in enumerate(refs):
            self.down(ref, x + i * pitch, y)

    def tps(self, x, y, refs, pitch=22.86, title="Test pads"):
        self.text(title, x - 3, y - 6, 1.4)
        for i, ref in enumerate(refs):
            px, py = self.tp(ref, x + (i % 8) * pitch, y + (i // 8) * 12.7)
            net = self.net(ref, 1)
            self.wire((px, py), (px, py - 2.54))
            if net in PWR and net != "GND":
                self.pwr(net, px, py - 2.54)
            else:
                self.wire((px, py - 2.54), (px + 2.54, py - 2.54))
                self.label(net, px + 2.54, py - 2.54, "R")

    # ---- output
    def render(self):
        on = lambda p, a, b: p != a and p != b and ((a[0] == b[0] == p[0] and min(a[1], b[1]) < p[1] < max(a[1], b[1])) or
                                                    (a[1] == b[1] == p[1] and min(a[0], b[0]) < p[0] < max(a[0], b[0])))
        # 1. break wires wherever a pin, label or another wire's end touches them, so every connection is at an end
        special = {(x, y) for x, y, _ in self.pins.values()} | self.marks | {pt for w in self.wires for pt in w}
        segs = []
        for a, b in self.wires:
            cut = sorted([p for p in special if on(p, a, b)], key=lambda p: (p[0], p[1]))
            chain = sorted([a, b], key=lambda p: (p[0], p[1]))
            chain = [chain[0]] + cut + [chain[1]]
            segs += [(u, v) for u, v in zip(chain, chain[1:]) if u != v]
        segs = list(dict.fromkeys(segs))
        # 2. anything left unconnected gets a labelled stub so the netlist is still right
        ends = {pt for w in segs for pt in w} | self.marks
        self.wires = []
        for (ref, num), (x, y, side) in list(self.pins.items()):
            if (x, y) not in ends:
                net = self.net(ref, num)
                if net is None:
                    self.nc(ref, num)
                elif side in ("L", "R") and design.PARTS[ref]["kind"] not in KIND_GLYPH:
                    self.stub(ref, num)
                else:
                    self.label(net, x, y, side)
        segs += self.wires
        # 3. groups of connected points; every group must be one net and carry that net's name once
        parent = {}

        def find(p):
            parent.setdefault(p, p)
            while parent[p] != p:
                parent[p] = parent[parent[p]]
                p = parent[p]
            return p

        for a, b in segs:
            parent[find(a)] = find(b)
        groups = {}
        for (ref, num), (x, y, side) in self.pins.items():
            net = self.net(ref, num)
            if net:
                small = design.PARTS[ref]["kind"] in KIND_GLYPH
                groups.setdefault(find((x, y)), []).append((0 if small and side in ("U", "L") else 1 if small else 2, x, y, net, ref, num))
        tagged = {}
        for x, y, net in self.tags:
            tagged.setdefault(find((x, y)), set()).add(net)
        for root, pins in groups.items():
            nets = {p[3] for p in pins} | tagged.get(root, set())
            if len(nets) > 1:
                print(f"  {self.name}: nets joined by the drawing: {sorted(nets)} at {[(p[4], p[5]) for p in pins][:6]}")
            net = pins[0][3]
            if net not in tagged.get(root, ()):
                # name tag in the middle of the group's longest wire, lying along it
                mine = sorted((s_ for s_ in segs if find(s_[0]) == root), key=lambda w: -(abs(w[0][0] - w[1][0]) + abs(w[0][1] - w[1][1])))
                if mine and abs(mine[0][0][0] - mine[0][1][0]) + abs(mine[0][0][1] - mine[0][1][1]) >= 2 * G - 0.01:
                    (ax, ay), (bx, by) = mine[0]
                    horiz = ay == by
                    x, y = (sn(min(ax, bx) + H), ay) if horiz else (ax, sn(max(ay, by) - H))
                    segs.remove(mine[0])
                    segs += [((ax, ay), (x, y)), ((x, y), (bx, by))]
                    ang = 0 if horiz else 90
                else:
                    _, x, y = sorted(p[:3] for p in pins)[0]
                    ang = 90
                self.items.append(f'  (global_label {q(net)} (shape passive) (at {x:.2f} {y:.2f} {ang}) (fields_autoplaced yes) '
                                  f'(effects (font (size 0.9 0.9)) (justify left)) (uuid {q(uid(f"name:{self.name}:{net}:{x}:{y}"))}))\n')
        # 4. junction dots
        count = {}
        for a, b in segs:
            count[a] = count.get(a, 0) + 1
            count[b] = count.get(b, 0) + 1
        for x, y, _ in self.pins.values():
            if (x, y) in count:
                count[(x, y)] += 1
        junc = [p for p, n in count.items() if n >= 3]
        s = (f'(kicad_sch (version 20231120) (generator "hdmilight_gen") (generator_version "2")\n  (uuid {q(uid("file:" + self.name))})\n'
             f'  (paper {q(self.paper)})\n  (title_block (title {q("HDMI Light v3 - " + self.title)}) (rev "A"))\n  (lib_symbols\n')
        s += "".join(self.libs.values()) + "  )\n"
        for i, (a, b) in enumerate(segs):
            s += (f'  (wire (pts (xy {a[0]:.2f} {a[1]:.2f}) (xy {b[0]:.2f} {b[1]:.2f})) (stroke (width 0) (type default)) '
                  f'(uuid {q(uid(f"w:{self.name}:{i}"))}))\n')
        for i, p in enumerate(junc):
            s += f'  (junction (at {p[0]:.2f} {p[1]:.2f}) (diameter 0) (color 0 0 0 0) (uuid {q(uid(f"j:{self.name}:{i}"))}))\n'
        s += "".join(self.items) + ")\n"
        return s
