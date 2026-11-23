"""Write kb.MODELS into the existing hdmilight.kicad_pcb as text, without rebuilding or re-saving the board
(a pcbnew round trip rewrites the whole file).  stage.py already applies the same table on a rebuild."""
import os
import re

MODELS, M3D = {}, None
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "kb.py")).read()   # kb.py imports pcbnew; only the table is needed
exec(src[src.index("M3D = "):src.index("LAYERS = ")])

PCB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hdmilight.kicad_pcb")


def block(name, off):
    return (f'\t\t(model "{M3D}/{name}"\n\t\t\t(offset\n\t\t\t\t(xyz {off[0]:g} {off[1]:g} {off[2]:g})\n\t\t\t)\n'
            '\t\t\t(scale\n\t\t\t\t(xyz 1 1 1)\n\t\t\t)\n\t\t\t(rotate\n\t\t\t\t(xyz 0 0 0)\n\t\t\t)\n\t\t)\n')


def main():
    parts = re.split(r'(?=\n\t\(footprint ")', open(PCB).read())
    n = 0
    for i, p in enumerate(parts):
        m = re.match(r'\n\t\(footprint "([^"]+)"', p)
        if not m or m.group(1) not in MODELS:
            continue
        end = re.search(r"\n\t\)(?=\n|$)", p).start() + 1                                     # the footprint's closing bracket
        body = re.sub(r'\t\t\(model ".*?\n\t\t\)\n', "", p[:end], flags=re.S)  # drop any model it has
        parts[i] = body + block(*MODELS[m.group(1)]) + p[end:]
        n += 1
    open(PCB, "w").write("".join(parts))
    print(n, "footprints given a model")


if __name__ == "__main__":
    main()
