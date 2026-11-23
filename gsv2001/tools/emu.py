#!/usr/bin/env python3
"""Run the GSV2001 driver ("Gsv2k1", SDK 20210311) inside Kramer's KDS-EN7 MCU firmware under Unicorn, with the board's
I2C functions replaced by a register model, and print every transaction: the reference the GSV2001 variant of the
rewritten SDK (../sdk) is checked against.

  tools/emu.py init                  chip init only
  tools/emu.py scenario NAME [N]     build chip + ports as the firmware's app does for a GSV2001 board, then N passes
  options: --calls (append the calling firmware functions to each transfer), --variant 2 (run the Gsv2k2 driver in the
           same image instead), --image PATH (default work/kramer/IP5000-A30V0.0.20.bin)

The firmware is not in the repository: ../sdk/gsv_sdk.h says where it comes from. The image holds three drivers (Gsv2k1, 2k2,
2k8) behind one common layer; the chip type given to the SDK picks one. Debug text the SDK prints is kept as
"  log: ..." lines (the replay harness skips lines that start with a space)."""
import struct, sys, os, re, bisect
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
BASE = 0x08006000
C_MAIN, MAIN = 0x08006130, 0x0803369c           # the C library's entry (copies initialised data) and the app's main()
BSP_I2C_READ, BSP_I2C_WRITE, BSP_UART_TX, BSP_UART_RX, BSP_GET_MS, BSP_GET_KEY, BSP_IRDA = 0x08010224, 0x0801026a, 0x080102d4, 0x080102bc, 0x08010214, 0x0801020c, 0x080102b4
USER_KEY, USER_UART, USER_IRDA = 0x08030dba, 0x08030dbc, 0x08030db8
READ_EDID, SCRATCH = 0x0803045c, 0x2000e000       # read a Tx port's sink EDID (port, 256-byte buffer)
EVENT = 0x0800d26c                               # the app's event handler (port, event, a, b)
DRV_CONNECT = 0x0800efe2                         # per-chip dispatch of the driver's connect routine (source, sink, what)
READ_BKSV, TX_PLUG, TX_PLUG_END = 0x08019004, 0x080175a8, 0x08018190   # Gsv2k1TxReadBksv; Gsv2k1TxPlug and the function after it
F = dict(write_bits=0x0800d210, hook_bsp=0x0800f0c0, hook_user=0x0800ed40, api_init=0x0800a7ca, chip_create=0x0800a628, port_create=0x0800a640,
         chip_init=0x0800a7d8, ports_init_all=0x0800aba0, port_link=0x0800a688, fsm_run=0x0800abc0)
RAM_TXB_ALLOWED = 0x20000050
RAM_BOARD, RAM_CHIP_TYPE = 0x2000000c, 0x200000ae      # app globals: board build (1 = one input) and chip type (1 = Gsv2k1)
CHIP, DESC, PORT0, PORT = 0x2000c000, 0x2000c040, 0x2000c100, 0x88
# (name, id, kind): the app's video ports for a one-input board (its audio and CEC ports are left out)
PORTS = {1: [('rx0', 0, 0), ('txa', 4, 5), ('txb', 5, 5)],
         2: [('rx0', 0, 0), ('rx1', 1, 0), ('txa', 4, 5), ('txb', 5, 5)]}
RET_MAGIC = 0x0803fffe
STACK_TOP = 0x2000f000
CLOCK_STEP_MS = 20

class RegModel:
    def __init__(self): self.regs = {}; self.hdcp22_sinks = set()
    def read(self, dev, reg):
        # the Tx DDC byte register (page 0x60 / 0x70 reg 0x1d): a sink that does HDCP 2.2 answers 4 at HDCP offset 0x50
        if (reg & 0xff) == 0x1d and (reg >> 8) in self.hdcp22_sinks and self.regs.get((dev, (reg & 0xff00) | 0x1e), 0) | 1 == 0x75 \
           and self.regs.get((dev, (reg & 0xff00) | 0x20), 0) == 0x50: return 4
        return self.regs.get((dev, reg), 0)
    def write(self, dev, reg, val): self.regs[(dev, reg)] = val
    def set(self, page, reg, val, dev=0xb0): self.regs[(dev, (page << 8) | reg)] = val

