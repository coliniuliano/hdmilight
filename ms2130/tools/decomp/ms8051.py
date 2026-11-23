"""Take an MS2130 flash image apart: its 8051 code is Keil C51 output, whose library helpers carry data in the
instruction stream (a 32-bit constant or a switch table right after the LCALL), so a plain disassembler loses its way.

  ms8051.py prepare IMAGE OUT   code.bin, flow.json, code_patched.bin and the lists the Ghidra scripts read
  ms8051.py initseg IMAGE       the initial-value table (start-up values of variables; USB descriptors live here)
  ms8051.py usb IMAGE           USB descriptors: device (from the initial values) and configurations (from code)
  ms8051.py xref OUT            register and table cross-references, from OUT/flow.json

IMAGE is a full flash dump or an update image (header at 0, code at 0x30)."""
import sys, json, struct, collections, os

HDR = 0x30

def load_code(path):
    d = open(path, 'rb').read()
    if d[:2] != b'\x3c\xc3': sys.exit(f"{path}: no 3c c3 header")
    n = d[2] << 8 | d[3]
    return d[HDR:HDR + n]

# ---- 8051 instruction lengths and the Keil helpers ------------------------------------------------------------
L3 = {0x02, 0x12, 0x10, 0x20, 0x30, 0x43, 0x53, 0x63, 0x75, 0x85, 0x90, 0xb4, 0xb5, 0xd5} | set(range(0xb6, 0xc0))
L1 = {0x00, 0x03, 0x04, 0x13, 0x14, 0x22, 0x23, 0x32, 0x33, 0x73, 0x83, 0x84, 0x93, 0xa3, 0xa4, 0xa5, 0xb3, 0xc3,
      0xc4, 0xd3, 0xd4, 0xd6, 0xd7, 0xe0, 0xe2, 0xe3, 0xe4, 0xf0, 0xf2, 0xf3, 0xf4}
for _b in (0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x90, 0xc0, 0xe0, 0xf0):
    L1.update(range(_b + 6, _b + 16))

def ilen(op): return 3 if op in L3 else 1 if op in L1 else 2

SIG = {  # first bytes of each library routine
    'lstkxdata': bytes.fromhex('a8828583f0d083d08212'),                      # 4 bytes follow the call, stored at @DPTR
    'ccase': bytes.fromhex('d083d082f8e4937012740193700da3a393f8740193f5828883e47374029368'),
    'icase': bytes.fromhex('d083d082f8e4937012740193700da3a393f8740193f5828883e473740293b5f0'),
    'lcase': bytes.fromhex('d083d082e4937012740193700da3a393f8740193f5828883e4737402936c'),
    'icall': bytes.fromhex('8a838982e473'),
}

def helpers(code):
    h = {}
    for k, s in SIG.items():
        i = code.find(s)
        if i < 0: sys.exit(f"helper {k} not found: not the Keil library this tool knows")
        h[k] = i
    x = h['lstkxdata']; j = bytes([0x02, x >> 8, x & 0xff])
    for k, s in (('lstkptr', bytes.fromhex('bb010789828a83') + j), ('lstkoptr', bytes.fromhex('bb010de58229f582e5833af583') + j)):
        i = code.find(s)
        if i >= 0: h[k] = i
    return h

def initseg_addr(code):
    i = code.find(bytes.fromhex('e47e019360'))        # start-up loop: CLR A; MOV R6,#1; MOVC; JZ
    while i >= 0:
        if code[i - 3] == 0x90: return code[i - 2] << 8 | code[i - 1]
        i = code.find(bytes.fromhex('e47e019360'), i + 1)
    sys.exit("start-up code not found")

