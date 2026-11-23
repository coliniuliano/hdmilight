#!/usr/bin/env python3
"""Local test for tools/ota_paced.py against a mock of the firmware's /api/update (127.0.0.1 only).

The mock behaves like web.c's h_update: a pause before the body (esp_ota_begin erasing), 16 KiB reads with a short
pause each (flash writes), a 5 s receive timeout (httpd's), and the same JSON reply. Checks: the image arrives
byte-identical, the effective segment size is the requested one, unacknowledged data never exceeds the in-flight
limit, and a receiver that stops reading is reported as a stall rather than hanging.
"""
import hashlib
import json
import os
import socket
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ota_paced  # noqa: E402


def mock_server(port, ready, result, erase_s=3.0, write_s=0.03, stop_after=None):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(1)
    ready.set()
    c, _ = ls.accept()
    c.settimeout(5)
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += c.recv(1024)
    head, rest = buf.split(b"\r\n\r\n", 1)
    length = int([l for l in head.split(b"\r\n") if l.lower().startswith(b"content-length:")][0].split(b":")[1])
    time.sleep(erase_s)                       # esp_ota_begin erasing the slot
    body, h = rest, hashlib.sha256(rest)
    try:
        while len(body) < length:
            if stop_after is not None and len(body) >= stop_after:
                time.sleep(60)                # receiver wedged: stops reading and ACKing (window fills)
            chunk = c.recv(min(16384, length - len(body)))
            if not chunk:
                break
            body += chunk
            h.update(chunk)
            time.sleep(write_s)               # esp_ota_write
        result["sha"] = hashlib.sha256(body).hexdigest()
        result["bytes"] = len(body)
        msg = json.dumps({"ok": True, "bytes": len(body), "slot": "ota_0", "note": "rebooting"})
        c.sendall(f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {len(msg)}\r\n\r\n{msg}".encode())
    except socket.timeout:
        result["error"] = "server recv timeout"
    c.close()
    ls.close()


def run(image, port, mss, inflight, **kw):
    ready, result = threading.Event(), {}
    t = threading.Thread(target=mock_server, args=(port, ready, result), kwargs=kw, daemon=True)
    t.start()
    ready.wait()
    peak = {"after_send": 0}
    real_unacked, real_send = ota_paced.unacked, socket.socket.send

    def tracking_send(self, b, *a):
        n = real_send(self, b, *a)
        peak["after_send"] = max(peak["after_send"], real_unacked(self))
        return n
    socket.socket.send = tracking_send
    try:
        line, body = ota_paced.upload("127.0.0.1", port, image, mss, inflight, stall_s=8, log=lambda *_: None)
    finally:
        socket.socket.send = real_send
    t.join(10)
    return line, body, result, peak["after_send"]


def main():
    data = os.urandom(1_200_000)
    with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as f:
        f.write(data)
        image = f.name
    want = hashlib.sha256(data).hexdigest()
    ok = True

    t0 = time.time()
    line, body, res, peak = run(image, 18901, 400, 2)
    dt = time.time() - t0
    good = " 200 " in line and res.get("sha") == want and peak <= 2 * 400
    print(f"{'ok' if good else 'FAIL'}: 1.2 MB upload byte-identical={res.get('sha') == want}, "
          f"peak unacknowledged after a send {peak} B (limit 800), {dt:.1f} s, reply {body}")
    ok &= good

    s = socket.socket()
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 400)
    ready, result = threading.Event(), {}
    threading.Thread(target=mock_server, args=(18902, ready, result), kwargs={"erase_s": 0}, daemon=True).start()
    ready.wait()
    s.connect(("127.0.0.1", 18902))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 400)    # as ota_paced does after connect
    eff = s.getsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG)
    s.close()
    print(f"{'ok' if eff == 400 else 'FAIL'}: effective segment size after connect {eff} B (asked 400)")
    ok &= eff == 400

    try:
        run(image, 18903, 400, 2, erase_s=0, stop_after=300_000)
        print("FAIL: wedged receiver was not reported")
        ok = False
    except RuntimeError as e:
        print(f"ok: wedged receiver reported: {e}")

    os.unlink(image)
    print("ALL PASSED" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
