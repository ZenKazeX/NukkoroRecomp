"""Independent Gekko specification model, area "ps" (paired singles), author B.

SOURCES (the only ones used):
  G = docs/reference/ppc/gekko_user_manual.txt (authoritative), cited "PAGE N" (its ===== PAGE N ===== marker)
  C = docs/reference/ppc/ppc_750cl.txt, cited "750CL PAGE N" (used only to disambiguate typos in G)
  General IEEE-754 arithmetic knowledge.
No GekkoForge implementation, test, audit or fixes document was consulted.

COVERED
  primary 4, A-form (bits 26-30):  10 ps_sum0, 11 ps_sum1, 12 ps_muls0, 13 ps_muls1, 14 ps_madds0,
      15 ps_madds1, 18 ps_div, 20 ps_sub, 21 ps_add, 23 ps_sel, 24 ps_res, 25 ps_mul, 26 ps_rsqrte,
      28 ps_msub, 29 ps_madd, 30 ps_nmsub, 31 ps_nmadd
  primary 4, X-form (bits 21-30):  0 ps_cmpu0, 32 ps_cmpo0, 64 ps_cmpu1, 96 ps_cmpo1, 40 ps_neg,
      72 ps_mr, 136 ps_nabs, 264 ps_abs, 528 ps_merge00, 560 ps_merge01, 592 ps_merge10,
      624 ps_merge11, 1014 dcbz_l
  primary 4, 6-bit XO (bits 25-30): 6 psq_lx, 7 psq_stx, 38 psq_lux, 39 psq_stux
  primary 56 psq_l, 57 psq_lu, 60 psq_st, 61 psq_stu
  every other primary-4 word: illegal instruction (PAGE 88 "All unused extended opcodes are illegal",
      primary 4 listed as having unused extended opcodes).
  Scalar FP instructions are NOT executed here (fp area); their ps1 rules are recorded as data in
  SCALAR_PS1_RULES.

STATE REPRESENTATION DECISION
  gekko_state stores fpr[n] = [ps0, ps1] as u64 double images. PAGE 82: paired single operands are
  "two 32 bit single-precision floating-point values"; PAGE 83: "The relationship between the internal
  format for paired single operands and that for double-precision floating-point operands is
  unspecified. It is a programming error to apply double-precision instructions to paired single
  operands and vice versa." A ps value is therefore modelled as the double image of a
  single-precision value (NaN: sign, 23 fraction bits << 29). Any paired-single instruction that
  CONSUMES a ps slot whose image is not exactly a single-precision value raises UndefinedBehaviour.

UNDEFINED-BEHAVIOUR CONTRACT
  step() raises UndefinedBehaviour when the corpus leaves (part of) the outcome undefined.
    exc.applied == False : whole instruction undefined; the state was NOT modified at all.
    exc.applied == True  : every defined effect has been applied; exc.undefined maps field -> what is
                           undefined: 'fpr[d][h]' -> u64 bit mask of the image, 'fpscr' / 'cr' / 'srr1' /
                           'hid2' -> u32 bit mask, 'srr0' / 'dsisr' / 'pc' / 'msr' / 'exception' -> True,
                           'mem' -> {address: u8 bit mask}.
  [crosscheck 2026-09-14] Revised after the differential cross-check against ps_A (crosscheck_ps.py): every
  place where the corpus leaves a choice open is evaluated as a set of admissible outcomes and exactly the
  differing bits are reported (see the "OUTCOME SETS" section).

EXTERNAL ASSUMPTIONS (the corpus explicitly defers these to "PowerPC Microprocessor Family: The
Programming Environments manual" (PEM), which is not in the corpus; the standard PowerPC architecture
definitions are used and each use is tagged EXTERNAL-PEM in the code):
  E1 FPSCR bit layout (FX 0, FEX 1, VX 2, OX 3, UX 4, ZX 5, XX 6, VXSNAN 7, VXISI 8, VXIDI 9, VXZDZ 10,
     VXIMZ 11, VXVC 12, FR 13, FI 14, FPRF 15-19 (C, FL, FG, FE, FU), VXSOFT 21, VXSQRT 22, VXCVI 23,
     VE 24, OE 25, UE 26, ZE 27, XE 28, NI 29, RN 30-31). Consistent with PAGE 83 ("bits 15-19 are the
     result bits", exception bits "0-14, and 22-23") and PAGE 82 (NI = FPSCR[29]).
  E2 RN encoding 0 nearest-even, 1 toward 0, 2 toward +inf, 3 toward -inf.
  E3 FX set when an exception bit goes 0->1; VX = OR of VX* ; FEX = OR of enabled exception bits.
  E4 FPRF class codes; FR = magnitude incremented by rounding; FI = inexact; XX sticky inexact.
  E5 [revised] NaN operand propagation: the quiet bit is the fraction MSB.  A QNaN operand propagates
     unchanged, an SNaN keeps its sign and gets the quiet bit, the rest of its payload is NOT given; with
     several NaN operands the PEM priority (Table 2-21 footnote PAGE 85) is NOT used - every NaN operand is
     admissible.  A generated QNaN has the all-ones exponent and the quiet bit; its payload is not given,
     nor its sign except for ps_nmadd / ps_nmsub (PAGE 522/524: sign 0).  (Was: first NaN in field order,
     payload kept, generated QNaN = 0x7FC00000.)
  E6 [revised] Underflow: tininess before OR after rounding, loss of accuracy as inexact OR denormalization
     loss (IEEE 754-1985 7.4 allows each; the corpus does not choose).  (Was: before rounding, inexact.)
  E7 Overflow with OE=0: +/-inf or +/-max by RN, OX, XX, FI set, FR undefined.  OE=1 / UE=1 enabled
     results: value, FPRF, FR, FI, XX not given.
  E8 Invalid / zero-divide with VE=0 / ZE=0: QNaN (E5) / signed inf, FR = FI = 0.  VE=1 / ZE=1: "No result"
     -> that half, FPRF, FR, FI not given; the other half is defined (per-half descriptions PAGE 502-531).
  E9 [revised] Exception entry for program / FP-unavailable / alignment: SRR0 = address of the excepting
     instruction; SRR1[0, 5-9, 16-31] = MSR (PAGE 166, PAGE 165 note on full-function reserved bits);
     SRR1[1-4, 10-15] NOT given (was: 0, SRR1[12] = 1 for illegal).
  E10 FPSCR[NI]=1: only the Table 2-21/2-22 NI columns are applied (denormalized operand -> zero of unknown
     sign then "Do the operation", with UX/XX/FR/FI open; denormalized result -> zero of unknown sign with
     UX/XX/FR/FI open); NaN rows are identical to IEEE mode.  (Was: total UB for any NaN / denormal / invalid
     under NI.)  Compares and ps_sel: both the zeroed and the plain comparison are admissible.
  E11 MSR[FE0|FE1] != 0 and FEX = 1 after the instruction: taken-ness / settings of the program exception
     and the instruction's own commits (frD, FPSCR, CR) are not given (PAGE 166, 175, 46, 223).
  E12 Memory model: State.mem is the effective address space; no DSI, and W = I = 0 for dcbz_l (the real
     mode WIMG 0b0011 of PAGE 134 in every mode).
"""
from __future__ import annotations

import itertools
from fractions import Fraction

from gekko_state import MASK32, MASK64, State, bits_to_double, double_to_bits  # noqa: F401

AREA = "ps"


class UndefinedBehaviour(Exception):
    def __init__(self, what, *, applied=False, undefined=None):
        super().__init__(what)
        self.what = what
        self.applied = applied
        self.undefined = dict(undefined or {})


