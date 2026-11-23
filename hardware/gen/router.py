"""A small steered path-finder for the slow nets.

Two routing layers (F and B) on a 0.1 mm grid.  Every obstacle is rasterised twice: grown by clearance + half a
track (where a track centre may go) and by clearance + via radius (where a via centre may go).  A cell holds 0
(free), a net id (only that net may enter) or -1 (nobody).  Nets are routed one at a time, pin to nearest copper
of the same net, with A*; power nets may also end on a via into their plane.
"""
import heapq
import math
from array import array

RES = 0.1
CLR = 0.15         # grid clearance for 0.15 mm tracks; 0.2 mm power tracks end up at 0.125 (board rule 0.125)
TRACK = 0.15
VIA_D, VIA_H = 0.5, 0.3
SQ2 = math.sqrt(2)
DIRS = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0), (1, 1, SQ2), (1, -1, SQ2), (-1, 1, SQ2), (-1, -1, SQ2)]


class Grid:
    def __init__(self, w, h):
        self.nx, self.ny = int(round(w / RES)) + 1, int(round(h / RES)) + 1
        n = self.nx * self.ny
        self.trk = [array("h", [0]) * n, array("h", [0]) * n]   # F, B: track centres
        self.via = [array("h", [0]) * n, array("h", [0]) * n]   # F, B: via centres
        self.cu = [array("h", [0]) * n, array("h", [0]) * n]    # actual copper owner (for "already connected")
        self.netid = {}
        self.ownvia = {}     # net id -> cell indices holding a via of that net (layer changes always allowed)

    def nid(self, name):
        if name not in self.netid:
            self.netid[name] = len(self.netid) + 1
        return self.netid[name]

    def idx(self, ix, iy):
        return iy * self.nx + ix

    def cell(self, x, y):
        return int(round(x / RES)), int(round(y / RES))

    # ---- rasterising
    def _paint(self, arr, pred, x1, y1, x2, y2, net):
        ix1, iy1 = max(0, int(math.floor(x1 / RES))), max(0, int(math.floor(y1 / RES)))
        ix2, iy2 = min(self.nx - 1, int(math.ceil(x2 / RES))), min(self.ny - 1, int(math.ceil(y2 / RES)))
        nx = self.nx
        for iy in range(iy1, iy2 + 1):
            y = iy * RES
            base = iy * nx
            for ix in range(ix1, ix2 + 1):
                if pred(ix * RES, y):
                    v = arr[base + ix]
                    if v == 0:
                        arr[base + ix] = net
                    elif v != net:
                        arr[base + ix] = -1

    def rect(self, layers, x1, y1, x2, y2, net, grow_t=CLR + TRACK / 2, grow_v=CLR + VIA_D / 2, via_block=False):
        """Copper rectangle.  via_block: no via centre may land in the grown area, even of the same net (pads)."""
        def box(g):
            ax, ay, bx, by = x1 - g - 1e-6, y1 - g - 1e-6, x2 + g + 1e-6, y2 + g + 1e-6
            return (lambda x, y: ax <= x <= bx and ay <= y <= by), ax, ay, bx, by

        for l in layers:
            self._paint(self.trk[l], *box(grow_t), net)
            self._paint(self.via[l], *box(grow_v), -1 if via_block else net)
            if net > 0:
                self._paint(self.cu[l], *box(-0.04), net)

    def circle(self, layers, cx, cy, r, net, via_block=False):
        for l in layers:
            for arr, g, n in ((self.trk[l], CLR + TRACK / 2, net), (self.via[l], CLR + VIA_D / 2, -1 if via_block else net)):
                rr = (r + g) ** 2
                self._paint(arr, lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= rr, cx - r - g, cy - r - g, cx + r + g,
                            cy + r + g, n)
            if net > 0:
                rr = max(r - 0.04, 0.05) ** 2
                self._paint(self.cu[l], lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= rr, cx - r, cy - r, cx + r, cy + r, net)

    def seg(self, layer, x1, y1, x2, y2, halfw, net, copper=True):
        dx, dy = x2 - x1, y2 - y1
        L2 = dx * dx + dy * dy

        def dist2(x, y):
            if L2 == 0:
                return (x - x1) ** 2 + (y - y1) ** 2
            t = max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / L2))
            return (x - x1 - t * dx) ** 2 + (y - y1 - t * dy) ** 2

        for arr, g in ((self.trk[layer], CLR + TRACK / 2), (self.via[layer], CLR + VIA_D / 2)):
            rr = (halfw + g) ** 2
            self._paint(arr, lambda x, y: dist2(x, y) <= rr, min(x1, x2) - halfw - g, min(y1, y2) - halfw - g,
                        max(x1, x2) + halfw + g, max(y1, y2) + halfw + g, net)
        if not copper:
            return
        rr = (halfw + 0.02) ** 2
        self._paint(self.cu[layer], lambda x, y: dist2(x, y) <= rr, min(x1, x2) - halfw, min(y1, y2) - halfw,
                    max(x1, x2) + halfw, max(y1, y2) + halfw, net)

    def block(self, x1, y1, x2, y2, layers=(0, 1)):
        for l in layers:
            self._paint(self.trk[l], lambda x, y: True, x1, y1, x2, y2, -1)
            self._paint(self.via[l], lambda x, y: True, x1, y1, x2, y2, -1)

    def block_tracks(self, x1, y1, x2, y2, layers=(0, 1)):
        for l in layers:
            self._paint(self.trk[l], lambda x, y: True, x1, y1, x2, y2, -1)

    def block_vias(self, x1, y1, x2, y2):
        for l in (0, 1):
            self._paint(self.via[l], lambda x, y: True, x1, y1, x2, y2, -1)

    # ---- search
    def ok(self, l, i, net):
        v = self.trk[l][i]
        return v == 0 or v == net

    def via_ok(self, i, net):
        a, b = self.via[0][i], self.via[1][i]
        return (a == 0 or a == net) and (b == 0 or b == net) or i in self.ownvia.get(net, ())

    def route(self, net, starts, is_goal, plane=None, layer_cost=(1.0, 1.0), via_cost=20.0, plane_via_cost=6.0,
              allow_via=True, max_expand=400000, window=None, turn=0.4, target=None):
        """starts: [(layer, ix, iy)].  is_goal(l, i) -> bool for cells already on this net's copper.
        plane(x, y) -> True where a via reaches this net's plane.  Returns (path, end_kind) or None.
        path is [(l, ix, iy)] from a start to the goal."""
        nx = self.nx
        best = {}
        came = {}
        pq = []
        cnt = 0
        for l, ix, iy in starts:
            k = (l, iy * nx + ix)
            best[k] = 0.0
            came[k] = None
            heapq.heappush(pq, (0.0, cnt, 0.0, l, ix, iy, -1))
            cnt += 1
        if target:
            tx, ty = target

            def h(ix, iy):
                dx, dy = abs(ix - tx), abs(iy - ty)
                return 0.97 * (max(dx, dy) + 0.4142 * min(dx, dy))
        else:
            def h(ix, iy):
                return 0.0
        wx1, wy1, wx2, wy2 = window if window else (0, 0, self.nx - 1, self.ny - 1)
        expanded = 0
        while pq:
            _f, _, g, l, ix, iy, d = heapq.heappop(pq)
            i = iy * nx + ix
            if l == 2:                      # virtual node: via into the plane at this cell (d = layer we came from)
                return self._trace(came, (2, i, d)), "plane"
            k = (l, i)
            if g > best.get(k, 1e18) + 1e-9:
                continue
            if g > 0 and is_goal(l, i):
                return self._trace(came, k), "copper"
            if plane is not None and g > 0 and self.via_ok(i, net) and plane(ix * RES, iy * RES):
                gk = (2, i, l)
                if g + plane_via_cost < best.get(gk, 1e18):
                    best[gk] = g + plane_via_cost
                    came[gk] = k
                    heapq.heappush(pq, (g + plane_via_cost, cnt, g + plane_via_cost, 2, ix, iy, l))
                    cnt += 1
            expanded += 1
            if expanded > max_expand:
                return None
            lc = layer_cost[l]
            for di, (dx, dy, c) in enumerate(DIRS):
                jx, jy = ix + dx, iy + dy
                if jx < wx1 or jy < wy1 or jx > wx2 or jy > wy2:
                    continue
                j = jy * nx + jx
                if not self.ok(l, j, net):
                    continue
                if dx and dy and not (self.ok(l, iy * nx + jx, net) and self.ok(l, jy * nx + ix, net)):
                    continue
                ng = g + c * lc + (turn if d != -1 and d != di else 0.0)
                kk = (l, j)
                if ng < best.get(kk, 1e18):
                    best[kk] = ng
                    came[kk] = k
                    heapq.heappush(pq, (ng + h(jx, jy), cnt, ng, l, jx, jy, di))
                    cnt += 1
            if allow_via and self.via_ok(i, net):
                ol = 1 - l
                if self.ok(ol, i, net):
                    ng = g + (2.0 if i in self.ownvia.get(net, ()) else via_cost)   # reuse a via that is already there
                    kk = (ol, i)
                    if ng < best.get(kk, 1e18):
                        best[kk] = ng
                        came[kk] = k
                        heapq.heappush(pq, (ng + h(ix, iy), cnt, ng, ol, ix, iy, -1))
                        cnt += 1
        return None

    def _trace(self, came, k):
        out = []
        while k is not None:
            out.append(k)
            k = came[k]
        out.reverse()
        nx = self.nx
        return [(k[0], k[1] % nx, k[1] // nx) for k in out]


def simplify(pts):
    """Drop collinear interior points of a polyline given as [(x, y)]."""
    out = [pts[0]]
    for a, b in zip(pts[1:], pts[2:]):
        p = out[-1]
        if abs((a[0] - p[0]) * (b[1] - a[1]) - (a[1] - p[1]) * (b[0] - a[0])) > 1e-9:
            out.append(a)
    out.append(pts[-1])
    return out
