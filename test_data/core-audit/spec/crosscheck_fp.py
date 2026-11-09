"""Differential cross-check of the two independent "fp" (scalar floating point) specification models.

    python crosscheck_fp.py [--n N] [--seed S] [--jobs J] [--models fp_A,fp_B] [--show K] [--no-edge]
                            [--only NAME[,NAME]]

Last results (2026-09-14, after the CROSSCHECK FIX edits recorded in fp.py):
    fp_A vs fp_B : decode 10,240 + edge 150,706 + random 250,000 = 410,946 vectors, 0 disagreements
    fp_B vs fp   : decode 10,240 + edge 150,706 + random 200,000 = 360,946 vectors, 0 disagreements (--seed 777)

Draws N random execution vectors plus a deterministic edge sweep for every scalar FP instruction both
models support (8 D-form and 8 indexed loads/stores, 10 primary-59 and 11 primary-63 A-form
instructions, 15 primary-63 X-form instructions), with clean and reserved-bit-dirty encodings, Rc=0/1,
aliasing register indices (D==A==B==C and every pairwise alias), boundary operands (+-0, +-inf,
QNaN/SNaN with assorted payloads and signs, single-width and double-width NaN payloads, largest and
smallest normal and denormal of both formats, rounding ties, 1+-ulp, single overflow/underflow
thresholds, integer-conversion boundaries, fused multiply-add cancellations, near-overflow and
near-underflow products/quotients), single-representable and non-single double images, every RN
mode, every FPSCR enable bit, NI, dirty sticky bits, MSR[FP/FE0/FE1/IP/ILE/LE], HID2[PSE],
aligned/misaligned/wrapping EAs with preloaded memory, and runs both models from identical cloned
states comparing
  - the outcome class: ok / partial (some locations undefined) / invalid (whole outcome undefined,
    state untouched) / error (a Python exception other than UndefinedBehaviour)
  - the normalised map of undefined locations with bit masks, and whether a bounded-result
    constraint (fres/frsqrte estimate) is attached
  - the full State.arch_dict(), mem.log, mem.bytes and pending, undefined bits masked in both
  - A's executable bounded-result predicates evaluated on B's representative state
It also sweeps the opcode space of primaries 31/48-55/59/63 comparing supports().

FPSCR pre-states always carry FEX and VX consistent with "the usual rule" (PAGE 473): every
FPSCR-altering instruction recomputes them, so an inconsistent summary is unreachable.

UB contracts being normalised:
  fp_A (and the reconciled fp.py): step() raises UndefinedBehaviour WITHOUT touching the state;
       .undefined == {'all': True} -> invalid; else .apply_defined(state) applies the defined bits,
       .undefined maps 'fpr[n].psK' / 'gpr[n]' / REG / 'mem[i]' (i-th memory op) / 'mem' /
       'exception' -> mask | True, and .checks are predicates over the post-state.
  fp_B: .applied False -> invalid (state untouched); True -> state holds one representative outcome,
       .fpr_masks {(reg, slot): mask}, .fpscr_mask, .cr_mask, .mem_masks [(addr, width, mask)],
       .other [whole-register names], .bounds [text].
"""
from __future__ import annotations

import argparse
import collections
import importlib
import multiprocessing as mp
import os
import random
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from gekko_state import MASK32, MASK64, State  # noqa: E402

M = MASK32
SIGN = 1 << 63


def _b(i):
    return 1 << (31 - i)


# ------------------------------------------------------------------------------------------
# constants written here from the manual, independently of both models
# ------------------------------------------------------------------------------------------
POW, ILE, EE, PR, FP, ME = _b(13), _b(15), _b(16), _b(17), _b(18), _b(19)       # PAGE 164-165
FE0, SE, BE, FE1, IP, IR, DR, PM, RI, LE = _b(20), _b(21), _b(22), _b(23), _b(25), _b(26), _b(27), \
    _b(29), _b(30), _b(31)
PSE = _b(2)                                                                         # PAGE 67
F = {n: _b(i) for i, n in enumerate(["FX", "FEX", "VX", "OX", "UX", "ZX", "XX", "VXSNAN", "VXISI",
                                     "VXIDI", "VXZDZ", "VXIMZ", "VXVC", "FR", "FI", "C", "FL", "FG",
                                     "FE", "FU", "R20", "VXSOFT", "VXSQRT", "VXCVI", "VE", "OE",
                                     "UE", "ZE", "XE", "NI", "RN0", "RN1"])}
VX_ALL = sum(F[n] for n in ("VXSNAN", "VXISI", "VXIDI", "VXZDZ", "VXIMZ", "VXVC", "VXSOFT", "VXSQRT",
                            "VXCVI"))


def usual_rule(f):
    f &= ~(F["VX"] | F["FEX"])
    if f & VX_ALL:
        f |= F["VX"]
    for x, e in (("VX", "VE"), ("OX", "OE"), ("UX", "UE"), ("ZX", "ZE"), ("XX", "XE")):
        if f & F[x] and f & F[e]:
            f |= F["FEX"]
    return f & M