# ------------------------------------------------------------------------------------------------
# bit helpers - big-endian numbering, bit 0 = MSB (PAGE 78 GQR figure numbers bits 0..31 left to right)
# ------------------------------------------------------------------------------------------------
def _f(word, a, b):
    return (word >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def _bit(n):
    return 1 << (31 - n)


# MSR bits, Table 4-4 PAGE 164-165
MSR_POW = _bit(13)
MSR_ILE = _bit(15)
MSR_EE = _bit(16)
MSR_PR = _bit(17)
MSR_FP = _bit(18)
MSR_ME = _bit(19)
MSR_FE0 = _bit(20)
MSR_SE = _bit(21)
MSR_BE = _bit(22)
MSR_FE1 = _bit(23)
MSR_IP = _bit(25)
MSR_IR = _bit(26)
MSR_DR = _bit(27)
MSR_PM = _bit(29)
MSR_RI = _bit(30)
MSR_LE = _bit(31)

# HID2 bits, Table 2-6 PAGE 67
HID2_LSQE = _bit(0)
HID2_WPE = _bit(1)
HID2_PSE = _bit(2)
HID2_LCE = _bit(3)
HID2_DCHERR = _bit(8)
HID2_DCHEE = _bit(12)
# HID0[DCE] bit 17, PAGE 64
HID0_DCE = _bit(17)

# FPSCR bits - EXTERNAL-PEM E1
FX, FEX, VX, OX, UX, ZX, XX = (_bit(i) for i in range(7))
VXSNAN, VXISI, VXIDI, VXZDZ, VXIMZ, VXVC = (_bit(i) for i in range(7, 13))
FR, FI = _bit(13), _bit(14)
FPRF_MASK = 0x0001F000
FPCC_MASK = 0x0000F000
VXSOFT, VXSQRT, VXCVI = _bit(21), _bit(22), _bit(23)
VE, OE, UE, ZE, XE, NI = (_bit(i) for i in range(24, 30))
VX_ALL = VXSNAN | VXISI | VXIDI | VXZDZ | VXIMZ | VXVC | VXSOFT | VXSQRT | VXCVI
STICKY_EXC = OX | UX | ZX | XX | VX_ALL

# FPRF codes (C FL FG FE FU) << 12 - EXTERNAL-PEM E4
FPRF_QNAN = 0x11 << 12
FPRF_NINF = 0x09 << 12
FPRF_NNORM = 0x08 << 12
FPRF_NDEN = 0x18 << 12
FPRF_NZERO = 0x12 << 12
FPRF_PZERO = 0x02 << 12
FPRF_PDEN = 0x14 << 12
FPRF_PNORM = 0x04 << 12
FPRF_PINF = 0x05 << 12

DEFAULT_QNAN_IMAGE = 0x7FF8000000000000  # placeholder QNaN image; only exponent + quiet bit are defined (E5)
ONE_IMAGE = 0x3FF0000000000000
HALF = Fraction(1, 2)


def _pow2(k):
    return Fraction(1 << k) if k >= 0 else Fraction(1, 1 << -k)


MIN_NORMAL = _pow2(-126)
OVERFLOW_AT = _pow2(128)
MAX_SINGLE_BITS = 0x7F7FFFFF


def _floor_log2(mag):
    e = mag.numerator.bit_length() - mag.denominator.bit_length()
    if mag < _pow2(e):
        e -= 1
    elif mag >= _pow2(e + 1):
        e += 1
    return e


# ------------------------------------------------------------------------------------------------
# single <-> image conversions (representation decision, PAGE 82-83)
# ------------------------------------------------------------------------------------------------
def _single_bits_to_image(b):
    s = (b >> 31) & 1
    ex = (b >> 23) & 0xFF
    fr = b & 0x7FFFFF
    if ex == 0xFF:
        return (s << 63) | (0x7FF << 52) | (fr << 29)
    if ex == 0 and fr == 0:
        return s << 63
    if ex == 0:
        m, k = fr, -149
    else:
        m, k = (0x800000 | fr), ex - 150
    e = m.bit_length() - 1 + k
    mant = m << (52 - (m.bit_length() - 1))
    return (s << 63) | ((e + 1023) << 52) | (mant & ((1 << 52) - 1))


def _image_to_single_bits(img):
    """Return the 32-bit single image, or None when img is not exactly a single-precision value."""
    img &= MASK64
    s = img >> 63
    ex = (img >> 52) & 0x7FF
    fr = img & ((1 << 52) - 1)
    if ex == 0x7FF:
        if fr == 0:
            return (s << 31) | (0xFF << 23)
        if fr & ((1 << 29) - 1):
            return None
        return (s << 31) | (0xFF << 23) | (fr >> 29)
    if ex == 0:
        return (s << 31) if fr == 0 else None
    e = ex - 1023
    m = (1 << 52) | fr  # value = m * 2^(e-52)
    if e > 127:
        return None
    if e >= -126:
        if fr & ((1 << 29) - 1):
            return None
        return (s << 31) | ((e + 127) << 23) | (fr >> 29)
    if e < -149:
        return None
    sh = e + 97  # value / 2^-149 = m * 2^(e-52+149)
    if sh >= 0:
        frac = m << sh
    else:
        if m & ((1 << -sh) - 1):
            return None
        frac = m >> -sh
    return (s << 31) | frac


class _V:
    """A single-precision operand value."""
    __slots__ = ("kind", "sign", "mag", "bits")

    def __init__(self, bits):
        self.bits = bits
        self.sign = (bits >> 31) & 1
        ex = (bits >> 23) & 0xFF
        fr = bits & 0x7FFFFF
        self.mag = None
        if ex == 0xFF:
            self.kind = "inf" if fr == 0 else ("qnan" if fr & 0x400000 else "snan")
        elif ex == 0:
            if fr == 0:
                self.kind = "zero"
            else:
                self.kind = "fin"
                self.mag = Fraction(fr) * _pow2(-149)
        else:
            self.kind = "fin"
            self.mag = Fraction(0x800000 | fr) * _pow2(ex - 150)

    @property
    def nan(self):
        return self.kind in ("qnan", "snan")

    @property
    def denormal(self):
        return self.kind == "fin" and self.mag < MIN_NORMAL

    def signed(self):
        if self.kind == "zero":
            return Fraction(0)
        if self.kind == "inf":
            return float("-inf") if self.sign else float("inf")
        return -self.mag if self.sign else self.mag


def _mag_to_single_bits(sign, mag):
    """mag: Fraction >= 0 exactly representable as a single."""
    if mag == 0:
        return sign << 31
    if mag >= MIN_NORMAL:
        e = _floor_log2(mag)
        m = mag / _pow2(e - 23)
        assert m.denominator == 1 and (1 << 23) <= m.numerator < (1 << 24)
        return (sign << 31) | ((e + 127) << 23) | (m.numerator - (1 << 23))
    fr = mag / _pow2(-149)
    assert fr.denominator == 1
    return (sign << 31) | fr.numerator


def _fprf_of_image(img):
    """FPRF class of a single result image (EXTERNAL-PEM E4)."""
    b = _image_to_single_bits(img)
    v = _V(b)
    if v.nan:
        return FPRF_QNAN
    if v.kind == "inf":
        return FPRF_NINF if v.sign else FPRF_PINF
    if v.kind == "zero":
        return FPRF_NZERO if v.sign else FPRF_PZERO
    if v.denormal:
        return FPRF_NDEN if v.sign else FPRF_PDEN
    return FPRF_NNORM if v.sign else FPRF_PNORM


# ------------------------------------------------------------------------------------------------
# execution context: collects partially-undefined fields
# ------------------------------------------------------------------------------------------------
class _Ctx:
    def __init__(self):
        self.undef = {}
        self.notes = []

    def mark(self, field, value, note):
        if isinstance(value, dict):  # 'mem': {address: undefined bit mask of that byte}
            cur = self.undef.setdefault(field, {})
            for a, m in value.items():
                cur[a] = cur.get(a, 0) | m
        elif isinstance(value, bool) or isinstance(value, tuple):
            self.undef[field] = value
        else:
            self.undef[field] = self.undef.get(field, 0) | value
        if note not in self.notes:
            self.notes.append(note)


def _total(what):
    raise UndefinedBehaviour(what, applied=False)


# ------------------------------------------------------------------------------------------------
# exceptions
# ------------------------------------------------------------------------------------------------
_VECTOR = {"alignment": 0x00600, "program": 0x00700, "fp_unavailable": 0x00800}  # PAGE 175
SRR1_SPECIFIC_MASK = 0x783F0000  # SRR1[1-4, 10-15] PAGE 166 step 2
# [crosscheck fix] SRR1[0] is copied too: MSR bit 0 is "Reserved. Full function" (Table 4-4 PAGE 164) and
# "Full function reserved bits are saved in SRR1 when an exception occurs" (PAGE 165 note)
SRR1_MSR_COPY_MASK = 0x87C0FFFF  # SRR1[0, 5-9, 16-31] PAGE 166 step 3 + PAGE 165 note


def _take_exception(st, ctx, name):
    old = st.msr
    # SRR0: EXTERNAL-PEM E9 (PAGE 166 step 1 defers to the individual description, PAGE 175 defers to PEM)
    st.srr0 = st.pc & MASK32
    # PAGE 166 steps 2-3.  [crosscheck fix] the exception-specific bits SRR1[1-4, 10-15] are "loaded with
    # information specific to the exception type" whose values the corpus defers to the PEM (PAGE 175 "Chapter
    # 6, 'Exceptions' in the PowerPC Microprocessor Family: The Programming Environments manual describes
    # register settings for this exception"): undefined, not the PEM's 0 / SRR1[12] (former E9).
    st.srr1 = old & SRR1_MSR_COPY_MASK
    ctx.mark("srr1", SRR1_SPECIFIC_MASK, "SRR1[1-4,10-15] exception-specific contents deferred to PEM (PAGE 166, 175)")
    # Table 4-6 PAGE 168: ILE, ME, IP "not altered", LE <- ILE, all others cleared,
    # "Reserved bits are read as if written as 0"
    new = old & (MSR_ILE | MSR_ME | MSR_IP)
    if old & MSR_ILE:
        new |= MSR_LE
    st.msr = new
    # PAGE 166 step 5 / PAGE 168: vector offset added to 0x000n_nnnn or 0xFFFn_nnnn by MSR[IP]
    st.pc = ((0xFFF00000 if old & MSR_IP else 0) | _VECTOR[name]) & MASK32
    st.exception = name


def _illegal(st, ctx):
    # PAGE 90: illegal instruction -> program exception handler; PAGE 175 program exception 0x700
    _take_exception(st, ctx, "program")


def _alignment(st, ctx, ea):
    _take_exception(st, ctx, "alignment")
    st.dar = ea & MASK32  # PAGE 59 "After a DSI or an alignment exception, DAR is set to the EA"
    ctx.mark("dsisr", True, "alignment exception DSISR contents not given by the corpus (PAGE 175 defers to PEM)")


# ------------------------------------------------------------------------------------------------
# operand access
# ------------------------------------------------------------------------------------------------
def _opnd(st, reg, half):
    b = _image_to_single_bits(st.fpr[reg][half])
    if b is None:
        _total("fpr[%d] ps%d does not hold a single-precision value; format relationship unspecified "
               "(PAGE 83 'It is a programming error to apply double-precision instructions to paired "
               "single operands and vice versa')" % (reg, half))
    return _V(b)


# ------------------------------------------------------------------------------------------------
# exact arithmetic (all intermediates exact, one final rounding to single, PAGE 502 and siblings:
# "The result is rounded to single-precision under control of ... RN")
#
# [crosscheck fix] OUTCOME SETS.  The corpus leaves several choices open inside one computation
# (NaN payload / priority, tininess, loss of accuracy, NI zero sign, enabled-exception results,
# completion of an instruction taking a precise FP enabled exception).  Each half is evaluated to a
# LIST of admissible outcomes; _commit_pair takes the product of both halves' lists and reports as
# undefined exactly the bits that differ between outcomes (plus each outcome's own undefined image
# bits).  All FPSCR/CR outputs are monotone in the sticky bits and FR/FI, so an outcome whose
# sticky bit / FR / FI is not given is represented by its all-0 and all-1 extremes.
# ------------------------------------------------------------------------------------------------
IMG_SIGN = 1 << 63
IMG_PAYLOAD = ((1 << 22) - 1) << 29       # single NaN payload bits below the quiet bit, image position
IMG_INF = 0x7FF << 52
MAX_SINGLE = Fraction((1 << 24) - 1) * _pow2(104)


def _num(v):
    """Tagged exact value for a non-NaN operand."""
    if v.kind == "zero":
        return ("zero", v.sign)
    if v.kind == "inf":
        return ("inf", v.sign)
    return ("fin", v.sign, v.mag)


def _t_add(x, y, rn):
    """x + y on tagged exact values (IEEE-754 sign rules)."""
    if x[0] == "inf" and y[0] == "inf":
        return ("inf", x[1]) if x[1] == y[1] else ("invalid", VXISI)
    if x[0] == "inf":
        return x
    if y[0] == "inf":
        return y
    vx = Fraction(0) if x[0] == "zero" else (-x[2] if x[1] else x[2])
    vy = Fraction(0) if y[0] == "zero" else (-y[2] if y[1] else y[2])
    s = vx + vy
    if s == 0:
        if x[0] == "zero" and y[0] == "zero" and x[1] == y[1]:
            return ("zero", x[1])
        return ("zero", 1 if rn == 3 else 0)
    return ("fin", 1 if s < 0 else 0, abs(s))


def _t_neg(x):
    if x[0] == "invalid":
        return x
    return (x[0], 1 - x[1]) + tuple(x[2:])


def _t_mul(x, y):
    sign = x[1] ^ y[1]
    if (x[0] == "inf" and y[0] == "zero") or (x[0] == "zero" and y[0] == "inf"):
        return ("invalid", VXIMZ)
    if x[0] == "inf" or y[0] == "inf":
        return ("inf", sign)
    if x[0] == "zero" or y[0] == "zero":
        return ("zero", sign)
    return ("fin", sign, x[2] * y[2])


def _t_div(x, y):
    sign = x[1] ^ y[1]
    if x[0] == "inf" and y[0] == "inf":
        return ("invalid", VXIDI)
    if x[0] == "zero" and y[0] == "zero":
        return ("invalid", VXZDZ)
    if x[0] == "inf":
        return ("inf", sign)
    if y[0] == "inf":
        return ("zero", sign)
    if y[0] == "zero":
        return ("zdiv", sign)
    if x[0] == "zero":
        return ("zero", sign)
    return ("fin", sign, x[2] / y[2])


def _rnd(mag, qexp, sign, rn):
    q = mag / _pow2(qexp)
    n = q.numerator // q.denominator
    rem = q - n
    up = False
    if rem != 0:
        if rn == 0:
            up = rem > HALF or (rem == HALF and (n & 1) == 1)
        elif rn == 2:
            up = sign == 0
        elif rn == 3:
            up = sign == 1
    if up:
        n += 1
    return n * _pow2(qexp), rem != 0, up


def _round_single(sign, mag, rn):
    e = _floor_log2(mag)
    val, inexact, up = _rnd(mag, max(e - 23, -149), sign, rn)
    # reference rounding with an unbounded exponent range: tininess "after rounding" and
    # denormalization loss are defined against it (IEEE 754-1985 7.4) [crosscheck fix]
    val_u, _, _ = _rnd(mag, e - 23, sign, rn)
    return {"val": val, "inexact": inexact, "up": up, "tiny_before": mag < MIN_NORMAL,
            "tiny_after": val_u < MIN_NORMAL, "denorm_loss": val != val_u,
            "overflow": val >= OVERFLOW_AT}


class _Half:
    """One admissible outcome of one half.  fr / fi None = not given; amb = sticky bits not given;
    fprf None = not given; umask = image bits not given; noresult = 'No result' (VE/ZE enabled)."""

    def __init__(self, image, umask=0, sets=0, amb=0, fr=0, fi=0, fprf="auto", noresult=False):
        self.image = image & MASK64
        self.umask = umask & MASK64
        self.sets = sets
        self.amb = amb
        self.fr = fr
        self.fi = fi
        self.fprf = _fprf_of_image(self.image) if fprf == "auto" else fprf
        self.noresult = noresult


def _noresult(sets, amb=0):
    # "* No result if FPSCR[ZE] = 1", "** No result if FPSCR[VE] = 1" (PAGE 525/526); FPRF is set "except
    # for invalid operation exceptions when FPSCR[VE] = 1 and zero divide exceptions when FPSCR[ZE] = 1"
    # (PAGE 507): the corpus gives neither the target half nor FPRF, FR and FI for that computation.
    return [_Half(0, umask=MASK64, sets=sets, amb=amb, fr=None, fi=None, fprf=None, noresult=True)]


def _generated_qnan(st, flag, nm_family, amb=0):
    if st.fpscr & VE:
        return _noresult(flag, amb)
    # E5 revised: the default QNaN image is PEM chapter 3 (not in the corpus); Table 2-22 PAGE 85 only says
    # "Return QNaN" -> exponent and quiet bit defined, payload not; sign defined only for ps_nmadd /
    # ps_nmsub: "QNaNs that are generated as the result of a disabled invalid operation exception have a
    # sign bit of zero" (PAGE 522, PAGE 524).  FR = FI = 0 (not incremented, not inexact; PAGE 399 wording).
    um = IMG_PAYLOAD | (0 if nm_family else IMG_SIGN)
    return [_Half(DEFAULT_QNAN_IMAGE, umask=um, sets=flag, amb=amb, fprf=FPRF_QNAN)]


def _nan_prop(st, ordered, amb=0):
    """E5 revised: None when no operand is a NaN; else the admissible propagated results."""
    nans = [v for v in ordered if v.nan]
    if not nans:
        return None
    sets = VXSNAN if any(v.kind == "snan" for v in nans) else 0
    if sets and st.fpscr & VE:
        return _noresult(sets, amb)
    out, seen = [], set()
    for v in nans:
        if v.bits in seen:
            continue
        seen.add(v.bits)
        # a QNaN operand propagates unchanged ("QNaNs propagate with no effect on their sign bit" PAGE 522;
        # Table 2-21 PAGE 84-85 "QNaN"); an SNaN "converted to QNaNs ... retain[s] the sign bit" (PAGE 522):
        # quiet bit set, rest of the payload conversion not given.  With several NaN operands the choice is
        # "Prioritize according to Chapter 3 ... Programming Environments manual" (Table 2-21 footnote
        # PAGE 85), not in the corpus: every NaN operand is admissible.
        out.append(_Half(_single_bits_to_image(v.bits | 0x400000),
                         umask=0 if v.kind == "qnan" else IMG_PAYLOAD, sets=sets, amb=amb, fprf=FPRF_QNAN))
    return out


def _finish(st, t, negate=False, nm_family=False):
    fp = st.fpscr
    rn = fp & 3
    kind = t[0]
    if kind == "invalid":
        return _generated_qnan(st, t[1], nm_family)
    neg = IMG_SIGN if negate else 0
    if kind == "zdiv":
        if fp & ZE:
            return _noresult(ZX)
        return [_Half((t[1] << 63) | IMG_INF, sets=ZX)]
    if kind == "inf":
        return [_Half(((t[1] << 63) | IMG_INF) ^ neg)]
    if kind == "zero":
        return [_Half((t[1] << 63) ^ neg)]
    sign, mag = t[1], t[2]
    r = _round_single(sign, mag, rn)
    if r["overflow"]:
        to_inf = rn == 0 or (rn == 2 and sign == 0) or (rn == 3 and sign == 1)  # E7
        bits = ((sign << 31) | (0xFF << 23)) if to_inf else ((sign << 31) | MAX_SINGLE_BITS)
        img = _single_bits_to_image(bits) ^ neg
        if fp & OE:
            # enabled overflow: delivered value, FPRF, FR, FI and XX not in the corpus
            return [_Half(img, umask=MASK64, sets=OX, amb=XX, fr=None, fi=None, fprf=None)]
        return [_Half(img, sets=OX | XX, fr=None, fi=1)]
    img = _single_bits_to_image(_mag_to_single_bits(sign, r["val"])) ^ neg
    out = []
    # E6 revised: tininess before OR after rounding, loss of accuracy as inexact OR denormalization loss
    # (IEEE 754-1985 7.4 allows each; the corpus does not choose)
    for tiny in {r["tiny_before"], r["tiny_after"]}:
        if tiny and fp & UE:
            # enabled underflow: delivered value, FPRF, FR, FI and XX not in the corpus
            out.append(_Half(img, umask=MASK64, sets=UX, amb=XX, fr=None, fi=None, fprf=None))
            continue
        for loss in ({False} if fp & UE else {r["inexact"], r["denorm_loss"]}):
            s = XX if r["inexact"] else 0
            if tiny and loss:
                s |= UX
            out.append(_Half(img, sets=s, fr=1 if r["up"] else 0, fi=1 if r["inexact"] else 0))
    if not fp & NI:
        return out
    # FPSCR[NI]=1: Table 2-22 PAGE 85 "Single Denormalized ... Return zero" - sign and exception bits not
    # given, nor whether "denormalized" is judged before or after rounding
    denorm = {r["val"] != 0 and r["val"] < MIN_NORMAL, r["tiny_before"]}
    res = list(out) if (False in denorm or fp & UE) else []
    if True in denorm:
        res += [_Half(0, amb=UX | XX, fr=None, fi=None), _Half(IMG_SIGN, amb=UX | XX, fr=None, fi=None)]
    return res


def _ni_operands(st, *vals):
    """Table 2-21 PAGE 84, FPSCR[NI]=1: a denormalized operand is replaced by zero ("Zero A", ...) before
    "Do the operation"; the sign of that zero is not given.  Yields (tagged operands, zeroed?)."""
    idx = [i for i, v in enumerate(vals) if v.denormal] if st.fpscr & NI else []
    base = [_num(v) for v in vals]
    if not idx:
        yield base, False
        return
    for signs in itertools.product((0, 1), repeat=len(idx)):
        nums = list(base)
        for i, s in zip(idx, signs):
            nums[i] = ("zero", s)
        yield nums, True


def _zeroed(halves):
    # whether replacing a denormalized operand is itself signalled is not given (PAGE 82 "delivering
    # results that approximate the values required by the IEEE standard")
    for h in halves:
        h.amb |= UX | XX
        h.fr = None
        h.fi = None
    return halves


def _half_binop(st, op, a, b):
    """op in add/sub/mul/div; a = first field operand, b = second (frB, or frC for mul)."""
    h = _nan_prop(st, [a, b])  # Table 2-21: NaN rows are the same for NI = 0 and NI = 1
    if h is not None:
        return h
    out = []
    for (x, y), zeroed in _ni_operands(st, a, b):
        if op == "add":
            t = _t_add(x, y, st.fpscr & 3)
        elif op == "sub":
            t = _t_add(x, _t_neg(y), st.fpscr & 3)
        elif op == "mul":
            t = _t_mul(x, y)
        else:
            t = _t_div(x, y)
        res = _finish(st, t)
        out += _zeroed(res) if zeroed else res
    return out


def _half_madd(st, a, c, b, subtract, negate):
    """[a * c] +/- b, fused (PAGE 508 'added to this intermediate product ... The result is rounded');
    nmadd/nmsub = round then negate (PAGE 522 'produces the same result as would be obtained by using
    the Paired Single Multiply-Add (ps_maddx) instruction and then negating the result'; PAGE 524)."""
    amb = 0
    if not (a.nan or c.nan):
        ni = st.fpscr & NI
        ka = "zero" if (ni and a.denormal) else a.kind
        kc = "zero" if (ni and c.denormal) else c.kind
        if {ka, kc} == {"inf", "zero"}:
            # infinity * 0 with a NaN addend: whether VXIMZ is also signalled is not given by the corpus
            # (PAGE 508 lists VXIMZ but not its interaction with a NaN operand)
            amb = VXIMZ
    h = _nan_prop(st, [a, b, c], amb)
    if h is not None:
        return h  # PAGE 522/524: "QNaNs propagate with no effect on their sign bit" (not negated)
    rn = st.fpscr & 3
    out = []
    for (x, y, z), zeroed in _ni_operands(st, a, c, b):
        p = _t_mul(x, y)
        if p[0] == "invalid":
            res = _finish(st, p, nm_family=negate)
        else:
            res = _finish(st, _t_add(p, _t_neg(z) if subtract else z, rn), negate=negate, nm_family=negate)
        out += _zeroed(res) if zeroed else res
    return out


def _apply_fpscr(old, sets, fr, fi, fprf):
    new = old
    if sets & STICKY_EXC & ~old:
        new |= FX  # E3
    new |= sets
    if fr is not None:
        new = (new & ~FR) | (FR if fr else 0)
    if fi is not None:
        new = (new & ~FI) | (FI if fi else 0)
    if fprf is not None:
        new = (new & ~FPRF_MASK) | fprf
    new = (new & ~VX) | (VX if new & VX_ALL else 0)
    en = ((new & VX_ALL) and (new & VE)) or ((new & OX) and (new & OE)) or ((new & UX) and (new & UE)) \
        or ((new & ZX) and (new & ZE)) or ((new & XX) and (new & XE))
    new = (new & ~FEX) | (FEX if en else 0)
    return new & MASK32


def _set_cr1_from_fpscr(st, ctx):
    # PAGE 83: "when RC=1, the CR1 field of the CR is set from FPSCR bits 0-3" (moves / merges / ps_sel leave
    # the FPSCR untouched, so it is fully defined here)
    st.cr = (st.cr & ~0x0F000000) | ((st.fpscr >> 4) & 0x0F000000)


def _fe_marks(ctx):
    # PAGE 166 "If either bit is set, all IEEE enabled floating-point exceptions are taken and cause a
    # program exception"; the register settings are PEM (PAGE 175).  A precise exception means "neither the
    # faulting instruction nor subsequent instructions ... will complete execution" (PAGE 46) and completing
    # "implies committing execution results to the architected destination registers" (PAGE 223), against
    # the instruction pages' unconditional updates: taking the exception and the commit are both undefined.
    note = ("floating-point enabled program exception (MSR[FE0|FE1]!=0, FPSCR[FEX]=1): taken-ness, entry settings "
            "and completion of the instruction not given (PAGE 166, 175, 46, 223)")
    for fld in ("pc", "srr0", "msr", "exception"):
        ctx.mark(fld, True, note)
    ctx.mark("srr1", MASK32, note)


def _commit_pair(st, ctx, d, halves, fprf_idx, rc):
    """halves: two entries, each a list of _Half outcomes (computed) or an int image (pure move, no FPSCR
    effect).  PAGE 83: FPRF from the ps0 computation (ps1 for ps_sum1); exception bits 0-14, 22-23 'either
    the ps0 or the ps1 computation can set the bit'."""
    fp, cr_old = st.fpscr, st.cr
    old = list(st.fpr[d])
    lists = [h if isinstance(h, list) else [_Half(h)] for h in halves]
    outs = []
    for h0, h1 in itertools.product(*lists):
        for ext in (0, 1):
            sets = fr = fi = 0
            for h in (h0, h1):
                sets |= h.sets | (h.amb if ext else 0)
                fr |= ext if h.fr is None else h.fr
                fi |= ext if h.fi is None else h.fi
            src = (h0, h1)[fprf_idx]
            # PAGE 502 "except for invalid operation exceptions when FPSCR[VE] = 1" does not say which half's
            # exception suspends the ps0 rule: a 'No result' in either half leaves FPRF undefined
            if h0.noresult or h1.noresult or src.fprf is None:
                fprf, uf = None, FPRF_MASK
            else:
                fprf, uf = src.fprf, 0
            new = _apply_fpscr(fp, sets, fr, fi, fprf)
            # PAGE 83: "when RC=1, the CR1 field of the CR is set from FPSCR bits 0-3"
            cr = ((cr_old & ~0x0F000000) | ((new >> 4) & 0x0F000000)) if rc else cr_old
            outs.append((h0.image, h1.image, new, cr, h0.umask, h1.umask, uf))
    if (st.msr & (MSR_FE0 | MSR_FE1)) and any(o[2] & FEX for o in outs):
        _fe_marks(ctx)
        outs.append((old[0], old[1], fp, cr_old, 0, 0, 0))  # the not-completed alternative
    b = outs[0]
    m0 = m1 = mf = mc = 0
    for o in outs:
        m0 |= (o[0] ^ b[0]) | o[4]
        m1 |= (o[1] ^ b[1]) | o[5]
        mf |= (o[2] ^ b[2]) | o[6]
        mc |= o[3] ^ b[3]
    note = ("paired single outcome not fully given by the corpus: alternatives differ (NaN PAGE 85/522, NI PAGE 84-85, "
            "enabled exceptions PAGE 502-531/166)")
    st.fpr[d] = [b[0], b[1]]  # parallel evaluation: PAGE 83 "Simultaneously"
    st.fpscr = b[2]
    st.cr = b[3]
    if m0:
        ctx.mark("fpr[%d][0]" % d, m0, note)
    if m1:
        ctx.mark("fpr[%d][1]" % d, m1, note)
    if mf:
        ctx.mark("fpscr", mf, note)
    if mc:
        ctx.mark("cr", mc, note)


# ------------------------------------------------------------------------------------------------
# decode tables
# ------------------------------------------------------------------------------------------------
# A-form XO (bits 26-30) PAGE 583 Table A-1 / chapter 12 pages; reserved field list per figure
A_FORM = {
    10: ("ps_sum0", ()),            # PAGE 530
    11: ("ps_sum1", ()),            # PAGE 531
    12: ("ps_muls0", ((16, 20),)),  # PAGE 518 "0 0 0 0 0 C 12"
    13: ("ps_muls1", ((16, 20),)),  # PAGE 519
    14: ("ps_madds0", ()),          # PAGE 509
    15: ("ps_madds1", ()),          # PAGE 510
    18: ("ps_div", ((21, 25),)),    # PAGE 507 "B 0 0 0 0 0 18"
    20: ("ps_sub", ((21, 25),)),    # PAGE 529
    21: ("ps_add", ((21, 25),)),    # PAGE 502
    23: ("ps_sel", ()),             # PAGE 528
    24: ("ps_res", ((11, 15), (21, 25))),     # PAGE 525
    25: ("ps_mul", ((16, 20),)),    # PAGE 517
    26: ("ps_rsqrte", ((11, 15), (21, 25))),  # PAGE 526
    28: ("ps_msub", ()),            # PAGE 516
    29: ("ps_madd", ()),            # PAGE 508
    30: ("ps_nmsub", ()),           # PAGE 524
    31: ("ps_nmadd", ()),           # PAGE 522
}
# X-form XO (bits 21-30)
X10_FORM = {
    0: ("ps_cmpu0", ((9, 10), (31, 31))),     # PAGE 505
    32: ("ps_cmpo0", ((9, 10), (31, 31))),    # PAGE 503
    64: ("ps_cmpu1", ((9, 10), (31, 31))),    # PAGE 506
    96: ("ps_cmpo1", ((9, 10), (31, 31))),    # PAGE 504
    40: ("ps_neg", ((11, 15),)),              # PAGE 521
    72: ("ps_mr", ((11, 15),)),               # PAGE 515
    136: ("ps_nabs", ((11, 15),)),            # PAGE 520
    264: ("ps_abs", ((11, 15),)),             # PAGE 501
    528: ("ps_merge00", ()),                  # PAGE 511
    560: ("ps_merge01", ()),                  # PAGE 512
    592: ("ps_merge10", ()),                  # PAGE 513 (x'1000 04A0'; PAGE 583 '1001010101' is a typo)
    624: ("ps_merge11", ()),                  # PAGE 514
    1014: ("dcbz_l", ((6, 10), (31, 31))),    # PAGE 384
}
# 6-bit XO (bits 25-30), W bit 21, I bits 22-24, bit 31 reserved (PAGE 495-500)
X6_FORM = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}
D_FORM = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}  # PAGE 493-494, 497-498