# ---- flow trace -----------------------------------------------------------------------------------------------
def trace(code):
    N = len(code); H = helpers(code)
    inline = {H[k]: 4 for k in ('lstkxdata', 'lstkptr', 'lstkoptr') if k in H}
    case = {H['ccase']: 1, H['icase']: 2, H['lcase']: 4}
    insn, funcs, tables, lstk, work, ljmp = {}, {}, {}, {}, [], []
    def add(a, why, fn=False):
        if a >= N: return
        if fn: funcs.setdefault(a, why)
        if a not in insn: work.append(a)
    def rel(a, off): return (a + (off - 256 if off > 127 else off)) & 0xffff
    def run():
        while work:
            a = work.pop()
            while a < N and a not in insn:
                op = code[a]; n = ilen(op); insn[a] = n; nx = a + n
                if op == 0x12 or (op & 0x1f) == 0x11:
                    t = (code[a+1] << 8 | code[a+2]) if op == 0x12 else ((nx & 0xf800) | ((op >> 5) << 8) | code[a+1])
                    add(t, a, True)
                    if t in inline: lstk[a] = (nx, inline[t], t); nx += inline[t]
                    elif t in case:
                        w = case[t]; p = nx; ent = []
                        while code[p] or code[p+1]:
                            tg = code[p] << 8 | code[p+1]; ent.append((int.from_bytes(code[p+2:p+2+w], 'big'), tg)); add(tg, a); p += 2 + w
                        d = code[p+2] << 8 | code[p+3]; ent.append((None, d)); add(d, a)
                        tables[a] = (nx, p + 4, w, ent); break
                elif op == 0x02: add(code[a+1] << 8 | code[a+2], a); break
                elif (op & 0x1f) == 0x01: add((nx & 0xf800) | ((op >> 5) << 8) | code[a+1], a); break
                elif op == 0x80: add(rel(nx, code[a+1]), a); break
                elif op in (0x22, 0x32, 0x73): break
                elif op in (0x40, 0x50, 0x60, 0x70) or 0xd8 <= op <= 0xdf: add(rel(nx, code[a+1]), a)
                elif op in (0x10, 0x20, 0x30, 0xb4, 0xb5, 0xd5) or 0xb6 <= op <= 0xbf: add(rel(nx, code[a+2]), a)
                a = nx
    for v in (0x00, 0x03, 0x0b, 0x13, 0x1b, 0x23, 0x2b):
        if code[v] == 0x02: add(v, 'vector', True)
    run()
    while True:      # JMP @A+DPTR through a table of LJMPs: the table is where a MOV DPTR,#imm in reached code points
        new = 0
        for a in sorted(insn):
            if code[a] != 0x90: continue
            t = code[a+1] << 8 | code[a+2]
            if t >= N or code[t] != 0x02 or t in insn: continue
            b = a + 3; k = 0                      # only when the JMP @A+DPTR follows in the same straight run
            while k < 8 and b in insn and code[b] not in (0x73, 0x02, 0x22, 0x32, 0x80, 0x90): b += insn[b]; k += 1
            if b not in insn or code[b] != 0x73: continue
            k = 0
            while t < N and code[t] == 0x02 and t not in insn and (code[t+1] << 8 | code[t+2]) < N:
                ljmp.append((t, a)); add(t, 'ljmp-table'); run(); t += 3; k += 1
            new += k
        if not new: break
    return dict(insn=insn, funcs=funcs, tables=tables, lstk=lstk, ljmp=ljmp, helpers=H)

def coverage(code, f):
    cov = bytearray(len(code))
    for a, n in f['insn'].items():
        for i in range(n): cov[a+i] |= 1
    for a, (s, n, t) in f['lstk'].items():
        for i in range(n): cov[s+i] |= 2
    for a, (s, e, w, ent) in f['tables'].items():
        for i in range(s, e): cov[i] |= 2
    gaps = []; i = 0
    while i < len(code):
        if not cov[i]:
            j = i
            while j < len(code) and not cov[j]: j += 1
            gaps.append((i, j - i)); i = j
        else: i += 1
    return cov, gaps

# ---- helper calls rewritten as plain code -----------------------------------------------------------------------
STUBS = 0xD000

