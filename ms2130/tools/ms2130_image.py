"""MS2130 flash image breakdown: header, code, checksums, user data (EDID, HDCP keys, strings), and diff vs other images."""
import sys, hashlib, glob, os

CODE_OFF = 0x30
USER = [("edid", 0xF830, 0x100), ("hdcpUserKey", 0xF930, 0x120), ("ultrasemi", 0xFA50, 0x20), ("ultrasemi_version", 0xFA70, 0x20),
        ("ultrasemi_magic", 0xFA90, 0x20), ("ManufacturerString", 0xFAB0, 0x20), ("ProduceString", 0xFAD0, 0x20),
        ("U2ProduceString", 0xFAF0, 0x20), ("AudioString", 0xFB10, 0x20), ("U2AudioString", 0xFB30, 0x20),
        ("SerialnumString", 0xFB50, 0x20), ("U2SerialnumString", 0xFB70, 0x20), ("BCSH", 0xFB90, 0x20), ("AudioRoute", 0xFBB0, 0x20)]

def hsum(d, lo, hi, skip=()): return sum(b for i, b in enumerate(d[lo:hi], lo) if i not in skip) & 0xffff

def header(d):
    code_len = (d[2] << 8) | d[3]
    end = CODE_OFF + code_len
    hdr_ok = hsum(d, 2, CODE_OFF, range(0x0c, 0x10)) == ((d[end] << 8) | d[end + 1])
    code_ok = hsum(d, CODE_OFF, end) == ((d[end + 2] << 8) | d[end + 3])
    return code_len, end, hdr_ok, code_ok

def strings(d, lo, hi, minlen=6):
    out, cur = [], b""
    for b in d[lo:hi]:
        if 32 <= b < 127: cur += bytes([b])
        else:
            if len(cur) >= minlen: out.append(cur.decode())
            cur = b""
    return out

def ustr(b):
    n = b[0]
    return repr(b[1:1 + n].decode("ascii", "replace")) if 2 <= n <= 31 else f"(len byte {n:#x}, raw {b[:8].hex()}...)"

def edid(e):
    if e[:8] != b"\x00\xff\xff\xff\xff\xff\xff\x00": return ["not an EDID header: " + e[:8].hex()]
    m = (e[8] << 8) | e[9]
    mfg = "".join(chr(64 + ((m >> s) & 0x1f)) for s in (10, 5, 0))
    out = [f"manufacturer {mfg}, product 0x{e[11]:02x}{e[10]:02x}, serial 0x{int.from_bytes(e[12:16],'little'):08x}, week {e[16]} year {1990 + e[17]}, EDID {e[18]}.{e[19]}",
           f"video input 0x{e[20]:02x} ({'digital' if e[20] & 0x80 else 'analog'}), {e[21]}x{e[22]} cm, gamma {(e[23] + 100) / 100:.2f}, features 0x{e[24]:02x}, extensions {e[0x7e]}",
           f"block0 checksum {'ok' if sum(e[:128]) & 0xff == 0 else 'BAD'}" + (f", block1 checksum {'ok' if sum(e[128:256]) & 0xff == 0 else 'BAD'}" if e[0x7e] else "")]
    for i in range(4):
        dsc = e[0x36 + i * 18: 0x36 + (i + 1) * 18]
        if dsc[0] | dsc[1]:
            pix = (dsc[0] | dsc[1] << 8) / 100
            h = dsc[2] | (dsc[4] & 0xf0) << 4; hb = dsc[3] | (dsc[4] & 0x0f) << 8
            v = dsc[5] | (dsc[7] & 0xf0) << 4; vb = dsc[6] | (dsc[7] & 0x0f) << 8
            out.append(f"detailed timing {h}x{v} @ {pix:.2f} MHz (total {h + hb}x{v + vb}, ~{pix * 1e6 / ((h + hb) * (v + vb)):.1f} Hz)")
        elif dsc[3] == 0xfc: out.append("monitor name " + repr(dsc[5:18].decode('ascii', 'replace').rstrip('\n ')))
        elif dsc[3] == 0xfd: out.append(f"range limits V {dsc[5]}-{dsc[6]} Hz, H {dsc[7]}-{dsc[8]} kHz, pixclk <= {dsc[9] * 10} MHz")
        elif dsc[3] == 0xff: out.append("serial string " + repr(dsc[5:18].decode('ascii', 'replace').rstrip('\n ')))
        else: out.append(f"descriptor tag 0x{dsc[3]:02x}")
    if e[0x7e] and e[0x80] == 2:
        out.append(f"CEA-861 rev {e[0x81]}, DTD offset {e[0x82]}, flags 0x{e[0x83]:02x} (underscan {bool(e[0x83]&0x80)}, audio {bool(e[0x83]&0x40)}, YCbCr444 {bool(e[0x83]&0x20)}, YCbCr422 {bool(e[0x83]&0x10)})")
        i = 4
        while i < e[0x82]:
            tag, ln = e[0x80 + i] >> 5, e[0x80 + i] & 0x1f; body = e[0x81 + i: 0x81 + i + ln]
            if tag == 1: out.append("  audio block: " + " ".join(f"fmt{b[0] >> 3}/{(b[0] & 7) + 1}ch sr=0x{b[1]:02x} b2=0x{b[2]:02x}" for b in (body[k:k + 3] for k in range(0, ln, 3))))
            elif tag == 2: out.append("  video block VICs: " + " ".join(f"{b & 0x7f}{'*' if b & 0x80 else ''}" for b in body))
            elif tag == 3:
                oui = body[0] | body[1] << 8 | body[2] << 16
                if oui == 0x000c03: out.append(f"  HDMI VSDB: physical address {body[3] >> 4}.{body[3] & 15}.{body[4] >> 4}.{body[4] & 15}, flags 0x{body[5]:02x}" + (f", max TMDS {body[6] * 5} MHz" if ln > 6 else "") + f", raw {body.hex()}")
                elif oui == 0xc45dd8: out.append(f"  HDMI Forum VSDB: version {body[3]}, max TMDS char rate {body[4] * 5} MHz, flags {body[5:].hex()}")
                else: out.append(f"  vendor block OUI {oui:06x}: {body.hex()}")
            elif tag == 4: out.append(f"  speaker allocation {body.hex()}")
            elif tag == 7: out.append(f"  extended tag {body[0]}: {body[1:].hex()}")
            else: out.append(f"  tag {tag} len {ln}: {body.hex()}")
            i += ln + 1
        for k in range(0x80 + e[0x82], 0xfe, 18):
            dsc = e[k:k + 18]
            if dsc[0] | dsc[1]:
                pix = (dsc[0] | dsc[1] << 8) / 100; h = dsc[2] | (dsc[4] & 0xf0) << 4; hb = dsc[3] | (dsc[4] & 0x0f) << 8; v = dsc[5] | (dsc[7] & 0xf0) << 4; vb = dsc[6] | (dsc[7] & 0x0f) << 8
                out.append(f"  DTD {h}x{v} @ {pix:.2f} MHz (~{pix * 1e6 / ((h + hb) * (v + vb)):.1f} Hz)")
    return out

