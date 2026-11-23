"""Small helpers over KiCad's pcbnew API: build a board from design.PARTS, place, draw tracks and vias."""
import math
import os
import pcbnew
from pcbnew import FromMM, ToMM, VECTOR2I

HERE = os.path.dirname(os.path.abspath(__file__))
# KiCad's stock footprint libraries: $KICAD_FOOTPRINT_DIR, else KiCad.app on macOS
KLIB = os.environ.get("KICAD_FOOTPRINT_DIR") or "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
M3D = "${KICAD10_3DMODEL_DIR}"
# 3D models for the footprints that come without one (or name a file KiCad does not ship).  All are stock KiCad
# models of a part with the same body: footprint -> (model, offset in mm).  Used for renders only.
MODELS = {
    # ESP32-S3-WROOM-1 is the same 18 x 25.5 mm module; its origin is 3.62 mm from ours
    "hdmilight:ESP32-S31-WROOM-1": ("RF_Module.3dshapes/ESP32-S3-WROOM-1.step", (0, 3.62, 0)),
    "hdmilight:QFN-76-1EP_9x9mm_P0.4mm_EP6.3x6.3mm":
        ("Package_DFN_QFN.3dshapes/QFN-76-1EP_9x9mm_P0.4mm_EP5.81x6.31mm.step", (0, 0, 0)),
    "Package_DFN_QFN:QFN-64-1EP_9x9mm_P0.5mm_EP5.7x5.7mm":
        ("Package_DFN_QFN.3dshapes/QFN-64-1EP_9x9mm_P0.5mm_EP6x6mm.step", (0, 0, 0)),
    # GCT USB4105 is another 16-pin top-mount receptacle with the same shell and peg positions
    "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12":
        ("Connector_USB.3dshapes/USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal.step", (0, 0, 0)),
}
LAYERS = {"F": pcbnew.F_Cu, "In1": pcbnew.In1_Cu, "In2": pcbnew.In2_Cu, "B": pcbnew.B_Cu}


def P(x, y):
    return VECTOR2I(FromMM(float(x)), FromMM(float(y)))


def set_model(fp):
    """Give a footprint its stand-in 3D model from MODELS, replacing whatever it had.  True if it changed."""
    key = "%s:%s" % (fp.GetFPID().GetLibNickname(), fp.GetFPID().GetLibItemName())
    if key not in MODELS:
        return False
    name, off = MODELS[key]
    m = pcbnew.FP_3DMODEL()
    m.m_Filename = M3D + "/" + name
    m.m_Offset.x, m.m_Offset.y, m.m_Offset.z = off
    fp.Models().clear()
    fp.Models().push_back(m)
    return True