def patch(code, f):
    """Each LCALL to a helper with inline data becomes an LJMP to equivalent plain instructions placed at STUBS."""
    c = bytearray(code); stub = bytearray(); notes = []; left = []
    X = f['helpers']['lstkxdata']
    for a, (s, n, t) in sorted(f['lstk'].items()):
        v = bytes(code[s:s+4])
        if t != X: left.append((a, s, s + 4)); notes.append(f"{a:04x} long 0x{v.hex()} through helper {t:04x}, call kept"); continue
        h = STUBS + len(stub)
        for i, b in enumerate(v): stub += bytes([0x74, b, 0xf0]) + (b"\xa3" if i < 3 else b"")
        stub += bytes([0x02, (s + 4) >> 8, (s + 4) & 0xff])
        c[a:a+3] = bytes([0x02, h >> 8, h & 0xff]); notes.append(f"{a:04x} xdata long = 0x{v.hex()}  stub {h:04x}")
    for a, (s, e, w, ent) in sorted(f['tables'].items()):
        h = STUBS + len(stub)
        for v, t in ent:
            lj = bytes([0x02, t >> 8, t & 0xff])
            if v is None: stub += lj
            elif w == 1: stub += bytes([0xb4, v, 3]) + lj
            elif w == 2: stub += bytes([0xb4, v & 0xff, 12, 0xc5, 0xf0, 0xb4, v >> 8, 5, 0xc5, 0xf0]) + lj + bytes([0xc5, 0xf0])
            else:
                b = v.to_bytes(4, 'big'); stub += bytes([0xbc, b[0], 12, 0xbd, b[1], 9, 0xbe, b[2], 6, 0xbf, b[3], 3]) + lj
        c[a:a+3] = bytes([0x02, h >> 8, h & 0xff])
        notes.append(f"{a:04x} switch on {w} byte(s), stub {h:04x}: " + " ".join(f"{('%x' % v) if v is not None else 'default'}->{t:04x}" for v, t in ent))
    if len(code) > STUBS: sys.exit("code reaches the stub area")
    return bytes(c) + b"\xff" * (STUBS - len(code)) + bytes(stub), notes, left

def prepare(image, out):
    os.makedirs(out, exist_ok=True)
    code = load_code(image); f = trace(code); cov, gaps = coverage(code, f)
    img, notes, left = patch(code, f)
    w = lambda name, data, mode='w': open(os.path.join(out, name), mode).write(data)
    w('code.bin', code, 'wb'); w('code_patched.bin', img, 'wb')
    w('patch_notes.txt', "\n".join(notes) + "\n")
    w('entries.txt', "".join(f"{a:04x}\n" for a in sorted(f['funcs'])) + "".join(f"{a:04x} -\n" for a, _ in f['ljmp']))
    w('insn.txt', "".join(f"{a:04x}\n" for a in sorted(f['insn'])))
    w('inline.txt', "".join(f"{a:04x} {s:04x} {n:04x}\n" for a, s, n in left))
    json.dump(dict(insn=sorted(f['insn']), funcs=sorted(f['funcs']), helpers=f['helpers'], initseg=initseg_addr(code),
                   ljmp=[a for a, _ in f['ljmp']], lstk={a: v for a, v in f['lstk'].items()},
                   tables={a: v for a, v in f['tables'].items()}), open(os.path.join(out, 'flow.json'), 'w'))
    print(f"code {len(code)} bytes: {len(f['insn'])} instructions in {sum(1 for x in cov if x & 1)} bytes, "
          f"{sum(1 for x in cov if x == 2)} bytes of inline data, {len(f['funcs'])} call targets, {len(f['tables'])} switch tables, "
          f"{len(f['lstk'])} inline constants, {len(f['ljmp'])} jump-table entries")
    print("helpers: " + " ".join(f"{k}={v:04x}" for k, v in sorted(f['helpers'].items())) + f"  initial values at {initseg_addr(code):04x}")
    print(f"not reached ({sum(n for a, n in gaps)} bytes in {len(gaps)} runs; 16 bytes or more shown):")
    for a, n in gaps:
        if n >= 16: print(f"  {a:04x}..{a+n-1:04x} {n:5d}  {code[a:a+16].hex()}")

