"""Differential cross-check of the two independent "ps" (paired single) specification models.

    python crosscheck_ps.py [--n N] [--seed S] [--jobs J] [--models ps_A,ps_B] [--show K] [--no-edge]
                            [--mutants]

    --mutants  applies each MUTANTS entry (a plausible single-point defect) to the second model and
               requires the edge sweep and the random sweep to each report it.

Last results (2026-09-14, seed 20260914):
    first run (original models): 1,369 of 4,000 random vectors disagreed, 16 mechanism classes
        (D1-D16 in ps.py); both models corrected in place ("[crosscheck fix]")
    ps_A vs ps_B : decode 4,608 + edge 244,058 + random 240,000 = 488,666 vectors, 0 disagreements
    ps   vs ps_B : decode 4,608 + edge 244,058 + random 220,000 = 468,666 vectors, 0 disagreements
    --mutants    : 58 of 58 detected by the edge sweep AND by the random sweep (32,000)

Draws N random execution vectors plus a deterministic edge sweep for every paired-single
instruction (29 ps_* arithmetic/move/compare instructions, 8 psq_* quantized loads/stores and
dcbz_l), with clean and reserved-bit-dirty encodings, aliasing register indices (D==A==B==C and
every pairwise alias), boundary single-precision operands (+-0, +-inf, QNaN/SNaN with assorted
payloads and signs, largest/smallest normal and denormal, rounding ties, near-overflow and
near-underflow products, integer-quantization boundaries), a sprinkling of non-single double
images, every RN mode, every FPSCR enable bit, NI, dirty sticky bits and inconsistent FX/FEX/VX
summaries, MSR[FP/FE0/FE1/IP/ILE/LE/EE/ME], HID2[PSE/LSQE/LCE/DCHEE/DCHERR], HID0[DCE], GQR
types 0-7 with every edge scale, aligned/misaligned/wrapping EAs with preloaded memory, and runs
both models from identical cloned states comparing
  - the outcome class: ok / partial (some locations undefined) / invalid (whole outcome
    undefined, state untouched) / error (a Python exception other than UndefinedBehaviour)
  - the normalised map of undefined locations with bit masks
  - the full State.arch_dict(), mem.log and mem.bytes, undefined bits masked in both
It also sweeps the opcode space of primaries 4/56/57/60/61 comparing supports()/mnemonic().

UB contracts being normalised:
  ps_A (and the reconciled ps.py): step() raises UndefinedBehaviour WITHOUT touching the state;
       .defined_state is None -> invalid; else a State holding the defined parts, .undefined is
       a list of location strings ('fpr[n][k]', 'fpr[n][k]:nan-payload', 'fpscr:<BIT>',
       'cr:CR1', 'srr1:0x....', 'hid2:DCHERR', 'mem:0xADDR+sign', 'srr0', 'pc', ...).
  ps_B: .applied False -> invalid (state untouched); True -> state holds the defined parts and
       .undefined maps field -> True | int mask | (addr, len).
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
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from gekko_state import MASK32, MASK64, State  # noqa: E402

M = MASK32


def _b(i):
    return 1 << (31 - i)


# ------------------------------------------------------------------------------------------
# constants written here from the manual, independently of both models
# ------------------------------------------------------------------------------------------
# MSR Table 4-4 PAGE 164-165
POW, ILE, EE, PR, FP, ME = _b(13), _b(15), _b(16), _b(17), _b(18), _b(19)
FE0, SE, BE, FE1, IP, IR, DR, PM, RI, LE = _b(20), _b(21), _b(22), _b(23), _b(25), _b(26), _b(27), \
    _b(29), _b(30), _b(31)
MSR_RESERVED_BITS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 24, 28]
# HID2 Table 2-6 PAGE 67
LSQE, WPE, PSE, LCE = _b(0), _b(1), _b(2), _b(3)
DCHERR, DNCERR, DCMERR, DQOERR, DCHEE, DNCEE, DCMEE, DQOEE = (_b(i) for i in range(8, 16))
DCE = _b(17)                    # HID0 PAGE 64
# FPSCR (PEM layout, consistent with PAGE 82/83/473)
F = {n: _b(i) for i, n in enumerate(["FX", "FEX", "VX", "OX", "UX", "ZX", "XX", "VXSNAN", "VXISI",
                                     "VXIDI", "VXZDZ", "VXIMZ", "VXVC", "FR", "FI", "C", "FL", "FG",
                                     "FE", "FU", "R20", "VXSOFT", "VXSQRT", "VXCVI", "VE", "OE",
                                     "UE", "ZE", "XE", "NI", "RN0", "RN1"])}
F["FPRF"] = 0x1F << 12

# name -> (primary, form, xo, reserved ranges)   Chapter 12 figures PAGE 493-531, 384
CAT = {
    "ps_sum0": (4, "a", 10, ()), "ps_sum1": (4, "a", 11, ()),
    "ps_muls0": (4, "a", 12, ((16, 20),)), "ps_muls1": (4, "a", 13, ((16, 20),)),
    "ps_madds0": (4, "a", 14, ()), "ps_madds1": (4, "a", 15, ()),
    "ps_div": (4, "a", 18, ((21, 25),)), "ps_sub": (4, "a", 20, ((21, 25),)),
    "ps_add": (4, "a", 21, ((21, 25),)), "ps_sel": (4, "a", 23, ()),
    "ps_res": (4, "a", 24, ((11, 15), (21, 25))), "ps_mul": (4, "a", 25, ((16, 20),)),
    "ps_rsqrte": (4, "a", 26, ((11, 15), (21, 25))), "ps_msub": (4, "a", 28, ()),
    "ps_madd": (4, "a", 29, ()), "ps_nmsub": (4, "a", 30, ()), "ps_nmadd": (4, "a", 31, ()),
    "ps_cmpu0": (4, "x", 0, ((9, 10), (31, 31))), "ps_cmpo0": (4, "x", 32, ((9, 10), (31, 31))),
    "ps_cmpu1": (4, "x", 64, ((9, 10), (31, 31))), "ps_cmpo1": (4, "x", 96, ((9, 10), (31, 31))),
    "ps_neg": (4, "x", 40, ((11, 15),)), "ps_mr": (4, "x", 72, ((11, 15),)),
    "ps_nabs": (4, "x", 136, ((11, 15),)), "ps_abs": (4, "x", 264, ((11, 15),)),
    "ps_merge00": (4, "x", 528, ()), "ps_merge01": (4, "x", 560, ()),
    "ps_merge10": (4, "x", 592, ()), "ps_merge11": (4, "x", 624, ()),
    "dcbz_l": (4, "x", 1014, ((6, 10), (31, 31))),
    "psq_lx": (4, "xw", 6, ((31, 31),)), "psq_stx": (4, "xw", 7, ((31, 31),)),
    "psq_lux": (4, "xw", 38, ((31, 31),)), "psq_stux": (4, "xw", 39, ((31, 31),)),
    "psq_l": (56, "d", None, ()), "psq_lu": (57, "d", None, ()),
    "psq_st": (60, "d", None, ()), "psq_stu": (61, "d", None, ()),
}
NAMES = sorted(CAT)
ARITH = ("ps_add", "ps_sub", "ps_div", "ps_mul", "ps_muls0", "ps_muls1", "ps_madd", "ps_msub",
         "ps_nmadd", "ps_nmsub", "ps_madds0", "ps_madds1", "ps_sum0", "ps_sum1")


def _setbits(w, a, b, v):
    width = b - a + 1
    sh = 31 - b
    mask = ((1 << width) - 1) << sh
    return (w & ~mask & M) | ((v << sh) & mask)


def _getbits(w, a, b):
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


# ------------------------------------------------------------------------------------------
# single-precision operand pools (as binary64 images)
# ------------------------------------------------------------------------------------------
def s2d(w):
    """binary32 bits -> binary64 image, bit exact (signalling NaNs kept)."""
    s, ex, fr = (w >> 31) & 1, (w >> 23) & 0xFF, w & 0x7FFFFF
    if ex == 0xFF:
        return (s << 63) | (0x7FF << 52) | (fr << 29)
    if ex == 0:
        if fr == 0:
            return s << 63
        sh = 24 - fr.bit_length()            # normalise the denormal
        e = -126 - sh                        # value = fr * 2^-149
        mant = (fr << (53 - fr.bit_length())) & ((1 << 52) - 1)
        return (s << 63) | ((e + 1023) << 52) | mant
    return (s << 63) | ((ex - 127 + 1023) << 52) | (fr << 29)


def f2s(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


SINGLE_EDGE = [
    0x00000000, 0x80000000, 0x7F800000, 0xFF800000,              # zeros, infinities
    0x7FC00000, 0xFFC00000, 0x7FC00001, 0xFFFFFFFF, 0x7FE12345,   # QNaNs
    0x7F800001, 0xFF800001, 0x7FA00000, 0xFFBFFFFF, 0x7F812345,   # SNaNs
    0x7F7FFFFF, 0xFF7FFFFF, 0x7F7FFFFE, 0x7F000000, 0x7E800000,   # large
    0x00800000, 0x80800000, 0x00800001, 0x007FFFFF, 0x807FFFFF,   # smallest normal / largest denorm
    0x00000001, 0x80000001, 0x00000002, 0x00000003, 0x00400000, 0x00012345,   # denormals
    f2s(1.0), f2s(-1.0), f2s(2.0), f2s(0.5), f2s(3.0), f2s(-3.0), f2s(1.5), f2s(0.25),
    0x3F800001, 0x3F7FFFFF, 0xBF800001, 0x33800000, 0x34000000, 0x33FFFFFF,   # 1+eps, 2^-24 ...
    f2s(16777216.0), f2s(16777215.0), f2s(8388608.0), f2s(0.1), f2s(-0.1), f2s(1.0 / 3.0),
    f2s(255.0), f2s(255.75), f2s(256.0), f2s(-128.0), f2s(-128.5), f2s(-129.0), f2s(127.5),
    f2s(65535.0), f2s(65535.5), f2s(65536.0), f2s(32767.5), f2s(-32768.0), f2s(-32768.5),
    f2s(-0.5), f2s(-0.75), f2s(0.75), f2s(-1.5), f2s(100.0), f2s(4294967296.0), f2s(2.0 ** -31),
    f2s(2.0 ** 63), f2s(2.0 ** 64), f2s(2.0 ** 100), f2s(2.0 ** -100), f2s(2.0 ** 64) | 1,
    0x5F800000, 0x1F800000, 0x20000000, 0x60000000, 0x5FFFFFFF, 0x20000001, 0x1FFFFFFF,   # 2^64, 2^-64 ...
    0x3FB504F3, 0x3FB504F4, 0x5F3504F3,
    0x00200001, 0x00200000, 0x001FFFFF, 0x00200002, 0x7E800001, 0x7E7FFFFF, 0x7E800000 | 0xFFF,   # 1/x at the edges
]
# operands whose products / quotients land at the underflow threshold
TINY_X = [0x007FFFFF, 0x007FFFFE, 0x007FFFF0, 0x00FFFFFF, 0x00800001, 0x00400001, 0x00000003]
TINY_Y = [0x3F800001, 0x3F800002, 0x3F800003, 0x3F800004, 0x3FFFFFFF, 0x3F000000, 0x3EFFFFFF, 0x3F7FFFFF]
# (2^23 - k) * 2^-149 * (1 + k * 2^-23) = 2^-126 - k^2 * 2^-172: tiny before rounding, not after
TINY_PAIRS = [(0x00800000 - k, 0x3F800000 + k) for k in range(1, 9)]
DOUBLE_ONLY = [0x3FB999999999999A, 0x0000000000000001, 0x8000000000000001, 0x000FFFFFFFFFFFFF,
               0x7FF0000000000001, 0x7FF8000000000001, 0x3FF0000000000001, 0x47F0000000000000,
               0x36A0000000000000, 0x36A0000000000001, 0x7FEFFFFFFFFFFFFF, 0x3690000000000000]


def rand_single(r):
    k = r.random()
    if k < 0.45:
        w = r.choice(SINGLE_EDGE)
        if r.random() < 0.15:
            w ^= 1 << r.randrange(32)
        return s2d(w)
    if k < 0.60:
        # exponent extremes / denormals
        ex = r.choice([0, 0, 1, 2, 0xFE, 0xFD, 0xFF, 0x7F, 0x80, 0x7E, 0x60, 0x20, 0x96, 0x97])
        return s2d((r.getrandbits(1) << 31) | (ex << 23) | r.choice([0, 1, 0x7FFFFF, 0x400000,
                                                                     r.getrandbits(23)]))
    if k < 0.75:
        # small integers and simple fractions: exercise exact results and quantization
        v = r.randrange(-70000, 70000) / r.choice([1, 1, 2, 4, 8, 256, 65536, 3])
        try:
            return s2d(f2s(v))
        except OverflowError:
            return 0
    if k < 0.77:
        return r.choice(DOUBLE_ONLY) | (r.getrandbits(1) << 63)
    return s2d(r.getrandbits(32))


# ------------------------------------------------------------------------------------------
# word / state generation
# ------------------------------------------------------------------------------------------
def rand_reg(r):
    return r.choice((0, 1, 2, 3)) if r.random() < 0.6 else r.randrange(32)


def gen_word(r, name):
    op, form, xo, reserved = CAT[name]
    dirty = r.random() < 0.08
    w = (op << 26) | r.getrandbits(26)
    if form in ("a", "x", "xw"):
        for a, b in ((6, 10), (11, 15), (16, 20), (21, 25)):
            w = _setbits(w, a, b, rand_reg(r))
        if form == "a":
            w = _setbits(w, 26, 30, xo)
        elif form == "x":
            w = _setbits(w, 21, 30, xo)
            if name.startswith("ps_cmp"):
                w = _setbits(w, 6, 8, r.randrange(8))
        else:
            w = _setbits(w, 25, 30, xo)
    else:
        w = _setbits(w, 6, 10, rand_reg(r))
        w = _setbits(w, 11, 15, rand_reg(r))
        if r.random() < 0.5:
            w = _setbits(w, 20, 31, r.choice([0, 4, 8, 1, 2, 3, 0x7FF, 0x800, 0xFFF, 0xFFC, 0xFF8, 0xFF0]))
    if not dirty:
        for a, b in reserved:
            w = _setbits(w, a, b, 0)
    return w & M


EA_EDGE = [0x80001000, 0x80001004, 0x80001001, 0x80001002, 0x80001003, 0x00000000, 0xFFFFFFFC,
           0xFFFFFFFE, 0xFFFFFFFF, 0x7FFFFFFC, 0x00000800, 0x80000000, 0xCC008000, 0x0000FFF0]


def rand_fpscr(r):
    k = r.random()
    if k < 0.25:
        f = 0
    elif k < 0.55:
        f = 0
        for n in ("VE", "OE", "UE", "ZE", "XE"):
            if r.random() < 0.25:
                f |= F[n]
    else:
        f = r.getrandbits(32)
        # keep NI less common, it collapses many paths to UB
        if r.random() < 0.7:
            f &= ~F["NI"]
    if r.random() < 0.12:
        f |= F["NI"]
    return (f & ~3) | r.randrange(4)


def rand_msr(r):
    msr = FP if r.random() < 0.92 else 0
    for bit, p in ((FE0, 0.12), (FE1, 0.12), (IP, 0.3), (ILE, 0.2), (LE, 0.06), (EE, 0.5), (ME, 0.5),
                   (IR, 0.5), (DR, 0.5), (RI, 0.5), (PR, 0.3), (SE, 0.05), (BE, 0.05), (PM, 0.1),
                   (POW, 0.03)):
        if r.random() < p:
            msr |= bit
    if r.random() < 0.04:
        msr |= _b(r.choice(MSR_RESERVED_BITS))
    return msr


def rand_gqr(r):
    def typ():
        return r.choice((0, 0, 4, 5, 6, 7, 4, 5, 6, 7)) if r.random() < 0.95 else r.choice((1, 2, 3))

    def scale():
        return r.choice((0, 1, 2, 31, 32, 33, 62, 63, 8, 56, 16, 48)) if r.random() < 0.6 else r.randrange(64)
    g = (scale() << 24) | (typ() << 16) | (scale() << 8) | typ()
    if r.random() < 0.1:
        g |= r.getrandbits(32) & 0xC01F3F80        # reserved bits 0-1, 8-12, 16-17, 24-28
    return g


def gen_state(r, name, w):
    st = State()
    st.msr = rand_msr(r)
    st.hid2 = 0
    for bit, p in ((PSE, 0.94), (LSQE, 0.9), (LCE, 0.6), (WPE, 0.3), (DCHEE, 0.4), (DCHERR, 0.3),
                   (DNCEE, 0.2), (DNCERR, 0.2), (DCMEE, 0.2), (DQOEE, 0.2)):
        if r.random() < p:
            st.hid2 |= bit
    st.hid0 = (DCE if r.random() < 0.85 else 0) | (r.getrandbits(32) & ~DCE if r.random() < 0.3 else 0)
    st.fpscr = rand_fpscr(r)
    st.cr = r.getrandbits(32)
    st.xer = r.getrandbits(32) if r.random() < 0.3 else 0
    st.pc = r.choice((0x80003100, 0x80003100, 0xFFFFFFFC, 0x00000700))
    st.srr0, st.srr1 = r.getrandbits(32), r.getrandbits(32)
    st.dar, st.dsisr = r.getrandbits(32), r.getrandbits(32)
    st.lr, st.ctr = r.getrandbits(32), r.getrandbits(32)
    st.gqr = [rand_gqr(r) for _ in range(8)]
    for i in range(32):
        st.gpr[i] = r.choice(EA_EDGE) if r.random() < 0.5 else r.getrandbits(32)
    # indexed forms: keep rB small so EAs are interesting
    if r.random() < 0.6:
        st.gpr[_getbits(w, 16, 20)] = r.choice((0, 4, 1, 2, 3, 8, 0xFFFFFFFC, 0x10, 0xFFFFFFFF))
    for i in range(32):
        st.fpr[i] = [rand_single(r), rand_single(r)]
    # correlated operands for arithmetic: same exponent ranges produce ties / cancellations
    if name in ARITH and r.random() < 0.35:
        a, b, c = _getbits(w, 11, 15), _getbits(w, 16, 20), _getbits(w, 21, 25)
        pick = r.choice(SINGLE_EDGE)
        for reg in (a, b, c):
            for k in (0, 1):
                if r.random() < 0.6:
                    x = pick
                    if r.random() < 0.5:
                        x ^= 1 << r.randrange(3)
                    if r.random() < 0.5:
                        x ^= 0x80000000
                    st.fpr[reg][k] = s2d(x)
    if name in ("ps_mul", "ps_muls0", "ps_muls1", "ps_div", "ps_madd", "ps_madds0", "ps_madds1") and r.random() < 0.1:
        a, b, c = _getbits(w, 11, 15), _getbits(w, 16, 20), _getbits(w, 21, 25)
        other = b if name == "ps_div" else c
        for k in (0, 1):
            x, y = r.choice(TINY_PAIRS) if r.random() < 0.5 else (r.choice(TINY_X), r.choice(TINY_Y))
            st.fpr[a][k] = s2d(x | (r.getrandbits(1) << 31))
            if other != a:
                st.fpr[other][k] = s2d(y)
        if r.random() < 0.5:
            st.fpscr = (st.fpscr | F["UE"]) & ~F["NI"]
        if name.startswith("ps_madd") and b not in (a, c):
            st.fpr[b] = [0, 1 << 63]
    if name in ("ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub", "ps_madds0", "ps_madds1") and r.random() < 0.08:
        # infinity * 0 with a NaN addend (VXIMZ interaction)
        a, b, c = _getbits(w, 11, 15), _getbits(w, 16, 20), _getbits(w, 21, 25)
        k = r.randrange(2)
        z, i_ = r.choice(((0, 0x7FF0000000000000), (0x7FF0000000000000, 1 << 63), (0, 0xFFF0000000000000)))
        st.fpr[a][k], st.fpr[c][k] = z, i_
        if b not in (a, c):
            st.fpr[b][k] = s2d(r.choice((0x7FC00000, 0x7F800001, 0xFFC00005)))
    st.exception = r.choice((None, None, "program"))
    st.reserve = r.random() < 0.2
    st.reserve_addr = r.getrandbits(32)
    # preload memory around every EA the instruction may use
    if name.startswith("psq") or name == "dcbz_l":
        a, b = _getbits(w, 11, 15), _getbits(w, 16, 20)
        d = _getbits(w, 20, 31)
        d = d - 0x1000 if d & 0x800 else d
        cands = {((st.gpr[a] if a else 0) + st.gpr[b]) & M, (st.gpr[a] + st.gpr[b]) & M,
                 ((st.gpr[a] if a else 0) + d) & M, (st.gpr[a] + d) & M}
        for ea in cands:
            kind = r.random()
            for off in range(-32, 40, 4):
                if kind < 0.4:
                    word = f2s_edge = r.choice(SINGLE_EDGE)
                elif kind < 0.7:
                    word = r.choice((0x00000000, 0xFFFFFFFF, 0x80008000, 0x7F7F7FFF, 0x0180FF7F,
                                     0x7FFF8000, 0x00010001))
                else:
                    word = r.getrandbits(32)
                st.mem.load((ea + off) & M, word.to_bytes(4, "big"))
    return st


# ------------------------------------------------------------------------------------------
# normalisation of the two UB contracts
# ------------------------------------------------------------------------------------------
REG32 = ("cr", "xer", "lr", "ctr", "msr", "fpscr", "srr0", "srr1", "dar", "dsisr", "dec", "hid0", "hid2", "pc")
PAYLOAD64 = ((1 << 22) - 1) << 29            # single NaN payload bits (below the quiet bit)


def _add(out, key, mask):
    if mask is True or out.get(key) is True:
        out[key] = True
    else:
        out[key] = out.get(key, 0) | mask


def norm_A(lst):
    out = {}
    for loc in lst:
        if loc == "all":
            _add(out, "all", True)
            continue
        m = re.fullmatch(r"fpr\[(\d+)\]\[(\d)\](?::(nan-payload|0x[0-9A-Fa-f]+))?", loc)
        if m:
            sub = m.group(3)
            mask = MASK64 if sub is None else (PAYLOAD64 if sub == "nan-payload" else int(sub, 16))
            _add(out, "fpr[%s][%s]" % (m.group(1), m.group(2)), mask)
            continue
        m = re.fullmatch(r"mem:0x([0-9A-Fa-f]+)(\+sign)?", loc)
        if m:
            _add(out, "mem[%08X]" % int(m.group(1), 16), 0x80 if m.group(2) else 0xFF)
            continue
        base, _, sub = loc.partition(":")
        if sub.startswith("0x"):
            _add(out, base, int(sub, 16))
        elif base == "fpscr":
            _add(out, "fpscr", F[sub] if sub else M)
        elif base == "cr":
            _add(out, "cr", 0x0F000000 if sub == "CR1" else M)
        elif base == "hid2":
            _add(out, "hid2", {"DCHERR": DCHERR}.get(sub, M) if sub else M)
        elif base in REG32:
            _add(out, base, M)
        elif base == "exception":
            _add(out, "exception", True)
        else:
            _add(out, "?" + loc, True)
    return {k: v for k, v in out.items() if v}


def norm_B(d):
    out = {}
    for k, v in d.items():
        if k == "mem":
            if isinstance(v, dict):
                for addr, m in v.items():
                    _add(out, "mem[%08X]" % (addr & M), m & 0xFF)
            else:
                addr, ln = v
                for i in range(ln):
                    _add(out, "mem[%08X]" % ((addr + i) & M), 0xFF)
        elif re.fullmatch(r"fpr\[\d+\]\[\d\]", k):
            _add(out, k, MASK64 if v is True else int(v) & MASK64)
        elif k in REG32:
            _add(out, k, M if v is True else int(v) & M)
        elif k == "exception":
            _add(out, k, True)
        else:
            _add(out, "?" + k, True)
    return {k: v for k, v in out.items() if v}


def run_model(mod, st, w):
    """-> (outcome, normalised undefined map, message); st holds the defined result."""
    try:
        mod.step(st, w)
        return ("ok", {}, "")
    except mod.UndefinedBehaviour as u:
        if hasattr(u, "applied"):                        # B contract
            if not u.applied:
                return ("invalid", {}, u.what)
            return ("partial", norm_B(u.undefined), u.what)
        if u.defined_state is None:                      # A contract
            return ("invalid", {}, u.what)
        ds = u.defined_state
        for k in vars(ds):
            if k == "mem":
                st.mem.bytes.clear()
                st.mem.bytes.update(ds.mem.bytes)
                st.mem.log[:] = ds.mem.log
            elif k in ("gpr", "gqr", "sprg"):
                getattr(st, k)[:] = getattr(ds, k)
            elif k == "fpr":
                for i in range(32):
                    st.fpr[i][:] = ds.fpr[i]
            else:
                setattr(st, k, getattr(ds, k))
        return ("partial", norm_A(u.undefined), u.what)
    except Exception as e:  # noqa: BLE001
        return ("error", {}, "%s: %s" % (type(e).__name__, e))


def snapshot(st, und=None):
    d = st.arch_dict()
    d = {k: ([list(p) for p in v] if k == "fpr" else (list(v) if isinstance(v, list) else v)) for k, v in d.items()}
    d["mem.log"] = list(st.mem.log)
    d["mem.bytes"] = dict(st.mem.bytes)
    d["pending"] = dict(st.pending)
    for loc, v in (und or {}).items():
        m = re.fullmatch(r"fpr\[(\d+)\]\[(\d)\]", loc)
        if m:
            d["fpr"][int(m.group(1))][int(m.group(2))] &= ~v & MASK64
        elif loc in REG32:
            d[loc] &= ~(M if v is True else v) & M
        elif loc == "exception":
            d["exception"] = "?"
        m = re.fullmatch(r"mem\[([0-9A-F]{8})\]", loc)
        if m:
            a = int(m.group(1), 16)
            if a in d["mem.bytes"]:
                d["mem.bytes"][a] &= ~v & 0xFF
            else:
                d["mem.bytes"][a] = 0
    if und and any(k.startswith("mem[") for k in und):
        # a write log entry that touches an undefined byte is compared with those bits cleared
        newlog = []
        for kind, addr, width, val in d["mem.log"]:
            if kind == "w":
                for i in range(width):
                    mk = und.get("mem[%08X]" % ((addr + i) & M))
                    if mk:
                        val &= ~(mk << (8 * (width - 1 - i)))
            newlog.append((kind, addr, width, val))
        d["mem.log"] = newlog
    # bytes explicitly zero and absent are the same memory
    d["mem.bytes"] = {k: v for k, v in d["mem.bytes"].items() if v}
    return d


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
                    _add(und, k, v)
            dA, dB = snapshot(sA, und), snapshot(sB, und)
            if dA != dB:
                problems.append(("state", tuple(diff_fields(dA, dB))))
    return oA, oB, problems, sA, sB, pre


def _fmt(v):
    if v is None or v is True:
        return v
    return "%X" % v


def mnemonic_of(w):
    op = w >> 26
    for n, (p, form, xo, _) in CAT.items():
        if p != op:
            continue
        if form == "d" or (form == "a" and _getbits(w, 26, 30) == xo) or \
                (form == "x" and _getbits(w, 21, 30) == xo) or (form == "xw" and _getbits(w, 25, 30) == xo):
            return n
    return "illegal"


def _feature(name, w, base, problems):
    """coarse sub-classification so distinct mechanisms land in distinct classes."""
    fps = base.fpscr
    tags = []
    if not base.hid2 & PSE:
        tags.append("PSE0")
    if not base.msr & FP:
        tags.append("FP0")
    if fps & F["NI"]:
        tags.append("NI")
    if base.msr & (FE0 | FE1):
        tags.append("FE")
    return tuple(tags)


FPSCR_NAMES = [n for n in F if n != "FPRF"]


def _bitnames(mask):
    return [n for n in FPSCR_NAMES if mask & F[n]]


def atoms(name, w, base, problems):
    """split a vector's problems into atomic, mechanism-level disagreement keys."""
    d = _getbits(w, 6, 10)
    out = []
    for p in problems:
        if p[0] == "undefined-set":
            for k, a, b in p[1]:
                ma = 0 if a is None else (True if a is True else int(a, 16))
                mb = 0 if b is None else (True if b is True else int(b, 16))
                side = lambda m: "A" if m else "-"  # noqa: E731
                fm = re.fullmatch(r"fpr\[(\d+)\]\[(\d)\]", k)
                if fm:
                    reg = "D" if int(fm.group(1)) == d else "r%s" % fm.group(1)
                    kind = lambda m: ("-" if not m else ("payload" if m == PAYLOAD64 else "all"))  # noqa: E731
                    out.append(("undef", "fpr[%s][%s]" % (reg, fm.group(2)), "A:" + kind(ma), "B:" + kind(mb)))
                elif k == "fpscr":
                    ia = set() if not ma else (set(FPSCR_NAMES) if ma is True else set(_bitnames(ma)))
                    ib = set() if not mb else (set(FPSCR_NAMES) if mb is True else set(_bitnames(mb)))
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
                    x = "fpr[%s][%s]" % ("D" if int(fm.group(1)) == d else "r", fm.group(2))
                elif "^" in x:
                    reg, _, m = x.partition("^")
                    if reg == "fpscr":
                        x = "fpscr:" + ",".join(_bitnames(int(m, 16)))
                    else:
                        x = reg + "^" + m
                out.append(("state", x))
        elif p[0] == "outcome":
            out.append(p)
        else:
            out.append(p[:1])
    return sorted(set(out))