class Board:
    def __init__(self, parts, w, h):
        self.b = pcbnew.BOARD()
        self.b.SetCopperLayerCount(4)
        self.parts = parts
        self.w, self.h = w, h
        self.fps = {}
        self.nets = {}
        self._fpcache = {}
        for p in parts.values():
            self._add(p)

    # ---- nets / footprints
    def net(self, name):
        if name not in self.nets:
            n = pcbnew.NETINFO_ITEM(self.b, name)
            self.b.Add(n)
            self.nets[name] = n
        return self.nets[name]

    def _add(self, p):
        lib, name = p["fp"].split(":")
        path = os.path.join(HERE, "..", "lib", "hdmilight.pretty") if lib == "hdmilight" else f"{KLIB}/{lib}.pretty"
        fp = pcbnew.FootprintLoad(os.path.abspath(path), name)
        fp.SetReference(p["ref"])
        fp.SetValue(p["value"])
        fp.SetFPID(pcbnew.LIB_ID(lib, name))
        set_model(fp)
        try:    # tie the footprint to its schematic symbol so KiCad's "update PCB from schematic" matches them
            import make_sch
            fp.SetPath(pcbnew.KIID_PATH("/" + make_sch.sheet_uuid(p["sheet"]) + "/" + make_sch.sym_uuid(p["ref"])))
            fp.SetSheetname("/" + make_sch.SHEETS[p["sheet"]] + "/")
            fp.SetSheetfile(p["sheet"] + ".kicad_sch")
        except Exception as e:      # the link is a convenience; the board is still valid without it
            print("no schematic link for", p["ref"], e)
        if p["dnp"]:
            fp.SetDNP(True)
        for key in ("mpn", "lcsc"):     # same fields as the schematic symbol
            if p[key]:
                try:
                    f = pcbnew.PCB_FIELD(fp, pcbnew.FIELD_T_USER, key.upper())
                    f.SetText(p[key])
                    f.SetVisible(False)
                    f.SetLayer(pcbnew.F_Fab)
                    fp.Add(f)
                except Exception as e:
                    print("field", key, p["ref"], e)
        for pad in fp.Pads():
            netname = p["pins"].get(pad.GetNumber())
            if netname:
                pad.SetNet(self.net(netname))
        self.b.Add(fp)
        self.fps[p["ref"]] = fp
        fp.SetPosition(P(-20, -20))

    def place(self, ref, x, y, rot=0, bottom=False):
        fp = self.fps[ref]
        fp.SetPosition(P(x, y))
        if bottom and fp.GetLayer() == pcbnew.F_Cu:
            fp.Flip(P(x, y), pcbnew.FLIP_DIRECTION_TOP_BOTTOM)
        fp.SetOrientationDegrees(rot)
        fp._placed = True

    def placed(self, ref):
        return getattr(self.fps[ref], "_placed", False)

    def pad(self, ref, num, which=0):
        """Centre of a pad in mm."""
        pads = [p for p in self.fps[ref].Pads() if p.GetNumber() == str(num)]
        c = pads[which].GetPosition()
        return (ToMM(c.x), ToMM(c.y))

    def padnet(self, ref, num):
        for p in self.fps[ref].Pads():
            if p.GetNumber() == str(num):
                return p.GetNetname()

    # ---- copper
    def track(self, pts, net, width, layer="F"):
        n = self.net(net)
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            if abs(x1 - x2) < 1e-6 and abs(y1 - y2) < 1e-6:
                continue
            t = pcbnew.PCB_TRACK(self.b)
            t.SetStart(P(x1, y1))
            t.SetEnd(P(x2, y2))
            t.SetWidth(FromMM(width))
            t.SetLayer(LAYERS[layer])
            t.SetNet(n)
            self.b.Add(t)

    def via(self, x, y, net, dia=0.5, drill=0.3):
        v = pcbnew.PCB_VIA(self.b)
        v.SetPosition(P(x, y))
        v.SetWidth(pcbnew.F_Cu, FromMM(dia)) if _via_layer_width else v.SetWidth(FromMM(dia))
        v.SetDrill(FromMM(drill))
        v.SetViaType(pcbnew.VIATYPE_THROUGH)
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        v.SetNet(self.net(net))
        self.b.Add(v)
        return (x, y)

    def zone(self, net, layer, pts, clearance=0.2, minw=0.2, prio=0, thermal=True):
        z = pcbnew.ZONE(self.b)
        z.SetLayer(LAYERS[layer])
        z.SetNet(self.net(net))
        o = z.Outline()
        o.NewOutline()
        for x, y in pts:
            o.Append(FromMM(x), FromMM(y))
        z.SetLocalClearance(FromMM(clearance))
        z.SetMinThickness(FromMM(minw))
        z.SetAssignedPriority(prio)
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL if thermal else pcbnew.ZONE_CONNECTION_FULL)
        z.SetThermalReliefGap(FromMM(0.25))
        z.SetThermalReliefSpokeWidth(FromMM(0.3))
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        self.b.Add(z)
        return z

    def keepout(self, pts, layers=("F", "In1", "In2", "B"), tracks=True, vias=True, copper=True, fps=False):
        z = pcbnew.ZONE(self.b)
        z.SetIsRuleArea(True)
        ls = pcbnew.LSET()
        for l in layers:
            ls.AddLayer(LAYERS[l])
        z.SetLayerSet(ls)
        o = z.Outline()
        o.NewOutline()
        for x, y in pts:
            o.Append(FromMM(x), FromMM(y))
        z.SetDoNotAllowTracks(tracks)
        z.SetDoNotAllowVias(vias)
        z.SetDoNotAllowZoneFills(copper)
        z.SetDoNotAllowPads(False)
        z.SetDoNotAllowFootprints(fps)
        self.b.Add(z)

    def edge(self, pts, closed=True):
        if closed:
            pts = list(pts) + [pts[0]]
        for a, b in zip(pts, pts[1:]):
            s = pcbnew.PCB_SHAPE(self.b)
            s.SetShape(pcbnew.SHAPE_T_SEGMENT)
            s.SetStart(P(*a))
            s.SetEnd(P(*b))
            s.SetLayer(pcbnew.Edge_Cuts)
            s.SetWidth(FromMM(0.1))
            self.b.Add(s)

    def text(self, s, x, y, size=1.0, layer=pcbnew.F_SilkS, rot=0, thick=None, knockout=False):
        t = pcbnew.PCB_TEXT(self.b)
        t.SetText(s)
        t.SetPosition(P(x, y))
        t.SetLayer(layer)
        t.SetTextSize(VECTOR2I(FromMM(size), FromMM(size)))
        t.SetTextThickness(FromMM(thick or size * 0.15))
        t.SetTextAngleDegrees(rot)
        if layer == pcbnew.B_SilkS:
            t.SetMirrored(True)
        if knockout:                    # solid block with the letters cut out of it
            t.SetIsKnockout(True)
        self.b.Add(t)
        return t

    def fill(self):
        pcbnew.ZONE_FILLER(self.b).Fill(self.b.Zones())

    def save(self, path):
        pcbnew.SaveBoard(path, self.b)


# KiCad 9+ takes a layer argument for via width
try:
    _v = pcbnew.PCB_VIA(pcbnew.BOARD())
    _v.SetWidth(pcbnew.F_Cu, 100000)
    _via_layer_width = True
except TypeError:
    _via_layer_width = False
