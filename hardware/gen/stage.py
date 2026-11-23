"""Build and route the board: python stage.py [hand|all]"""
import pickle
import runpy
import sys
import time

import design, kb, place, make_pcb, route

B = kb.Board(design.PARTS, place.W, place.H)
place.outline(B)
place.place_all(B)
# LED terminal: 1.8 mm holes (what LCSC's own footprint for the KF2EDGR header uses; the Phoenix MSTBA part fits too)
# and pads widened to 2.4 mm so the ring stays 0.3 mm
import pcbnew
for pad in B.fps["J4"].Pads():
    pad.SetDrillSize(kb.VECTOR2I(kb.FromMM(1.8), kb.FromMM(1.8)))
    try:
        sz = pad.GetSize(pcbnew.F_Cu)
        pad.SetSize(pcbnew.F_Cu, kb.VECTOR2I(max(sz.x, kb.FromMM(2.4)), max(sz.y, kb.FromMM(2.4))))
    except TypeError:
        sz = pad.GetSize()
        pad.SetSize(kb.VECTOR2I(max(sz.x, kb.FromMM(2.4)), max(sz.y, kb.FromMM(2.4))))
t = time.time()
r = route.route_all(B, slow=(len(sys.argv) < 2 or sys.argv[1] != "hand"))
print("routed in %.1fs" % (time.time() - t))
# silkscreen: only ICs, connectors and test pads get text; passives keep their reference on the Fab layer,
# and each test pad is labelled with its net name
import pcbnew
for ref, fp in B.fps.items():
    kind = design.PARTS[ref]["kind"]
    rt, vt = fp.Reference(), fp.Value()
    pos = fp.GetPosition()
    vt.SetVisible(False)
    for t in (rt, vt):
        t.SetTextAngleDegrees(0)
        t.SetKeepUpright(True)
    if kind in ("R", "C", "L", "FB", "F", "LED", "TVS", "ESD4", "XTAL", "MH"):
        rt.SetLayer(pcbnew.F_Fab)
        rt.SetTextSize(kb.VECTOR2I(kb.FromMM(0.4), kb.FromMM(0.4)))
        rt.SetTextThickness(kb.FromMM(0.06))
        rt.SetPosition(pos)
    elif kind == "SW":        # buttons are labelled with what they do
        rt.SetVisible(False)
        vt.SetVisible(True)
        vt.SetLayer(pcbnew.F_SilkS)
        vt.SetTextSize(kb.VECTOR2I(kb.FromMM(0.8), kb.FromMM(0.8)))
        vt.SetTextThickness(kb.FromMM(0.13))
        vt.SetPosition(kb.P(kb.ToMM(pos.x) - 4.6, kb.ToMM(pos.y)))
    elif kind == "TP":
        rt.SetVisible(False)
        vt.SetVisible(True)
        vt.SetLayer(pcbnew.F_SilkS)
        vt.SetTextSize(kb.VECTOR2I(kb.FromMM(0.65), kb.FromMM(0.65)))
        vt.SetTextThickness(kb.FromMM(0.11))
        vt.SetPosition(kb.P(kb.ToMM(pos.x), kb.ToMM(pos.y) - 1.05))
    else:
        rt.SetTextSize(kb.VECTOR2I(kb.FromMM(0.8), kb.FromMM(0.8)))
        rt.SetTextThickness(kb.FromMM(0.13))
    for g in fp.GraphicalItems():       # JLCPCB prints silkscreen lines from 0.15 mm
        if g.Type() == pcbnew.PCB_SHAPE_T and g.GetLayer() == pcbnew.F_SilkS and g.GetWidth() < kb.FromMM(0.15):
            g.SetWidth(kb.FromMM(0.15))
# test-pad labels: short names, and pads that sit in a row take turns above and below so the names do not collide
tps = sorted(((kb.ToMM(fp.GetPosition().x), kb.ToMM(fp.GetPosition().y), ref) for ref, fp in B.fps.items()
              if design.PARTS[ref]["kind"] == "TP"), key=lambda t: (round(t[1], 1), t[0]))
TS = 0.7                                        # text height; each name goes where it covers no pad and no other name
obst = []
for ref, fp in B.fps.items():
    for pad in fp.Pads():
        bb = pad.GetBoundingBox()
        obst.append((ref, tuple(kb.ToMM(v) for v in (bb.GetLeft(), bb.GetTop(), bb.GetRight(), bb.GetBottom()))))
