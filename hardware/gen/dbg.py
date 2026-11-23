"""Print the routing grid around a pad: python dbg.py REF.PAD [halfwidth_mm] [layer]"""
import pickle, sys
d = pickle.load(open("../out/grid.pkl", "rb"))
ref, pad = sys.argv[1].split(".")
hw = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
L = int(sys.argv[3]) if len(sys.argv) > 3 else 0
for net, pads in d["pads"].items():
    for r_, n_, (x, y) in pads:
        if r_ == ref and n_ == pad:
            me, cx, cy = d["netid"][net], x, y
            print(ref, pad, net, "at", x, y, "layer", "FB"[L])
nx = d["nx"]
names = {v: k for k, v in d["netid"].items()}
seen = {}
ix0, iy0 = round(cx * 10), round(cy * 10)
m = int(hw * 10)
for iy in range(iy0 - m, iy0 + m + 1):
    row = ""
    for ix in range(ix0 - m, ix0 + m + 1):
        t = d["trk"][L][iy * nx + ix]; v = d["via"][L][iy * nx + ix]; vb = d["via"][1 - L][iy * nx + ix]
        c = d["cu"][L][iy * nx + ix]
        if ix == ix0 and iy == iy0: ch = "@"
        elif c == me: ch = "X"
        elif t == me: ch = "+" if (v in (0, me) and vb in (0, me)) else "x"
        elif t == 0: ch = "o" if (v in (0, me) and vb in (0, me)) else "."
        elif t == -1: ch = "#"
        else:
            ch = seen.setdefault(t, chr(ord("a") + len(seen) % 26)) if False else "~"
            seen[t] = names.get(t)
        row += ch
    print(row)
print("legend: @ pad centre, X own copper, +/x own-net area (via ok / not), o free+via ok, . free, # blocked, ~ other net:", sorted(set(seen.values())))
