"""How tidy is the routing?  Per net: routed length, vias, and the shortest tree that could join its pads
(octile minimum spanning tree over pad centres).  Run with KiCad's python: netstat.py [n-worst]"""
import math
import sys
from collections import defaultdict

import pcbnew

import route

B = pcbnew.LoadBoard("../hdmilight.kicad_pcb")
mm = pcbnew.ToMM
length, vias, pads, layers = defaultdict(float), defaultdict(int), defaultdict(list), defaultdict(set)
for t in B.GetTracks():
    n = t.GetNetname()
    if t.Type() == pcbnew.PCB_VIA_T:
        vias[n] += 1
    else:
        length[n] += mm(t.GetLength())
        layers[n].add(B.GetLayerName(t.GetLayer())[0])
for f in B.GetFootprints():
    for p in f.Pads():
        if p.GetNetname():
            pads[p.GetNetname()].append((mm(p.GetPosition().x), mm(p.GetPosition().y)))


def octile(a, b):
    dx, dy = abs(a[0] - b[0]), abs(a[1] - b[1])
    return max(dx, dy) + 0.4142 * min(dx, dy)


def mst(pts):
    pts = list(set(pts))
    if len(pts) < 2:
        return 0.0
    dist = {i: octile(pts[0], pts[i]) for i in range(1, len(pts))}
    tot = 0.0
    while dist:
        i = min(dist, key=dist.get)
        tot += dist.pop(i)
        for j in dist:
            dist[j] = min(dist[j], octile(pts[i], pts[j]))
    return tot


rows = []
for n in pads:
    if n in route.PLANE_NETS or n == "GND" or len(pads[n]) < 2:
        continue
    m = mst(pads[n])
    rows.append((length[n] - m, n, length[n], m, vias[n], len(pads[n]), "".join(sorted(layers[n]))))
rows.sort(reverse=True)
tl, tm, tv = sum(r[2] for r in rows), sum(r[3] for r in rows), sum(r[4] for r in rows)
print(f"{len(rows)} signal nets: {tl:.0f} mm routed, {tm:.0f} mm shortest possible ({tl / tm:.2f}x), {tv} vias")
print(f"{'net':18} {'routed':>7} {'ideal':>6} {'ratio':>5} {'vias':>4} pads layers")
for ex, n, l, m, v, k, ly in rows[:int(sys.argv[1]) if len(sys.argv) > 1 else 25]:
    print(f"{n:18} {l:7.1f} {m:6.1f} {l / m if m else 0:5.2f} {v:4d} {k:4d} {ly}")
