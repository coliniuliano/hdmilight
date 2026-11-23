"""Build hdmilight.kicad_pcb from design.PARTS: placement (place.py) and routing (route.py)."""
import os
import shutil
import subprocess
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import design
import kb
import place

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(os.path.join(HERE, "..", "hdmilight.kicad_pcb"))
# kicad-cli: $KICAD_CLI, else on PATH, else KiCad.app on macOS
CLI = os.environ.get("KICAD_CLI") or shutil.which("kicad-cli") or "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"


def courtyard_boxes(B):
    out = {}
    for ref, fp in B.fps.items():
        if not B.placed(ref):
            continue
        for layer in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
            cy = fp.GetCourtyard(layer)
            if cy.OutlineCount():
                bb = cy.BBox()
                out[ref] = (kb.ToMM(bb.GetLeft()), kb.ToMM(bb.GetTop()), kb.ToMM(bb.GetRight()), kb.ToMM(bb.GetBottom()),
                            layer)
    return out


def check_overlaps(B, verbose=True):
    boxes = courtyard_boxes(B)
    refs = sorted(boxes)
    n = 0
    for i, a in enumerate(refs):
        ax1, ay1, ax2, ay2, al = boxes[a]
        for b in refs[i + 1:]:
            bx1, by1, bx2, by2, bl = boxes[b]
            if al != bl:
                continue
            ox, oy = min(ax2, bx2) - max(ax1, bx1), min(ay2, by2) - max(ay1, by1)
            if ox > 0.01 and oy > 0.01:
                n += 1
                if verbose:
                    print(f"  overlap {a} / {b}: {ox:.2f} x {oy:.2f} mm")
    for r, (x1, y1, x2, y2, _) in boxes.items():
        if r[0] in "JH" or r == "U1":
            continue
        if x1 < 0.3 or y1 < 0.3 or x2 > B.w - 0.3 or y2 > B.h - 0.3:
            n += 1
            if verbose:
                print(f"  off board: {r} ({x1:.1f},{y1:.1f})-({x2:.1f},{y2:.1f})")
    return n


def render(name="top", layers="F.Cu,F.SilkS,F.CrtYd,Edge.Cuts", mirror=False, width=1800):
    d = os.path.join(HERE, "..", "out")
    os.makedirs(d, exist_ok=True)
    svg = os.path.join(d, name + ".svg")
    cmd = [CLI, "pcb", "export", "svg", "--mode-single", "-o", svg, "-l", layers, "--page-size-mode", "2",
           "--exclude-drawing-sheet", "--drill-shape-opt", "2"]
    if mirror:
        cmd.append("--mirror")
    subprocess.run(cmd + [OUT], check=True, capture_output=True)
    png = os.path.join(d, name + ".png")
    subprocess.run(["rsvg-convert", "-w", str(width), "-b", "white", svg, "-o", png], check=True)
    return png


def main(stage="place"):
    B = kb.Board(design.PARTS, place.W, place.H)
    place.outline(B)
    place.place_all(B)
    unplaced = [r for r in B.fps if not B.placed(r)]
    # park anything not placed yet below the board so it is visible
    for i, r in enumerate(unplaced):
        B.fps[r].SetPosition(kb.P(2 + (i % 26) * 3, place.H + 6 + (i // 26) * 4))
    print(f"{len(B.fps) - len(unplaced)} placed, {len(unplaced)} unplaced: {' '.join(unplaced[:60])}")
    print("placement problems:", check_overlaps(B))
    if stage != "place":
        import route
        route.route_all(B)
    B.save(OUT)
    return B


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "place")
    for a in sys.argv[2:]:
        if a == "render":
            print(render())