def record(classes, counts, name, w, base, res):
    oA, oB, problems, sA, sB, pre = res
    serious = [p for p in problems if not p[0].endswith("reason-without-page")]
    if not problems:
        return
    for at in atoms(name, w, base, problems):
        key = (name, at)
        counts[key] += 1 if serious else 0
        if key not in classes:
            a, b, c, d = _getbits(w, 11, 15), _getbits(w, 16, 20), _getbits(w, 21, 25), _getbits(w, 6, 10)
            classes[key] = {
                "word": "%08X" % w, "D,A,B,C": (d, a, b, c),
                "pre": {"fpscr": "%08X" % base.fpscr, "msr": "%08X" % base.msr, "hid2": "%08X" % base.hid2,
                        "hid0": "%08X" % base.hid0, "gqr": ["%08X" % g for g in base.gqr],
                        "frA": ["%016X" % x for x in base.fpr[a]], "frB": ["%016X" % x for x in base.fpr[b]],
                        "frC": ["%016X" % x for x in base.fpr[c]], "frD": ["%016X" % x for x in base.fpr[d]],
                        "gprA": "%08X" % base.gpr[a], "gprB": "%08X" % base.gpr[b]},
                "problems": problems,
                "A": (oA[0], {k: _fmt(v) for k, v in oA[1].items()}, oA[2][:400]),
                "B": (oB[0], {k: _fmt(v) for k, v in oB[1].items()}, oB[2][:400]),
                "diff": {f: (_val(sA, f), _val(sB, f)) for f in diff_fields(snapshot(sA), snapshot(sB))[:8]},
            }
    if serious:
        counts[("__vectors__", ())] += 1


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