# name -> (primary, form, xo, reserved ranges)   instruction pages PAGE 394-474, 430-437, 546-554
CAT = {
    "lfs": (48, "d", None, ()), "lfsu": (49, "d", None, ()), "lfd": (50, "d", None, ()),
    "lfdu": (51, "d", None, ()), "stfs": (52, "d", None, ()), "stfsu": (53, "d", None, ()),
    "stfd": (54, "d", None, ()), "stfdu": (55, "d", None, ()),
    "lfsx": (31, "x", 535, ()), "lfsux": (31, "x", 567, ()), "lfdx": (31, "x", 599, ()),
    "lfdux": (31, "x", 631, ()), "stfsx": (31, "x", 663, ()), "stfsux": (31, "x", 695, ()),
    "stfdx": (31, "x", 727, ()), "stfdux": (31, "x", 759, ()),
    "fdivs": (59, "a", 18, ((21, 25),)), "fsubs": (59, "a", 20, ((21, 25),)),
    "fadds": (59, "a", 21, ((21, 25),)), "fsqrts": (59, "a", 22, ((11, 15), (21, 25))),
    "fres": (59, "a", 24, ((11, 15), (21, 25))), "fmuls": (59, "a", 25, ((16, 20),)),
    "fmsubs": (59, "a", 28, ()), "fmadds": (59, "a", 29, ()), "fnmsubs": (59, "a", 30, ()),
    "fnmadds": (59, "a", 31, ()),
    "fdiv": (63, "a", 18, ((21, 25),)), "fsub": (63, "a", 20, ((21, 25),)),
    "fadd": (63, "a", 21, ((21, 25),)), "fsqrt": (63, "a", 22, ((11, 15), (21, 25))),
    "fsel": (63, "a", 23, ()), "fmul": (63, "a", 25, ((16, 20),)),
    "frsqrte": (63, "a", 26, ((11, 15), (21, 25))), "fmsub": (63, "a", 28, ()),
    "fmadd": (63, "a", 29, ()), "fnmsub": (63, "a", 30, ()), "fnmadd": (63, "a", 31, ()),
    "fcmpu": (63, "x", 0, ((9, 10), (31, 31))), "frsp": (63, "x", 12, ((11, 15),)),
    "fctiw": (63, "x", 14, ((11, 15),)), "fctiwz": (63, "x", 15, ((11, 15),)),
    "fcmpo": (63, "x", 32, ((9, 10), (31, 31))), "mtfsb1": (63, "x", 38, ((11, 20),)),
    "fneg": (63, "x", 40, ((11, 15),)), "mcrfs": (63, "x", 64, ((9, 10), (14, 20), (31, 31))),
    "mtfsb0": (63, "x", 70, ((11, 20),)), "fmr": (63, "x", 72, ((11, 15),)),
    "mtfsfi": (63, "x", 134, ((9, 15), (20, 20))), "fnabs": (63, "x", 136, ((11, 15),)),
    "fabs": (63, "x", 264, ((11, 15),)), "mffs": (63, "x", 583, ((11, 20),)),
    "mtfsf": (63, "x", 711, ((6, 6), (15, 15))),
}
NAMES = sorted(CAT)
SINGLE_INSTR = {"fdivs", "fsubs", "fadds", "fmuls", "fmsubs", "fmadds", "fnmsubs", "fnmadds", "fres"}
ARITH2 = {"fdivs", "fsubs", "fadds", "fmuls", "fdiv", "fsub", "fadd", "fmul"}
ARITH3 = {"fmsubs", "fmadds", "fnmsubs", "fnmadds", "fmsub", "fmadd", "fnmsub", "fnmadd"}
UNARY = {"fres", "frsqrte", "frsp", "fctiw", "fctiwz", "fneg", "fmr", "fnabs", "fabs"}
LOADS = {"lfs", "lfsu", "lfd", "lfdu", "lfsx", "lfsux", "lfdx", "lfdux"}
STORES = {"stfs", "stfsu", "stfd", "stfdu", "stfsx", "stfsux", "stfdx", "stfdux"}
FPSCR_OPS = {"mtfsb0", "mtfsb1", "mtfsf", "mtfsfi", "mcrfs", "mffs"}


def _setbits(w, a, b, v):
    width = b - a + 1
    sh = 31 - b
    mask = ((1 << width) - 1) << sh
    return (w & ~mask & M) | ((v << sh) & mask)


def _getbits(w, a, b):
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


# ------------------------------------------------------------------------------------------
# operand pools (binary64 images)
# ------------------------------------------------------------------------------------------
def s2d(w):
    """binary32 bits -> binary64 image, bit exact (signalling NaNs kept, denormals normalised)."""
    s, ex, fr = (w >> 31) & 1, (w >> 23) & 0xFF, w & 0x7FFFFF
    if ex == 0xFF:
        return (s << 63) | (0x7FF << 52) | (fr << 29)
    if ex == 0:
        if fr == 0:
            return s << 63
        sh = 24 - fr.bit_length()
        e = -126 - sh
        mant = (fr << (53 - fr.bit_length())) & ((1 << 52) - 1)
        return (s << 63) | ((e + 1023) << 52) | mant
    return (s << 63) | ((ex - 127 + 1023) << 52) | (fr << 29)


