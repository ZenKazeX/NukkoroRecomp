"""Independent semantic model, author A, area "ps".

Gekko paired-single extensions, derived ONLY from docs/reference/ppc/gekko_user_manual.txt
(cited "PAGE N" = the "===== PAGE N =====" marker) with ppc_750cl.txt as cross-check
(cited "750CL PAGE N"), plus general IEEE-754 knowledge.

Covered (supports() is True for every primary-4 word and for primaries 56/57/60/61):
  A-form  (primary 4, XO in bits 26-30): ps_sum0 10, ps_sum1 11, ps_muls0 12, ps_muls1 13,
          ps_madds0 14, ps_madds1 15, ps_div 18, ps_sub 20, ps_add 21, ps_sel 23, ps_res 24,
          ps_mul 25, ps_rsqrte 26, ps_msub 28, ps_madd 29, ps_nmsub 30, ps_nmadd 31
  X-form  (primary 4, XO in bits 21-30): ps_cmpu0 0, ps_cmpo0 32, ps_neg 40, ps_cmpu1 64,
          ps_mr 72, ps_cmpo1 96, ps_nabs 136, ps_abs 264, ps_merge00 528, ps_merge01 560,
          ps_merge10 592, ps_merge11 624, dcbz_l 1014
  XW-form (primary 4, XO in bits 25-30): psq_lx 6, psq_stx 7, psq_lux 38, psq_stux 39
  DW-form: psq_l 56, psq_lu 57, psq_st 60, psq_stu 61
  every other primary-4 word: illegal instruction (PAGE 88 "All unused extended opcodes are
  illegal" and primary 4 is listed as having unused extended opcodes).

Representation (from gekko_state): fpr[n] = [ps0_bits, ps1_bits], each the u64 IEEE double image
of the single-precision value.  A ps value whose double image is not exactly a single-precision
value is a "double-precision operand"; applying a paired-single instruction to it is a
programming error with unspecified result (PAGE 83) -> UndefinedBehaviour.

Undefined behaviour is never invented.  step() raises UndefinedBehaviour WITHOUT touching the
state.  exc.undefined is a list of location strings; exc.defined_state is either None (nothing
is defined, e.g. reserved bits) or a State clone holding every location the manual does define
(locations named in exc.undefined hold a placeholder there).
Location names: 'fpr[n][k]' (whole image), 'fpr[n][k]:0x<u64 mask>', 'fpscr:0x<u32 mask>',
'cr:0x<u32 mask>', 'srr1:0x<u32 mask>', 'srr0', 'srr1', 'msr', 'pc', 'exception', 'dsisr',
'hid2:DCHERR', 'mem:0x<addr>' (whole byte), 'mem:0x<addr>+sign' (bit 0x80 of that byte), 'all'.
undefined_mask(exc, loc) folds them into one bit mask per location.

[crosscheck 2026-09-14] Revised after the differential cross-check against ps_B (crosscheck_ps.py): the
FP core evaluates every outcome the corpus admits ("Alternatives" section) and reports exactly the bits
that differ; each change is marked "[crosscheck fix]" with its page citation.

NOTE on sources: the Gekko manual names FPSCR fields (FX, FEX, VX, OX, ..., NI, RN) and gives
FPSCR[0]=FX, FPSCR[3]=OX (PAGE 473), FPSCR[15-19]=result bits (PAGE 83), FPSCR[29]=NI (PAGE 82),
but defers the remaining bit positions, the FPRF class codes and the RN encoding to the PowerPC
PEM (PAGE 2-... "see Floating-Point Status and Control Register (FPSCR) in Chapter 2 of the
PowerPC Microprocessor Family: The Programming Environments manual", manual line ~2766).  Those
three tables are taken from the PowerPC architecture naming convention and are marked [PEM]
below; they cannot be verified against the supplied manuals.
"""
from __future__ import annotations

import os
import struct
import sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gekko_state import MASK32, MASK64, State  # noqa: E402

AREA = "ps"


# ======================================================================================
# Undefined-behaviour plumbing
# ======================================================================================
class UndefinedBehaviour(Exception):
    def __init__(self, what, defined_state=None, undefined=("all",)):
        super().__init__(what)
        self.what = what
        self.defined_state = defined_state
        self.undefined = list(undefined)


def undefined_mask(exc, loc):
    """OR of the undefined bits a raised UndefinedBehaviour reports for one location:
    loc = 'fpr[n][k]' (64-bit), 'fpscr', 'cr', 'srr1', 'hid2', 'srr0', 'msr', 'pc', 'dar', 'dsisr'
    (32-bit), or 'mem:0xADDR' (8-bit).  'all' (whole instruction undefined) gives every bit."""
    full = MASK64 if loc.startswith("fpr[") else (0xFF if loc.startswith("mem:") else MASK32)
    m = 0
    for u in exc.undefined:
        base, _, sub = u.partition(":")
        if u == "all":
            return full
        if loc.startswith("mem:"):
            if u == loc:
                m |= 0xFF
            elif u == loc + "+sign":
                m |= 0x80
            continue
        if base != loc:
            continue
        if not sub:
            m |= full
        elif sub.startswith("0x"):
            m |= int(sub, 16)
        elif sub == "nan-payload":
            m |= ((1 << 22) - 1) << 29
        elif sub == "CR1":
            m |= 0x0F000000
        elif sub == "FPRF":
            m |= 0x1F << 12
        elif sub == "DCHERR":
            m |= 1 << 23
        else:
            m |= 1 << (31 - ["FX", "FEX", "VX", "OX", "UX", "ZX", "XX", "VXSNAN", "VXISI", "VXIDI", "VXZDZ",
                             "VXIMZ", "VXVC", "FR", "FI"].index(sub))
    return m


class _Total(Exception):
    """internal: the whole instruction is undefined (no defined parts)."""


# ======================================================================================
# Register bit constants (big-endian bit n == 1 << (31-n))
# ======================================================================================
def _b(n):
    return 1 << (31 - n)


# HID2, Table 2-6 PAGE 67: bit0 LSQE, bit1 WPE, bit2 PSE, bit3 LCE, bit8 DCHERR, bit12 DCHEE
HID2_LSQE, HID2_PSE, HID2_LCE = _b(0), _b(2), _b(3)
HID2_DCHERR, HID2_DCHEE = _b(8), _b(12)
# HID0 bit 17 DCE, PAGE 64
HID0_DCE = _b(17)
# MSR, Table 4-4 PAGE 164-165
MSR_POW, MSR_ILE, MSR_EE, MSR_PR, MSR_FP, MSR_ME = _b(13), _b(15), _b(16), _b(17), _b(18), _b(19)
MSR_FE0, MSR_SE, MSR_BE, MSR_FE1, MSR_IP = _b(20), _b(21), _b(22), _b(23), _b(25)
MSR_IR, MSR_DR, MSR_PM, MSR_RI, MSR_LE = _b(26), _b(27), _b(29), _b(30), _b(31)

# FPSCR [PEM layout; FX=0, OX=3 PAGE 473; result bits 15-19 PAGE 83; NI=29 PAGE 82]
FP = {
    "FX": _b(0), "FEX": _b(1), "VX": _b(2), "OX": _b(3), "UX": _b(4), "ZX": _b(5), "XX": _b(6),
    "VXSNAN": _b(7), "VXISI": _b(8), "VXIDI": _b(9), "VXZDZ": _b(10), "VXIMZ": _b(11),
    "VXVC": _b(12), "FR": _b(13), "FI": _b(14), "C": _b(15), "VXSOFT": _b(21), "VXSQRT": _b(22),
    "VXCVI": _b(23), "VE": _b(24), "OE": _b(25), "UE": _b(26), "ZE": _b(27), "XE": _b(28),
    "NI": _b(29),
}
FPRF_MASK = 0x1F << 12          # bits 15-19 PAGE 83
FPCC_MASK = 0xF << 12           # bits 16-19 (FPCC) PAGE 503 "FPCC <- c"
VX_BITS = ("VXSNAN", "VXISI", "VXIDI", "VXZDZ", "VXIMZ", "VXVC", "VXSOFT", "VXSQRT", "VXCVI")
# "the usual rule that FX is set when an exception bit changes from 0 to 1" PAGE 473
EXC_BITS = ("OX", "UX", "ZX", "XX") + VX_BITS
# FPRF class codes C,FL,FG,FE,FU [PEM]
FPRF_CODE = {"qnan": 0x11, "-inf": 0x09, "-normal": 0x08, "-denorm": 0x18, "-zero": 0x12,
             "+zero": 0x02, "+denorm": 0x14, "+normal": 0x04, "+inf": 0x05}

# ======================================================================================
# Single-precision value helpers (IEEE-754 binary32 carried as binary64 image)
# ======================================================================================
TWO = Fraction(2)
SMAX = Fraction((1 << 24) - 1) * TWO ** 104     # largest finite single
SMIN_NORMAL = TWO ** -126                       # smallest normal single
DENORM_Q = TWO ** -149                          # single denormal quantum
QUIET64 = 1 << 51
SIGN64 = 1 << 63
ONE_BITS = 0x3FF0000000000000                   # 1.0  (psq_l W=1 ps1 <- 1.0, PAGE 493)


def single_representable(bits):
    """True iff the u64 double image is exactly a binary32 value (incl. inf/NaN/denorm)."""
    ex = (bits >> 52) & 0x7FF
    fr = bits & ((1 << 52) - 1)
    if ex == 0x7FF:
        return (fr & ((1 << 29) - 1)) == 0
    if ex == 0:
        return fr == 0            # double denormals are never binary32 values
    e = ex - 1023
    if e > 127 or e < -149:
        return False
    if e >= -126:
        return (fr & ((1 << 29) - 1)) == 0
    need = -97 - e                # trailing zeros required in (2^52 | fr)
    return ((1 << 52) | fr) & ((1 << need) - 1) == 0


def widen32(w):
    """binary32 bits -> binary64 image of the same value (bit exact, NaNs keep signalling)."""
    s = (w >> 31) & 1
    ex = (w >> 23) & 0xFF
    fr = w & 0x7FFFFF
    if ex == 0xFF:
        return (s << 63) | (0x7FF << 52) | (fr << 29)
    if ex == 0:
        if fr == 0:
            return s << 63
        return bits_from(s, Fraction(fr) * DENORM_Q)
    return (s << 63) | ((ex - 127 + 1023) << 52) | (fr << 29)


def narrow64(bits):
    """binary64 image of a binary32 value -> binary32 bits (caller checked representable)."""
    s = bits >> 63
    ex = (bits >> 52) & 0x7FF
    fr = bits & ((1 << 52) - 1)
    if ex == 0x7FF:
        return (s << 31) | (0xFF << 23) | (fr >> 29)
    if ex == 0:
        return s << 31
    e = ex - 1023
    if e >= -126:
        return (s << 31) | ((e + 127) << 23) | (fr >> 29)
    m = Fraction((1 << 52) | fr) * TWO ** (e - 52)
    return (s << 31) | int(m / DENORM_Q)


def _flog2(m):
    n, d = m.numerator, m.denominator
    e = n.bit_length() - d.bit_length()
    if e >= 0:
        if n < (d << e):
            e -= 1
    elif (n << -e) < d:
        e -= 1
    return e


def bits_from(sign, mag):
    """exact binary64 image of sign*mag (mag must be exactly representable)."""
    if mag == 0:
        return sign << 63
    e = _flog2(mag)
    t = mag / TWO ** (e - 52)
    assert t.denominator == 1 and (1 << 52) <= t.numerator < (1 << 53), "not representable"
    return (sign << 63) | ((e + 1023) << 52) | (t.numerator - (1 << 52))


class V:
    __slots__ = ("kind", "sign", "mag", "quiet", "bits")

    def __init__(self, bits):
        self.bits = bits
        self.sign = bits >> 63
        ex = (bits >> 52) & 0x7FF
        fr = bits & ((1 << 52) - 1)
        self.mag = None
        self.quiet = None
        if ex == 0x7FF:
            if fr == 0:
                self.kind = "inf"
            else:
                self.kind = "nan"
                self.quiet = bool(fr & QUIET64)
        elif ex == 0 and fr == 0:
            self.kind = "zero"
        else:
            self.kind = "fin"
            self.mag = Fraction((1 << 52) | fr) * TWO ** (ex - 1075)

    @property
    def denorm(self):
        return self.kind == "fin" and self.mag < SMIN_NORMAL


def _cmp(a, b):
    """-1/0/1 for non-NaN V values (+0 == -0)."""
    def num(v):
        if v.kind == "zero":
            return (0, Fraction(0))
        if v.kind == "inf":
            return (-1 if v.sign else 1, Fraction(0))
        return (0, -v.mag if v.sign else v.mag)
    (ia, fa), (ib, fb) = num(a), num(b)
    if ia != ib:
        return -1 if ia < ib else 1
    if ia != 0:
        return 0
    return (fa > fb) - (fa < fb)


def fprf_class(bits):
    v = V(bits)
    sg = "-" if v.sign else "+"
    if v.kind == "nan":
        return "qnan"
    if v.kind == "inf":
        return sg + "inf"
    if v.kind == "zero":
        return sg + "zero"
    return sg + ("denorm" if v.denorm else "normal")


def _grid(m, q, sign, rn):
    t = m / q
    n = t.numerator // t.denominator
    rem = t - n
    if rem == 0:
        return n, False
    # RN encoding [PEM]: 0 nearest-even, 1 toward 0, 2 toward +inf, 3 toward -inf
    if rn == 0:
        if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and (n & 1)):
            n += 1
    elif rn == 2:
        if sign == 0:
            n += 1
    elif rn == 3:
        if sign == 1:
            n += 1
    return n, True