def mnemonic(word):
    op = word >> 26
    if op in D_FORM:
        return D_FORM[op]
    if op != 4:
        return None
    xo5 = (word >> 1) & 0x1F
    if xo5 in A_FORM:
        return A_FORM[xo5][0]
    if xo5 in (6, 7):
        return X6_FORM[(word >> 1) & 0x3F]
    xo10 = (word >> 1) & 0x3FF
    if xo10 in X10_FORM:
        return X10_FORM[xo10][0]
    return "illegal"


def supports(word):
    return (word >> 26) in (4, 56, 57, 60, 61)


def _check_reserved(word, name, ranges):
    for a, b in ranges:
        if _f(word, a, b):
            # PAGE 87: "If instructions are encoded with incorrectly set bits in reserved fields, the
            # results on execution can be said to be boundedly undefined."
            _total("%s: reserved bits %d-%d set -> boundedly undefined (PAGE 87)" % (name, a, b))


def _ps_gate(st, ctx):
    """Return True if execution may proceed. PAGE 162 priority: program (1) before FP unavailable (3)."""
    if not st.hid2 & HID2_PSE:
        _illegal(st, ctx)  # PAGE 90 "all other paired single instructions are illegal when HID2[PSE] = 0"
        return False
    if not st.msr & MSR_FP:
        # PAGE 83 "paired single instructions, including loads, stores and moves, cause a floating-point
        # unavailable exception if execution is attempted when MSR[FP] = 0"; PAGE 175 vector 0x800
        _take_exception(st, ctx, "fp_unavailable")
        return False
    return True