def f2s(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def d(x):
    return struct.unpack(">Q", struct.pack(">d", x))[0]


SINGLE_EDGE = [
    0x00000000, 0x80000000, 0x7F800000, 0xFF800000,
    0x7FC00000, 0xFFC00000, 0x7FC00001, 0xFFFFFFFF, 0x7FE12345,
    0x7F800001, 0xFF800001, 0x7FA00000, 0xFFBFFFFF, 0x7F812345,
    0x7F7FFFFF, 0xFF7FFFFF, 0x7F7FFFFE, 0x7F000000, 0x7E800000,
    0x00800000, 0x80800000, 0x00800001, 0x007FFFFF, 0x807FFFFF,
    0x00000001, 0x80000001, 0x00000002, 0x00000003, 0x00400000, 0x00012345, 0x00400001,
    f2s(1.0), f2s(-1.0), f2s(2.0), f2s(0.5), f2s(3.0), f2s(-3.0), f2s(1.5), f2s(0.25),
    0x3F800001, 0x3F7FFFFF, 0xBF800001, 0x33800000, 0x34000000, 0x33FFFFFF,
    f2s(16777216.0), f2s(16777215.0), f2s(0.1), f2s(-0.1), f2s(1.0 / 3.0),
    f2s(2.5), f2s(-2.5), f2s(-0.5), f2s(2147483648.0), f2s(-2147483648.0), f2s(2147483520.0),
    f2s(4294967296.0), f2s(2.0 ** 100), f2s(2.0 ** -100), 0x5F800000, 0x1F800000, 0x20000000,
    0x60000000, 0x5FFFFFFF, 0x20000001, 0x1FFFFFFF, 0x3FB504F3, 0x5F3504F3,
]
DOUBLE_EDGE = [
    0, SIGN, 0x7FF0000000000000, 0xFFF0000000000000,
    0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000000000001, 0x7FFFFFFFFFFFFFFF, 0xFFFC000000000000,
    0x7FF8000020000000, 0x7FF0000000000001, 0xFFF0000000000001, 0x7FF0000020000000,
    0x7FF4000000000000, 0xFFF7FFFFFFFFFFFF, 0x7FF00000E0000000, 0x7FF0000010000000,
    0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF, 0x7FE0000000000000, 0x0010000000000000,
    0x000FFFFFFFFFFFFF, 1, 0x8000000000000001, 0x0008000000000000, 3, 0x800FFFFFFFFFFFFF,
    d(1.0), d(-1.0), d(2.0), d(0.5), d(3.0), d(-3.0), d(1 / 3), d(0.1), 0x3FF0000000000001,
    0x3FEFFFFFFFFFFFFF, 0x3CA0000000000000, 0x3CB0000000000000, 0xBFF0000000000001,
    0x47EFFFFFE0000000, 0x47EFFFFFF0000000, 0x47EFFFFFEFFFFFFF, 0x47F0000000000000,
    0x47E0000000000000, 0xC7EFFFFFF0000000,
    0x3810000000000000, 0x380FFFFFF0000000, 0x380FFFFFE0000000, 0x3800000000000000,
    0x36A0000000000000, 0x3690000000000000, 0x36A8000000000000, 0x3698000000000000,
    0x36A0000000000001, 0x3680000000000000, 0x3670000000000000, 0x3660000000000000,
    0x37F0000000000000, 0xB80FFFFFF0000000, 0x3810000010000000,
    d(2.0 ** 31), d(2.0 ** 31 - 1), d(2147483647.5), d(-2.0 ** 31), d(-2.0 ** 31 - 0.5),
    d(-2.0 ** 31 - 1), d(2.5), d(-2.5), d(1.5), d(-0.5), d(0.5000000000000001),
    0x3FDFFFFFFFFFFFFF, d(4294967296.0), d(2.0 ** 52), d(-3.5), d(2147483646.5), d(-2147483648.5),
    0x7E70000000000000, 0x0170000000000000, 0x1A70000000000000, 0x6570000000000000,
    0x5FF0000000000000, 0x2000000000000000, 0x2010000000000000, 0x5FE0000000000000,
    0x0000000000000010, 0x0010000000000001,
]
ALL_EDGE = sorted(set(DOUBLE_EDGE) | {s2d(w) for w in SINGLE_EDGE})
SINGLE_POOL = [s2d(w) for w in SINGLE_EDGE]


def rand_double(r):
    k = r.random()
    if k < 0.4:
        x = r.choice(DOUBLE_EDGE)
        if r.random() < 0.1:
            x ^= 1 << r.randrange(64)
        return x
    if k < 0.55:
        ex = r.choice([0, 0, 1, 2, 0x7FE, 0x7FD, 0x7FF, 0x3FF, 0x400, 0x3FE, 874, 873, 896, 897, 0x380,
                       0x47F, 0x47E, 0x480, 0x41D, 0x41E, 0x41F, 0x200, 0x600, 1, 1150, 1151])
        return (r.getrandbits(1) << 63) | (ex << 52) | r.choice([0, 1, (1 << 52) - 1, 1 << 51, 1 << 29,
                                                                 r.getrandbits(52)])
    if k < 0.7:
        v = r.randrange(-70000, 70000) / r.choice([1, 1, 2, 4, 8, 256, 65536, 3, 10])
        return d(v)
    return r.getrandbits(64)


def rand_single(r):
    k = r.random()
    if k < 0.45:
        w = r.choice(SINGLE_EDGE)
        if r.random() < 0.15:
            w ^= 1 << r.randrange(32)
        return s2d(w)
    if k < 0.6:
        ex = r.choice([0, 0, 1, 2, 0xFE, 0xFD, 0xFF, 0x7F, 0x80, 0x7E, 0x60, 0x20, 0x96, 0x9E, 0x9F])
        return s2d((r.getrandbits(1) << 31) | (ex << 23) | r.choice([0, 1, 0x7FFFFF, 0x400000,
                                                                     r.getrandbits(23)]))
    if k < 0.75:
        v = r.randrange(-70000, 70000) / r.choice([1, 1, 2, 4, 8, 256, 65536, 3])
        return s2d(f2s(v))
    if k < 0.8:
        return rand_double(r)
    return s2d(r.getrandbits(32))


def rand_operand(r, single):
    if single:
        return rand_single(r) if r.random() < 0.9 else rand_double(r)
    return rand_double(r) if r.random() < 0.65 else rand_single(r)


# ------------------------------------------------------------------------------------------
# word / state generation
# ------------------------------------------------------------------------------------------
def rand_reg(r):
    return r.choice((0, 1, 2, 3)) if r.random() < 0.6 else r.randrange(32)


D_EDGE = [0, 4, 8, -4, -8, 1, 2, 3, 0x7FFC, -0x8000, 0x7FFF, 5, 12, -1]


def gen_word(r, name):
    op, form, xo, reserved = CAT[name]
    dirty = r.random() < 0.07
    w = (op << 26) | r.getrandbits(26)
    if form == "d":
        w = _setbits(w, 6, 10, rand_reg(r))
        w = _setbits(w, 11, 15, rand_reg(r))
        if r.random() < 0.6:
            w = _setbits(w, 16, 31, r.choice(D_EDGE) & 0xFFFF)
    else:
        for a, b in ((6, 10), (11, 15), (16, 20), (21, 25)):
            w = _setbits(w, a, b, rand_reg(r))
        if form == "a":
            w = _setbits(w, 26, 30, xo)
        else:
            w = _setbits(w, 21, 30, xo)
            if name == "x31_rc_clean" or (op == 31 and r.random() < 0.85):
                w = _setbits(w, 31, 31, 0)
        if name in ("fcmpu", "fcmpo", "mcrfs", "mtfsfi"):
            w = _setbits(w, 6, 8, r.randrange(8))
        if name == "mcrfs":
            w = _setbits(w, 11, 13, r.randrange(8))
        if name in ("mtfsb0", "mtfsb1"):
            w = _setbits(w, 6, 10, r.randrange(32))
    if not dirty:
        for a, b in reserved:
            w = _setbits(w, a, b, 0)
    return w & M


EA_EDGE = [0x80001000, 0x80001004, 0x80001001, 0x80001002, 0x80001003, 0x00000000, 0xFFFFFFFC,
           0xFFFFFFFE, 0xFFFFFFFF, 0x7FFFFFFC, 0x00000800, 0x80000000, 0xCC008000, 0x0000FFF0,
           0xFFFFFFF8]


def rand_fpscr(r):
    k = r.random()
    if k < 0.25:
        f = 0
    elif k < 0.6:
        f = 0
        for n in ("VE", "OE", "UE", "ZE", "XE"):
            if r.random() < 0.3:
                f |= F[n]
        if r.random() < 0.3:
            f |= r.getrandbits(32) & 0xFFFFF800      # dirty sticky/status bits
    else:
        f = r.getrandbits(32)
        if r.random() < 0.7:
            f &= ~F["NI"]
    if r.random() < 0.12:
        f |= F["NI"]
    return usual_rule((f & ~3) | r.randrange(4))


def rand_msr(r):
    msr = FP if r.random() < 0.93 else 0
    for bit, p in ((FE0, 0.15), (FE1, 0.15), (IP, 0.3), (ILE, 0.2), (LE, 0.04), (EE, 0.5), (ME, 0.5),
                   (IR, 0.5), (DR, 0.5), (RI, 0.5), (PR, 0.3), (SE, 0.05), (BE, 0.05), (PM, 0.1),
                   (POW, 0.03)):
        if r.random() < p:
            msr |= bit
    if r.random() < 0.04:
        msr |= _b(r.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 24, 28]))
    return msr


def _product_pair(r, single):
    """two operands whose product/quotient lands near an overflow/underflow/tininess threshold."""
    P, EMIN, EMAX = (24, -126, 127) if single else (53, -1022, 1023)
    target = r.choice([EMAX, EMAX + 1, EMIN, EMIN - 1, EMIN - P + 1, EMIN - P, EMIN - P - 1, 0,
                       EMAX - 1, EMIN + 1])
    ea = r.randint(max(-1022, target - 1023), min(1023, target + 1022)) if not single else \
        r.randint(max(-149, target - 127), min(127, target + 149))
    eb = target - ea
    ma = r.choice([1.0, 1.5, 1.9999999, 1.25, 1.0000001, 1.75])
    mb = r.choice([1.0, 1.5, 1.3333333333333333, 1.9999999, 0.75])
    try:
        a = struct.unpack(">Q", struct.pack(">d", ma * 2.0 ** ea))[0]
        b = struct.unpack(">Q", struct.pack(">d", mb * 2.0 ** eb))[0]
    except OverflowError:
        return rand_operand(r, single), rand_operand(r, single)
    if single:
        try:
            a, b = s2d(f2s(ma * 2.0 ** ea)), s2d(f2s(mb * 2.0 ** eb))
        except OverflowError:
            pass
    return a | (r.getrandbits(1) << 63), b | (r.getrandbits(1) << 63)