def round_single(sign, m, rn):
    """Round exact magnitude m>0 to single under RN ("rounded to single-precision under
    control of ... RN" PAGE 502).  Denormal results keep IEEE gradual underflow: "Return
    single-precision denormalized number with trailing zeros" Table 2-22 PAGE 85."""
    e = _flog2(m)
    qn = TWO ** (e - 23)
    nu, inex_u = _grid(m, qn, sign, rn)
    mu = nu * qn
    r = {"tiny_before": m < SMIN_NORMAL, "tiny_after": mu < SMIN_NORMAL}
    if mu > SMAX:
        r["overflow"] = True
        big = (rn == 0) or (rn == 2 and sign == 0) or (rn == 3 and sign == 1)
        r["bits"] = (sign << 63) | (0x7FF << 52) if big else bits_from(sign, SMAX)
        r["inexact"] = True
        r["fr"] = None
        return r
    r["overflow"] = False
    if r["tiny_before"]:
        n, inex = _grid(m, DENORM_Q, sign, rn)
        mm = n * DENORM_Q
    else:
        mm, inex = mu, inex_u
    r["bits"] = bits_from(sign, mm)
    r["inexact"] = inex
    r["fr"] = mm > m
    # IEEE 754-1985 7.4 lets "loss of accuracy" be detected as an inexact result or as a
    # denormalization loss (delivered result differs from the unbounded-exponent rounding); the
    # corpus does not choose, so both are reported.  [crosscheck fix]
    r["denorm_loss"] = r["tiny_before"] and mm != mu
    r["denorm_result"] = mm != 0 and mm < SMIN_NORMAL
    return r


# ======================================================================================
# Transaction: execute directly on the state, buffer memory writes, roll back on UB
# ======================================================================================
_SCALARS = ("cr", "xer", "lr", "ctr", "msr", "fpscr", "srr0", "srr1", "dar", "dsisr", "dec", "tb",
            "hid0", "hid2", "pc", "reserve", "reserve_addr", "exception")


class _Tx:
    def __init__(self, st):
        self.st = st
        self.snap = {k: getattr(st, k) for k in _SCALARS}
        self.gpr = list(st.gpr)
        self.fpr = [list(p) for p in st.fpr]
        self.gqr = list(st.gqr)
        self.log_len = len(st.mem.log)
        self.writes = []
        self.old_bytes = []
        self.undef = []
        self.reasons = []

    def read(self, addr, width):
        return self.st.mem.read(addr & MASK32, width)

    def write(self, addr, width, value):
        self.writes.append((addr & MASK32, width, value))

    def u(self, loc, why):
        if loc not in self.undef:
            self.undef.append(loc)
        self.reasons.append(why)

    def commit_mem(self):
        mem = self.st.mem
        for a, w, v in self.writes:
            for i in range(w):
                k = (a + i) & MASK32
                self.old_bytes.append((k, mem.bytes.get(k)))
            mem.write(a, w, v)

    def rollback(self):
        st = self.st
        for k, v in self.snap.items():
            setattr(st, k, v)
        st.gpr[:] = self.gpr
        for i, p in enumerate(self.fpr):
            st.fpr[i][:] = p
        st.gqr[:] = self.gqr
        for k, old in reversed(self.old_bytes):
            if old is None:
                st.mem.bytes.pop(k, None)
            else:
                st.mem.bytes[k] = old
        del st.mem.log[self.log_len:]


def _total(what):
    raise _Total(what)


# ======================================================================================
# Exception entry (PAGE 163, 166, 168)
# ======================================================================================
def _take(st, tx, name, vector):
    msr = st.msr
    # SRR0: "the address of the instruction that caused the exception" PAGE 163 (the "next
    # one" alternative is listed only for system call, trace and trap)
    st.srr0 = st.pc
    # "SRR1[5-9, 16-31] are loaded with a copy of the corresponding MSR bits" PAGE 166;
    # "SRR1[1-4, 10-15] are loaded with information specific to the exception type" PAGE 166,
    # which for program / FP-unavailable / alignment is deferred to PEM ch.6 (PAGE 175).
    # [crosscheck fix] SRR1[0] is defined too: MSR bit 0 is "Reserved. Full function" (Table 4-4
    # PAGE 164) and "Full function reserved bits are saved in SRR1 when an exception occurs"
    # (PAGE 165 note).  Only SRR1[1-4, 10-15] are undefined.
    st.srr1 = msr & 0x87C0FFFF
    tx.u("srr1:0x783F0000", "exception-specific SRR1[1-4,10-15] deferred to PEM PAGE 166/175")
    # Table 4-6 PAGE 168: POW EE PR FP FE0 SE BE FE1 IR DR PM RI cleared, LE <- ILE,
    # ILE ME IP unchanged; "Reserved bits are read as if written as 0"
    new = msr & (MSR_ILE | MSR_ME | MSR_IP)
    if msr & MSR_ILE:
        new |= MSR_LE
    st.msr = new
    # vector offset from the base selected by MSR[IP] PAGE 166
    st.pc = ((0xFFF00000 if msr & MSR_IP else 0) | vector) & MASK32
    st.exception = name


def _illegal(st, tx):
    _take(st, tx, "program", 0x700)       # illegal instruction -> program exception PAGE 90


def _fp_unavailable(st, tx):
    # "paired single instructions, including loads, stores and moves, cause a floating-point
    # unavailable exception if execution is attempted when MSR[FP] = 0" PAGE 83; vector 0x800
    # PAGE 175
    _take(st, tx, "fp_unavailable", 0x800)


def _alignment(st, tx, ea):
    _take(st, tx, "alignment", 0x600)     # vector 0x600 PAGE 175
    # [crosscheck fix] "After a DSI or an alignment exception, DAR is set to the effective address
    # (EA) generated by the faulting instruction" PAGE 59
    st.dar = ea & MASK32
    tx.u("dsisr", "alignment DSISR setting deferred to PEM PAGE 175 (PAGE 60 'See DSISR in PEM')")


# ======================================================================================
# Decoding
# ======================================================================================
A_XO = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0",
        15: "ps_madds1", 18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res",
        25: "ps_mul", 26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub",
        31: "ps_nmadd"}                                    # Table A-1 PAGE 583-584
X_XO = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr",
        96: "ps_cmpo1", 136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01",
        592: "ps_merge10", 624: "ps_merge11", 1014: "dcbz_l"}
# ps_merge10 is 1001010101 in Table A-1 PAGE 584 but 592 (x'1000 04A0') on PAGE 513 -> 592
XW_XO = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}   # PAGE 495-500
D_OP = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}      # PAGE 493-498


def supports(word):
    return ((word >> 26) & 0x3F) in (4, 56, 57, 60, 61)


def mnemonic(word):
    op = (word >> 26) & 0x3F
    if op in D_OP:
        return D_OP[op]
    if op != 4:
        return None
    if ((word >> 1) & 0x1F) in A_XO:
        return A_XO[(word >> 1) & 0x1F]
    if ((word >> 1) & 0x3FF) in X_XO:
        return X_XO[(word >> 1) & 0x3FF]
    if ((word >> 1) & 0x3F) in XW_XO:
        return XW_XO[(word >> 1) & 0x3F]
    return "illegal"


def _f(w, a, b):
    """big-endian bit field a..b"""
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


# reserved-field requirements from the instruction figures (PAGE 501-531) and Table A-1
_RESERVED = {
    "ps_div": [(21, 25)], "ps_sub": [(21, 25)], "ps_add": [(21, 25)],
    "ps_mul": [(16, 20)], "ps_muls0": [(16, 20)], "ps_muls1": [(16, 20)],
    "ps_res": [(11, 15), (21, 25)], "ps_rsqrte": [(11, 15), (21, 25)],
    "ps_neg": [(11, 15)], "ps_mr": [(11, 15)], "ps_nabs": [(11, 15)], "ps_abs": [(11, 15)],
    "ps_cmpu0": [(9, 10), (31, 31)], "ps_cmpo0": [(9, 10), (31, 31)],
    "ps_cmpu1": [(9, 10), (31, 31)], "ps_cmpo1": [(9, 10), (31, 31)],
    "dcbz_l": [(6, 10), (31, 31)],
    "psq_lx": [(31, 31)], "psq_stx": [(31, 31)], "psq_lux": [(31, 31)], "psq_stux": [(31, 31)],
}


# ======================================================================================
# Alternatives.  [crosscheck fix, replaces the single-outcome "Half" bookkeeping]
# Wherever the corpus leaves a choice open (NaN payload and priority, tininess and loss of
# accuracy, the sign of an NI zero, enabled-exception results, completion of an instruction that
# takes a precise floating-point enabled exception) the computation yields every admissible
# concrete outcome.  A location is defined iff it has the same value in all of them; bits that
# differ are reported undefined.  Every FPSCR / CR1 output is a monotone function of the sticky
# flags and of FR / FI, so a flag whose setting is undefined is represented exactly by its two
# extreme settings.
# ======================================================================================
from itertools import product as _product  # noqa: E402

QNAN_CODE = FPRF_CODE["qnan"]
PAYLOAD64 = ((1 << 22) - 1) << 29          # single NaN payload bits below the quiet bit
INF64 = 0x7FF << 52


class Alt:
    __slots__ = ("bits", "vmask", "flags", "fr", "fi", "fprf", "suppress")

    def __init__(self, bits=0, vmask=0, flags=(), fr=0, fi=0, fprf=None, suppress=False):
        self.bits = bits & MASK64
        self.vmask = vmask & MASK64       # undefined bits of the stored image
        self.flags = frozenset(flags)     # sticky exception bits set
        self.fr = fr
        self.fi = fi
        self.fprf = fprf                  # 5-bit class code, None = not defined by the corpus
        self.suppress = suppress          # "No result" (VE=1 invalid / ZE=1 zero divide)


def _code(bits):
    return FPRF_CODE[fprf_class(bits)]


def _alts(bits, vmask=0, flags=(), undef_flags=(), fr=0, fi=0, fprf="auto", suppress=False):
    """fr / fi None = undefined; undef_flags = sticky bits whose setting is undefined."""
    if fprf == "auto":
        fprf = _code(bits)
    lo = Alt(bits, vmask, flags, 0 if fr is None else fr, 0 if fi is None else fi, fprf, suppress)
    if not undef_flags and fr is not None and fi is not None:
        return [lo]
    hi = Alt(bits, vmask, set(flags) | set(undef_flags), 1 if fr is None else fr,
             1 if fi is None else fi, fprf, suppress)
    return [lo, hi]


def _no_result(flags, undef_flags=()):
    # "* No result if FPSCR[ZE] = 1", "** No result if FPSCR[VE] = 1" PAGE 525/526; "FPSCR[FPRF] is
    # set to the class and sign of the ps0 result, except for invalid operation exceptions when
    # FPSCR[VE] = 1 and zero divide exceptions when FPSCR[ZE] = 1" PAGE 507: the corpus gives
    # neither the target half nor FPRF, FR, FI in that case.
    return _alts(0, MASK64, flags, undef_flags, None, None, None, True)


def _generated_qnan(flag, fpscr, kind, undef_flags=()):
    if fpscr & FP["VE"]:
        return _no_result({flag}, undef_flags)
    # Disabled invalid operation: the result is a QNaN ("Return QNaN" Table 2-22 PAGE 85).  The
    # default QNaN image is a PEM chapter 3 definition that is not in the corpus, so only the
    # all-ones exponent and the quiet bit are defined.  Its sign is defined only where stated:
    # "QNaNs that are generated as the result of a disabled invalid operation exception have a
    # sign bit of zero" PAGE 522 (ps_nmadd) and PAGE 524 (ps_nmsub).  FR = FI = 0: the result is
    # neither incremented nor inexact (FR/FI definitions PAGE 399).
    vmask = PAYLOAD64 | (0 if kind in ("nmadd", "nmsub") else SIGN64)
    return _alts(0x7FF8000000000000, vmask, {flag}, undef_flags, 0, 0, QNAN_CODE)


def _nan_alts(nans, flags, undef_flags):
    # One NaN operand: a QNaN propagates ("QNaNs propagate with no effect on their sign bit" PAGE
    # 522/524; Table 2-21 PAGE 84-85 result "QNaN"); an SNaN is converted to a QNaN that "retain[s]
    # the sign bit of the SNaN" (PAGE 522) with the quiet bit set - how its remaining payload is
    # converted is not stated (IEEE 754-1985 only recommends preserving it).
    # Several NaN operands: "Prioritize according to Chapter 3, 'Operand Conventions,' in the
    # PowerPC Microprocessor Family: The Programming Environments manual" (Table 2-21 footnote
    # PAGE 85), not in the corpus -> every NaN operand is an admissible result.
    out, seen = [], set()
    for v in nans:
        if v.bits in seen:
            continue
        seen.add(v.bits)
        out += _alts(v.bits | QUIET64, 0 if v.quiet else PAYLOAD64, flags, undef_flags, 0, 0, QNAN_CODE)
    return out


