#!/usr/bin/env python3
"""resolve.py IMAGE BASE DECOMP OUT [HELPERS]: make a Ghidra dump (tools/ghidra/DumpDecomp.java) of a flat Cortex-M
firmware readable: literal-pool constants resolved (plain numbers, RAM_xxxxxxxx, ROM_xxxxxxxx, &FUN_xxxxxxxx, or the
string itself when the literal points at one), device descriptors folded to GSV_PAGE(p), and the register helpers
named. HELPERS is "addr=name,addr=name" (hex addresses of gsv_write_bits and friends in that image).

  tools/resolve.py work/sp12cs.bin 0x08003000 work/sp12cs_decomp.c work/k1.c \\
      0800d722=gsv_write_bits,0800d702=gsv_read_bits,0800d766=gsv_read_field,0800d816=gsv_write_field,0800d6de=gsv_read_block,0800d6f0=gsv_write_block
"""
import re, struct, sys

img = open(sys.argv[1], 'rb').read(); BASE = int(sys.argv[2], 0)
src = open(sys.argv[3]).read()
helpers = dict(x.split('=') for x in sys.argv[5].split(',')) if len(sys.argv) > 5 else {}
starts = set(int(m, 16) for m in re.findall(r'/\* ==== \S+ @ ([0-9a-f]{8})', src))

def word(a):
    o = a - BASE
    return struct.unpack_from('<I', img, o)[0] if 0 <= o <= len(img) - 4 else None

def string_at(v):
    o = v - BASE
    if not (0 <= o < len(img)): return None
    e = img.find(b'\0', o)
    s = img[o:e] if e >= 0 else b''
    if len(s) >= 4 and all(32 <= c < 127 or c in (9, 10, 13) for c in s): return s.decode()
    return None

def val(v):
    if 0x20000000 <= v < 0x20010000: return 'RAM_%08x' % v
    if v & 1 and (v & ~1) in starts: return '&FUN_%08x' % (v & ~1)
    if BASE <= v < BASE + len(img):
        s = string_at(v)
        if s is not None: return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\r', '\\r') + '"'
        return 'ROM_%08x' % v
    return '0x%x' % v

def dat(m):
    v = word(int(m.group(1), 16))
    return val(v) if v is not None else m.group(0)

# strings Ghidra found itself: s_text_0800xxxx (optionally "+ n") -> the text
def gstr(m):
    s = string_at(int(m.group(1), 16) + (int(m.group(2), 0) if m.group(2) else 0))
    return ('"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\r', '\\r') + '"') if s is not None else m.group(0)
src = re.sub(r'\bs_\w*?_([0-9a-f]{8})\b(?: \+ (\d+|0x[0-9a-f]+))?', gstr, src)
src = re.sub(r'\b(?:DAT|PTR_FUN|PTR_DAT|PTR_LAB|PTR_s|PTR|UNK)_(?:\w*?_)?([0-9a-f]{8})\b', dat, src)
for a, name in helpers.items(): src = src.replace('FUN_' + a, name)
src = re.sub(r'\b0x1b0([0-9a-f]{2})\b', lambda m: 'GSV_PAGE(0x%s)' % m.group(1), src)
def fold(m):
    v = int(m.group(1), 16) + int(m.group(2).replace(' ', ''), 0)
    return 'GSV_PAGE(0x%02x)' % v if 0 <= v < 256 else m.group(0)
src = re.sub(r'GSV_PAGE\(0x([0-9a-f]{2})\) \+ (-?\s?(?:0x[0-9a-f]+|\d+))\b', fold, src)
src = re.sub(r'\n[ \t]*\n(?=[ \t]*\S)', '\n', src)
open(sys.argv[4], 'w').write(src)
print(len(starts), 'functions')