def gen_state(r, name, w):
    st = State()
    st.msr = rand_msr(r)
    st.hid2 = (PSE if r.random() < 0.75 else 0) | (r.getrandbits(32) & ~PSE if r.random() < 0.2 else 0xC0000000 & ~PSE)
    st.hid0 = r.getrandbits(32) if r.random() < 0.2 else 0
    st.fpscr = rand_fpscr(r)
    st.cr = r.getrandbits(32)
    st.xer = r.getrandbits(32) if r.random() < 0.3 else 0
    st.pc = r.choice((0x80003100, 0x80003100, 0xFFFFFFFC, 0x00000700))
    st.srr0, st.srr1 = r.getrandbits(32), r.getrandbits(32)
    st.dar, st.dsisr = r.getrandbits(32), r.getrandbits(32)
    st.lr, st.ctr = r.getrandbits(32), r.getrandbits(32)
    st.gqr = [r.getrandbits(32) for _ in range(8)]
    for i in range(32):
        st.gpr[i] = r.choice(EA_EDGE) if r.random() < 0.5 else r.getrandbits(32)
    if r.random() < 0.6:
        st.gpr[_getbits(w, 16, 20)] = r.choice((0, 4, 1, 2, 3, 8, 0xFFFFFFFC, 0x10, 0xFFFFFFFF, 12))
    single = name in SINGLE_INSTR
    for i in range(32):
        st.fpr[i] = [rand_operand(r, single), rand_operand(r, single)]
    A, B, C, D = _getbits(w, 11, 15), _getbits(w, 16, 20), _getbits(w, 21, 25), _getbits(w, 6, 10)
    if (name in ARITH2 or name in ARITH3) and r.random() < 0.45:
        k = r.random()
        if k < 0.35:
            x, y = _product_pair(r, single)
            st.fpr[A][0] = x
            st.fpr[C if name.startswith("fmul") or name in ARITH3 else B][0] = y
            if name in ARITH3 and r.random() < 0.6:
                try:
                    fa = struct.unpack(">d", struct.pack(">Q", x))[0]
                    fc = struct.unpack(">d", struct.pack(">Q", y))[0]
                    p = -fa * fc if name in ("fmadd", "fmadds", "fnmadd", "fnmadds") else fa * fc
                    if single:
                        p = struct.unpack(">f", struct.pack(">f", p))[0]
                    bb = d(p)
                    if r.random() < 0.5:
                        bb ^= 1 << r.randrange(3)
                    st.fpr[B][0] = s2d(f2s(p)) if single else bb
                except (OverflowError, struct.error):
                    pass
        elif k < 0.7:
            pick = r.choice(SINGLE_POOL if single else ALL_EDGE)
            for reg in (A, B, C):
                x = pick
                if r.random() < 0.5:
                    x ^= (1 << r.randrange(3)) << (29 if single else 0)
                if r.random() < 0.5:
                    x ^= SIGN
                st.fpr[reg][0] = x
        else:
            for reg in (A, B, C):
                st.fpr[reg][0] = r.choice(SINGLE_POOL if single else ALL_EDGE)
    if name in ("fctiw", "fctiwz") and r.random() < 0.4:
        st.fpr[B][0] = d(r.choice([2.0 ** 31, 2.0 ** 31 - 1, -2.0 ** 31, -2.0 ** 31 - 1]) +
                         r.choice([0, 0.5, -0.5, 0.25, 0.75, -0.25, 1e-7, -1e-7, 0.4999999999]))
    st.exception = r.choice((None, None, "program"))
    st.reserve = r.random() < 0.2
    st.reserve_addr = r.getrandbits(32)
    if name in LOADS or name in STORES:
        a, b = _getbits(w, 11, 15), _getbits(w, 16, 20)
        dd = _getbits(w, 16, 31)
        dd = dd - 0x10000 if dd & 0x8000 else dd
        cands = {((st.gpr[a] if a else 0) + st.gpr[b]) & M, (st.gpr[a] + st.gpr[b]) & M,
                 ((st.gpr[a] if a else 0) + dd) & M, (st.gpr[a] + dd) & M}
        for ea in cands:
            kind = r.random()
            for off in range(-8, 16, 4):
                if kind < 0.45:
                    word = r.choice(SINGLE_EDGE)
                elif kind < 0.7:
                    word = r.choice(DOUBLE_EDGE) >> r.choice((0, 32))
                else:
                    word = r.getrandbits(32)
                st.mem.load((ea + off) & M, (word & M).to_bytes(4, "big"))
    return st


# ------------------------------------------------------------------------------------------
# normalisation of the two UB contracts
# ------------------------------------------------------------------------------------------
REG32 = ("cr", "xer", "lr", "ctr", "msr", "fpscr", "srr0", "srr1", "dar", "dsisr", "dec", "hid0", "hid2", "pc")


def _add(out, key, mask):
    if mask is True or out.get(key) is True:
        out[key] = True
    else:
        out[key] = out.get(key, 0) | mask


def _mem_bytes(out, addr, width, mask):
    for i in range(width):
        bm = (mask >> (8 * (width - 1 - i))) & 0xFF
        if bm:
            _add(out, "mem[%08X]" % ((addr + i) & M), bm)


def norm_A(u):
    out = {}
    mem = u.defined.get("mem", ())
    for loc, m in u.undefined.items():
        if loc == "all":
            return None
        mm = re.fullmatch(r"fpr\[(\d+)\]\.ps(\d)", loc)
        if mm:
            _add(out, "fpr[%s][%s]" % (mm.group(1), mm.group(2)), MASK64 if m is True else m)
            continue
        mm = re.fullmatch(r"mem\[(\d+)\]", loc)
        if mm:
            _, addr, width, _, _ = mem[int(mm.group(1))]
            _mem_bytes(out, addr, width, (1 << (8 * width)) - 1 if m is True else m)
            continue
        if re.fullmatch(r"gpr\[\d+\]", loc):
            _add(out, loc, M if m is True else m)
        elif loc in REG32:
            _add(out, loc, M if m is True else m)
        elif loc in ("exception", "mem"):
            _add(out, loc, True)
        else:
            _add(out, "?" + loc, True)
    if u.checks:
        out["bounded"] = True
    return {k: v for k, v in out.items() if v}