def _finite_alts(sign, m, fpscr, neg):
    rn = fpscr & 3
    r = round_single(sign, m, rn)
    ue, oe, ni = fpscr & FP["UE"], fpscr & FP["OE"], fpscr & FP["NI"]

    def fin(bits, **kw):
        # ps_nmadd/ps_nmsub: "rounded to single-precision ..., then negated" PAGE 524, "the same
        # result as ... ps_madd and then negating the result" PAGE 522
        return _alts(bits ^ SIGN64 if neg else bits, **kw)

    if r["overflow"]:
        if oe:
            # enabled overflow: the delivered value (PEM exponent adjustment) is not in the corpus
            return fin(r["bits"], vmask=MASK64, flags={"OX"}, undef_flags={"XX"}, fr=None, fi=None,
                       fprf=None)
        # disabled overflow: +-inf or +-max by RN (IEEE 754), OX and XX; FR ("set if the result is
        # incremented when rounded" PAGE 399) has no meaning for an overflowed result
        return fin(r["bits"], flags={"OX", "XX"}, fr=None, fi=1)
    inexact = r["inexact"]
    ieee = []
    # tininess "before" or "after" rounding: IEEE 754-1985 leaves the choice, the corpus is silent
    for tiny in {r["tiny_before"], r["tiny_after"]}:
        if ue and tiny:
            # enabled underflow: delivered value not in the corpus
            ieee += fin(r["bits"], vmask=MASK64, flags={"UX"}, undef_flags={"XX"}, fr=None, fi=None,
                        fprf=None)
            continue
        losses = {False} if ue else {inexact, r["denorm_loss"]}
        for loss in losses:
            fl = {"XX"} if inexact else set()
            if tiny and loss:
                fl.add("UX")
            ieee += fin(r["bits"], flags=fl, fr=int(r["fr"]), fi=int(inexact))
    if not ni:
        return ieee
    # NI = 1: "Single Denormalized -> Return zero" Table 2-22 PAGE 85.  Whether a result is
    # "denormalized" before or after rounding is not stated; the sign of the zero and the
    # exception bits are not given.
    out = []
    classes = {r["denorm_result"], r["tiny_before"]}
    if False in classes or ue:
        out += ieee
    if True in classes:
        for s in (0, 1):
            out += _alts(s << 63, 0, (), {"UX", "XX"}, None, None)
    return out


def _compute(kind, vs, fpscr):
    """IEEE arithmetic on non-NaN operands.  kind in add sub mul div madd msub nmadd nmsub;
    vs = operand values in the instruction's natural order: add/sub/div (A,B), mul (A,C),
    madd family (A,C,B)."""
    neg = kind in ("nmadd", "nmsub")
    rn = fpscr & 3

    def zero_sum_sign(sx, sy):
        # IEEE-754: x + y exact zero: same signs keep sign, otherwise +0 (-0 when rounding
        # toward -inf)
        if sx == sy:
            return sx
        return 1 if rn == 3 else 0

    def add_like(x, y):   # x, y: tuples (kind, sign, value)
        kx, sx, vx = x
        ky, sy, vy = y
        if kx == "inf" and ky == "inf":
            if sx != sy:
                return ("invalid", "VXISI")
            return ("inf", sx)
        if kx == "inf":
            return ("inf", sx)
        if ky == "inf":
            return ("inf", sy)
        s = vx + vy
        if s == 0:
            if kx == "zero" and ky == "zero":
                return ("zero", zero_sum_sign(sx, sy))
            return ("zero", 1 if rn == 3 else 0)
        return ("fin", 1 if s < 0 else 0, s)

    def tup(v, flip=False):
        s = v.sign ^ (1 if flip else 0)
        if v.kind == "inf":
            return ("inf", s, None)
        if v.kind == "zero":
            return ("zero", s, Fraction(0))
        return ("fin", s, -v.mag if s else v.mag)

    def mul_like(x, y):
        kx, sx, vx = x
        ky, sy, vy = y
        s = sx ^ sy
        if (kx == "inf" and ky == "zero") or (kx == "zero" and ky == "inf"):
            return ("invalid", "VXIMZ")
        if kx == "inf" or ky == "inf":
            return ("inf", s, None)
        if kx == "zero" or ky == "zero":
            return ("zero", s, Fraction(0))
        return ("fin", s, vx * vy)

    if kind in ("add", "sub"):
        res = add_like(tup(vs[0]), tup(vs[1], flip=(kind == "sub")))
    elif kind == "mul":
        res = mul_like(tup(vs[0]), tup(vs[1]))
    elif kind == "div":
        a, b = vs
        s = a.sign ^ b.sign
        if a.kind == "inf" and b.kind == "inf":
            res = ("invalid", "VXIDI")
        elif a.kind == "zero" and b.kind == "zero":
            res = ("invalid", "VXZDZ")
        elif a.kind == "inf":
            # [crosscheck fix] inf / 0 = inf exactly, no ZX: IEEE 754-1985 7.2 signals division by zero
            # only "if the divisor is zero and the dividend is a finite nonzero number" (the corpus
            # has no definition of ZX of its own; cf. the exception-free "+inf -> +0" row PAGE 525)
            res = ("inf", s)
        elif b.kind == "zero":
            res = ("zerodiv", s)
        elif b.kind == "inf" or a.kind == "zero":
            res = ("zero", s)
        else:
            q = a.mag / b.mag
            res = ("fin", s, -q if s else q)
    else:
        p = mul_like(tup(vs[0]), tup(vs[1]))
        if p[0] == "invalid":
            res = p
        else:
            res = add_like(p, tup(vs[2], flip=kind in ("msub", "nmsub")))

    tag = res[0]
    if tag == "invalid":
        return _generated_qnan(res[1], fpscr, kind)
    if tag == "zerodiv":
        if fpscr & FP["ZE"]:
            return _no_result({"ZX"})
        return _alts((res[1] << 63) | INF64, 0, {"ZX"})
    if tag == "inf":
        bits = (res[1] << 63) | INF64
    elif tag == "zero":
        bits = res[1] << 63
    else:
        return _finite_alts(res[1], abs(res[2]), fpscr, neg)
    return _alts(bits ^ SIGN64 if neg else bits)


def _arith_alts(kind, ops, fpscr):
    vs = [V(b) for b in ops]
    ni = fpscr & FP["NI"]
    nans = [v for v in vs if v.kind == "nan"]
    if nans:
        # Table 2-21 PAGE 84-85: a NaN operand gives "QNaN" in both IEEE and non-IEEE mode with
        # the other operands "Don't care"
        flags = {"VXSNAN"} if any(not v.quiet for v in nans) else set()
        undef = set()
        if kind in ("madd", "msub", "nmadd", "nmsub"):
            ka = "zero" if (ni and vs[0].denorm) else vs[0].kind
            kc = "zero" if (ni and vs[1].denorm) else vs[1].kind
            if {ka, kc} == {"zero", "inf"}:
                # infinity * 0 with a NaN addend: PAGE 508 lists VXIMZ but not its interaction
                # with a NaN operand
                undef.add("VXIMZ")
        if flags and fpscr & FP["VE"]:
            return _no_result(flags, undef)
        return _nan_alts(nans, flags, undef)
    zi = [i for i, v in enumerate(vs) if v.denorm] if ni else []
    if not zi:
        return _compute(kind, vs, fpscr)
    # NI = 1, denormalized operand: "Zero A" / "Zero B" / ... Table 2-21 PAGE 84 then "Do the
    # operation"; the sign of the zero is not given, nor whether replacing the operand is itself
    # signalled (PAGE 82: results "approximate the values required by the IEEE standard").
    out = []
    for signs in _product((0, 1), repeat=len(zi)):
        vv = list(vs)
        for i, s in zip(zi, signs):
            vv[i] = V(s << 63)
        for a in _compute(kind, vv, fpscr):
            out += _alts(a.bits, a.vmask, a.flags, {"UX", "XX"}, None, None, a.fprf, a.suppress)
    return out


def _need_single(tx, bits, what):
    if not single_representable(bits):
        _total("paired-single instruction applied to a double-precision operand (%s): "
               "'It is a programming error to apply double-precision instructions to paired "
               "single operands and vice versa' PAGE 83" % what)


def _fx_update(old, new):
    """FX set on 0->1 transition of an exception bit (PAGE 473 'usual rule'); VX and FEX
    summaries recomputed (PAGE 473 'FEX and VX are set according to the usual rule')."""
    for n in EXC_BITS:
        if (new & FP[n]) and not (old & FP[n]):
            new |= FP["FX"]
    if any(new & FP[n] for n in VX_BITS):
        new |= FP["VX"]
    else:
        new &= ~FP["VX"]
    fex = ((new & FP["VX"]) and (new & FP["VE"])) or ((new & FP["OX"]) and (new & FP["OE"])) or \
          ((new & FP["UX"]) and (new & FP["UE"])) or ((new & FP["ZX"]) and (new & FP["ZE"])) or \
          ((new & FP["XX"]) and (new & FP["XE"]))
    new = (new | FP["FEX"]) if fex else (new & ~FP["FEX"])
    return new & MASK32


def _cr1(st, tx):
    # "when RC=1, the CR1 field of the CR is set from FPSCR bits 0-3" PAGE 83
    st.cr = (st.cr & ~0x0F000000) | ((st.fpscr >> 28) << 24)


def _fe_possible(st, results):
    # "If either bit [MSR FE0/FE1] is set, all IEEE enabled floating-point exceptions are taken and
    # cause a program exception" PAGE 166; the program exception register settings are deferred
    # to the PEM (PAGE 175).  A precise exception means "neither the faulting instruction nor
    # subsequent instructions ... will complete execution" PAGE 46, where completing "implies
    # committing execution results to the architected destination registers" PAGE 223 - which
    # contradicts the instruction pages' unconditional FPSCR / frD updates.  So: whether and how
    # the exception is taken is undefined, and so is every register bit the instruction commits.
    return bool(st.msr & (MSR_FE0 | MSR_FE1)) and any(r[1] & FP["FEX"] for r in results)


def _mark_fe(tx):
    for loc in ("exception", "srr0", "srr1", "msr", "pc"):
        tx.u(loc, "precise FP enabled program exception: taken-ness and register settings deferred to "
                  "PEM (PAGE 166, PAGE 175, PAGE 46, PAGE 223)")


def _reduce(results):
    """results: list of tuples of ints (value tuple) and mask tuple -> (values, masks)."""
    v0, m0 = results[0]
    masks = list(m0)
    for v, m in results[1:]:
        for i in range(len(v0)):
            masks[i] |= (v[i] ^ v0[i]) | m[i]
    return v0, masks


def _combine(st, tx, d, halves, fprf_src, rc):
    """halves: {k: [Alt, ...]} for k = ps index.  Applies frD, FPSCR and CR1 with undefined bits."""
    old_fpscr, old_cr = st.fpscr, st.cr
    old_fpr = list(st.fpr[d])
    keys = sorted(halves)
    results = []
    for combo in _product(*(halves[k] for k in keys)):
        by = dict(zip(keys, combo))
        new = old_fpscr
        um = 0
        fr = fi = 0
        suppress = False
        for a in combo:
            for n in a.flags:
                new |= FP[n]
            fr |= a.fr
            fi |= a.fi
            suppress |= a.suppress
        # "FPSCR bits that reflect exceptional conditions ... bits 0-14, and 22-23 ... either the
        # ps0 or the ps1 computation can set the bit" PAGE 83 (FR = 13, FI = 14 included -> OR)
        new = (new & ~(FP["FR"] | FP["FI"])) | (FP["FR"] if fr else 0) | (FP["FI"] if fi else 0)
        # "FPSCR bits 15-19 are the result bits. They are determined by the result of the ps0
        # computation, except for ps_cmpu1, ps_cmpo1 and ps_sum1" PAGE 83; "... except for invalid
        # operation exceptions when FPSCR[VE] = 1" PAGE 502 does not say which half's exception
        # suspends the rule, so a suppressed result in either half leaves FPRF undefined.
        src = by[fprf_src]
        if suppress or src.fprf is None:
            um |= FPRF_MASK
        else:
            new = (new & ~FPRF_MASK) | (src.fprf << 12)
        new = _fx_update(old_fpscr, new)
        fpr0, fpr1 = old_fpr
        fm = [0, 0]
        if 0 in by:
            fpr0, fm[0] = by[0].bits, by[0].vmask
        if 1 in by:
            fpr1, fm[1] = by[1].bits, by[1].vmask
        cr = old_cr
        if rc:
            cr = (cr & ~0x0F000000) | ((new >> 28) << 24)
        results.append(((fpr0, fpr1, new, cr), (fm[0], fm[1], um, 0)))
    if _fe_possible(st, [(v[2], v[2]) for v, _ in results]):
        _mark_fe(tx)
        results.append(((old_fpr[0], old_fpr[1], old_fpscr, old_cr), (0, 0, 0, 0)))
    (f0, f1, fps, cr), (m0, m1, mf, mc) = _reduce(results)
    for k, v, m in ((0, f0, m0), (1, f1, m1)):
        st.fpr[d][k] = v & ~m & MASK64
        if m:
            tx.u("fpr[%d][%d]:0x%016X" % (d, k, m), "result bits left open: NaN payload/priority (PAGE 85, 522), "
                 "QNaN image (PAGE 85), tininess/NI zero (PAGE 84-85), enabled exceptions (PAGE 502-531, 166)")
    st.fpscr = fps & ~mf & MASK32
    if mf:
        tx.u("fpscr:0x%08X" % mf, "FPSCR bits left open by the alternatives (PAGE 83, 85, 502-531)")
    st.cr = cr & ~mc & MASK32
    if mc:
        tx.u("cr:0x%08X" % mc, "CR1 copies open FPSCR bits (PAGE 83)")


