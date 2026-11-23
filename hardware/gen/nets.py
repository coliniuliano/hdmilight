"""Order and options for the slow nets, and the loop that sends them through the grid router."""
import math

import route
from router import RES, simplify

HAND = lambda n: n.startswith(("RXA_", "TXA_", "TXB_")) and n[-1] in "PN" and n[-2] in "012C" or n in ("USB_DM", "USB_DP")

# nets routed first, in this order (short analogue and timing-sensitive ones); the rest follow by size
FIRST = ["GSV_XTALI", "GSV_XTALO", "MS_XTIN", "MS_XTOUT", "GSV_REXT", "MS_USB_REXT", "MS_RX_REXT",
         "GSV_MPLL_PVDD", "GSV_TX_PVDD", "LX_1V2", "LX_3V3", "LX_1V1", "FB_1V2", "FB_3V3", "FB_1V1",
         "ESP_USB_DM", "ESP_USB_DP", "USBC_DM", "USBC_DP"]
# 0.2 mm is the widest the grid's clearance allows; anything heavier is drawn explicitly in route.py
WIDE = {k: 0.2 for k in ("VBUS", "LX_1V2", "LX_3V3", "LX_1V1", "HDMI_OUT_5V", "+2V5", "MS_AVDD33", "MS_DVDD11",
                         "GSV_MPLL_PVDD", "GSV_TX_PVDD", "RX_5V")}
LAST = ["GND"]


def span(pads):
    xs = [p[2][0] for p in pads]
    ys = [p[2][1] for p in pads]
    return (max(xs) - min(xs)) + (max(ys) - min(ys))


def route_nets(r, only=None, verbose=True):
    G = r.G
    names = [n for n in r.padlist if not HAND(n)]
    rest = sorted((n for n in names if n not in FIRST and n not in route.PLANE_NETS), key=lambda n: (n in route.POURS, span(r.padlist[n])))
    # supplies first (short drops into the planes), then signals shortest first
    pours = [n for n in rest if n in route.POURS]
    rest = [n for n in rest if n not in route.POURS]
    order = [n for n in FIRST if n in names] + [n for n in route.PLANE_NETS if n != "GND" and n in names] + pours + LAST + rest
    fails = []
    for name in order:
        if only and name not in only:
            continue
        pads = r.padlist[name]
        if len(pads) < 2 and name not in route.PLANE_NETS:
            continue
        f = route_net(r, name, pads)
        if f:
            fails += [(name, x) for x in f]
    r.fails = fails
    if verbose:
        print(f"routed {len(order)} nets, {len(fails)} failed connections")
        for n, x in fails:
            print("  FAILED", n, x)
    return fails


