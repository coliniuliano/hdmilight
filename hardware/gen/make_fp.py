"""Generate the two footprints KiCad does not ship: the GSV2001 QFN-76 and the ESP32-S31-WROOM-1 module."""
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "lib", "hdmilight.pretty")
os.makedirs(OUT, exist_ok=True)


def header(name, descr, ref_y, val_y, smd=True):
    return f'''(footprint "{name}"
	(version 20240108)
	(generator "hdmilight_gen")
	(layer "F.Cu")
	(descr "{descr}")
	(property "Reference" "REF**" (at 0 {ref_y} 0) (layer "F.SilkS") (effects (font (size 0.8 0.8) (thickness 0.12))))
	(property "Value" "{name}" (at 0 {val_y} 0) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))))
	(attr smd)
'''


def line(x1, y1, x2, y2, layer, w=0.12):
    return f'\t(fp_line (start {x1:.4f} {y1:.4f}) (end {x2:.4f} {y2:.4f}) (stroke (width {w}) (type solid)) (layer "{layer}"))\n'


def rect(x1, y1, x2, y2, layer, w=0.12):
    return (line(x1, y1, x2, y1, layer, w) + line(x2, y1, x2, y2, layer, w) +
            line(x2, y2, x1, y2, layer, w) + line(x1, y2, x1, y1, layer, w))


def pad(num, x, y, w, h, layers='"F.Cu" "F.Paste" "F.Mask"', rr=0.25, extra=""):
    return (f'\t(pad "{num}" smd roundrect (at {x:.4f} {y:.4f}) (size {w:.4f} {h:.4f}) (layers {layers}) '
            f'(roundrect_rratio {rr}){extra})\n')


def qfn76():
    """GSV2001: QFN76L 9x9, e=0.4, b=0.2, L=0.4, exposed pad 6.3x6.3 (datasheet Figure 10).
    Pads run 0.475 mm past the body so an iron can reach them."""
    name = "QFN-76-1EP_9x9mm_P0.4mm_EP6.3x6.3mm"
    s = header(name, "GSV2001 QFN76L 9x9mm, 0.4mm pitch, EP 6.3x6.3mm (Gscoolink GSV2001 spec Rev 2.0 Figure 10)", -6.1, 6.1)
    pc, pl, pw = 4.5, 0.95, 0.2
    n = 19
    num = 1
    for side in range(4):
        for i in range(n):
            off = (i - (n - 1) / 2) * 0.4
            if side == 0:   # left, top to bottom
                s += pad(num, -pc, off, pl, pw)
            elif side == 1:  # bottom, left to right
                s += pad(num, off, pc, pw, pl)
            elif side == 2:  # right, bottom to top
                s += pad(num, pc, -off, pl, pw)
            else:           # top, right to left
                s += pad(num, -off, -pc, pw, pl)
            num += 1
    s += pad(77, 0, 0, 6.3, 6.3, layers='"F.Cu" "F.Mask"', rr=0.02)
    # paste: 4x4 windows, about half the pad area
    for i in range(4):
        for j in range(4):
            s += (f'\t(pad "" smd roundrect (at {(i - 1.5) * 1.5:.3f} {(j - 1.5) * 1.5:.3f}) (size 1.1 1.1) '
                  f'(layers "F.Paste") (roundrect_rratio 0.2))\n')
    s += rect(-4.5, -4.5, 4.5, 4.5, "F.Fab", 0.1)
    s += rect(-5.25, -5.25, 5.25, 5.25, "F.CrtYd", 0.05)
    c = 4.62
    for sx, sy in ((1, 1), (1, -1), (-1, 1)):
        s += line(sx * c, sy * c, sx * c, sy * (c - 0.5), "F.SilkS")
        s += line(sx * c, sy * c, sx * (c - 0.5), sy * c, "F.SilkS")
    s += f'\t(fp_circle (center -5.4 -4.0) (end -5.25 -4.0) (stroke (width 0.3) (type solid)) (fill solid) (layer "F.SilkS"))\n'
    s += ")\n"
    open(os.path.join(OUT, name + ".kicad_mod"), "w").write(s)


def esp32s31():
    """ESP32-S31-WROOM-1 land pattern, datasheet v0.5 Figure 11-1.  Origin = centre of the perimeter pad array.
    Unrotated: antenna at the top (-y), pin 1 top-left."""
    name = "ESP32-S31-WROOM-1"
    s = header(name, "ESP32-S31-WROOM-1 module 18x25.5mm (datasheet v0.5 Figure 11-1)", -17.2, 10.6)
    # 40 perimeter pads 1.5 x 0.9 at 1.27 pitch
    for i in range(14):
        s += pad(1 + i, -8.75, -8.88 + i * 1.27, 1.5, 0.9, rr=0.15)
        s += pad(40 - i, 8.75, -8.88 + i * 1.27, 1.5, 0.9, rr=0.15)
    for i in range(12):
        s += pad(15 + i, -6.985 + i * 1.27, 8.88, 0.9, 1.5, rr=0.15)
    # 9 ground pads 0.9 x 0.9 on a 1.4 pitch; the middle column is 7.5 mm from the module's left edge (x = -1.5)
    for gx in (-2.9, -1.5, -0.1):
        for gy in (-2.56, -1.16, 0.24):
            s += pad(41, gx, gy, 0.9, 0.9, rr=0.1)
    # 20 inner signal pads 0.4 x 0.8 at 0.8 pitch.  Only DM/DP (54, 55) are used on this board: the other 18 get
    # copper but no paste, so nothing under the module can bridge except the two joints that matter.
    nopaste = '"F.Cu" "F.Mask"'
    for i in range(8):   # 42..49 along the top, right to left
        s += pad(42 + i, 1.3 - i * 0.8, -3.76, 0.4, 0.8, rr=0.2, layers=nopaste)
    for i in range(6):   # 50..55 down the left
        s += (pad(50 + i, -4.1, -2.76 + i * 0.8, 0.8, 0.4, rr=0.2) if 50 + i in (54, 55) else
              pad(50 + i, -4.1, -2.76 + i * 0.8, 0.8, 0.4, rr=0.2, layers=nopaste))
    for i in range(6):   # 56..61 up the right
        s += pad(56 + i, 1.1, 1.24 - i * 0.8, 0.8, 0.4, rr=0.2, layers=nopaste)
    top, bot = -8.88 - 7.49, -8.88 - 7.49 + 25.5
    s += rect(-9, top, 9, bot, "F.Fab", 0.1)
    s += line(-9, top + 6, 9, top + 6, "F.Fab", 0.1)
    s += rect(-9.9, top - 0.25, 9.9, bot + 0.75, "F.CrtYd", 0.05)
    s += line(-9.12, top, 9.12, top, "F.SilkS") + line(-9.12, top, -9.12, -9.7, "F.SilkS") + line(9.12, top, 9.12, -9.7, "F.SilkS")
    s += line(-9.12, bot + 0.1, -7.8, bot + 0.1, "F.SilkS") + line(9.12, bot + 0.1, 7.8, bot + 0.1, "F.SilkS")
    s += f'\t(fp_circle (center -10.1 -8.88) (end -9.95 -8.88) (stroke (width 0.3) (type solid)) (fill solid) (layer "F.SilkS"))\n'
    s += f'\t(fp_text user "ANTENNA" (at 0 {top + 3:.2f} 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))\n'
    s += ")\n"
    open(os.path.join(OUT, name + ".kicad_mod"), "w").write(s)


if __name__ == "__main__":
    qfn76()
    esp32s31()
    print("footprints written to", os.path.abspath(OUT))