# ======================================================================================
# Instruction handlers
# ======================================================================================
def _h_arith(st, tx, w, name):
    d, a, b, c, rc = _f(w, 6, 10), _f(w, 11, 15), _f(w, 16, 20), _f(w, 21, 25), w & 1
    F = st.fpr
    fps = st.fpscr
    # read all operands before any write (aliasing)
    if name in ("ps_add", "ps_sub", "ps_div"):
        kind = name[3:]
        pairs = {0: (F[a][0], F[b][0]), 1: (F[a][1], F[b][1])}          # PAGE 502/529/507
    elif name == "ps_mul":
        kind = "mul"
        pairs = {0: (F[a][0], F[c][0]), 1: (F[a][1], F[c][1])}          # PAGE 517
    elif name == "ps_muls0":
        kind = "mul"
        pairs = {0: (F[a][0], F[c][0]), 1: (F[a][1], F[c][0])}          # PAGE 518
    elif name == "ps_muls1":
        kind = "mul"
        pairs = {0: (F[a][0], F[c][1]), 1: (F[a][1], F[c][1])}          # PAGE 519
    elif name in ("ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub"):
        kind = name[3:]
        pairs = {0: (F[a][0], F[c][0], F[b][0]), 1: (F[a][1], F[c][1], F[b][1])}  # PAGE 508-524
    elif name == "ps_madds0":
        kind = "madd"
        pairs = {0: (F[a][0], F[c][0], F[b][0]), 1: (F[a][1], F[c][0], F[b][1])}  # PAGE 509
    elif name == "ps_madds1":
        kind = "madd"
        pairs = {0: (F[a][0], F[c][1], F[b][0]), 1: (F[a][1], F[c][1], F[b][1])}  # PAGE 510
    else:
        raise AssertionError(name)
    for k, ops in pairs.items():
        for i, o in enumerate(ops):
            _need_single(tx, o, "%s ps%d operand %d" % (name, k, i))
    halves = {k: _arith_alts(kind, ops, fps) for k, ops in pairs.items()}
    _combine(st, tx, d, halves, 0, rc)


def _h_sum(st, tx, w, name):
    d, a, b, c, rc = _f(w, 6, 10), _f(w, 11, 15), _f(w, 16, 20), _f(w, 21, 25), w & 1
    F = st.fpr
    if name == "ps_sum0":
        # frD(ps0) <- frA(ps0) + frB(ps1); frD(ps1) <- frC(ps1)  PAGE 530
        x, y, mv = F[a][0], F[b][1], F[c][1]
        k_ar, k_mv = 0, 1
    else:
        # frD(ps0) <- frC(ps0); frD(ps1) <- frA(ps0) + frB(ps1)  PAGE 531
        x, y, mv = F[a][0], F[b][1], F[c][0]
        k_ar, k_mv = 1, 0
    for o in (x, y, mv):
        _need_single(tx, o, name)
    halves = {k_ar: _arith_alts("add", (x, y), st.fpscr), k_mv: [Alt(mv, 0, (), 0, 0, _code(mv))]}
    _combine(st, tx, d, halves, k_ar, rc)


def _h_sel(st, tx, w, name):
    d, a, b, c, rc = _f(w, 6, 10), _f(w, 11, 15), _f(w, 16, 20), _f(w, 21, 25), w & 1
    F = st.fpr
    out, masks = [], []
    for k in (0, 1):
        _need_single(tx, F[a][k], "ps_sel frA")
        va = V(F[a][k])
        # "If the operand is greater than or equal to zero ... frC; less than zero or is a
        # NaN ... frB"; "ignore the sign of zero" PAGE 528
        ge = {va.kind == "zero" or (va.kind in ("fin", "inf") and va.sign == 0)}
        if st.fpscr & FP["NI"] and va.denorm:
            # NI = 1: a denormalized operand is replaced by zero ("Zero A" Table 2-21 PAGE 84) in the
            # arithmetic operand model; whether ps_sel's comparison sees the zeroed operand is not
            # stated (PAGE 528 pseudocode is mode-independent) -> both selections admissible
            # [crosscheck fix]
            ge.add(True)
        vals = []
        for g in sorted(ge, reverse=True):
            src = F[c][k] if g else F[b][k]
            _need_single(tx, src, "ps_sel source")
            vals.append(src)
        m = 0
        for v in vals[1:]:
            m |= v ^ vals[0]
        out.append(vals[0] & ~m)
        masks.append(m)
    F[d][0], F[d][1] = out
    for k in (0, 1):
        if masks[k]:
            tx.u("fpr[%d][%d]:0x%016X" % (d, k, masks[k]), "ps_sel selection under NI=1 not defined (PAGE 84, 528)")
    if rc:
        _cr1(st, tx)                        # no FPSCR bits altered (only CR1 listed) PAGE 528


def _h_move(st, tx, w, name):
    d, b, rc = _f(w, 6, 10), _f(w, 16, 20), w & 1
    F = st.fpr
    vals = [F[b][0], F[b][1]]
    for v in vals:
        _need_single(tx, v, name)
    if name == "ps_mr":
        out = vals                                          # PAGE 515
    elif name == "ps_neg":
        out = [v ^ SIGN64 for v in vals]                    # bit 0 inverted PAGE 521
    elif name == "ps_abs":
        out = [v & ~SIGN64 for v in vals]                   # bit 0 cleared PAGE 501
    else:
        out = [v | SIGN64 for v in vals]                    # bit 0 set PAGE 520
    F[d][0], F[d][1] = [o & MASK64 for o in out]
    # "This instruction does not alter the FPSCR" PAGE 501/520/521
    if rc:
        _cr1(st, tx)


def _h_merge(st, tx, w, name):
    d, a, b, rc = _f(w, 6, 10), _f(w, 11, 15), _f(w, 16, 20), w & 1
    F = st.fpr
    sel = {"ps_merge00": ((a, 0), (b, 0)), "ps_merge01": ((a, 0), (b, 1)),   # PAGE 511/512
           "ps_merge10": ((a, 1), (b, 0)), "ps_merge11": ((a, 1), (b, 1))}[name]  # PAGE 513/514
    v0 = F[sel[0][0]][sel[0][1]]
    v1 = F[sel[1][0]][sel[1][1]]
    _need_single(tx, v0, name)
    _need_single(tx, v1, name)
    F[d][0], F[d][1] = v0, v1
    if rc:
        _cr1(st, tx)


def _h_cmp(st, tx, w, name):
    crf, a, b = _f(w, 6, 8), _f(w, 11, 15), _f(w, 16, 20)
    k = 0 if name.endswith("0") else 1                       # High=ps0, Low=ps1 PAGE 503-506
    x, y = st.fpr[a][k], st.fpr[b][k]
    _need_single(tx, x, name)
    _need_single(tx, y, name)
    vx, vy = V(x), V(y)
    old, old_cr = st.fpscr, st.cr
    cands = [(vx, vy)]
    if old & FP["NI"] and (vx.denorm or vy.denorm):
        # NI = 1: whether the comparison sees a denormalized operand zeroed ("Zero A" Table 2-21
        # PAGE 84) is not stated; the PAGE 503-506 pseudocode is mode-independent [crosscheck fix]
        cands.append((V(0) if vx.denorm else vx, V(0) if vy.denorm else vy))
    snan = (vx.kind == "nan" and not vx.quiet) or (vy.kind == "nan" and not vy.quiet)
    qnan = (vx.kind == "nan" and vx.quiet) or (vy.kind == "nan" and vy.quiet)
    flags = 0
    if snan:
        flags |= FP["VXSNAN"]
        if name.startswith("ps_cmpo") and not (old & FP["VE"]):
            flags |= FP["VXVC"]                               # PAGE 503
    elif qnan and name.startswith("ps_cmpo"):
        flags |= FP["VXVC"]
    sh = 28 - 4 * crf
    results = []
    for p, q in cands:
        if p.kind == "nan" or q.kind == "nan":
            c = 0b0001
        else:
            r = _cmp(p, q)
            c = 0b1000 if r < 0 else (0b0100 if r > 0 else 0b0010)
        new = _fx_update(old, (old & ~FPCC_MASK) | (c << 12) | flags)   # FPCC <- c
        cr = (old_cr & ~(0xF << sh)) | (c << sh)                        # CR[4*crfD..4*crfD+3] <- c
        results.append(((new, cr), (0, 0)))
    if _fe_possible(st, [(v[0], v[0]) for v, _ in results]):
        _mark_fe(tx)
        results.append(((old, old_cr), (0, 0)))
    (fps, cr), (mf, mc) = _reduce(results)
    st.fpscr = fps & ~mf & MASK32
    st.cr = cr & ~mc & MASK32
    if mf:
        tx.u("fpscr:0x%08X" % mf, "compare under NI=1 / FE mode left open (PAGE 84, 166, 503-506)")
    if mc:
        tx.u("cr:0x%08X" % mc, "compare under NI=1 / FE mode left open (PAGE 84, 166, 503-506)")


def _estimate_one(name, v, fpscr):
    if v.kind == "zero":
        # -0 -> -inf ZX, +0 -> +inf ZX; "* No result if FPSCR[ZE] = 1" PAGE 525/526
        if fpscr & FP["ZE"]:
            return _no_result({"ZX"})
        return _alts((v.sign << 63) | INF64, 0, {"ZX"})
    if v.kind == "inf" and (name == "ps_res" or v.sign == 0):
        return _alts(v.sign << 63)                            # +-inf -> +-0 ; +inf -> +0
    if name == "ps_rsqrte" and v.sign == 1:
        return _generated_qnan("VXSQRT", fpscr, "rsqrte")      # -inf / <0 -> QNaN, VXSQRT PAGE 526
    # "correct to a precision of one part in 4096" PAGE 525/526; the exact value is implementation
    # specific ("the value placed into register frD may vary between implementations" PAGE 416)
    if name == "ps_res":
        exact = 1 / v.mag
        lo, hi = exact * Fraction(4095, 4096), exact * Fraction(4097, 4096)
        if lo >= SMIN_NORMAL and hi <= SMAX:
            return _alts(0, MASK64, (), (), 0, 0, FPRF_CODE[("-" if v.sign else "+") + "normal"])
        # the estimate may overflow or underflow the single range: OX / UX and FPRF not determined
        return _alts(0, MASK64, (), {"OX", "UX"}, 0, 0, None)
    # 1/sqrt(x) of a positive single lies in [2^-64, 2^74.5]: always a normal single
    return _alts(0, MASK64, (), (), 0, 0, FPRF_CODE["+normal"])


def _h_estimate(st, tx, w, name):
    d, b, rc = _f(w, 6, 10), _f(w, 16, 20), w & 1
    F = st.fpr
    fps = st.fpscr
    ops = [F[b][0], F[b][1]]
    for o in ops:
        _need_single(tx, o, name)
    halves = {}
    for k, o in enumerate(ops):
        v = V(o)
        if v.kind == "nan":
            # SNaN -> QNaN VXSNAN ("** No result if FPSCR[VE] = 1"), QNaN -> QNaN PAGE 525/526
            flags = set() if v.quiet else {"VXSNAN"}
            alts = _no_result(flags) if (flags and fps & FP["VE"]) else _nan_alts([v], flags, ())
        else:
            # FPSCR[NI] = 1 does NOT flush the operand of an estimate. Table 2-21 (PAGE 84) says a
            # denormalized operand is replaced by zero, but the hardware-verified behaviour (hwtests
            # cputest/ni.cpp, inputs_expected_flushed = false; Common/FloatUtils.h fres_expected /
            # frsqrte_expected, compared against a real Gekko) estimates the denormal itself: a single
            # denormal operand has |x| < 2^-126, so 1/x is far above the single range (ps_res then
            # delivers the largest single, or an estimate of the true reciprocal at the boundary) and a
            # negative one is an invalid square root (QNaN), not -0 -> -inf.  The generic path below
            # bounds exactly that: the true value, no zero replacement.
            alts = _estimate_one(name, v, fps)
        # "FR (undefined), FI (undefined)" PAGE 525/527; XX is not in either Affected list
        halves[k] = [x for a in alts for x in _alts(a.bits, a.vmask, a.flags, (), None, None, a.fprf,
                                                    a.suppress)]
    _combine(st, tx, d, halves, 0, rc)


def _gqr_fields(g):
    # Table 2-16 PAGE 78: 2-7 LD_SCALE, 13-15 LD_TYPE, 18-23 ST_SCALE, 29-31 ST_TYPE
    return {"ld_scale": _f(g, 2, 7), "ld_type": _f(g, 13, 15),
            "st_scale": _f(g, 18, 23), "st_type": _f(g, 29, 31)}


def _scale(s6):
    # "S is the twos compliment value in the LD_SCALE field" PAGE 110; 100000 = -32 PAGE 111
    return s6 - 64 if s6 & 0x20 else s6


def _width(t):
    # Table 2-17 PAGE 78: 0 single float (4 bytes), 4 u8, 5 u16, 6 s8, 7 s16.  The PAGE 493
    # pseudocode reads "c <- 10 / 20" for the integer types; 750CL PAGE 567 reads "c <- 1 / 2"
    # and PAGE 109 says "8 or 16 bit integers" -> byte widths 1 and 2.
    if t in (1, 2, 3):
        _total("GQR quantized type %d is reserved (Table 2-17 PAGE 78)" % t)
    return {0: 4, 4: 1, 5: 2, 6: 1, 7: 2}[t]


