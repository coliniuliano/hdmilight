#!/usr/bin/env python3
"""The oneboard firmware's pin map (firmware/main/oneboard.h) against the board's netlist (hardware/gen/design.py).
Exits non-zero if any pin differs, so the two cannot drift apart unnoticed."""
import os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "hardware", "gen"))
import design

NET = {  # firmware name -> net on the board
    "GSV_SDA": "I2C_SDA", "GSV_SCL": "I2C_SCL", "GSV_RESETB": "GSV_RESETB", "GSV_INT": "GSV_INT", "GSV_CSB": "GSV_CSB",
    "CEC_RX": "CEC_RX", "CEC_TX": "CEC_TX", "STATUS_LED": "ESP_LED", "MS_PWR_EN": "MS_PWR_EN", "MS_DET": "MS_DET",
    "FL_CS": "FL_CS", "FL_SCLK": "FL_SCLK", "FL_MOSI": "FL_MOSI", "FL_MISO": "FL_MISO", "LED_DATA": "LED_DATA",
}
u1 = design.PARTS["U1"]
io_of_net = {}
names = {str(k): v for k, v in u1["names"].items()}
for pad, net in u1["pins"].items():
    m = re.match(r"IO(\d+)", names.get(str(pad), ""))
    if net and m: io_of_net[net] = int(m.group(1))
hdr = open(os.path.join(ROOT, "firmware", "main", "oneboard.h")).read()
fw = {m.group(1): int(m.group(2)) for m in re.finditer(r"#define\s+ONEBOARD_PIN_(\w+)\s+(\d+)", hdr)}
bad = 0
for name, net in NET.items():
    want, got = io_of_net.get(net), fw.get(name)
    ok = want is not None and want == got
    bad += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {name:11} firmware IO{got}  board net {net} = IO{want}")
for name in sorted(set(fw) - set(NET)): print(f"FAIL {name}: in oneboard.h but not checked here"); bad += 1
sys.exit(1 if bad else 0)
