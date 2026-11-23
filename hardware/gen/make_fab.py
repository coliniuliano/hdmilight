"""Write the files JLCPCB needs for a quote or an order into ../fab:

  hdmilight_gerbers.zip   Gerbers + drill files (upload on the PCB order page)
  bom_jlcpcb.csv          Comment, Designator, Footprint, LCSC Part #  (assembly BOM)
  cpl_jlcpcb.csv          Designator, Mid X, Mid Y, Layer, Rotation    (placement)
  parts_not_assembled.csv what JLCPCB will not fit, and why

The paste layer sent to JLCPCB leaves out U1, U2 and every unfitted part, so those pads arrive flat and clean
instead of carrying reflowed solder bumps.

Run after stage.py.  `python3 make_fab.py prices` also asks JLCPCB for current prices and prints a parts-cost estimate.
"""
import csv
import math
import os
import re
import shutil
import subprocess
import sys
import zipfile
from collections import OrderedDict

import design

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
FAB = os.path.join(ROOT, "fab")
PCB = os.path.join(ROOT, "hdmilight.kicad_pcb")
# kicad-cli and the python KiCad ships (pcbnew): $KICAD_CLI / $KICAD_PYTHON, else on PATH, else KiCad.app on macOS
CLI = os.environ.get("KICAD_CLI") or shutil.which("kicad-cli") or "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
LAYERS = "F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts"
PY = os.environ.get("KICAD_PYTHON") or "/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3"
# JLCPCB places each part at the rotation in this file relative to the part as drawn in its own library, which is
# not always how KiCad draws it.  These offsets come from fitting each part's LCSC library footprint pad for pad
# onto the KiCad footprint (pre-order review).  Per-part entries win over the package patterns.
# The placement preview on JLCPCB's order page is still the final check: compare every rotation against the
# part's pin-1 mark there, U3 above all.
ROT_PART = {"C5905137": 0,      # MS2130: library pin 1 is where KiCad's is
            "C7519": 270,       # USBLC6-2SC6 (SOT-23-6)
            "C40960": 270}      # RClamp0524P
ROT = [(r"^SOT-23", 180), (r"^SOIC-", 270)]
# Parts whose KiCad anchor is not the body centre that JLCPCB expects: (dx, dy) in the footprint's own frame.
CENTRE = [(r"^HDMI_A_Amphenol", (0.0, -2.05)), (r"^USB_C_Receptacle_HRO", (0.0, -1.42))]
def stripped(dst, mode, refs):
    subprocess.run([PY, os.path.join(HERE, "strip_paste.py"), PCB, dst, mode, ",".join(sorted(refs))], check=True,
                   capture_output=True)
    return dst


def assembled(p):
    return not (p["dnp"] or p["kind"] in ("TP", "MH") or p["ref"] in design.NOT_ASSEMBLED or p["ref"] in design.HAND_FIT)


def gerbers():
    g = os.path.join(FAB, "gerbers")
    shutil.rmtree(g, ignore_errors=True)
    os.makedirs(g)
    hand = {r for r, p in design.PARTS.items() if not assembled(p) and p["kind"] not in ("TP", "MH")
            and r not in design.HAND_FIT}
    tmp = os.path.join(FAB, "tmp")
    os.makedirs(tmp, exist_ok=True)
    jlc = stripped(os.path.join(tmp, "hdmilight.kicad_pcb"), "without", hand)
    subprocess.run([CLI, "pcb", "export", "gerbers", "-o", g + "/", "-l", LAYERS, "--no-x2", "--no-netlist",
                    "--subtract-soldermask", "--exclude-value", jlc], check=True, capture_output=True)
    for n in os.listdir(g):
        if n.endswith(".gbrjob"):          # not a layer, and its stackup is KiCad's default, not JLC04161H-7628
            os.remove(os.path.join(g, n))
    shutil.rmtree(tmp)
    subprocess.run([CLI, "pcb", "export", "drill", "-o", g + "/", "--format", "excellon", "--excellon-units", "mm",
                    "--excellon-zeros-format", "decimal", "--excellon-separate-th", "--drill-origin", "absolute", PCB],
                   check=True, capture_output=True)
    z = os.path.join(FAB, "hdmilight_gerbers.zip")
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as f:
        for n in sorted(os.listdir(g)):
            f.write(os.path.join(g, n), n)
    return sorted(os.listdir(g))