def norm_B(u):
    out = {}
    for (reg, slot), m in u.fpr_masks.items():
        _add(out, "fpr[%d][%d]" % (reg, slot), m & MASK64)
    _add(out, "fpscr", u.fpscr_mask & M)
    _add(out, "cr", u.cr_mask & M)
    for addr, width, m in u.mem_masks:
        _mem_bytes(out, addr, width, m)
    for name, m in getattr(u, "reg_masks", {}).items():
        _add(out, name, m & M)
    for name in u.other:
        if name in REG32:
            _add(out, name, M)
        else:
            _add(out, "?" + name, True)
    if u.bounds:
        out["bounded"] = True
    return {k: v for k, v in out.items() if v}


def run_model(mod, st, w):
    """-> (outcome, normalised undefined map, message, checks); st holds the (defined) result."""
    try:
        mod.step(st, w)
        return ("ok", {}, "", ())
    except mod.UndefinedBehaviour as u:
        if hasattr(u, "applied"):                        # B contract
            if not u.applied:
                return ("invalid", {}, u.what, ())
            return ("partial", norm_B(u), u.what, ())
        und = norm_A(u)
        if und is None:
            return ("invalid", {}, u.what, ())
        u.apply_defined(st)
        return ("partial", und, u.what, tuple(u.checks))
    except Exception as e:  # noqa: BLE001
        import traceback
        return ("error", {}, "%s: %s | %s" % (type(e).__name__, e, traceback.format_exc()[-600:]), ())


def snapshot(st, und=None):
    dd = st.arch_dict()
    dd = {k: ([list(p) for p in v] if k == "fpr" else (list(v) if isinstance(v, list) else v)) for k, v in dd.items()}
    dd["mem.log"] = list(st.mem.log)
    dd["mem.bytes"] = dict(st.mem.bytes)
    dd["pending"] = dict(st.pending)
    und = und or {}
    for loc, v in und.items():
        m = re.fullmatch(r"fpr\[(\d+)\]\[(\d)\]", loc)
        if m:
            dd["fpr"][int(m.group(1))][int(m.group(2))] &= ~(MASK64 if v is True else v) & MASK64
            continue
        m = re.fullmatch(r"gpr\[(\d+)\]", loc)
        if m:
            dd["gpr"][int(m.group(1))] &= ~(M if v is True else v) & M
            continue
        if loc in REG32:
            dd[loc] &= ~(M if v is True else v) & M
        elif loc == "exception":
            dd["exception"] = "?"
        elif loc == "mem":
            dd["mem.log"] = "?"
            dd["mem.bytes"] = "?"
        m = re.fullmatch(r"mem\[([0-9A-F]{8})\]", loc)
        if m and dd["mem.bytes"] != "?":
            a = int(m.group(1), 16)
            dd["mem.bytes"][a] = dd["mem.bytes"].get(a, 0) & ~v & 0xFF
    if dd["mem.log"] != "?" and any(k.startswith("mem[") for k in und):
        newlog = []
        for kind, addr, width, val in dd["mem.log"]:
            if kind == "w":
                for i in range(width):
                    mk = und.get("mem[%08X]" % ((addr + i) & M))
                    if mk:
                        val &= ~(mk << (8 * (width - 1 - i)))
            newlog.append((kind, addr, width, val))
        dd["mem.log"] = newlog
    if dd["mem.bytes"] != "?":
        dd["mem.bytes"] = {k: v for k, v in dd["mem.bytes"].items() if v}
    return dd


def diff_fields(a, b):
    out = []
    for k in a:
        if a[k] != b[k]:
            if k in ("gpr", "gqr", "sprg"):
                out.extend("%s[%d]" % (k, i) for i in range(len(a[k])) if a[k][i] != b[k][i])
            elif k == "fpr":
                out.extend("fpr[%d][%d]" % (i, j) for i in range(32) for j in (0, 1) if a[k][i][j] != b[k][i][j])
            elif k in REG32:
                out.append("%s^%08X" % (k, a[k] ^ b[k]))
            else:
                out.append(k)
    return out


def compare(mA, mB, base, w):
    pre = snapshot(base)
    sA, sB = base.clone(), base.clone()
    oA = run_model(mA, sA, w)
    oB = run_model(mB, sB, w)
    problems = []
    if oA[0] != oB[0]:
        problems.append(("outcome", oA[0], oB[0]))
    elif oA[0] == "error":
        problems.append(("error",))
    else:
        for tag, o in (("A", oA), ("B", oB)):
            if o[2] and "PAGE" not in o[2]:
                problems.append((tag + "-reason-without-page",))
        if oA[0] == "invalid":
            if snapshot(sA) != pre:
                problems.append(("A-modified-on-invalid", tuple(diff_fields(pre, snapshot(sA)))))
            if snapshot(sB) != pre:
                problems.append(("B-modified-on-invalid", tuple(diff_fields(pre, snapshot(sB)))))
        else:
            if oA[1] != oB[1]:
                keys = sorted(set(oA[1]) | set(oB[1]))
                problems.append(("undefined-set", tuple((k, _fmt(oA[1].get(k)), _fmt(oB[1].get(k)))
                                                        for k in keys if oA[1].get(k) != oB[1].get(k))))
            und = {}
            for src in (oA[1], oB[1]):
                for k, v in src.items():
                    if k != "bounded":
                        _add(und, k, v)
            dA, dB = snapshot(sA, und), snapshot(sB, und)
            if dA != dB:
                problems.append(("state", tuple(diff_fields(dA, dB))))
            if oA[3]:
                try:
                    okc = all(c(sB) for c in oA[3])
                except Exception as e:  # noqa: BLE001
                    okc = False
                if not okc:
                    problems.append(("A-bound-rejects-B-representative",))
    return oA, oB, problems, sA, sB, pre


def _fmt(v):
    if v is None or v is True:
        return v
    return "%X" % v


FPSCR_NAMES = [n for n in F]


def _bitnames(mask):
    return [n for n in FPSCR_NAMES if mask & F[n]]


def _feature(base, w, name):
    tags = []
    if not base.hid2 & PSE:
        tags.append("PSE0")
    if not base.msr & FP:
        tags.append("FP0")
    if base.fpscr & F["NI"]:
        tags.append("NI")
    if base.msr & (FE0 | FE1):
        tags.append("FE")
    return tuple(tags)


def _coarse64(m):
    if m is True or m == MASK64:
        return "all"
    tags = []
    for nm, bits in (("sign", SIGN), ("hi32", 0x7FFFFFFF00000000), ("lo32", 0xFFFFFFFF)):
        if m & bits == bits:
            tags.append(nm)
            m &= ~bits
    if m:
        tags.append("bits")
    return "+".join(tags) or "-"


