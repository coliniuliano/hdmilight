#!/usr/bin/env python3
"""mkdecomp.py RESOLVED NAMES OUT: write a readable decompile of the GSV2001 SDK (not kept in git) from the
resolved Ghidra dump of Kramer's KDS-EN7 MCU firmware (tools/resolve.py output): only the SDK's functions, named, with
the per-chip device index dropped from every device descriptor.

  tools/mkdecomp.py work/kramer/ip5000.c tools/kramer_names.txt work/kramer/gsv2k1.c

Names come from, in this order: NAMES (lines "addr name  # note"), the first debug string a function prints
(s_<text>_<addr>), the pages a small helper returns (dev_<page>_<page>), and for the per-chip dispatchers the name of
the Gsv2k1 function they call (disp_<name>)."""
import re, sys
src = open(sys.argv[1]).read()
RANGES = [(0x0800a620, 0x0800ac00, 'API (AvApi...)'), (0x0800c118, 0x08010200, 'common layer: state machines, register helpers, events, per-chip dispatch'),
          (0x08011524, 0x080116fc, 'Gsv2k1 device descriptors'), (0x08011ac4, 0x0801afc0, 'Gsv2k1 driver'),
          (0x0802e338, 0x08032200, 'common layer: state steps, port walkers, EDID and CEC helpers')]
fs = {}
for f in re.split(r'(?=/\* ==== )', src):
    m = re.match(r'/\* ==== (\S+) @ ([0-9a-f]{8})\s+size (\d+)', f)
    if m: fs[int(m.group(2), 16)] = (m.group(1), int(m.group(3)), f.split('\n', 1)[1])
names = {}; notes = {}
for line in open(sys.argv[2]):
    line = line.rstrip('\n')
    if not line.strip() or line.startswith('#'): continue
    head, _, note = line.partition('#'); a, n = head.split()[:2]
    names[int(a, 16)] = n; notes[int(a, 16)] = note.strip()
def slug(s): return re.sub(r'_+', '_', re.sub(r'[^A-Za-z0-9]+', '_', re.sub(r'%\w', '', s))).strip('_')[:34]
for a, (n, sz, body) in fs.items():
    if a in names: continue
    m = re.search(r'gsv_log\(\s*"([^"]{4,})"', body)
    if m and slug(m.group(1)): names[a] = 's_%s_%x' % (slug(m.group(1)), a & 0xfffff); continue
    pg = sorted(set(re.findall(r'GSV_PAGE\(0x([0-9a-f]{2})\)', body)))
    if pg and sz <= 90 and 'gsv_' not in body: names[a] = 'dev_' + '_'.join(pg[:2]) + '_%x' % (a & 0xfffff)
for a, (n, sz, body) in fs.items():       # dispatchers: if type 1 -> X, type 2 -> Y, type 4 -> Z
    if a in names: continue
    m = re.search(r"== '\\x01'\) \{\s*(?:\w+ = )?FUN_([0-9a-f]{8})\(", body)
    if m and sz < 90 and "'\\x02'" in body:
        t = int(m.group(1), 16); names[a] = 'disp_' + names.get(t, 'FUN_%08x' % t)
def sub(t):
    t = re.sub(r'FUN_([0-9a-f]{8})', lambda m: names.get(int(m.group(1), 16), m.group(0)), t)
    # the chip index (byte 1 of the chip object) ORed into bits 31..24 of every descriptor: always 0 with one chip
    t = re.sub(r'\(uint\)\*\(byte \*\)\(\*\w+ \+ 1\) << 0x18 \| ', '', t)
    t = re.sub(r' \+ \(uint\)\*\(byte \*\)\(\*\w+ \+ 1\) \* 0x1000000', '', t)
    t = re.sub(r' \| \(uint\)\*\(byte \*\)\(\*\w+ \+ 1\) << 0x18', '', t)
    t = re.sub(r'\(GSV_PAGE\(0x([0-9a-f]{2})\) - (0x[0-9a-f]+|\d+)U?\)', lambda m: 'GSV_PAGE(0x%02x)' % (int(m.group(1), 16) - int(m.group(2), 0)), t)
    t = re.sub(r'GSV_PAGE\(0x([0-9a-f]{2})\) - (0x[0-9a-f]+|\d+)U\b', lambda m: 'GSV_PAGE(0x%02x)' % (int(m.group(1), 16) - int(m.group(2), 0)), t)
    return t
out = ['''/* Readable decompilation of Gscoolink's GSV2001 SDK ("Gsv2k1" driver + common layer, SDK version 20210311) as built
 * into the MCU firmware of Kramer's KDS-EN7 encoders (IP5000-A30 V0.0.20, a GSV2001 product). Produced by Ghidra,
 * tools/resolve.py and tools/mkdecomp.py; ../sdk/gsv_sdk.h says where the image comes from. Only the SDK is here: the same
 * image also carries the Gsv2k2 and Gsv2k8 drivers and Kramer's application. Conventions:
 *   GSV_PAGE(p)   device descriptor for I2C address 0xB0 (0x58 seven-bit), register page p; the page is the high
 *                 byte of the 16-bit register address. (The original ORs a chip index into bits 31..24; dropped here.)
 *   gsv_write_bits(dev, reg, mask, shift, value)   read-modify-write of one register; mask 0xff = plain write
 *   gsv_read_bits(dev, reg, mask, shift, &out)     out = (register & mask) >> shift
 *   gsv_read_field / gsv_write_field(dev, reg, mask_first, mask_last, shift, order, bytes, value)   multi-byte fields
 *   read_block / write_block(dev, reg, count, buffer)
 *   gsv_log(...)  the SDK's debug print
 *   port object (0x88 bytes): +0 chip, +4 kind (0 Rx, 5 Tx, 8 scaler, 9 colour, 0xe audio, 4 CEC), +6 id (0 Rx,
 *                 4 Tx A, 5 Tx B), +0xc.. state bytes, +0x3c Rx info, +0x40 Tx status, +0x44 Rx HDCP, +0x48 Tx HDCP,
 *                 +0x4c video, +0x50 audio, +0x54 scaler, +0x58 colour, +0x5c CEC, +0x68 source, +0x6c / +0x70
 *                 downstream list, +0x74 audio source, +0x80 index, +0x84 next
 *   names: s_<text>_<addr> after the first debug string a function prints; dev_<page>_<page> helpers return a device
 *          descriptor; disp_<name> picks the driver by chip type (1 = Gsv2k1) and calls that function; names without
 *          an address suffix are from tools/kramer_names.txt; FUN_<addr> are not identified.
 * The library that drives the chip on the board is ../sdk. */
''']
n = 0
for lo, hi, title in RANGES:
    out.append('\n/* ' + '=' * 100 + '\n * %s (0x%08x..0x%08x)\n * ' % (title, lo, hi) + '=' * 100 + ' */\n')
    for a in sorted(fs):
        if not (lo <= a < hi): continue
        name, sz, body = fs[a]; n += 1
        note = ('  ' + notes[a]) if notes.get(a) else ''
        out.append('\n/* ==== %s @ %08x  size %d%s */\n%s' % (names.get(a, name), a, sz, note, sub(body).replace(name + '(', names.get(a, name) + '(', 1).rstrip() + '\n'))
open(sys.argv[3], 'w').write(''.join(out))
print(n, 'functions,', sum(1 for a in fs if any(lo <= a < hi for lo, hi, _ in RANGES) and a in names), 'named,', len(''.join(out).split('\n')), 'lines')