# ------------------------------------------------------------------------------------------------
# instruction bodies
# ------------------------------------------------------------------------------------------------
def _exec_a(st, ctx, word, name):
    d, a, b, c, rc = _f(word, 6, 10), _f(word, 11, 15), _f(word, 16, 20), _f(word, 21, 25), word & 1
    if not _ps_gate(st, ctx):
        return
    if name in ("ps_add", "ps_sub", "ps_div"):
        # PAGE 502 / 529 / 507: frD(psN) <- frA(psN) op frB(psN)
        op = {"ps_add": "add", "ps_sub": "sub", "ps_div": "div"}[name]
        ops = [(_opnd(st, a, h), _opnd(st, b, h)) for h in (0, 1)]
        halves = [_half_binop(st, op, x, y) for x, y in ops]
        _commit_pair(st, ctx, d, halves, 0, rc)
    elif name in ("ps_mul", "ps_muls0", "ps_muls1"):
        # PAGE 517: A(h)*C(h); PAGE 518: A(h)*C(0); PAGE 519: A(h)*C(1)
        csel = {"ps_mul": (0, 1), "ps_muls0": (0, 0), "ps_muls1": (1, 1)}[name]
        ops = [(_opnd(st, a, h), _opnd(st, c, csel[h])) for h in (0, 1)]
        halves = [_half_binop(st, "mul", x, y) for x, y in ops]
        _commit_pair(st, ctx, d, halves, 0, rc)
    elif name in ("ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub", "ps_madds0", "ps_madds1"):
        # PAGE 508 madd, 516 msub, 522 nmadd, 524 nmsub: A(h)*C(h) (+/-) B(h)
        # PAGE 509 madds0: A(h)*C(0) + B(h); PAGE 510 madds1: A(h)*C(1) + B(h)
        csel = {"ps_madds0": (0, 0), "ps_madds1": (1, 1)}.get(name, (0, 1))
        subtract = name in ("ps_msub", "ps_nmsub")
        negate = name in ("ps_nmadd", "ps_nmsub")
        ops = [(_opnd(st, a, h), _opnd(st, c, csel[h]), _opnd(st, b, h)) for h in (0, 1)]
        halves = [_half_madd(st, x, y, z, subtract, negate) for x, y, z in ops]
        _commit_pair(st, ctx, d, halves, 0, rc)
    elif name == "ps_sum0":
        # PAGE 530: ps0 <- A(0) + B(1); ps1 <- C(1); FPRF from ps0
        x, y = _opnd(st, a, 0), _opnd(st, b, 1)
        mv = _opnd(st, c, 1)
        h0 = _half_binop(st, "add", x, y)
        _commit_pair(st, ctx, d, [h0, _single_bits_to_image(mv.bits)], 0, rc)
    elif name == "ps_sum1":
        # PAGE 531: ps0 <- C(0); ps1 <- A(0) + B(1); "FPRF is set to the class and sign of the ps1 result"
        mv = _opnd(st, c, 0)
        x, y = _opnd(st, a, 0), _opnd(st, b, 1)
        h1 = _half_binop(st, "add", x, y)
        _commit_pair(st, ctx, d, [_single_bits_to_image(mv.bits), h1], 1, rc)
    elif name == "ps_sel":
        # PAGE 528: if A(h) >= 0.0 then C(h) else B(h); NaN -> B; "ignore the sign of zero"; no FPSCR
        out = []
        masks = []
        for h in (0, 1):
            va = _opnd(st, a, h)
            ge = (not va.nan) and (va.kind == "zero" or va.sign == 0)
            choices = [ge]
            if st.fpscr & NI and va.denormal and not ge:
                # [crosscheck fix] FPSCR[NI]=1: Table 2-21 PAGE 84 replaces a denormalized operand by zero for
                # arithmetic; whether ps_sel's comparison (mode-independent pseudocode, PAGE 528) sees the zero
                # (-> frC, since "These comparisons ignore the sign of zero") is not given: both selections
                choices.append(True)
            imgs = [_single_bits_to_image(_opnd(st, c if g else b, h).bits) for g in choices]
            m = 0
            for im in imgs:
                m |= im ^ imgs[0]
            out.append(imgs[0])
            masks.append(m)
        st.fpr[d] = out
        for h in (0, 1):
            if masks[h]:
                ctx.mark("fpr[%d][%d]" % (d, h), masks[h], "ps_sel with FPSCR[NI]=1 and a negative denormal (PAGE 84, 528)")
        if rc:
            _set_cr1_from_fpscr(st, ctx)
    elif name in ("ps_res", "ps_rsqrte"):
        _exec_estimate(st, ctx, name, d, b, rc)
    st.pc = (st.pc + 4) & MASK32


def _exec_estimate(st, ctx, name, d, b, rc):
    halves = []
    for h in (0, 1):
        v = _opnd(st, b, h)
        if v.nan:
            outs = _nan_prop(st, [v])  # table PAGE 525/526: SNaN -> QNaN VXSNAN ("** No result if VE=1"), QNaN -> QNaN
        else:
            # FPSCR[NI]=1 does not flush the operand of an estimate (hardware-verified: hwtests cputest/ni.cpp
            # inputs_expected_flushed = false, Common/FloatUtils.h fres_expected / frsqrte_expected against a
            # real Gekko), contrary to Table 2-21 PAGE 84: the denormal itself is estimated.
            outs = _estimate_value(st, name, v)
        for o in outs:
            o.fr = None  # "FR (undefined), FI (undefined)" PAGE 525 / PAGE 527; XX is not Affected
            o.fi = None
        halves.append(outs)
    _commit_pair(st, ctx, d, halves, 0, rc)


def _estimate_value(st, name, v):
    if v.kind == "zero":
        if st.fpscr & ZE:
            return _noresult(ZX)  # "* No result if FPSCR[ZE] = 1"
        return [_Half((v.sign << 63) | IMG_INF, sets=ZX)]  # -0 -> -inf ZX, +0 -> +inf ZX
    if v.kind == "inf" and v.sign == 0:
        return [_Half(0)]  # +inf -> +0
    if name == "ps_res" and v.kind == "inf":
        return [_Half(IMG_SIGN)]  # PAGE 525: -inf -> -0
    if name == "ps_rsqrte" and v.sign == 1:
        return _generated_qnan(st, VXSQRT, False)  # PAGE 526: -inf, <0 -> QNaN VXSQRT
    # [crosscheck fix] finite nonzero operand: the estimate is only "correct to a precision of one part in 4096"
    # (PAGE 525/526), so its bits are not given - but the instruction still completes (it was total UB before)
    if name == "ps_rsqrte":
        # 1/sqrt(x) for a positive single x lies in [2^-64, 2^74.5]: always a positive normal
        return [_Half(0, umask=MASK64, fprf=FPRF_PNORM)]
    r = 1 / v.mag
    if r * Fraction(4095, 4096) >= MIN_NORMAL and r * Fraction(4097, 4096) <= MAX_SINGLE:
        return [_Half(0, umask=MASK64, fprf=FPRF_NNORM if v.sign else FPRF_PNORM)]
    # the admissible estimates straddle the overflow / underflow threshold: OX, UX and FPRF not given
    return [_Half(0, umask=MASK64, amb=OX | UX, fprf=None)]


def _exec_x10(st, ctx, word, name):
    if name == "dcbz_l":
        _exec_dcbz_l(st, ctx, word)
        return
    if name.startswith("ps_cmp"):
        crf, a, b = _f(word, 6, 8), _f(word, 11, 15), _f(word, 16, 20)
        if not _ps_gate(st, ctx):
            return
        h = 0 if name.endswith("0") else 1
        va, vb = _opnd(st, a, h), _opnd(st, b, h)
        pairs = [(va, vb)]
        if st.fpscr & NI and (va.denormal or vb.denormal):
            # [crosscheck fix] FPSCR[NI]=1: whether the compare sees a denormalized operand replaced by zero
            # (Table 2-21 PAGE 84) is not given; the PAGE 503-506 pseudocode is mode-independent
            pairs.append((_V(0) if va.denormal else va, _V(0) if vb.denormal else vb))
        sets = 0
        ordered = name.startswith("ps_cmpo")
        if va.kind == "snan" or vb.kind == "snan":
            sets |= VXSNAN
            if ordered and not st.fpscr & VE:
                sets |= VXVC
        elif ordered and (va.kind == "qnan" or vb.kind == "qnan"):
            sets |= VXVC
        shift = 28 - 4 * crf
        outs = []
        for p, q in pairs:
            # PAGE 503-506 pseudocode
            if p.nan or q.nan:
                c = 0b0001
            elif p.signed() < q.signed():
                c = 0b1000
            elif p.signed() > q.signed():
                c = 0b0100
            else:
                c = 0b0010
            new = _apply_fpscr(st.fpscr, sets, None, None, None)
            new = (new & ~FPCC_MASK) | (c << 12)  # "FPCC <- c"
            outs.append((new, (st.cr & ~(0xF << shift)) | (c << shift)))  # "CR[(4*crfD),(4*crfD+3)] <- c"
        if (st.msr & (MSR_FE0 | MSR_FE1)) and any(o[0] & FEX for o in outs):
            _fe_marks(ctx)
            outs.append((st.fpscr, st.cr))
        mf = mc = 0
        for o in outs:
            mf |= o[0] ^ outs[0][0]
            mc |= o[1] ^ outs[0][1]
        st.fpscr, st.cr = outs[0]
        if mf:
            ctx.mark("fpscr", mf, "compare outcome alternatives differ (PAGE 84, 166, 503-506)")
        if mc:
            ctx.mark("cr", mc, "compare outcome alternatives differ (PAGE 84, 166, 503-506)")
        st.pc = (st.pc + 4) & MASK32
        return
    d, a, b, rc = _f(word, 6, 10), _f(word, 11, 15), _f(word, 16, 20), word & 1
    if not _ps_gate(st, ctx):
        return
    if name in ("ps_neg", "ps_mr", "ps_nabs", "ps_abs"):
        # PAGE 521 "bit 0 inverted", 515 move, 520 "bit 0 set", 501 "bit 0 cleared"; no FPSCR change
        out = []
        for h in (0, 1):
            img = _single_bits_to_image(_opnd(st, b, h).bits)
            if name == "ps_neg":
                img ^= 1 << 63
            elif name == "ps_nabs":
                img |= 1 << 63
            elif name == "ps_abs":
                img &= ~(1 << 63) & MASK64
            out.append(img)
        st.fpr[d] = out
    else:
        # PAGE 511-514: mergeXY: ps0 <- A(psX), ps1 <- B(psY)
        x, y = {"ps_merge00": (0, 0), "ps_merge01": (0, 1), "ps_merge10": (1, 0), "ps_merge11": (1, 1)}[name]
        va, vb = _opnd(st, a, x), _opnd(st, b, y)
        st.fpr[d] = [_single_bits_to_image(va.bits), _single_bits_to_image(vb.bits)]
    if rc:
        _set_cr1_from_fpscr(st, ctx)
    st.pc = (st.pc + 4) & MASK32


def _exec_dcbz_l(st, ctx, word):
    a, b = _f(word, 11, 15), _f(word, 16, 20)
    ea = ((st.gpr[a] if a else 0) + st.gpr[b]) & MASK32  # PAGE 384 "EA is the sum (rA|0) + (rB)"
    if not st.hid2 & HID2_LCE:
        _illegal(st, ctx)  # PAGE 384 / PAGE 311 "When HID2[LCE] = 0, execution of dcbz_l causes an illegal instruction exception"
        return
    if not st.hid0 & HID0_DCE:
        _alignment(st, ctx, ea)  # PAGE 175 "An attempt is made to execute dcbz or dcbz_l when the data cache is disabled"
        return
    base = ea & ~0x1F & MASK32  # PAGE 311 "allocate a 32 byte block in the locked cache"; cache-line aligned (PAGE 77)
    # [crosscheck fix] "All bytes are cleared" holds in both the miss and the hit case (PAGE 384), so the zeroed
    # block is defined.  Modelling convention (as for every other data access here, where no DSI is modelled):
    # the State has no BATs / page table; translation succeeds with W = I = 0, i.e. the real-mode WIMG 0b0011
    # (PAGE 134), so the write-through / cache-inhibited alignment case (PAGE 311) does not arise.
    st.mem.write(base, 32, 0)
    note = "dcbz_l cache hit (HID2[DCHERR] <- 1, possible machine check) depends on L1 cache state not in the model (PAGE 311, 384)"
    # [crosscheck fix] a hit SETS DCHERR, a miss leaves it: undefined only while it is 0
    if not st.hid2 & HID2_DCHERR:
        ctx.mark("hid2", HID2_DCHERR, note)
        # [crosscheck fix] the machine check needs "HID2[DCHEE] = MSR[EE] = MSR[ME] = 1, and HID2[DCHERR] = 0" (PAGE 311)
        if st.hid2 & HID2_DCHEE and st.msr & MSR_EE and st.msr & MSR_ME:
            for fld in ("exception", "pc", "srr0", "msr"):
                ctx.mark(fld, True, note)
            ctx.mark("srr1", MASK32, note)
    st.pc = (st.pc + 4) & MASK32


def _signed6(v):
    return v - 64 if v & 0x20 else v


def gqr_fields(gqr):
    """Table 2-16 PAGE 78: LD_SCALE 2-7, LD_TYPE 13-15, ST_SCALE 18-23, ST_TYPE 29-31; scales are
    'the twos compliment value' (PAGE 110 / PAGE 112). Reserved bits are ignored."""
    return {"ld_scale": _signed6(_f(gqr, 2, 7)), "ld_type": _f(gqr, 13, 15),
            "st_scale": _signed6(_f(gqr, 18, 23)), "st_type": _f(gqr, 29, 31)}


def _type_width(t):
    # PAGE 493 pseudocode "c <- 4; if lt = (4|6) then c <- 10; if lt = (5|7) then c <- 20" - the 10/20
    # are extraction typos for 1/2 (750CL PAGE 567 "c <- 1", "c <- 2"; PAGE 78 Table 2-17 8/16 bit types)
    return 4 if t == 0 else (1 if t in (4, 6) else 2)


def dequantize(raw, typ, scale):
    """Return the ps image for a memory operand. PAGE 111: type 0 'passed directly to the register
    without any conversion. This includes the case where the operand is a denorm.' PAGE 110:
    F = I * 2**(-S)."""
    if typ == 0:
        return _single_bits_to_image(raw)
    if typ == 6 and raw & 0x80:
        raw -= 0x100
    if typ == 7 and raw & 0x8000:
        raw -= 0x10000
    val = Fraction(raw) * _pow2(-scale)
    if val == 0:
        return 0
    # |I| <= 65535 and S in [-32, 31]: always exactly representable, no rounding
    return _single_bits_to_image(_mag_to_single_bits(1 if val < 0 else 0, abs(val)))


