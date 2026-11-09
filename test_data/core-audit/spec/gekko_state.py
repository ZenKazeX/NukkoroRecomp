"""Neutral state container for the independent Gekko specification model.

This file deliberately contains NO instruction semantics. Spec modules
(<area>_<author>.py) implement semantics from docs/reference/ppc/gekko_user_manual.txt
(and ppc_750cl.txt) only - never from the GekkoForge implementation.

Conventions (big-endian PowerPC bit numbering is used in comments; values are ints):
- gpr[0..31]: u32
- fpr[0..31]: [ps0_bits, ps1_bits], each the raw u64 IEEE-754 double image
  (scalar FP instructions use ps0; how ps1 is affected is a spec decision per the manual)
- cr, xer, lr, ctr, msr, fpscr, srr0, srr1, dar, dsisr, dec, hid0, hid2, pc: u32
- tb: u64 time base; gqr[0..7]: u32; sprg[0..3]: u32
- reserve: bool, reserve_addr: u32
- mem: Memory (sparse, big-endian); every access is appended to mem.log in order as
  (kind, address, width, value) with kind in {'r','w'} so ordering can be compared.
- pending: dict of asynchronous conditions, e.g. {'external': bool, 'decrementer': bool}
- exception: None, or the name of the exception taken by the last step
  ('program', 'fp_unavailable', 'dsi', 'isi', 'alignment', 'syscall', 'decrementer',
   'external', 'trap', ...), with srr0/srr1/msr/pc updated per the manual.

A spec module exposes:
    AREA: str
    def supports(word: int) -> bool
    def step(state: State, word: int) -> None     # executes the instruction at state.pc
"""
from __future__ import annotations

import copy
import struct

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF


class Memory:
    def __init__(self):
        self.bytes = {}
        self.log = []

    def load(self, address: int, data: bytes) -> None:
        for i, b in enumerate(data):
            self.bytes[(address + i) & MASK32] = b

    def _get(self, address: int, width: int) -> int:
        v = 0
        for i in range(width):
            v = (v << 8) | self.bytes.get((address + i) & MASK32, 0)
        return v

    def read(self, address: int, width: int) -> int:
        v = self._get(address, width)
        self.log.append(("r", address & MASK32, width, v))
        return v

    def write(self, address: int, width: int, value: int) -> None:
        value &= (1 << (8 * width)) - 1
        for i in range(width):
            self.bytes[(address + i) & MASK32] = (value >> (8 * (width - 1 - i))) & 0xFF
        self.log.append(("w", address & MASK32, width, value))


class State:
    def __init__(self):
        self.gpr = [0] * 32
        self.fpr = [[0, 0] for _ in range(32)]
        self.cr = 0
        self.xer = 0
        self.lr = 0
        self.ctr = 0
        self.msr = 0x00002032  # FP|IR|DR|RI - a typical game-mode MSR; tests set what they need
        self.fpscr = 0
        self.srr0 = 0
        self.srr1 = 0
        self.dar = 0
        self.dsisr = 0
        self.dec = 0
        self.tb = 0
        self.hid0 = 0
        self.hid2 = 0xE0000000
        self.gqr = [0] * 8
        self.sprg = [0] * 4
        self.pc = 0x80003100
        self.reserve = False
        self.reserve_addr = 0
        self.mem = Memory()
        self.pending = {"external": False, "decrementer": False}
        self.exception = None

    def clone(self) -> "State":
        return copy.deepcopy(self)

    def arch_dict(self) -> dict:
        return {k: getattr(self, k) for k in (
            "gpr", "fpr", "cr", "xer", "lr", "ctr", "msr", "fpscr", "srr0", "srr1", "dar", "dsisr",
            "dec", "tb", "hid0", "hid2", "gqr", "sprg", "pc", "reserve", "reserve_addr", "exception")}


def bits_to_double(bits: int) -> float:
    return struct.unpack(">d", struct.pack(">Q", bits & MASK64))[0]


def double_to_bits(value: float) -> int:
    return struct.unpack(">Q", struct.pack(">d", value))[0]
