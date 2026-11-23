#!/usr/bin/env python3
"""Oneboard build only: the MS2130 capture chip's boot flash through the ESP32's serial console (the USB-C port),
no network needed. The ESP32 reaches the flash over SPI and switches the MS2130 off meanwhile.

  tools/ms2130_serial.py [port] id               JEDEC id of the flash (proves the SPI path; changes nothing)
  tools/ms2130_serial.py [port] read out.bin     dump the 64 KB flash (changes nothing)
  tools/ms2130_serial.py [port] write image.bin  check, erase, write and verify an MS2130 image, then read it back

Default port: the only /dev/cu.usbmodem*. Needs nothing beyond Python 3.
The image to write is a full 64 KB dump of a working MS2130, kept out of git in ms2130/work/ because a full dump
includes the chip's user-data area, which can hold HDCP keys (tools/ms2130_flash.sh says how to make one).
The console's side of this is in firmware/main/oneboard.c ("flash read", "flash begin", "fd", "flash commit").
"""
import glob
import os
import re
import select
import sys
import termios
import time
import zlib

FLASH_BYTES = 65536
BLOCK = 40                      # bytes per "fd" line: the console's lines hold 95 characters
ANSI = re.compile(r"\x1b\[[0-9;]*m")


class Console:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0                                   # raw: no translation, no echo, no signals
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL      # and no hang-up on close
        a[4] = a[5] = termios.B115200
        a[6][termios.VMIN] = 0
        a[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.buf = b""

    def send(self, line):
        data = (line + "\n").encode()
        while data:
            if not select.select([], [self.fd], [], 5)[1]:
                sys.exit("the port does not accept data")
            data = data[os.write(self.fd, data):]

    def expect(self, pattern, timeout):
        """The next output line that starts with the pattern (a regex), as a match; None after the timeout."""
        end = time.time() + timeout
        while True:
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                m = re.match(pattern, ANSI.sub("", raw.decode("ascii", "replace")).strip())
                if m:
                    return m
            left = end - time.time()
            if left <= 0:
                return None
            if select.select([self.fd], [], [], left)[0]:
                self.buf += os.read(self.fd, 65536)

    def quiet(self):
        """Throw away what the board has already printed."""
        time.sleep(0.2)
        try:
            while os.read(self.fd, 65536):
                pass
        except BlockingIOError:
            pass
        self.buf = b""


def read_flash(con):
    """One attempt. Returns the 64 KB, or a string saying what went wrong."""
    con.quiet()
    con.send("flash read")
    data = bytearray()
    while True:
        m = con.expect(r"FL (?:(error) (.*)|(end) (\d+) ([0-9a-f]{8})|([0-9a-f]{6}) ([0-9a-f]+))$", 15)
        if not m:
            return "the board stopped answering at offset 0x%05x" % len(data)
        if m.group(1):
            sys.exit("the board says: " + m.group(2))
        if m.group(3):
            if len(data) != int(m.group(4)):
                return "%d of %s bytes arrived" % (len(data), m.group(4))
            if zlib.crc32(bytes(data)) != int(m.group(5), 16):
                return "checksum mismatch"
            return bytes(data)
        if int(m.group(6), 16) != len(data):
            return "a line was lost at offset 0x%05x" % len(data)
        data += bytes.fromhex(m.group(7))
        if len(data) % 8192 == 0:
            print("\rread %d %%" % (100 * len(data) // FLASH_BYTES), end="", flush=True)


def read_flash_retry(con):
    for attempt in range(3):
        r = read_flash(con)
        print("\r", end="")
        if isinstance(r, bytes):
            return r
        print("read attempt %d: %s" % (attempt + 1, r))
        time.sleep(3)                                            # let the rest of that dump go by
    sys.exit("could not read the flash")


def describe(data):
    blank = sum(1 for b in data if b == 0xFF)
    if blank == len(data):
        return "blank (all ff)"
    return "%d of %d bytes are not ff; starts %s" % (len(data) - blank, len(data), data[:8].hex(" "))


def main():
    args = sys.argv[1:]
    if args and args[0].startswith("/dev/"):
        port = args.pop(0)
    else:
        ports = glob.glob("/dev/cu.usbmodem*")
        if len(ports) != 1:
            sys.exit("give the port: found %s" % (ports or "no /dev/cu.usbmodem*"))
        port = ports[0]
    if not args or args[0] not in ("id", "read", "write") or (args[0] != "id" and len(args) < 2):
        sys.exit(__doc__)
    con = Console(port)

    if args[0] == "id":
        con.quiet()
        con.send("flashid")
        m = con.expect(r"JEDEC id (..) (..) (..)(.*)$", 10)
        if not m:
            sys.exit("no answer from the console on " + port)
        print("JEDEC id %s %s %s%s" % m.groups())
        sys.exit(1 if m.group(4) else 0)

    if args[0] == "read":
        data = read_flash_retry(con)
        with open(args[1], "wb") as f:
            f.write(data)
        print("%d bytes -> %s: %s" % (len(data), args[1], describe(data)))
        return

    with open(args[1], "rb") as f:
        img = f.read()
    if not 0 < len(img) <= FLASH_BYTES:
        sys.exit("%s is %d bytes: an MS2130 image is at most %d" % (args[1], len(img), FLASH_BYTES))
    con.quiet()
    con.send("flash begin %d" % len(img))
    m = con.expect(r"FL (ready|error) (.*)$", 10)
    if not m or m.group(1) != "ready":
        sys.exit("the board did not open a transfer: " + (m.group(2) if m else "no answer"))
    off = 0
    while off < len(img):
        block = img[off:off + BLOCK]
        for attempt in range(4):
            con.send("fd %06x %s" % (off, block.hex()))
            m = con.expect(r"FL (ok|error) (.*)$", 3)
            if m and m.group(1) == "ok" and int(m.group(2), 16) == off + len(block):
                break
            if m and m.group(1) == "error":
                con.send("flash abort")
                sys.exit("the board says: " + m.group(2))
        else:
            con.send("flash abort")
            sys.exit("no acknowledgement for the block at 0x%05x: nothing was written" % off)
        off += len(block)
        if off % 4000 == 0 or off == len(img):
            print("\rsent %d %%" % (100 * off // len(img)), end="", flush=True)
    print("\rsent %d bytes; erasing, writing and verifying..." % len(img))
    con.send("flash commit %08x" % zlib.crc32(img))
    m = con.expect(r"FL (done|error) (.*)$", 120)
    if not m:
        sys.exit("no answer after the commit: read the flash back to see what is in it")
    print(m.group(2))
    if m.group(1) != "done":
        sys.exit(1)
    time.sleep(3)
    back = read_flash_retry(con)
    if back[:len(img)] != img:
        sys.exit("READ-BACK DIFFERS from %s" % args[1])
    print("read back over the console: identical to %s" % args[1])


if __name__ == "__main__":
    main()