# ---- initial values ---------------------------------------------------------------------------------------------
def initseg(code):
    p = initseg_addr(code); recs = []
    while code[p]:
        h = code[p]; p += 1
        if h & 0x20: n = ((h & 0x1f) << 8) | code[p]; p += 1
        else: n = h & 0x3f
        t = h & 0xc0
        if t == 0x40: a = code[p] << 8 | code[p+1]; p += 2; recs.append(('xdata', a, code[p:p+n])); p += n
        elif t == 0xc0:
            for i in range(n): b = code[p]; p += 1; recs.append(('bit', 0x20 + ((b >> 3) & 0xf), bytes([b & 7, b >> 7])))
        else: a = code[p]; p += 1; recs.append(('pdata' if t == 0x80 else 'idata', a, code[p:p+n])); p += n
    return recs, p

def xdata_init(code):
    x = {}
    for t, a, d in initseg(code)[0]:
        if t == 'xdata': x.update({a + i: b for i, b in enumerate(d)})
    return x

def cmd_initseg(image):
    code = load_code(image); recs, end = initseg(code)
    print(f"initial values {initseg_addr(code):04x}..{end:04x}, {len(recs)} records")
    for t, a, d in recs:
        if t == 'bit': print(f"bit   {a:02x}.{d[0]} = {d[1]}"); continue
        s = ''
        if len(d) > 2 and d[0] == len(d) and d[1] == 3: s = '  string ' + repr(d[2:].decode('utf-16-le', 'replace'))
        print(f"{t:5s} {a:04x} len {len(d):4d}  " + (d.hex() if len(d) <= 32 else d[:16].hex() + '...') + s)

# ---- USB descriptors --------------------------------------------------------------------------------------------
GUID = {'5955593200001000800000aa00389b71': 'YUY2', '4e56313200001000800000aa00389b71': 'NV12'}
u16 = lambda b, o: b[o] | b[o+1] << 8
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]

