#!/usr/bin/env python3
"""Decode a ".fw2" firmware container (the "Firmware upgrading with VC & EDID control tool" format used by the
Blustream SP12CS update, an HDCVT-built splitter) into the flat MCU image.

  tools/fw2_decode.py MCU-SP12CS-V1.1.0_20220513.fw2 sp12cs.bin

The payload starts at 0x205 and each byte is  c = ((p ^ k) + k) & 0xff  with k one character of a 32-character
hexadecimal string, repeating. The string is not needed in advance: an ARM image is full of 00 and ff bytes, so for
every position the hex digit that yields the most of them is the right one (the Cortex-M vector table then decodes to
a stack pointer in RAM and odd handler addresses, which is the check printed at the end)."""
import sys, collections

def decode(data, start=0x205):
    pay = data[start:]
    dec = lambda c, k: ((c - k) & 0xff) ^ k
    key = bytearray()
    for j in range(32):
        col = pay[j::32]
        best = max(b"0123456789ABCDEF", key=lambda ch: sum(1 for c in col if dec(c, ch) in (0, 0xff)))
        key.append(best)
    return bytes(dec(c, key[i % 32]) for i, c in enumerate(pay)), key.decode()

if __name__ == "__main__":
    if len(sys.argv) != 3: sys.exit(__doc__)
    out, key = decode(open(sys.argv[1], "rb").read())
    open(sys.argv[2], "wb").write(out)
    w = [int.from_bytes(out[i:i + 4], "little") for i in range(0, 16, 4)]
    ok = (w[0] >> 16) == 0x2000 and all(x & 1 and (x >> 24) == 0x08 for x in w[1:4])
    print(f"key {key}; {len(out)} bytes; vector table: sp {w[0]:08x}, reset {w[1]:08x} -> {'looks like a Cortex-M image' if ok else 'NOT a plausible vector table'}")