def _dequantize(raw, t, s6):
    if t == 0:
        # "the value from the L1 cache is passed directly to the register without any
        # conversion. This includes the case where the operand is a denorm." PAGE 111
        return widen32(raw)
    if t in (6, 7):
        bitsz = 8 if t == 6 else 16
        if raw & (1 << (bitsz - 1)):
            raw -= 1 << bitsz
    # "F = I * 2**(-S)" PAGE 110 (exact for 8/16-bit I and |S| <= 32)
    f = Fraction(raw) * TWO ** (-_scale(s6))
    return bits_from(1 if f < 0 else 0, abs(f))


_QLIM = {4: (0, 255), 5: (0, 65535), 6: (-128, 127), 7: (-32768, 32767)}   # PAGE 111


def _quantize(tx, bits, t, s6, addr):
    if not single_representable(bits):
        _total("psq store of a double-precision operand: 'loading an operand as a double and "
               "then storing it as a paired single will not yield the original value' PAGE 83")
    v = V(bits)
    if t == 0:
        if v.denorm:
            # "In the case of a denorm, the value 0.0 is stored in the L1 cache." PAGE 112
            if v.sign:
                tx.u("mem:0x%08X+sign" % addr, "sign of the 0.0 stored for a negative denorm "
                     "not specified PAGE 112")
            return 0
        return narrow64(bits)
    lo, hi = _QLIM[t]
    if v.kind == "nan":
        # "The converted value produced when the input operand is +Inf or NaN is the same as
        # the adjusted result value for overflow of positive values" PAGE 111.
        # [crosscheck fix] 750CL PAGE 108 refines this to "+Inf or a NaN with a '0' in the high
        # order bit" -> positive value and "-Inf or a NaN with a '1' in the high order bit" ->
        # negative value.  Both manuals agree for a positive NaN; for a negative NaN they
        # contradict each other, so the stored integer is undefined.
        n = hi
        if v.sign:
            for i in range(1 if t in (4, 6) else 2):
                tx.u("mem:0x%08X" % ((addr + i) & MASK32), "quantized negative NaN: Gekko PAGE 111 vs "
                     "750CL PAGE 108 disagree")
    elif v.kind == "inf":
        n = lo if v.sign else hi                              # -Inf -> negative overflow
    elif v.kind == "zero":
        n = 0
    else:
        # I = ROUND(F * 2**S), "Round toward zero", "Adjust the resulting value on overflow"
        # PAGE 111-112
        x = v.mag * TWO ** _scale(s6)
        n = x.numerator // x.denominator
        if v.sign:
            n = -n
        n = min(max(n, lo), hi)
    return n & ((1 << (8 * (1 if t in (4, 6) else 2))) - 1)


def _h_psq(st, tx, w, name):
    op = (w >> 26) & 0x3F
    fr, a = _f(w, 6, 10), _f(w, 11, 15)
    if op != 4:
        W, I = _f(w, 16, 16), _f(w, 17, 19)
        d = _f(w, 20, 31)
        d = d - 0x1000 if d & 0x800 else d                     # EXTS(d), 12-bit d PAGE 338
        base = (0 if a == 0 else st.gpr[a]) if name in ("psq_l", "psq_st") else st.gpr[a]
        ea = (base + d) & MASK32
    else:
        b = _f(w, 16, 20)
        W, I = _f(w, 21, 21), _f(w, 22, 24)
        base = (0 if a == 0 else st.gpr[a]) if name in ("psq_lx", "psq_stx") else st.gpr[a]
        ea = (base + st.gpr[b]) & MASK32
    g = _gqr_fields(st.gqr[I])
    load = "_l" in name
    t = g["ld_type"] if load else g["st_type"]
    s6 = g["ld_scale"] if load else g["st_scale"]
    c = _width(t)
    if st.msr & MSR_LE:
        _total("psq in little-endian mode (MSR[LE]=1) 'will give undefined results' PAGE 109-110")
    # alignment: "generates an alignment exception if the operand is not word-aligned when the
    # corresponding GQRn[LD_TYPE] or GQRn[ST_TYPE] are type 0 and does not ... when 4,5,6,7"
    # 750CL PAGE 175 (Gekko PAGE 175 note)
    if t == 0 and (ea & 3):
        _alignment(st, tx, ea)
        return
    upd = name in ("psq_lu", "psq_lux", "psq_stu", "psq_stux")
    if load:
        v0 = _dequantize(tx.read(ea, c), t, s6)
        v1 = _dequantize(tx.read(ea + c, c), t, s6) if W == 0 else ONE_BITS   # PAGE 493
        st.fpr[fr][0], st.fpr[fr][1] = v0, v1
    else:
        x0 = st.fpr[fr][0]
        tx.write(ea, c, _quantize(tx, x0, t, s6, ea))
        if W == 0:
            x1 = st.fpr[fr][1]
            tx.write(ea + c, c, _quantize(tx, x1, t, s6, (ea + c) & MASK32))
    if upd:
        st.gpr[a] = ea                                          # rA <- EA PAGE 494/498/499


def _h_dcbz_l(st, tx, w, name):
    a, b = _f(w, 11, 15), _f(w, 16, 20)
    # EA is the sum (rA|0) + (rB) PAGE 384
    ea = ((0 if a == 0 else st.gpr[a]) + st.gpr[b]) & MASK32
    # "An attempt is made to execute dcbz or dcbz_l when the data cache is disabled" -> alignment
    # PAGE 175
    if not (st.hid0 & HID0_DCE):
        _alignment(st, tx, ea)
        return
    # block of 32 bytes ("allocate a tag for a 32 byte block" PAGE 310): "All bytes are cleared"
    # PAGE 384.  Modelled as one 32-byte zero write at EA & ~31.
    tx.write(ea & ~31, 32, 0)
    # "When the instruction hits either the normal cache or the locked cache, Gekko sets
    # HID2[DCHERR]=1 ... with HID2[DCHEE] = MSR[EE] = MSR[ME] = 1, and HID2[DCHERR] = 0, Gekko
    # also sets SRR1[10]=1 and raises machine check" PAGE 311 -- cache contents are not part of
    # the architectural State, so the outcome is unknowable here.
    # [crosscheck fix] a hit sets DCHERR to 1 and a miss leaves it alone, so the bit is undefined
    # only while it is still 0 (PAGE 384 "setting HID2[DCHERR] to 1").
    # Modelling convention shared with every data access of this area: the State carries no BAT or
    # page table, translation is assumed to succeed with W = I = 0 (exactly the real-mode WIMG
    # 0b0011, PAGE 134), so the write-through / cache-inhibited alignment case of PAGE 311 does not
    # arise.
    if not (st.hid2 & HID2_DCHERR):
        tx.u("hid2:DCHERR", "dcbz_l cache hit depends on cache contents PAGE 311")
    if (st.hid2 & HID2_DCHEE) and (st.msr & MSR_EE) and (st.msr & MSR_ME) and \
            not (st.hid2 & HID2_DCHERR):
        for loc in ("exception", "srr0", "srr1", "msr", "pc"):
            tx.u(loc, "dcbz_l cache-hit machine check possible PAGE 311")


_HANDLERS = {}
for _n in ("ps_add", "ps_sub", "ps_div", "ps_mul", "ps_muls0", "ps_muls1", "ps_madd", "ps_msub",
           "ps_nmadd", "ps_nmsub", "ps_madds0", "ps_madds1"):
    _HANDLERS[_n] = _h_arith
_HANDLERS.update({"ps_sum0": _h_sum, "ps_sum1": _h_sum, "ps_sel": _h_sel, "ps_mr": _h_move,
                  "ps_neg": _h_move, "ps_abs": _h_move, "ps_nabs": _h_move,
                  "ps_merge00": _h_merge, "ps_merge01": _h_merge, "ps_merge10": _h_merge,
                  "ps_merge11": _h_merge, "ps_cmpu0": _h_cmp, "ps_cmpo0": _h_cmp,
                  "ps_cmpu1": _h_cmp, "ps_cmpo1": _h_cmp, "ps_res": _h_estimate,
                  "ps_rsqrte": _h_estimate, "dcbz_l": _h_dcbz_l})
for _n in ("psq_l", "psq_lu", "psq_st", "psq_stu", "psq_lx", "psq_lux", "psq_stx", "psq_stux"):
    _HANDLERS[_n] = _h_psq


def _execute(st, tx, word):
    name = mnemonic(word)
    if name is None:
        raise ValueError("not a ps-area word: 0x%08X" % word)
    st.exception = None
    if name == "illegal":
        # "All unused extended opcodes are illegal" PAGE 88
        _illegal(st, tx)
        return
    # reserved bits set -> "boundedly undefined" PAGE 87
    for lo, hi in _RESERVED.get(name, ()):
        if _f(word, lo, hi):
            _total("%s with reserved bits %d-%d set: boundedly undefined PAGE 87" % (name, lo, hi))
    # invalid forms: "If rA = 0, the instruction form is invalid" PAGE 494/495/498/499
    if name in ("psq_lu", "psq_lux", "psq_stu", "psq_stux") and _f(word, 11, 15) == 0:
        _total("%s with rA = 0 is an invalid form PAGE 494" % name)
    # gating, priority 1 program before priority 3 FP unavailable (Table 4-3 PAGE 162)
    if name == "dcbz_l":
        if not (st.hid2 & HID2_LCE):
            _illegal(st, tx)        # "dcbz_l instruction is illegal when HID2[LCE] = 0" PAGE 90
            return
    else:
        if not (st.hid2 & HID2_PSE):
            _illegal(st, tx)        # "All paired single instructions are illegal" PAGE 67
            return
        if name in ("psq_l", "psq_lu", "psq_st", "psq_stu") and not (st.hid2 & HID2_LSQE):
            _illegal(st, tx)        # "if HID2[PSE] = 0 | HID2[LSQE] = 0 then ..." PAGE 493
            return
        if not (st.msr & MSR_FP):
            _fp_unavailable(st, tx)
            return
    _HANDLERS[name](st, tx, word, name)
    if st.exception is None:
        st.pc = (st.pc + 4) & MASK32


def step(state, word):
    tx = _Tx(state)
    try:
        _execute(state, tx, word & MASK32)
    except _Total as e:
        tx.rollback()
        raise UndefinedBehaviour(str(e)) from None
    except Exception:
        tx.rollback()
        raise
    tx.commit_mem()
    if tx.undef:
        defined = state.clone()
        tx.rollback()
        raise UndefinedBehaviour("; ".join(dict.fromkeys(tx.reasons)), defined, tx.undef)


# ======================================================================================
# Scalar FP instructions and ps1 (reference data; executed by the fp area, not here)
# ======================================================================================
SCALAR_PS1_RULES = {
    # "The ps1 value in the destination register is duplicated from the ps0 result" PAGE 83
    "fadds fsubs fmuls fdivs fmadds fmsubs fnmadds fnmsubs fres":
        "HID2[PSE]=1: frD(ps0) <- result on ps0 operands; frD(ps1) <- frD(ps0) (PAGE 83, 396, 402)",
    # PAGE 418
    "frsp": "HID2[PSE]=1: frD(ps0) <- round-to-single(frB); frD(ps1) UNDEFINED (PAGE 418)",
    # PAGE 405
    "fmr": "HID2[PSE]=1: double operand copied whole; paired-single operand: ps0 copied, "
           "frD(ps1) unchanged (PAGE 405)",
    # PAGE 421
    "fsel": "HID2[PSE]=1 paired-single sources: only frA(ps0) compared, selected ps0 copied, "
            "frD(ps1) UNDEFINED (PAGE 421)",
    # PAGE 83, 110, 434-437
    "lfs lfsu lfsx lfsux": "HID2[PSE]=1: frD(ps0) <- Single(MEM(EA,4)); frD(ps1) <- same (PAGE 434)",
    "stfs stfsu stfsx stfsux": "HID2[PSE]=1: stores the ps0 value only (PAGE 83, 110)",
    # PAGE 83
    "double-precision instructions": "valid and execute as in non-paired-single mode (PAGE 83); "
                                     "any single value in ps0 may be a double-precision input (PAGE 84)",
    "fctiw fctiwz": "integer result in the low-order word; not usable by ps operations (PAGE 84)",
}