def route_net(r, name, pads):
    G = r.G
    net = G.nid(name)
    w = WIDE.get(name, route.W_PWR if name in route.PLANE_NETS else route.W_SIG)
    is_plane = name in route.PLANE_NETS or name in route.POURS or name in r.extra or name == "VBUS"
    if name in route.POURS:
        x1, y1, x2, y2 = route.POURS[name]
        plane = lambda x, y: x1 + 0.5 <= x <= x2 - 0.5 and y1 + 0.5 <= y <= y2 - 0.5
    elif name == "GND":
        plane = lambda x, y: not (route.ANT[0] < x < route.ANT[2] and y > route.ANT[1])
    elif name in route.PLANE_NETS:
        plane = lambda x, y: route.plane_net(x, y) == name
    else:
        plane = None
    nx = G.nx
    conn = set()            # cells already joined to the net's tree (or, for plane nets, to the plane)
    own = G.ownvia.get(net, ())
    cu = G.cu

    def join(cells):
        """Add cells to the tree, together with any hand-drawn copper of this net they touch."""
        stack = [c for c in cells if c not in conn]
        conn.update(stack)
        while stack:
            l, i = stack.pop()
            for d in (1, -1, nx, -nx, nx + 1, nx - 1, -nx + 1, -nx - 1):
                k = (l, i + d)
                if k not in conn and cu[l][i + d] == net and k not in r.padcells:   # never hop into another pad
                    conn.add(k)
                    stack.append(k)
            if i in own:
                k = (1 - l, i)
                if k not in conn:
                    conn.add(k)
                    stack.append(k)

    join(r.extra.get(name, ()))
    todo = list(pads)
    if is_plane:
        for p in pads:
            if p[5]:        # through-hole pads meet the plane by themselves
                join([(l, iy * nx + ix) for l, ix, iy in p[4]])
        join([(l, i) for i in own for l in (0, 1)])      # vias drawn by hand already reach the plane
        for p in pads:
            if (p[0], p[1]) in r.pre:
                join([(l, iy * nx + ix) for l, ix, iy in p[4]])
        todo = [p for p in pads if not p[5] and (p[0], p[1]) not in r.pre]
    else:
        # start from the pad closest to the middle of the net
        mx = sum(p[2][0] for p in pads) / len(pads)
        my = sum(p[2][1] for p in pads) / len(pads)
        todo.sort(key=lambda p: (p[2][0] - mx) ** 2 + (p[2][1] - my) ** 2)
        first = todo.pop(0)
        join([(l, iy * nx + ix) for l, ix, iy in first[4]])
    fails = []
    retry = []
    passes = 0
    while todo:
        if conn and not is_plane:
            # nearest remaining pad to anything connected: cheap estimate via pad centres already connected
            done = [p for p in pads if p not in todo]
            todo.sort(key=lambda p: min((p[2][0] - q[2][0]) ** 2 + (p[2][1] - q[2][1]) ** 2 for q in done))
        p = todo.pop(0)
        cells = p[4]
        if any((l, iy * nx + ix) in conn for l, ix, iy in cells):
            continue
        cx, cy = p[2]
        ix0, iy0 = G.cell(cx, cy)
        res = None
        if is_plane:
            m = int((8 if name in route.PLANE_NETS else 45) / RES)    # hand-drawn trunks can be far away
            win = (max(0, ix0 - m), max(0, iy0 - m), min(G.nx - 1, ix0 + m), min(G.ny - 1, iy0 + m))
            res = G.route(net, cells, lambda l, i: (l, i) in conn, plane=plane, window=win,
                          max_expand=120000 if name in route.PLANE_NETS else 600000,
                          via_cost=12.0, plane_via_cost=5.0)
        else:
            # aim at the nearest connected pad; any copper of the net ends the search
            done = [q for q in pads if q is not p and any((l, iy * nx + ix) in conn for l, ix, iy in q[4])]
            tgt = min(done, key=lambda q: (q[2][0] - cx) ** 2 + (q[2][1] - cy) ** 2)[2] if done else None
            for grow, lim in ((6, 150000), (1000, 600000)):
                if tgt:
                    tix, tiy = G.cell(*tgt)
                    m = int(grow / RES)
                    win = (max(0, min(ix0, tix) - m), max(0, min(iy0, tiy) - m), min(G.nx - 1, max(ix0, tix) + m),
                           min(G.ny - 1, max(iy0, tiy) + m))
                else:
                    win, tix, tiy = None, None, None
                res = G.route(net, cells, lambda l, i: (l, i) in conn, window=win, max_expand=lim, via_cost=25.0,
                              target=(tix, tiy) if tgt else None)
                if res:
                    break
        if not res:
            retry.append(p)
            if not todo and passes < 2 and len(retry) > 0:
                todo, retry, passes = retry, [], passes + 1
            continue
        path, kind = res
        emit(r, name, path, w, p, conn)
        join([(l, iy * nx + ix) for l, ix, iy in cells])
        if not todo and retry and passes < 2:
            todo, retry, passes = retry, [], passes + 1
    fails = [f"{p[0]}.{p[1]}" for p in retry]
    return fails


def emit(r, name, path, w, startpad, conn):
    G = r.G
    nx = G.nx
    runs = []
    cur = []
    vias = []
    for k, (l, ix, iy) in enumerate(path):
        if l == 2:                                # plane via at the previous cell
            vias.append((ix * RES, iy * RES))
            continue
        if cur and cur[0] != l:
            runs.append(cur)
            vias.append((ix * RES, iy * RES))
            cur = []
        if not cur:
            cur = [l]
        cur.append((ix, iy))
        conn.add((l, iy * nx + ix))
    if cur:
        runs.append(cur)
    for i, run in enumerate(runs):
        l = run[0]
        pts = [(ix * RES, iy * RES) for ix, iy in run[1:]]
        # join the pad centres at either end
        a = r.padcells.get((l, run[1][1] * nx + run[1][0]))
        if i == 0:
            a = startpad[2] if l in startpad[3] else a
        if a and (abs(a[0] - pts[0][0]) > 1e-6 or abs(a[1] - pts[0][1]) > 1e-6):
            pts.insert(0, a)
        b = r.padcells.get((l, run[-1][1] * nx + run[-1][0]))
        if b and i == len(runs) - 1 and (abs(b[0] - pts[-1][0]) > 1e-6 or abs(b[1] - pts[-1][1]) > 1e-6):
            if G.cu[l][run[-1][1] * nx + run[-1][0]] == G.nid(name):
                pts.append(b)
        if len(pts) >= 2:
            r.track(simplify(pts), name, w, "F" if l == 0 else "B")
    for x, y in vias:
        if (round(x, 2), round(y, 2)) not in r.viapos:
            r.via(x, y, name)
        i = int(round(y / RES)) * nx + int(round(x / RES))
        conn.add((0, i))
        conn.add((1, i))