def config(code, start):
    tot = u16(code, start + 2); d = code[start:start + tot]; p = 0; cls = sub = None
    print(f"== configuration at code {start:04x}: {tot} bytes, {d[4]} interfaces, attributes {d[7]:02x}, bMaxPower {d[8]}")
    while p < tot:
        n, t = d[p], d[p+1]; b = d[p:p+n]
        if t == 4: cls, sub = b[5], b[6]; print(f"  interface {b[2]} alt {b[3]}: {b[4]} endpoints, class {b[5]:02x}/{b[6]:02x}/{b[7]:02x}, string {b[8]}")
        elif t == 0x0b: print(f"  association: interfaces {b[2]}..{b[2]+b[3]-1}, class {b[4]:02x}/{b[5]:02x}/{b[6]:02x}, string {b[7]}")
        elif t == 5: print(f"    endpoint {b[2]:02x} {('control', 'isochronous', 'bulk', 'interrupt')[b[3] & 3]} (attributes {b[3]:02x}) max packet {u16(b,4)} interval {b[6]}")
        elif t == 0x30: print(f"    SuperSpeed companion: burst {b[2]}, attributes {b[3]:02x}, bytes per interval {u16(b,4)}")
        elif t == 0x21 and cls == 3: print(f"    HID {u16(b,2):04x}, report descriptor {u16(b,7)} bytes")
        elif t == 0x24 and cls == 0x0e and sub == 1:
            st = b[2]
            if st == 1: print(f"    video control header: UVC {u16(b,3):04x}, clock {u32(b,7)}")
            elif st == 2: print(f"    input terminal {b[3]} type {u16(b,4):04x}, controls {b[15:15+b[14]].hex()}")
            elif st == 3: print(f"    output terminal {b[3]} type {u16(b,4):04x}, source {b[7]}")
            elif st == 5: print(f"    processing unit {b[3]}, source {b[4]}, controls {b[8:8+b[7]].hex()}")
            else: print(f"    video control subtype {st}: {b.hex()}")
        elif t == 0x24 and cls == 0x0e and sub == 2:
            st = b[2]
            if st == 1: print(f"    stream header: {b[3]} formats, endpoint {b[6]:02x}")
            elif st == 4: print(f"    format {b[3]} uncompressed {GUID.get(b[5:21].hex(), b[5:21].hex())}, {b[21]} bits per pixel, {b[4]} frames, default {b[22]}")
            elif st == 6: print(f"    format {b[3]} MJPEG, {b[4]} frames, default {b[6]}")
            elif st in (5, 7):
                k = b[25]; iv = [u32(b, 26 + 4 * i) for i in range(k)]
                print(f"      frame {b[3]:2d} {u16(b,5)}x{u16(b,7)}: " + " ".join(f"{1e7 / i:.4g}" for i in iv) + " fps")
            elif st == 0x0d: print(f"    colour: primaries {b[3]}, transfer {b[4]}, matrix {b[5]}")
            else: print(f"    video stream subtype {st}: {b.hex()}")
        elif t == 0x24 and cls == 1 and sub == 2 and b[2] == 2:
            print(f"    audio format: {b[4]} channels, {b[6]} bits, " + " ".join(str(b[8+3*i] | b[9+3*i] << 8 | b[10+3*i] << 16) for i in range(b[7])) + " Hz")
        elif t in (0x24, 0x25): print(f"    class-specific {t:02x}/{b[2]:02x}: {b.hex()}")
        elif t not in (2, 7): print(f"    type {t:02x}: {b.hex()}")
        p += n

def is_config(code, i):
    if code[i] != 9 or code[i+1] not in (2, 7): return False
    tot = u16(code, i + 2); p = 0
    if not 18 <= tot <= 4096 or i + tot > len(code): return False
    while p < tot:
        if code[i+p] < 2: return False
        p += code[i+p]
    return p == tot

def cmd_usb(image):
    code = load_code(image); x = xdata_init(code)
    blob = lambda a, n: bytes(x.get(a + i, 0) for i in range(n))
    for a in sorted(x):
        d = blob(a, 18)
        if d[0] == 18 and d[1] == 1 and x.get(a - 1) is None or (d[0] == 18 and d[1] == 1 and d[4] == 0xef):
            print(f"device descriptor (initial value at xdata {a:04x}): USB {d[3]:x}.{d[2]:02x}, class {d[4]:02x}/{d[5]:02x}/{d[6]:02x}, "
                  f"bMaxPacketSize0 {d[7]}, {d[9]<<8|d[8]:04x}:{d[11]<<8|d[10]:04x}, device {d[13]:x}.{d[12]:02x}, strings {d[14]}/{d[15]}/{d[16]}")
    for t, a, d in initseg(code)[0]:
        if t == 'xdata' and len(d) > 2 and d[0] == len(d) and d[1] == 3: print(f"string (xdata {a:04x}): {d[2:].decode('utf-16-le', 'replace')!r}")
    i = 0
    while i < len(code) - 9:
        if is_config(code, i): config(code, i); i += u16(code, i + 2)
        else: i += 1

