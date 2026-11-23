"""Fault and load tests for the host build of firmware/main/esphome_api.c. Run through run.sh.

argv: port pid idle_timeout_ms [fd_limited]
  normal mode: garbage frames, oversized frames, byte-by-byte frames, idle timeout, eviction when full,
               200 rapid connect/disconnects without a descriptor leak, server CPU stays low throughout.
  fd_limited:  the server runs with so few descriptors that accept() fails; it must not spin, and must serve
               again once the idle timeout frees descriptors.
"""
import asyncio
import os
import socket
import subprocess
import sys
import time

from aioesphomeapi import APIClient

PORT, PID, IDLE_MS = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
FD_LIMITED = len(sys.argv) > 4

HELLO = bytes([0x00, 0x00, 0x01])  # plaintext frame, empty HelloRequest (all fields optional)


def cpu_seconds():
    out = subprocess.run(["ps", "-o", "time=", "-p", str(PID)], capture_output=True, text=True).stdout.strip()
    parts = out.replace("-", ":").split(":")
    return sum(float(x) * 60 ** i for i, x in enumerate(reversed(parts)))


def open_fds():
    out = subprocess.run(["lsof", "-p", str(PID)], capture_output=True, text=True).stdout
    return sum(1 for line in out.splitlines() if "TCP" in line or "IPv4" in line)


def raw(timeout=3.0):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    return s


def closed_by_server(s, within):
    s.settimeout(within)
    try:
        return s.recv(64) == b""
    except (socket.timeout, TimeoutError):
        return False
    except ConnectionResetError:
        return True


async def api_ok():
    cli = APIClient("127.0.0.1", PORT, "")
    await asyncio.wait_for(cli.connect(login=True), 5)
    info = await asyncio.wait_for(cli.device_info(), 5)
    await cli.disconnect()
    return info.name == "hdmilight"


def check(cond, what):
    if not cond:
        raise AssertionError(what)
    print("ok:", what)


async def normal():
    cpu0 = cpu_seconds()

    s = raw(); s.sendall(b"\x01\x02\x03")
    check(closed_by_server(s, 2), "non-plaintext first byte -> connection closed")

    s = raw(); s.sendall(bytes([0x00, 0xFF, 0x7F, 0x01]))  # length 16383
    check(closed_by_server(s, 2), "oversized frame -> connection closed")

    s = raw()
    for b in HELLO:
        s.sendall(bytes([b])); time.sleep(0.05)
    s.settimeout(2); hdr = s.recv(3)
    check(len(hdr) >= 3 and hdr[0] == 0 and hdr[2] == 2, "hello sent one byte at a time -> HelloResponse")
    s.close()

    s = raw()
    check(closed_by_server(s, IDLE_MS / 1000 + 2), f"silent client dropped after the {IDLE_MS} ms idle timeout")

    idle = [raw() for _ in range(3)]
    time.sleep(0.3)
    check(await api_ok(), "4th client served while 3 idle clients held every slot (stalest evicted)")
    evicted = sum(closed_by_server(c, 0.5) for c in idle)
    check(evicted >= 1, f"{evicted} idle client(s) evicted to make room")
    for c in idle:
        c.close()

    time.sleep(0.5)
    before = open_fds()
    for _ in range(200):
        c = raw(); c.sendall(HELLO); c.close()
    time.sleep(1.0)
    after = open_fds()
    check(after <= before + 1, f"200 rapid connect/disconnects: server sockets {before} -> {after}")
    check(await api_ok(), "still serving after the connection storm")

    used = cpu_seconds() - cpu0
    check(used < 2.0, f"server CPU over the whole run {used:.2f} s (no spinning)")


async def fd_limited():
    # The server has room for its listening socket and about 3 clients. Hold them, then queue more.
    held = []
    for _ in range(6):
        try:
            held.append(raw(1))
        except OSError:
            break
    time.sleep(0.5)
    cpu0 = cpu_seconds()
    time.sleep(3.0)
    used = cpu_seconds() - cpu0
    check(used < 0.5, f"descriptors exhausted, accept failing: server CPU {used:.2f} s over 3 s (no spin)")
    for c in held:
        c.close()
    time.sleep(IDLE_MS / 1000 + 1.5)
    check(await api_ok(), "serves again once descriptors are free")


asyncio.run(fd_limited() if FD_LIMITED else normal())
print("ALL PASSED" + (" (fd-limited)" if FD_LIMITED else ""))