# ======================================================================================
# Self test
# ======================================================================================
if __name__ == "__main__":
    def dbl(x):
        return struct.unpack(">Q", struct.pack(">d", x))[0]

    def A(xo, d, a, b, c, rc=0):
        return (4 << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1) | rc

    def X(xo, d, a, b, rc=0):
        return (4 << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | rc

    def CMP(xo, crf, a, b):
        return (4 << 26) | (crf << 23) | (a << 16) | (b << 11) | (xo << 1)

    def PSQD(op, fr, a, W, I, d):
        return (op << 26) | (fr << 21) | (a << 16) | (W << 15) | (I << 12) | (d & 0xFFF)

    def PSQX(sub, fr, a, b, W, I):
        return (4 << 26) | (fr << 21) | (a << 16) | (b << 11) | (W << 10) | (I << 7) | (sub << 1)

    def mk(**kw):
        s = State()
        s.msr = 0x00002000            # FP only
        s.hid2 = HID2_LSQE | HID2_PSE  # 0xA0000000
        s.hid0 = HID0_DCE
        for k, v in kw.items():
            setattr(s, k, v)
        return s

    def setp(s, n, p0, p1):
        s.fpr[n] = [dbl(p0) if isinstance(p0, float) else p0,
                    dbl(p1) if isinstance(p1, float) else p1]

    fails = []
    count = [0]

    def check(name, cond, info=""):
        if not cond:
            fails.append("%s %s" % (name, info))

    def ok(name, s, word):
        count[0] += 1
        try:
            step(s, word)
        except UndefinedBehaviour as e:
            fails.append("%s: unexpected UB %s %s" % (name, e.undefined, e.what))
            return None
        return s

    def ub(name, s, word, must=(), total=False):
        count[0] += 1
        import copy as _copy
        before = _copy.deepcopy(s.arch_dict())
        mem_before = dict(s.mem.bytes)
        log_len = len(s.mem.log)
        try:
            step(s, word)
        except UndefinedBehaviour as e:
            check(name + " state untouched", s.arch_dict() == before and len(s.mem.log) == log_len
                  and s.mem.bytes == mem_before)
            if total:
                check(name + " total", e.defined_state is None, str(e.undefined))
            for m in must:
                if isinstance(m, tuple):      # (location, bits that must be undefined)
                    got = undefined_mask(e, m[0])
                    check(name + " undefined has %s:%X" % m, (got & m[1]) == m[1], str(e.undefined))
                else:
                    check(name + " undefined has " + m, m in e.undefined, str(e.undefined))
            return e
        fails.append("%s: expected UB" % name)
        return None

    PC = 0x80003100
    FXb, VXb, OXb, UXb, ZXb, XXb = FP["FX"], FP["VX"], FP["OX"], FP["UX"], FP["ZX"], FP["XX"]
    FRb, FIb = FP["FR"], FP["FI"]

    # 1 ps_add basic, FPRF +normal (0x04<<12)
    s = mk(); setp(s, 1, 1.5, -2.0); setp(s, 2, 2.25, 0.5)
    ok("add", s, A(21, 3, 1, 2, 0))
    check("add ps0", s.fpr[3] == [dbl(3.75), dbl(-1.5)], s.fpr[3])
    check("add fpscr", s.fpscr == 0x00004000, hex(s.fpscr))
    check("add pc", s.pc == PC + 4 and s.exception is None)

    # 2 ps_add full aliasing D=A=B
    s = mk(); setp(s, 5, 1.0, 0.5)
    ok("add alias", s, A(21, 5, 5, 5, 0))
    check("add alias", s.fpr[5] == [dbl(2.0), dbl(1.0)], s.fpr[5])

    # 3 ps_sub exact zero: +0 under RN=0, -0 under RN=3 (IEEE)
    s = mk(); setp(s, 1, 1.0, 1.0)
    ok("sub zero", s, A(20, 2, 1, 1, 0))
    check("sub +0", s.fpr[2] == [0, 0] and s.fpscr == 0x00002000, (s.fpr[2], hex(s.fpscr)))
    s = mk(fpscr=3); setp(s, 1, 1.0, 1.0)
    ok("sub zero rn3", s, A(20, 2, 1, 1, 0))
    check("sub -0", s.fpr[2] == [SIGN64, SIGN64] and s.fpscr == 0x00012003, hex(s.fpscr))

    # 3b zero + zero: (+0)+(-0) = +0 (RN=0) / -0 (RN=3); (-0)+(-0) = -0 in every mode (IEEE)
    s = mk(); setp(s, 1, 0.0, -0.0); setp(s, 2, -0.0, -0.0)
    ok("add zeros", s, A(21, 3, 1, 2, 0))
    check("add zeros rn0", s.fpr[3] == [0, SIGN64], s.fpr[3])
    s = mk(fpscr=3); setp(s, 1, 0.0, -0.0); setp(s, 2, -0.0, -0.0)
    ok("add zeros rn3", s, A(21, 3, 1, 2, 0))
    check("add zeros rn3", s.fpr[3] == [SIGN64, SIGN64], s.fpr[3])

    # 4 ps_add tie to even: 2^24 + 1 -> 2^24 (FI, XX, FX; FR=0)
    s = mk(); setp(s, 1, 16777216.0, 16777216.0); setp(s, 2, 1.0, 3.0)
    ok("add tie", s, A(21, 3, 1, 2, 0))
    check("tie ps0", s.fpr[3][0] == dbl(16777216.0))
    # 5 ps1: 2^24 + 3 -> 16777220 (tie -> even rounds up, FR=1); FR/FI OR over halves PAGE 83
    check("tie ps1", s.fpr[3][1] == dbl(16777220.0))
    check("tie fpscr", s.fpscr == (FXb | XXb | FRb | FIb | 0x4000), hex(s.fpscr))

    # 6 ps_add Rc=1: CR1 <- FPSCR[0-3]
    s = mk(cr=0xFFFFFFFF); setp(s, 1, 16777216.0, 1.0); setp(s, 2, 1.0, 1.0)
    ok("add rc", s, A(21, 3, 1, 2, 0, rc=1))
    check("add rc cr1", s.cr == 0xF8FFFFFF, hex(s.cr))

    # 7 ps_div 1/3 rounds up to 0x3EAAAAAB (FR=1)
    s = mk(); setp(s, 1, 1.0, 1.0); setp(s, 2, 3.0, -3.0)
    ok("div", s, A(18, 4, 1, 2, 0))
    check("div ps0", s.fpr[4][0] == widen32(0x3EAAAAAB), hex(s.fpr[4][0]))
    check("div ps1", s.fpr[4][1] == widen32(0xBEAAAAAB))
    check("div fpscr", s.fpscr == (FXb | XXb | FRb | FIb | 0x4000), hex(s.fpscr))

    # 8 ps_div x/0 -> +inf, ZX; FPRF +inf 0x05
    s = mk(); setp(s, 1, 1.0, 2.0); setp(s, 2, 0.0, 4.0)
    ok("div0", s, A(18, 3, 1, 2, 0))
    check("div0 val", s.fpr[3] == [0x7FF0000000000000, dbl(0.5)])
    check("div0 fpscr", s.fpscr == (FXb | ZXb | 0x5000), hex(s.fpscr))

    # 9 ps_div 0/0 -> VXZDZ, generated QNaN (sign + payload undefined), FPRF QNaN 0x11
    s = mk(); setp(s, 1, 0.0, 1.0); setp(s, 2, 0.0, 1.0)
    e = ub("div 0/0", s, A(18, 3, 1, 2, 0), must=(("fpr[3][0]", SIGN64 | PAYLOAD64),))
    if e:
        ds = e.defined_state
        check("0/0 exact mask", undefined_mask(e, "fpr[3][0]") == SIGN64 | PAYLOAD64 and
              (ds.fpr[3][0] & ~(SIGN64 | PAYLOAD64)) == 0x7FF8000000000000)
        check("0/0 fpscr", ds.fpscr == (FXb | VXb | FP["VXZDZ"] | 0x11000), hex(ds.fpscr))
        check("0/0 ps1", ds.fpr[3][1] == dbl(1.0))

    # 10 ps_mul overflow RN=0 -> +inf; OX XX FI; FR undefined
    s = mk(); setp(s, 1, 2.0 ** 127, 1.0); setp(s, 2, 4.0, 1.0)
    e = ub("mul ovf", s, A(25, 3, 1, 0, 2), must=(("fpscr", FRb),))
    if e:
        ds = e.defined_state
        check("ovf val", ds.fpr[3][0] == 0x7FF0000000000000)
        check("ovf fpscr", (ds.fpscr & ~FRb) == (FXb | OXb | XXb | FIb | 0x5000), hex(ds.fpscr))
    # 11 overflow RN=1 -> max single 0x47EFFFFFE0000000
    s = mk(fpscr=1); setp(s, 1, -(2.0 ** 127), 1.0); setp(s, 2, 4.0, 1.0)
    e = ub("mul ovf rz", s, A(25, 3, 1, 0, 2), must=(("fpscr", FRb),))
    if e:
        check("ovf rz val", e.defined_state.fpr[3][0] == 0xC7EFFFFFE0000000,
              hex(e.defined_state.fpr[3][0]))

    # 12 ps_mul 0*inf -> VXIMZ ; ps_add inf-inf -> VXISI
    s = mk(); setp(s, 1, 0.0, 1.0); setp(s, 2, float("inf"), 1.0)
    e = ub("mul imz", s, A(25, 3, 1, 0, 2), must=(("fpr[3][0]", SIGN64 | PAYLOAD64),))
    if e:
        check("imz", e.defined_state.fpscr & FP["VXIMZ"] and e.defined_state.fpscr & VXb)
    s = mk(); setp(s, 1, float("inf"), 1.0); setp(s, 2, float("-inf"), 1.0)
    e = ub("add isi", s, A(21, 3, 1, 2, 0), must=(("fpr[3][0]", SIGN64 | PAYLOAD64),))
    if e:
        check("isi", e.defined_state.fpscr == (FXb | VXb | FP["VXISI"] | 0x11000),
              hex(e.defined_state.fpscr))

    # 13 ps_madd frD <- A*C + B
    s = mk(); setp(s, 1, 3.0, 2.0); setp(s, 2, 5.0, -1.0); setp(s, 3, 4.0, 0.5)
    ok("madd", s, A(29, 4, 1, 2, 3))
    check("madd", s.fpr[4] == [dbl(17.0), 0], s.fpr[4])

    # 14 fused: (1+2^-23)(1-2^-23) + (-1) = -2^-46 exactly (no intermediate rounding PAGE 97)
    s = mk(); setp(s, 1, 1.0 + 2.0 ** -23, 1.0); setp(s, 3, 1.0 - 2.0 ** -23, 1.0)
    setp(s, 2, -1.0, -1.0)
    ok("madd fused", s, A(29, 4, 1, 2, 3))
    check("fused", s.fpr[4][0] == dbl(-(2.0 ** -46)) and s.fpr[4][1] == 0, s.fpr[4])
    check("fused exact", s.fpscr == 0x8000, hex(s.fpscr))

    # 15 ps_nmadd / ps_msub / ps_nmsub
    s = mk(); setp(s, 1, 3.0, 3.0); setp(s, 2, 5.0, 5.0); setp(s, 3, 4.0, 4.0)
    ok("nmadd", s, A(31, 4, 1, 2, 3))
    check("nmadd", s.fpr[4] == [dbl(-17.0), dbl(-17.0)])
    check("nmadd fprf", s.fpscr == 0x8000, hex(s.fpscr))
    ok("msub", s, A(28, 5, 1, 2, 3))
    check("msub", s.fpr[5] == [dbl(7.0), dbl(7.0)])
    ok("nmsub", s, A(30, 6, 1, 2, 3))
    check("nmsub", s.fpr[6] == [dbl(-7.0), dbl(-7.0)])

    # 16 ps_nmadd rounds before negation ("same result as ps_madd and then negating" PAGE 522)
    s = mk(fpscr=2); setp(s, 1, 1.0, 1.0); setp(s, 3, 1.0, 1.0); setp(s, 2, 2.0 ** -30, 2.0 ** -30)
    ok("nmadd rn2", s, A(31, 4, 1, 2, 3))
    check("nmadd rn2", s.fpr[4][0] == dbl(-(1.0 + 2.0 ** -23)), hex(s.fpr[4][0]))
    check("nmadd rn2 fpscr", s.fpscr == (FXb | XXb | FRb | FIb | 0x8000 | 2), hex(s.fpscr))

    # 17 ps_madds0 (frC(ps0) for both) / ps_madds1 (frC(ps1) for both)
    s = mk(); setp(s, 1, 2.0, 3.0); setp(s, 3, 10.0, 100.0); setp(s, 2, 1.0, 1.0)
    ok("madds0", s, A(14, 4, 1, 2, 3))
    check("madds0", s.fpr[4] == [dbl(21.0), dbl(31.0)], s.fpr[4])
    ok("madds1", s, A(15, 5, 1, 2, 3))
    check("madds1", s.fpr[5] == [dbl(201.0), dbl(301.0)], s.fpr[5])
    # 18 ps_muls0 / ps_muls1
    ok("muls0", s, A(12, 6, 1, 0, 3))
    check("muls0", s.fpr[6] == [dbl(20.0), dbl(30.0)])
    ok("muls1", s, A(13, 7, 1, 0, 3))
    check("muls1", s.fpr[7] == [dbl(200.0), dbl(300.0)])

    # 19 ps_sum0: ps0 = A.ps0 + B.ps1, ps1 = C.ps1 ; aliasing D == C
    s = mk(); setp(s, 1, 1.0, 9.0); setp(s, 2, 8.0, 2.0); setp(s, 3, 6.0, 7.0)
    ok("sum0", s, A(10, 3, 1, 2, 3))
    check("sum0", s.fpr[3] == [dbl(3.0), dbl(7.0)], s.fpr[3])
    # 20 ps_sum1: ps0 = C.ps0, ps1 = A.ps0 + B.ps1 ; FPRF from ps1 (-normal 0x08)
    s = mk(); setp(s, 1, 1.0, 9.0); setp(s, 2, 8.0, -4.0); setp(s, 3, 6.0, 7.0)
    ok("sum1", s, A(11, 4, 1, 2, 3))
    check("sum1", s.fpr[4] == [dbl(6.0), dbl(-3.0)], s.fpr[4])
    check("sum1 fprf", s.fpscr == 0x8000, hex(s.fpscr))

    # 21 moves: neg on NaN flips sign, FPSCR untouched; abs(-0) ; nabs(1)
    s = mk(fpscr=0x12345000 & ~0x3F); setp(s, 1, 0x7FF8000000000000, -0.0)
    f0 = s.fpscr
    ok("neg", s, X(40, 2, 0, 1))
    check("neg", s.fpr[2] == [0xFFF8000000000000, 0] and s.fpscr == f0)
    ok("abs", s, X(264, 3, 0, 1))
    check("abs", s.fpr[3] == [0x7FF8000000000000, 0])
    setp(s, 4, 1.0, -2.0)
    ok("nabs", s, X(136, 5, 0, 4))
    check("nabs", s.fpr[5] == [dbl(-1.0), dbl(-2.0)])
    # 22 ps_mr Rc=1 -> CR1 from FPSCR[0-3]
    s = mk(fpscr=0x90000000, cr=0); setp(s, 1, 1.0, 2.0)
    ok("mr rc", s, X(72, 6, 0, 1, rc=1))
    check("mr rc", s.fpr[6] == [dbl(1.0), dbl(2.0)] and s.cr == 0x09000000, hex(s.cr))

    # 23 merges with aliasing D == A
    for xo, exp in ((528, (1.0, 3.0)), (560, (1.0, 4.0)), (592, (2.0, 3.0)), (624, (2.0, 4.0))):
        s = mk(); setp(s, 1, 1.0, 2.0); setp(s, 2, 3.0, 4.0)
        ok("merge%d" % xo, s, X(xo, 1, 1, 2))
        check("merge%d" % xo, s.fpr[1] == [dbl(exp[0]), dbl(exp[1])], s.fpr[1])

    # 24 ps_sel: -0 >= 0 selects C; NaN selects B
    s = mk(); setp(s, 1, -0.0, 0x7FF8000000000000); setp(s, 3, 1.0, 2.0); setp(s, 2, 3.0, 4.0)
    ok("sel", s, A(23, 4, 1, 2, 3))
    check("sel", s.fpr[4] == [dbl(1.0), dbl(4.0)] and s.fpscr == 0)

    # 25 ps_cmpu0 crf3: 1 < 2 -> CR field3 = 8, FPCC = 8
    s = mk(cr=0x12345678); setp(s, 1, 1.0, 5.0); setp(s, 2, 2.0, 5.0)
    ok("cmpu0", s, CMP(0, 3, 1, 2))
    check("cmpu0", s.cr == 0x12385678 and s.fpscr == 0x8000, (hex(s.cr), hex(s.fpscr)))
    # 26 ps_cmpu1 on ps1: equal -> 2 ; +0 == -0
    s = mk(); setp(s, 1, 1.0, 0.0); setp(s, 2, 2.0, -0.0)
    ok("cmpu1", s, CMP(64, 0, 1, 2))
    check("cmpu1", s.cr == 0x20000000 and s.fpscr == 0x2000, (hex(s.cr), hex(s.fpscr)))
    # 27 ps_cmpo1 QNaN -> unordered, VXVC
    s = mk(); setp(s, 1, 1.0, 0x7FF8000000000000); setp(s, 2, 1.0, 1.0)
    ok("cmpo1 qnan", s, CMP(96, 7, 1, 2))
    check("cmpo1", s.cr == 0x1 and s.fpscr == (FXb | VXb | FP["VXVC"] | 0x1000), hex(s.fpscr))
    # 28 ps_cmpo0 SNaN with VE=1 -> VXSNAN only, FEX=1
    snan = widen32(0x7FA00000)
    s = mk(fpscr=FP["VE"]); setp(s, 1, snan, 1.0); setp(s, 2, 1.0, 1.0)
    ok("cmpo0 snan ve", s, CMP(32, 1, 1, 2))
    check("cmpo0 ve", s.fpscr == (FXb | FP["FEX"] | VXb | FP["VXSNAN"] | 0x1000 | FP["VE"]),
          hex(s.fpscr))
    check("cmpo0 cr", s.cr == 0x01000000, hex(s.cr))
    # 29 ps_cmpo0 SNaN VE=0 -> VXSNAN and VXVC ; ps_cmpu0 SNaN -> VXSNAN only
    s = mk(); setp(s, 1, snan, 1.0); setp(s, 2, 1.0, 1.0)
    ok("cmpo0 snan", s, CMP(32, 0, 1, 2))
    check("cmpo0 snan", s.fpscr == (FXb | VXb | FP["VXSNAN"] | FP["VXVC"] | 0x1000), hex(s.fpscr))
    s = mk(fpscr=FP["VXSNAN"] | VXb); setp(s, 1, snan, 1.0); setp(s, 2, 1.0, 1.0)
    ok("cmpu0 snan sticky", s, CMP(0, 0, 1, 2))
    check("cmpu0 no FX when already set", s.fpscr == (VXb | FP["VXSNAN"] | 0x1000), hex(s.fpscr))

    # 30 ps_res special operands: +0 -> +inf ZX ; -inf -> -0 (FR/FI undefined)
    s = mk(); setp(s, 1, 0.0, float("-inf"))
    e = ub("res", s, A(24, 2, 0, 1, 0), must=(("fpscr", FRb | FIb),))
    if e:
        ds = e.defined_state
        check("res val", ds.fpr[2] == [0x7FF0000000000000, SIGN64], ds.fpr[2])
        check("res fpscr", (ds.fpscr & ~(FRb | FIb)) == (FXb | ZXb | 0x5000), hex(ds.fpscr))
    # 31 ps_res with ZE=1 on +0: "No result" in that half -> half and FPRF undefined (PAGE 525/507);
    #    the +inf half is a defined +0; ZX and FEX are defined
    s = mk(fpscr=FP["ZE"] | 0x8000); setp(s, 1, 0.0, float("inf")); setp(s, 2, 7.0, 7.0)
    e = ub("res ze", s, A(24, 2, 0, 1, 0), must=(("fpr[2][0]", MASK64), ("fpscr", FPRF_MASK)))
    if e:
        ds = e.defined_state
        check("res ze ps1 defined", ds.fpr[2][1] == 0 and undefined_mask(e, "fpr[2][1]") == 0)
        check("res ze fex", ds.fpscr & FP["FEX"] and ds.fpscr & ZXb and ds.fpscr & FXb)
    # 32 ps_rsqrte -1 -> QNaN VXSQRT ; +4 -> estimate (value undefined, FPRF +normal)
    s = mk(); setp(s, 1, -1.0, 4.0)
    e = ub("rsqrte", s, A(26, 2, 0, 1, 0), must=(("fpr[2][0]", SIGN64 | PAYLOAD64), ("fpr[2][1]", MASK64)))
    if e:
        check("rsqrte fpscr", (e.defined_state.fpscr & ~(FRb | FIb)) ==
              (FXb | VXb | FP["VXSQRT"] | 0x11000), hex(e.defined_state.fpscr))

    # 33 HID2[PSE]=0 -> illegal instruction program exception
    s = mk(hid2=HID2_LSQE, msr=0x00002000 | MSR_EE | MSR_ME | MSR_IR | MSR_DR | MSR_RI)
    e = ub("pse0", s, A(21, 3, 1, 2, 0), must=("srr1:0x783F0000",))
    if e:
        ds = e.defined_state
        check("pse0 exc", ds.exception == "program" and ds.pc == 0x700 and ds.srr0 == PC)
        check("pse0 msr", ds.msr == MSR_ME, hex(ds.msr))
        check("pse0 srr1", ds.srr1 == (0x00002000 | MSR_EE | MSR_ME | MSR_IR | MSR_DR | MSR_RI),
              hex(ds.srr1))
    # 34 MSR[FP]=0 -> FP unavailable at 0xFFF00800 with IP=1, LE <- ILE
    s = mk(msr=MSR_IP | MSR_ILE); setp(s, 1, 1.0, 1.0)
    e = ub("fpu", s, X(72, 2, 0, 1))
    if e:
        ds = e.defined_state
        check("fpu", ds.exception == "fp_unavailable" and ds.pc == 0xFFF00800 and
              ds.msr == (MSR_IP | MSR_ILE | MSR_LE), (hex(ds.pc), hex(ds.msr)))
    # 35 priority: PSE=0 and FP=0 -> program
    s = mk(hid2=0, msr=0)
    e = ub("prio", s, X(72, 2, 0, 1))
    if e:
        check("prio", e.defined_state.exception == "program")
    # 36 psq_l with LSQE=0 is illegal; psq_lx with LSQE=0 is legal
    s = mk(hid2=HID2_PSE)
    e = ub("psq_l lsqe", s, PSQD(56, 1, 0, 0, 0, 0))
    if e:
        check("psq_l lsqe", e.defined_state.exception == "program")
    s = mk(hid2=HID2_PSE); s.gpr[4] = 0x1000; s.mem.load(0x1000, bytes.fromhex("3FC00000C0000000"))
    ok("psq_lx lsqe0", s, PSQX(6, 1, 0, 4, 0, 0))
    check("psq_lx", s.fpr[1] == [dbl(1.5), dbl(-2.0)] and s.pc == PC + 4)
    # 37 dcbz_l with LCE=0 illegal even with PSE=1
    s = mk()
    e = ub("dcbz_l lce0", s, X(1014, 0, 3, 4))
    if e:
        check("dcbz_l lce0", e.defined_state.exception == "program")

    # 38 psq_l type 0 W=0: both words, read order ps0 then ps1 ; SNaN passes through bit exact
    s = mk(); s.gpr[3] = 0x80001000
    s.mem.load(0x80001008, bytes.fromhex("7FA0000100000001"))
    ok("psq_l t0", s, PSQD(56, 2, 3, 0, 0, 8))
    check("psq_l t0 vals", s.fpr[2] == [widen32(0x7FA00001), widen32(0x00000001)], s.fpr[2])
    check("psq_l t0 log", s.mem.log == [("r", 0x80001008, 4, 0x7FA00001),
                                        ("r", 0x8000100C, 4, 1)], s.mem.log)
    check("psq_l t0 denorm", s.fpr[2][1] == bits_from(0, TWO ** -149))
    # 39 psq_l u8 scale 2 W=1: 0xFF -> 63.75, ps1 = 1.0 ; negative d
    s = mk(); s.gpr[3] = 0x2010; s.gqr[5] = (2 << 24) | (4 << 16)
    s.mem.load(0x2000, bytes([0xFF, 0x77]))
    ok("psq_l u8", s, PSQD(56, 1, 3, 1, 5, -16))
    check("psq_l u8", s.fpr[1] == [dbl(63.75), ONE_BITS] and s.mem.log == [("r", 0x2000, 1, 0xFF)])
    # 40 psq_l s16 scale -1 (111111): 0x8000 -> -65536 ; ps1 0x0001 -> 2.0
    s = mk(); s.gqr[1] = (0x3F << 24) | (7 << 16); s.mem.load(0x10, bytes.fromhex("80000001"))
    ok("psq_l s16", s, PSQD(56, 1, 0, 0, 1, 0x10))
    check("psq_l s16", s.fpr[1] == [dbl(-65536.0), dbl(2.0)], s.fpr[1])
    # 41 psq_l s8 scale -32 (100000): 1 -> 4294967296.0 ; u16 scale 31: 1 -> 2^-31
    s = mk(); s.gqr[2] = (0x20 << 24) | (6 << 16); s.mem.load(0x10, bytes([0x01, 0xFF]))
    ok("psq_l s8", s, PSQD(56, 1, 0, 0, 2, 0x10))
    check("psq_l s8", s.fpr[1] == [dbl(4294967296.0), dbl(-(2.0 ** 32))], s.fpr[1])
    s = mk(); s.gqr[2] = (31 << 24) | (5 << 16); s.mem.load(0x10, bytes([0x00, 0x01, 0, 0]))
    ok("psq_l u16", s, PSQD(56, 1, 0, 0, 2, 0x10))
    check("psq_l u16", s.fpr[1] == [dbl(2.0 ** -31), 0], s.fpr[1])
    # 42 psq_lu updates rA ; rA = 0 invalid form
    s = mk(); s.gpr[7] = 0x3000; s.mem.load(0x3004, bytes.fromhex("40000000"))
    ok("psq_lu", s, PSQD(57, 1, 7, 1, 0, 4))
    check("psq_lu", s.gpr[7] == 0x3004 and s.fpr[1] == [dbl(2.0), ONE_BITS])
    ub("psq_lu ra0", mk(), PSQD(57, 1, 0, 1, 0, 4), total=True)
    # 43 psq_lux: EA = rA + rB, update
    s = mk(); s.gpr[3] = 0x100; s.gpr[4] = 0x20; s.gqr[3] = 5 << 16
    s.mem.load(0x120, bytes.fromhex("12345678"))
    ok("psq_lux", s, PSQX(38, 9, 3, 4, 0, 3))
    check("psq_lux", s.gpr[3] == 0x120 and s.fpr[9] == [dbl(4660.0), dbl(22136.0)])

    # 44 psq_st s8 scale 1: 100 -> 200 clamps to 127 ; -300*2 -> -128 (Table 2-45 PAGE 112)
    s = mk(); s.gqr[1] = (1 << 8) | 6; setp(s, 2, 100.0, -300.0)
    ok("psq_st s8", s, PSQD(60, 2, 0, 0, 1, 0x40))
    check("psq_st s8", s.mem.log == [("w", 0x40, 1, 0x7F), ("w", 0x41, 1, 0x80)], s.mem.log)
    # 45 Table 2-45 rows: u16 S=31 -> 65535 ; u8 S=-2 -> 25 ; s16 S=2 -> 400
    s = mk(); s.gqr[0] = (31 << 8) | 5; setp(s, 2, 100.0, 100.0)
    ok("psq_st u16", s, PSQD(60, 2, 0, 1, 0, 0x40))
    check("psq_st u16", s.mem.log == [("w", 0x40, 2, 65535)], s.mem.log)
    s = mk(); s.gqr[0] = (0x3E << 8) | 4; setp(s, 2, 100.0, 100.0)
    ok("psq_st u8", s, PSQD(60, 2, 0, 1, 0, 0x40))
    check("psq_st u8", s.mem.log == [("w", 0x40, 1, 25)], s.mem.log)
    s = mk(); s.gqr[0] = (2 << 8) | 7; setp(s, 2, 100.0, -100.75)
    ok("psq_st s16", s, PSQD(60, 2, 0, 0, 0, 0x40))
    check("psq_st s16", s.mem.log == [("w", 0x40, 2, 400), ("w", 0x42, 2, (-403) & 0xFFFF)],
          s.mem.log)
    # 46 +NaN -> positive overflow ; -Inf -> negative overflow ; -1.5 u8 -> 0 ; -1.5 s8 -> -1
    s = mk(); s.gqr[0] = 4; setp(s, 2, 0x7FF8000000000000, -1.5)
    ok("psq_st nan u8", s, PSQD(60, 2, 0, 0, 0, 0x40))
    check("psq_st nan u8", s.mem.log == [("w", 0x40, 1, 255), ("w", 0x41, 1, 0)], s.mem.log)
    #    -NaN: Gekko PAGE 111 (positive overflow) vs 750CL PAGE 108 (negative overflow) -> undefined
    s = mk(); s.gqr[0] = 7; setp(s, 2, 0xFFF8000000000000, -1.5)
    e = ub("psq_st -nan s16", s, PSQD(60, 2, 0, 0, 0, 0x40),
           must=(("mem:0x00000040", 0xFF), ("mem:0x00000041", 0xFF)))
    if e:
        check("psq_st -nan s16 ps1", e.defined_state.mem.log[1] == ("w", 0x42, 2, 0xFFFF)
              and undefined_mask(e, "mem:0x00000042") == 0)
    s = mk(); s.gqr[0] = 6; setp(s, 2, float("-inf"), -1.5)
    ok("psq_st s8", s, PSQD(60, 2, 0, 0, 0, 0x40))
    check("psq_st -inf s8", s.mem.log == [("w", 0x40, 1, 0x80), ("w", 0x41, 1, 0xFF)], s.mem.log)
    # 47 type 0: normal passes through; positive denorm -> 0.0 ; negative denorm sign undefined
    s = mk(); setp(s, 2, 1.5, widen32(0x00000005))
    ok("psq_st t0", s, PSQD(60, 2, 0, 0, 0, 0x40))
    check("psq_st t0", s.mem.log == [("w", 0x40, 4, 0x3FC00000), ("w", 0x44, 4, 0)], s.mem.log)
    s = mk(); setp(s, 2, 1.5, widen32(0x80000005))
    ub("psq_st t0 -denorm", s, PSQD(60, 2, 0, 0, 0, 0x40), must=("mem:0x00000044+sign",))
    # 48 type 0 misaligned -> alignment ; u8 misaligned -> fine
    s = mk(); setp(s, 2, 1.5, 1.5)
    e = ub("psq_st align", s, PSQD(60, 2, 0, 0, 0, 0x41), must=("dsisr",))
    if e:
        check("align", e.defined_state.exception == "alignment" and e.defined_state.pc == 0x600
              and e.defined_state.mem.log == [])
        check("align dar = EA (PAGE 59)", e.defined_state.dar == 0x41 and undefined_mask(e, "dar") == 0)
    s = mk(); s.gqr[0] = 4; setp(s, 2, 1.5, 2.5)
    ok("psq_st u8 misaligned", s, PSQD(60, 2, 0, 0, 0, 0x41))
    check("u8 misaligned", s.mem.log == [("w", 0x41, 1, 1), ("w", 0x42, 1, 2)], s.mem.log)
    # 49 psq_stu / psq_stux / psq_stx
    s = mk(); s.gpr[5] = 0x1000; setp(s, 3, 2.0, 3.0)
    ok("psq_stu", s, PSQD(61, 3, 5, 1, 0, -8))
    check("psq_stu", s.gpr[5] == 0xFF8 and s.mem.log == [("w", 0xFF8, 4, 0x40000000)])
    s = mk(); s.gpr[5] = 0x1000; s.gpr[6] = 0x10; setp(s, 3, 2.0, 3.0)
    ok("psq_stux", s, PSQX(39, 3, 5, 6, 0, 0))
    check("psq_stux", s.gpr[5] == 0x1010 and s.mem.log ==
          [("w", 0x1010, 4, 0x40000000), ("w", 0x1014, 4, 0x40400000)])
    s = mk(); s.gpr[6] = 0x10; setp(s, 3, 2.0, 3.0)
    ok("psq_stx ra0", s, PSQX(7, 3, 0, 6, 1, 0))
    check("psq_stx", s.gpr[0] == 0 and s.mem.log == [("w", 0x10, 4, 0x40000000)])

    # 50 dcbz_l with LCE=1 zeroes the 32-byte block; DCHERR outcome undefined
    s = mk(hid2=HID2_PSE | HID2_LCE); s.gpr[3] = 0x80001000; s.gpr[4] = 0x25
    s.mem.load(0x80001020, bytes(range(1, 33)))
    e = ub("dcbz_l", s, X(1014, 0, 3, 4), must=("hid2:DCHERR",))
    if e:
        ds = e.defined_state
        check("dcbz_l zero", ds.mem.log == [("w", 0x80001020, 32, 0)] and
              all(ds.mem.bytes[0x80001020 + i] == 0 for i in range(32)) and ds.pc == PC + 4)
        check("dcbz_l rollback", s.mem.bytes[0x80001020] == 1)
    s = mk(hid2=HID2_LCE, hid0=0)
    e = ub("dcbz_l dce0", s, X(1014, 0, 3, 4))
    if e:
        check("dcbz_l dce0", e.defined_state.exception == "alignment")

    # 51 reserved bits -> boundedly undefined ; unused XO -> illegal
    s = mk(); setp(s, 1, 1.0, 1.0)
    ub("reserved", s, A(21, 3, 1, 1, 5), total=True)
    ub("cmp rc", mk(), CMP(0, 0, 1, 2) | 1, total=True)
    e = ub("unused xo", mk(), X(1, 0, 0, 0))
    if e:
        check("unused xo", e.defined_state.exception == "program")
    # 52 double-precision operand -> programming error
    s = mk(); setp(s, 1, 0.1, 1.0)
    ub("double operand", s, A(21, 2, 1, 1, 0), total=True)

    # 53 tiny: 2^-149 * 0.5 = 2^-150 -> tie -> +0, UX XX FI, FPRF +zero
    s = mk(); setp(s, 1, bits_from(0, TWO ** -149), 1.0); setp(s, 2, 0.5, 1.0)
    ok("underflow", s, A(25, 3, 1, 0, 2))
    check("underflow", s.fpr[3][0] == 0 and s.fpscr == (FXb | UXb | XXb | FIb | 0x2000),
          hex(s.fpscr))
    # 54 exact denormal result: no UX, FPRF +denorm 0x14
    s = mk(); setp(s, 1, bits_from(0, TWO ** -140), 1.0); setp(s, 2, 0.5, 1.0)
    ok("denorm exact", s, A(25, 3, 1, 0, 2))
    check("denorm exact", s.fpr[3][0] == bits_from(0, TWO ** -141) and s.fpscr == 0x14000,
          hex(s.fpscr))
    # 55 tininess ambiguous: (2^23-1)2^-149 * (1+2^-23) = 2^-126 - 2^-172 -> 2^-126, UX undefined
    s = mk(); setp(s, 1, bits_from(0, Fraction((1 << 23) - 1) * DENORM_Q), 1.0)
    setp(s, 2, 1.0 + 2.0 ** -23, 1.0)
    e = ub("tininess", s, A(25, 3, 1, 0, 2), must=(("fpscr", UXb),))
    if e:
        ds = e.defined_state
        check("tininess val", ds.fpr[3][0] == 0x3810000000000000, hex(ds.fpr[3][0]))
        check("tininess flags", (ds.fpscr & (XXb | FIb | FRb | FPRF_MASK)) ==
              (XXb | FIb | FRb | 0x4000), hex(ds.fpscr))
    # 56 NI=1 with denormal operand: "Zero A" (Table 2-21 PAGE 84) -> 2^-140 + 1.0 = 1.0 exactly;
    #    whether the replacement is signalled (UX, XX, FI, FR) is not given
    s = mk(fpscr=FP["NI"]); setp(s, 1, bits_from(0, TWO ** -140), 1.0); setp(s, 2, 1.0, 1.0)
    e = ub("ni denorm", s, A(21, 3, 1, 2, 0), must=(("fpscr", UXb | XXb | FIb | FRb),))
    if e:
        check("ni denorm val", e.defined_state.fpr[3] == [dbl(1.0), dbl(2.0)] and
              undefined_mask(e, "fpr[3][0]") == 0 and (e.defined_state.fpscr & FPRF_MASK) == 0x4000)
    #    NI=1: 2^-100 * 2^-41 = 2^-141 (a denormal result) -> "Return zero" (Table 2-22 PAGE 85),
    #    sign undefined
    s = mk(fpscr=FP["NI"]); setp(s, 1, 2.0 ** -100, 1.0); setp(s, 2, 2.0 ** -41, 1.0)
    e = ub("ni tiny mul", s, A(25, 3, 1, 0, 2), must=(("fpr[3][0]", SIGN64),))
    if e:
        check("ni tiny mul zero", (e.defined_state.fpr[3][0] & ~SIGN64) == 0 and
              undefined_mask(e, "fpr[3][0]") == SIGN64)
    # 57 VE=1 invalid: "No result" -> target half and FPRF undefined (PAGE 502), FEX set, the other
    #    half defined ; FE0 -> program exception and the committed bits undefined
    s = mk(fpscr=FP["VE"] | 0x4000); setp(s, 1, float("inf"), 1.0); setp(s, 2, float("-inf"), 1.0)
    e = ub("ve isi", s, A(21, 3, 1, 2, 0), must=(("fpr[3][0]", MASK64), ("fpscr", FPRF_MASK)))
    if e:
        ds = e.defined_state
        check("ve other half", ds.fpscr & FP["FEX"] and ds.fpr[3][1] == dbl(2.0)
              and undefined_mask(e, "fpr[3][1]") == 0)
    s = mk(fpscr=FP["VE"], msr=MSR_FP | MSR_FE0); setp(s, 1, float("inf"), 1.0)
    setp(s, 2, float("-inf"), 1.0)
    ub("fe0", s, A(21, 3, 1, 2, 0), must=("exception", "srr0", "pc", ("fpr[3][1]", dbl(2.0)),
                                          ("fpscr", FXb | FP["FEX"] | VXb | FP["VXISI"])))
    # 58 SNaN propagation: quiet bit set, sign kept, VXSNAN ; payload retention undefined
    s = mk(); setp(s, 1, widen32(0xFFA00001), 1.0); setp(s, 2, 1.0, 1.0)
    e = ub("snan add", s, A(21, 3, 1, 2, 0), must=(("fpr[3][0]", PAYLOAD64),))
    if e:
        ds = e.defined_state
        check("snan add", ds.fpr[3][0] | PAYLOAD64 == widen32(0xFFE00001) | PAYLOAD64 and
              undefined_mask(e, "fpr[3][0]") == PAYLOAD64 and
              ds.fpscr == (FXb | VXb | FP["VXSNAN"] | 0x11000), (hex(ds.fpr[3][0]), hex(ds.fpscr)))
    # 59 two QNaN operands: which propagates is PEM priority -> the differing payload bits undefined
    s = mk(); setp(s, 1, widen32(0x7FC00001), 1.0); setp(s, 2, widen32(0x7FC00002), 1.0)
    e = ub("two nans", s, A(21, 3, 1, 2, 0), must=(("fpr[3][0]", 3 << 29),))
    if e:
        check("two nans mask", undefined_mask(e, "fpr[3][0]") == 3 << 29)
    #    the same QNaN twice propagates exactly
    s = mk(); setp(s, 1, widen32(0x7FC00001), 1.0); setp(s, 2, widen32(0x7FC00001), 1.0)
    ok("same qnan", s, A(21, 3, 1, 2, 0))
    check("same qnan", s.fpr[3][0] == widen32(0x7FC00001))
    # 60 little-endian psq -> undefined
    s = mk(msr=MSR_FP | MSR_LE)
    ub("psq le", s, PSQD(56, 1, 0, 0, 0, 0), total=True)
    # 61 reserved GQR type
    s = mk(); s.gqr[0] = 2 << 16
    ub("gqr type 2", s, PSQD(56, 1, 0, 0, 0, 0), total=True)
    # 62 ps_add with operand read back after a W=1 load (ps1=1.0) and ps_neg of single SNaN keeps
    #    signalling bit (move is bit exact)
    s = mk(); setp(s, 1, snan, snan)
    ok("neg snan", s, X(40, 1, 0, 1))
    check("neg snan", s.fpr[1] == [snan | SIGN64, snan | SIGN64] and s.fpscr == 0)

    print("vectors run:", count[0])
    if fails:
        print("FAILED %d" % len(fails))
        for f_ in fails:
            print("  " + f_)
        sys.exit(1)
    print("ALL PASS")