# ------------------------------------------------------------------------------------------
# mutants: plausible single-point defects applied to the second model to validate detection
# ------------------------------------------------------------------------------------------
MUTANTS = [
    # (description, anchor text in ps_B.py, replacement) - each a plausible single-point defect
    ("SRR1 bit 0 not copied", "SRR1_MSR_COPY_MASK = 0x87C0FFFF", "SRR1_MSR_COPY_MASK = 0x07C0FFFF"),
    ("LE not loaded from ILE", "    if old & MSR_ILE:", "    if False:"),
    ("vector ignores MSR[IP]", "(0xFFF00000 if old & MSR_IP else 0)", "(0)"),
    ("alignment DAR not set", "    st.dar = ea & MASK32  # PAGE 59", "    pass  # PAGE 59"),
    ("exact-zero sum sign RN=2 not 3", 'return ("zero", 1 if rn == 3 else 0)', 'return ("zero", 1 if rn == 2 else 0)'),
    ("ties away instead of even", "(rem == HALF and (n & 1) == 1)", "(rem == HALF)"),
    ("RN=2 rounds toward -inf", "            up = sign == 0\n", "            up = sign == 1\n"),
    ("generated QNaN sign defined for all", "(0 if nm_family else IMG_SIGN)", "(0)"),
    ("SNaN not suppressed with VE=1", "    if sets and st.fpscr & VE:", "    if sets and st.fpscr & ZE:"),
    ("SNaN not quieted", "_single_bits_to_image(v.bits | 0x400000)", "_single_bits_to_image(v.bits)"),
    ("SNaN payload kept defined", 'umask=0 if v.kind == "qnan" else IMG_PAYLOAD', "umask=0"),
    ("overflow RN=2 to +inf for both signs", "(rn == 2 and sign == 0) or (rn == 3", "(rn == 2) or (rn == 3"),
    ("tininess only before rounding", 'for tiny in {r["tiny_before"], r["tiny_after"]}:', 'for tiny in {r["tiny_before"]}:'),
    ("loss of accuracy only inexact", '{r["inexact"], r["denorm_loss"]}', '{r["inexact"]}'),
    ("FR never set", 'fr=1 if r["up"] else 0', "fr=0"),
    ("FI never set", 'fi=1 if r["inexact"] else 0)', "fi=0)"),
    ("NI zero sign fixed +0", ", _Half(IMG_SIGN, amb=UX | XX, fr=None, fi=None)]", "]"),
    ("NI zeroing only UX open", "        h.amb |= UX | XX\n", "        h.amb |= UX\n"),
    ("VXIMZ with NaN addend defined", "            amb = VXIMZ", "            amb = 0"),
    ("nmadd not negated", "negate=negate, nm_family=negate)", "negate=False, nm_family=negate)"),
    ("FX set without 0->1 transition", "if sets & STICKY_EXC & ~old:", "if sets & STICKY_EXC:"),
    ("FEX ignores XX&XE", "or ((new & ZX) and (new & ZE)) or ((new & XX) and (new & XE))", "or ((new & ZX) and (new & ZE))"),
    ("CR1 not updated for arithmetic Rc", "0x0F000000)) if rc else cr_old", "0x0F000000)) if False else cr_old"),
    ("FE not-completed alternative missing", "        outs.append((old[0], old[1], fp, cr_old, 0, 0, 0))  # the not-completed alternative", "        pass"),
    ("ps_muls0 uses frC(ps1)", '"ps_muls0": (0, 0)', '"ps_muls0": (0, 1)'),
    ("ps_madds1 uses frC(ps0) for ps0", '"ps_madds1": (1, 1)', '"ps_madds1": (0, 1)'),
    ("ps_sum1 FPRF from ps0", "[_single_bits_to_image(mv.bits), h1], 1, rc)", "[_single_bits_to_image(mv.bits), h1], 0, rc)"),
    ("ps_sel NaN selects frC", 'ge = (not va.nan) and (va.kind == "zero" or va.sign == 0)', 'ge = (va.kind == "zero" or va.sign == 0)'),
    ("ps_res -inf gives +0", "return [_Half(IMG_SIGN)]  # PAGE 525", "return [_Half(0)]  # PAGE 525"),
    ("ps_res range check without tolerance", "r * Fraction(4097, 4096) <= MAX_SINGLE", "r <= MAX_SINGLE"),
    ("ps_cmpo VXVC regardless of VE", "if ordered and not st.fpscr & VE:", "if ordered:"),
    ("compare CR field index off", "shift = 28 - 4 * crf", "shift = 28 - 4 * (crf & 3)"),
    ("ps_nabs toggles sign", "img |= 1 << 63", "img ^= 1 << 63"),
    ("ps_merge10 swapped", '"ps_merge10": (1, 0)', '"ps_merge10": (0, 1)'),
    ("dcbz_l block 16-byte aligned", "base = ea & ~0x1F & MASK32", "base = ea & ~0x0F & MASK32"),
    ("DCHERR open even when already 1", "    if not st.hid2 & HID2_DCHERR:", "    if True:"),
    ("dcbz_l machine check ignores EE/ME", "if st.hid2 & HID2_DCHEE and st.msr & MSR_EE and st.msr & MSR_ME:", "if st.hid2 & HID2_DCHEE:"),
    ("dequantize s8 sign bit wrong", "if typ == 6 and raw & 0x80:", "if typ == 6 and raw & 0x40:"),
    ("dequantize scale sign flipped", "val = Fraction(raw) * _pow2(-scale)", "val = Fraction(raw) * _pow2(scale)"),
    ("quantize floors instead of truncating", "    t = int(x)  # PAGE 111", "    t = x.numerator // x.denominator  # PAGE 111"),
    ("quantize -NaN defined", "        return hi & mask, mask", "        return hi & mask, 0"),
    ("type-0 -denorm sign defined", "return 0, (0x80000000 if v.sign else 0)", "return 0, 0"),
    ("alignment only for odd EA", "    if typ == 0 and ea & 3:", "    if typ == 0 and ea & 1:"),
    ("W=1 ps1 not 1.0", "ps1 = ONE_IMAGE  # PAGE 493", "ps1 = 0  # PAGE 493"),
    ("update form does not write rA", '        st.gpr[a] = ea  # PAGE 494 "rA <- EA"', "        pass"),
    ("psq_lx ignores rA|0", "base = st.gpr[a] if (update or a != 0) else 0  # PAGE 496", "base = st.gpr[a]  # PAGE 496"),
    ("indexed forms also need LSQE", "(not indexed and not hid2 & HID2_LSQE)", "(not hid2 & HID2_LSQE)"),
    ("reserved GQR type 3 accepted", "    if typ in (1, 2, 3):", "    if typ in (1, 2):"),
    ("ps_sel NI alternative missing", "            if st.fpscr & NI and va.denormal and not ge:", "            if False:"),
    ("compare NI zeroed alternative missing", "        if st.fpscr & NI and (va.denormal or vb.denormal):", "        if False:"),
    ("FPRF kept when only the other half has no result", "if h0.noresult or h1.noresult or src.fprf is None:",
     "if src.noresult or src.fprf is None:"),
    ("enabled overflow value defined", "return [_Half(img, umask=MASK64, sets=OX, amb=XX, fr=None, fi=None, fprf=None)]",
     "return [_Half(img, sets=OX | XX, fr=None, fi=1)]"),
    ("QNaN operand payload open", 'umask=0 if v.kind == "qnan" else IMG_PAYLOAD', "umask=IMG_PAYLOAD"),
    ("NaN priority = first operand", "    for v in nans:\n        if v.bits in seen:", "    for v in nans[:1]:\n        if v.bits in seen:"),
    ("SRR1 exception-specific bits defined", 'ctx.mark("srr1", SRR1_SPECIFIC_MASK,', 'ctx.mark("srr1", 0,'),
    ("alignment DSISR defined", 'ctx.mark("dsisr", True,', 'ctx.mark("dsisr", False,'),
    ("NI denormal result judged after rounding only", 'denorm = {r["val"] != 0 and r["val"] < MIN_NORMAL, r["tiny_before"]}',
     'denorm = {r["val"] != 0 and r["val"] < MIN_NORMAL}'),
    ("enabled underflow defines XX", "sets=UX, amb=XX, fr=None, fi=None, fprf=None))\n            continue",
     "sets=UX | XX, fr=None, fi=None, fprf=None))\n            continue"),
]


