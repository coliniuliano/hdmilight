#!/usr/bin/env python3
"""Upload a firmware image to /api/update without ever queueing more than ~1 KB in the board's Wi-Fi chip.

Written for a P4 running stock esp_hosted 3.1.0: that host wedges Wi-Fi RX for good the first time the C6 hands it
more than 1536 bytes in one SDIO transfer (firmware/patches/esp_hosted/README.md), which a normal upload (tools/ota.sh,
curl) does almost at once. Every image since the patch is safe either way; this stays the default uploader because it
works on any of them. Whether an image has the patch: `strings firmware/.elf-archive/<sha>.elf | grep "hdmilight patch"`.

This sender caps the TCP segment size (TCP_MAXSEG, default 400 bytes, about 480 on the air) and keeps at most
--inflight segments unacknowledged (default 2), using the kernel's count of unacknowledged bytes (SO_NWRITE, macOS).
At most ~960 bytes of upload data can then sit in the C6, whatever the P4 is doing. Slower than curl: about a minute.

Usage: tools/ota_paced.py <host> <image> [--mss 400] [--inflight 2]     (asks for the host when left out)
Keep everything else off the board while it runs (web UI closed, nothing polling it).
"""
import argparse
import glob
import hashlib
import json
import os
import socket
import sys
import time
import urllib.request

SO_NWRITE = 0x1024  # macOS <sys/socket.h>: bytes in the send buffer not yet acknowledged


def unacked(sock):
    return sock.getsockopt(socket.SOL_SOCKET, SO_NWRITE)


TCP_CONNECTION_INFO = 0x106  # macOS <netinet/tcp.h>


def tx_stats(sock):
    """(packets, bytes, retransmitted packets, maxseg) sent on this connection, from struct tcp_connection_info."""
    raw = sock.getsockopt(socket.IPPROTO_TCP, TCP_CONNECTION_INFO, 112)
    maxseg = int.from_bytes(raw[16:20], "little")
    txp, txb = int.from_bytes(raw[56:64], "little"), int.from_bytes(raw[64:72], "little")
    rtxp = int.from_bytes(raw[104:112], "little")
    return txp, txb, rtxp, maxseg


def status(host, timeout=5):
    with urllib.request.urlopen(f"http://{host}/api/status", timeout=timeout) as r:
        return json.loads(r.read())