def _coarse_map(und, dreg):
    out = []
    for k, v in sorted(und.items()):
        fm = re.fullmatch(r"fpr\[(\d+)\]\[(\d)\]", k)
        if fm:
            out.append("fpr[%s][%s]=%s" % ("D" if int(fm.group(1)) == dreg else "r", fm.group(2), _coarse64(v)))
        elif k == "fpscr" and v is not True:
            out.append("fpscr=" + ",".join(_bitnames(v)))
        elif k.startswith("mem["):
            out.append("mem")
        elif k in REG32 and v is not True:
            out.append("%s=%08X" % (k, v))
        else:
            out.append(k)
    return tuple(sorted(set(out)))


def atoms(name, w, base, problems, oA=None, oB=None):
    """split a vector's problems into atomic, mechanism-level disagreement keys."""
    dreg = _getbits(w, 6, 10)
    out = []
    for p in problems:
        if p[0] == "outcome":
            out.append(("outcome", p[1], p[2], "A:" + ";".join(_coarse_map(oA[1], dreg)),
                        "B:" + ";".join(_coarse_map(oB[1], dreg))))
            continue
        if p[0] == "undefined-set":
            for k, a, b in p[1]:
                ma = 0 if a is None else (True if a is True else int(a, 16))
                mb = 0 if b is None else (True if b is True else int(b, 16))
                fm = re.fullmatch(r"fpr\[(\d+)\]\[(\d)\]", k)
                if fm:
                    reg = "D" if int(fm.group(1)) == dreg else "r"
                    x = (ma if ma is not True else MASK64) ^ (mb if mb is not True else MASK64)
                    out.append(("undef", "fpr[%s][%s]" % (reg, fm.group(2)),
                                "A:" + _coarse64((MASK64 if ma is True else ma) & x),
                                "B:" + _coarse64((MASK64 if mb is True else mb) & x)))
                elif k in ("fpscr", "cr") and ma is not True and mb is not True:
                    ia, ib = set(_bitnames(ma or 0)), set(_bitnames(mb or 0))
                    if k == "cr":
                        x = (ma or 0) ^ (mb or 0)
                        fa, fb = (ma or 0) & x, (mb or 0) & x
                        cf = lambda m: ",".join("CR%d" % i for i in range(8) if m & (0xF << (28 - 4 * i)))  # noqa: E731
                        out.append(("undef", "cr", "A:" + cf(fa), "B:" + cf(fb)))
                    else:
                        for n in sorted(ia ^ ib):
                            out.append(("undef", "fpscr:" + n, "A" if n in ia else "-", "B" if n in ib else "-"))
                elif k in REG32 and ma is not True and mb is not True:
                    x = (ma or 0) ^ (mb or 0)
                    out.append(("undef", k, "A:%08X" % ((ma or 0) & x), "B:%08X" % ((mb or 0) & x)))
                elif k.startswith("mem["):
                    out.append(("undef", "mem", "A:%s" % (a,), "B:%s" % (b,)))
                else:
                    out.append(("undef", k, "A" if ma else "-", "B" if mb else "-"))
        elif p[0] == "state":
            for x in p[1]:
                fm = re.fullmatch(r"fpr\[(\d+)\]\[(\d)\]", x)
                if fm:
                    x = "fpr[%s][%s]" % ("D" if int(fm.group(1)) == dreg else "r", fm.group(2))
                elif "^" in x:
                    reg, _, m = x.partition("^")
                    if reg == "fpscr":
                        x = "fpscr:" + ",".join(_bitnames(int(m, 16)))
                    elif reg == "cr":
                        x = "cr:" + ",".join("CR%d" % i for i in range(8) if int(m, 16) & (0xF << (28 - 4 * i)))
                    else:
                        x = reg + "^" + m
                out.append(("state", x))
        else:
            out.append(p[:1])
    return sorted(set(out))


def record(classes, counts, name, w, base, res):
    oA, oB, problems, sA, sB, pre = res
    serious = [p for p in problems if not p[0].endswith("reason-without-page")]
    if not problems:
        return
    feat = _feature(base, w, name)
    for at in atoms(name, w, base, problems, oA, oB):
        key = (name, at, feat)
        counts[key] += 1 if serious else 0
        if key not in classes:
            a, b, c, dd = _getbits(w, 11, 15), _getbits(w, 16, 20), _getbits(w, 21, 25), _getbits(w, 6, 10)
            classes[key] = {
                "word": "%08X" % w, "D,A,B,C": (dd, a, b, c),
                "pre": {"fpscr": "%08X" % base.fpscr, "msr": "%08X" % base.msr, "hid2": "%08X" % base.hid2,
                        "frA": "%016X" % base.fpr[a][0], "frB": "%016X" % base.fpr[b][0],
                        "frC": "%016X" % base.fpr[c][0], "frD": ["%016X" % x for x in base.fpr[dd]],
                        "gprA": "%08X" % base.gpr[a], "gprB": "%08X" % base.gpr[b], "cr": "%08X" % base.cr},
                "problems": problems,
                "A": (oA[0], {k: _fmt(v) for k, v in oA[1].items()}, oA[2][:700]),
                "B": (oB[0], {k: _fmt(v) for k, v in oB[1].items()}, oB[2][:700]),
                "diff": {f: (_val(sA, f), _val(sB, f)) for f in diff_fields(snapshot(sA), snapshot(sB))[:8]},
            }
    if serious:
        counts[("__vectors__", (), ())] += 1


def _val(st, f):
    m = re.fullmatch(r"fpr\[(\d+)\]\[(\d)\]", f)
    if m:
        return "%016X" % st.fpr[int(m.group(1))][int(m.group(2))]
    m = re.fullmatch(r"(gpr|gqr)\[(\d+)\]", f)
    if m:
        return "%08X" % getattr(st, m.group(1))[int(m.group(2))]
    base = f.split("^")[0]
    if base in REG32:
        return "%08X" % getattr(st, base)
    if f == "mem.log":
        return st.mem.log[-4:]
    if f == "mem.bytes":
        return len(st.mem.bytes)
    return getattr(st, f, None)


def load_model(spec):
    return importlib.import_module(spec)


# ------------------------------------------------------------------------------------------
# sweeps
# ------------------------------------------------------------------------------------------
def worker(args):
    seed, count, modA, modB, only = args
    mA, mB = load_model(modA), load_model(modB)
    r = random.Random(seed)
    names = only or NAMES
    classes, counts, outcomes = collections.OrderedDict(), collections.Counter(), collections.Counter()
    for _ in range(count):
        name = r.choice(names)
        w = gen_word(r, name) & M
        base = gen_state(r, name, w)
        res = compare(mA, mB, base, w)
        outcomes[(name, res[0][0])] += 1
        record(classes, counts, name, w, base, res)
    return classes, counts, outcomes


def _clean_state():
    st = State()
    st.msr = FP | ME | IR | DR | RI
    st.hid2 = 0xE0000000
    return st