def load_model(spec):
    if "@" not in spec:
        return importlib.import_module(spec)
    base, idx = spec.split("@")
    key = "%s_mut%s" % (base, idx)
    if key in sys.modules:
        return sys.modules[key]
    desc, old, new = MUTANTS[int(idx)]
    with open(os.path.join(HERE, base + ".py"), encoding="utf-8") as fh:
        src = fh.read()
    if old not in src:
        raise RuntimeError("mutant %s (%s): anchor text not found" % (idx, desc))
    src = src.replace(old, new, 1).replace('if __name__ == "__main__":', "if False:")
    mod = types.ModuleType(key)
    mod.__file__ = os.path.join(HERE, base + ".py")
    sys.modules[key] = mod
    exec(compile(src, key, "exec"), mod.__dict__)
    return mod


# ------------------------------------------------------------------------------------------
# sweeps
# ------------------------------------------------------------------------------------------
def mechanisms(res):
    """names of the UB mechanisms a vector exercised (first model's view), for coverage reporting."""
    out = set()
    o = res[0]
    out.add("outcome:" + o[0])
    for k, v in o[1].items():
        if k.startswith("fpr["):
            if v == PAYLOAD64:
                out.add("fpr:nan-payload")
            elif v == PAYLOAD64 | (1 << 63):
                out.add("fpr:generated-qnan")
            elif v == MASK64:
                out.add("fpr:whole")
            elif v == (1 << 63):
                out.add("fpr:sign-only(NI zero)")
            else:
                out.add("fpr:partial")
        elif k == "fpscr":
            for n in FPSCR_NAMES:
                if v & F[n]:
                    out.add("fpscr:" + ("FPRF" if n in ("C", "FL", "FG", "FE", "FU") else n))
        elif k.startswith("mem["):
            out.add("mem:%02X" % v)
        elif k == "cr" and v != M:
            out.add("cr:CR1-bits" if not v & ~0x0F000000 else "cr:crfD-bits")
        elif k in REG32:
            out.add("%s:%s" % (k, "all" if v == M else "%08X" % v))
        else:
            out.add(k)
    return out


