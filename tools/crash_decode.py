#!/usr/bin/env python3
"""Decode the board's last crash (/api/crash, see firmware/main/crashlog.h) into source lines.

Finds the ELF whose SHA-256 matches the crashed image (any firmware/build-*/hdmilight_v3.elf, or the archive
tools/ota_paced.py keeps in firmware/.elf-archive/), then runs addr2line on the faulting PC, the return address and
every stack word that points into code, and reads the reason strings out of the ELF.

Usage: tools/crash_decode.py <host | crash.json>     (asks for the host when left out)
"""
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, "..", "firmware")
TOOL_PREFIX = "riscv32-esp-elf-"


def tool(name):
    p = shutil.which(TOOL_PREFIX + name)
    if not p:
        hits = glob.glob(os.path.expanduser(f"~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/{TOOL_PREFIX}{name}"))
        p = sorted(hits)[-1] if hits else None
    if not p:
        sys.exit(f"{TOOL_PREFIX}{name} not found (source ESP-IDF's export.sh)")
    return p


def find_elf(sha_hex):
    want = int(sha_hex, 16)
    for path in sorted(glob.glob(os.path.join(FW, "build-*", "hdmilight_v3.elf"))) + glob.glob(os.path.join(FW, ".elf-archive", "*.elf")):
        if os.path.exists(path):
            d = hashlib.sha256(open(path, "rb").read()).digest()
            if int.from_bytes(d[:4], "big") == want:
                return path
    return None


def addr2line(elf, addrs):
    if not addrs:
        return {}
    out = subprocess.run([tool("addr2line"), "-pfiaC", "-e", elf] + [hex(a) for a in addrs],
                         capture_output=True, text=True).stdout.strip().splitlines()
    res, cur = {}, None
    for line in out:
        if line.startswith("0x"):
            cur = int(line.split(":")[0], 16)
            res[cur] = line.split(":", 1)[1].strip()
        elif cur is not None:
            res[cur] += "\n" + " " * 16 + line.strip()
    return res


def elf_string(elf, addr):
    if not addr:
        return ""
    dump = subprocess.run([tool("objdump"), "-s", f"--start-address={addr:#x}", f"--stop-address={addr + 96:#x}", elf],
                          capture_output=True, text=True).stdout
    raw = b""
    for line in dump.splitlines():
        parts = line.strip().split()
        if parts and len(parts[0]) == 8 and all(c in "0123456789abcdef" for c in parts[0]):
            for word in parts[1:5]:
                try:
                    raw += bytes.fromhex(word)
                except ValueError:
                    break
    return raw.split(b"\0")[0].decode(errors="replace")


def is_code(a):
    return 0x40000000 <= a < 0x50000000   # P4 flash-mapped code and IRAM


def main():
    if len(sys.argv) > 1:
        src = sys.argv[1]
    elif sys.stdin.isatty():       # no default: every board answers to "hdmilight" until it is renamed
        src = input("Board to read the crash from (hostname or IP), or a saved JSON file: ").strip()
    else:
        sys.exit("usage: tools/crash_decode.py <host | crash.json>")
    if not src:
        sys.exit("no host given")
    if os.path.exists(src):
        c = json.load(open(src))
    else:
        with urllib.request.urlopen(f"http://{src}/api/crash", timeout=10) as r:
            c = json.loads(r.read())
    if not c.get("present"):
        print("no crash recorded for this boot")
        return
    elf = find_elf(c["elf_sha"])
    print(f"crash in image {c['elf_sha']}, core {c['core']}, exception {c['exception']}, mcause {c['mcause']}, "
          f"mtval {c['mtval']}")
    if c.get("abort"):
        print("abort:", c["abort"])
    if not elf:
        print("no matching ELF found; raw addresses only:", c["mepc"], c["ra"])
        return
    print("elf:", os.path.relpath(elf))
    print("reason:", elf_string(elf, int(c["reason_ptr"], 16)), "|", elf_string(elf, int(c["desc_ptr"], 16)))
    regs = {k: int(c[k], 16) for k in ("mepc", "ra", "addr")}
    stack = [int(x, 16) for x in c.get("stack", [])]
    code = [a for a in stack if is_code(a)]
    names = addr2line(elf, list(dict.fromkeys([regs["mepc"], regs["ra"], regs["addr"]] + code)))
    print(f"\nPC   {regs['mepc']:#010x}  {names.get(regs['mepc'], '?')}")
    print(f"RA   {regs['ra']:#010x}  {names.get(regs['ra'], '?')}")
    if code:
        print("\ncode addresses on the stack (newest first; not all are real return addresses):")
        for a in dict.fromkeys(code):
            print(f"     {a:#010x}  {names.get(a, '?')}")
    print("\nlast log lines before the crash:")
    print("\n".join(c.get("log", "").splitlines()[-40:]))


if __name__ == "__main__":
    main()