FPSCR_MODES = [0, 1, 2, 3, F["VE"], F["OE"] | 2, F["UE"] | 3, F["ZE"], F["XE"] | 1, F["NI"],
               F["NI"] | F["UE"] | F["VE"], usual_rule(F["VE"] | F["OE"] | F["UE"] | F["ZE"] | F["XE"]),
               usual_rule(0xFFFFFFF8 & ~F["NI"]), usual_rule(F["XX"] | F["XE"] | 2),
               usual_rule(F["OX"] | F["UX"] | F["VXSNAN"] | F["FI"] | F["FR"] | 0x1F000 | 1)]


def edge_task(args):
    part, nparts, modA, modB, only = args
    mA, mB = load_model(modA), load_model(modB)
    classes, counts, outcomes = collections.OrderedDict(), collections.Counter(), collections.Counter()
    n = [0]
    idx = [0]

    def go(name, w, st):
        idx[0] += 1
        if idx[0] % nparts != part:
            return
        if only and name not in only:
            return
        n[0] += 1
        res = compare(mA, mB, st, w & M)
        outcomes[(name, res[0][0])] += 1
        record(classes, counts, name, w & M, st, res)

    r = random.Random(4242)
    for name in NAMES:
        op, form, xo, reserved = CAT[name]
        single = name in SINGLE_INSTR
        pool = SINGLE_POOL if single else ALL_EDGE
        if name in ARITH2 or name in ARITH3 or name in ("fsel", "fcmpu", "fcmpo"):
            for i, x in enumerate(pool):
                for j, y in enumerate(pool):
                    step = 2 if (name in ARITH2 or name in ("fcmpu", "fcmpo")) else 5
                    if (i * 7 + j * 3) % step:
                        continue
                    st = _clean_state()
                    st.fpscr = FPSCR_MODES[(i + 3 * j) % len(FPSCR_MODES)]
                    if (i ^ j) & 8:
                        st.msr |= FE0
                    if (i + j) % 7 == 3:
                        st.hid2 = 0xC0000000
                    z = pool[(i * 13 + j * 5) % len(pool)]
                    st.fpr[1] = [x, 0x1111111111111111]
                    st.fpr[2] = [y, 0x2222222222222222]
                    st.fpr[4] = [z, 0x4444444444444444]
                    st.fpr[3] = [0x3333333333333333, 0x3333333333333333]
                    alias = (i + 2 * j) % 11
                    D, A, B, C = 3, 1, 2, 4
                    if alias == 1:
                        D = A
                    elif alias == 2:
                        D = B
                    elif alias == 3:
                        A = B = C
                        D = A
                    elif alias == 4:
                        B = A
                    elif alias == 5:
                        C = B
                    if form == "a":
                        w = (op << 26) | (D << 21) | (A << 16) | (B << 11) | (C << 6) | (xo << 1) | ((i + j) & 1)
                        for a_, b_ in reserved:
                            w = _setbits(w, a_, b_, 0)
                    else:
                        w = (op << 26) | (((i + j) % 8) << 23) | (A << 16) | (B << 11) | (xo << 1)
                    go(name, w, st)
        elif name in UNARY:
            for i, x in enumerate(pool):
                for fm in FPSCR_MODES:
                    for pse in (True, False):
                        st = _clean_state()
                        st.fpscr = fm
                        if not pse:
                            st.hid2 = 0xC0000000
                        if fm & F["VE"] and i & 1:
                            st.msr |= FE1
                        st.fpr[2] = [x, 0x2222222222222222]
                        st.fpr[1] = [0x1111111111111111, 0x1111111111111111]
                        D = 2 if (i % 5 == 0) else 1
                        w = (op << 26) | (D << 21) | (2 << 11) | (xo << 1) | (i & 1)
                        go(name, w, st)
        elif name in LOADS or name in STORES:
            vals = SINGLE_EDGE + [x >> 32 for x in DOUBLE_EDGE[:40]]
            for vi, v32 in enumerate(vals):
                for ea in (0x80001000, 0x80001001, 0x80001002, 0x80001004, 0xFFFFFFFC, 0x00000000):
                    for pse in (True, False):
                        st = _clean_state()
                        if not pse:
                            st.hid2 = 0xC0000000
                        if vi % 9 == 4:
                            st.msr |= LE
                        if vi % 13 == 6:
                            st.msr &= ~FP
                        st.gpr[5] = ea
                        st.gpr[6] = 4 if vi & 1 else 0
                        st.gpr[7] = 0x80001000
                        st.mem.load(ea, v32.to_bytes(4, "big") + bytes([0x80, 0x01, 0xFF, 0x7F]))
                        st.mem.load((ea + 4) & M, (ALL_EDGE[vi % len(ALL_EDGE)] & M).to_bytes(4, "big"))
                        st.fpr[3] = [ALL_EDGE[(vi * 7) % len(ALL_EDGE)], 0x3333333333333333]
                        A = 5 if vi % 4 else (0 if name not in ("lfsu", "lfdu", "stfsu", "stfdu", "lfsux",
                                                                "lfdux", "stfsux", "stfdux") or vi % 8 == 0 else 5)
                        if form == "d":
                            disp = (0, 4, -4, 1)[vi % 4] if A else (ea & 0x7FFF)
                            w = (op << 26) | (3 << 21) | (A << 16) | (disp & 0xFFFF)
                        else:
                            w = (op << 26) | (3 << 21) | (A << 16) | (6 << 11) | (xo << 1) | (1 if vi % 11 == 5 else 0)
                        go(name, w, st)
        elif name in ("fsqrt", "fsqrts"):
            for msr in (0, FP, FP | IP | ILE, FP | FE0):
                for dirty in (0, 1):
                    st = _clean_state()
                    st.msr = msr
                    w = (op << 26) | (1 << 21) | (dirty << 16) | (2 << 11) | (xo << 1)
                    go(name, w, st)
        elif name in ("mtfsb0", "mtfsb1"):
            for bit in range(32):
                for fm in FPSCR_MODES + [usual_rule(F["VE"] | F["VXSNAN"]), usual_rule(F["OE"]),
                                         usual_rule(F["FX"] | F["OX"] | F["OE"])]:
                    for msr in (FP, FP | FE0, FP | FE1 | IP):
                        for rc in (0, 1):
                            st = _clean_state()
                            st.msr = msr
                            st.fpscr = fm
                            go(name, (op << 26) | (bit << 21) | (xo << 1) | rc, st)
        elif name == "mtfsfi":
            for crf in range(8):
                for imm in range(16):
                    for fm in (FPSCR_MODES[::2] + [usual_rule(F["VXSNAN"]), usual_rule(F["ZX"])]):
                        for msr in (FP, FP | FE1):
                            st = _clean_state()
                            st.msr = msr
                            st.fpscr = fm
                            go(name, (op << 26) | (crf << 23) | (imm << 12) | (xo << 1) | ((imm + crf) & 1), st)
        elif name == "mtfsf":
            srcs = [0, 0xFFFFFFFF, 0x60000000, 0x90000000, 0x01000080, 0x10000040, 0x00000800, 0x08000020,
                    0x20000000, 0x40000000]
            for fmask in range(256):
                for si, src in enumerate(srcs):
                    for fm in (FPSCR_MODES[fmask % len(FPSCR_MODES)], usual_rule(F["VXSNAN"] | F["VE"])):
                        st = _clean_state()
                        st.msr = FP | (FE0 if (fmask + si) & 1 else 0)
                        st.fpscr = fm
                        st.fpr[4] = [src | (0xDEADBEEF << 32), 0x4444]
                        go(name, (op << 26) | (fmask << 17) | (4 << 11) | (xo << 1) | (si & 1), st)
        elif name == "mcrfs":
            for crfd in range(8):
                for crfs in range(8):
                    for fm in FPSCR_MODES + [usual_rule(0xFFFFFFFF), usual_rule(0xF7FFF7FF)]:
                        for msr in (FP, FP | FE0):
                            st = _clean_state()
                            st.msr = msr
                            st.fpscr = fm
                            st.cr = 0x12345678
                            go(name, (op << 26) | (crfd << 23) | (crfs << 18) | (xo << 1), st)
        elif name == "mffs":
            for fm in FPSCR_MODES:
                for rc in (0, 1):
                    for pse in (True, False):
                        st = _clean_state()
                        st.fpscr = fm
                        if not pse:
                            st.hid2 = 0
                        go(name, (op << 26) | (5 << 21) | (xo << 1) | rc, st)
    # gating grid for every mnemonic: PSE x FP x FE x IP/ILE x reserved-dirty
    for name in NAMES:
        for hid2 in (0, PSE, 0xE0000000):
            for msr in (0, FP, FP | IP | ILE, FP | LE, IP, FP | FE0 | FE1):
                for dirty in (False, True):
                    st = _clean_state()
                    st.hid2, st.msr = hid2, msr | (EE | ME | IR | DR | RI)
                    st.fpscr = usual_rule(F["VE"] | F["ZE"] | F["OE"])
                    w = gen_word(r, name)
                    for a_, b_ in CAT[name][3]:
                        w = _setbits(w, a_, b_, (1 if dirty else 0))
                    st.gpr[_getbits(w, 11, 15)] = 0x80001000
                    st.gpr[_getbits(w, 16, 20)] = 4
                    for reg in range(32):
                        st.fpr[reg] = [r.choice(ALL_EDGE), r.choice(ALL_EDGE)]
                    go(name, w, st)
    return n[0], classes, counts, outcomes