def worker(args):
    seed, count, modA, modB = args
    mA, mB = load_model(modA), load_model(modB)
    r = random.Random(seed)
    classes, counts, outcomes = collections.OrderedDict(), collections.Counter(), collections.Counter()
    for _ in range(count):
        name = r.choice(NAMES)
        w = gen_word(r, name) & M
        base = gen_state(r, name, w)
        res = compare(mA, mB, base, w)
        outcomes[(name, res[0][0])] += 1
        for mech in mechanisms(res):
            outcomes[("__mech__", mech)] += 1
        record(classes, counts, name, w, base, res)
    return classes, counts, outcomes


def _clean_state():
    st = State()
    st.msr = FP
    st.hid2 = PSE | LSQE | LCE
    st.hid0 = DCE
    return st


def edge_vectors(modA, modB):
    """deterministic sweep: every mnemonic x every edge operand pair x every RN x enable set."""
    mA, mB = load_model(modA), load_model(modB)
    classes, counts, outcomes = collections.OrderedDict(), collections.Counter(), collections.Counter()
    n = 0

    def go(name, w, st):
        nonlocal n
        n += 1
        res = compare(mA, mB, st, w)
        outcomes[(name, res[0][0])] += 1
        for mech in mechanisms(res):
            outcomes[("__mech__", mech)] += 1
        record(classes, counts, name, w, st, res)

    ops = [s2d(x) for x in SINGLE_EDGE] + [DOUBLE_ONLY[0], DOUBLE_ONLY[1]]
    # enable / sticky patterns (RN is chosen independently); includes sticky bits with FX clear and
    # inconsistent VX / FEX summaries
    fpscr_modes = [0, F["VE"], F["OE"], F["UE"], F["ZE"], F["XE"], F["NI"], F["NI"] | F["UE"],
                   F["VE"] | F["OE"] | F["UE"] | F["ZE"] | F["XE"], 0xFFFFFFF8 & ~F["NI"],
                   0x7FF80700 & ~F["NI"], 0x1F000000 | F["XE"], F["FEX"] | F["VX"] | F["VE"], 0x02F9F0F8]
    r = random.Random(12345)
    for name in NAMES:
        op, form, xo, _ = CAT[name]
        if name in ARITH or name in ("ps_sel", "ps_cmpu0", "ps_cmpo0", "ps_cmpu1", "ps_cmpo1"):
            # D=3 A=1 B=2 C=4 ; operand grid on one half, random on the other
            for i, x in enumerate(ops):
                for j, y in enumerate(ops):
                    if (i * 7 + j) % 3 and not name.startswith("ps_cmp"):
                        continue
                    st = _clean_state()
                    st.fpscr = (fpscr_modes[(i * 5 + j * 3) % len(fpscr_modes)] & ~3) | ((i + 2 * j) % 4)
                    if (i * j) % 16 == 3:
                        st.msr |= FE0
                    k = (i + j) & 1
                    st.fpr[1] = [x, y] if k else [y, x]
                    st.fpr[2] = [y, x] if k else [x, y]
                    st.fpr[4] = [ops[(i * 3 + j) % len(ops)], ops[(i + j * 5) % len(ops)]]
                    if form == "a":
                        w = (4 << 26) | (3 << 21) | (1 << 16) | (2 << 11) | (4 << 6) | (xo << 1) | ((i + j) & 1)
                        if name in ("ps_mul", "ps_muls0", "ps_muls1"):
                            w &= ~(0x1F << 11)
                        if name in ("ps_add", "ps_sub", "ps_div"):
                            w &= ~(0x1F << 6)
                    else:
                        w = (4 << 26) | (((i + j) % 8) << 23) | (1 << 16) | (2 << 11) | (xo << 1)
                    go(name, w, st)
        elif name in ("ps_res", "ps_rsqrte", "ps_neg", "ps_mr", "ps_abs", "ps_nabs",
                      "ps_merge00", "ps_merge01", "ps_merge10", "ps_merge11"):
            for i, x in enumerate(ops):
                for mi, fm in enumerate(fpscr_modes):
                    st = _clean_state()
                    st.fpscr = (fm & ~3) | ((i + mi) % 4)
                    st.fpr[2] = [x, ops[(i * 5 + 3) % len(ops)]]
                    st.fpr[1] = [ops[(i * 11 + 1) % len(ops)], x]
                    if form == "a" or not name.startswith("ps_merge"):
                        # estimates and moves: only frB, the A (and C) fields are reserved
                        w = (4 << 26) | (3 << 21) | (2 << 11) | (xo << 1) | (i & 1)
                    else:
                        w = (4 << 26) | (3 << 21) | (1 << 16) | (2 << 11) | (xo << 1) | (i & 1)
                    if fm & F["FE"] and i % 5 == 0:
                        st.msr |= FE1
                    go(name, w, st)
        elif name.startswith("psq"):
            load = "_l" in name
            for t in range(8):
                for s6 in (0, 1, 2, 31, 32, 33, 62, 63, 7, 57):
                    for W in (0, 1):
                        for ea in (0x80001000, 0x80001001, 0x80001002, 0xFFFFFFFC):
                            for vi in range(0, len(ops), 9 if load else (1 if t == 0 else 3)):
                                st = _clean_state()
                                g = (s6 << 24) | (t << 16) | (s6 << 8) | t
                                I = (t + s6 + vi) & 7
                                st.gqr[I] = g
                                st.gpr[5] = ea
                                st.gpr[6] = 0
                                x, y = ops[vi], ops[(vi * 7 + 5) % len(ops)]
                                st.fpr[3] = [x, y]
                                v32 = SINGLE_EDGE[vi % len(SINGLE_EDGE)]
                                st.mem.load(ea, v32.to_bytes(4, "big") + bytes([0x80, 0x01, 0xFF, 0x7F]))
                                if form == "d":
                                    w = (op << 26) | (3 << 21) | (5 << 16) | (W << 15) | (I << 12)
                                else:
                                    w = (4 << 26) | (3 << 21) | (5 << 16) | (6 << 11) | (W << 10) | (I << 7) | (xo << 1)
                                go(name, w, st)
        elif name == "dcbz_l":
            for hid2 in (PSE | LCE, PSE, LCE | DCHEE, LCE | DCHEE | DCHERR, 0):
                for hid0 in (0, DCE):
                    for msr in (FP, FP | EE | ME, EE | ME, IP | ILE):
                        for a, b in ((0, 5), (5, 6), (5, 0)):
                            st = _clean_state()
                            st.hid2, st.hid0, st.msr = hid2, hid0, msr
                            st.gpr[5], st.gpr[6] = 0x80001013, 0x21
                            st.mem.load(0x80001000, bytes(range(1, 80)))
                            w = (4 << 26) | (a << 16) | (b << 11) | (1014 << 1)
                            go(name, w, st)
    # underflow threshold: exact results just below / at / above 2^-126 in every RN, UE and NI mode
    # (tininess before vs after rounding, loss of accuracy as inexact vs denormalisation loss)
    tiny_x, tiny_y = TINY_X, TINY_Y
    for name in ("ps_mul", "ps_div", "ps_madd", "ps_nmsub", "ps_add"):
        op, form, xo, _ = CAT[name]
        for x in tiny_x + [p[0] for p in TINY_PAIRS[:3]]:
            for y in tiny_y + [p[1] for p in TINY_PAIRS[:3]]:
                for sgn in (0, 0x80000000):
                    for rn in range(4):
                        for en in (0, F["UE"], F["NI"], F["XE"]):
                            st = _clean_state()
                            st.fpscr = en | rn
                            if name == "ps_add":
                                st.fpr[1] = [s2d(x | sgn), s2d(x)]
                                st.fpr[2] = [s2d((x - 1) | 0x80000000), s2d(0x80000001 ^ sgn)]
                            elif name == "ps_div":
                                st.fpr[1] = [s2d(x | sgn), s2d(x)]
                                st.fpr[2] = [s2d(y), s2d(0x3F800000 | (y & 0xFF))]
                            else:
                                st.fpr[1] = [s2d(x | sgn), s2d(y)]
                                st.fpr[4] = [s2d(y), s2d(x | sgn)]
                                st.fpr[2] = [s2d(0x80000001), 0]
                            w = (4 << 26) | (3 << 21) | (1 << 16) | (2 << 11) | (4 << 6) | (xo << 1)
                            if name == "ps_mul":
                                w &= ~(0x1F << 11)
                            if name in ("ps_add", "ps_div"):
                                w &= ~(0x1F << 6)
                            go(name, w, st)
    # gating grid for every mnemonic: PSE x LSQE x LCE x FP x reserved-dirty x rA=0
    for name in NAMES:
        for hid2 in (0, PSE, LSQE, PSE | LSQE, PSE | LCE, LCE, PSE | LSQE | LCE):
            for msr in (0, FP, FP | IP | ILE, FP | LE, IP, FP | _b(0) | _b(5), _b(0) | _b(1) | _b(28)):
                for dirty in (False, True):
                    st = _clean_state()
                    st.hid2, st.msr = hid2, msr | (EE | ME | IR | DR | RI)
                    w = gen_word(r, name)
                    for a, b in CAT[name][3]:
                        w = _setbits(w, a, b, (1 if dirty else 0))
                    st.gpr[_getbits(w, 11, 15)] = 0x80001000
                    st.gpr[_getbits(w, 16, 20)] = 4
                    go(name, w, st)
    return n, classes, counts, outcomes


