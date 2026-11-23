#!/usr/bin/env python3
"""Run the GSV2001 ("Gsv2k1") SDK inside the Blustream SP12CS firmware under Unicorn, with its I2C hooks replaced by a
register model, and print every transaction. The SP12CS's SDK is a year newer than the Kramer one the library copies
(tools/emu.py); this shows where the two differ.

  tools/emu_sp12cs.py init                  chip init only
  tools/emu_sp12cs.py scenario NAME [N]     build chip + ports as the firmware's app does, then N SDK passes
  scenarios: idle, sink (1080p source, a sink on output A), board (sinks on both outputs)
  options: --calls (append the calling firmware functions to each transfer), --image PATH (default work/sp12cs.bin, from tools/fw2_decode.py)

The firmware is not in the repository: see ../sdk/gsv_sdk.h.

Differences from tools/emu.py: Cortex-M0 image at 0x08003000; the SDK calls the board's I2C functions with (descriptor
0x0001B0pp, register, buffer, count); it has no tick counters but asks the board for the time in milliseconds, so a
virtual clock advances a fixed step per SDK pass (CLOCK_STEP_MS) and per delay call. It also takes "sink present"
from bit 2 of the output's reg 0xf1 (read after clearing the output's bit in page 0 reg 0x90), not bit 0."""
import struct, sys, os
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
BASE = 0x08003000
# board functions the SDK is given (app init passes them to the hook registration at 0x0800da62)
BSP_I2C_READ, BSP_I2C_WRITE, BSP_UART_TX, BSP_UART_RX, BSP_GET_MS, BSP_GET_KEY, BSP_IRDA = 0x0800f0d2, 0x0800f17c, 0x0800f1a2, 0x0800f1ca, 0x0800f1ce, 0x0800f1de, 0x0800f1e4
USER_KEY, USER_UART, USER_IRDA = 0x08008768, 0x08008ca6, 0x080082ba       # app command listeners, called once per port per pass
APP_GET_MS, APP_DELAY_MS = 0x08003422, 0x08003428
F = dict(hook_bsp=0x0800da62, hook_user=0x0800b350, chip_create=0x0800b364, port_create=0x0800b588, chip_init=0x0800b5ae,
         ports_init_all=0x0800b83a, port_link=0x0800b75e, fsm_run=0x0800b5e6, route_update=0x08007e78)
CHIP, RX0 = 0x20000860, 0x20000870                 # where the firmware keeps them
PORT = 0x7c
PORTS = [('rx0', 0, 0), ('txa', 4, 5), ('txb', 5, 5), ('audio', 8, 0xe), ('color', 0x18, 9)]     # (name, id, kind), contiguous from RX0
RET_MAGIC = 0x0801fffe
STACK_TOP = 0x20010000
CLOCK_STEP_MS = 20

class RegModel:
    def __init__(self): self.regs = {}
    def read(self, dev, reg): return self.regs.get((dev, reg), 0)
    def write(self, dev, reg, val): self.regs[(dev, reg)] = val
    def set(self, page, reg, val, dev=0xb0): self.regs[(dev, (page << 8) | reg)] = val