_INT_RANGE = {4: (0, 255), 5: (0, 65535), 6: (-128, 127), 7: (-32768, 32767)}


def quantize(image, typ, scale):
    """Return (integer bit pattern of width c stored for a ps image, bits of it the corpus does not give)."""
    b = _image_to_single_bits(image)
    if b is None:
        _total("quantized store of a ps slot that is not a single-precision value (PAGE 83)")
    v = _V(b)
    if typ == 0:
        # PAGE 112: "passed directly to the L1 cache without any conversion, except when this operand is a
        # denorm. In the case of a denorm, the value 0.0 is stored"
        if v.denormal:
            # [crosscheck fix] a NEGATIVE denorm: only the sign of the stored zero is open (PAGE 112 'the value
            # 0.0', PAGE 84 'stored as the value 0'), not the whole instruction
            return 0, (0x80000000 if v.sign else 0)
        return b, 0
    lo, hi = _INT_RANGE[typ]
    mask = 0xFF if typ in (4, 6) else 0xFFFF
    if v.nan:
        if v.sign == 0:
            return hi & mask, 0  # PAGE 111 "+Inf or NaN is the same as the adjusted result value for overflow of positive values"
        # [crosscheck fix] negative NaN: Gekko PAGE 111 says positive-overflow value, 750CL PAGE 108 says
        # negative-overflow value -> the stored integer is undefined (the rest of the instruction is not)
        return hi & mask, mask
    if v.kind == "inf":
        return (lo if v.sign else hi) & mask, 0  # PAGE 111-112 (+Inf -> positive, -Inf -> negative overflow value)
    if v.kind == "zero":
        return 0, 0
    x = v.signed() * _pow2(scale)  # PAGE 112 "I = ROUND(F * 2**(S))"
    t = int(x)  # PAGE 111 step 5 "Round toward zero" (int() of a Fraction truncates toward zero)
    t = max(lo, min(hi, t))  # PAGE 111 step 6 overflow adjustment
    return t & mask, 0


def _exec_psq(st, ctx, word, name):
    load = "_l" in name
    update = name in ("psq_lu", "psq_lux", "psq_stu", "psq_stux")
    indexed = name.endswith("x")
    if indexed:
        _check_reserved(word, name, ((31, 31),))  # PAGE 495-500 figure bit 31 "0"
        d, a, b, w, i = _f(word, 6, 10), _f(word, 11, 15), _f(word, 16, 20), _f(word, 21, 21), _f(word, 22, 24)
        disp = None
    else:
        # PAGE 493 figure: 56 | D | A | W (16) | I (17-19) | d (20-31)
        d, a, w, i, disp = _f(word, 6, 10), _f(word, 11, 15), _f(word, 16, 16), _f(word, 17, 19), _f(word, 20, 31)
        b = None
    if update and a == 0:
        # PAGE 494/495/498/499 "If rA = 0, the instruction form is invalid."; PAGE 87 invalid forms
        _total("%s with rA = 0: invalid form (PAGE 494/495/498/499, PAGE 87)" % name)
    hid2 = st.hid2
    # PAGE 493/497 "if HID2[PSE] = 0 | HID2[LSQE] = 0 then Goto illegal instruction error handler";
    # PAGE 495/496/499/500 indexed forms: PSE only
    if not hid2 & HID2_PSE or (not indexed and not hid2 & HID2_LSQE):
        _illegal(st, ctx)
        return
    if not st.msr & MSR_FP:
        _take_exception(st, ctx, "fp_unavailable")  # PAGE 83 "including loads, stores"
        return
    if st.msr & MSR_LE:
        _total("%s with MSR[LE]=1: 'will give undefined results' (PAGE 109-110)" % name)
    g = gqr_fields(st.gqr[i])
    typ = g["ld_type"] if load else g["st_type"]
    scale = g["ld_scale"] if load else g["st_scale"]
    if typ in (1, 2, 3):
        _total("%s with GQR%d type %d: 'reserved' (Table 2-17 PAGE 78)" % (name, i, typ))
    c = _type_width(typ)
    if indexed:
        base = st.gpr[a] if (update or a != 0) else 0  # PAGE 496 "if rA = 0 then b <- 0"; PAGE 495 "(rA) + (rB)"
        ea = (base + st.gpr[b]) & MASK32
    else:
        base = st.gpr[a] if (update or a != 0) else 0  # PAGE 493 "(rA|0)"; PAGE 494 "(rA)"
        ea = (base + (disp - 0x1000 if disp & 0x800 else disp)) & MASK32  # EXTS(d), 12-bit d (PAGE 110)
    if typ == 0 and ea & 3:
        # PAGE 175 NOTE (clarified by 750CL PAGE 175 "if the operand is not word-aligned when ... type 0");
        # Table 4-3 PAGE 162 alignment priority 6 (below FP unavailable)
        _alignment(st, ctx, ea)
        return
    if load:
        ps0 = dequantize(st.mem.read(ea, c), typ, scale)
        if w == 0:
            ps1 = dequantize(st.mem.read((ea + c) & MASK32, c), typ, scale)
        else:
            ps1 = ONE_IMAGE  # PAGE 493 "frD(ps1) <- 1.0"
        st.fpr[d] = [ps0, ps1]
    else:
        q0, u0 = quantize(st.fpr[d][0], typ, scale)
        q1, u1 = quantize(st.fpr[d][1], typ, scale) if w == 0 else (None, 0)
        st.mem.write(ea, c, q0)  # PAGE 497 "MEM(EA,c) <- quantized(frS(ps0),stt,sts)"
        if w == 0:
            st.mem.write((ea + c) & MASK32, c, q1)  # "MEM(EA+c,c) <- quantized(frS(ps1),stt,sts)"
        for at, um in ((ea, u0), ((ea + c) & MASK32, u1)):
            if um:
                ctx.mark("mem", {(at + i) & MASK32: (um >> (8 * (c - 1 - i))) & 0xFF for i in range(c)
                                 if (um >> (8 * (c - 1 - i))) & 0xFF},
                         "quantized store bits not given (PAGE 112 denorm sign / PAGE 111 vs 750CL PAGE 108 NaN)")
    if update:
        st.gpr[a] = ea  # PAGE 494 "rA <- EA"
    st.pc = (st.pc + 4) & MASK32


def _execute(st, ctx, word):
    op = word >> 26
    if op in D_FORM:
        _exec_psq(st, ctx, word, D_FORM[op])
        return
    if op != 4:
        raise ValueError("ps_B.step: unsupported word %08X" % word)
    xo5 = (word >> 1) & 0x1F
    if xo5 in A_FORM:
        name, res = A_FORM[xo5]
        _check_reserved(word, name, res)
        _exec_a(st, ctx, word, name)
        return
    if xo5 in (6, 7):
        _exec_psq(st, ctx, word, X6_FORM[(word >> 1) & 0x3F])
        return
    xo10 = (word >> 1) & 0x3FF
    if xo10 in X10_FORM:
        name, res = X10_FORM[xo10]
        _check_reserved(word, name, res)
        _exec_x10(st, ctx, word, name)
        return
    _illegal(st, ctx)  # PAGE 88 unused extended opcodes of primary 4 are illegal


def _commit_state(dst, src):
    for k, v in vars(src).items():
        if k == "mem":
            dst.mem.bytes.clear()
            dst.mem.bytes.update(src.mem.bytes)
            dst.mem.log[:] = src.mem.log
        elif k in ("gpr", "fpr", "gqr", "sprg"):
            getattr(dst, k)[:] = v
        else:
            setattr(dst, k, v)


def step(state, word):
    word &= MASK32
    work = state.clone()
    work.exception = None
    ctx = _Ctx()
    _execute(work, ctx, word)
    _commit_state(state, work)
    if ctx.undef:
        raise UndefinedBehaviour("; ".join(ctx.notes), applied=True, undefined=ctx.undef)


# ------------------------------------------------------------------------------------------------
# ps1 rules for scalar floating-point instructions (data only; executed by the fp area)
# ------------------------------------------------------------------------------------------------
SCALAR_PS1_RULES = {
    # PAGE 83: "all the single-precision floating-point instructions (fadds, fsubs, fmuls, fdivs, fmadds,
    # fmsubs, fnmadds, fnmsubs, fres, frsp) are valid, and operate on the ps0 operand ... The ps1 value in
    # the destination register is duplicated from the ps0 result"; e.g. PAGE 396 fadds "placed in both
    # frD(ps0) and frD(ps1)", PAGE 417 fres
    "fadds": "ps1 <- ps0 result (PAGE 83, PAGE 396)",
    "fsubs": "ps1 <- ps0 result (PAGE 83, PAGE 423)",
    "fmuls": "ps1 <- ps0 result (PAGE 83, PAGE 409)",
    "fdivs": "ps1 <- ps0 result (PAGE 83, PAGE 402)",
    "fmadds": "ps1 <- ps0 result (PAGE 83, PAGE 404)",
    "fmsubs": "ps1 <- ps0 result (PAGE 83, PAGE 407)",
    "fnmadds": "ps1 <- ps0 result (PAGE 83, PAGE 413)",
    "fnmsubs": "ps1 <- ps0 result (PAGE 83, PAGE 415)",
    "fres": "ps1 <- ps0 result (PAGE 83, PAGE 417)",
    # PAGE 418: "placed into frD(ps0). The value in frD(ps1) is undefined." (PAGE 83 "See Page 12-85 for an exception about frsp")
    "frsp": "ps1 UNDEFINED (PAGE 418)",
    # PAGE 434 lfs: "placed into frD(ps0) and replicated in frD(ps1)"; PAGE 83 lfs[u][x]
    "lfs": "ps1 <- ps0 (PAGE 83, PAGE 434)", "lfsu": "ps1 <- ps0 (PAGE 83)", "lfsx": "ps1 <- ps0 (PAGE 83)",
    "lfsux": "ps1 <- ps0 (PAGE 83)",
    # PAGE 83 "The store floating-point single instructions (stfs[u][x]) store the ps0 value only."
    "stfs": "stores ps0 only (PAGE 83, PAGE 110)",
    # PAGE 405 fmr: paired-single source -> "frB[ps0] is copied to frD[ps0] and the content of frD[ps1] is unchanged"
    "fmr": "paired-single source: ps0 copied, ps1 unchanged; double source: whole operand copied (PAGE 405)",
    # PAGE 421 fsel: paired-single source -> "The content of frD[ps1] is undefined"
    "fsel": "paired-single source: only frA(ps0) compared, ps0 selected, ps1 UNDEFINED (PAGE 421)",
    # PAGE 83 "all the double-precision instructions are still valid, and execute as in non-paired single
    # mode"; the effect on a ps1 value is not given ("relationship ... is unspecified")
    "double-precision": "ps1 effect not specified (PAGE 83)",
}