for ref, fp in B.fps.items():                   # ... and no part body (the area its pads span)
    boxes = [o for r_, o in obst if r_ == ref]
    if design.PARTS[ref]["kind"] not in ("TP", "MH") and boxes:
        body = (min(o[0] for o in boxes), min(o[1] for o in boxes), max(o[2] for o in boxes), max(o[3] for o in boxes))
        obst += [(ref, body)] * 4
names = []
FIXED = {"TP26": (47.9, 30.75), "TP27": (52.7, 30.75), "TP14": (57.9, 27.45)}


def clash(box, own):
    n = sum(1 for ref, o in obst if ref != own and box[0] < o[2] + 0.1 and o[0] < box[2] + 0.1
            and box[1] < o[3] + 0.1 and o[1] < box[3] + 0.1)
    n += 25 * sum(1 for o in names if box[0] < o[2] + 0.15 and o[0] < box[2] + 0.15 and box[1] < o[3] + 0.1 and o[1] < box[3] + 0.1)
    nx1, ny1, nx2, _ = place.NOTCH
    if box[0] < 0.6 or box[1] < 0.6 or box[2] > place.W - 0.6 or box[3] > place.H - 0.6 or (
            box[2] > nx1 - 0.4 and box[0] < nx2 + 0.4 and box[3] > ny1 - 0.4):
        n += 10
    return n


for x, y, ref in tps:
    name = design.PARTS[ref]["value"]
    name = {"HDMI_OUT_5V": "OUT_5V"}.get(name, name[4:] if name.startswith("GSV_") else name)
    B.fps[ref].Value().SetVisible(False)        # the value field must stay equal to the schematic's
    w, h = 0.95 * TS * len(name), 1.3 * TS
    if ref in FIXED:                            # three crowded spots are placed by hand
        cx, cy = FIXED[ref]
        names.append((cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2))
        B.text(name, cx, cy, TS, thick=0.12)
        continue
    spots = [(x, y - 1.1), (x, y + 1.1), (x + 0.8 + w / 2, y), (x - 0.8 - w / 2, y), (x, y - 2.05), (x, y + 2.05),
             (x + w / 2, y - 1.1), (x - w / 2, y - 1.1), (x + w / 2, y + 1.1), (x - w / 2, y + 1.1),
             (x, y - 3.0), (x, y + 3.0), (x + 0.8 + w / 2, y - 1.1), (x - 0.8 - w / 2, y - 1.1),
             (x + 0.8 + w / 2, y + 1.1), (x - 0.8 - w / 2, y + 1.1)]
    cx, cy = min(spots, key=lambda c: clash((c[0] - w / 2, c[1] - h / 2, c[0] + w / 2, c[1] + h / 2), ref))
    names.append((cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2))
    B.text(name, cx, cy, TS, thick=0.12)
B.fps["J4"].Reference().SetPosition(kb.P(72.6, 35.3))
B.text("HDMI Light v3  rev A", 57.5, 3.2, 1.0)
B.text("IN", 11.0, 6.9, 1.0)
B.text("OUT", 74.2, 31.6, 1.0)
for label, pin in (("GND", 1), ("DATA", 2), ("BI", 3)):          # beside each terminal pin
    px, py = B.pad("J4", pin)
    B.text(label, px - 4.3, py, 0.8, thick=0.13)
B.text("5V 1.5A", 8.4, 51.9, 0.8, thick=0.13)
for pin, label in design.PARTS["J5"]["names"].items():          # debug header: each pin named along the board edge
    px, py = B.pad("J5", pin)
    t = B.text(label, px, py - 2.2, 0.8, thick=0.13)
    t.SetTextSize(kb.VECTOR2I(kb.FromMM(0.68), kb.FromMM(0.8)))     # narrower letters: three fit a 2.54 mm pitch with a gap
for t in (B.text("Colin Iuliano 2026", 16.6, 3.3, 1.0, thick=0.16, knockout=True),
          B.text("Colin Iuliano 2026", 58.7, 2.5, 1.5, layer=pcbnew.B_SilkS, thick=0.24, knockout=True)):
    bb = t.GetBoundingBox()
    print("  name block: x %.1f..%.1f  y %.1f..%.1f" % tuple(kb.ToMM(v) for v in (bb.GetLeft(), bb.GetRight(), bb.GetTop(), bb.GetBottom())))
B.save(make_pcb.OUT)
runpy.run_path("make_pro.py")
# drop hand-placed vias that ended up unused (KiCad reports them as dangling), then refill
import json, subprocess, pcbnew
subprocess.run([make_pcb.CLI, "pcb", "drc", "--format", "json", "--severity-all", "-o", "../out/drc_pre.json", make_pcb.OUT],
               capture_output=True)
