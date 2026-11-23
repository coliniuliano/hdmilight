"""Copy a board with solder paste removed from some footprints (run with KiCad's python; used by make_fab.py).

strip_paste.py in.kicad_pcb out.kicad_pcb without R1,U2   -> no paste on the listed parts
strip_paste.py in.kicad_pcb out.kicad_pcb only U1,U2      -> paste on the listed parts only
"""
import sys

import pcbnew

src, dst, mode, refs = sys.argv[1], sys.argv[2], sys.argv[3], set(sys.argv[4].split(","))
b = pcbnew.LoadBoard(src)
todo = []                       # gather first: removing a pad upsets later iteration in KiCad's python
for fp in b.GetFootprints():
    if (mode == "without") == (fp.GetReference() in refs):
        todo += [(fp, pad) for pad in fp.Pads()]
for fp, pad in todo:
    ls = pad.GetLayerSet()
    if not ls.Contains(pcbnew.F_Paste):
        continue
    ls.RemoveLayer(pcbnew.F_Paste)
    if len(ls.Seq()):
        pad.SetLayerSet(ls)
    else:
        fp.Remove(pad)          # paste-only aperture
pcbnew.SaveBoard(dst, b)