def mutant_task(args):
    kind, i, seed, count, modA, spec = args
    if kind == "edge":
        _, _, ct, _ = edge_vectors(modA, spec)
        return kind, i, ct[("__vectors__", ())]
    _, ct, _ = worker((seed, count, modA, spec))
    return kind, i, ct[("__vectors__", ())]


def run_mutants(a, modA, modB):
    """--mutants: every MUTANTS entry must be reported by the edge sweep AND by the random sweep."""
    chunks = 4
    tasks = []
    for i in range(len(MUTANTS)):
        spec = "%s@%d" % (modB, i)
        if not a.no_edge:
            tasks.append(("edge", i, 0, 0, modA, spec))
        for j in range(chunks):
            tasks.append(("random", i, a.seed * 31 + j, max(1, a.n // chunks), modA, spec))
    hits = collections.defaultdict(lambda: {"edge": 0, "random": 0})
    with mp.Pool(a.jobs) as pool:
        for kind, i, nbad in pool.imap_unordered(mutant_task, tasks):
            hits[i][kind] += nbad
    missed_e = missed_r = 0
    for i, (desc, _, _) in enumerate(MUTANTS):
        h = hits[i]
        print("mutant %2d %-44s edge %7s  random(%d) %6d" % (i, desc, "-" if a.no_edge else h["edge"], a.n, h["random"]))
        missed_e += (not a.no_edge) and h["edge"] == 0
        missed_r += h["random"] == 0
    print("MUTANTS: %d; missed by edge sweep: %s; missed by random sweep: %d"
          % (len(MUTANTS), "-" if a.no_edge else missed_e, missed_r))
    return 1 if (missed_e + missed_r) else 0


def decode_sweep(mA, mB, seed):
    r = random.Random(seed)
    bad = []
    n = 0
    for op in range(64):
        for _ in range(8):
            w = (op << 26) | r.getrandbits(26)
            n += 1
            if bool(mA.supports(w)) != bool(mB.supports(w)):
                bad.append(("supports", "%08X" % w))
    for xo in range(1024):
        for _ in range(4):
            w = (4 << 26) | r.getrandbits(26)
            w = _setbits(w, 21, 30, xo)
            n += 1
            ma, mb = mA.mnemonic(w), mB.mnemonic(w)
            mc = mnemonic_of(w)
            if not (ma == mb == mc):
                bad.append(("mnemonic", "%08X" % w, ma, mb, mc))
    return n, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=240000)
    ap.add_argument("--seed", type=int, default=20260914)
    ap.add_argument("--jobs", type=int, default=max(1, min(24, (os.cpu_count() or 2) - 4)))
    ap.add_argument("--models", default="ps_A,ps_B")
    ap.add_argument("--show", type=int, default=1)
    ap.add_argument("--no-edge", action="store_true")
    ap.add_argument("--mutants", action="store_true")
    a = ap.parse_args()
    modA, modB = a.models.split(",")

    if a.mutants:
        return run_mutants(a, modA, modB)

    mA, mB = load_model(modA), load_model(modB)
    nd, bad = decode_sweep(mA, mB, a.seed)
    print("decode sweep: %d words, %d disagreements" % (nd, len(bad)))
    for x in bad[:10]:
        print("   ", x)

    outcomes = collections.Counter()
    ne, ecl, ect = 0, {}, collections.Counter()
    if not a.no_edge:
        ne, ecl, ect, eoc = edge_vectors(modA, modB)
        outcomes.update(eoc)
        print("edge sweep: %d vectors, %d disagreeing, %d classes" % (ne, ect[("__vectors__", ())], len(ecl)))
        _print_classes(ecl, ect, a.show)

    per = a.n // a.jobs
    tasks = [(a.seed * 7919 + j, per + (1 if j < a.n % a.jobs else 0), modA, modB) for j in range(a.jobs)]
    if a.jobs == 1:
        results = [worker(tasks[0])]
    else:
        with mp.Pool(a.jobs) as pool:
            results = pool.map(worker, tasks)
    classes, counts = collections.OrderedDict(), collections.Counter()
    for cl, ct, oc in results:
        for k, v in cl.items():
            classes.setdefault(k, v)
        counts.update(ct)
        outcomes.update(oc)
    total_bad = counts[("__vectors__", ())]
    print("random sweep: %d vectors, %d disagreeing, %d classes" % (a.n, total_bad, len(classes)))
    _print_classes(classes, counts, a.show)
    print("outcome coverage per mnemonic (ok / partial / invalid / error):")
    for nm in NAMES:
        print("   %-10s %7d / %7d / %6d / %4d" % (nm, outcomes[(nm, "ok")], outcomes[(nm, "partial")],
                                                  outcomes[(nm, "invalid")], outcomes[(nm, "error")]))
    print("undefined-behaviour mechanism coverage (vectors, edge + random):")
    mech = sorted((k[1], v) for k, v in outcomes.items() if k[0] == "__mech__")
    for i in range(0, len(mech), 3):
        print("   " + "".join("%-28s %8d   " % kv for kv in mech[i:i + 3]))
    dis = total_bad + ect[("__vectors__", ())] + len(bad)
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
        print("  [%d] %s  in %s" % (tot, at, ",".join(sorted({k[0] for k in ks}))))
        for k in ks[:show]:
            print("       e.g. %s %s" % (k[0], classes[k]))


if __name__ == "__main__":
    sys.exit(main())