class Emu:
    def __init__(self, image, calls=False):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(0x08000000, 0x20000); uc.mem_write(BASE, image)
        uc.mem_map(0x20000000, 0x10000)
        uc.mem_map(0x40000000, 0x100000); uc.mem_map(0x48000000, 0x100000)
        uc.mem_map(0xE0000000, 0x100000)
        self.model = RegModel(); self.log = []; self.clock = 1000
        self.model.set(0x02, 0x9c, 0x20); self.model.set(0x02, 0x91, 0x90)       # Rx PLL calibrated, as tools/emu.py assumes
        self.stacks = calls; self.image_len = len(image); self.funcs = []
        if calls:                                   # function starts, from the Ghidra dump, to name the callers of each transfer
            import re
            d = ROOT + '/work/sp12cs_decomp.c'
            if os.path.exists(d): self.funcs = sorted(int(m, 16) for m in re.findall(r'/\* ==== FUN_([0-9a-f]{8}) @', open(d).read()))
        uc.hook_add(UC_HOOK_CODE, self._code)
        uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, self._unmapped)
    def _unmapped(self, uc, access, addr, size, value, data):
        raise RuntimeError(f"unmapped access at 0x{addr:x} pc=0x{uc.reg_read(UC_ARM_REG_PC):x}")
    def _ret(self, r0=0):
        self.uc.reg_write(UC_ARM_REG_R0, r0); self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))
    def _stack(self):
        """The firmware functions on the call stack, innermost first: every stack word that is a return address after a BL."""
        import bisect
        uc = self.uc; sp = uc.reg_read(UC_ARM_REG_SP); out = []
        words = [uc.reg_read(UC_ARM_REG_LR)] + list(struct.unpack(f'<{(STACK_TOP - sp) // 4}I', bytes(uc.mem_read(sp, STACK_TOP - sp))))
        for w in words:
            if not (w & 1) or not (BASE + 4 <= w < BASE + self.image_len): continue
            hi, lo = struct.unpack('<HH', bytes(uc.mem_read((w & ~1) - 4, 4)))
            if not ((hi & 0xf800) == 0xf000 and (lo & 0xd000) == 0xd000) and (lo & 0xff87) != 0x4780: continue
            i = bisect.bisect_right(self.funcs, w & ~1) - 1
            if i >= 0 and (not out or out[-1] != self.funcs[i]): out.append(self.funcs[i])
        if out and 0x0800d6de <= out[0] < 0x0800d900: out = out[1:]      # the register access helper itself
        return ' <- '.join(f'{a:08x}' for a in out[:8])
    def _args(self): return [self.uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    def _code(self, uc, addr, size, data):
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
        elif addr == BSP_GET_MS:
            uc.mem_write(self._args()[0], struct.pack('<I', self.clock)); self._ret(0)
        elif addr == APP_GET_MS: self._ret(self.clock)
        elif addr == APP_DELAY_MS: self.clock += self._args()[0] & 0xffff; self._ret(0)
        elif addr in (BSP_UART_TX, BSP_UART_RX, BSP_GET_KEY, BSP_IRDA, USER_KEY, USER_UART, USER_IRDA): self._ret(0)
    def call(self, func, *args, max_insns=40_000_000):
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
    return f"{kind} dev 0x{a:02x} page 0x{b >> 8:02x} reg 0x{b & 0xff:02x} " + " ".join(f"{x:02x}" for x in data) + (f"    ; {stack}" if stack else "")

def hooks(e):
    e.call(F['hook_bsp'], BSP_I2C_READ | 1, BSP_I2C_WRITE | 1, BSP_UART_TX | 1, BSP_UART_RX | 1, BSP_GET_MS | 1, BSP_GET_KEY | 1, BSP_IRDA | 1)
    e.call(F['hook_user'], USER_KEY | 1, USER_UART | 1, USER_IRDA | 1)

def create(e):
    desc = 0x2000f000
    e.uc.mem_write(desc, struct.pack('<I', 0x0001b000))
    e.call(F['chip_create'], CHIP, 1, 0, desc, RX0, 0)
    addr = {}
    for i, (name, pid, kind) in enumerate(PORTS):
        addr[name] = RX0 + i * PORT
        e.call(F['port_create'], CHIP, addr[name], pid, kind)
    return addr

def run_init(image, calls=False):
    e = Emu(image, calls); hooks(e); create(e)
    e.call(F['chip_init'], CHIP)
    return e.log

def build_system(e):
    hooks(e); addr = create(e)
    e.call(F['chip_init'], CHIP)
    e.call(F['ports_init_all'])
    e.call(F['port_link'], addr['rx0'], addr['txa'], 2)
    e.call(F['port_link'], addr['rx0'], addr['txb'], 2)
    e.call(F['port_link'], addr['rx0'], addr['audio'], 1)
    return addr

def run_scenario(image, name, ticks, calls=False):
    e = Emu(image, calls)
    e.mark("build"); build_system(e)
    script = SCENARIOS[name]
    for t in range(ticks):
        e.mark(f"tick {t}")
        if t in script: script[t](e.model)
        e.clock += CLOCK_STEP_MS
        e.call(F['fsm_run'])
        e.call(F['route_update'], CHIP)
    return e.log

def _events():
    """the Kramer emulator's chip events (same chip, same status registers), plus the sink-present bit this SDK reads"""
    import importlib.util
    sp = importlib.util.spec_from_file_location('emu', HERE + '/emu.py'); k = importlib.util.module_from_spec(sp); sp.loader.exec_module(k)
    def sink_a(m): k.ev_sink(m); m.set(0x60, 0xf1, 0x05)
    def sink_b(m): k.ev_sink_b(m); m.set(0x70, 0xf1, 0x05)
    return k, sink_a, sink_b
def _scenarios():
    k, sink_a, sink_b = _events()
    return { 'idle': {},
             'sink': {1: k.ev_5v(1), 55: k.ev_lock_1080p60, 60: sink_a},
             'board': {1: k.ev_5v(1), 55: k.ev_lock_1080p60, 60: sink_a, 64: sink_b} }
SCENARIOS = _scenarios()

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    image = open(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--image'), ROOT + '/work/sp12cs.bin'), 'rb').read()
    args = [a for a in args if not a.endswith('.bin')]
    what = args[0] if args else 'init'
    calls = '--calls' in sys.argv
    if what == 'init': log = run_init(image, calls)
    elif what == 'scenario': log = run_scenario(image, args[1], int(args[2]) if len(args) > 2 else 10, calls)
    else: sys.exit(__doc__)
    for x in log: print(fmt(x))
    print(f"# {sum(1 for x in log if x[0] == 'W')} writes, {sum(1 for x in log if x[0] == 'R')} reads")