def upload(host, port, image, mss, inflight, stall_s=30, log=print):
    data = open(image, "rb").read()
    head = (f"POST /api/update HTTP/1.1\r\nHost: {host}\r\nContent-Type: application/octet-stream\r\n"
            f"Content-Length: {len(data)}\r\nConnection: close\r\n\r\n").encode()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, mss)   # before connect: also what we advertise
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(stall_s)
    s.connect((host, port))
    # macOS: the value set before connect only shapes the MSS we advertise; the send MSS is recomputed at connect,
    # so set it again now (lowering is always allowed) and check it took.
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, mss)
    eff = s.getsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG)
    if eff > mss:
        raise RuntimeError(f"kernel kept MSS {eff} > {mss}; refusing to send full-size segments")
    log(f"connected, segment size {eff} B, at most {inflight} unacknowledged; {len(data)} bytes, "
        f"sha256 {hashlib.sha256(data).hexdigest()[:16]}")
    limit = eff * (inflight - 1)          # send the next segment only when this many bytes or fewer are unacked
    payload = head + data
    pos, t0, last_progress, last_report = 0, time.time(), time.time(), 0
    while pos < len(payload):
        while unacked(s) > limit:
            if time.time() - last_progress > stall_s:
                raise RuntimeError(f"no ACK progress for {stall_s} s at byte {pos} (link wedged?)")
            time.sleep(0.0005)
        n = s.send(payload[pos:pos + eff])
        pos += n
        last_progress = time.time()
        if pos - last_report >= 100_000 or pos == len(payload):
            last_report = pos
            el = time.time() - t0
            log(f"  {pos - len(head) if pos > len(head) else 0:>8}/{len(data)} bytes, {el:5.1f} s, "
                f"{pos / el / 1024:5.1f} KiB/s")
    while unacked(s) > 0:                 # everything acknowledged before we wait for the answer
        if time.time() - last_progress > stall_s:
            raise RuntimeError("last bytes never acknowledged")
        time.sleep(0.001)
    txp, txb, rtxp, maxseg = tx_stats(s)
    log(f"sent {txp} TCP packets, {txb} bytes: {txb / max(txp, 1):.0f} B per packet on average "
        f"(segment cap {maxseg}), {rtxp} retransmitted")
    if txp and txb / txp > eff + 1:
        raise RuntimeError(f"average packet {txb / txp:.0f} B exceeds the {eff} B cap")
    resp = b""
    s.settimeout(60)                      # the board answers after esp_ota_end + set_boot_partition
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            resp += chunk
    except ConnectionResetError:
        # Every byte was acknowledged, then the board reset the connection: it rebooted before its reply reached us
        # (seen on every update from this firmware). Whether the update took is decided by the build changing.
        s.close()
        return "RESET", ""
    s.close()
    head_end = resp.find(b"\r\n\r\n")
    status_line = resp.split(b"\r\n", 1)[0].decode(errors="replace")
    body = resp[head_end + 4:].decode(errors="replace") if head_end >= 0 else ""
    return status_line, body


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    here = os.path.dirname(os.path.abspath(__file__))
    # No defaults: every board answers to "hdmilight" until it is renamed, and each kind of board has its own image.
    ap.add_argument("host", nargs="?", help="hostname or IP of the board; asked for when left out")
    ap.add_argument("image", nargs="?", help="the image to send, e.g. firmware/build-oneboard/hdmilight_v3.bin")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--mss", type=int, default=400)
    ap.add_argument("--inflight", type=int, default=2)
    ap.add_argument("--no-status", action="store_true", help="skip the before/after /api/status checks (tests)")
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    if not a.host:
        if not sys.stdin.isatty():
            ap.error("give the board's hostname or IP")
        a.host = input("Board to update (hostname or IP): ").strip()
        if not a.host:
            sys.exit("no host given")
    if not a.image or not os.path.isfile(a.image):
        built = sorted(glob.glob(os.path.join(here, "..", "firmware", "build-*", "hdmilight_v3.bin")))
        sys.exit("name the image to send; built ones:\n" + "\n".join("  " + os.path.relpath(b) for b in built))
    if a.mss * a.inflight > 1100:
        sys.exit("mss x inflight must stay well under 1536 bytes (headers and other traffic need room)")
    elf = os.path.join(os.path.dirname(os.path.abspath(a.image)), "hdmilight_v3.elf")
    if os.path.exists(elf):   # keep the ELF of every image sent, so tools/crash_decode.py can decode its crashes
        import shutil
        sha = hashlib.sha256(open(elf, "rb").read()).hexdigest()[:8]
        arch = os.path.join(here, "..", "firmware", ".elf-archive")
        os.makedirs(arch, exist_ok=True)
        shutil.copy2(elf, os.path.join(arch, f"{sha}.elf"))
        print(f"archived ELF {sha}")
    before = None
    if not a.no_status:
        before = status(a.host).get("build")
        print("before:", before)
    line, body = upload(a.host, a.port, a.image, a.mss, a.inflight)
    print(line)
    print(body)
    reset = line == "RESET"
    if reset:
        print("connection reset after all data was acknowledged (board rebooting?); checking the build")
    elif " 200 " not in line or '"ok":true' not in body.replace(" ", ""):
        sys.exit("upload rejected")
    if a.no_status:
        return
    print("rebooting...")
    time.sleep(25)
    for _ in range(12):
        try:
            after = status(a.host).get("build")
            print("after: ", after)
            if reset and after == before:
                sys.exit("build unchanged: the update did not take")
            return
        except SystemExit:
            raise
        except Exception:
            time.sleep(5)
    sys.exit("board did not answer after the update")


if __name__ == "__main__":
    main()