# ================================================================================================
# self-test: hand-derived vectors
# ================================================================================================
if __name__ == "__main__":
    import struct

    def simg(x):
        """double image of a value that is exactly a single."""
        f = struct.unpack(">f", struct.pack(">f", x))[0]
        assert f == x or x != x, x
        return double_to_bits(f)

    def enc_a(xo, d, a, b, c, rc=0):
        return (4 << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1) | rc

    def enc_x(xo, d, a, b, rc=0):
        return (4 << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | rc

    def enc_cmp(xo, crf, a, b):
        return (4 << 26) | (crf << 23) | (a << 16) | (b << 11) | (xo << 1)

    def enc_q(op, d, a, w, i, disp):
        return (op << 26) | (d << 21) | (a << 16) | (w << 15) | (i << 12) | (disp & 0xFFF)

    def enc_qx(xo6, d, a, b, w, i):
        return (4 << 26) | (d << 21) | (a << 16) | (b << 11) | (w << 10) | (i << 7) | (xo6 << 1)

    ADD, SUB, MUL, DIV, SEL, RES, RSQ = 21, 20, 25, 18, 23, 24, 26
    SUM0, SUM1, MULS0, MULS1, MADDS0, MADDS1, MSUB, MADD, NMSUB, NMADD = 10, 11, 12, 13, 14, 15, 28, 29, 30, 31
    CMPU0, CMPO0, CMPU1, CMPO1, NEG, MR, NABS, ABS = 0, 32, 64, 96, 40, 72, 136, 264
    M00, M01, M10, M11, DCBZL = 528, 560, 592, 624, 1014

    PINF, NINF = 0x7FF0000000000000, 0xFFF0000000000000
    QNAN = 0x7FF8000000000000
    NQNAN = 0xFFF8000000000000
    SNAN = 0x7FF0000020000000   # single 0x7F800001
    PZ, NZ = 0, 1 << 63
    MAXS = simg(3.4028234663852886e38)
    PC = 0x80003100

    def new(**kw):
        st = State()
        st.hid2 = HID2_PSE | HID2_LSQE
        st.msr = MSR_FP
        st.fpscr = 0
        st.pc = PC
        for k, v in kw.items():
            setattr(st, k, v)
        return st

    passed = [0]

    def run(label, st, word, expect_ub=None):
        before = st.clone()
        try:
            step(st, word)
            ub = None
        except UndefinedBehaviour as e:
            ub = e
        if expect_ub is None:
            assert ub is None, "%s: unexpected UB %s" % (label, ub)
        elif expect_ub == "total":
            assert ub is not None and not ub.applied, "%s: expected total UB, got %r" % (label, ub)
            assert st.arch_dict() == before.arch_dict() and st.mem.log == before.mem.log, label + ": state changed"
        else:
            assert ub is not None and ub.applied, "%s: expected applied UB, got %r" % (label, ub)
        return ub

    def um(ub, field):
        """undefined bit mask reported for one field (0 when defined)."""
        if ub is None:
            return 0
        v = ub.undefined.get(field, 0)
        return (MASK64 if field.startswith("fpr") else MASK32) if v is True else v

    def ok(label, cond):
        assert cond, label
        passed[0] += 1

    # 1 ps_add basic, both halves (PAGE 502)
    st = new(); st.fpr[2] = [simg(1.5), simg(-1.0)]; st.fpr[3] = [simg(2.25), simg(0.5)]
    run("add", st, enc_a(ADD, 1, 2, 3, 0))
    ok("v1 ps_add", st.fpr[1] == [simg(3.75), simg(-0.5)] and st.fpscr == FPRF_PNORM and st.pc == PC + 4
       and st.exception is None)

    # 2 ps_add inexact, round to nearest: 1 + 2^-30 -> 1.0, FI, XX, FX
    st = new(); st.fpr[2] = [simg(1.0), simg(1.0)]; st.fpr[3] = [simg(2.0 ** -30), 0]
    run("add-inexact", st, enc_a(ADD, 1, 2, 3, 0))
    ok("v2 ps_add inexact nearest", st.fpr[1] == [simg(1.0), simg(1.0)] and st.fpscr == 0x82024000)

    # 3 same, RN = +inf: 1 + 2^-23, FR set
    st = new(fpscr=2); st.fpr[2] = [simg(1.0), simg(1.0)]; st.fpr[3] = [simg(2.0 ** -30), 0]
    run("add-rn2", st, enc_a(ADD, 1, 2, 3, 0))
    ok("v3 ps_add RN=+inf", st.fpr[1] == [simg(1.0 + 2.0 ** -23), simg(1.0)] and st.fpscr == 0x82064002)

    # 4 FI/XX from ps1 only; FPRF from ps0; Rc=1 -> CR1 = FPSCR[0-3] (PAGE 83)
    st = new(); st.cr = 0xF0000000; st.fpr[2] = [simg(-1.0), simg(1.0)]; st.fpr[3] = [simg(-1.0), simg(2.0 ** -30)]
    run("add-ps1-inexact", st, enc_a(ADD, 1, 2, 3, 0, rc=1))
    ok("v4 FI from ps1, FPRF from ps0, CR1", st.fpr[1] == [simg(-2.0), simg(1.0)]
       and st.fpscr == (FX | XX | FI | FPRF_NNORM) and st.cr == 0xF8000000)

    # 5 overflow: max + max -> +inf, OX XX FI, FR undefined (E7)
    st = new(); st.fpr[2] = [MAXS, simg(1.0)]; st.fpr[3] = [MAXS, simg(1.0)]
    ub = run("add-ovf", st, enc_a(ADD, 1, 2, 3, 0), expect_ub="applied")
    ok("v5 ps_add overflow", st.fpr[1] == [PINF, simg(2.0)] and (st.fpscr & ~FR) == 0x92025000
       and ub.undefined.get("fpscr", 0) == FR)

    # 6 ps_sub inf - inf -> default QNaN VXISI (PAGE 529 VXISI)
    st = new(); st.fpr[2] = [PINF, simg(3.0)]; st.fpr[3] = [PINF, simg(1.0)]
    ub = run("sub-isi", st, enc_a(SUB, 1, 2, 3, 0), expect_ub="applied")
    # generated QNaN: sign and payload not given (default QNaN is PEM; PAGE 522/524 fix the sign only for nm*)
    ok("v6 ps_sub inf-inf", st.fpr[1] == [QNAN, simg(2.0)] and st.fpscr == 0xA0811000
       and um(ub, "fpr[1][0]") == IMG_SIGN | IMG_PAYLOAD and um(ub, "fpr[1][1]") == 0)

    # 7 zero signs RN nearest: 1-1 = +0, (-0)-(-0) = +0
    st = new(); st.fpr[2] = [simg(1.0), NZ]; st.fpr[3] = [simg(1.0), NZ]
    run("sub-zero", st, enc_a(SUB, 1, 2, 3, 0))
    ok("v7 ps_sub exact zero RN=0", st.fpr[1] == [PZ, PZ] and st.fpscr == FPRF_PZERO)

    # 8 zero signs RN -inf: both -0
    st = new(fpscr=3); st.fpr[2] = [simg(1.0), NZ]; st.fpr[3] = [simg(1.0), NZ]
    run("sub-zero-rn3", st, enc_a(SUB, 1, 2, 3, 0))
    ok("v8 ps_sub exact zero RN=-inf", st.fpr[1] == [NZ, NZ] and st.fpscr == (FPRF_NZERO | 3))

    # 9 ps_mul 0*inf -> VXIMZ (frA, frC fields; PAGE 517)
    st = new(); st.fpr[2] = [PZ, simg(2.0)]; st.fpr[4] = [PINF, simg(3.0)]
    ub = run("mul-imz", st, enc_a(MUL, 1, 2, 0, 4), expect_ub="applied")
    ok("v9 ps_mul 0*inf", st.fpr[1] == [QNAN, simg(6.0)] and st.fpscr == 0xA0111000
       and um(ub, "fpr[1][0]") == IMG_SIGN | IMG_PAYLOAD)

    # 10 ps_div 1/0 -> +inf ZX
    st = new(); st.fpr[2] = [simg(1.0), simg(6.0)]; st.fpr[3] = [PZ, simg(3.0)]
    run("div-zx", st, enc_a(DIV, 1, 2, 3, 0))
    ok("v10 ps_div 1/0", st.fpr[1] == [PINF, simg(2.0)] and st.fpscr == 0x84005000)

    # 11 ps_div 0/0 VXZDZ in ps0, -1/-0 ZX in ps1 -> both sticky sets (PAGE 83 either half)
    st = new(); st.fpr[2] = [PZ, simg(-1.0)]; st.fpr[3] = [PZ, NZ]
    ub = run("div-zdz", st, enc_a(DIV, 1, 2, 3, 0), expect_ub="applied")
    ok("v11 ps_div 0/0 and -1/-0", st.fpr[1] == [QNAN, PINF] and st.fpscr == 0xA4211000
       and um(ub, "fpr[1][1]") == 0 and um(ub, "fpscr") == 0)
    # nmadd 0*inf: the generated QNaN has a sign bit of zero (PAGE 522)
    st = new(); st.fpr[2] = [PZ, simg(1.0)]; st.fpr[4] = [NINF, simg(1.0)]; st.fpr[3] = [simg(1.0), simg(1.0)]
    ub = run("nmadd-imz", st, enc_a(NMADD, 1, 2, 3, 4), expect_ub="applied")
    ok("v11b ps_nmadd generated QNaN sign 0", um(ub, "fpr[1][0]") == IMG_PAYLOAD and st.fpr[1][0] == QNAN
       and st.fpr[1][1] == simg(-2.0))
    # inf / 0 = inf without ZX (IEEE 754-1985 7.2: division by zero needs a finite nonzero dividend)
    st = new(); st.fpr[2] = [NINF, simg(1.0)]; st.fpr[3] = [PZ, simg(1.0)]
    run("div-inf0", st, enc_a(DIV, 1, 2, 3, 0))
    ok("v11c ps_div inf/0", st.fpr[1] == [NINF, simg(1.0)] and st.fpscr == FPRF_NINF)

    # 12 ZE=1 zero divide: result undefined, sticky + FEX defined
    st = new(fpscr=ZE); st.fpr[2] = [simg(1.0), simg(1.0)]; st.fpr[3] = [PZ, simg(1.0)]
    ub = run("div-ze", st, enc_a(DIV, 1, 2, 3, 0), expect_ub="applied")
    ok("v12 ZE=1", (st.fpscr & ~(FPRF_MASK | FR | FI)) == 0xC4000010 and ub.undefined.get("fpr[1][0]"))

    # 13 ps_neg of QNaN flips sign, FPSCR untouched, Rc copies FPSCR[0-3] (PAGE 521)
    st = new(fpscr=0x80000000); st.fpr[3] = [QNAN, simg(1.0)]
    run("neg", st, enc_x(NEG, 1, 0, 3, rc=1))
    ok("v13 ps_neg NaN sign", st.fpr[1] == [NQNAN, simg(-1.0)] and st.fpscr == 0x80000000 and st.cr == 0x08000000)

    # 14 ps_abs aliasing D=B; ps_nabs (PAGE 501, 520)
    st = new(); st.fpr[1] = [simg(-2.0), NZ]
    run("abs", st, enc_x(ABS, 1, 0, 1))
    ok("v14 ps_abs", st.fpr[1] == [simg(2.0), PZ])
    run("nabs", st, enc_x(NABS, 1, 0, 1))
    ok("v15 ps_nabs", st.fpr[1] == [simg(-2.0), NZ] and st.fpscr == 0)

    # 16 ps_mr (PAGE 515)
    st = new(); st.fpr[6] = [SNAN, simg(7.0)]
    run("mr", st, enc_x(MR, 5, 0, 6))
    ok("v16 ps_mr SNaN no FPSCR", st.fpr[5] == [SNAN, simg(7.0)] and st.fpscr == 0)

    # 17 merges with aliasing (parallel semantics PAGE 83)
    st = new(); st.fpr[2] = [simg(1.0), simg(2.0)]; st.fpr[3] = [simg(3.0), simg(4.0)]
    s2 = st.clone(); run("m00", s2, enc_x(M00, 1, 2, 3))
    ok("v17 ps_merge00", s2.fpr[1] == [simg(1.0), simg(3.0)])
    s2 = st.clone(); run("m01", s2, enc_x(M01, 1, 2, 3))
    ok("v18 ps_merge01", s2.fpr[1] == [simg(1.0), simg(4.0)])
    s2 = st.clone(); run("m10", s2, enc_x(M10, 3, 2, 3))   # D == B
    ok("v19 ps_merge10 D==B", s2.fpr[3] == [simg(2.0), simg(3.0)])
    s2 = st.clone(); run("m11", s2, enc_x(M11, 2, 2, 3))   # D == A
    ok("v20 ps_merge11 D==A", s2.fpr[2] == [simg(2.0), simg(4.0)])

    # 21 ps_sel: -0 >= 0 selects C; NaN selects B (PAGE 528); fields A=2 B=3 C=4
    st = new(); st.fpr[2] = [NZ, QNAN]; st.fpr[3] = [simg(10.0), simg(20.0)]; st.fpr[4] = [simg(30.0), simg(40.0)]
    run("sel", st, enc_a(SEL, 1, 2, 3, 4))
    ok("v21 ps_sel -0/NaN", st.fpr[1] == [simg(30.0), simg(20.0)] and st.fpscr == 0)
    st.fpr[2] = [simg(-1.0), PINF]
    run("sel2", st, enc_a(SEL, 1, 2, 3, 4))
    ok("v22 ps_sel -1/+inf", st.fpr[1] == [simg(10.0), simg(40.0)])

    # 23 ps_cmpu0 crf7 less; ps_cmpu1 crf0 equal (PAGE 505, 506)
    st = new(); st.fpr[2] = [simg(1.0), simg(5.0)]; st.fpr[3] = [simg(2.0), simg(5.0)]
    run("cmpu0", st, enc_cmp(CMPU0, 7, 2, 3))
    ok("v23 ps_cmpu0", st.cr == 0x8 and st.fpscr == 0x8000)
    run("cmpu1", st, enc_cmp(CMPU1, 0, 2, 3))
    ok("v24 ps_cmpu1", st.cr == 0x20000008 and st.fpscr == 0x2000)

    # 25 ps_cmpo0 QNaN -> VXVC (PAGE 503)
    st = new(); st.fpr[2] = [QNAN, 0]; st.fpr[3] = [simg(1.0), 0]
    run("cmpo0", st, enc_cmp(CMPO0, 1, 2, 3))
    ok("v25 ps_cmpo0 QNaN", st.cr == 0x01000000 and st.fpscr == 0xA0081000)

    # 26 ps_cmpo1 SNaN VE=0 -> VXSNAN|VXVC (PAGE 504)
    st = new(); st.fpr[2] = [0, simg(1.0)]; st.fpr[3] = [0, SNAN]
    run("cmpo1", st, enc_cmp(CMPO1, 2, 2, 3))
    ok("v26 ps_cmpo1 SNaN VE=0", st.cr == 0x00100000 and st.fpscr == 0xA1081000)

    # 27 ps_cmpo1 SNaN VE=1 -> VXSNAN only, FEX; FE0=FE1=0 so no exception
    st = new(fpscr=VE); st.fpr[2] = [0, simg(1.0)]; st.fpr[3] = [0, SNAN]
    run("cmpo1-ve", st, enc_cmp(CMPO1, 2, 2, 3))
    ok("v27 ps_cmpo1 SNaN VE=1", st.fpscr == 0xE1001080 and st.exception is None)

    # 28 ps_cmpu0 SNaN -> VXSNAN, no VXVC
    st = new(); st.fpr[2] = [SNAN, 0]; st.fpr[3] = [simg(1.0), 0]
    run("cmpu0-snan", st, enc_cmp(CMPU0, 0, 2, 3))
    ok("v28 ps_cmpu0 SNaN", st.fpscr == 0xA1001000 and st.cr == 0x10000000)

    # 29 ps_sum0: ps0 = A0 + B1, ps1 = C1 (A=2 B=3 C=4)
    st = new(); st.fpr[2] = [simg(1.0), simg(100.0)]; st.fpr[3] = [simg(100.0), simg(2.0)]; st.fpr[4] = [simg(1000.0), simg(7.0)]
    run("sum0", st, enc_a(SUM0, 1, 2, 3, 4))
    ok("v29 ps_sum0", st.fpr[1] == [simg(3.0), simg(7.0)] and st.fpscr == FPRF_PNORM)

    # 30 ps_sum1: ps0 = C0, ps1 = A0 + B1, FPRF from ps1 (PAGE 531)
    st = new(); st.fpr[2] = [simg(-5.0), simg(100.0)]; st.fpr[3] = [simg(100.0), simg(2.0)]; st.fpr[4] = [simg(1000.0), simg(7.0)]
    run("sum1", st, enc_a(SUM1, 1, 2, 3, 4))
    ok("v30 ps_sum1 FPRF ps1", st.fpr[1] == [simg(1000.0), simg(-3.0)] and st.fpscr == FPRF_NNORM)

    # 31 ps_muls0 with D == C: ps1 uses the ORIGINAL C(ps0) (PAGE 518 + PAGE 83 parallel)
    st = new(); st.fpr[2] = [simg(2.0), simg(3.0)]; st.fpr[4] = [simg(5.0), simg(7.0)]
    s2 = st.clone(); run("muls0", s2, enc_a(MULS0, 4, 2, 0, 4))
    ok("v31 ps_muls0 D==C", s2.fpr[4] == [simg(10.0), simg(15.0)])
    s2 = st.clone(); run("muls1", s2, enc_a(MULS1, 1, 2, 0, 4))
    ok("v32 ps_muls1", s2.fpr[1] == [simg(14.0), simg(21.0)])

    # 33 ps_madd fused: (1+2^-12)^2 - 1 = 2^-11 + 2^-24 exactly (unfused would give 2^-11)
    st = new(); st.fpr[2] = [simg(1 + 2.0 ** -12), simg(3.0)]; st.fpr[4] = [simg(1 + 2.0 ** -12), simg(5.0)]
    st.fpr[3] = [simg(-1.0), simg(7.0)]
    run("madd", st, enc_a(MADD, 1, 2, 3, 4))
    ok("v33 ps_madd fused", st.fpr[1] == [simg(2.0 ** -11 + 2.0 ** -24), simg(22.0)] and st.fpscr == FPRF_PNORM)

    # 34 ps_madds0: A(h)*C(0)+B(h); ps_madds1: A(h)*C(1)+B(h)
    st = new(); st.fpr[2] = [simg(2.0), simg(3.0)]; st.fpr[4] = [simg(5.0), simg(100.0)]; st.fpr[3] = [simg(1.0), simg(1.0)]
    s2 = st.clone(); run("madds0", s2, enc_a(MADDS0, 1, 2, 3, 4))
    ok("v34 ps_madds0", s2.fpr[1] == [simg(11.0), simg(16.0)])
    s2 = st.clone(); run("madds1", s2, enc_a(MADDS1, 1, 2, 3, 4))
    ok("v35 ps_madds1", s2.fpr[1] == [simg(201.0), simg(301.0)])

    # 36 msub / nmadd / nmsub (PAGE 516, 522, 524)
    st = new(); st.fpr[2] = [simg(3.0), simg(2.0)]; st.fpr[4] = [simg(5.0), simg(2.0)]; st.fpr[3] = [simg(7.0), simg(4.0)]
    s2 = st.clone(); run("msub", s2, enc_a(MSUB, 1, 2, 3, 4))
    ok("v36 ps_msub", s2.fpr[1] == [simg(8.0), PZ] and s2.fpscr == FPRF_PNORM)
    s2 = st.clone(); run("nmadd", s2, enc_a(NMADD, 1, 2, 3, 4))
    ok("v37 ps_nmadd", s2.fpr[1] == [simg(-22.0), simg(-8.0)] and s2.fpscr == FPRF_NNORM)
    s2 = st.clone(); run("nmsub", s2, enc_a(NMSUB, 1, 2, 3, 4))
    ok("v38 ps_nmsub exact zero -> -0", s2.fpr[1] == [simg(-8.0), NZ])

    # 39 nmadd: QNaN propagates with its sign unchanged (PAGE 522)
    st = new(); st.fpr[2] = [simg(1.0), simg(1.0)]; st.fpr[4] = [simg(1.0), simg(1.0)]; st.fpr[3] = [NQNAN, simg(1.0)]
    run("nmadd-nan", st, enc_a(NMADD, 1, 2, 3, 4))
    ok("v39 ps_nmadd NaN sign", st.fpr[1] == [NQNAN, simg(-2.0)] and st.fpscr == FPRF_QNAN)

    # 40 madd inf*0 + QNaN: result = B NaN, VXIMZ unspecified -> applied UB with VXIMZ in mask
    st = new(); st.fpr[2] = [PINF, simg(1.0)]; st.fpr[4] = [PZ, simg(1.0)]; st.fpr[3] = [QNAN, simg(1.0)]
    ub = run("madd-imz-nan", st, enc_a(MADD, 1, 2, 3, 4), expect_ub="applied")
    ok("v40 madd IMZ ambiguity", st.fpr[1] == [QNAN, simg(2.0)] and ub.undefined["fpscr"] & VXIMZ)

    # 41 ps_res specials (PAGE 525): +0 -> +inf ZX, -inf -> -0; FR/FI undefined
    st = new(); st.fpr[3] = [PZ, NINF]
    ub = run("res", st, enc_a(RES, 1, 0, 3, 0), expect_ub="applied")
    ok("v41 ps_res specials", st.fpr[1] == [PINF, NZ] and (st.fpscr & ~(FR | FI)) == 0x84005000
       and ub.undefined["fpscr"] == FR | FI)

    # 42 ps_res finite -> the estimate's bits are not given, the instruction still completes (FPRF +normal)
    st = new(); st.fpr[3] = [simg(3.0), PZ]
    ub = run("res-finite", st, enc_a(RES, 1, 0, 3, 0), expect_ub="applied")
    ok("v42 ps_res estimate undefined", um(ub, "fpr[1][0]") == MASK64 and st.fpr[1][1] == PINF
       and (st.fpscr & ~(FR | FI)) == (FX | ZX | FPRF_PNORM) and um(ub, "fpscr") == FR | FI and st.pc == PC + 4)
    #    1/x straddling the overflow threshold: OX, UX, FPRF not given
    st = new(); st.fpr[3] = [_single_bits_to_image(0x00200001), simg(2.0)]
    ub = run("res-edge", st, enc_a(RES, 1, 0, 3, 0), expect_ub="applied")
    ok("v42b ps_res near overflow", um(ub, "fpscr") & (OX | UX | FPRF_MASK) == OX | UX | FPRF_MASK)

    # 43 ps_rsqrte: -1 -> QNaN VXSQRT, +inf -> +0 (PAGE 526)
    st = new(); st.fpr[3] = [simg(-1.0), PINF]
    run("rsqrte", st, enc_a(RSQ, 1, 0, 3, 0), expect_ub="applied")
    ok("v43 ps_rsqrte", st.fpr[1] == [QNAN, PZ] and (st.fpscr & ~(FR | FI)) == 0xA0011200)

    # 44 psq_l type 0 W=0 (PAGE 493)
    st = new(); st.gpr[3] = 0x80001000; st.mem.load(0x80001000, bytes.fromhex("3FC0000040000000"))
    run("psq_l t0", st, enc_q(56, 1, 3, 0, 0, 0))
    ok("v44 psq_l float", st.fpr[1] == [simg(1.5), simg(2.0)]
       and st.mem.log == [("r", 0x80001000, 4, 0x3FC00000), ("r", 0x80001004, 4, 0x40000000)])

    # 45 psq_l u8 scale +2 -> I * 2^-2 (PAGE 110)
    st = new(); st.gpr[3] = 0x80001000; st.gqr[1] = (2 << 24) | (4 << 16); st.mem.load(0x80001000, b"\xFF\x80")
    run("psq_l u8", st, enc_q(56, 1, 3, 0, 1, 0))
    ok("v45 psq_l u8", st.fpr[1] == [simg(63.75), simg(32.0)] and [x[2] for x in st.mem.log] == [1, 1])

    # 46 psq_l s8 scale -3 (0x3D), W=1 -> ps1 = 1.0
    st = new(); st.gpr[3] = 0x80001000; st.gqr[2] = (0x3D << 24) | (6 << 16); st.mem.load(0x80001000, b"\xFF\x7F")
    run("psq_l s8 W1", st, enc_q(56, 1, 3, 1, 2, 0))
    ok("v46 psq_l s8 W=1", st.fpr[1] == [simg(-8.0), ONE_IMAGE] and len(st.mem.log) == 1)

    # 47 psq_l s16 and u16 of 0x8000
    st = new(); st.gpr[3] = 0x80001000; st.gqr[3] = 7 << 16; st.gqr[4] = 5 << 16
    st.mem.load(0x80001000, b"\x80\x00\x7F\xFF")
    run("psq_l s16", st, enc_q(56, 1, 3, 0, 3, 0))
    ok("v47 psq_l s16", st.fpr[1] == [simg(-32768.0), simg(32767.0)])
    run("psq_l u16", st, enc_q(56, 2, 3, 0, 4, 0))
    ok("v48 psq_l u16", st.fpr[2] == [simg(32768.0), simg(32767.0)])

    # 49 Table 2-44 (PAGE 111): scale 100000 (-32) -> 4.29E+9, 011111 (31) -> 4.66E-10
    st = new(); st.gpr[3] = 0x80001000; st.gqr[5] = (0x20 << 24) | (4 << 16); st.gqr[6] = (0x1F << 24) | (4 << 16)
    st.mem.load(0x80001000, b"\x01\x01")
    run("t2-44a", st, enc_q(56, 1, 3, 1, 5, 0)); run("t2-44b", st, enc_q(56, 2, 3, 1, 6, 0))
    ok("v49 Table 2-44", st.fpr[1][0] == simg(4294967296.0) and st.fpr[2][0] == simg(2.0 ** -31))

    # 50 psq_lu negative displacement, rA <- EA (PAGE 494)
    st = new(); st.gpr[3] = 0x80001008; st.mem.load(0x80001000, bytes.fromhex("3FC0000040000000"))
    run("psq_lu", st, enc_q(57, 1, 3, 0, 0, -8))
    ok("v50 psq_lu", st.fpr[1] == [simg(1.5), simg(2.0)] and st.gpr[3] == 0x80001000)

    # 51 psq_lu rA = 0 -> invalid form, total UB
    st = new()
    run("psq_lu ra0", st, enc_q(57, 1, 0, 0, 0, 0), expect_ub="total")
    ok("v51 psq_lu rA=0", True)

    # 52 psq_lx rA=0 uses 0 (PAGE 496); psq_lux updates rA (PAGE 495)
    st = new(); st.gpr[4] = 0x80001000; st.gpr[5] = 0x80000FF0; st.gpr[6] = 0x10
    st.mem.load(0x80001000, bytes.fromhex("3FC0000040000000"))
    run("psq_lx", st, enc_qx(6, 1, 0, 4, 0, 0))
    ok("v52 psq_lx rA=0", st.fpr[1] == [simg(1.5), simg(2.0)])
    run("psq_lux", st, enc_qx(38, 2, 5, 6, 1, 0))
    ok("v53 psq_lux", st.fpr[2] == [simg(1.5), ONE_IMAGE] and st.gpr[5] == 0x80001000)

    # 54 psq_st u16 scale +1: 100 -> 200, 40000 -> 80000 saturates 65535 (PAGE 111-112)
    st = new(); st.gpr[3] = 0x80002000; st.gqr[1] = (1 << 8) | 5; st.fpr[1] = [simg(100.0), simg(40000.0)]
    run("psq_st u16", st, enc_q(60, 1, 3, 0, 1, 0))
    ok("v54 psq_st u16", st.mem.log == [("w", 0x80002000, 2, 200), ("w", 0x80002002, 2, 0xFFFF)])

    # 55 s8 truncation toward zero and clamping
    st = new(); st.gpr[3] = 0x80002000; st.gqr[1] = 6; st.fpr[1] = [simg(127.5), simg(-128.75)]
    run("psq_st s8", st, enc_q(60, 1, 3, 0, 1, 0))
    ok("v55 psq_st s8 trunc", [x[3] for x in st.mem.log] == [0x7F, 0x80])
    st.mem.log.clear(); st.fpr[1] = [simg(-129.0), PINF]
    run("psq_st s8 b", st, enc_q(60, 1, 3, 0, 1, 0))
    ok("v56 psq_st s8 clamp/+inf", [x[3] for x in st.mem.log] == [0x80, 0x7F])
    st.mem.log.clear(); st.fpr[1] = [NINF, QNAN]
    run("psq_st s8 c", st, enc_q(60, 1, 3, 0, 1, 0))
    ok("v57 psq_st s8 -inf/+NaN", [x[3] for x in st.mem.log] == [0x80, 0x7F])

    # 58 negative-sign NaN quantized: manuals disagree (PAGE 111 vs 750CL PAGE 108) -> that integer undefined
    st = new(); st.gpr[3] = 0x80002000; st.gqr[1] = 6; st.fpr[1] = [NQNAN, PZ]
    ub = run("psq_st -nan", st, enc_q(60, 1, 3, 1, 1, 0), expect_ub="applied")
    ok("v58 -NaN quantize", ub.undefined.get("mem") == {0x80002000: 0xFF} and st.pc == PC + 4)

    # 59 u8: -1 -> 0, 255.75 -> 255, 256 -> 255
    st = new(); st.gpr[3] = 0x80002000; st.gqr[1] = 4; st.fpr[1] = [simg(-1.0), simg(255.75)]
    run("psq_st u8", st, enc_q(60, 1, 3, 0, 1, 0))
    st.fpr[1] = [simg(256.0), simg(-0.5)]
    run("psq_st u8b", st, enc_q(60, 1, 3, 0, 1, 0))
    ok("v59 psq_st u8", [x[3] for x in st.mem.log] == [0, 255, 255, 0])

    # 60 Table 2-45 rows (PAGE 112): 100.0 with S=2 u8 255; S=31 s16 32767; S=-32 u16 0; S=1 s8 127; S=31 u16 65535
    tv = [(2, 4, 255), (31, 7, 32767), (-32, 5, 0), (1, 6, 127), (31, 5, 65535), (-2, 7, 25), (0, 4, 100)]
    for s, t, want in tv:
        ok("v60 Table 2-45 S=%d type %d" % (s, t), quantize(simg(100.0), t, s) == (want, 0))

    # 61 type 0 store: ps1 positive denorm stored as 0.0 (PAGE 84, 112); negative denorm -> total UB
    st = new(); st.gpr[3] = 0x80002000; st.fpr[1] = [simg(1.5), simg(2.0 ** -149)]
    run("psq_st t0 denorm", st, enc_q(60, 1, 3, 0, 0, 0))
    ok("v61 psq_st denorm", st.mem.log == [("w", 0x80002000, 4, 0x3FC00000), ("w", 0x80002004, 4, 0)])
    st.fpr[1] = [simg(1.5), simg(-(2.0 ** -149))]
    ub = run("psq_st t0 -denorm", st, enc_q(60, 1, 3, 0, 0, 0), expect_ub="applied")
    ok("v61b -denorm stores a zero of unknown sign", ub.undefined.get("mem") == {0x80002004: 0x80}
       and st.mem.log[-1] == ("w", 0x80002004, 4, 0))

    # 62 type 0 load keeps denorm and SNaN bit-exact (PAGE 111)
    st = new(); st.gpr[3] = 0x80001000; st.mem.load(0x80001000, bytes.fromhex("000000017F800001"))
    run("psq_l denorm", st, enc_q(56, 1, 3, 0, 0, 0))
    ok("v62 psq_l denorm/SNaN", st.fpr[1] == [simg(2.0 ** -149), SNAN])

    # 63 psq_stu / psq_stux update, psq_stx W=1 single write
    st = new(); st.gpr[3] = 0x80002010; st.gpr[4] = 0x80002000; st.gpr[5] = 0x20; st.gqr[1] = 4
    st.fpr[1] = [simg(9.0), simg(10.0)]
    run("psq_stu", st, enc_q(61, 1, 3, 1, 1, -16))
    ok("v63 psq_stu", st.gpr[3] == 0x80002000 and st.mem.log == [("w", 0x80002000, 1, 9)])
    run("psq_stux", st, enc_qx(39, 1, 4, 5, 0, 1))
    ok("v64 psq_stux", st.gpr[4] == 0x80002020 and st.mem.log[1:] == [("w", 0x80002020, 1, 9), ("w", 0x80002021, 1, 10)])
    run("psq_stx", st, enc_qx(7, 1, 0, 4, 1, 1))
    ok("v65 psq_stx W=1", st.mem.log[3:] == [("w", 0x80002020, 1, 9)] and st.gpr[4] == 0x80002020)

    # 66 PSE=0 -> illegal instruction program exception (PAGE 90, 175; Table 4-6 PAGE 168)
    st = new(hid2=HID2_LSQE, msr=0x80002032); st.fpr[2] = [simg(1.0), simg(1.0)]
    ub = run("pse0", st, enc_a(ADD, 1, 2, 2, 0), expect_ub="applied")
    # SRR1[1-4,10-15] not given by the corpus; SRR1[0] (full-function reserved MSR bit, PAGE 164/165) is copied
    ok("v66 PSE=0 program", st.exception == "program" and st.pc == 0x700 and st.srr0 == PC
       and st.srr1 == 0x80002032 and um(ub, "srr1") == SRR1_SPECIFIC_MASK and st.msr == 0 and st.fpr[1] == [0, 0])

    # 67 vector prefix and ILE (PAGE 166, 168)
    st = new(hid2=0, msr=MSR_FP | MSR_ILE | MSR_ME | MSR_IP | MSR_EE)
    run("pse0-ip", st, enc_x(MR, 1, 0, 2), expect_ub="applied")
    ok("v67 IP/ILE", st.pc == 0xFFF00700 and st.msr == (MSR_ILE | MSR_ME | MSR_IP | MSR_LE)
       and st.srr1 == ((MSR_FP | MSR_ILE | MSR_ME | MSR_IP | MSR_EE) & SRR1_MSR_COPY_MASK))

    # 68 LSQE=0: psq_l illegal, psq_lx legal (PAGE 493 vs 496)
    st = new(hid2=HID2_PSE); st.gpr[3] = 0x80001000; st.mem.load(0x80001000, bytes.fromhex("3FC0000040000000"))
    s2 = st.clone(); run("lsqe0 psq_l", s2, enc_q(56, 1, 3, 0, 0, 0), expect_ub="applied")
    ok("v68 LSQE=0 psq_l illegal", s2.exception == "program" and s2.mem.log == [])
    s2 = st.clone(); run("lsqe0 psq_lx", s2, enc_qx(6, 1, 0, 3, 0, 0))
    ok("v69 LSQE=0 psq_lx legal", s2.exception is None and s2.fpr[1] == [simg(1.5), simg(2.0)])

    # 70 MSR[FP]=0 -> FP unavailable, including moves and loads (PAGE 83, 175)
    st = new(msr=MSR_IR | MSR_DR); st.fpr[2] = [simg(1.0), simg(2.0)]
    run("fp0 mr", st, enc_x(MR, 1, 0, 2), expect_ub="applied")
    ok("v70 FP=0 ps_mr", st.exception == "fp_unavailable" and st.pc == 0x800 and st.srr1 == (MSR_IR | MSR_DR)
       and st.fpr[1] == [0, 0])

    # 71 PSE=0 and FP=0 -> program wins (Table 4-3 PAGE 162)
    st = new(msr=0, hid2=0)
    run("prio", st, enc_x(MR, 1, 0, 2), expect_ub="applied")
    ok("v71 priority program over FPU", st.exception == "program")

    # 72 dcbz_l: LCE=0 illegal; LCE=1 DCE=0 alignment; LCE=1 DCE=1 zero 32 bytes (applied UB)
    st = new(); st.gpr[3] = 0x80003000; st.gpr[4] = 0x25; st.mem.load(0x80003020, b"\xAA" * 40)
    s2 = st.clone(); run("dcbzl lce0", s2, (4 << 26) | (3 << 16) | (4 << 11) | (DCBZL << 1), expect_ub="applied")
    ok("v72 dcbz_l LCE=0", s2.exception == "program")
    s2 = st.clone(); s2.hid2 |= HID2_LCE
    run("dcbzl dce0", s2, (4 << 26) | (3 << 16) | (4 << 11) | (DCBZL << 1), expect_ub="applied")
    ok("v73 dcbz_l DCE=0 alignment", s2.exception == "alignment" and s2.pc == 0x600 and s2.dar == 0x80003025)
    s2 = st.clone(); s2.hid2 |= HID2_LCE; s2.hid0 = HID0_DCE
    ub = run("dcbzl", s2, (4 << 26) | (3 << 16) | (4 << 11) | (DCBZL << 1), expect_ub="applied")
    ok("v74 dcbz_l zero block", s2.mem._get(0x80003020, 32) == 0 and s2.mem._get(0x80003040, 8) == 0xAAAAAAAAAAAAAAAA
       and ub.undefined == {"hid2": HID2_DCHERR} and s2.pc == PC + 4)
    #    DCHERR already 1: a hit cannot change it, no machine check possible -> fully defined
    s2 = st.clone(); s2.hid2 |= HID2_LCE | HID2_DCHERR | HID2_DCHEE; s2.hid0 = HID0_DCE; s2.msr |= MSR_EE | MSR_ME
    run("dcbzl dcherr", s2, (4 << 26) | (3 << 16) | (4 << 11) | (DCBZL << 1))
    ok("v74b dcbz_l DCHERR=1", s2.mem._get(0x80003020, 32) == 0 and s2.exception is None)
    #    DCHEE = EE = ME = 1, DCHERR = 0: machine check possible (PAGE 311)
    s2 = st.clone(); s2.hid2 |= HID2_LCE | HID2_DCHEE; s2.hid0 = HID0_DCE; s2.msr |= MSR_EE | MSR_ME
    ub = run("dcbzl mc", s2, (4 << 26) | (3 << 16) | (4 << 11) | (DCBZL << 1), expect_ub="applied")
    ok("v74c dcbz_l machine check open", ub.undefined.get("pc") and ub.undefined.get("srr0") and s2.mem._get(0x80003020, 32) == 0)

    # 75 type-0 misaligned EA -> alignment, DAR = EA; type 4 misaligned -> no exception (PAGE 175)
    st = new(); st.gpr[3] = 0x80001001; st.gqr[1] = 4 << 16; st.mem.load(0x80001001, b"\x05\x06")
    s2 = st.clone(); run("align t0", s2, enc_q(56, 1, 3, 0, 0, 0), expect_ub="applied")  # DSISR, SRR1[1-4,10-15]
    ok("v75 alignment type 0", s2.exception == "alignment" and s2.dar == 0x80001001 and s2.mem.log == []
       and s2.srr0 == PC)
    s2 = st.clone(); run("align t4", s2, enc_q(56, 1, 3, 0, 1, 0))
    ok("v76 no alignment type 4", s2.exception is None and s2.fpr[1] == [simg(5.0), simg(6.0)])

    # 77 reserved bits set -> boundedly undefined (PAGE 87)
    st = new(); st.fpr[2] = [simg(1.0), simg(1.0)]
    run("reserved", st, enc_a(ADD, 1, 2, 2, 1), expect_ub="total")
    ok("v77 reserved field", True)

    # 78 unused opcode-4 extended opcode -> illegal (PAGE 88)
    st = new()
    run("unused", st, (4 << 26) | (5 << 1), expect_ub="applied")
    ok("v78 unused primary-4", st.exception == "program" and st.pc == 0x700)

    # 79 operand that is not a single (double 0.1) -> total UB (PAGE 83)
    st = new(); st.fpr[2] = [double_to_bits(0.1), simg(1.0)]
    run("double opnd", st, enc_a(ADD, 1, 2, 2, 0), expect_ub="total")
    ok("v79 non-single operand", True)

    # 80 exact denormal result: no UX; inexact tiny: UX|XX|FR|FI (E6)
    st = new(); st.fpr[2] = [simg(2.0 ** -100), simg(2.0 ** -100)]; st.fpr[4] = [simg(2.0 ** -49), simg(1.5 * 2.0 ** -50)]
    run("tiny", st, enc_a(MUL, 1, 2, 0, 4))
    ok("v80 underflow", st.fpr[1] == [simg(2.0 ** -149), simg(2.0 ** -149)] and st.fpscr == 0x8A074000)

    # 81 NI=1 with denormal operands: "Zero A and B" (Table 2-21 PAGE 84), zero signs not given -> the sign of
    #    the +-0 sum and UX/XX/FR/FI are open; the other half (1 + 1) is exact
    st = new(fpscr=NI); st.fpr[2] = [simg(2.0 ** -140), simg(1.0)]
    ub = run("ni", st, enc_a(ADD, 1, 2, 2, 0), expect_ub="applied")
    ok("v81 NI denormal", um(ub, "fpr[1][0]") == IMG_SIGN and (st.fpr[1][0] & ~IMG_SIGN) == 0
       and st.fpr[1][1] == simg(2.0) and um(ub, "fpscr") == UX | XX | FR | FI | 0x10000 | FX)

    # 82 SNaN propagation: quiet bit set, VXSNAN; first NaN in field order (A before B)
    st = new(); st.fpr[2] = [simg(1.0), NQNAN]; st.fpr[3] = [SNAN, SNAN]
    ub = run("snan", st, enc_a(ADD, 1, 2, 3, 0), expect_ub="applied")
    # ps0: SNaN quieted, payload conversion not given; ps1: QNaN vs SNaN priority is PEM -> sign/payload open
    ok("v82 NaN propagation", um(ub, "fpr[1][0]") == IMG_PAYLOAD and (st.fpr[1][0] & ~IMG_PAYLOAD) == 0x7FF8000000000000
       and um(ub, "fpr[1][1]") == IMG_SIGN | IMG_PAYLOAD and st.fpscr == 0xA1011000)

    # 83 VE=1 with MSR[FE0]=1 -> enabled program exception not modelled, applied UB
    st = new(fpscr=VE, msr=MSR_FP | MSR_FE0); st.fpr[2] = [SNAN, simg(1.0)]
    ub = run("fe", st, enc_a(ADD, 1, 2, 2, 0), expect_ub="applied")
    ok("v83 enabled exception", ub.undefined.get("pc") and um(ub, "fpscr") & (FX | FEX | VX | VXSNAN) == (FX | FEX | VX | VXSNAN))

    # 84 MSR[LE]=1 psq -> total UB (PAGE 110)
    st = new(msr=MSR_FP | MSR_LE); st.gpr[3] = 0x80001000
    run("le", st, enc_q(56, 1, 3, 0, 0, 0), expect_ub="total")
    ok("v84 LE psq", True)

    # 85 reserved GQR type -> total UB (Table 2-17 PAGE 78)
    st = new(); st.gpr[3] = 0x80001000; st.gqr[0] = 2 << 16
    run("gqr t2", st, enc_q(56, 1, 3, 0, 0, 0), expect_ub="total")
    ok("v85 GQR type 2", True)

    # 86 nearest-even tie on a ps_add: 1 + 2^-24 is a tie -> 1.0 (even), FR=0 FI=1
    st = new(); st.fpr[2] = [simg(1.0), simg(1.0 + 2.0 ** -23)]; st.fpr[3] = [simg(2.0 ** -24), simg(2.0 ** -24)]
    run("tie", st, enc_a(ADD, 1, 2, 3, 0))
    ok("v86 ties-to-even", st.fpr[1] == [simg(1.0), simg(1.0 + 2.0 ** -22)] and st.fpscr == (FX | XX | FR | FI | FPRF_PNORM))

    # 87 gqr field decode (PAGE 78)
    g = gqr_fields(0x3D070105)
    ok("v87 GQR decode", g == {"ld_scale": -3, "ld_type": 7, "st_scale": 1, "st_type": 5})

    # 88 mnemonic decode sanity against the hex opcodes on the chapter-12 pages
    for w, m in [(0x1000002A, "ps_add"), (0x10000040, "ps_cmpo0"), (0x100004A0, "ps_merge10"), (0x1000004C, "psq_lux"),
                 (0x1000000E, "psq_stx"), (0x100007EC, "dcbz_l"), (0x1000003E, "ps_nmadd"), (0x10000210, "ps_abs"),
                 (0xE0000000, "psq_l"), (0xF4000000, "psq_stu"), (0x10000016, "ps_sum1"), (0x10000034, "ps_rsqrte")]:
        ok("v88 decode %08X" % w, mnemonic(w) == m)

    print("ps_B self-test: %d vectors passed" % passed[0])