def hdcp(k):
    ksv = k[:5]; ones = sum(bin(b).count("1") for b in ksv)
    out = [f"first 5 bytes {ksv.hex()} ({ones} ones{' = valid KSV' if ones == 20 else ''}), bytes 5..287 nonzero: {sum(1 for b in k[5:] if b)}/{len(k) - 5}, all 0xff: {all(b == 0xff for b in k)}, all 0x00: {all(b == 0 for b in k)}"]
    return out

def main():
    mine = open(sys.argv[1], "rb").read()
    code_len, end, hok, cok = header(mine)
    print(f"== {sys.argv[1]}: {len(mine)} bytes, header {mine[:4].hex()}, code {code_len} bytes (0x{CODE_OFF:x}..0x{end:x}), header checksum {'ok' if hok else 'BAD'}, code checksum {'ok' if cok else 'BAD'}")
    print(f"   header bytes: {mine[:CODE_OFF].hex()}")
    print(f"   sha256 code: {hashlib.sha256(mine[CODE_OFF:end]).hexdigest()}")
    ds = [s for s in strings(mine, CODE_OFF, end) if any(mo in s for mo in ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")) and any(c.isdigit() for c in s)]
    print(f"   date-like strings: {ds[:6]}")
    print(f"   strings in code (>= 8 chars): {[s for s in strings(mine, CODE_OFF, end, 8)][:40]}")
    gap = mine[end + 4:0xF830]
    print(f"   between code end and user data (0x{end + 4:x}..0xf830): {len(gap)} bytes, all 0xff: {all(b == 0xff for b in gap)}, nonff bytes: {sum(1 for b in gap if b != 0xff)}")
    tail = mine[0xFBD0:]
    print(f"   after user data (0xfbd0..): all 0xff: {all(b == 0xff for b in tail)}, nonff: {sum(1 for b in tail if b != 0xff)}")
    print("-- user data")
    for name, off, ln in USER:
        b = mine[off:off + ln]
        if all(x == 0xff for x in b): print(f"   {name:20s} @0x{off:05x}: erased (all 0xff)"); continue
        if name == "edid":
            print(f"   {name:20s} @0x{off:05x}:"); [print("      " + l) for l in edid(b)]
            print("      raw: " + b.hex())
        elif name == "hdcpUserKey":
            print(f"   {name:20s} @0x{off:05x}:"); [print("      " + l) for l in hdcp(b)]
        elif name.endswith("String"): print(f"   {name:20s} @0x{off:05x}: {ustr(b)}")
        elif name == "BCSH": print(f"   {name:20s} @0x{off:05x}: en={b[0]:#x} b={int.from_bytes(b[1:2],'little',signed=True)} c={b[2]} s={b[3]} h={int.from_bytes(b[4:5],'little',signed=True)}  raw {b[:8].hex()}")
        elif name == "AudioRoute": print(f"   {name:20s} @0x{off:05x}: sample_rate={b[0]} i2s_in_mode={b[1]} mix3={b[2]} i2s1={b[3]} usb_out_mute={b[4]} lk621={b[5]}  raw {b[:8].hex()}")
        else: print(f"   {name:20s} @0x{off:05x}: {b.hex()}  {repr(bytes(x for x in b if 32 <= x < 127))}")
    print("-- comparison with public images")
    for p in sys.argv[2:]:
        o = open(p, "rb").read(); cl, oe, ho, co = header(o)
        same_len = cl == code_len
        diff = sum(1 for a, b in zip(mine[CODE_OFF:end], o[CODE_OFF:oe]) if a != b) if same_len else None
        od = [s for s in strings(o, CODE_OFF, oe) if any(mo in s for mo in ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")) and any(c.isdigit() for c in s)]
        print(f"   {os.path.basename(p)[:48]:48s} code {cl:5d} B  chk {'ok' if ho and co else 'BAD'}  " + (f"same length, {diff} bytes differ" if same_len else "different length") + f"  date {od[:1]}")
        if len(o) > 0xF830 + 0x100:
            oe_ = o[0xF830:0xF930]
            print(f"      has user data: EDID {'identical to mine' if oe_ == mine[0xF830:0xF930] else 'differs'}, HDCP key region {'identical' if o[0xF930:0xFA50] == mine[0xF930:0xFA50] else 'differs'}")
main()