dang = {(round(v["items"][0]["pos"]["x"], 2), round(v["items"][0]["pos"]["y"], 2))
        for v in json.load(open("../out/drc_pre.json"))["violations"] if v["type"] == "via_dangling"}
n = 0
for t in list(B.b.GetTracks()):
    if t.Type() == pcbnew.PCB_VIA_T:
        p = t.GetPosition()
        if (round(kb.ToMM(p.x), 2), round(kb.ToMM(p.y), 2)) in dang:
            print("  unused via removed:", t.GetNetname(), round(kb.ToMM(p.x), 2), round(kb.ToMM(p.y), 2))
            B.b.Remove(t)
            n += 1
for v in json.load(open("../out/drc_pre.json"))["violations"]:
    if v["type"] == "track_dangling":
        q = v["items"][0]["pos"]
        for t in list(B.b.GetTracks()):
            if t.Type() != pcbnew.PCB_VIA_T and kb.ToMM(t.GetLength()) < 0.35 and t.HitTest(kb.P(q["x"], q["y"])):
                B.b.Remove(t)
                n += 1
print("removed", n, "unused vias / stubs")


def trim_tails(board, passes=4):
    """A hand-drawn trunk that the path-finder joined part-way along keeps an unused tail.  Cut each such track
    back to the last point where something of its net touches it (KiCad reports them as dangling tracks)."""
    cut = 0
    for _ in range(passes):
        board.save(make_pcb.OUT)
        subprocess.run([make_pcb.CLI, "pcb", "drc", "--format", "json", "--severity-all", "-o", "../out/drc_pre.json",
                        make_pcb.OUT], capture_output=True)
        todo = [v["items"][0] for v in json.load(open("../out/drc_pre.json"))["violations"] if v["type"] == "track_dangling"]
        changed = 0
        for item in todo:
            q = item["pos"]
            for t in list(board.b.GetTracks()):
                if t.Type() == pcbnew.PCB_VIA_T or not t.HitTest(kb.P(q["x"], q["y"])) or t.GetNetname() == "RX_HPD_C":
                    continue
                a, e = t.GetStart(), t.GetEnd()
                ax, ay, ex, ey = (kb.ToMM(v) for v in (a.x, a.y, e.x, e.y))
                L = math.hypot(ex - ax, ey - ay)
                if L < 1e-6:
                    continue
                ts = []          # positions along the track (0..1) where other copper of the net touches it
                net, lay = t.GetNetname(), t.GetLayer()
                for o in board.b.GetTracks():
                    if o is t or o.GetNetname() != net or (o.Type() != pcbnew.PCB_VIA_T and o.GetLayer() != lay):
                        continue
                    for p in ((o.GetStart(),) if o.Type() == pcbnew.PCB_VIA_T else (o.GetStart(), o.GetEnd())):
                        px, py = kb.ToMM(p.x), kb.ToMM(p.y)
                        u = ((px - ax) * (ex - ax) + (py - ay) * (ey - ay)) / (L * L)
                        d = math.hypot(ax + u * (ex - ax) - px, ay + u * (ey - ay) - py)
                        if -0.01 <= u <= 1.01 and d < 0.06:
                            ts.append(min(1.0, max(0.0, u)))
                for fp in board.b.GetFootprints():
                    for pad in fp.Pads():
                        if pad.GetNetname() == net and pad.IsOnLayer(lay):
                            for u in (0.0, 1.0):
                                if pad.HitTest(kb.P(ax + u * (ex - ax), ay + u * (ey - ay))):
                                    ts.append(u)
                if len(ts) < 2 or max(ts) - min(ts) < 1e-3:
                    board.b.Remove(t)
                else:
                    lo, hi = min(ts), max(ts)
                    if lo < 1e-3 and hi > 1 - 1e-3:
                        continue
                    t.SetStart(kb.P(ax + lo * (ex - ax), ay + lo * (ey - ay)))
                    t.SetEnd(kb.P(ax + hi * (ex - ax), ay + hi * (ey - ay)))
                changed += 1
                break
        cut += changed
        if not changed:
            break
    return cut


import math
print("trimmed", trim_tails(B), "unused track tails")

B.fill()
B.save(make_pcb.OUT)
runpy.run_path("make_pro.py")   # SaveBoard rewrites the project with defaults; put our rules back
pickle.dump({"trk": r.G.trk, "via": r.G.via, "cu": r.G.cu, "netid": r.G.netid, "nx": r.G.nx, "ny": r.G.ny,
             "pads": {n: [(p[0], p[1], p[2]) for p in v] for n, v in r.padlist.items()},
             "fails": getattr(r, "fails", [])}, open("../out/grid.pkl", "wb"))