def decode_sweep(mA, mB, seed):
    r = random.Random(seed)
    bad = []
    n = 0
    for op in range(64):
        for _ in range(16):
            w = (op << 26) | r.getrandbits(26)
            n += 1
            if bool(mA.supports(w)) != bool(mB.supports(w)):
                bad.append(("supports", "%08X" % w))
    for op in (31, 59, 63):
        for xo in range(1024):
            for _ in range(3):
                w = _setbits((op << 26) | r.getrandbits(26), 21, 30, xo)
                n += 1
                if bool(mA.supports(w)) != bool(mB.supports(w)):
                    bad.append(("supports", "%08X" % w))
    return n, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=200000)
    ap.add_argument("--seed", type=int, default=20260914)
    ap.add_argument("--jobs", type=int, default=max(1, min(16, (os.cpu_count() or 2) - 8)))
    ap.add_argument("--models", default="fp_A,fp_B")
    ap.add_argument("--show", type=int, default=1)
    ap.add_argument("--no-edge", action="store_true")
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    modA, modB = a.models.split(",")
    only = [x for x in a.only.split(",") if x]

    mA, mB = load_model(modA), load_model(modB)
    nd, bad = decode_sweep(mA, mB, a.seed)
    print("decode sweep: %d words, %d disagreements" % (nd, len(bad)))
    for x in bad[:10]:
        print("   ", x)

    outcomes = collections.Counter()
    ne, ect = 0, collections.Counter()
    if not a.no_edge:
        with mp.Pool(a.jobs) as pool:
            eres = pool.map(edge_task, [(j, a.jobs, modA, modB, only) for j in range(a.jobs)])
        ecl = collections.OrderedDict()
        for n_, cl, ct, oc in eres:
            ne += n_
            for k, v in cl.items():
                ecl.setdefault(k, v)
            ect.update(ct)
            outcomes.update(oc)
        print("edge sweep: %d vectors, %d disagreeing, %d classes" % (ne, ect[("__vectors__", (), ())], len(ecl)))
        _print_classes(ecl, ect, a.show)

    per = a.n // a.jobs
    tasks = [(a.seed * 7919 + j, per + (1 if j < a.n % a.jobs else 0), modA, modB, only) for j in range(a.jobs)]
    if a.n:
        if a.jobs == 1:
            results = [worker(tasks[0])]
        else:
            with mp.Pool(a.jobs) as pool:
                results = pool.map(worker, tasks)
    else:
        results = []
    classes, counts = collections.OrderedDict(), collections.Counter()
    for cl, ct, oc in results:
        for k, v in cl.items():
            classes.setdefault(k, v)
        counts.update(ct)
        outcomes.update(oc)
    total_bad = counts[("__vectors__", (), ())]
    print("random sweep: %d vectors, %d disagreeing, %d classes" % (a.n, total_bad, len(classes)))
    _print_classes(classes, counts, a.show)
    print("outcome coverage per mnemonic (ok / partial / invalid / error):")
    for nm in NAMES:
        print("   %-8s %7d / %7d / %6d / %4d" % (nm, outcomes[(nm, "ok")], outcomes[(nm, "partial")],
                                                outcomes[(nm, "invalid")], outcomes[(nm, "error")]))
    dis = total_bad + ect[("__vectors__", (), ())] + len(bad)
    print("TOTAL vectors %d (decode %d + edge %d + random %d); disagreements %d" % (nd + ne + a.n, nd, ne, a.n, dis))
    return 0 if dis == 0 else 1


def _print_classes(classes, counts, show):
    by = collections.defaultdict(list)
    for k in classes:
        if k[0] == "__vectors__":
            continue
        by[k[1]].append(k)
    for at in sorted(by, key=lambda at: -sum(counts[k] for k in by[at])):
        ks = by[at]
        tot = sum(counts[k] for k in ks)
        if tot == 0 and show < 2:
            continue
        names = sorted({k[0] for k in ks})
        feats = sorted({"+".join(k[2]) or "-" for k in ks})
        print("  [%d] %s  in %s  feats %s" % (tot, at, ",".join(names), ",".join(feats)))
        for k in ks[:show]:
            print("       e.g. %s %s %s" % (k[0], k[2], classes[k]))


if __name__ == "__main__":
    sys.exit(main())