# ---- cross-references -------------------------------------------------------------------------------------------
def cmd_xref(out):
    code = open(os.path.join(out, 'code.bin'), 'rb').read(); f = json.load(open(os.path.join(out, 'flow.json'))); ins = f['insn']
    dp = collections.Counter(); per = collections.defaultdict(collections.Counter); ptr = collections.defaultdict(list)
    sfr = collections.defaultdict(lambda: [0, 0]); bits = collections.Counter()
    for i, a in enumerate(ins):
        op = code[a]
        if op == 0x90: dp[code[a+1] << 8 | code[a+2]] += 1
        if op in (0x12, 0x02):
            r6 = r7 = None
            for b in reversed(ins[max(0, i - 6):i]):
                if code[b] == 0x7f and r7 is None: r7 = code[b+1]
                elif code[b] == 0x7e and r6 is None: r6 = code[b+1]
                elif code[b] in (0x12, 0x02, 0x22, 0x80): break
            if r6 is not None and r7 is not None: per[code[a+1] << 8 | code[a+2]][r6 << 8 | r7] += 1
        if op == 0x7a:
            near = ins[max(0, i - 3):i + 4]
            if any(code[b] == 0x7b and code[b+1] == 0xff for b in near):      # R3 = 0xff: the pointer is into code
                for b in near:
                    if code[b] == 0x79: ptr[code[a+1] << 8 | code[b+1]].append(a)
        d = None
        if op in (0x05, 0x15, 0x42, 0x43, 0x52, 0x53, 0x62, 0x63, 0x75, 0xf5, 0xd0, 0xd5) or 0x88 <= op <= 0x8f: sfr[code[a+1]][1] += 1
        elif op in (0x25, 0x35, 0x45, 0x55, 0x65, 0x95, 0xe5, 0xc0, 0xb5, 0xc5) or 0xa6 <= op <= 0xaf: sfr[code[a+1]][0] += 1
        elif op == 0x85: sfr[code[a+1]][0] += 1; sfr[code[a+2]][1] += 1
        elif op in (0x10, 0x20, 0x30, 0x72, 0x82, 0x92, 0xa0, 0xa2, 0xb0, 0xb2, 0xc2, 0xd2) and code[a+1] >= 0x80:
            bits[(code[a+1] & 0xf8, code[a+1] & 7, 'write' if op in (0x10, 0x92, 0xb2, 0xc2, 0xd2) else 'read')] += 1
    print("MOV DPTR,#imm by page:")
    pages = collections.defaultdict(list)
    for a in sorted(dp): pages[a >> 8].append(a)
    for pg, l in sorted(pages.items()): print(f"  {pg:02x}xx: {sum(dp[a] for a in l):4d} uses, {len(l):3d} addresses, {l[0]:04x}..{l[-1]:04x}")
    print("constants in R6:R7 before a call, by callee (10 or more uses):")
    for t, c in sorted(per.items(), key=lambda kv: -sum(kv[1].values())):
        if sum(c.values()) >= 10: print(f"  {t:04x}: {sum(c.values()):3d} calls; " + " ".join(f"{v:04x}" for v in sorted(c)))
    print("special function registers (direct addresses 0x80 and up, without SP/DPTR/PSW/ACC/B):")
    for s in sorted(sfr):
        if s >= 0x80 and s not in (0x81, 0x82, 0x83, 0xd0, 0xe0, 0xf0): print(f"  {s:02x}: read {sfr[s][0]:3d}, written {sfr[s][1]:3d}")
    for (s, b, k), n in sorted(bits.items()):
        if s not in (0xd0, 0xe0, 0xf0): print(f"  bit {s:02x}.{b} {k} x{n}")
    print("pointers into code (MOV R2,#hi / MOV R1,#lo) that land outside reached instructions:")
    S = set(ins)
    for t in sorted(ptr):
        if t < len(code) and t not in S and (t > 0x1d): print(f"  {t:04x}: used at " + " ".join(f"{a:04x}" for a in ptr[t]) + f"   {code[t:t+8].hex()}")

if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == 'prepare': prepare(a[1], a[2])
    elif len(a) == 2 and a[0] == 'initseg': cmd_initseg(a[1])
    elif len(a) == 2 and a[0] == 'usb': cmd_usb(a[1])
    elif len(a) == 2 and a[0] == 'xref': cmd_xref(a[1])
    else: sys.exit(__doc__)
