"""F1b differential-harness vector generator: exhaustive practical coverage.

Superset of gen_vectors.py (F1a): every F1a family (new seed), plus
  fp      every scalar floating-point class (A-forms double and single, X-forms, FPSCR moves, loads
          and stores), special/denormal/NaN/SNaN/tie operands, RN/NI/enables/FE0-FE1 modes, frD
          aliasing, Rc, HID2[PSE]
  ps      every paired-single class, psq loads/stores with every GQR type and edge scales, W,
          update and rA=0 forms, aliasing, Rc
  gate    MSR[FP]=0 on every FP and Gekko-extension class, HID2[LCE] gating of dcbz_l, mid-block and
          cold-entry faults (the fault helper names the faulting address; the pending retirement
          literal it carried went with the instruction clock), and (M1) the same faults with MSR[IP] = 0,
          which end at the low 0x800 vector. HID2 LSQE|PSE are a machine invariant (change C, 2026-10-05: Cpu::write_hid2
          refuses a write that would clear either), so every HID2 here keeps them
  sys     sc (stops at both vector bases), rfi (MSR variants, EE 0->1 with a pending line), tw/twi (all TO
          masks x operand relations), mtmsr/mfmsr, the mtspr/mfspr SPR matrix (incl. invalid SPR
          numbers, Rc), mftb with any TBR field, MSR[PR]=1 privileged instructions, dcbz_l, and the
          classes no spec module models (isync, mtsr/mfsr/mtsrin/mfsrin, cache/TLB hints, tlbia,
          stfiwx, eciwx/ecowx: engine-only)
  bnd     block-boundary sequences: CR producer / consumer across labels and calls, XER[CA] across a
          call, the FP-guard and HID2 hoists across labels, calls, bcctrl, mtmsr and mtspr 920,
          update-form store/load across a branch, EndOfFunction fall-through, bctr default paths
          (another row's entry, a non-entry address), resume_elsewhere, every interior entry
  irq     interrupt recognition under native time (docs/native-time-20261005/DESIGN.md) for each base
          sequence (straight run, back edge, CTR loop, call and bclr return, bclrl/bcctrl, backward and
          forward bctr, mtmsr EE on/off, rfi EE 0->1, cross-record back edge, the RAM-poll idle loop,
          mfspr DEC, sc, trap, FP fault): the external line asserted before entry - deliverable at entry,
          so it is taken at the first recognition point (R1 at entry with EE = 1; the guest's own rfi
          with EE 0->1) - stop style at both vector bases, and latched and never delivered with EE = 0.
          RETIRED 2026-10-06 (native time): the external line raised at retired count k > 0 (`irq_ext k`),
          the decrementer edge placed at retired count k by DEC and the timer residue (`dec_edge_at`),
          their EE = 0 variants and the decrementer round trips. Each expressed an instruction-count
          position that no longer exists: native code has no instruction count, guest code runs in zero
          game time (DEC and TB never move inside a vector) and a line changes only at a wait point, where
          the scheduler delivers events - none of which a difftest vector contains. The drivers refuse
          `irq_ext k > 0` (status irq-at-retired-count) and the decrementer edge can no longer fall inside
          a vector, so these could only report "not run" or degenerate into duplicates of the k = 0
          vectors. The external round trips (k drawn 0..5) became k = 0 round trips, the one form native
          time can express. Every retired vector's id stays reserved (`retire_id`) and every RNG draw and
          row is kept, so every surviving vector keeps its id, state, rows and addresses. The 0x900
          handler code the decrementer round trips executed is entered directly instead (fam_tail).

M1 (docs/memory-architecture-20261006): there is no real mode and no exception-vector code. The CPU
performs the architected exception entry (SRR0, SRR1, the exception MSR) and then the OS's exception entry,
which these machines never install, so every exception ends its vector at the architected boundary: every
vector offset is a stop at both bases (MSR[IP] = 1: 0xFFF00xxx, MSR[IP] = 0: 0x00000xxx). The real-mode
handler rows that used to sit at text1 (0x00000500) and the round trips through them are gone: the IP = 0
variants are stop-style vectors at the low base (shape *-stop-ip0, same ids); the two vectors that entered
the 0x900 handler in real mode and the bnd sc round trip through the helper rows are retired (ids reserved,
RNG stream kept).

Outputs exactly like gen_vectors.py, into --out (deleted and recreated).
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import random
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen_vectors as GV     # noqa: E402
import ppcenc as E           # noqa: E402
import ppcenc_ext as X       # noqa: E402
import emitter_classes as EC  # noqa: E402

# /3 (native time, 2026-10-06): no `tick` (the hardware service interval, deleted), no `dec_edge_at` (a
# decrementer edge at a retired count), `irq_ext` only 0 (asserted before entry), the event-spin shape (fam_tail).
SCHEMA = "gekko-difftest-vector/3"
SENTINEL = GV.SENTINEL
M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF
MSR = 0x00003032
IP, EE, PR, FP = 0x40, 0x8000, 0x4000, 0x2000
FE0, FE1 = 0x800, 0x100
HID2 = 0xE0000000
PSE, LSQE, LCE, WPE = 0x20000000, 0x80000000, 0x10000000, 0x40000000
DATA_BASE = GV.DATA_BASE
VEC_OFFSETS = (0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800, 0x900, 0xC00, 0xD00, 0xF00,
               0x1300, 0x1400, 0x1700)
HIGH_STOPS = [0xFFF00000 | o for o in VEC_OFFSETS]
LOW_STOPS = list(VEC_OFFSETS)
PRE = E.addi(23, 23, 1)
# The event-spin vectors (fam_tail): compare_f1b.py's event_spin() accepts the bound statuses for this shape only.
EVENT_SPIN_SHAPE = "event-spin/ram-poll"
EVENT_SPIN_STEPS = 201  # 67 whole iterations of the 3-instruction loop: the step bound ends at the loop head


def dbits(x: float) -> int:
    return struct.unpack(">Q", struct.pack(">d", x))[0]


# ---------------------------------------------------------------------------------------------
# floating-point value pools (double images)
# ---------------------------------------------------------------------------------------------
SPECIAL_D = [
    0x0, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000,
    0xFFF8000000000000, 0x7FF8000000000001, 0x7FFC000012345678, 0xFFFC000000000001, 0x7FF0000000000001,
    0x7FF4000000000000, 0xFFF0000000000001, 0x7FF7FFFFFFFFFFFF, 0x0000000000000001, 0x000FFFFFFFFFFFFF,
    0x8000000000000001, 0x0008000000000000, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF,
    0x3FF0000000000000, 0xBFF0000000000000, 0x3FE0000000000000, 0x4000000000000000, 0x4008000000000000,
    0x3FB999999999999A, 0x3FD5555555555555, 0x41DFFFFFFFC00000, 0x41DFFFFFFFE00000, 0x41E0000000000000,
    0xC1E0000000000000, 0xC1E0000000100000, 0xC1E0000000200000, 0x4004000000000000, 0xC004000000000000,
    0x3FF8000000000000, 0x4002000000000000, 0x47EFFFFFE0000000, 0x47EFFFFFF0000000, 0x3810000000000000,
    0x3800000000000000, 0x36A0000000000000, 0x3690000000000000, 0x380FFFFFE0000000, 0x0010000000000001,
    0x3FF0000010000000, 0x3FF0000030000000, 0x41E0000000100000, 0xC1DFFFFFFFE00000, 0x3FDFFFFFFFFFFFFF,
    0x0020000000000000, 0x001FFFFFFFFFFFFF, 0x4330000000000000, 0x7FE0000000000000,
]
SPECIAL_S = [
    0x0, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000,
    0x7FFC000000000000, 0x7FF8000020000000, 0x7FF0000020000000, 0x7FF4000000000000, 0xFFF0000020000000,
    0x3FF0000000000000, 0xBFF0000000000000, 0x3FE0000000000000, 0x4000000000000000, 0x4008000000000000,
    0x3FF8000000000000, 0x4004000000000000, 0xC004000000000000, 0x4002000000000000, 0x47EFFFFFE0000000,
    0xC7EFFFFFE0000000, 0x3810000000000000, 0x3800000000000000, 0x36A0000000000000, 0xB6A0000000000000,
    0x380FFFFFE0000000, 0x406FE00000000000, 0x4070000000000000, 0x40EFFFE000000000, 0x40DFFFC000000000,
    0xC060000000000000, 0x405FE00000000000, 0x40E0000000000000, 0x3F80000000000000, 0x3FF0000020000000,
    0x415FFFFFC0000000, 0x41DFFFFFC0000000, 0xC1E0000000000000, 0x3FC5555560000000,
]


class GenB(GV.Gen):
    def __init__(self, seed, scale):
        super().__init__(seed, scale)

    # ---- layout ------------------------------------------------------------------------------

    def add(self, family, rows, entry, state, words_note, mem=None, spec="compare", reason="",
            max_steps=4096, parent=None, prefix_k=None, mnemonic=None, expect_mnemonic=None, **extra):
        v = super().add(family, rows, entry, state, words_note, mem=mem, spec=spec, reason=reason,
                        max_steps=max_steps, parent=parent, prefix_k=prefix_k, mnemonic=mnemonic,
                        expect_mnemonic=expect_mnemonic)
        v["schema"] = SCHEMA
        msr = state["msr"]
        v["irq_ext"] = extra.get("irq_ext")
        # Native time: a line can only be asserted before entry (see the module docstring).
        assert v["irq_ext"] in (None, 0), (v["id"], v["irq_ext"])
        v["policy"] = extra.get("policy", "project")
        v["shape"] = extra.get("shape")
        # every vector may change MSR[IP] (rfi, mtmsr): every exception ends at its architected vector, at
        # either base (M1: no vector code exists)
        v["stops"] = extra.get("stops") or (HIGH_STOPS + LOW_STOPS)
        state["spr"] = {int(k): val for k, val in state["spr"].items()}
        state["sr"] = {int(k): val for k, val in (state.get("sr") or {}).items()}
        unknown = set(extra) - {"irq_ext", "policy", "shape", "stops"}
        assert not unknown, (v["id"], sorted(unknown))  # tick / dec_edge_at are retired (native time)
        return v

    def retire_id(self, family):
        """A retired vector (native time, see the module docstring): its id stays reserved so every later
        vector of the family keeps its id (the known-candidate fingerprints are exact per vector id)."""
        self.new_id(family)

    def base_state(self):
        st = super().base_state()
        st["sr"] = {}
        return st

    # ---- value helpers -----------------------------------------------------------------------
    def rsingle(self):
        rng = self.rng
        sign = -1.0 if rng.random() < 0.5 else 1.0
        r = rng.random()
        if r < 0.1:      # single denormal
            return dbits(sign * rng.randrange(1, 1 << 23) * 2.0 ** -149)
        if r < 0.3:      # small integers and halves
            return dbits(sign * rng.randrange(0, 70000) / rng.choice((1, 2, 4)))
        e = rng.randrange(-126, 128) if r < 0.6 else rng.randrange(-20, 32)
        return dbits(sign * ((1 << 23) + rng.getrandbits(23)) * 2.0 ** (e - 23))

    def rdouble(self):
        rng = self.rng
        r = rng.random()
        if r < 0.4:
            return rng.getrandbits(64)
        if r < 0.7:
            sign = rng.getrandbits(1) << 63
            return sign | (rng.randrange(1023 - 60, 1023 + 60) << 52) | rng.getrandbits(52)
        return self.rsingle()

    def fval(self, single):
        rng = self.rng
        r = rng.random()
        if r < 0.35:
            return rng.choice(SPECIAL_S if single else SPECIAL_D)
        if r < 0.45 and not single:
            return rng.choice(SPECIAL_S)
        return self.rsingle() if single else self.rdouble()

    def fpscr(self):
        rng = self.rng
        f = 0
        if rng.random() < 0.3:
            f |= rng.getrandbits(32) & 0x9FF80700        # sticky exception bits (not FEX/VX/b20)
        if rng.random() < 0.3:
            f |= rng.getrandbits(32) & 0x0001F000        # FPRF
        if rng.random() < 0.2:
            f |= rng.getrandbits(32) & 0x00060000        # FR FI
        if rng.random() < 0.25:
            f |= rng.getrandbits(32) & 0x000000F8        # VE OE UE ZE XE
        if rng.random() < 0.45:
            f |= 0x4                                     # NI
        if rng.random() < 0.4:
            f |= rng.randrange(1, 4)                     # RN
        if f & 0x01F80700:
            f |= 0x20000000
        if (((f >> 22) & f) & 0xF8) != 0:
            f |= 0x40000000
        return f & M32

    def fp_state(self, single, ip=True, fe=False):
        st = self.base_state()
        rng = self.rng
        st["fpr"] = [[self.fval(single), self.fval(True) if rng.random() < 0.7 else self.fval(single)]
                     for _ in range(32)]
        st["fpscr"] = self.fpscr()
        st["msr"] = MSR | (IP if ip else 0) | ((rng.choice((FE0, FE1, FE0 | FE1))) if fe else 0)
        st["dec"] = 0x7FFFFFFF
        if rng.random() < 0.1:
            st["spr"][920] = HID2 & ~WPE  # a non-default HID2; LSQE|PSE are a machine invariant
        return st

    def ps_state(self, ip=True):
        st = self.fp_state(True, ip)
        st["fpr"] = [[self.fval(True), self.fval(True)] for _ in range(32)]
        st["spr"].pop(920, None)
        return st

    def fregs(self, n, pattern):
        """n distinct FPR indices, then apply an aliasing pattern (list of index pairs to equate)."""
        rng = self.rng
        regs = rng.sample(range(32), n)
        for i, j in pattern:
            regs[i] = regs[j]
        return regs

    def one(self, family, word, st, name, mem=None, spec="compare", reason="", expect=True, **extra):
        row = self.row_pic([word, E.BLR], f"{family}_{name}")
        return self.add(family, [row], row, st, f"{word:08X}", mem=mem, spec=spec, reason=reason, mnemonic=name,
                        expect_mnemonic=name if expect else None, **extra)

    # =========================================================================================
    # fp
    # =========================================================================================
    ALIAS3 = ([], [(0, 1)], [(0, 2)], [(0, 3)], [(1, 2)], [(0, 1), (2, 1), (3, 1)], [(2, 3)])

    def fam_fp(self, per_class):
        rng = self.rng
        a63 = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte",
               28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
        a59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls", 28: "fmsubs",
               29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}
        for op, table in ((63, a63), (59, a59)):
            single = op == 59
            for xo, name in table.items():
                for i in range(per_class):
                    d, a, b, c = self.fregs(4, self.ALIAS3[i % len(self.ALIAS3)])
                    if xo in (22, 24, 26):
                        a, c = 0, 0
                    elif xo == 25:
                        b = 0
                    elif xo in (18, 20, 21):
                        c = 0
                    word = X.a_form(op, d, a, b, c, xo, rng.randrange(2))
                    st = self.fp_state(single, ip=rng.random() < 0.8, fe=rng.random() < 0.08)
                    self.shape_fp(st, name, a, b, c)
                    self.one("fp", word, st, name)
        # X-forms
        for i in range(per_class):
            rc = rng.randrange(2)
            cases = [
                (X.fcmp(63, 0, rng.randrange(8), *rng.sample(range(32), 2)), "fcmpu", True),
                (X.fcmp(63, 32, rng.randrange(8), *rng.sample(range(32), 2)), "fcmpo", True),
                (E.x_form(63, rng.randrange(32), 0, rng.randrange(32), 12, rc), "frsp", True),
                (E.x_form(63, rng.randrange(32), 0, rng.randrange(32), 14, rc), "fctiw", False),
                (E.x_form(63, rng.randrange(32), 0, rng.randrange(32), 15, rc), "fctiwz", False),
                (E.x_form(63, rng.randrange(32), 0, rng.randrange(32), 40, rc), "fneg", False),
                (E.x_form(63, rng.randrange(32), 0, rng.randrange(32), 72, rc), "fmr", False),
                (E.x_form(63, rng.randrange(32), 0, rng.randrange(32), 136, rc), "fnabs", False),
                (E.x_form(63, rng.randrange(32), 0, rng.randrange(32), 264, rc), "fabs", False),
                (E.x_form(63, rng.randrange(32), 0, 0, 583, rc), "mffs", False),
                (X.mcrfs(rng.randrange(8), rng.randrange(8)), "mcrfs", False),
                (X.mtfsb(rng.randrange(32), True, rc), "mtfsb1", False),
                (X.mtfsb(rng.randrange(32), False, rc), "mtfsb0", False),
                (X.mtfsfi(rng.randrange(8), rng.randrange(16), rc), "mtfsfi", False),
                (X.mtfsf(rng.choice((0xFF, 0x01, 0x80, 0x7F, rng.randrange(256))), rng.randrange(32), rc), "mtfsf",
                 False),
            ]
            for word, name, single in cases:
                st = self.fp_state(single and rng.random() < 0.5, ip=rng.random() < 0.8, fe=rng.random() < 0.06)
                if name in ("fctiw", "fctiwz"):
                    b = (word >> 11) & 31
                    st["fpr"][b][0] = rng.choice((0x41DFFFFFFFC00000, 0x41DFFFFFFFE00000, 0x41E0000000000000,
                                                  0xC1E0000000000000, 0xC1E0000000100000, 0xC1E0000000200000,
                                                  0x4004000000000000, 0x3FE0000000000000, 0x3FF8000000000000,
                                                  0xBFE0000000000000, 0x41DFFFFFFFD00000, self.fval(False)))
                if name == "mtfsf" and rng.random() < 0.5:
                    b = (word >> 11) & 31
                    st["fpr"][b][0] = rng.getrandbits(64) & ~0x800 if rng.random() < 0.7 else rng.getrandbits(64)
                if name in ("mtfsb1", "mtfsb0") and ((word >> 21) & 31) == 20:
                    pass
                if name in ("fcmpu", "fcmpo") and rng.random() < 0.3:
                    a_, b_ = (word >> 16) & 31, (word >> 11) & 31
                    st["fpr"][b_][0] = st["fpr"][a_][0]
                self.one("fp", word, st, name)
        self.fam_fp_loadstore(per_class)

    def shape_fp(self, st, name, a, b, c):
        """Bias operand pairs toward the exceptional cases the pools rarely combine."""
        rng = self.rng
        r = rng.random()
        fpr = st["fpr"]
        if r < 0.08:
            fpr[b][0] = fpr[a][0]                       # x - x (exact zero sign under RN)
        elif r < 0.14:
            fpr[b][0] = fpr[a][0] ^ 0x8000000000000000  # x + -x
        elif r < 0.2:
            fpr[a][0], fpr[c][0] = 0x0, 0x7FF0000000000000   # 0 * inf
        elif r < 0.25:
            fpr[b][0] = 0x0                              # division by zero
        elif r < 0.3 and name.startswith(("fmadd", "fmsub", "fnmadd", "fnmsub")):
            fpr[a][0], fpr[c][0] = 0x7FF0000000000000, 0x0
            fpr[b][0] = rng.choice((0x7FF8000000000000, 0x7FF0000000000001))
        elif r < 0.35:
            fpr[a][0] = 0x8000000000000000 | fpr[a][0]  # negative operand (sqrt, fsel)

    def fam_fp_loadstore(self, per_class):
        rng = self.rng
        d_forms = ((48, "lfs", 4, "r"), (49, "lfsu", 4, "r"), (50, "lfd", 8, "r"), (51, "lfdu", 8, "r"),
                   (52, "stfs", 4, "w"), (53, "stfsu", 4, "w"), (54, "stfd", 8, "w"), (55, "stfdu", 8, "w"))
        x_forms = ((535, "lfsx", 4, "r", False), (567, "lfsux", 4, "r", True), (599, "lfdx", 8, "r", False),
                   (631, "lfdux", 8, "r", True), (663, "stfsx", 4, "w", False), (695, "stfsux", 4, "w", True),
                   (727, "stfdx", 8, "w", False), (759, "stfdux", 8, "w", True), (983, "stfiwx", 4, "w", False))
        for i in range(per_class * 2):
            for op, name, width, rw in d_forms:
                st = self.fp_state(False, ip=rng.random() < 0.8)
                d = rng.randrange(32)
                a = self.reg(avoid=(0,))
                align = 4 if rng.random() < 0.85 else 1
                ea = self.ea_in_window(width, align)
                disp = self.imm16() & ~(3 if align == 4 else 0)
                st["gpr"][a] = (ea - disp) & M32
                mem = self.mem_around(ea, width)
                if rw == "r":
                    blob = bytearray(bytes.fromhex(mem[0][1]))
                    off = ea - mem[0][0]
                    if width == 4:
                        val = rng.choice((0x7F800001, 0x7FC00000, 0x80000001, 0x00000001, 0x7F7FFFFF, 0x80000000,
                                          0x3FC00000, 0xFF800000, rng.getrandbits(32)))
                        blob[off:off + 4] = val.to_bytes(4, "big")
                    else:
                        blob[off:off + 8] = self.fval(False).to_bytes(8, "big")
                    mem = [[mem[0][0], blob.hex()]]
                else:
                    st["fpr"][d][0] = self.fval(rng.random() < 0.4)
                    if rng.random() < 0.3:
                        st["fpr"][d][0] = rng.choice((0x3690000000000000, 0x36A8000000000000, 0x0000000000000001,
                                                      0x380FFFFFF0000000, 0x47EFFFFFF0000000, 0x7FF0000000000001))
                word = E.d_form(op, d, a, disp)
                self.one("fp", word, st, name, mem=mem)
            for xo, name, width, rw, update in x_forms:
                st = self.fp_state(False, ip=rng.random() < 0.8)
                d = rng.randrange(32)
                a = self.reg(avoid=(0,) if update else ())
                b = self.reg()
                if rng.random() < 0.2:
                    b = a
                align = 4 if rng.random() < 0.85 else 1
                ea = self.ea_in_window(width, max(align, 2 if (b == a and a != 0) else 1))
                if b == a and a != 0:
                    ea &= ~(4 - 1) if align == 4 else ~1
                    st["gpr"][a] = ea // 2
                elif a == 0:
                    st["gpr"][b] = ea
                else:
                    st["gpr"][a] = rng.getrandbits(32)
                    st["gpr"][b] = (ea - st["gpr"][a]) & M32
                mem = self.mem_around(ea, width)
                if rw == "w":
                    st["fpr"][d][0] = self.fval(rng.random() < 0.4)
                word = E.x_form(31, d, a, b, xo)
                spec, reason = ("engine-only", "no spec module models stfiwx (optional instruction, PAGE 99)") \
                    if name == "stfiwx" else ("compare", "")
                self.one("fp", word, st, name, mem=mem, spec=spec, reason=reason, expect=spec == "compare")

    # =========================================================================================
    # ps
    # =========================================================================================
    def fam_ps(self, per_class):
        rng = self.rng
        a4 = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0", 15: "ps_madds1",
              18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul", 26: "ps_rsqrte",
              28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd"}
        x4 = {40: "ps_neg", 72: "ps_mr", 136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01",
              592: "ps_merge10", 624: "ps_merge11"}
        for xo, name in a4.items():
            for i in range(per_class):
                d, a, b, c = self.fregs(4, self.ALIAS3[i % len(self.ALIAS3)])
                if xo in (24, 26):
                    a, c = 0, 0
                elif xo in (12, 13, 25):
                    b = 0
                elif xo in (18, 20, 21):
                    c = 0
                word = X.a_form(4, d, a, b, c, xo, rng.randrange(2))
                st = self.ps_state(ip=rng.random() < 0.8)
                if rng.random() < 0.06:
                    st["msr"] |= rng.choice((FE0, FE1))
                self.shape_ps(st, a, b, c)
                self.one("ps", word, st, name)
        for xo, name in x4.items():
            for i in range(per_class):
                d, a, b = self.fregs(3, ([], [(0, 1)], [(0, 2)], [(1, 2)], [(0, 1), (2, 1)])[i % 5])
                if xo in (40, 72, 136, 264):
                    a = 0
                word = E.x_form(4, d, a, b, xo, rng.randrange(2) if i % 7 == 0 else 0)
                st = self.ps_state(ip=rng.random() < 0.8)
                spec = "compare"
                self.one("ps", word, st, name, spec=spec)
        for i in range(per_class):
            for xo, name in ((0, "ps_cmpu0"), (32, "ps_cmpo0"), (64, "ps_cmpu1"), (96, "ps_cmpo1")):
                a, b = rng.sample(range(32), 2)
                if i % 5 == 0:
                    b = a
                word = X.fcmp(4, xo, rng.randrange(8), a, b)
                st = self.ps_state(ip=rng.random() < 0.8)
                if rng.random() < 0.3:
                    st["fpr"][b] = list(st["fpr"][a])
                self.one("ps", word, st, name)
        self.fam_psq(per_class * 3)

    def shape_ps(self, st, a, b, c):
        rng = self.rng
        r = rng.random()
        fpr = st["fpr"]
        if r < 0.1:
            fpr[b] = list(fpr[a])
        elif r < 0.18:
            fpr[a] = [0x0, 0x7FF0000000000000]
            fpr[c] = [0x7FF0000000000000, 0x0]
        elif r < 0.24:
            fpr[b] = [0x0, 0x8000000000000000]
        elif r < 0.3:
            fpr[a] = [fpr[a][0] | 0x8000000000000000, fpr[a][1]]

    GQR_SCALES = (0, 1, 2, 7, 8, 15, 16, 31, 0x3F, 0x38, 0x30, 0x21, 0x20)

    def fam_psq(self, count):
        rng = self.rng
        for i in range(count):
            kind = rng.choice(("psq_l", "psq_lu", "psq_st", "psq_stu", "psq_lx", "psq_lux", "psq_stx", "psq_stux"))
            store = "st" in kind
            update = kind.endswith("u") or kind.endswith("ux")
            indexed = kind.endswith("x")
            st = self.ps_state(ip=rng.random() < 0.85)
            gi = rng.randrange(8)
            typ = rng.choice((0, 4, 5, 6, 7, 0, 4, 5, 6, 7, 1, 2, 3)) if rng.random() < 0.97 else 0
            scale = rng.choice(self.GQR_SCALES) if rng.random() < 0.7 else rng.randrange(64)
            other_t, other_s = rng.choice((0, 4, 5, 6, 7)), rng.randrange(64)
            gqr = ((scale << 24) | (typ << 16) | (other_s << 8) | other_t) if not store else \
                ((other_s << 24) | (other_t << 16) | (scale << 8) | typ)
            st["spr"][912 + gi] = gqr
            w = rng.randrange(2)
            d = rng.randrange(32)
            width = {0: 4, 1: 4, 2: 4, 3: 4, 4: 1, 6: 1, 5: 2, 7: 2}[typ] * (1 if w else 2)
            align = 4 if (typ < 4 and rng.random() < 0.85) else rng.choice((1, 2, 4))
            ea = self.ea_in_window(width + 8, align)
            if indexed:
                a = self.reg(avoid=(0,)) if update else self.reg()
                b = self.reg()
                if rng.random() < 0.2 and a != 0:
                    b = a
                    ea &= ~1
                    st["gpr"][a] = ea // 2
                elif a == 0:
                    st["gpr"][b] = ea
                else:
                    st["gpr"][a] = rng.getrandbits(32)
                    st["gpr"][b] = (ea - st["gpr"][a]) & M32
                xo6 = {"psq_lx": 6, "psq_stx": 7, "psq_lux": 38, "psq_stux": 39}[kind]
                word = X.psq_x(xo6, d, a, b, w, gi)
            else:
                a = self.reg(avoid=(0,))
                off = rng.randrange(-0x800, 0x800)
                st["gpr"][a] = (ea - off) & M32
                op = {"psq_l": 56, "psq_lu": 57, "psq_st": 60, "psq_stu": 61}[kind]
                word = X.psq_d(op, d, a, w, gi, off)
            mem = self.mem_around(ea, width + 4)
            if not store:
                blob = bytearray(bytes.fromhex(mem[0][1]))
                o = ea - mem[0][0]
                if typ >= 4:
                    for j in range(width):
                        blob[o + j] = rng.choice((0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, rng.getrandbits(8)))
                else:
                    for j in range(0, width, 4):
                        blob[o + j:o + j + 4] = rng.choice((0x7F800001, 0x7FC00000, 0x00000001, 0x80000000, 0x3FC00000,
                                                            0x437F0000, rng.getrandbits(32))).to_bytes(4, "big")
                mem = [[mem[0][0], blob.hex()]]
            else:
                pool = (0x0, 0x8000000000000000, dbits(1.5), dbits(-1.5), dbits(255.0), dbits(255.5), dbits(256.0),
                        dbits(-128.0), dbits(-128.5), dbits(127.5), dbits(32767.5), dbits(65535.0), dbits(65536.0),
                        dbits(-32768.5), 0x7FF8000000000000, 0x7FF0000000000000, 0xFFF0000000000000,
                        0x36A0000000000000, 0xFFF8000000000000, dbits(0.75), dbits(-0.75), dbits(3.999))
                st["fpr"][d] = [rng.choice(pool) if rng.random() < 0.7 else self.rsingle(),
                                rng.choice(pool) if rng.random() < 0.7 else self.rsingle()]
            self.one("ps", word, st, kind, mem=mem)
        # dcbz_l with LCE set and clear
        for i in range(max(4, count // 10)):
            st = self.ps_state(ip=True)
            if rng.random() < 0.7:
                st["spr"][920] = HID2 | LCE
            a, b = self.reg(), self.reg()
            ea = self.ea_in_window(40)
            if a == 0:
                st["gpr"][b] = ea
            elif b == a:
                ea &= ~1
                st["gpr"][a] = ea // 2
            else:
                st["gpr"][a] = rng.getrandbits(32)
                st["gpr"][b] = (ea - st["gpr"][a]) & M32
            self.one("ps", E.x_form(4, 0, a, b, 1014), st, "dcbz_l", mem=self.mem_around(ea & ~31, 32, 16, 16))

    # =========================================================================================
    # fpseq: straight FP / paired-single runs (hot-body FP-guard and HID2 hoists), prefixes, entries
    # =========================================================================================
    def fam_fpseq(self, count):
        rng = self.rng
        for i in range(count):
            n = rng.randrange(2, 9)
            words, names = [], []
            live = rng.sample(range(1, 32), 6)
            for j in range(n):
                d, a, b, c = rng.choice(live), rng.choice(live), rng.choice(live), rng.choice(live)
                off = 4 * rng.randrange(0, 8)
                kind = rng.randrange(20)
                if kind == 0:
                    w, nm = X.a_form(63, d, a, b, 0, rng.choice((18, 20, 21))), "fdiv/fsub/fadd"
                elif kind == 1:
                    w, nm = X.a_form(59, d, a, b, 0, rng.choice((18, 20, 21))), "fdivs/fsubs/fadds"
                elif kind == 2:
                    w, nm = X.a_form(rng.choice((59, 63)), d, a, 0, c, 25), "fmul(s)"
                elif kind == 3:
                    w, nm = X.a_form(rng.choice((59, 63)), d, a, b, c, rng.choice((28, 29, 30, 31))), "fmadd-family"
                elif kind == 4:
                    w, nm = E.x_form(63, d, 0, b, rng.choice((40, 72, 136, 264, 12))), "fmove/frsp"
                elif kind == 5:
                    w, nm = X.fcmp(63, rng.choice((0, 32)), rng.randrange(8), a, b), "fcmp"
                elif kind == 6:
                    w, nm = X.a_form(4, d, a, b, 0, rng.choice((18, 20, 21))), "ps_arith"
                elif kind == 7:
                    w, nm = X.a_form(4, d, a, b, c, rng.choice((14, 15, 29, 28))), "ps_madd-family"
                elif kind == 8:
                    w, nm = E.x_form(4, d, a, b, rng.choice((528, 560, 592, 624))), "ps_merge"
                elif kind == 9:
                    w, nm = E.x_form(4, d, 0, b, rng.choice((40, 72, 136, 264))), "ps_move"
                elif kind == 10:
                    w, nm = E.d_form(rng.choice((48, 50)), d, 3, off), "lfs/lfd"
                elif kind == 11:
                    w, nm = E.d_form(rng.choice((52, 54)), d, 3, off), "stfs/stfd"
                elif kind == 12:
                    w, nm = X.psq_d(rng.choice((56, 60)), d, 3, rng.randrange(2), 0, off), "psq_l/psq_st"
                elif kind == 13:
                    w, nm = X.mtfsb(29, rng.random() < 0.5), "mtfsb NI"
                elif kind == 14:
                    w, nm = X.mtfsfi(7, rng.randrange(4)), "mtfsfi RN"
                elif kind == 15:
                    w, nm = E.x_form(63, d, 0, b, 15), "fctiwz"
                elif kind == 16:
                    w, nm = E.mtspr(9, 920), "mtspr HID2"
                elif kind == 17:
                    w, nm = X.mtmsr(8), "mtmsr"
                else:
                    w, nm = E.addi(10, 10, j + 1), "addi"
                words.append(w)
                names.append(nm)
            st = self.ps_state(ip=rng.random() < 0.7)
            st["fpscr"] = st["fpscr"] & ~0x7 if rng.random() < 0.5 else st["fpscr"]
            ea = self.ea_in_window(48, 4)
            st["gpr"][3] = ea
            st["gpr"][9] = rng.choice((HID2, HID2 & ~WPE, HID2 | LCE, (HID2 | LCE) & ~WPE))
            st["gpr"][8] = (st["msr"] & ~FP) if rng.random() < 0.6 else st["msr"]
            if rng.random() < 0.15:
                st["msr"] &= ~FP
            blob = bytearray()
            for j in range(12):
                blob += rng.choice((0x3FC00000, 0xC1200000, 0x7F800001, 0x00000001, 0x80000000, 0x437F0000,
                                    rng.getrandbits(32))).to_bytes(4, "big")
            mem = [[ea, blob.hex()]]
            self.with_prefixes_and_entries("fpseq", words, st, mem, "compare", "", names)

    # =========================================================================================
    # gate: MSR[FP] / HID2 gating, faults mid-block and at cold entries, lazy-FPU round trip
    # =========================================================================================
    def fp_class_words(self):
        """One representative word per FP / Gekko-extension class (all require MSR[FP] or HID2)."""
        rng = self.rng
        words = []

        def valid_a(op, xo):
            # reserved operand fields zero, so the spec model compares the fault instead of calling the
            # word boundedly undefined
            d, a, b, c = 1, 2, 3, 4
            if xo in (18, 20, 21):
                c = 0
            elif xo in (22, 24, 26):
                a, c = 0, 0
            elif xo in (25, 12, 13):
                b = 0
            return X.a_form(op, d, a, b, c, xo)
        for xo in (18, 20, 21, 22, 23, 25, 26, 28, 29, 30, 31):
            words.append(valid_a(63, xo))
        for xo in (18, 20, 21, 22, 24, 25, 28, 29, 30, 31):
            words.append(valid_a(59, xo))
        for xo in (12, 14, 15, 40, 72, 136, 264):
            words.append(E.x_form(63, 5, 0, 6, xo))
        words.append(E.x_form(63, 5, 0, 0, 583))
        words += [X.fcmp(63, 0, 2, 1, 2), X.fcmp(63, 32, 3, 1, 2), X.mcrfs(1, 2), X.mtfsb(29, True),
                  X.mtfsb(31, False), X.mtfsfi(7, 1), X.mtfsf(0xFF, 7)]
        words += [E.d_form(op, 1, 3, 0) for op in range(48, 56)]
        words += [E.x_form(31, 1, 3, 4, xo) for xo in (535, 567, 599, 631, 663, 695, 727, 759, 983)]
        words += [valid_a(4, xo) for xo in (10, 11, 12, 13, 14, 15, 18, 20, 21, 23, 24, 25, 26, 28, 29, 30, 31)]
        words += [E.x_form(4, 1, 0, 3, xo) for xo in (40, 72, 136, 264)]
        words += [E.x_form(4, 1, 2, 3, xo) for xo in (528, 560, 592, 624)]
        words += [X.fcmp(4, xo, 1, 2, 3) for xo in (0, 32, 64, 96)]
        words += [X.psq_d(op, 1, 3, 0, 0, 0) for op in (56, 57, 60, 61)]
        words += [X.psq_x(xo, 1, 3, 4, 0, 0) for xo in (6, 7, 38, 39)]
        words.append(E.x_form(4, 0, 3, 4, 1014))
        return words

    def gate_state(self, word, ip):
        st = self.fp_state(True, ip=ip)
        st["fpr"] = [[self.rsingle(), self.rsingle()] for _ in range(32)]
        ea = DATA_BASE + 0x1000
        st["gpr"][3] = ea
        st["gpr"][4] = 0
        return st, self.mem_around(ea, 16)

    def fam_gate(self):
        rng = self.rng
        words = self.fp_class_words()
        for word in words:
            name = EC.emitter_class(word)
            # MSR[FP] = 0 (stop at 0xFFF00800; dcbz_l is not an FP instruction)
            st, mem = self.gate_state(word, True)
            st["msr"] &= ~FP
            if name == "dcbz_l":
                st["spr"][920] = HID2 | LCE
            spec = "engine-only" if name == "stfiwx" else "compare"
            self.one("gate", word, st, name, mem=mem, spec=spec,
                     reason="no spec module models stfiwx" if spec != "compare" else "", expect=False)
            if (word >> 26) == 4 or (word >> 26) in (56, 57, 60, 61):
                # Valid HID2 values (LSQE|PSE kept), alone and with MSR[FP] = 0: the paired-single
                # classes run or raise FP unavailable; dcbz_l runs with LCE and is illegal without it
                for hid2, fpbit in ((HID2 & ~WPE, FP), (HID2 | LCE, FP), (HID2, 0), (HID2 & ~LCE, FP),
                                    (HID2 | LCE, 0)):
                    st, mem = self.gate_state(word, rng.random() < 0.7)
                    st["spr"][920] = hid2
                    st["msr"] = (st["msr"] & ~FP) | fpbit
                    self.one("gate", word, st, name, mem=mem, expect=False)
        # mid-block faults after k straight instructions, at both vector bases
        for k in range(0, 7):
            for word in rng.sample(words, 6):
                name = EC.emitter_class(word)
                if name == "stfiwx":
                    continue
                body = [E.addi(10, 10, 1)] * k + [word, E.addi(11, 11, 1)]
                for ip in (True, False):
                    st, mem = self.gate_state(word, ip)
                    st["msr"] &= ~FP
                    st["residue"] = rng.randrange(12)  # no effect under native time; the draw keeps the RNG stream
                    if (word >> 26) == 4 and rng.random() < 0.4:
                        st["spr"][920] = HID2 | LCE
                    row = self.alloc(len(body) + 1)
                    self.put_row(row, body + [E.BLR], "gate_mid")
                    v = self.add("gate", [row], row, st, " ".join(f"{w:08X}" for w in body), mem=mem,
                                 mnemonic=f"addi*{k},{name}", shape="fault-mid-block")
                    if k >= 1:
                        # cold entry straight onto the faulting instruction (dispatcher find_range)
                        self.add("gate", [row], row + 4 * k, json.loads(json.dumps(st)),
                                 f"cold entry at the fault of {v['id']}", mem=mem, parent=v["id"], mnemonic=name,
                                 shape="fault-cold-entry")

    # =========================================================================================
    # sys
    # =========================================================================================
    def fam_sys(self, count):
        rng = self.rng
        # ---- sc: stops at both vector bases
        for i in range(count):
            body = [E.addi(3, 3, 1)] * rng.randrange(0, 5) + [X.SC, E.addi(4, 4, 1)]
            for ip in (True, False):
                st = self.base_state()
                st["msr"] = MSR | (IP if ip else 0) | (EE if rng.random() < 0.3 else 0) | \
                    rng.choice((0, 0, 0x4, 0x800, 0x100))
                st["dec"] = 0x7FFFFFFF
                row = self.alloc(len(body) + 1)
                self.put_row(row, body + [E.BLR], "sys_sc")
                self.add("sys", [row], row, st, " ".join(f"{w:08X}" for w in body), mnemonic="sc",
                         shape="sc-stop" if ip else "sc-stop-ip0")
        # ---- rfi with MSR variants
        for i in range(count):
            target = self.alloc(4)
            self.put_row(target, [E.addi(5, 5, 1), E.addi(6, 6, 1), E.BLR], "sys_rfi_target")
            src = self.alloc(8)
            mid = rng.random() < 0.3
            dest = target + (4 if mid else 0) + rng.choice((0, 0, 1, 2, 3))
            srr1 = MSR | rng.choice((0, IP)) | rng.choice((0, EE)) | rng.choice((0, 0x4, 0x100)) | \
                rng.choice((0, 0, 0x00040000, 0x80000000, 0x02000000, 0x00800000))
            if rng.random() < 0.3:
                srr1 &= ~FP
            body = E.load_address(12, dest) + [E.mtspr(12, 26)] + E.load_address(12, srr1) + [E.mtspr(12, 27), X.RFI]
            self.put_row(src, body, "sys_rfi")
            st = self.base_state()
            st["msr"] = MSR | IP
            st["dec"] = 0x7FFFFFFF
            extra = {}
            if srr1 & EE and rng.random() < 0.6:
                extra = {"irq_ext": 0, "policy": "irq", "shape": "rfi-ee-on-pending"}
            self.add("sys", [src, target], src, st, f"rfi srr1={srr1:08X} -> {dest:08X}", mnemonic="rfi", **extra)
        # ---- tw / twi: every TO mask x operand relations
        rel = ((0x00000001, 0x00000002), (0xFFFFFFFF, 0x00000001), (0x00000005, 0x00000005),
               (0x00000002, 0xFFFFFFFF), (0x7FFFFFFF, 0x80000000), (0x80000000, 0x7FFFFFFF), (0, 0))
        for to in range(32):
            for a_val, b_val in rel:
                for immediate in (False, True):
                    st = self.base_state()
                    ip = rng.random() < 0.6
                    st["msr"] = MSR | (IP if ip else 0)
                    st["dec"] = 0x7FFFFFFF
                    st["residue"] = rng.randrange(12)  # no effect under native time; the draw keeps the RNG stream
                    st["gpr"][7] = a_val
                    if immediate:
                        imm = b_val if b_val < 0x8000 or b_val >= 0xFFFF8000 else rng.choice((0x7FFF, -0x8000))
                        word = X.twi(to, 7, imm)
                        name = "twi"
                    else:
                        st["gpr"][8] = b_val
                        word = X.tw(to, 7, 8)
                        name = "tw"
                    body = [E.addi(3, 3, 1)] * rng.randrange(0, 3) + [word, E.addi(4, 4, 1)]
                    row = self.row_pic(body + [E.BLR], f"sys_{name}")
                    self.add("sys", [row], row, st, " ".join(f"{w:08X}" for w in body), mnemonic=name,
                             shape="trap-stop" if ip else "trap-stop-ip0")
        # ---- mtmsr / mfmsr
        for i in range(count):
            st = self.base_state()
            st["msr"] = MSR | IP | rng.choice((0, EE))
            st["dec"] = 0x7FFFFFFF
            new = MSR | IP | rng.choice((0, EE)) | rng.choice((0, 0, 0x4, 0x800, 0x100, 0x80000000, 0x00800000))
            if rng.random() < 0.3:
                new &= ~FP
            st["gpr"][4] = new
            body = [X.mfmsr(3), X.mtmsr(4), X.mfmsr(5), E.addi(6, 6, 1)]
            extra = {}
            if new & EE and not st["msr"] & EE and rng.random() < 0.5:
                extra = {"irq_ext": 0, "policy": "irq", "shape": "mtmsr-ee-on-pending"}
            row = self.row_pic(body + [E.BLR], "sys_msr")
            self.add("sys", [row], row, st, " ".join(f"{w:08X}" for w in body), mnemonic="mfmsr,mtmsr,mfmsr",
                     **extra)
        # ---- mtspr / mfspr matrix
        sprs = [1, 8, 9, 18, 19, 22, 26, 27, 272, 273, 274, 275, 268, 269, 284, 285, 287, 1008, 1009, 920,
                912, 913, 914, 915, 916, 917, 918, 919, 921, 922, 923, 282, 1013, 1010, 1011, 1017, 1019, 1020,
                1021, 1022, 25, 530, 531, 540, 541, 936, 937, 938, 939, 940, 941, 942, 943, 944, 952, 953, 954,
                955, 956, 957, 958, 959, 0, 2, 3, 100, 500, 1023, 7, 16]
        for number in sprs:
            for i in range(max(1, count // 12)):
                st = self.base_state()
                st["msr"] = MSR | IP
                st["dec"] = 0x7FFFFFFF
                r = rng.choice((3, 4, 5, 30))
                value = rng.choice((0, 0xFFFFFFFF, rng.getrandbits(32), 0x80000000, 0x7FFFFFFF))
                if number == 920:
                    value = rng.choice((0xE0000000, 0xF0000000, 0xA0000000, 0xB0000000, value | 0xA0000000))
                if 912 <= number <= 919:
                    value = rng.getrandbits(32) & (~0xC0F8C0F8 & M32) if rng.random() < 0.8 else value
                if number == 1008:
                    value = (value & 0xFFFFFFFF) | 0x00004000
                if number in (22, 284, 285):
                    value &= 0x7FFFFFFF
                st["gpr"][r] = value
                rc = 1 if rng.random() < 0.08 else 0
                kind = rng.randrange(3)
                if number == 8:
                    body = [E.MFLR(29), X.spr_rc(E.mtspr(r, 8), rc), E.mfspr(6, 8), E.MTLR(29)]
                elif kind == 0:
                    body = [X.spr_rc(E.mfspr(r, number), rc)]
                elif kind == 1:
                    body = [X.spr_rc(E.mtspr(r, number), rc)]
                else:
                    body = [E.mtspr(r, number), E.mfspr(6, number)]
                if number in (284, 285) or kind == 1 and number in (22, 268, 269):
                    body = [E.addi(3, 3, 1)] * rng.randrange(0, 12) + body
                row = self.row_pic(body + [E.BLR], f"sys_spr_{number}")
                self.add("sys", [row], row, st, " ".join(f"{w:08X}" for w in body), mnemonic=f"spr{number}",
                         spec="auto", reason=f"SPR {number}: compared when a spec module carries the register")
        # ---- mftb with any TBR field
        for i in range(count):
            number = rng.choice((268, 269, 22, 26, 1, 8, 9, 284, 285, rng.randrange(1024)))
            st = self.base_state()
            st["msr"] = MSR | IP
            st["dec"] = 0x7FFFFFFF
            body = [E.addi(3, 3, 1)] * rng.randrange(0, 13) + [X.mftb(5, number)]
            row = self.row_pic(body + [E.BLR], "sys_mftb")
            self.add("sys", [row], row, st, " ".join(f"{w:08X}" for w in body), mnemonic="mftb", spec="auto",
                     reason=f"mftb TBR {number}: compared when a spec module carries the register")
        # ---- MSR[PR] = 1: privileged instructions
        priv = [X.mfmsr(5), X.mtmsr(5), X.RFI, E.mfspr(5, 26), E.mtspr(5, 272), E.mfspr(5, 1), E.mtspr(5, 22),
                E.mfspr(5, 268), X.mftb(5, 268), E.mtspr(5, 284), E.mfspr(5, 1008), E.mfspr(5, 920),
                E.mtspr(5, 9), X.SC]
        for i in range(max(2, count // 4)):
            for word in priv:
                st = self.base_state()
                st["msr"] = MSR | IP | PR
                st["dec"] = 0x7FFFFFFF
                st["gpr"][5] = MSR | IP | PR
                body = [E.addi(3, 3, 1), word, E.addi(4, 4, 1)]
                row = self.row_pic(body + [E.BLR], "sys_priv")
                if word == X.RFI:
                    # an rfi that executes returns to the next instruction with the same user-mode MSR
                    st["spr"][26] = row + 8
                    st["spr"][27] = st["msr"]
                self.add("sys", [row], row, st, " ".join(f"{w:08X}" for w in body), mnemonic="priv",
                         shape="privileged", spec="auto", reason="MSR[PR]=1: compared when a spec module models the word")
        # ---- classes no spec module models (engine vs engine only)
        for i in range(max(2, count // 3)):
            st = self.base_state()
            st["msr"] = MSR | IP
            st["dec"] = 0x7FFFFFFF
            st["sr"] = {n: rng.getrandbits(32) for n in range(16)}
            ea = self.ea_in_window(8, 4)
            st["gpr"][3] = ea
            st["gpr"][4] = 0
            st["gpr"][5] = rng.getrandbits(32)
            mem = self.mem_around(ea, 8)
            for word, name in ((X.ISYNC, "isync"), (X.mtsr(rng.randrange(16), 5), "mtsr"),
                               (X.mfsr(6, rng.randrange(16)), "mfsr"), (X.mtsrin(5, 7), "mtsrin"),
                               (X.mfsrin(6, 7), "mfsrin"), (E.x_form(31, 0, 3, 4, 278), "dcbt"),
                               (E.x_form(31, 0, 3, 4, 246), "dcbtst"), (E.x_form(31, 0, 0, 0, 598), "sync"),
                               (E.x_form(31, 0, 0, 0, 566), "tlbsync"), (E.x_form(31, 0, 0, 0, 854), "eieio"),
                               (E.x_form(31, 0, 0, 4, 306), "tlbie"), (E.x_form(31, 0, 0, 0, 370), "tlbia"),
                               (E.x_form(31, 6, 3, 4, 310), "eciwx"), (E.x_form(31, 5, 3, 4, 438), "ecowx"),
                               (E.x_form(31, 0, 3, 4, 86), "dcbf"), (E.x_form(31, 0, 3, 4, 54), "dcbst"),
                               (E.x_form(31, 0, 3, 4, 470), "dcbi"), (E.x_form(31, 0, 3, 4, 982), "icbi")):
                st2 = json.loads(json.dumps(st))
                st2["gpr"][7] = rng.getrandbits(32)
                self.one("sys", word, st2, name, mem=mem, spec="engine-only",
                         reason=f"no spec module models {name}", expect=False)

    # =========================================================================================
    # fpmode: controlled FPSCR-mode experiments. One arithmetic word and one state, repeated with only
    # RN (0..3), only NI (0/1), only the enable bits, or only MSR[FE0|FE1] changed, so a divergence
    # that follows the mode bit is attributable to it (the comparator reports each vector independently).
    # =========================================================================================
    def fam_fpmode(self, count):
        rng = self.rng
        arith = [(63, 21), (63, 20), (63, 18), (63, 25), (63, 29), (63, 31), (59, 21), (59, 20), (59, 18),
                 (59, 25), (59, 29), (59, 30), (4, 21), (4, 20), (4, 18), (4, 25), (4, 28), (4, 31), (4, 10),
                 (4, 14)]
        for i in range(count):
            op, xo = rng.choice(arith)
            d, a, b, c = rng.sample(range(1, 32), 4)
            if xo in (18, 20, 21, 10):
                c = 0
            if xo in (25,):
                b = 0
            word = X.a_form(op, d, a, b, c, xo)
            single = op != 63
            base = self.ps_state(ip=True) if single else self.fp_state(False, ip=True)
            base["fpscr"] = 0
            base["msr"] &= ~(FE0 | FE1)
            if rng.random() < 0.5:
                # operands near a single/double rounding boundary so RN visibly matters
                for r_ in (a, b, c):
                    base["fpr"][r_] = [self.rsingle(), self.rsingle()]
            group = f"fpmode-{i:04d}"
            for rn in range(4):
                st = json.loads(json.dumps(base))
                st["fpscr"] = rn
                self.one("fpmode", word, st, EC.emitter_class(word), expect=False, shape=f"{group}/RN={rn}")
            for ni in (0, 1):
                st = json.loads(json.dumps(base))
                st["fpscr"] = 0x4 * ni
                if ni:
                    for r_ in (a, b, c):
                        base_val = rng.choice((0x0000000000000001, 0x36A0000000000000, 0x3800000000000000, 0x0))
                        st["fpr"][r_][0] = base_val
                self.one("fpmode", word, st, EC.emitter_class(word), expect=False, shape=f"{group}/NI={ni}")
            for en in (0x00, 0xF8):
                st = json.loads(json.dumps(base))
                st["fpscr"] = en
                st["fpr"][b][0] = rng.choice((0x0, 0x7FF0000000000000, 0x7FF0000000000001))
                self.one("fpmode", word, st, EC.emitter_class(word), expect=False, shape=f"{group}/EN={en:#x}")
            for fe in (0, FE0, FE1):
                st = json.loads(json.dumps(base))
                st["fpscr"] = 0xF8
                st["msr"] |= fe
                st["fpr"][b][0] = 0x0
                self.one("fpmode", word, st, EC.emitter_class(word), expect=False, shape=f"{group}/FE={fe:#x}")

    # =========================================================================================
    # classes: the emitter classes the random families can miss (explicit, one vector set each)
    # =========================================================================================
    def fam_classes(self, reps):
        rng = self.rng
        for i in range(reps):
            st = self.base_state()
            st["gpr"][4] = rng.choice((0x80000000, 0x7FFFFFFF, 1, 0))
            st["xer"] = rng.choice((0, 0x40000000, 0x80000000, 0xC0000000))
            for rc in (0, 1):
                self.one("classes", E.xo_form(3, 4, 0, 104, 1, rc), json.loads(json.dumps(st)), "neg")
            fps = self.ps_state(ip=True)
            for xo in (0, 32, 64, 96):
                self.one("classes", X.fcmp(4, xo, rng.randrange(8), 1, 2) | 1, json.loads(json.dumps(fps)),
                         EC.X4[xo], spec="engine-only", reason="invalid form: ps_cmp with Rc=1 (reserved bit 31)",
                         expect=False)
            fps["spr"][920] = HID2 | LCE
            ea = self.ea_in_window(40, 32)
            fps["gpr"][3], fps["gpr"][4] = ea, 0
            self.one("classes", E.x_form(4, 0, 3, 4, 1014, 1), fps, "dcbz_l", mem=self.mem_around(ea, 32, 16, 16),
                     spec="engine-only", reason="invalid form: dcbz_l with Rc=1 (reserved bit 31)", expect=False)

    # =========================================================================================
    # bnd: block-boundary sequences
    # =========================================================================================
    def fam_bnd(self, count):
        rng = self.rng
        for i in range(count):
            shape = i % 14
            st = self.base_state()
            st["msr"] = MSR | IP
            st["dec"] = 0x7FFFFFFF
            ea = self.ea_in_window(32, 4)
            st["gpr"][3] = ea
            mem = self.mem_around(ea, 32, 16, 16)
            if shape == 0:      # CR producer, b to a label, consumer at the label
                A = self.alloc(8)
                cmpw, fld = rng.choice(((E.cmpw(0, 4, 5), 0), (E.cmplw(1, 4, 5), 1),
                                        (E.cmp_imm(11, 2, 4, self.imm16()), 2), (E.xo_form(6, 4, 5, 266, 0, 1), 0)))
                bi = fld * 4 + rng.randrange(4)
                words = [cmpw, E.b(A + 4, A + 8), E.addi(9, 9, 1), E.bc(rng.choice((12, 4)), bi, A + 12, A + 24),
                         E.addi(10, 10, 1), E.addi(10, 10, 2), E.addi(11, 11, 1), E.BLR]
                if rng.random() < 0.5:
                    st["gpr"][5] = st["gpr"][4]
                self.put_row(A, words, "bnd_cr_label")
                self.add("bnd", [A], A, st, "cmp ; b L ; L: bc", mnemonic="cr-across-label", shape="cr-label")
            elif shape == 1:    # CR producer, call, consumer after the return
                C = self.row_pic([E.addi(12, 12, 1), E.BLR], "bnd_callee_nocr")
                A = self.alloc(8)
                words = [E.MFLR(30), E.cmpw(3, 4, 5), E.b(A + 8, C, lk=1), E.MTLR(30),
                         E.bc(rng.choice((12, 4)), 12 + rng.randrange(3), A + 16, A + 24), E.addi(9, 9, 1),
                         E.addi(10, 10, 1), E.BLR]
                if rng.random() < 0.5:
                    st["gpr"][5] = st["gpr"][4]
                self.put_row(A, words, "bnd_cr_call")
                self.add("bnd", [A, C], A, st, "cmp ; bl ; bc", mnemonic="cr-across-call", shape="cr-call")
            elif shape == 2:    # XER[CA] producer, call, adde after the return; and across a label
                C = self.row_pic([E.addi(12, 12, 1), E.BLR], "bnd_callee_nocr")
                A = self.alloc(8)
                st["gpr"][4], st["gpr"][5] = rng.choice(((0xFFFFFFFF, 1), (0x80000000, 0x80000000), (1, 2),
                                                         (0x7FFFFFFF, 0x7FFFFFFF)))
                prod = rng.choice((E.xo_form(6, 4, 5, 10), E.d_form(8, 6, 4, self.imm16()), E.x_form(31, 4, 6, 3, 824)))
                cons = rng.choice((E.xo_form(7, 4, 5, 138), E.xo_form(7, 4, 0, 202), E.xo_form(7, 5, 0, 234, 1)))
                if rng.random() < 0.5:
                    words = [E.MFLR(30), prod, E.b(A + 8, C, lk=1), E.MTLR(30), cons, E.BLR]
                    self.add_row_vec("bnd", A, words, [A, C], st, "CA producer ; bl ; adde", "ca-call", mem)
                else:
                    words = [prod, E.b(A + 4, A + 12), E.addi(9, 9, 1), cons, E.BLR]
                    self.add_row_vec("bnd", A, words, [A], st, "CA producer ; b L ; L: adde", "ca-label", mem)
            elif shape == 3:    # FP guard hoist vs a label reached with MSR[FP] = 0
                A = self.alloc(10)
                ip = rng.random() < 0.5
                st["msr"] = MSR | (IP if ip else 0)
                st["gpr"][7] = 1
                st["gpr"][8] = st["msr"] & ~FP
                words = [E.d_form(50, 1, 3, 0), E.d_form(50, 2, 3, 8), E.cmp_imm(11, 0, 7, 0), E.addi(7, 7, -1),
                         E.bc(12, 2, A + 16, A + 32), X.mtmsr(8), E.b(A + 24, A + 4), E.BLR, E.BLR]
                self.put_row(A, words, "bnd_fpguard_label")
                self.add("bnd", [A], A, st, "lfd ; L: lfd ; ... mtmsr FP=0 ; b L", mem=mem, mnemonic="fpguard-label",
                         shape="fpguard-label")
            elif shape == 4:    # FP guard across a call / bcctrl whose callee clears MSR[FP]
                C = self.row_pic([X.mfmsr(6), X.rlwinm(6, 6, 0, 19, 17), X.mtmsr(6), E.BLR], "bnd_callee_fpoff")
                A = self.alloc(10)
                ip = rng.random() < 0.5
                st["msr"] = MSR | (IP if ip else 0)
                if rng.random() < 0.5:
                    words = [E.MFLR(30), E.d_form(50, 1, 3, 0), E.b(A + 8, C, lk=1), E.MTLR(30), E.d_form(50, 2, 3, 8),
                             E.BLR]
                    note = "lfd ; bl (callee clears FP) ; lfd"
                else:
                    words = [E.MFLR(30), E.d_form(50, 1, 3, 0)] + E.load_address(12, C) + \
                        [E.MTCTR(12), E.bcctr(20, 0, 1), E.MTLR(30), E.d_form(50, 2, 3, 8), E.BLR]
                    note = "lfd ; bcctrl (callee clears FP) ; lfd"
                self.add_row_vec("bnd", A, words, [A, C], st, note, "fpguard-call", mem)
            elif shape == 5:    # HID2 hoist vs mtspr 920 (LCE/WPE varied) and vs a label
                A = self.alloc(10)
                st["msr"] = MSR | (IP if rng.random() < 0.5 else 0)
                st["fpr"] = [[self.rsingle(), self.rsingle()] for _ in range(32)]
                st["gpr"][9] = rng.choice((HID2 | LCE, HID2 & ~WPE, (HID2 | LCE) & ~WPE))
                if rng.random() < 0.5:
                    words = [X.a_form(4, 1, 2, 3, 0, 21), E.mtspr(9, 920), X.a_form(4, 4, 5, 6, 0, 20),
                             X.psq_d(56, 7, 3, 0, 0, 0), E.BLR]
                    note = "ps_add ; mtspr HID2 ; ps_sub ; psq_l"
                    self.add_row_vec("bnd", A, words, [A], st, note, "hid2-mtspr", mem)
                else:
                    st["gpr"][7] = 1
                    words = [X.a_form(4, 1, 2, 3, 0, 21), X.a_form(4, 4, 5, 6, 0, 20), E.cmp_imm(11, 0, 7, 0),
                             E.addi(7, 7, -1), E.bc(12, 2, A + 16, A + 32), E.mtspr(9, 920), E.b(A + 24, A + 4),
                             E.BLR, E.BLR]
                    self.add_row_vec("bnd", A, words, [A], st, "ps_add ; L: ps_sub ; mtspr HID2 ; b L",
                                     "hid2-label", mem)
            elif shape == 6:    # update-form store then load of the same base across a branch
                A = self.alloc(10)
                st["gpr"][6] = ea
                words = [E.d_form(37, 5, 6, 8), E.b(A + 4, A + 12), E.addi(9, 9, 1), E.d_form(32, 7, 6, -8),
                         E.d_form(33, 8, 6, 4), E.bc(rng.choice((12, 4)), rng.randrange(32), A + 20, A + 28),
                         E.addi(10, 10, 1), E.d_form(35, 11, 6, -1), E.x_form(31, 6, 6, 4, 183), E.BLR]
                st["gpr"][4] = 4
                self.put_row(A, words, "bnd_update_branch")
                self.add("bnd", [A], A, st, "stwu ; b L ; L: lwz ; lwzu ; bc ; lbzu ; stwux", mem=mem,
                         mnemonic="update-across-branch", shape="update-branch")
            elif shape == 7:    # EndOfFunction fall-through into the next row (a full 16-word row)
                A = self.alloc(16)
                B = self.alloc(3)
                assert B == A + 0x40
                words = [E.addi(3 + (j % 5), 3 + (j % 5), j + 1) for j in range(15)] + [E.cmpw(0, 4, 5)]
                self.put_row(A, words, "bnd_eof_a")
                self.put_row(B, [E.bc(12, 2, B, B + 8), E.addi(9, 9, 1), E.BLR], "bnd_eof_b")
                self.add("bnd", [A, B], A, st, "16 instructions falling off the record into the next", mnemonic="eof",
                         shape="end-of-function")
                self.add("bnd", [A, B], A + 4 * rng.randrange(1, 16), json.loads(json.dumps(st)),
                         "mid entry, then EndOfFunction", mnemonic="eof", shape="end-of-function-entry")
            elif shape == 8:    # bctr to another row's entry, and to a non-entry address of another row
                B = self.alloc(6)
                self.put_row(B, [E.addi(5, 5, 1), E.addi(5, 5, 2), E.addi(6, 6, 3), E.addi(7, 7, 4), E.BLR],
                             "bnd_bctr_target")
                A = self.alloc(6)
                dest = B + 4 * rng.choice((0, 1, 2, 3))
                words = E.load_address(12, dest) + [E.MTCTR(12), E.bcctr(20, 0), E.addi(9, 9, 1), E.BLR]
                self.put_row(A, words, "bnd_bctr_default")
                self.add("bnd", [A, B], A, st, f"bctr to {'entry' if dest == B else 'non-entry'} of another row",
                         mnemonic="bctr-default", shape="bctr-default")
            elif shape == 9:    # callee returns elsewhere: resume_elsewhere at a recovered root / mid-block
                A = self.alloc(10)
                elsewhere = A + 20 if rng.random() < 0.5 else A + 24
                C = self.alloc(4)
                self.put_row(C, E.load_address(12, elsewhere) + [E.MTLR(12), E.BLR], "bnd_callee_elsewhere")
                words = [E.MFLR(30), E.b(A + 4, C, lk=1), E.addi(3, 3, 1), E.MTLR(30), E.BLR,
                         E.addi(4, 4, 1), E.addi(4, 4, 2), E.MTLR(30), E.BLR]
                self.put_row(A, words, "bnd_resume_elsewhere")
                self.add("bnd", [A, C], A, st, f"callee returns to {elsewhere:08X}", mnemonic="resume-elsewhere",
                         shape="resume-elsewhere")
            elif shape == 10:   # bclrl / conditional bclr with CTR, and a bcctr conditional default
                C = self.row_pic([E.addi(5, 5, 9), E.BLR], "bnd_icallee")
                A = self.alloc(10)
                bo = rng.choice((20, 12, 4, 16, 18, 0))
                words = [E.MFLR(30)] + E.load_address(12, C) + [E.MTLR(12), E.bclr(bo, rng.randrange(32), 1),
                                                                E.MFLR(29), E.MTLR(30), E.BLR]
                st["ctr"] = rng.choice((1, 2, 0))
                self.add_row_vec("bnd", A, words, [A, C], st, f"bclrl {bo}", "bclrl", mem)
            elif shape == 11:   # every interior entry of a mixed row (labels, fusion, calls)
                C = self.row_pic([E.addi(12, 12, 1), E.BLR], "bnd_callee_nocr")
                A = self.alloc(14)
                words = [E.MFLR(30), E.addi(3, 3, 1), E.cmpw(0, 4, 5), E.bc(4, 0, A + 12, A + 20), E.addi(6, 6, 1),
                         E.b(A + 20, C, lk=1), E.addi(7, 7, 1), E.xo_form(8, 7, 6, 10), E.xo_form(9, 8, 0, 202),
                         E.MTLR(30), E.BLR]
                self.put_row(A, words, "bnd_entries")
                if rng.random() < 0.5:
                    st["gpr"][5] = st["gpr"][4]
                st["gpr"][30] = SENTINEL          # entries past the mflr still return to the sentinel
                for k in range(len(words)):
                    self.add("bnd", [A, C], A + 4 * k, json.loads(json.dumps(st)), f"entry +{4 * k:#x}",
                             mnemonic="entries", shape="every-entry")
            elif shape == 12:   # RETIRED (M1): the sc round trip through the real-mode helper rows
                body = [E.addi(3, 3, 1), X.SC, E.addi(4, 4, 1)]
                st["msr"] = MSR
                self.row_pic(body + [E.BLR], "bnd_sc_rt")   # the row keeps every later row's address
                self.retire_id("bnd")
            else:               # bctr backward switch with a mid-block non-label target in the same row
                A = self.alloc(10)
                st["gpr"][4] = rng.getrandbits(31)
                st["gpr"][5] = st["gpr"][4] + rng.randrange(1, 6)
                words = E.load_address(12, A + 16) + [E.MTCTR(12), E.addi(9, 9, 1), E.addi(4, 4, 1),
                                                      E.cmplw(0, 4, 5), E.bc(4, 0, A + 24, A + 36), E.bcctr(20, 0),
                                                      E.addi(9, 9, 2), E.BLR]
                self.put_row(A, words, "bnd_bctr_nonlabel")
                self.add("bnd", [A], A, st, "bctr back to a non-label address (default path)", mnemonic="bctr-back",
                         shape="bctr-nonlabel", max_steps=400)

    # =========================================================================================
    # tail: generated after every other family, so each earlier vector keeps its rows, addresses and
    # candidate fingerprint (the ledger stays exact)
    # =========================================================================================
    def fam_tail(self, reps):
        rng = self.rng
        # neg. (OE=0, Rc=1): the other families executed this class only as an invalid form (rB != 0)
        for i in range(reps):
            for d, a in ((3, 4), (4, 4)):
                st = self.base_state()
                st["gpr"][4] = (0x80000000, 0x7FFFFFFF, 0, 1)[i % 4]
                st["xer"] = rng.choice((0, 0x40000000, 0x80000000, 0xC0000000))
                self.one("tail", E.xo_form(d, a, 0, 104, 0, 1), st, "neg")
        # FP-guard hoist across a call and across mtmsr whose resume address is ANOTHER record's start.
        # main.cpp adds a resume point to a row's entries (hence labels) only if no other record claims
        # it; here the next record claims it, so the only reset before the second lfd is the one the
        # call / mtmsr emitter performs itself (mutants E03/E04 remove exactly those resets).
        C = self.row_pic([X.mfmsr(6), X.rlwinm(6, 6, 0, 19, 17), X.mtmsr(6), E.BLR], "bnd_callee_fpoff")
        for i in range(reps):
            for kind in ("call", "mtmsr", "call-bcctrl"):
                st = self.base_state()
                st["msr"] = MSR | (IP if i % 2 == 0 else 0)
                st["dec"] = 0x7FFFFFFF
                ea = self.ea_in_window(32, 4)
                st["gpr"][3] = ea
                mem = self.mem_around(ea, 32, 16, 16)
                A = self.alloc(12)
                if kind == "call":
                    head = [E.MFLR(30), E.d_form(50, 1, 3, 0), E.b(A + 8, C, lk=1)]
                    tail = [E.d_form(50, 2, 3, 8), E.MTLR(30), E.BLR]
                    rows = [A, C]
                elif kind == "call-bcctrl":
                    head = [E.MFLR(30), E.d_form(50, 1, 3, 0)] + E.load_address(12, C) + [E.MTCTR(12),
                                                                                         E.bcctr(20, 0, 1)]
                    tail = [E.d_form(50, 2, 3, 8), E.MTLR(30), E.BLR]
                    rows = [A, C]
                else:
                    head = [E.d_form(50, 1, 3, 0), X.mfmsr(6), X.rlwinm(6, 6, 0, 19, 17), X.mtmsr(6)]
                    tail = [E.d_form(50, 2, 3, 8), E.BLR]
                    rows = [A]
                B = A + 4 * len(head)
                self.put_row(A, head + tail[:1], f"tail_overlap_{kind}_a")
                self.put_row(B, tail, f"tail_overlap_{kind}_b")
                rows = rows + [B]
                self.add("tail", rows, A, st, f"lfd ; {kind} (FP off) ; [record start] lfd", mem=mem,
                         mnemonic=f"fpguard-{kind}-overlap", shape=f"fpguard-{kind}-overlap")

        # A guest loop that calls a function on every iteration: a correct translation returns to the caller's
        # frame after each call, so the host stack stays flat however many iterations run. The driver paints
        # the host stack under every native run and fails a lane whose deepest overwrite exceeds its calibrated
        # depth by more than --host-stack-bound (status host-stack-bound; compare_f1b.py reads the verdict
        # host_stack_within_bound). The hardware tick at interval 1 these vectors used to request, to sample
        # the stack at every retire_block, is retired with the instruction clock.
        leaf = self.row_pic([E.addi(5, 5, 1), E.BLR], "tail_loop_leaf")
        for iterations in (250, 1000):
            st = self.base_state()
            st["msr"] = MSR | IP
            st["dec"] = 0x7FFFFFFF
            st["ctr"] = iterations
            A = self.alloc(6)
            words = [E.MFLR(30), E.b(A + 4, leaf, lk=1), E.addi(4, 4, 1), E.bc(16, 0, A + 12, A + 4), E.MTLR(30),
                     E.BLR]
            self.put_row(A, words, "tail_call_loop")
            self.add("tail", [A, leaf], A, st, f"loop of {iterations} calls (host stack stays flat)",
                     mnemonic="call-loop", shape="call-loop", max_steps=5 * iterations + 64)

        # Event spins (native time). A RAM-poll idle loop `lwz r4,0(r5) ; cmplwi r4,0 ; beq self` on a zero word
        # nothing writes can only be ended by an event, and these vectors contain no wait point and no event
        # source: the game clock never moves (DEC 0 is one tick before its edge and stays there), so the loop
        # spins in zero game time, exactly the hazard DESIGN.md section 4 describes. Every lane must end at the
        # loop head with the loop's fixed-point state (r4 = 0, CR0 EQ): native unlogged lanes through the
        # driver's request_stop_from_host watchdog (status watchdog, the cross-thread stop System's wait-point
        # watchdog uses), native logged lanes at the driver's access cap (access-limit), interpreter lanes and
        # the spec at the step bound (step-limit). max_steps is a multiple of the loop's 3 instructions so the
        # step bound also ends at the loop head. compare_f1b.py accepts those statuses for these vectors only
        # (event_spin(): this family, shape and loop).
        # RETIRED 2026-10-06: the 28 "ram-poll-idle/interval" vectors that ran this loop under hardware service
        # intervals of 64 and 2^20 instructions with a decrementer edge at retired count k: the service interval,
        # coalesce_ram_poll and the instruction-positioned edge no longer exist.
        A = self.alloc(4)
        self.put_row(A, [E.d_form(32, 4, 5, 0), E.cmp_imm(10, 0, 4, 0), E.bc(12, 2, A + 8, A), E.BLR], "tail_idle")
        for _ in range(28):
            self.retire_id("tail")  # the retired ram-poll-idle/interval vectors keep their ids reserved
        for ee, dec, what in ((EE, 0x00000000, "EE = 1, DEC one tick before its edge"),
                              (0, 0x7FFFFFFF, "EE = 0")):
            st = self.base_state()
            st["msr"] = MSR | IP | ee
            st["gpr"][5] = DATA_BASE + 0x2000
            st["reserve"] = False
            st["dec"] = dec
            self.add("tail", [A], A, st, f"ram-poll-idle waiting for an event the harness never delivers ({what})",
                     mem=[[DATA_BASE + 0x2000, "00" * 16]], mnemonic="ram-poll-idle", shape=EVENT_SPIN_SHAPE,
                     max_steps=EVENT_SPIN_STEPS)

        # RETIRED (M1): the two vectors that entered the real-mode 0x900 handler row directly (IR = DR = 0).
        # There is no real mode and no handler row; their ids stay reserved.
        for _srr1 in (MSR, MSR | EE):
            self.base_state()
            self.retire_id("tail")

    def add_row_vec(self, family, A, words, rows, st, note, shape, mem):
        self.put_row(A, words, f"{family}_{shape}")
        return self.add(family, rows, A, st, note, mem=mem, mnemonic=shape, shape=shape)

    # =========================================================================================
    # irq: interrupt recognition (native time: the line asserted before entry; see the module docstring)
    # =========================================================================================
    def irq_shapes(self):
        rng = self.rng
        out = []
        A = self.alloc(6)
        n = rng.randrange(3, 6)
        self.put_row(A, [E.addi(3, 3, 1)] * n + [E.BLR], "irq_straight")
        out.append(("straight", [A], A, {}))
        A = self.alloc(4)
        self.put_row(A, [E.addi(4, 4, 1), E.bc(16, 0, A + 4, A), E.BLR], "irq_bdnz")
        out.append(("bdnz-back-edge", [A], A, {"ctr": 4}))
        A = self.alloc(5)
        self.put_row(A, [E.addi(3, 3, 1), E.cmplw(0, 3, 5), E.bc(12, 0, A + 8, A), E.BLR], "irq_cmploop")
        out.append(("fused-compare-loop", [A], A, {"gpr": {3: 10, 5: 13}}))
        A = self.alloc(4)
        self.put_row(A, [E.addi(4, 4, 1), E.b(A + 4, A + 8), E.BLR], "irq_bfwd")
        out.append(("b-forward", [A], A, {}))
        C = self.alloc(3)
        self.put_row(C, [E.addi(5, 5, 1), E.addi(5, 5, 2), E.BLR], "irq_callee")
        A = self.alloc(6)
        self.put_row(A, [E.MFLR(30), E.addi(3, 3, 1), E.b(A + 8, C, lk=1), E.addi(3, 3, 2), E.MTLR(30), E.BLR],
                     "irq_call")
        out.append(("bl-return", [A, C], A, {}))
        A = self.alloc(9)
        self.put_row(A, [E.MFLR(30)] + E.load_address(12, C) + [E.MTLR(12), E.bclr(20, 0, 1), E.addi(3, 3, 1),
                                                                E.MTLR(30), E.BLR], "irq_bclrl")
        out.append(("bclrl", [A, C], A, {}))
        A = self.alloc(9)
        self.put_row(A, [E.MFLR(30)] + E.load_address(12, C) + [E.MTCTR(12), E.bcctr(20, 0, 1), E.addi(3, 3, 1),
                                                                E.MTLR(30), E.BLR], "irq_bcctrl")
        out.append(("bcctrl", [A, C], A, {}))
        A = self.alloc(9)
        head = A + 12
        self.put_row(A, E.load_address(12, head) + [E.MTCTR(12), E.addi(4, 4, 1), E.cmplw(0, 4, 5),
                                                    E.bc(4, 0, A + 20, A + 32), E.bcctr(20, 0), E.b(A + 28, head),
                                                    E.BLR], "irq_bctr_back")
        out.append(("bctr-backward-switch", [A], A, {"gpr": {4: 100, 5: 103}}))
        A = self.alloc(9)
        self.put_row(A, E.load_address(12, A + 24) + [E.MTCTR(12), E.addi(4, 4, 1), E.bcctr(20, 0), E.addi(4, 4, 2),
                                                      E.addi(4, 4, 3), E.addi(4, 4, 4), E.BLR], "irq_bctr_fwd_nonlabel")
        out.append(("bctr-forward-nonlabel", [A], A, {}))
        A = self.alloc(8)
        self.put_row(A, [E.addi(3, 3, 1), X.mfmsr(6), X.rlwinm(7, 6, 0, 17, 15), X.mtmsr(7), E.addi(3, 3, 2),
                         X.mtmsr(6), E.addi(3, 3, 3), E.BLR], "irq_mtmsr")
        out.append(("mtmsr-ee-off-on", [A], A, {}))
        B = self.alloc(3)
        self.put_row(B, [E.addi(5, 5, 1), E.addi(5, 5, 2), E.BLR], "irq_rfi_target")
        A = self.alloc(8)
        self.put_row(A, [X.mfmsr(6), E.ori(6, 6, 0x8000), E.mtspr(6, 27)] + E.load_address(12, B) +
                     [E.mtspr(12, 26), X.RFI], "irq_rfi")
        out.append(("rfi", [A, B], A, {"msr_clear_ee_start": True}))
        B = self.alloc(3)
        A = self.alloc(3)
        self.put_row(B, [E.addi(4, 4, 1), E.cmplw(0, 4, 5), E.bc(4, 0, B + 8, B + 16), E.b(B + 12, A), E.BLR],
                     "irq_xloop_b")
        self.put_row(A, [E.addi(3, 3, 1), E.b(A + 4, B)], "irq_xloop_a")
        out.append(("cross-record-back-edge", [B, A], B, {"gpr": {4: 50, 5: 53}}))
        A = self.alloc(4)
        self.put_row(A, [E.d_form(32, 4, 5, 0), E.cmp_imm(10, 0, 4, 0), E.bc(12, 2, A + 8, A), E.BLR], "irq_idle")
        out.append(("ram-poll-idle", [A], A, {"gpr": {5: DATA_BASE + 0x2000}, "infinite": True}))
        A = self.alloc(5)
        self.put_row(A, [E.addi(3, 3, 1), E.addi(3, 3, 2), E.mfspr(6, 22), E.addi(3, 3, 3), E.BLR], "irq_mfspr_dec")
        out.append(("mfspr-dec-flush", [A], A, {}))
        A = self.alloc(6)
        self.put_row(A, [E.cmpw(0, 4, 5), E.bc(4, 2, A + 4, A + 16), E.addi(3, 3, 1), E.addi(3, 3, 2),
                         E.addi(3, 3, 3), E.BLR], "irq_fallthrough")
        out.append(("conditional-fallthrough-batch", [A], A, {"gpr": {4: 1, 5: 1}}))
        A = self.alloc(5)
        self.put_row(A, [E.addi(3, 3, 1), X.SC, E.addi(3, 3, 2), E.BLR], "irq_sc")
        out.append(("sc", [A], A, {}))
        A = self.alloc(5)
        self.put_row(A, [E.addi(3, 3, 1), E.addi(3, 3, 1), X.twi(31, 3, 0), E.addi(3, 3, 2), E.BLR], "irq_trap")
        out.append(("trap", [A], A, {}))
        A = self.alloc(5)
        self.put_row(A, [E.addi(3, 3, 1), E.d_form(50, 1, 5, 0), E.addi(3, 3, 2), E.BLR], "irq_fpfault")
        out.append(("fp-unavailable", [A], A, {"gpr": {5: DATA_BASE + 0x2000}, "fp_off": True}))
        return out

    def fam_irq(self, reps):
        rng = self.rng
        from spec_runner_f1b import run_vector
        for rep in range(reps):
            for shape, rows, entry, opts in self.irq_shapes():
                st = self.base_state()
                st["msr"] = MSR | IP | EE
                if opts.get("fp_off"):
                    st["msr"] &= ~FP
                if opts.get("msr_clear_ee_start"):
                    st["msr"] &= ~EE
                st["dec"] = 0x7FFFFFFF
                if "ctr" in opts:
                    st["ctr"] = opts["ctr"]
                for r_, val in opts.get("gpr", {}).items():
                    st["gpr"][r_] = val
                st["reserve"] = False
                mem = [[DATA_BASE + 0x2000, "00" * 16]] if shape in ("ram-poll-idle", "fp-unavailable") else []
                # retired count of the uninterrupted run (spec model dry run; bounded for the idle loop): it sets
                # how many positions the retired variants occupied, so their ids stay reserved one for one
                if opts.get("infinite"):
                    total = 14
                else:
                    probe = self.probe_vector(rows, entry, st, mem)
                    total = run_vector(probe, None)["final"]["retired"]
                for k in range(0, total + 2):
                    if k == 0:
                        s2 = json.loads(json.dumps(st))
                        self.add("irq", rows, entry, s2, f"{shape}: external line asserted before entry", mem=mem,
                                 mnemonic=shape, irq_ext=0, policy="irq", shape=shape,
                                 max_steps=200 if opts.get("infinite") else 4096)
                    else:
                        self.retire_id("irq")  # external line at retired k (retired: no instruction count)
                        self.retire_id("irq")  # decrementer edge at retired k (retired: DEC never moves in a vector)
                # EE = 0: the condition is latched, never delivered
                for k in (0, max(1, total // 2)):
                    if shape == "rfi":
                        continue
                    if opts.get("infinite"):
                        continue
                    if k != 0:
                        self.retire_id("irq")  # EE = 0, line at retired k (retired with the k > 0 positions)
                        continue
                    s4 = json.loads(json.dumps(st))
                    s4["msr"] &= ~EE
                    self.add("irq", rows, entry, s4, f"{shape}: EE=0, line asserted before entry", mem=mem,
                             mnemonic=shape, irq_ext=0, policy="irq", shape=shape)
            # MSR[IP] = 0: the interrupt ends the vector at the low 0x500 vector (M1: no handler code)
            for shape, rows, entry, opts in self.irq_shapes():
                if opts.get("infinite") or shape in ("rfi", "sc", "trap", "fp-unavailable"):
                    continue
                st = self.base_state()
                st["msr"] = MSR | EE
                st["dec"] = 0x7FFFFFFF
                if "ctr" in opts:
                    st["ctr"] = opts["ctr"]
                for r_, val in opts.get("gpr", {}).items():
                    st["gpr"][r_] = val
                st["reserve"] = False
                # The retired-count position k this draw chose is gone: the line is asserted before entry, so
                # the interrupt is taken at entry (R1) and the vector ends at the 0x500 vector.
                rng.randrange(0, 6)
                self.add("irq", rows, entry, json.loads(json.dumps(st)),
                         f"{shape}: external at the low vector base, line asserted before entry", mnemonic=shape,
                         irq_ext=0, policy="irq", shape=shape + "/stop-ip0")
                rng.randrange(1, 8)  # the retired decrementer round trip's position (draw kept for the RNG stream)
                self.retire_id("irq")

    def probe_vector(self, rows, entry, st, mem):
        row_words = {f"{a:08X}": self.rows[a]["words"] for a in rows}
        return {"id": "probe", "entry": entry, "state": json.loads(json.dumps(st)), "mem": mem, "row_words": row_words,
                "max_steps": 4096, "stops": HIGH_STOPS + LOW_STOPS, "irq_ext": None}


# ---------------------------------------------------------------------------------------------
# outputs
# ---------------------------------------------------------------------------------------------
def write_dol(path: Path, gen: GenB):
    end = gen.next_slot
    size0 = end - GV.CODE_BASE
    image = bytearray(0x100 + size0)
    struct.pack_into(">I", image, 0x00, 0x100)
    struct.pack_into(">I", image, 0x48, GV.CODE_BASE)
    struct.pack_into(">I", image, 0x90, size0)
    struct.pack_into(">I", image, 0xE0, GV.CODE_BASE)
    for address, row in gen.rows.items():
        assert address >= GV.CODE_BASE, f"row {address:08X} below the code base (no real-mode rows exist)"
        base = 0x100 + address - GV.CODE_BASE
        for i, w in enumerate(row["words"]):
            struct.pack_into(">I", image, base + 4 * i, w)
    path.write_bytes(bytes(image))


def write_driver_vectors(path: Path, gen: GenB):
    lines = ["# gekko difftest vectors v2 (generated by tools/difftest/gen_f1b.py)"]
    for address in sorted(gen.rows):
        words = gen.rows[address]["words"]
        lines.append("code %08X %s" % (address, " ".join(f"{w:08X}" for w in words)))
    for v in gen.vectors:
        st = v["state"]
        lines.append(f"vector {v['id']}")
        lines.append("entry %08X" % v["entry"])
        lines.append("gpr " + " ".join(f"{x:08X}" for x in st["gpr"]))
        lines.append("fpr " + " ".join(f"{p[0]:016X} {p[1]:016X}" for p in st["fpr"]))
        for key in ("cr", "xer", "lr", "ctr", "msr", "fpscr", "dec"):
            lines.append("%s %08X" % (key, st[key]))
        lines.append("tb %016X" % st["tb"])
        lines.append("residue %d" % st["residue"])
        for number, value in sorted(st["spr"].items(), key=lambda kv: int(kv[0])):
            lines.append("spr %d %08X" % (int(number), value))
        for number, value in sorted((st.get("sr") or {}).items(), key=lambda kv: int(kv[0])):
            lines.append("sr %d %08X" % (int(number), value))
        lines.append("reserve %d %08X" % (1 if st["reserve"] else 0, st["reserve_addr"]))
        for ea, hexbytes in v["mem"]:
            lines.append("mem %08X %s" % (ea, hexbytes))
        lines.append("max %d" % v["max_steps"])
        # (No `tick` line: the hardware service interval is retired with the instruction clock; `residue` above
        # is still written for the vector format and has no effect - see difftest_driver.cpp.)
        if v.get("irq_ext") is not None:
            lines.append("irq_ext %d" % v["irq_ext"])
        if v.get("stops"):
            lines.append("stops " + " ".join(f"{a:08X}" for a in v["stops"]))
        lines.append("end")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def spec_mnemonic_check(gen: GenB, spec_dir: Path) -> list:
    import spec_runner_f1b as SR
    SR.load(spec_dir)
    problems = []
    for v in gen.vectors:
        if v["expect_mnemonic"] is None:
            continue
        word = int(v["note"].split()[0], 16)
        mods = SR.owners(word)
        names = [m.mnemonic(word) for m in mods[:1]]
        base = v["expect_mnemonic"].rstrip(".")
        if base == "mftb":
            base = "mfspr"
        if not any(isinstance(n, str) and n.rstrip(".") == base for n in names):
            problems.append(f"{v['id']}: word {word:08X} intended {v['expect_mnemonic']} spec decodes {names}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20260915)
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("--shards", type=int, default=8)
    parser.add_argument("--spec", type=Path, required=True)
    parser.add_argument("--families", default="all")
    args = parser.parse_args()

    out = args.out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    sys.path.insert(0, str(args.spec))
    import spec_runner_f1b as SR
    SR.load(args.spec)

    gen = GenB(args.seed, args.scale)

    def n(base):
        return max(1, int(round(base * args.scale)))

    fams = set(args.families.split(",")) if args.families != "all" else None

    def want(name):
        return fams is None or name in fams

    if want("f1a"):
        gen.fam_int1(n(350))
        gen.fam_spr1(n(40))
        gen.fam_clock(n(60))
        gen.fam_ls1(n(250))
        gen.fam_alias(n(2))
        gen.fam_hint()
        gen.fam_seq(n(40))
        gen.fam_lsseq(n(30))
        gen.fam_br(n(260))
        gen.fam_invalid(n(20))
    if want("fp"):
        gen.fam_fp(n(18))
    if want("ps"):
        gen.fam_ps(n(18))
    if want("classes"):
        gen.fam_classes(n(4))
    if want("fpmode"):
        gen.fam_fpmode(n(40))
    if want("fpseq"):
        gen.fam_fpseq(n(90))
    if want("gate"):
        gen.fam_gate()
    if want("sys"):
        gen.fam_sys(n(24))
    if want("bnd"):
        gen.fam_bnd(n(140))
    if want("irq"):
        gen.fam_irq(max(1, int(round(args.scale))))
    if want("tail"):
        gen.fam_tail(n(4))

    problems = spec_mnemonic_check(gen, args.spec)
    if problems:
        print("encoder/spec decoder disagreement:\n  " + "\n  ".join(problems[:40]), file=sys.stderr)
        return 3
    unemittable = sorted({f"{w:08X}" for a, r in gen.rows.items() for w in r["words"] if EC.emitter_class(w) is None})
    if unemittable:
        print("rows contain words the emitter catalogue rejects: " + " ".join(unemittable[:20]), file=sys.stderr)
        return 3

    dol = out / "diff.dol"
    write_dol(dol, gen)
    with (out / "functions.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("addr_hex", "addr", "name", "size", "ins", "tags", "strings", "callees"))
        for address in sorted(gen.rows):
            row = gen.rows[address]
            writer.writerow((hex(address), address, f"{row['name']}_{address:08X}", len(row["words"]) * 4,
                             len(row["words"]), "", "", ""))
    command = [str(args.generator.resolve()), "--dol", str(dol), "--functions", str(out / "functions.csv"),
               "--out", str(out), "--shards", str(args.shards), "--report", str(out / "recomp_report.txt")]
    result = subprocess.run(command, capture_output=True, text=True)
    (out / "ppcrecomp.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
        return result.returncode
    functions, ranges = GV.parse_generated(out)
    rejected = sorted(a for a in gen.rows if a not in functions)
    report = (out / "recomp_report.txt").read_text(encoding="utf-8")
    if rejected:
        print("ppcrecomp rejected rows the harness expects to be translated:", file=sys.stderr)
        for a in rejected[:40]:
            print(f"  {a:08X} {gen.rows[a]['name']} {' '.join(f'{w:08X}' for w in gen.rows[a]['words'])}",
                  file=sys.stderr)
        print(report[:4000], file=sys.stderr)
        return 4

    def covered(address):
        return any(lo <= address < hi for lo, hi in ranges)

    bad = [v["id"] for v in gen.vectors if v["entry"] not in functions and not covered(v["entry"])]
    if bad:
        print("vector entries with no dispatch path: " + ", ".join(bad[:20]), file=sys.stderr)
        return 5
    for i in range(args.shards):
        if not (out / f"recomp_{i:03d}.cpp").exists():
            print(f"missing generated shard {i}", file=sys.stderr)
            return 6
    with (out / "vectors.jsonl").open("w", encoding="utf-8") as stream:
        for v in gen.vectors:
            record = dict(v)
            record["row_words"] = {f"{a:08X}": gen.rows[a]["words"] for a in v["rows"]}
            record["entry_declared"] = v["entry"] in functions
            stream.write(json.dumps(record, sort_keys=True) + "\n")
    write_driver_vectors(out / "vectors.txt", gen)
    families = {}
    for v in gen.vectors:
        key = f"{v['family']}/{v['spec']}/{v.get('policy', 'project')}"
        families[key] = families.get(key, 0) + 1
    manifest = {"schema": SCHEMA, "generator": str(args.generator.resolve()),
                "generator_sha256": hashlib.sha256(args.generator.read_bytes()).hexdigest(),
                "generator_command": command, "seed": args.seed, "scale": args.scale, "shards": args.shards,
                "rows": len(gen.rows), "vectors": len(gen.vectors), "families": families,
                "code_bytes": gen.next_slot - GV.CODE_BASE,
                "declared_entries": sum(1 for v in gen.vectors if v["entry"] in functions), "report": report}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    print(f"difftest F1b: {len(gen.vectors)} vectors over {len(gen.rows)} rows, {args.shards} shards -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