def bom():
    rows = OrderedDict()
    for p in design.PARTS.values():
        if assembled(p):
            key = (p["lcsc"], p["fp"].split(":")[1])           # one row per LCSC part, as JLCPCB wants
            rows.setdefault(key, [p["mpn"] or p["value"], []])[1].append(p["ref"])
    with open(os.path.join(FAB, "bom_jlcpcb.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for (lcsc, fp), (val, refs) in rows.items():
            w.writerow([val, ",".join(refs), fp, lcsc])
    with open(os.path.join(FAB, "parts_not_assembled.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Value", "LCSC Part #", "Why"])
        for p in design.PARTS.values():
            if p["kind"] in ("TP", "MH"):
                continue
            if p["dnp"]:
                w.writerow([p["ref"], p["value"], p["lcsc"], "do not fit: optional part"])
            elif p["ref"] in design.NOT_ASSEMBLED:
                w.writerow([p["ref"], p["value"], p["lcsc"], design.NOT_ASSEMBLED[p["ref"]]])
            elif p["ref"] in design.HAND_FIT:
                w.writerow([p["ref"], p["mpn"] or p["value"], p["lcsc"], design.HAND_FIT[p["ref"]]])
    return rows


def cpl():
    tmp = os.path.join(FAB, "pos_kicad.csv")
    subprocess.run([CLI, "pcb", "export", "pos", "-o", tmp, "--format", "csv", "--units", "mm", "--side", "front", PCB],
                   check=True, capture_output=True)
    n = 0
    with open(tmp) as f, open(os.path.join(FAB, "cpl_jlcpcb.csv"), "w", newline="") as o:
        w = csv.writer(o)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for r in csv.DictReader(f):
            p = design.PARTS.get(r["Ref"])
            if not p or not assembled(p):
                continue
            rot = float(r["Rot"])
            x, y = float(r["PosX"]), float(r["PosY"])
            for pat, (dx, dy) in CENTRE:
                if re.match(pat, r["Package"]):
                    a = math.radians(rot)                      # KiCad rotation; the position file's y axis points up
                    x += dx * math.cos(a) + dy * math.sin(a)
                    y -= -dx * math.sin(a) + dy * math.cos(a)
            if p["lcsc"] in ROT_PART:
                rot += ROT_PART[p["lcsc"]]
            else:
                for pat, add in ROT:
                    if re.match(pat, r["Package"]):
                        rot += add
            w.writerow([r["Ref"], f"{x:.4f}mm", f"{y:.4f}mm", "Top", f"{rot % 360:.0f}"])
            n += 1
    os.remove(tmp)
    return n


def prices(rows):
    import jlc
    total, ext = 0.0, []
    print(f"{'LCSC':10} {'type':5} {'qty':>3} {'each':>8} {'line':>7}  {'stock':>9}  part")
    for (lcsc, fp), (val, refs) in rows.items():
        hit = [c for c in jlc.query(lcsc, 5) if c.get("lcsc") == lcsc]
        if not hit:
            print(f"{lcsc:10} NOT FOUND  {val}")
            continue
        c = hit[0]
        price = float(c["price"] or 0)
        total += price * len(refs)
        if c["lib"] != "base":
            ext.append(lcsc)
        print(f"{lcsc:10} {'basic' if c['lib'] == 'base' else 'ext':5} {len(refs):>3} {price:>8.4f} {price * len(refs):>7.3f}  {c['stock']:>9}  {val} ({c['mpn']})")
    print(f"\nparts per board at qty-1 prices: ${total:.2f}   extended lines: {len(ext)} (x $3 feeder fee per order = ${3 * len(ext)})")


if __name__ == "__main__":
    os.makedirs(FAB, exist_ok=True)
    files = gerbers()
    rows = bom()
    n = cpl()
    print(len(files), "gerber/drill files:", " ".join(files))
    print(len(rows), "BOM lines,", n, "placements,", sum(len(v[1]) for v in rows.values()), "parts fitted by JLCPCB")
    if len(sys.argv) > 1 and sys.argv[1] == "prices":
        prices(rows)