class Emu:
    def __init__(self, image, calls=False):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(0x08000000, 0x40000); uc.mem_write(BASE, image)
        uc.mem_map(0x20000000, 0x10000)
        uc.mem_map(0x40000000, 0x100000); uc.mem_map(0x48000000, 0x100000)
        uc.mem_map(0xE0000000, 0x100000)
        self.model = RegModel(); self.log = []; self.clock = 1000
        self.model.set(0x02, 0x9c, 0x20); self.model.set(0x02, 0x91, 0x90)       # Rx PLL calibrated
        self.stacks = calls; self.image_len = len(image); self.funcs = []; self.stop_at = None
        self.skip_connect = False; self.no_hdcp_ports = set()
        if calls:
            d = ROOT + '/work/kramer/ip5000_decomp.c'
            if os.path.exists(d): self.funcs = sorted(int(m, 16) for m in re.findall(r'/\* ==== \S+ @ ([0-9a-f]{8})', open(d).read()))
        uc.hook_add(UC_HOOK_CODE, self._code)
        uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, self._unmapped)
    def _unmapped(self, uc, access, addr, size, value, data):
        raise RuntimeError(f"unmapped access at 0x{addr:x} pc=0x{uc.reg_read(UC_ARM_REG_PC):x}")
    def _ret(self, r0=0):
        self.uc.reg_write(UC_ARM_REG_R0, r0); self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))
    def _stack(self):
        """The firmware functions on the call stack, innermost first: every stack word that is a return address after a BL."""
        uc = self.uc; sp = uc.reg_read(UC_ARM_REG_SP); out = []
        words = [uc.reg_read(UC_ARM_REG_LR)] + list(struct.unpack(f'<{(STACK_TOP - sp) // 4}I', bytes(uc.mem_read(sp, STACK_TOP - sp))))
        for w in words:
            if not (w & 1) or not (BASE + 4 <= w < BASE + self.image_len): continue
            hi, lo = struct.unpack('<HH', bytes(uc.mem_read((w & ~1) - 4, 4)))
            if not ((hi & 0xf800) == 0xf000 and (lo & 0xd000) == 0xd000) and (lo & 0xff87) != 0x4780: continue
            i = bisect.bisect_right(self.funcs, w & ~1) - 1
            if i >= 0 and (not out or out[-1] != self.funcs[i]): out.append(self.funcs[i])
        if out and 0x0800cf88 <= out[0] < 0x0800d26c: out = out[1:]      # the register access helper itself
        return ' <- '.join(f'{a:08x}' for a in out[:8])
    def _args(self): return [self.uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    def _code(self, uc, addr, size, data):
        if addr == self.stop_at: uc.emu_stop(); return
        if addr == DRV_CONNECT and self.skip_connect: self._ret(0); return
        if addr == READ_BKSV and self.no_hdcp_ports:
            # An output the app marks as non-HDCP: leave out the BKSV read Gsv2k1TxPlug repeats on every pass while the
            # sink is not HDCP capable (the board firmware does the same; the read inside the EDID read stays).
            port = (uc.reg_read(UC_ARM_REG_R0) - PORT0) // PORT
            if port in self.no_hdcp_ports and TX_PLUG <= (uc.reg_read(UC_ARM_REG_LR) & ~1) < TX_PLUG_END: self._ret(0); return
        if addr == BSP_I2C_WRITE or addr == BSP_I2C_READ:
            desc, reg, buf, n = self._args(); dev = (desc >> 8) & 0xff; reg16 = ((desc & 0xff) << 8) | (reg & 0xff); n &= 0xffff
            if addr == BSP_I2C_WRITE:
                b = bytes(uc.mem_read(buf, n))
                for i, x in enumerate(b): self.model.write(dev, (reg16 + i) & 0xffff, x)
                self.log.append(('W', dev, reg16, b, self._stack() if self.stacks else ''))
            else:
                b = bytes(self.model.read(dev, (reg16 + i) & 0xffff) for i in range(n))
                uc.mem_write(buf, b); self.log.append(('R', dev, reg16, b, self._stack() if self.stacks else ''))
            self._ret(0)
        elif addr == BSP_UART_TX:
            buf, n = self._args()[:2]; t = bytes(uc.mem_read(buf, n & 0xffff)).decode('latin1').rstrip('\r\n')
            self.log.append(('L', t, 0, b'', '')); self._ret(0)
        elif addr == BSP_GET_MS:
            uc.mem_write(self._args()[0], struct.pack('<I', self.clock)); self._ret(0)
        elif addr in (BSP_UART_RX, BSP_GET_KEY, BSP_IRDA, USER_KEY, USER_UART, USER_IRDA): self._ret(0)
        elif addr == EVENT:
            # App policy. The Kramer app answers these from its own configuration (HDCP modes, which EDID to offer),
            # which does not exist here. Give the answers the EZ-SP12H2 app gives (../sdk gsv_event; this SDK
            # numbers the events differently, the CEC ones sit in between), so the SDK goes on to plug, EDID and HDCP:
            # 0x52 (there 10) Rx reset -> HDCP supported; 0x53 (0xb) Tx reset -> HDCP type 2; 0x54 (0xc) -> the Rx EDID
            # is ready. 0x55 (0xd, the sink supports everything) and the rest run as they are.
            port, ev = self._args()[:2]
            self.log.append(('L', f'[event {ev:#x} port {(port - PORT0) // PORT}]', 0, b'', ''))
            def ptr(off): return struct.unpack('<I', bytes(uc.mem_read(port + off, 4)))[0]
            if ev == 0x52: uc.mem_write(ptr(0x44), b'\x01')
            elif ev == 0x53:                   # HDCP type 2 (try 2.2, then 1.4); 0 (never) for an output the app marks as non-HDCP
                uc.mem_write(ptr(0x48) + 2, b'\x00\x00' if (port - PORT0) // PORT in self.no_hdcp_ports else b'\x02\x00')
            elif ev == 0x54: uc.mem_write(ptr(0x3c) + 6, b'\x05')
            elif ev == 0x57:                   # (0xf) a sink's EDID was read: read it once more for the app, as the
                uc.mem_write(ptr(0x40) + 0x10, b'\xff\xff\xff\xff')     # EZ-SP12H2 app does, and assume it supports everything
                uc.reg_write(UC_ARM_REG_R1, SCRATCH); uc.reg_write(UC_ARM_REG_PC, READ_EDID | 1); return
            else: return
            self._ret(0)
    def start(self):
        """Let the C library copy the initialised data, up to the app's main()."""
        uc = self.uc; uc.reg_write(UC_ARM_REG_SP, STACK_TOP); self.stop_at = MAIN
        uc.emu_start(C_MAIN | 1, 0, count=20_000_000)
        if uc.reg_read(UC_ARM_REG_PC) & ~1 != MAIN: raise RuntimeError(f"start-up did not reach main: pc=0x{uc.reg_read(UC_ARM_REG_PC):x}")
        self.stop_at = None
    def call(self, func, *args, max_insns=60_000_000):
        uc = self.uc
        for i, a in enumerate(args[:4]): uc.reg_write(UC_ARM_REG_R0 + i, a)
        sp = STACK_TOP
        if len(args) > 4:
            extra = args[4:]; sp -= 4 * len(extra)
            for i, a in enumerate(extra): uc.mem_write(sp + 4 * i, struct.pack('<I', a))
        uc.reg_write(UC_ARM_REG_SP, sp); uc.reg_write(UC_ARM_REG_LR, RET_MAGIC | 1)
        uc.emu_start(func | 1, RET_MAGIC, count=max_insns)
        pc = uc.reg_read(UC_ARM_REG_PC)
        if pc & ~1 != RET_MAGIC: raise RuntimeError(f"did not return: stopped at pc=0x{pc:x} after {len(self.log)} ops; last {self.log[-3:]}")
        return uc.reg_read(UC_ARM_REG_R0)
    def mark(self, text): self.log.append(('#', text, 0, b'', ''))

def fmt(e):
    kind, a, b, data, stack = e
    if kind == '#': return f"# {a}"
    if kind == 'L': return f"  log: {a}"
    return f"{kind} dev 0x{a:02x} page 0x{b >> 8:02x} reg 0x{b & 0xff:02x} " + " ".join(f"{x:02x}" for x in data) + (f"    ; {stack}" if stack else "")

def create(e, variant, scaler=False):
    uc = e.uc
    uc.mem_write(RAM_BOARD, bytes([1 if variant == 1 else 4])); uc.mem_write(RAM_CHIP_TYPE, bytes([variant]))
    uc.mem_write(RAM_TXB_ALLOWED, b'\x01')            # Kramer's app holds output B in reset until it sets this
    e.call(F['api_init'])
    e.call(F['hook_bsp'], BSP_I2C_READ | 1, BSP_I2C_WRITE | 1, BSP_UART_TX | 1, BSP_UART_RX | 1, BSP_GET_MS | 1, BSP_GET_KEY | 1, BSP_IRDA | 1)
    e.call(F['hook_user'], USER_KEY | 1, USER_UART | 1, USER_IRDA | 1)
    uc.mem_write(DESC, struct.pack('<I', 0x0001b000))
    e.call(F['chip_create'], CHIP, variant, 0, DESC, PORT0, 0)
    addr = {}
    for i, (name, pid, kind) in enumerate(PORTS[variant] + ([('scaler', 0x14, 8)] if scaler else [])):
        addr[name] = PORT0 + i * PORT
        e.call(F['port_create'], CHIP, addr[name], pid, kind)
    return addr

def run_init(image, calls=False, variant=1):
    e = Emu(image, calls); e.start(); create(e, variant)
    e.call(F['chip_init'], CHIP)
    return e.log

def build_system(e, variant, scaler=False, passthrough_a=False):
    addr = create(e, variant, scaler)
    e.call(F['chip_init'], CHIP)
    e.call(F['ports_init_all'])
    if passthrough_a:
        # Output A as a pass-through of the input. The driver keeps an output it has not connected at index 0xff and
        # has a code path for that everywhere (17 functions check the index), but its own connect routine always
        # takes index 0, so no application reaches it on one chip. Link the ports the way the SDK does when source
        # and sink are on different chips (the driver's connect routine is not called), then set the output's field
        # in page 1 reg 9 to 2 with the SDK's own register helper: the value that routine writes for index 0xff.
        e.skip_connect = True
        e.call(F['port_link'], addr['rx0'], addr['txa'], 2)
        e.skip_connect = False
        e.call(F['write_bits'], 0x0001b001, 9, 3, 0, 2)
    else:
        e.call(F['port_link'], addr['rx0'], addr['txa'], 2)
    e.call(F['port_link'], addr['rx0'], addr['txb'], 2)
    return addr

def run_scenario(image, name, ticks, calls=False, variant=1):
    e = Emu(image, calls); e.start()
    if name in NO_HDCP_B: e.no_hdcp_ports.add(2)        # port 2 = Tx B
    actions = ACTIONS.get(name, {})
    e.mark("build"); addr = build_system(e, variant, scaler=bool(actions), passthrough_a=name in PASSTHROUGH_A)
    if -1 in actions: actions[-1](e, addr)             # what the app does right after building the system
    script = SCENARIOS[name]
    for t in range(ticks):
        e.mark(f"tick {t}")
        if t in script: script[t](e.model)
        if t in actions: actions[t](e, addr)
        e.clock += CLOCK_STEP_MS
        e.call(F['fsm_run'])
    return e.log

def ev_5v(on):            # Rx0 +5V detect: page 3 reg 0x10 bit 1
    return lambda m: m.set(0x03, 0x10, 0x02 if on else 0x00)

def ev_lock(m):
    # Rx0 sees a 1080p60 HDMI source: TMDS PLL/DE/clock locked, 8-bit, 148.5 MHz, eye widths good
    m.set(0x20, 0x00, 0x98 | 0x20)       # lock_b(7) lock_c(4) lock_a(3) hdmi_mode(5)
    m.set(0x20, 0x03, 0x00)              # 8-bit, colordepth 0
    m.set(0x02, 0x39, 0x02); m.set(0x02, 0x3a, 0xe6); m.set(0x02, 0x2f, 0x00)   # 742 / 5 = 148 MHz
    for lane in range(3):
        m.set(0x27 + lane, 0x20, 0x10); m.set(0x27 + lane, 0x92, 0x17); m.set(0x27 + lane, 0x93, 0x70)
        m.set(0x27 + lane, 0x96, 0x20); m.set(0x27 + lane, 0x3a, 0x00); m.set(0x27 + lane, 0x0e, 0x00)
    # timing: 1920x1080 @ 60, htotal 2200, vtotal 1125
    m.set(0x20, 0x26, 0x07); m.set(0x20, 0x27, 0x80)   # hactive 1920
    m.set(0x20, 0x0e, 0x04); m.set(0x20, 0x0f, 0x65)   # vtotal 1125
    m.set(0x20, 0x2e, 0x08); m.set(0x20, 0x2f, 0x70)   # vactive*2 = 2160
    m.set(0x20, 0x0c, 0x08); m.set(0x20, 0x0d, 0x98)   # htotal 2200, progressive, sync pols 0

def sink_edid():
    # a minimal 1080p HDMI sink EDID: base block + CEA-861 block with an HDMI VSDB (physical address 1.0.0.0)
    b0 = bytearray(128)
    b0[0:8] = b'\x00\xff\xff\xff\xff\xff\xff\x00'; b0[8:10] = b'\x1e\x6d'; b0[0x12:0x14] = b'\x01\x03'; b0[0x14] = 0x80
    b0[0x36:0x48] = bytes.fromhex('023a801871382d40582c4500c48e2100001e'); b0[0x7e] = 1
    b0[0x7f] = (-sum(b0[:0x7f])) & 0xff
    b1 = bytearray(128); b1[0:4] = b'\x02\x03\x12\x70'
    b1[4:9] = bytes.fromhex('4310040122'); b1[9:16] = bytes.fromhex('66030c00100078')
    b1[0x7f] = (-sum(b1[:0x7f])) & 0xff
    return bytes(b0 + b1)

def ev_sink(m):
    # a TV appears on Tx A: HPD, DDC ready, HDCP 1.4-capable sink, Tx PLL locked at 148 MHz
    m.set(0x60, 0xf1, 0x01); m.set(0x60, 0xc5, 0x10); m.set(0x60, 0xc8, 0x04)
    m.set(0x60, 0x1c, 0x01); m.set(0x60, 0x1d, 0x0f)   # DDC always completes and answers 0x0f: a BKSV with 20 ones
    for i, b in enumerate(sink_edid()): m.set(0x61, i, b)
    m.set(0x02, 0x8c, 0x81); m.set(0x02, 0x8d, 0x00); m.set(0x02, 0x8e, 0x01); m.set(0x02, 0x8f, 0x00)
    m.set(0x02, 0x90, 0x02); m.set(0x02, 0x91, 0xe6); m.set(0x02, 0x92, 0x00); m.set(0x02, 0x9b, 0x08)

def ev_sink_b(m):
    # the same sink on Tx B (register blocks 0x70.. instead of 0x60..); the Tx PLL answers are shared with Tx A
    m.set(0x70, 0xf1, 0x01); m.set(0x70, 0xc5, 0x10); m.set(0x70, 0xc8, 0x04)
    m.set(0x70, 0x1c, 0x01); m.set(0x70, 0x1d, 0x0f)
    for i, b in enumerate(sink_edid()): m.set(0x71, i, b)

def ev_lock_4k(m):
    # 3840x2160p60 instead: 594 MHz TMDS, htotal 4400, vtotal 2250
    ev_lock(m)
    m.set(0x02, 0x39, 0x0b); m.set(0x02, 0x3a, 0x9a)   # 2970 / 5 = 594
    m.set(0x20, 0x26, 0x0f); m.set(0x20, 0x27, 0x00)   # hactive 3840
    m.set(0x20, 0x0e, 0x08); m.set(0x20, 0x0f, 0xca)   # vtotal 2250
    m.set(0x20, 0x2e, 0x10); m.set(0x20, 0x2f, 0xe0)   # vactive*2 = 4320
    m.set(0x20, 0x0c, 0x11); m.set(0x20, 0x0d, 0x30)   # htotal 4400

def ev_encrypt(m):
    # the source turns on HDCP 1.4: encryption interrupt + state on the Rx, sink reports HDCP 1.4 capable
    m.set(0x03, 0x39, 0x30); m.set(0x03, 0x5f, 0x01); m.set(0x60, 0xc8, 0x04); m.set(0x60, 0xf2, 0x80)

def ev_encrypt_both(m):
    ev_encrypt(m); m.set(0x70, 0xc8, 0x04); m.set(0x70, 0xf2, 0x80)     # the sink on Tx B is HDCP 1.4 capable too

def sink_edid():
    # A 1080p HDMI sink: base block + CEA-861 block with video (VIC 16, 4, 1), basic audio and the HDMI vendor block
    # (physical address 1.0.0.0). It must be well formed: with the vendor block swallowed by a wrong audio-block length
    # the SDK treats the sink as DVI.
    b0 = bytearray(128)
    b0[0:8] = b'\x00\xff\xff\xff\xff\xff\xff\x00'; b0[8:10] = b'\x1e\x6d'; b0[0x12:0x14] = b'\x01\x03'; b0[0x14] = 0x80
    b0[0x36:0x48] = bytes.fromhex('023a801871382d40582c4500c48e2100001e'); b0[0x7e] = 1
    b0[0x7f] = (-sum(b0[:0x7f])) & 0xff
    blocks = bytes.fromhex('43100401' '23090707' '66030c00100078')
    b1 = bytearray(128); b1[0:4] = bytes([0x02, 0x03, 4 + len(blocks), 0x70]); b1[4:4 + len(blocks)] = blocks
    b1[0x7f] = (-sum(b1[:0x7f])) & 0xff
    return bytes(b0 + b1)

SCENARIOS = {                                          # nothing connected, then a source, then sinks
    'idle': {},                                        # nothing connected
    'plug': {1: ev_5v(1)},                             # a source's +5V appears on the input at tick 1
    'lock': {1: ev_5v(1), 55: ev_lock},                # ...and the TMDS link locks after power-up
    'sink': {1: ev_5v(1), 55: ev_lock, 60: ev_sink},   # ...and a TV is plugged into Tx A
    'hdcp': {1: ev_5v(1), 55: ev_lock, 60: ev_sink, 100: ev_encrypt},
    'board': {1: ev_5v(1), 55: ev_lock_4k, 60: ev_sink, 64: ev_sink_b},      # 4K source, sinks on both outputs
    'boardhdcp': {1: ev_5v(1), 55: ev_lock_4k, 60: ev_sink, 64: ev_sink_b, 150: ev_encrypt_both},
}

def act_scale_txb(e, addr):
    # what an app does to put the down-scaler in front of output B (the SDK's route "Rx - Scaler - Tx")
    e.call(F['port_link'], addr['rx0'], addr['scaler'], 0)
    e.call(F['port_link'], addr['scaler'], addr['txb'], 0)
def act_direct_txb(e, addr):
    e.call(F['port_link'], addr['rx0'], addr['txb'], 2)
def hdmi_source(m, htotal, vtotal, hactive, vactive, tmds5, vic):
    # A real HDMI source as the driver's registers read it (the SDK's frame-rate sum shows which register is which):
    # page 0x20 reg 0x26 = H total, 0x2e = 2 x V total, 0x0c = H active, 0x0e = V active; page 2 reg 0x39 = TMDS clock
    # x 5 in MHz; lock and HDMI-mode status bits; and an RGB AVI infoframe with the VIC (page 0x24: header at 0xe0,
    # body from 0x00; page 0x22 reg 0x19 bit 6 = AVI present).
    m.set(0x20, 0x00, 0xb8); m.set(0x20, 0x03, 0x00)                  # TMDS / DE / V locked, HDMI mode, 8 bit
    m.set(0x02, 0x39, tmds5 >> 8); m.set(0x02, 0x3a, tmds5 & 0xff); m.set(0x02, 0x2f, 0x00)
    for lane in range(3):                                             # equaliser converged, eyes wide open
        m.set(0x27 + lane, 0x20, 0x10); m.set(0x27 + lane, 0x92, 0x17); m.set(0x27 + lane, 0x93, 0x70)
        m.set(0x27 + lane, 0x96, 0x20); m.set(0x27 + lane, 0x3a, 0x00); m.set(0x27 + lane, 0x0e, 0x00)
    m.set(0x20, 0x26, htotal >> 8); m.set(0x20, 0x27, htotal & 0xff)
    m.set(0x20, 0x2e, (2 * vtotal) >> 8); m.set(0x20, 0x2f, (2 * vtotal) & 0xff)
    m.set(0x20, 0x0c, hactive >> 8); m.set(0x20, 0x0d, hactive & 0xff)
    m.set(0x20, 0x0e, vactive >> 8); m.set(0x20, 0x0f, vactive & 0xff)
    hdr = [0x82, 0x02, 0x0d]; body = [0x00, 0x28, 0x00, vic, 0x00] + [0] * 8      # RGB, 16:9
    for i, b in enumerate(hdr): m.set(0x24, 0xe0 + i, b)
    m.set(0x24, 0x00, (-(sum(hdr) + sum(body))) & 0xff)
    for i, b in enumerate(body): m.set(0x24, 0x01 + i, b)
    m.set(0x22, 0x19, 0x40)
    m.set(0x03, 0x35, 0x20)                           # the Rx reports HDMI (not DVI) mode: outputs send infoframes and can encrypt
    m.set(0x03, 0x38, 0x22)                           # raw status: DE regeneration and vertical lock present
def ev_lock_1080p60(m): hdmi_source(m, 2200, 1125, 1920, 1080, 742, 16)        # 148.5 MHz
def ev_lock_4k60(m): hdmi_source(m, 4400, 2250, 3840, 2160, 2970, 97)          # 594 MHz
# real HDMI sources in every scenario (the first events above model a source without
# infoframes and with H/V totals and active sizes swapped)
SCENARIOS.update({
    'lock': {1: ev_5v(1), 55: ev_lock_1080p60},
    'sink': {1: ev_5v(1), 55: ev_lock_1080p60, 60: ev_sink},
    'hdcp': {1: ev_5v(1), 55: ev_lock_1080p60, 60: ev_sink, 100: ev_encrypt},
    'board': {1: ev_5v(1), 55: ev_lock_4k60, 60: ev_sink, 64: ev_sink_b},
    'boardhdcp': {1: ev_5v(1), 55: ev_lock_4k60, 60: ev_sink, 64: ev_sink_b, 150: ev_encrypt_both},
})
SCENARIOS['scale'] = {1: ev_5v(1), 55: ev_lock_4k60, 60: ev_sink, 64: ev_sink_b}     # 4K60 source, sinks on both outputs ...
ACTIONS = {'scale': {120: act_scale_txb, 200: act_direct_txb}}     # ... output B through the scaler from pass 120 to 200
# the HDMI Light board: output A pass-through, output B on the Tx core and through the scaler once the 4K input is up
SCENARIOS['boardpt'] = SCENARIOS['scale']; ACTIONS['boardpt'] = {110: act_scale_txb}
SCENARIOS['boardpthdcp'] = dict(SCENARIOS['scale']); SCENARIOS['boardpthdcp'][220] = ev_encrypt_both; ACTIONS['boardpthdcp'] = {110: act_scale_txb}
def ev_sink_b_nohdcp(m):
    # the sink on output B is a capture chip: an HDMI sink that does not answer HDCP (its BKSV reads as zeros)
    ev_sink_b(m); m.set(0x70, 0x1d, 0x00); m.set(0x70, 0xc8, 0x00)
# the same board with what is really on it: a TV with HDCP on A, the MS2130 without HDCP on B
SCENARIOS['boardms'] = {1: ev_5v(1), 55: ev_lock_4k60, 60: ev_sink, 64: ev_sink_b_nohdcp}; ACTIONS['boardms'] = {110: act_scale_txb}
SCENARIOS['boardmshdcp'] = dict(SCENARIOS['boardms']); SCENARIOS['boardmshdcp'][220] = ev_encrypt; ACTIONS['boardmshdcp'] = {110: act_scale_txb}
def act_set_edid(e, addr):
    # give the input an EDID the way an app does (EZ-SP12H2: 0x08010924): input off, physical address worked out and
    # patched, EDID RAM written, EDID enabled. Uses the SDK's own routines for each step.
    EDID, SPA = 0x2000d000, 0x2000d200
    e.uc.mem_write(EDID, sink_edid()); e.uc.mem_write(SPA, bytes(4))
    e.call(0x0802f804, addr['rx0'])                              # Rx power down (HPD low)
    off = e.call(0x0802f424, EDID, SPA)                          # find the physical address in the EDID
    e.call(0x0802f586, addr['rx0'], SPA, 0)                      # the address this input hands on
    e.call(0x08030198, addr['rx0'], EDID, off, SPA)              # EDID RAM, address patch, enable
    rx = struct.unpack('<I', bytes(e.uc.mem_read(addr['rx0'] + 0x3c, 4)))[0]
    e.uc.mem_write(rx + 6, b'\x05')                              # "EDID ready"
def act_scale_and_edid(e, addr): act_scale_txb(e, addr)
# everything the board firmware does: EDID at start, again once the TV's has been read, pass-through A, scaler on B
SCENARIOS['boardfull'] = dict(SCENARIOS['boardmshdcp'])
ACTIONS['boardfull'] = {-1: act_set_edid, 90: act_set_edid, 110: act_scale_txb}
def ev_tv_hdcp22(m): m.hdcp22_sinks.add(0x60)          # the TV on output A does HDCP 2.2
def ev_encrypt22(m):
    # the source turns on HDCP 2.2: encryption interrupt + state, "running 2.2" on the Rx; the TV's link comes up as
    # HDCP 2.2 (engine type 6, state 0x80)
    m.set(0x03, 0x39, 0x30); m.set(0x03, 0x5f, 0x01); m.set(0x03, 0x3a, 0x02); m.set(0x21, 0x4e, 0x00); m.set(0x21, 0x4f, 0x01)
    m.set(0x60, 0xc8, 0x06); m.set(0x62, 0x14, 0x80)
# the usual real case: 4K60 source and TV both on HDCP 2.2, capture chip without HDCP
SCENARIOS['boardfull22'] = dict(SCENARIOS['boardms']); SCENARIOS['boardfull22'][59] = ev_tv_hdcp22; SCENARIOS['boardfull22'][220] = ev_encrypt22
ACTIONS['boardfull22'] = {-1: act_set_edid, 90: act_set_edid, 110: act_scale_txb}
PASSTHROUGH_A = {'boardpt', 'boardpthdcp', 'boardms', 'boardmshdcp', 'boardfull', 'boardfull22'}
# the board firmware never tries HDCP on output B (the capture chip cannot answer it): these runs do the same
SCENARIOS['boardfw'] = SCENARIOS['boardfull']; ACTIONS['boardfw'] = ACTIONS['boardfull']
SCENARIOS['boardfw22'] = SCENARIOS['boardfull22']; ACTIONS['boardfw22'] = ACTIONS['boardfull22']
SCENARIOS['boardfwplain'] = dict(SCENARIOS['boardms']); ACTIONS['boardfwplain'] = ACTIONS['boardfull']
PASSTHROUGH_A |= {'boardfw', 'boardfw22', 'boardfwplain'}
NO_HDCP_B = {'boardfw', 'boardfw22', 'boardfwplain'}

if __name__ == '__main__':
    argv = sys.argv[1:]
    def opt(name, default):
        return argv[argv.index(name) + 1] if name in argv else default
    image = open(opt('--image', ROOT + '/work/kramer/IP5000-A30V0.0.20.bin'), 'rb').read()
    variant = int(opt('--variant', '1')); calls = '--calls' in argv
    args = [a for i, a in enumerate(argv) if not a.startswith('--') and (i == 0 or argv[i - 1] not in ('--image', '--variant'))]
    what = args[0] if args else 'init'
    if what == 'init': log = run_init(image, calls, variant)
    elif what == 'scenario': log = run_scenario(image, args[1], int(args[2]) if len(args) > 2 else 10, calls, variant)
    else: sys.exit(__doc__)
    for x in log: print(fmt(x))
    print(f"# {sum(1 for x in log if x[0] == 'W')} writes, {sum(1 for x in log if x[0] == 'R')} reads")
