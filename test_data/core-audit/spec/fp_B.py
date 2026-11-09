"""Independent Gekko specification model, area "fp" (scalar floating point), author B.

SOURCES (the only ones used):
  G = docs/reference/ppc/gekko_user_manual.txt  (cited "PAGE N" = its ===== PAGE N ===== marker)
  C = docs/reference/ppc/ppc_750cl.txt          (cited "750CL PAGE N")
  X = EXTERNAL: PowerPC Programming Environments Manual (PEM) sections that G explicitly defers to
      (Ch.3 execution model, D.4.1 frsp model, D.4.2 fctiw model, D.6 load model, D.7 store model,
      Ch.6 SRR1 program-exception bits) and IEEE 754-1985.  Not in the corpus; every rule taken from
      it is tagged "EXTERNAL" so a reviewer can tell corpus facts from assumptions.
No GekkoForge implementation, test, audit document, or other author's spec was consulted.
Host float arithmetic is never used to compute a result; all rounding is exact (fractions.Fraction
and integers).  Host floats appear only in the self-test cross-check.

COVERED (primary/extended opcode)
  48 lfs 49 lfsu 50 lfd 51 lfdu 52 stfs 53 stfsu 54 stfd 55 stfdu
  31/535 lfsx 567 lfsux 599 lfdx 631 lfdux 663 stfsx 695 stfsux 727 stfdx 759 stfdux
  59/A: 18 fdivs 20 fsubs 21 fadds 22 fsqrts(ILLEGAL) 24 fres 25 fmuls 28 fmsubs 29 fmadds
        30 fnmsubs 31 fnmadds
  63/A: 18 fdiv 20 fsub 21 fadd 22 fsqrt(ILLEGAL) 23 fsel 25 fmul 26 frsqrte 28 fmsub 29 fmadd
        30 fnmsub 31 fnmadd
  63/X: 0 fcmpu 12 frsp 14 fctiw 15 fctiwz 32 fcmpo 38 mtfsb1 40 fneg 64 mcrfs 70 mtfsb0 72 fmr
        134 mtfsfi 136 fnabs 264 fabs 583 mffs 711 mtfsf

UNDEFINED-BEHAVIOUR CONTRACT (UndefinedBehaviour exception)
  applied == False : the instruction's outcome is (boundedly) undefined as a whole; state NOT modified.
  applied == True  : state holds one representative outcome with every DEFINED effect applied.  The
                     following name the bits that must not be compared:
      fpr_masks  {(reg, slot): u64 mask}   slot 0 = ps0, 1 = ps1
      fpscr_mask u32, cr_mask u32
      mem_masks  [(address, width, mask)]
      other      [names of whole registers that are undefined, e.g. 'dsisr']
      bounds     [free-text constraints that still hold for masked values, e.g. fres precision]
  Ambiguities are resolved by ENUMERATION: every point where the manual admits two readings is a
  named binary choice; the instruction is evaluated under all combinations and only the bits that
  actually differ are reported undefined.  If the alternatives disagree on control flow (exception
  taken or not, GPR/EA/memory addresses) the result is applied == False.

FPSCR LAYOUT (big-endian bit n = 1 << (31-n)).  Corpus facts: NI = bit 29 (PAGE 82), result bits
  15-19 and exception bits 0-14/22-23 (PAGE 83), fields and FX/OX/FEX/VX roles (PAGE 473/474),
  mcrfs field contents (PAGE 457).  Remaining positions are the architectural layout: EXTERNAL.
  0 FX 1 FEX 2 VX 3 OX 4 UX 5 ZX 6 XX 7 VXSNAN 8 VXISI 9 VXIDI 10 VXZDZ 11 VXIMZ 12 VXVC 13 FR 14 FI
  15-19 FPRF(C,FL,FG,FE,FU) 20 reserved 21 VXSOFT 22 VXSQRT 23 VXCVI 24 VE 25 OE 26 UE 27 ZE 28 XE
  29 NI 30-31 RN (00 nearest, 01 zero, 10 +inf, 11 -inf: EXTERNAL)
"""
from __future__ import annotations

import itertools
import math
from fractions import Fraction

from gekko_state import MASK32, MASK64, State  # noqa: F401

AREA = "fp"


class UndefinedBehaviour(Exception):
    def __init__(self, what, applied=False, fpr_masks=None, fpscr_mask=0, cr_mask=0,
                 mem_masks=None, other=None, bounds=None, reg_masks=None):
        super().__init__(what)
        self.what = what
        self.applied = applied
        self.fpr_masks = dict(fpr_masks or {})
        self.fpscr_mask = fpscr_mask
        self.cr_mask = cr_mask
        self.mem_masks = list(mem_masks or [])
        self.other = list(other or [])
        self.bounds = list(bounds or [])
        # CROSSCHECK FIX (crosscheck_fp.py): partially undefined 32-bit registers {name: mask}, used for
        # exception entry (MSR reserved bits, SRR1 exception-specific bits).
        self.reg_masks = dict(reg_masks or {})


def _b(n):
    return 1 << (31 - n)


# ---- FPSCR bits (see module docstring for provenance) ------------------------------------------
FX, FEX, VX, OX, UX, ZX, XX = _b(0), _b(1), _b(2), _b(3), _b(4), _b(5), _b(6)
VXSNAN, VXISI, VXIDI, VXZDZ, VXIMZ, VXVC = _b(7), _b(8), _b(9), _b(10), _b(11), _b(12)
FR, FI = _b(13), _b(14)
FPRF_MASK = 0x1F << 12          # bits 15-19 (PAGE 83 "FPSCR bits 15-19 are the result bits")
FPCC_MASK = 0x0F << 12          # bits 16-19
VXSOFT, VXSQRT, VXCVI = _b(21), _b(22), _b(23)
VE, OE, UE, ZE, XE, NI = _b(24), _b(25), _b(26), _b(27), _b(28), _b(29)   # NI: PAGE 82 "FPSCR[29]"
VX_ALL = VXSNAN | VXISI | VXIDI | VXZDZ | VXIMZ | VXVC | VXSOFT | VXSQRT | VXCVI
EXC_ALL = OX | UX | ZX | XX | VX_ALL
BIT = {"OX": OX, "UX": UX, "ZX": ZX, "XX": XX, "VXSNAN": VXSNAN, "VXISI": VXISI, "VXIDI": VXIDI,
       "VXZDZ": VXZDZ, "VXIMZ": VXIMZ, "VXVC": VXVC, "VXSOFT": VXSOFT, "VXSQRT": VXSQRT,
       "VXCVI": VXCVI}
ENABLE = {"OX": OE, "UX": UE, "ZX": ZE, "XX": XE}
for _k in ("VXSNAN", "VXISI", "VXIDI", "VXZDZ", "VXIMZ", "VXVC", "VXSOFT", "VXSQRT", "VXCVI"):
    ENABLE[_k] = VE

# ---- MSR bits (PAGE 164/165 Table 4-4) ---------------------------------------------------------
MSR_POW, MSR_ILE, MSR_EE, MSR_PR, MSR_FP = _b(13), _b(15), _b(16), _b(17), _b(18)
MSR_FE0, MSR_SE, MSR_BE, MSR_FE1, MSR_IP = _b(20), _b(21), _b(22), _b(23), _b(25)
MSR_IR, MSR_DR, MSR_PM, MSR_RI, MSR_LE = _b(26), _b(27), _b(29), _b(30), _b(31)
MSR_ME = _b(19)
MSR_NAMED = (MSR_POW | MSR_ILE | MSR_EE | MSR_PR | MSR_FP | MSR_ME | MSR_FE0 | MSR_SE | MSR_BE | MSR_FE1
             | MSR_IP | MSR_IR | MSR_DR | MSR_PM | MSR_RI | MSR_LE)
# CROSSCHECK FIX: exception entry.  PAGE 168 Table 4-6 lists only the named MSR bits ("Reserved bits are read
# as if written as 0") -> the new value of a reserved MSR bit is undefined.  PAGE 166 step 2 "SRR1[1-4, 10-15]
# are loaded with information specific to the exception type" and PAGE 175 "Chapter 6 ... PEM describes register
# settings for this exception" (PEM not in the corpus) -> SRR1[1-4,10-15] undefined.  PAGE 166 step 3 "SRR1[5-9,
# 16-31] are loaded with a copy of the corresponding MSR bits. Depending on the implementation, reserved bits may
# not be copied" + PAGE 164 note "Full function reserved bits are saved in SRR1" (bits 0, 5-9, 28 are full
# function) -> bit 24 (reserved, function class unstated) is undefined; bit 0 is copied.
SRR1_UNDEF = 0x78000000 | 0x003F0000 | _b(24)
HID2_PSE = _b(2)                 # PAGE 67 Table 2-6 "2 PSE Paired single enable"

SRR1_FP_ENABLED = _b(11)         # EXTERNAL (PEM ch.6 program exception SRR1[11]); masked undefined, see SRR1_UNDEF
SRR1_ILLEGAL = _b(12)            # EXTERNAL (PEM ch.6 program exception SRR1[12]); masked undefined, see SRR1_UNDEF

QNAN_DEFAULT = 0x7FF8000000000000   # EXTERNAL (PEM ch.3 generated QNaN); sign 0 agrees with PAGE 412
SIGN64 = 1 << 63
QUIET64 = 1 << 51                    # double bit 12 (EXTERNAL: quiet/signalling discriminator)
FRAC52 = (1 << 52) - 1


def _pow2(k):
    return Fraction(1 << k) if k >= 0 else Fraction(1, 1 << -k)


def _summary(f):
    """Recompute FEX and VX by 'the usual rule' (PAGE 473 'Bits 1 and 2 (FEX and VX) are set
    according to the usual rule'); the rule itself (VX = OR of VX*, FEX = OR of enabled
    exceptions) is EXTERNAL."""
    f &= ~(VX | FEX)
    if f & VX_ALL:
        f |= VX
    if ((f & VX) and (f & VE)) or ((f & OX) and (f & OE)) or ((f & UX) and (f & UE)) or \
            ((f & ZX) and (f & ZE)) or ((f & XX) and (f & XE)):
        f |= FEX
    return f


# =================================================================================================
# Value classification
# =================================================================================================
class V:
    """Decoded 64-bit FPR image.  kind in {'zero','fin','inf','nan'}; mag is the exact magnitude."""
    __slots__ = ("kind", "sign", "mag", "bits", "snan", "dbl_denorm")

    def __init__(self, bits):
        bits &= MASK64
        self.bits = bits
        self.sign = bits >> 63
        e = (bits >> 52) & 0x7FF
        f = bits & FRAC52
        self.snan = False
        self.dbl_denorm = False
        self.mag = Fraction(0)
        if e == 0x7FF:
            self.kind = "nan" if f else "inf"
            self.snan = bool(f) and not (bits & QUIET64)
        elif e == 0:
            if f == 0:
                self.kind = "zero"
            else:
                self.kind = "fin"
                self.dbl_denorm = True
                self.mag = Fraction(f) * _pow2(-1074)
        else:
            self.kind = "fin"
            self.mag = Fraction((1 << 52) | f) * _pow2(e - 1075)

    @staticmethod
    def zero(sign):
        return V(sign << 63)

    def signed(self):
        return -self.mag if self.sign else self.mag


def _ilog2(x):
    """floor(log2(x)) for a positive Fraction, exactly."""
    n, d = x.numerator, x.denominator
    e = n.bit_length() - d.bit_length()
    if e >= 0:
        if n < (d << e):
            e -= 1
    elif (n << -e) < d:
        e -= 1
    return e


# =================================================================================================
# D.6 / D.7 bit-exact conversions  (PAGE 434 lfs "DOUBLE(MEM(EA, 4))", PAGE 436 "see Section D.6";
# PAGE 551 stfs "SINGLE(frS)" "see Section D.7"; PAGE 108 Table 2-41)
# =================================================================================================
def single_to_double(w):
    """D.6 (EXTERNAL algorithm, referenced by PAGE 436)."""
    w &= MASK32
    e = (w >> 23) & 0xFF
    if 0 < e < 255 or e == 255 or (w & 0x7FFFFFFF) == 0:
        b1 = (w >> 30) & 1
        if 0 < e < 255:
            ext = 0 if b1 else 7          # frD[2-4] <- NOT WORD[1]
        else:
            ext = 7 if b1 else 0          # frD[2-4] <- WORD[1] (zero, infinity, NaN)
        return ((w >> 30) << 62) | (ext << 59) | ((w & 0x3FFFFFFF) << 29)
    # denormalized single: normalize (D.6 "Denormalized operand")
    exp = -126
    frac = (w & 0x7FFFFF) << 29           # 53-bit field, frac[0] (bit 52) = 0
    while not (frac >> 52) & 1:
        frac <<= 1
        exp -= 1
    return ((w >> 31) << 63) | ((exp + 1023) << 52) | (frac & FRAC52)


def double_to_single(bits):
    """D.7 plus Gekko Table 2-41 (PAGE 108).  Returns (word, undefined_mask, note)."""
    bits &= MASK64
    s = bits >> 63
    e = (bits >> 52) & 0x7FF
    f = bits & FRAC52
    if e > 896 or (bits & ~SIGN64) == 0:
        # D.7 "no denormalization": WORD[0-1] <- frB[0-1]; WORD[2-31] <- frB[5-34]  (EXTERNAL);
        # Table 2-41 PAGE 108 "Double Normalized ... else Store", "Zero, infinity, QNaN Store",
        # "SNaN Store".  Out-of-range values are bit-dropped, not saturated.
        return ((bits >> 62) << 30) | ((bits >> 29) & 0x3FFFFFFF), 0, None
    if 874 <= e <= 896:
        # PAGE 108 "If(exp <= 896) then Denormalize and Store"; D.7 loop shifts 897-exp places,
        # truncating (no rounding).  PAGE 109 "Anywhere from 1 to 23 clock cycles".
        frac = ((1 << 52) | f) >> (897 - e)
        return (s << 31) | ((frac >> 29) & 0x7FFFFF), 0, None
    if e == 0:
        # PAGE 108 Table 2-41 "Double Denormalized Store zero": magnitude defined, sign unstated.
        return s << 31, 0x80000000, "stfs of a double denormal: sign of the stored zero PAGE 108"
    # 1 <= exp <= 873: D.7 says WORD undefined (EXTERNAL); PAGE 109 bounds the Gekko denormalizer
    # to 23 shifts.  The sign path (Table 2-41 'Denormalize') is kept, the magnitude is undefined.
    return s << 31, 0x7FFFFFFF, "stfs of a double with exponent < 874 (beyond 23-bit denormalization) PAGE 109"


def is_single_value(bits):
    """True when the double image holds a value representable in single precision, i.e. one that
    D.6 could have produced (PAGE 82 'Single-precision arithmetic instructions require all
    operands to be single-precision')."""
    bits &= MASK64
    v = V(bits)
    if v.kind in ("zero", "inf"):
        return True
    if v.kind == "nan":
        return (bits & ((1 << 29) - 1)) == 0
    return single_to_double(double_to_single(bits)[0]) == bits and double_to_single(bits)[1] == 0


# =================================================================================================
# Exact IEEE rounding
# =================================================================================================
FMT = {"d": (53, -1022, 1023, 1536), "s": (24, -126, 127, 192)}   # p, emin, emax, trap bias


def _round_mag(sign, x, p, emin, rn):
    """Round positive Fraction x to p bits with quantum clamped at emin-(p-1) (gradual underflow).
    Returns (m, q, incremented, inexact) with value m*2^q.  Rounding modes: PAGE 395 'rounded ...
    under control of the floating-point rounding control field RN'; encodings EXTERNAL."""
    e = _ilog2(x)
    q = max(e, emin) - (p - 1)
    sc = x * _pow2(-q)
    n = sc.numerator // sc.denominator
    rem = sc - n
    if rn == 0:
        inc = rem > Fraction(1, 2) or (rem == Fraction(1, 2) and (n & 1) == 1)
    elif rn == 1:
        inc = False
    elif rn == 2:
        inc = rem != 0 and sign == 0
    else:
        inc = rem != 0 and sign == 1
    m = n + (1 if inc else 0)
    if m == (1 << p):
        m >>= 1
        q += 1
    return m, q, bool(inc), rem != 0


def _encode(sign, m, q, fmt):
    """Encode value (-1)^sign * m * 2^q (already rounded) as a 64-bit FPR image."""
    if fmt == "d":
        if m == 0:
            return sign << 63
        if m >= (1 << 52):
            return (sign << 63) | ((q + 52 + 1023) << 52) | (m - (1 << 52))
        assert q == -1074
        return (sign << 63) | m
    if m == 0:
        w = sign << 31
    elif m >= (1 << 23):
        w = (sign << 31) | ((q + 23 + 127) << 23) | (m - (1 << 23))
    else:
        assert q == -149
        w = (sign << 31) | m
    return single_to_double(w)


def _max_finite(sign, fmt):
    p, _emin, emax, _ = FMT[fmt]
    return _encode(sign, (1 << p) - 1, emax - (p - 1), fmt)


def _inf(sign):
    return (sign << 63) | (0x7FF << 52)


def _fprf(kind, sign):
    """FPRF class codes (EXTERNAL table; PAGE 395 'FPRF is set to the class and sign')."""
    if kind == "nan":
        return 0x11
    return {"inf": (0x05, 0x09), "normal": (0x04, 0x08), "denorm": (0x14, 0x18),
            "zero": (0x02, 0x12)}[kind][sign]


class _Res:
    """Result of rounding one exact value.  mask = defined bits of `bits` (MASK64 unless undefined);
    frfi_undef: FR/FI undefined."""
    __slots__ = ("bits", "raised", "fr", "fi", "kind", "sign", "mask", "frfi_undef")

    def __init__(self, bits, raised, fr, fi, kind, sign, mask=MASK64, frfi_undef=False):
        self.bits, self.raised, self.fr, self.fi, self.kind, self.sign = bits, raised, fr, fi, kind, sign
        self.mask, self.frfi_undef = mask, frfi_undef


def _adj_out_of_range(ctx, sign, raised):
    """CROSSCHECK FIX: an enabled overflow/underflow whose exponent-adjusted result is still out of range is
    not described (PAGE 82 mentions only 'computation of the adjusted exponent value'): the value except its
    sign, FR, FI, FPRF and whether XX is also signalled are undefined; the enabled exception itself is set."""
    raised = set(raised)
    if ctx.choose("adjusted_result_out_of_range_sets_XX"):
        raised.add("XX")
    return _Res(sign << 63, raised, False, False, "normal", sign, mask=SIGN64, frfi_undef=True)


def round_result(ctx, fpscr, sign, x, fmt):
    """Round exact nonzero magnitude x with sign into format fmt under FPSCR fpscr.
    Execution model of PAGE 82 ('deliver results that approximate the values required by the IEEE
    standard' for NI) and the PEM ch.3 overflow/underflow/inexact rules (EXTERNAL)."""
    p, emin, emax, adj = FMT[fmt]
    rn = fpscr & 3
    tiny_before = x < _pow2(emin)   # 750CL PAGE 82 Table 2-25 note 1: "detection of a denorm result
    #                                  occurs prior to rounding" -- that note is attached to the NI column only.
    if tiny_before and not (fpscr & NI):
        # CROSSCHECK FIX: IEEE mode (NI=0) tininess detection is not stated in the corpus (the 750CL note is on
        # the NI column; PAGE 82 defers the execution model to PEM ch.3 and cites IEEE 754-1985, whose s7.4
        # permits detection before or after rounding) -> enumerated.  After rounding = the value rounded to p
        # bits with an unbounded exponent is below 2^emin.
        eu = _ilog2(x)
        mu, qu, _i, _x = _round_mag(sign, x * _pow2(-(eu - (p - 1))), p, -10 ** 9, rn)
        after = Fraction(mu) * _pow2(qu + eu - (p - 1)) < _pow2(emin)
        tiny = True if after else not ctx.choose("ieee_tininess_detected_after_rounding")
    else:
        tiny = tiny_before
    ue_path = bool(fpscr & UE)
    if tiny_before and (fpscr & NI):
        # PAGE 85 Table 2-22 NI column "Denormalized ... Return zero."; 750CL PAGE 82 note 1
        # "A result that is a denorm before it is rounded is returned as zero."
        # Unstated: sign of that zero, UX/XX/FR/FI, interplay with UE=1 -> enumerated choices.
        if not (ue_path and ctx.choose("ni_tiny_with_UE_takes_trap_path")):
            zs = sign ^ int(ctx.choose("ni_tiny_zero_sign_flipped"))
            raised = set()
            if ctx.choose("ni_tiny_sets_UX"):
                raised.add("UX")
            if ctx.choose("ni_tiny_sets_XX"):
                raised.add("XX")
            return _Res(zs << 63, raised, ctx.choose("ni_tiny_FR"), ctx.choose("ni_tiny_FI"), "zero", zs)
    if tiny and ue_path:
        # Enabled underflow: exponent adjusted by +1536 (double) / +192 (single). PAGE 82 "computation
        # of the adjusted exponent value ... when the corresponding exception enable bit is one";
        # bias values EXTERNAL.
        y = x * _pow2(adj)
        if y < _pow2(emin):
            return _adj_out_of_range(ctx, sign, {"UX"})
        m, q, inc, inex = _round_mag(sign, y, p, emin, rn)
        if q + p - 1 > emax:
            return _adj_out_of_range(ctx, sign, {"UX"})
        raised = {"UX"} | ({"XX"} if inex else set())
        return _Res(_encode(sign, m, q, fmt), raised, inc, inex, "normal", sign)
    m, q, inc, inex = _round_mag(sign, x, p, emin, rn)
    if m >= (1 << (p - 1)) and q + p - 1 > emax:
        if fpscr & OE:
            # Enabled overflow: exponent adjusted by -1536/-192 (PAGE 82; bias EXTERNAL)
            q2 = q - adj
            if q2 + p - 1 > emax:
                return _adj_out_of_range(ctx, sign, {"OX"})
            raised = {"OX"} | ({"XX"} if inex else set())
            return _Res(_encode(sign, m, q2, fmt), raised, inc, inex, "normal", sign)
        # Disabled overflow (EXTERNAL PEM): OX, XX, FI <- 1, FR undefined; value chosen by RN.
        to_inf = rn == 0 or (rn == 2 and sign == 0) or (rn == 3 and sign == 1)
        bits = _inf(sign) if to_inf else _max_finite(sign, fmt)
        # FR after a disabled overflow is not stated (PAGE 395 lists FR as affected; PAGE 399 defines it only as
        # 'incremented when rounded') -> enumerated
        return _Res(bits, {"OX", "XX"}, ctx.choose("overflow_FR_undefined_PAGE_395"), True,
                    "inf" if to_inf else "normal", sign)
    raised = set()
    if inex:
        raised.add("XX")
    if tiny:
        # disabled underflow = tiny AND loss of accuracy.  CROSSCHECK FIX: IEEE 754-1985 s7.4 (cited by PAGE 82)
        # lets loss of accuracy be either an inexact result or a denormalization loss (result differs from the
        # unbounded-exponent rounding); the corpus does not choose -> enumerated where the two differ.
        eu = _ilog2(x)
        mu, qu, _i, _x = _round_mag(sign, x * _pow2(-(eu - (p - 1))), p, -10 ** 9, rn)
        denorm_loss = Fraction(m) * _pow2(q) != Fraction(mu) * _pow2(qu + eu - (p - 1))
        loss = inex if inex == denorm_loss else ctx.choose("underflow_loss_is_denormalization_loss") == denorm_loss
        if loss:
            raised.add("UX")
    if m == 0:
        kind = "zero"
    elif m < (1 << (p - 1)):
        kind = "denorm"          # PAGE 85 Table 2-22 IEEE column "Return ... denormalized number"
    else:
        kind = "normal"
    return _Res(_encode(sign, m, q, fmt), raised, inc, inex, kind, sign)


# =================================================================================================
# Execution framework: pure evaluation -> outcome, enumeration of manual ambiguities, commit
# =================================================================================================
class _Ctx:
    def __init__(self, choices):
        self.choices = choices
        self.used = []

    def choose(self, key):
        if key not in self.used:
            self.used.append(key)
        return bool(self.choices.get(key, False))


class _Out:
    def __init__(self, st):
        self.fpr = {}            # reg -> [ps0 or None, ps1 or None]  (None = unchanged)
        self.fpr_mask = {}       # (reg, slot) -> mask
        self.fpscr = st.fpscr
        self.fpscr_mask = 0
        self.cr = st.cr
        self.cr_mask = 0
        self.gpr = {}
        self.reads = []          # (addr, width)
        self.writes = []         # (addr, width, value)
        self.mem_mask = []       # (addr, width, mask)
        self.exc = None          # (name, vector, srr1_bits, dar)
        self.other = []
        self.bounds = []
        self.notes = []

    def set_fpr(self, reg, ps0=None, ps1=None):
        cur = self.fpr.setdefault(reg, [None, None])
        if ps0 is not None:
            cur[0] = ps0 & MASK64
        if ps1 is not None:
            cur[1] = ps1 & MASK64

    def mask_fpr(self, reg, slot, mask, note):
        self.fpr_mask[(reg, slot)] = self.fpr_mask.get((reg, slot), 0) | mask
        self.note(note)

    def note(self, note):
        if note not in self.notes:
            self.notes.append(note)


def _run(st, word, choices):
    ctx = _Ctx(choices)
    out = _Out(st)
    _execute(ctx, st, out, word)
    return out, ctx.used


def _resolved(st, out, reg, slot):
    v = out.fpr.get(reg, [None, None])[slot]
    return st.fpr[reg][slot] if v is None else v


def step(state, word):
    """Execute the instruction `word` located at state.pc."""
    word &= MASK32
    if not supports(word):
        raise ValueError("fp_B: unsupported instruction 0x%08X" % word)
    # CROSSCHECK FIX: the state must not be modified when UndefinedBehaviour(applied=False) is raised; the
    # exception field is now cleared in _commit (was cleared here, before evaluation).
    base, used = _run(state, word, {})
    keys = list(used)
    outs = {(): base}
    while True:
        if len(keys) > 12:
            raise UndefinedBehaviour("too many interacting ambiguities", applied=False)
        grew = False
        for combo in itertools.product((False, True), repeat=len(keys)):
            tag = tuple(k for k, v in zip(keys, combo) if v)
            if tag in outs:
                continue
            o, used = _run(state, word, dict.fromkeys(tag, True))
            outs[tag] = o
            for k in used:
                if k not in keys:
                    keys.append(k)
                    grew = True
        if not grew:
            break
    rep = base
    fpr_mask = dict(rep.fpr_mask)
    fpscr_mask, cr_mask = rep.fpscr_mask, rep.cr_mask
    mem_mask = list(rep.mem_mask)
    notes = list(rep.notes)
    for o in outs.values():
        if o is rep:
            continue
        for n in o.notes:
            if n not in notes:
                notes.append(n)
        if (o.exc is None) != (rep.exc is None) or (o.exc and o.exc[:3] != rep.exc[:3]) or \
                o.gpr != rep.gpr or [w[:2] for w in o.writes] != [w[:2] for w in rep.writes] or \
                o.reads != rep.reads:
            raise UndefinedBehaviour("readings of the manual disagree on control flow/exception (PAGE 82-85, 161-166): "
                                     + "; ".join(notes + ["choices: " + ", ".join(keys)]),
                                     applied=False)
        for reg in set(o.fpr) | set(rep.fpr):
            for slot in (0, 1):
                d = (_resolved(state, o, reg, slot) ^ _resolved(state, rep, reg, slot)) \
                    | o.fpr_mask.get((reg, slot), 0)
                if d:
                    fpr_mask[(reg, slot)] = fpr_mask.get((reg, slot), 0) | d
        fpscr_mask |= (o.fpscr ^ rep.fpscr) | o.fpscr_mask
        cr_mask |= (o.cr ^ rep.cr) | o.cr_mask
        for (a1, w1, v1), (_a2, _w2, v2) in zip(o.writes, rep.writes):
            if v1 != v2:
                mem_mask.append((a1, w1, v1 ^ v2))
        mem_mask.extend(o.mem_mask)
    if len(outs) > 1:
        notes.append("enumerated choices: " + ", ".join(keys) + " (PAGE 82-85 execution model points)")
    reg_masks = {}
    if rep.exc is not None:
        reg_masks = {"msr": ~MSR_NAMED & MASK32, "srr1": SRR1_UNDEF}
        notes.append("exception entry: reserved MSR bits PAGE 168, SRR1[1-4,10-15,24] PAGE 164/166 undefined")
    _commit(state, rep)
    if fpr_mask or fpscr_mask or cr_mask or mem_mask or rep.other or rep.bounds or reg_masks:
        raise UndefinedBehaviour("; ".join(notes) or "undefined bits PAGE 82-85", applied=True,
                                 fpr_masks=fpr_mask, fpscr_mask=fpscr_mask, cr_mask=cr_mask,
                                 mem_masks=mem_mask, other=rep.other, bounds=rep.bounds,
                                 reg_masks=reg_masks)


def _commit(st, out):
    for addr, width in out.reads:
        st.mem.read(addr, width)
    for addr, width, value in out.writes:
        st.mem.write(addr, width, value)
    for reg, val in out.gpr.items():
        st.gpr[reg] = val & MASK32
    for reg, (p0, p1) in out.fpr.items():
        if p0 is not None:
            st.fpr[reg][0] = p0
        if p1 is not None:
            st.fpr[reg][1] = p1
    st.fpscr = out.fpscr & MASK32
    st.cr = out.cr & MASK32
    if out.exc is None:
        st.pc = (st.pc + 4) & MASK32
        st.exception = None
    else:
        name, vector, srr1_bits, dar = out.exc
        _enter_exception(st, name, vector, srr1_bits, dar)


def _enter_exception(st, name, vector, srr1_bits, dar):
    """PAGE 166 4.3.2 steps 1-5; PAGE 168 Table 4-6 (POW EE PR FP FE0 SE BE FE1 IR DR PM RI
    cleared; ILE ME IP 'not altered'; LE <- ILE); PAGE 164 note 'Full function reserved bits are
    saved in SRR1' (bits 0, 5-9) + PAGE 166 'SRR1[5-9, 16-31] are loaded with a copy of the
    corresponding MSR bits'.  SRR0 = address of this instruction for every exception this area
    raises (fp-unavailable, alignment, illegal, and FP-enabled which PAGE 175 makes precise)."""
    old = st.msr
    st.srr0 = st.pc
    st.srr1 = (old & 0x87C0FFFF) | srr1_bits
    new = old & ~(MSR_POW | MSR_EE | MSR_PR | MSR_FP | MSR_FE0 | MSR_SE | MSR_BE | MSR_FE1 |
                  MSR_IR | MSR_DR | MSR_PM | MSR_RI | MSR_LE)
    if old & MSR_ILE:
        new |= MSR_LE
    st.msr = new & MASK32
    if dar is not None:
        st.dar = dar & MASK32
    st.pc = ((0xFFF00000 if old & MSR_IP else 0) | vector) & MASK32   # PAGE 166 step 5
    st.exception = name


# ---- FPSCR / CR delivery ------------------------------------------------------------------------
def _deliver_status(st, out, raised, fr, fi, fprf, rc, keep_fr_fi=False):
    """One computational instruction's status update: sticky exception bits; FX by the 0->1 rule
    (PAGE 473 'the usual rule that FX is set when an exception bit changes from 0 to 1'); FR/FI
    (PAGE 399 'FR is set if the result is incremented when rounded. FI is set if the result is
    inexact'); FPRF (PAGE 395); FEX/VX summary; CR1 (PAGE 83 'the CR1 field of the CR is set from
    FPSCR bits 0-3'); precise FP-enabled program exception (PAGE 166 'If either bit is set, all IEEE
    enabled floating-point exceptions are taken and cause a program exception')."""
    old = st.fpscr
    new = old
    for name in raised:
        new |= BIT[name]
    if (new & ~old) & EXC_ALL:
        new |= FX
    if not keep_fr_fi:
        new &= ~(FR | FI)
        if fr:
            new |= FR
        if fi:
            new |= FI
    if fprf is not None:
        new = (new & ~FPRF_MASK) | (fprf << 12)
    new = _summary(new)
    out.fpscr = new
    if rc:
        out.cr = (out.cr & ~0x0F000000) | ((new >> 4) & 0x0F000000)
    if (st.msr & (MSR_FE0 | MSR_FE1)) and any(new & ENABLE[n] for n in raised):
        out.exc = ("program", 0x700, SRR1_FP_ENABLED, None)


def _cr1_from_fpscr(out, rc):
    if rc:
        out.cr = (out.cr & ~0x0F000000) | ((out.fpscr >> 4) & 0x0F000000)
        out.cr_mask |= (out.fpscr_mask >> 4) & 0x0F000000


def _put(st, out, reg, bits, kind):
    """Write a result to frD.ps0 and decide ps1.
    'single': PSE=1 -> ps1 <- ps0 (PAGE 83 'The ps1 value in the destination register is duplicated
              from the ps0 result'; PAGE 434 lfs 'replicated in frD(ps1)').
    'keep1' : PSE=1 -> ps1 unchanged (fmr PAGE 405 'the content of frD[ps1] is unchanged'; lfd is
              implied by PAGE 84 'psq_l frX ... lfd frX ... is guaranteed to restore the state of
              floating-point register frX regardless of its format').
    'double': PSE=1 -> ps1 undefined; never stated for double-precision results (PAGE 83 'execute
              as in non-paired single mode'; frsp PAGE 418 and fsel PAGE 421 say 'undefined').
    PSE=0   : ps1 is not architected (PAGE 82 paired single exists 'when HID2[PSE] = 1') -> undefined."""
    bits &= MASK64
    pse = st.hid2 & HID2_PSE
    if pse and kind == "single":
        out.set_fpr(reg, bits, bits)
        return
    out.set_fpr(reg, bits, None)
    if pse and kind == "keep1":
        return
    if pse and kind == "frsp":
        out.mask_fpr(reg, 1, MASK64, "frsp: 'The value in frD(ps1) is undefined' PAGE 418")
    elif pse and kind == "fsel":
        out.mask_fpr(reg, 1, MASK64, "fsel: 'The content of frD[ps1] is undefined' (paired case; "
                                     "operand type is not architecturally visible) PAGE 421")
    elif pse:
        out.mask_fpr(reg, 1, MASK64, "ps1 after a double-precision result under HID2[PSE]=1 is unstated PAGE 83")
    else:
        out.mask_fpr(reg, 1, MASK64, "ps1 is not architected when HID2[PSE]=0 PAGE 82")


# =================================================================================================
# Operands, NaN propagation, exact arithmetic
# =================================================================================================
def _operand(ctx, st, out, bits, tag, single, ni_choice_key=None):
    """Fetch an operand image.  Single-precision instructions require single-precision operands
    (PAGE 82 'Single-precision arithmetic instructions require all operands to be single-precision';
    PAGE 83 'It is a programming error to apply double-precision instructions to paired single
    operands and vice versa') -> otherwise the whole result is undefined.
    NI=1 (PAGE 84 Table 2-21 'Non-IEEE Mode (NI = 1) ... Zero A'): a denormalized operand is replaced
    by zero; the sign of that zero is not stated -> enumerated.  For a double-precision instruction
    a value below 2^-126 that is exactly a single may internally be 'Single denormalized' (PAGE 109
    'the operand ... is a denormalized single-precision value. The value could be the result of a
    load floating-point single instruction, a single-precision arithmetic instruction, or a floating
    round to single-precision instruction'); that internal type is not visible -> enumerated.
    ni_choice_key: for instructions Table 2-21 does not clearly govern (compares, fsel) the
    zeroing itself is an enumerated choice."""
    bits &= MASK64
    if single and not is_single_value(bits):
        raise UndefinedBehaviour("single-precision instruction applied to an operand that is not a "
                                 "single-precision value PAGE 82", applied=False)
    v = V(bits)
    if v.kind != "fin" or not (st.fpscr & NI):
        return v
    small = v.mag < _pow2(-126)
    if single:
        denorm = small
    else:
        denorm = v.dbl_denorm or (small and is_single_value(bits) and
                                  ctx.choose("ni_%s_typed_single_denormalized" % tag))
    if denorm and ni_choice_key is not None:
        denorm = ctx.choose(ni_choice_key)
    if not denorm:
        return v
    out.note("NI=1 denormalized operand is zeroed, sign of the zero unstated PAGE 84 Table 2-21")
    return V.zero(v.sign ^ int(ctx.choose("ni_%s_zero_sign_flipped" % tag)))


class _R:
    """Exact (unrounded) outcome of a computation."""

    def __init__(self, kind, sign=0, mag=None, bits=None, raised=None):
        self.kind, self.sign, self.mag, self.bits = kind, sign, mag, bits
        self.raised = set(raised or ())
        # default QNaN produced by an invalid operation (not a propagated operand NaN)
        self.generated = kind == "nan" and bool(self.raised & {"VXISI", "VXIDI", "VXZDZ", "VXIMZ", "VXSQRT"})


def _nan_result(ctx, ops, sub_tag=None):
    """NaN operand propagation.  PAGE 85 Table 2-21 note 1 'Prioritize according to Chapter 3,
    Operand Conventions' -> EXTERNAL priority frA, then frB, then frC; an SNaN is quieted
    (Table 2-22 PAGE 85 'QNaN, SNaN Return QNaN'); any SNaN operand sets VXSNAN (PAGE 395 list)."""
    raised = {"VXSNAN"} if any(v.kind == "nan" and v.snan for _t, v in ops) else set()
    for tag, v in ops:
        if v.kind == "nan":
            bits = v.bits | QUIET64
            if tag == sub_tag and ctx.choose("fsub_frB_NaN_propagates_with_inverted_sign"):
                # PAGE 422 'the contents of frB participate in the operation with its sign bit (bit
                # 0) inverted' - whether that applies to a propagated NaN is not stated.
                bits ^= SIGN64
            return _R("nan", bits=bits, raised=raised)
    raise AssertionError


GENERATED_INVALID = ("VXISI", "VXIDI", "VXZDZ", "VXIMZ", "VXSQRT")


def _ni_generated_invalid(ctx, st, out, dst, raised, rc):
    """CROSSCHECK FIX: PAGE 82 'In this mode [NI=1], denormalized numbers, NaNs, and some IEEE invalid
    operations are treated in a non-IEEE conforming manner' without naming which -> for an invalid operation
    that would generate the default QNaN, whether the invalid bit is set and the frD result (both slots),
    FPRF, FR and FI are all undefined."""
    if ctx.choose("ni_invalid_operation_not_signalled"):
        raised = {n for n in raised if n not in GENERATED_INVALID}
    _deliver_status(st, out, raised, False, False, None, rc, keep_fr_fi=True)
    out.fpscr_mask |= FPRF_MASK | FR | FI
    out.mask_fpr(dst, 0, MASK64, "NI=1 invalid operation result undefined PAGE 82")
    out.mask_fpr(dst, 1, MASK64, "NI=1 invalid operation result undefined PAGE 82")


def _exact_sum(st, sa, ma, za, sb, mb, zb):
    """Exact signed sum.  Zero sign: equal-signed zeros keep their sign, otherwise +0 except
    round toward -infinity gives -0 (IEEE 754; EXTERNAL)."""
    s = (-ma if sa else ma) + (-mb if sb else mb)
    if s == 0:
        if za and zb and sa == sb:
            return _R("zero", sa)
        return _R("zero", 1 if (st.fpscr & 3) == 3 else 0)
    return _R("fin", 1 if s < 0 else 0, abs(s))


def _r_add(ctx, st, A, B, sub):
    if A.kind == "nan" or B.kind == "nan":
        return _nan_result(ctx, [("A", A), ("B", B)], "B" if sub else None)
    bs = B.sign ^ sub      # PAGE 422 fsub 'frB participate ... with its sign bit (bit 0) inverted'
    if A.kind == "inf" and B.kind == "inf":
        if A.sign != bs:
            return _R("nan", bits=QNAN_DEFAULT, raised={"VXISI"})     # PAGE 395 'VXISI'
        return _R("inf", A.sign)
    if A.kind == "inf":
        return _R("inf", A.sign)
    if B.kind == "inf":
        return _R("inf", bs)
    return _exact_sum(st, A.sign, A.mag, A.kind == "zero", bs, B.mag, B.kind == "zero")


def _r_mul(ctx, st, A, C):
    if A.kind == "nan" or C.kind == "nan":
        return _nan_result(ctx, [("A", A), ("C", C)])
    s = A.sign ^ C.sign
    if A.kind == "inf" or C.kind == "inf":
        if A.kind == "zero" or C.kind == "zero":
            return _R("nan", bits=QNAN_DEFAULT, raised={"VXIMZ"})     # PAGE 408 'VXIMZ'
        return _R("inf", s)
    if A.kind == "zero" or C.kind == "zero":
        return _R("zero", s)
    return _R("fin", s, A.mag * C.mag)


def _r_div(ctx, st, A, B):
    if A.kind == "nan" or B.kind == "nan":
        return _nan_result(ctx, [("A", A), ("B", B)])
    s = A.sign ^ B.sign
    if A.kind == "inf":
        if B.kind == "inf":
            return _R("nan", bits=QNAN_DEFAULT, raised={"VXIDI"})     # PAGE 401 'VXIDI'
        return _R("inf", s)
    if B.kind == "inf":
        return _R("zero", s)
    if B.kind == "zero":
        if A.kind == "zero":
            return _R("nan", bits=QNAN_DEFAULT, raised={"VXZDZ"})     # PAGE 401 'VXZDZ'
        return _R("inf", s, raised={"ZX"})                           # PAGE 401 'ZX'
    if A.kind == "zero":
        return _R("zero", s)
    return _R("fin", s, A.mag / B.mag)


def _r_madd(ctx, st, A, C, B, sub):
    """(frA*frC) +/- frB with no intermediate rounding (PAGE 97 'These instructions combine
    multiply and add operations without an intermediate rounding operation')."""
    imz = (A.kind == "inf" and C.kind == "zero") or (A.kind == "zero" and C.kind == "inf")
    if A.kind == "nan" or B.kind == "nan" or C.kind == "nan":
        # CROSSCHECK FIX: frB is subtracted in fmsub/fnmsub (PAGE 406, 414); whether a NaN propagated from frB
        # keeps or inverts its sign is as unstated as for fsub (PAGE 422) -> same enumerated choice.
        r = _nan_result(ctx, [("A", A), ("B", B), ("C", C)], "B" if sub else None)
        if imz and ctx.choose("fmadd_inf_times_zero_with_NaN_operand_sets_VXIMZ"):
            r.raised.add("VXIMZ")   # not stated whether VXIMZ is detected when frB is a NaN
        return r
    ps = A.sign ^ C.sign
    if imz:
        return _R("nan", bits=QNAN_DEFAULT, raised={"VXIMZ"})         # PAGE 403 'VXIMZ'
    bs = B.sign ^ sub
    if A.kind == "inf" or C.kind == "inf":
        if B.kind == "inf" and bs != ps:
            return _R("nan", bits=QNAN_DEFAULT, raised={"VXISI"})     # PAGE 403 'VXISI'
        return _R("inf", ps)
    if B.kind == "inf":
        return _R("inf", bs)
    pz = A.kind == "zero" or C.kind == "zero"
    return _exact_sum(st, ps, A.mag * C.mag, pz, bs, B.mag, B.kind == "zero")


def _finish(ctx, st, out, dst, r, fmt, rc, negate=False, put_kind="double"):
    raised = set(r.raised)
    invalid = any(n.startswith("VX") for n in raised)
    if r.kind == "nan" and r.generated and (st.fpscr & NI):
        _ni_generated_invalid(ctx, st, out, dst, raised, rc)
        return
    if (invalid and st.fpscr & VE) or ("ZX" in raised and st.fpscr & ZE):
        # PAGE 401 'FPRF is set ... except for invalid operation exceptions when FPSCR[VE] = 1 and
        # zero divide exceptions when FPSCR[ZE] = 1'; PAGE 416 '* No result if FPSCR[ZE] = 1',
        # '** No result if FPSCR[VE] = 1'.  CROSSCHECK FIX: FR/FI when no result is produced are not stated
        # (PAGE 399 defines them only for a rounded result) -> undefined (was: cleared, EXTERNAL).
        _deliver_status(st, out, raised, False, False, None, rc, keep_fr_fi=True)
        out.fpscr_mask |= FR | FI
        out.note("no result (VE=1 invalid / ZE=1 zero divide): FR/FI undefined PAGE 399/401/416")
        return
    mask, frfi_undef = MASK64, False
    if r.kind == "nan":
        # PAGE 412 fnmadd 'QNaNs propagate with no effect on their sign bit' / 'generated ... sign bit
        # of zero' / 'SNaNs ... retain the sign bit': NaNs are never negated.
        bits, fr, fi, fprf = r.bits, False, False, 0x11
    elif r.kind in ("inf", "zero"):
        s = r.sign ^ int(negate)
        bits = _inf(s) if r.kind == "inf" else s << 63
        fr, fi, fprf = False, False, _fprf(r.kind, s)
    else:
        rr = round_result(ctx, st.fpscr, r.sign, r.mag, fmt)
        raised |= rr.raised
        s = rr.sign ^ int(negate)
        # PAGE 412 'rounded ... then negated': negation after rounding in the pre-negation sign.
        bits = rr.bits ^ (SIGN64 if negate else 0)
        fr, fi, fprf = rr.fr, rr.fi, _fprf(rr.kind, s)
        mask, frfi_undef = rr.mask, rr.frfi_undef
    _deliver_status(st, out, raised, fr, fi, fprf, rc, keep_fr_fi=frfi_undef)
    if frfi_undef:
        out.fpscr_mask |= FR | FI | FPRF_MASK
    _put(st, out, dst, bits, put_kind)
    if mask != MASK64:
        out.mask_fpr(dst, 0, MASK64 & ~mask, "adjusted result out of range PAGE 82")
        if put_kind == "single" and st.hid2 & HID2_PSE:
            out.mask_fpr(dst, 1, MASK64 & ~mask, "adjusted result out of range PAGE 82")


def _fields(word):
    return (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31, word & 1


def _arith(ctx, st, out, word, xo, single):
    d, a, b, c, rc = _fields(word)
    fmt = "s" if single else "d"
    put_kind = "single" if single else "double"

    def opnd(reg, tag):
        return _operand(ctx, st, out, st.fpr[reg][0], tag, single)

    if xo in (21, 20):                                   # fadd(s) PAGE 395/396, fsub(s) PAGE 422/423
        r = _r_add(ctx, st, opnd(a, "A"), opnd(b, "B"), 1 if xo == 20 else 0)
    elif xo == 25:                                       # fmul(s) PAGE 408/409 (frD,frA,frC)
        r = _r_mul(ctx, st, opnd(a, "A"), opnd(c, "C"))
    elif xo == 18:                                       # fdiv(s) PAGE 401/402
        r = _r_div(ctx, st, opnd(a, "A"), opnd(b, "B"))
    else:                                                # 29 fmadd 28 fmsub 31 fnmadd 30 fnmsub
        A, B, C = opnd(a, "A"), opnd(b, "B"), opnd(c, "C")
        r = _r_madd(ctx, st, A, C, B, 1 if xo in (28, 30) else 0)
        _finish(ctx, st, out, d, r, fmt, rc, negate=xo in (30, 31), put_kind=put_kind)
        return
    _finish(ctx, st, out, d, r, fmt, rc, put_kind=put_kind)


# ---- estimates ----------------------------------------------------------------------------------
def _estimate(ctx, st, out, word, recip_sqrt):
    """fres PAGE 416-417 / frsqrte PAGE 419-420.  'correct to a precision of one part in 4096' and
    'the value placed into register frD may vary between implementations, and between different
    executions on the same implementation' -> finite results are undefined within that bound.
    'FR (undefined), FI (undefined)'.  fres affects OX/UX but not XX; frsqrte affects neither."""
    d, _a, b, _c, rc = _fields(word)
    single = not recip_sqrt
    B = _operand(ctx, st, out, st.fpr[b][0], "B", single)
    fr = ctx.choose("estimate_FR_value")
    fi = ctx.choose("estimate_FI_value")
    put_kind = "single" if single else "double"
    what = "frsqrte" if recip_sqrt else "fres"
    raised = set()
    if B.kind == "nan":
        r = _nan_result(ctx, [("B", B)])                    # 'SNaN QNaN** VXSNAN', 'QNaN QNaN None'
    elif B.kind == "inf":
        if recip_sqrt and B.sign:
            r = _R("nan", bits=QNAN_DEFAULT, raised={"VXSQRT"})  # '-inf QNaN** VXSQRT'
        else:
            r = _R("zero", B.sign)                          # '+inf +0', fres '-inf -0'
    elif B.kind == "zero":
        r = _R("inf", B.sign, raised={"ZX"})                # '-0 -inf* ZX', '+0 +inf* ZX'
    elif recip_sqrt and B.sign:
        r = _R("nan", bits=QNAN_DEFAULT, raised={"VXSQRT"})  # '<0 QNaN** VXSQRT'
    else:
        r = None
    if r is not None:
        raised = set(r.raised)
        invalid = any(n.startswith("VX") for n in raised)
        if r.kind == "nan" and r.generated and (st.fpscr & NI):
            _ni_generated_invalid(ctx, st, out, d, raised, rc)
            return
        if (invalid and st.fpscr & VE) or ("ZX" in raised and st.fpscr & ZE):
            _deliver_status(st, out, raised, fr, fi, None, rc)   # '* No result' / '** No result'
            return
        if r.kind == "nan":
            bits, fprf = r.bits, 0x11
        elif r.kind == "inf":
            bits, fprf = _inf(r.sign), _fprf("inf", r.sign)
        else:
            bits, fprf = r.sign << 63, _fprf("zero", r.sign)
        _deliver_status(st, out, raised, fr, fi, fprf, rc)
        _put(st, out, d, bits, put_kind)
        out.note("%s: FR and FI undefined PAGE 417/420" % what)
        return
    x = B.mag
    if recip_sqrt:
        e = _ilog2(x)
        j = e // 2
        y = x * _pow2(-2 * j)                               # y in [1, 4)
        k = 80
        exact_lo = Fraction(math.isqrt((y.denominator << (2 * k)) // y.numerator), 1 << k) * _pow2(-j)
        rep = round_result(_Ctx({}), 0, 0, exact_lo, "d")
        fmt_bound = "|est - 1/sqrt(x)| / (1/sqrt(x)) <= 1/4096 PAGE 419"
        sign = 0
    else:
        exact = 1 / x
        lo, hi = exact * Fraction(4095, 4096), exact * Fraction(4097, 4096)
        p, emin, emax, _ = FMT["s"]
        max_s = Fraction((1 << 24) - 1) * _pow2(emax - 23)
        if lo < _pow2(emin) or hi > max_s:
            # CROSSCHECK FIX (was: whole outcome undefined): PAGE 417 lists OX and UX as affected but gives no
            # rule.  The estimate band leaves the finite normal single range: OX is set when every value in the
            # band exceeds the largest single and may be set when only part does; UX is set when the whole band
            # is tiny and UE=1 (tininess alone signals an enabled underflow) and may be set otherwise; the value
            # except its sign, FPRF, FR and FI are undefined.
            raised = set()
            if lo > max_s or (hi > max_s and ctx.choose("fres_out_of_range_band_sets_OX")):
                raised.add("OX")
            if lo < _pow2(emin) and ((st.fpscr & UE and hi < _pow2(emin))
                                     or ctx.choose("fres_out_of_range_band_sets_UX")):
                raised.add("UX")
            _deliver_status(st, out, raised, fr, fi, None, rc)
            out.fpscr_mask |= FPRF_MASK
            _put(st, out, d, B.sign << 63, put_kind)
            out.mask_fpr(d, 0, MASK64 & ~SIGN64, "fres: estimate out of the single range PAGE 416-417")
            if st.hid2 & HID2_PSE:
                out.mask_fpr(d, 1, MASK64 & ~SIGN64, "fres: estimate out of the single range PAGE 416-417")
            out.note("fres: FR and FI undefined PAGE 417")
            return
        rep = round_result(_Ctx({}), 0, B.sign, exact, "s")
        fmt_bound = "|est - 1/x| / (1/x) <= 1/4096 PAGE 416"
        sign = B.sign
    _deliver_status(st, out, set(), fr, fi, _fprf("normal", sign), rc)
    _put(st, out, d, rep.bits, put_kind)
    out.mask_fpr(d, 0, MASK64 & ~SIGN64, "%s: finite estimate varies within 1/4096 PAGE 416/419" % what)
    if put_kind == "single" and st.hid2 & HID2_PSE:
        out.mask_fpr(d, 1, MASK64 & ~SIGN64, "fres: ps1 duplicates the (undefined) ps0 estimate PAGE 417")
    out.bounds.append("fr%d: %s (and ps1 == ps0 when PSE=1 for fres)" % (d, fmt_bound))
    out.note("%s: FR and FI undefined PAGE 417/420" % what)


# ---- frsp, fctiw(z), fsel, moves -----------------------------------------------------------------
def _frsp(ctx, st, out, word):
    """PAGE 418: 'rounded to single-precision using the rounding mode specified by FPSCR[RN]';
    model D.4.1 (EXTERNAL).  NI operand/result rules from PAGE 84/85 (operand 'Double denormalized'
    -> zero, result 'Single Denormalized' -> zero)."""
    d, _a, b, _c, rc = _fields(word)
    B = _operand(ctx, st, out, st.fpr[b][0], "B", False)
    if B.kind == "nan":
        r = _nan_result(ctx, [("B", B)])
        # CROSSCHECK FIX: the result is a single-precision QNaN (Table 2-22 PAGE 85 'Single QNaN, SNaN Return
        # QNaN') and 'all single-precision floating-point numbers are maintained in double precision format'
        # (PAGE 550) -> its image carries no payload below single width: those bits are 0 (was: undefined).
        r.bits &= ~((1 << 29) - 1)
    elif B.kind in ("inf", "zero"):
        r = _R(B.kind, B.sign)
    else:
        r = _R("fin", B.sign, B.mag)
    _finish(ctx, st, out, d, r, "s", rc, put_kind="frsp")


def _fctiw(ctx, st, out, word, toward_zero):
    """fctiw PAGE 399 / fctiwz PAGE 400, model D.4.2 (EXTERNAL); NI INT row PAGE 85 Table 2-22."""
    d, _a, b, _c, rc = _fields(word)
    B = _operand(ctx, st, out, st.fpr[b][0], "B", False)
    f = st.fpscr
    rn = 1 if toward_zero else f & 3
    raised = set()
    fr = fi = False
    value = None
    invalid = None
    nan_word_undefined = False
    if B.kind == "nan":
        raised = {"VXCVI"} | ({"VXSNAN"} if B.snan else set())
        if B.snan and ctx.choose("fctiw_SNaN_does_not_also_set_VXCVI"):
            # CROSSCHECK FIX: PAGE 399 lists 'VXSNAN, VXCVI' without saying whether an SNaN sets both
            raised = {"VXSNAN"}
        # CROSSCHECK FIX: PAGE 399 gives the result word only for operands > 2^31-1 and < -2^31; for a NaN in
        # IEEE mode it is not stated (D.4.2 is EXTERNAL) -> low word undefined (NI mode: Table 2-22 below).
        invalid = 0x80000000
        nan_word_undefined = True
    elif B.kind == "inf":
        raised = {"VXCVI"}
        invalid = 0x80000000 if B.sign else 0x7FFFFFFF    # PAGE 399 '> 2^31-1 ... 0x7FFF_FFFF'
    elif B.kind == "zero":
        value = 0
    else:
        n = B.mag.numerator // B.mag.denominator
        rem = B.mag - n
        if rn == 0:
            inc = rem > Fraction(1, 2) or (rem == Fraction(1, 2) and n & 1)
        elif rn == 1:
            inc = False
        elif rn == 2:
            inc = rem != 0 and B.sign == 0
        else:
            inc = rem != 0 and B.sign == 1
        mi = n + (1 if inc else 0)
        iv = -mi if B.sign else mi
        if iv > 0x7FFFFFFF:
            raised, invalid = {"VXCVI"}, 0x7FFFFFFF         # PAGE 399 'greater than 2^31 - 1'
        elif iv < -0x80000000:
            raised, invalid = {"VXCVI"}, 0x80000000         # PAGE 399 'less than -2^31'
        else:
            value = iv & MASK32
            fr, fi = bool(inc), rem != 0                    # PAGE 399 FR/FI sentences
            if fi:
                raised = {"XX"}
    write = True
    ni_ve_undefined = False
    if invalid is not None:
        value = invalid
        if f & NI:
            value = 0x80000000   # PAGE 85 Table 2-22 NI 'If (Invalid Operation) then Place (0x8000) into FPR[32-63]'
            nan_word_undefined = False
            if f & VE:
                # CROSSCHECK FIX: Table 2-22 NI column 'Place (0x80000000)' vs the IEEE 'no result' for VE=1;
                # neither reading states FPRF -> frD (both slots) and FPRF undefined (was: a write choice with
                # FPRF defined).
                write = False
                ni_ve_undefined = True
        elif f & VE:
            write = False
    # CROSSCHECK FIX: FR/FI are defined only for a converted result ('FR is set if the result is incremented
    # when rounded. FI is set if the result is inexact' PAGE 399); for an invalid conversion they are not
    # stated -> undefined (was: cleared).
    _deliver_status(st, out, raised, fr, fi, None, rc, keep_fr_fi=invalid is not None)
    if invalid is not None:
        out.fpscr_mask |= FR | FI
        out.note("fctiw(z) invalid conversion: FR/FI undefined PAGE 399")
    if not (invalid is not None and f & VE) or ni_ve_undefined:
        # PAGE 399 'Except for trap-enabled invalid operation exceptions, FPSCR[FPRF] is undefined'
        out.fpscr_mask |= FPRF_MASK
        out.note("fctiw(z): FPRF undefined PAGE 399")
    if ni_ve_undefined:
        out.mask_fpr(d, 0, MASK64, "fctiw(z) NI=1 VE=1 invalid: result unstated PAGE 85")
        out.mask_fpr(d, 1, MASK64, "fctiw(z) NI=1 VE=1 invalid: result unstated PAGE 85")
    if write:
        _put(st, out, d, value, "double")
        out.mask_fpr(d, 0, MASK64 if nan_word_undefined else 0xFFFFFFFF00000000,
                     "fctiw(z): 'Bits 0-31 of frD are undefined' PAGE 399")


def _fsel(ctx, st, out, word):
    """PAGE 421: 'if (frA) >= 0.0 then frD <- (frC) else frD <- (frB)'; NaN selects frB; +0 == -0."""
    d, a, b, c, rc = _fields(word)
    A = _operand(ctx, st, out, st.fpr[a][0], "A", False, ni_choice_key="ni_zeroing_applies_to_fsel")
    ge = A.kind == "zero" or (A.kind in ("fin", "inf") and A.sign == 0)
    src = c if ge else b
    _put(st, out, d, st.fpr[src][0], "fsel")
    _cr1_from_fpscr(out, rc)


def _move(ctx, st, out, word, xo):
    """fmr PAGE 405, fneg PAGE 411, fabs PAGE 394, fnabs PAGE 410: bit operations on the image,
    'treats NaNs just like any other kind of value', 'does not alter the FPSCR'."""
    d, _a, b, _c, rc = _fields(word)
    v = st.fpr[b][0]
    if xo == 40:
        v ^= SIGN64
    elif xo == 264:
        v &= ~SIGN64
    elif xo == 136:
        v |= SIGN64
    # CROSSCHECK FIX: fmr PAGE 405 - a paired-single source leaves frD[ps1] unchanged but a double-precision
    # source is 'copied to frD'; the operand type is not architecturally visible and the ps1 image of a double
    # operand is unspecified (PAGE 83) -> ps1 undefined (was: unchanged).
    _put(st, out, d, v, "double")
    _cr1_from_fpscr(out, rc)


# ---- compares -----------------------------------------------------------------------------------
def _ordkey(v):
    if v.kind == "inf":
        return (-1 if v.sign else 1, Fraction(0))
    return (0, v.signed())


def _fcmp(ctx, st, out, word, ordered):
    """fcmpu PAGE 398 / fcmpo PAGE 397 pseudocode."""
    crf = (word >> 23) & 7
    a, b = (word >> 16) & 31, (word >> 11) & 31
    key = "ni_zeroing_applies_to_compare"
    A = _operand(ctx, st, out, st.fpr[a][0], "A", False, ni_choice_key=key)
    B = _operand(ctx, st, out, st.fpr[b][0], "B", False, ni_choice_key=key)
    if A.kind == "nan" or B.kind == "nan":
        c = 0b0001
    elif _ordkey(A) < _ordkey(B):
        c = 0b1000
    elif _ordkey(A) > _ordkey(B):
        c = 0b0100
    else:
        c = 0b0010                   # PAGE 98 'The comparison ignores the sign of zero'
    snan = (A.kind == "nan" and A.snan) or (B.kind == "nan" and B.snan)
    qnan = (A.kind == "nan" and not A.snan) or (B.kind == "nan" and not B.snan)
    raised = set()
    if snan:
        raised.add("VXSNAN")
        if ordered and not st.fpscr & VE:
            raised.add("VXVC")
    elif ordered and qnan:
        raised.add("VXVC")
    if "VXVC" in raised and st.fpscr & NI and ctx.choose("ni_invalid_compare_not_signalled"):
        # CROSSCHECK FIX: PAGE 82 NI mode treats 'some IEEE invalid operations' in a non-IEEE manner (unnamed)
        raised.discard("VXVC")
    _deliver_status(st, out, raised, False, False, None, False, keep_fr_fi=True)
    out.fpscr = (out.fpscr & ~FPCC_MASK) | (c << 12)          # 'FPCC <- c'
    shift = 28 - 4 * crf
    out.cr = (out.cr & ~(0xF << shift)) | (c << shift)        # 'CR[(4*crfD)-(4*crfD+3)] <- c'
    if word & 1:
        # PAGE 98 'In Gekko, crfD should be treated as undefined'; PAGE 175 'the BF field in an
        # instruction encoding for these cases is considered undefined' -> any CR field may be hit.
        out.cr_mask |= MASK32
        out.note("fcmpu/fcmpo with Rc=1: CR undefined PAGE 98/175")


# ---- FPSCR instructions -------------------------------------------------------------------------
def _mtfs_trap(st, out, old, new):
    """FPSCR-altering instructions (mtfsb0/mtfsb1/mtfsf/mtfsfi, and mcrfs - CROSSCHECK FIX: mcrfs added):
    FEX 0 -> 1 takes the enabled program exception; FEX left at 1 is unstated (PAGE 166 speaks of IEEE
    exceptions being taken, not of an enabled condition that already existed) -> UndefinedBehaviour."""
    if st.msr & (MSR_FE0 | MSR_FE1) and new & FEX:
        if old & FEX:
            raise UndefinedBehaviour("move-to-FPSCR with FEX already set and MSR[FE0|FE1] != 0: whether "
                                     "a program exception is taken is unstated PAGE 166", applied=False)
        out.exc = ("program", 0x700, SRR1_FP_ENABLED, None)


def _fpscr_op(ctx, st, out, word, xo):
    old = st.fpscr
    rc = word & 1
    if xo == 583:                                            # mffs PAGE 460 'frD[32-63] <- FPSCR'
        d = (word >> 21) & 31
        _put(st, out, d, old, "double")
        out.mask_fpr(d, 0, 0xFFFFFFFF00000000, "mffs: 'The high-order bits of register frD are undefined' PAGE 460")
        _cr1_from_fpscr(out, rc)
        return
    if xo == 64:                                             # mcrfs PAGE 457
        crfd, crfs = (word >> 23) & 7, (word >> 18) & 7
        shift = 28 - 4 * crfs
        c = (old >> shift) & 0xF
        # 'All exception bits copied (except FEX and VX) are cleared in the FPSCR'
        new = _summary(old & ~((0xF << shift) & (FX | EXC_ALL)))
        out.fpscr = new
        dshift = 28 - 4 * crfd
        out.cr = (out.cr & ~(0xF << dshift)) | (c << dshift)
        _mtfs_trap(st, out, old, new)
        return
    if xo == 711:                                            # mtfsf PAGE 473
        fm = (word >> 17) & 0xFF
        src = st.fpr[(word >> 11) & 31][0] & MASK32
        mask = 0
        for i in range(8):
            if fm & (0x80 >> i):
                mask |= 0xF << (28 - 4 * i)
        # 'FPSCR[FX] is altered only if FM[0] = 1'; 'bits 0 (FX) and 3 (OX) are set to the values of
        # frB[32] and frB[35]'; 'Bits 1 and 2 (FEX and VX) are set according to the usual rule'
        new = _summary((old & ~mask) | (src & mask))
    elif xo == 134:                                          # mtfsfi PAGE 474
        crfd, imm = (word >> 23) & 7, (word >> 12) & 0xF
        shift = 28 - 4 * crfd
        # 'FPSCR[FX] is altered only if crfD = 0'; FX/OX from IMM[0]/IMM[3]; FEX/VX usual rule
        new = _summary((old & ~(0xF << shift)) | (imm << shift))
    else:                                                    # mtfsb1 (38) PAGE 472, mtfsb0 (70) PAGE 471
        bit = (word >> 21) & 31
        m = _b(bit)
        if bit in (1, 2):
            new = old          # 'Bits 1 and 2 (FEX and VX) cannot be explicitly set' / 'cleared'
        elif xo == 38:
            new = old | m
            if (m & EXC_ALL) and not (old & m):
                new |= FX      # 'Affected: FPSCR bit crbD and FX' - by the usual 0->1 rule
        else:
            new = old & ~m
        new = _summary(new)
    if xo == 711 and mask & _b(20) or xo == 134 and shift == 8 or xo in (38, 70) and bit == 20:
        # CROSSCHECK FIX: FPSCR bit 20 is reserved (architectural layout, the corpus defines no function for
        # it) -> the value written to it is undefined
        out.fpscr_mask |= _b(20)
        out.note("FPSCR bit 20 reserved: written value undefined (PAGE 473 fields; layout EXTERNAL)")
    out.fpscr = new
    _cr1_from_fpscr(out, rc)
    _mtfs_trap(st, out, old, new)


# ---- loads and stores ---------------------------------------------------------------------------
def _loadstore(ctx, st, out, word, update, indexed, single, store):
    """lfs PAGE 434, lfsu 435, lfsux 436, lfsx 437, lfd 430, lfdu 431, lfdux 432, lfdx 433,
    stfs 551, stfsu 552, stfsux 553, stfsx 554, stfd/stfdu/stfdux/stfdx PAGE 546-549."""
    fr_n, a = (word >> 21) & 31, (word >> 16) & 31
    if update and a == 0:
        raise UndefinedBehaviour("update form with rA = 0: 'the instruction form is invalid' PAGE 107/431",
                                 applied=False)
    if indexed:
        off = st.gpr[(word >> 11) & 31]
        if word & 1:
            # PAGE 106 'invalid when the Rc bit is one. In Gekko, executing one of these invalid
            # instruction forms causes CR0 to be set to an undefined value.'
            out.cr_mask |= 0xF0000000
            out.note("indexed FP load/store with Rc=1: CR0 undefined PAGE 106")
    else:
        off = word & 0xFFFF
        if off & 0x8000:
            off -= 0x10000                                   # EXTS(d)
    base = st.gpr[a] if (a != 0 or update) else 0           # '(rA|0)' vs '(rA)'
    ea = (base + off) & MASK32
    if st.msr & MSR_LE:
        raise UndefinedBehaviour("little-endian byte ordering is defined outside the corpus PAGE 81",
                                 applied=False)
    if ea & 3:
        # PAGE 175 'An alignment exception is initiated when ... The operand of a floating-point load
        # or store is not word-aligned'; PAGE 81 'If a floating-point operand is not aligned, Gekko
        # invokes an alignment exception'.  DAR/DSISR settings are in PEM ch.6 (not in corpus).
        out.exc = ("alignment", 0x600, 0, ea)
        out.other.append("dsisr")
        out.note("alignment exception: DAR=EA assumed, DSISR unstated PAGE 175")
        return
    width = 4 if single else 8
    if store:
        v = st.fpr[fr_n][0]                                  # PAGE 83 'store the ps0 value only'
        if single:
            w, m, note = double_to_single(v)
            out.writes.append((ea, 4, w))
            if m:
                out.mem_mask.append((ea, 4, m))
                out.note(note)
        else:
            out.writes.append((ea, 8, v))
    else:
        out.reads.append((ea, width))
        raw = st.mem._get(ea, width)
        if single:
            _put(st, out, fr_n, single_to_double(raw), "single")
        else:
            _put(st, out, fr_n, raw, "keep1")
    if update:
        out.gpr[a] = ea                                      # 'EA is placed into rA'


# =================================================================================================
# Decode / dispatch
# =================================================================================================
_A59 = {18, 20, 21, 22, 24, 25, 28, 29, 30, 31}
_A63 = {18, 20, 21, 22, 23, 25, 26, 28, 29, 30, 31}
_X63 = {0, 12, 14, 15, 32, 38, 40, 64, 70, 72, 134, 136, 264, 583, 711}
_X31 = {535: (0, 1, 1, 0), 567: (1, 1, 1, 0), 599: (0, 1, 0, 0), 631: (1, 1, 0, 0),
        663: (0, 1, 1, 1), 695: (1, 1, 1, 1), 727: (0, 1, 0, 1), 759: (1, 1, 0, 1)}
#       xo: (update, indexed, single, store)


def _decode(word):
    op = word >> 26
    if 48 <= op <= 55:
        return ("D", op)
    if op == 31 and ((word >> 1) & 0x3FF) in _X31:
        return ("X31", (word >> 1) & 0x3FF)
    if op == 59 and ((word >> 1) & 31) in _A59:
        return ("A59", (word >> 1) & 31)
    if op == 63:
        if ((word >> 1) & 31) in _A63:
            return ("A63", (word >> 1) & 31)
        if ((word >> 1) & 0x3FF) in _X63:
            return ("X63", (word >> 1) & 0x3FF)
    return None


def supports(word):
    return _decode(word & MASK32) is not None


def _reserved_check(kind, xo, word):
    """PAGE 87 'If instructions are encoded with incorrectly set bits in reserved fields, the
    results on execution can be said to be boundedly undefined.'  Field layouts from the
    instruction pages (PAGE 394-423, 457, 460, 471-474)."""
    bad = 0
    A, B, C = (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31
    if kind in ("A59", "A63"):
        if xo in (18, 20, 21):
            bad = C
        elif xo == 25:
            bad = B
        elif xo in (24, 26):
            bad = A | C
    elif kind == "X63":
        if xo in (0, 32):
            bad = (word >> 21) & 3
        elif xo in (12, 14, 15, 40, 72, 136, 264):
            bad = A
        elif xo in (38, 70, 583):
            bad = (word >> 11) & 0x3FF
        elif xo == 64:
            bad = ((word >> 21) & 3) | ((word >> 11) & 0x7F) | (word & 1)
        elif xo == 134:
            bad = ((word >> 16) & 0x7F) | ((word >> 11) & 1)
        elif xo == 711:
            bad = ((word >> 25) & 1) | ((word >> 16) & 1)
    if bad:
        raise UndefinedBehaviour("reserved field set: boundedly undefined PAGE 87", applied=False)


def _execute(ctx, st, out, word):
    kind, xo = _decode(word)
    if kind in ("A59", "A63") and xo == 22:
        # PAGE 87 'It does not support the optional fsqrt, fsqrts' + 'invokes the illegal
        # instruction error handler (part of the program exception)'; program (priority 1) outranks
        # floating-point unavailable (priority 3) PAGE 162.
        out.exc = ("program", 0x700, SRR1_ILLEGAL, None)
        return
    _reserved_check(kind, xo, word)
    if (kind == "D" and xo & 1 or kind == "X31" and _X31[xo][0]) and ((word >> 16) & 31) == 0:
        # CROSSCHECK FIX: the invalid update form (rA = 0, PAGE 431 'If rA = 0, the instruction form is
        # invalid'; PAGE 87 'limited support') is undefined as a whole, including whether a floating-point
        # unavailable exception is taken -> checked before MSR[FP] (was checked after).
        raise UndefinedBehaviour("update form with rA = 0: 'the instruction form is invalid' PAGE 107/431",
                                 applied=False)
    if not st.msr & MSR_FP:
        # PAGE 175 'an attempt is made to execute a floating-point instruction (including floating-point
        # load, store, or move instructions), and ... (MSR[FP] = 0)'
        out.exc = ("fp_unavailable", 0x800, 0, None)
        return
    if kind == "D":
        op = xo
        _loadstore(ctx, st, out, word, update=op & 1, indexed=0, single=op in (48, 49, 52, 53),
                   store=op >= 52)
    elif kind == "X31":
        u, i, s, w = _X31[xo]
        _loadstore(ctx, st, out, word, update=u, indexed=i, single=s, store=w)
    elif kind in ("A59", "A63"):
        single = kind == "A59"
        if xo == 24:
            _estimate(ctx, st, out, word, recip_sqrt=False)
        elif xo == 26:
            _estimate(ctx, st, out, word, recip_sqrt=True)
        elif xo == 23:
            _fsel(ctx, st, out, word)
        else:
            _arith(ctx, st, out, word, xo, single)
    else:
        if xo in (0, 32):
            _fcmp(ctx, st, out, word, ordered=xo == 32)
        elif xo == 12:
            _frsp(ctx, st, out, word)
        elif xo in (14, 15):
            _fctiw(ctx, st, out, word, toward_zero=xo == 15)
        elif xo in (40, 72, 136, 264):
            _move(ctx, st, out, word, xo)
        else:
            _fpscr_op(ctx, st, out, word, xo)
    if out.exc is not None and out.exc[0] == "program" and out.exc[2] == SRR1_FP_ENABLED and             (out.fpr or out.cr != st.cr or out.cr_mask):
        # CROSSCHECK FIX: PAGE 161 program exceptions are 'taken without completing the exception-causing
        # instruction', while PAGE 395/401 imply a delivered result ('FPRF is set ... except for invalid
        # operation exceptions when FPSCR[VE] = 1') -> whether frD and the CR are written when a floating-point
        # enabled program exception is taken is enumerated (FPSCR is updated in both readings).
        if ctx.choose("fp_enabled_trap_discards_frD_and_CR"):
            out.fpr, out.fpr_mask = {}, {}
            out.cr, out.cr_mask = st.cr, 0


# =================================================================================================
# Self test: hand-derived vectors
# =================================================================================================
if __name__ == "__main__":
    import random
    import struct

    NCHK = [0]
    FAIL = []

    def check(name, got, want):
        NCHK[0] += 1
        if got != want:
            FAIL.append("%s: got %r want %r" % (name, got if not isinstance(got, int) else hex(got),
                                                 want if not isinstance(want, int) else hex(want)))

    def A_(op, xo, d, a, b, c, rc=0):
        return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1) | rc

    def X_(op, xo, d, a, b, rc=0):
        return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | rc

    def D_(op, d, a, disp):
        return (op << 26) | (d << 21) | (a << 16) | (disp & 0xFFFF)

    def dbl(x):   # constant construction only
        return struct.unpack(">Q", struct.pack(">d", x))[0]

    def fresh(fpscr=0, msr=0x2032, pse=True):
        s = State()
        s.msr = msr
        s.fpscr = fpscr
        s.hid2 = 0xE0000000 if pse else 0xC0000000
        return s

    def run(s, word):
        try:
            step(s, word)
            return None
        except UndefinedBehaviour as e:
            return e

    ONE, TWO, THREE = 0x3FF0000000000000, 0x4000000000000000, 0x4008000000000000
    PINF, NINF = 0x7FF0000000000000, 0xFFF0000000000000
    FPRF = lambda s: (s.fpscr >> 12) & 0x1F   # noqa: E731

    # --- 1-3 lfs, D.6 (PAGE 434) ---
    s = fresh(); s.gpr[3] = 0x80001000; s.mem.load(0x80001004, bytes.fromhex("3F800000"))
    e = run(s, D_(48, 1, 3, 4))
    check("lfs 1.0 ps0", s.fpr[1][0], ONE); check("lfs 1.0 ps1", s.fpr[1][1], ONE)
    check("lfs defined", e, None); check("lfs pc", s.pc, 0x80003104)
    check("lfs log", s.mem.log, [("r", 0x80001004, 4, 0x3F800000)])
    s = fresh(); s.mem.load(0x100, bytes.fromhex("00000001")); run(s, D_(48, 2, 0, 0x100))
    check("lfs min single denorm -> 2^-149", s.fpr[2][0], 0x36A0000000000000)
    s = fresh(); s.mem.load(0x100, bytes.fromhex("7F800001")); run(s, D_(48, 2, 0, 0x100))
    check("lfs SNaN stays SNaN", s.fpr[2][0], 0x7FF0000020000000)
    s = fresh(); s.mem.load(0x100, bytes.fromhex("80000000")); run(s, D_(48, 2, 0, 0x100))
    check("lfs -0", s.fpr[2][0], 0x8000000000000000)
    # --- 4 lfs with PSE=0: ps1 undefined ---
    s = fresh(pse=False); s.mem.load(0x100, bytes.fromhex("40000000")); e = run(s, D_(48, 2, 0, 0x100))
    check("lfs PSE=0 ps0", s.fpr[2][0], TWO); check("lfs PSE=0 ps1 undefined", e.fpr_masks.get((2, 1)), MASK64)
    # --- 5 lfsu rA=0 invalid form ---
    s = fresh(); e = run(s, D_(49, 2, 0, 0x100))
    check("lfsu rA=0 UB", (e.applied, s.pc), (False, 0x80003100))
    # --- 6 lfsx Rc=1: CR0 undefined, executes, (rA|0) ---
    s = fresh(); s.gpr[4] = 0x200; s.mem.load(0x200, bytes.fromhex("3F800000"))
    e = run(s, X_(31, 535, 1, 0, 4, 1))
    check("lfsx Rc=1 value", s.fpr[1][0], ONE); check("lfsx Rc=1 CR0 mask", e.cr_mask, 0xF0000000)
    # --- 7 lfdux: update + load order ---
    s = fresh(); s.gpr[5] = 0x1000; s.gpr[6] = 8; s.mem.load(0x1008, THREE.to_bytes(8, "big"))
    run(s, X_(31, 631, 7, 5, 6))
    check("lfdux value", s.fpr[7][0], THREE); check("lfdux rA", s.gpr[5], 0x1008)
    s.fpr[7][1] = 0x1234; run(s, X_(31, 631, 7, 5, 6))
    check("lfd PSE=1 keeps ps1 (PAGE 84)", s.fpr[7][1], 0x1234)
    # --- 8 alignment (PAGE 175) ---
    s = fresh(); s.gpr[3] = 0x80000001
    e = run(s, D_(50, 1, 3, 0))
    check("lfd misaligned exception", (s.exception, s.pc, s.srr0, s.dar), ("alignment", 0x600, 0x80003100, 0x80000001))
    check("alignment dsisr undefined", e.other, ["dsisr"])
    # --- 9 FP unavailable (PAGE 175) ---
    s = fresh(msr=0x0032); run(s, A_(63, 21, 1, 2, 3, 0))
    check("fadd MSR[FP]=0", (s.exception, s.pc, s.srr0, s.srr1, s.msr), ("fp_unavailable", 0x800, 0x80003100, 0x0032, 0))
    # --- 10 fsqrt illegal (PAGE 87) ---
    s = fresh(msr=0x0032); run(s, A_(63, 22, 1, 0, 3, 0))
    check("fsqrt illegal", (s.exception, s.pc, s.srr1), ("program", 0x700, 0x00080032))
    # --- 11-15 stfs, D.7 / Table 2-41 (PAGE 108, 551) ---
    s = fresh(); s.fpr[1][0] = ONE; s.gpr[3] = 0x400; run(s, D_(52, 1, 3, 4))
    check("stfs 1.0", s.mem.log, [("w", 0x404, 4, 0x3F800000)])
    s = fresh(); s.fpr[1][0] = 0x3800000000000000; run(s, D_(52, 1, 0, 0x400))
    check("stfs 2^-127 denormalizes", s.mem._get(0x400, 4), 0x00400000)
    s = fresh(); s.fpr[1][0] = 0x36A0000000000000; run(s, D_(52, 1, 0, 0x400))
    check("stfs 2^-149 (exp 874)", s.mem._get(0x400, 4), 0x00000001)
    s = fresh(); s.fpr[1][0] = 0x4C70000000000000; run(s, D_(52, 1, 0, 0x400))
    check("stfs 2^200 bit-drops", s.mem._get(0x400, 4), 0x63800000)
    s = fresh(); s.fpr[1][0] = 0x8000000000000001; e = run(s, D_(52, 1, 0, 0x400))
    check("stfs double denorm stores zero", (s.mem._get(0x400, 4) & 0x7FFFFFFF, e.mem_masks), (0, [(0x400, 4, 0x80000000)]))
    s = fresh(); s.fpr[1][0] = 0x7FF0000000000001; run(s, D_(52, 1, 0, 0x400))
    check("stfs SNaN low payload bit-drops", s.mem._get(0x400, 4), 0x7F800000)
    # --- 16 stfdu ---
    s = fresh(); s.fpr[9][0] = 0x0123456789ABCDEF; s.gpr[3] = 0x800; run(s, D_(55, 9, 3, -8))
    check("stfdu", (s.mem.log, s.gpr[3]), ([("w", 0x7F8, 8, 0x0123456789ABCDEF)], 0x7F8))
    # --- 17 fadd basic + FPRF (PAGE 395) ---
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = ONE, TWO; e = run(s, A_(63, 21, 1, 2, 3, 0))
    check("fadd 1+2", s.fpr[1][0], THREE); check("fadd FPRF +normal", FPRF(s), 0x04)
    check("fadd PSE=1 ps1 undefined", e.fpr_masks, {(1, 1): MASK64})
    # --- 18 aliasing frD = frA = frB ---
    s = fresh(); s.fpr[4][0] = THREE; run(s, A_(63, 21, 4, 4, 4, 0))
    check("fadd aliased 3+3", s.fpr[4][0], 0x4018000000000000)
    # --- 19 inf - inf -> VXISI, default QNaN ---
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = PINF, PINF; run(s, A_(63, 20, 1, 2, 3, 0, 1))
    check("fsub inf-inf value", s.fpr[1][0], QNAN_DEFAULT)
    check("fsub inf-inf fpscr", s.fpscr, FX | VX | VXISI | (0x11 << 12))
    check("fsub. CR1", s.cr, 0x0A000000)
    # --- 20-21 VE=1: no result; with FE0 -> program exception ---
    s = fresh(fpscr=VE); s.fpr[1][0] = 0x5555; s.fpr[2][0], s.fpr[3][0] = PINF, NINF
    run(s, A_(63, 21, 1, 2, 3, 0))
    check("fadd VE=1 no result", s.fpr[1][0], 0x5555)
    check("fadd VE=1 fpscr", s.fpscr, FX | FEX | VX | VXISI | VE)
    check("fadd VE=1 FE=0 no trap", s.exception, None)
    s = fresh(fpscr=VE, msr=0x2032 | MSR_FE0); s.fpr[2][0], s.fpr[3][0] = PINF, NINF
    run(s, A_(63, 21, 1, 2, 3, 0))
    check("fadd VE=1 FE0 trap", (s.exception, s.pc, s.srr0, s.srr1), ("program", 0x700, 0x80003100, 0x00102832))
    # --- 22-25 rounding modes on 1 + 2^-53 ---
    for rn, want, frfi in ((0, ONE, FI), (1, ONE, FI), (2, 0x3FF0000000000001, FR | FI), (3, ONE, FI)):
        s = fresh(fpscr=rn); s.fpr[2][0], s.fpr[3][0] = ONE, 0x3CA0000000000000
        run(s, A_(63, 21, 1, 2, 3, 0))
        check("fadd 1+2^-53 RN=%d" % rn, s.fpr[1][0], want)
        check("fadd 1+2^-53 RN=%d FR/FI/XX" % rn, s.fpscr & (FR | FI | XX | FX), frfi | XX | FX)
    s = fresh(fpscr=3); s.fpr[2][0], s.fpr[3][0] = 0xBFF0000000000000, 0xBCA0000000000000
    run(s, A_(63, 21, 1, 2, 3, 0))
    check("fadd -1-2^-53 RN=-inf", (s.fpr[1][0], bool(s.fpscr & FR)), (0xBFF0000000000001, True))
    # --- 26-28 zero signs ---
    s = fresh(); s.fpr[2][0] = THREE; run(s, A_(63, 20, 1, 2, 2, 0))
    check("fsub x-x = +0", (s.fpr[1][0], FPRF(s)), (0, 0x02))
    s = fresh(fpscr=3); s.fpr[2][0] = THREE; run(s, A_(63, 20, 1, 2, 2, 0))
    check("fsub x-x RN=-inf = -0", (s.fpr[1][0], FPRF(s)), (SIGN64, 0x12))
    s = fresh(); s.fpr[2][0] = SIGN64; run(s, A_(63, 21, 1, 2, 2, 0))
    check("fadd -0+-0 = -0", s.fpr[1][0], SIGN64)
    # --- 29-30 single precision (PAGE 396) ---
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = ONE, 0x3E70000000000000   # 2^-24
    e = run(s, A_(59, 21, 1, 2, 3, 0))
    check("fadds 1+2^-24 tie-even", (s.fpr[1][0], s.fpr[1][1], e), (ONE, ONE, None))
    check("fadds inexact", s.fpscr & (FI | XX), FI | XX)
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = 0x3FF0000000000001, ONE
    e = run(s, A_(59, 21, 1, 2, 3, 0))
    check("fadds non-single operand UB", (e.applied, s.pc), (False, 0x80003100))
    # --- 31-32 single overflow ---
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = 0x47E0000000000000, TWO     # 2^127 * 2
    e = run(s, A_(59, 25, 1, 2, 0, 3))
    check("fmuls overflow inf", s.fpr[1][0], PINF)
    check("fmuls overflow flags", s.fpscr & ~FR, FX | OX | XX | FI | (0x05 << 12))
    check("fmuls overflow FR undefined", e.fpscr_mask, FR)
    s = fresh(fpscr=1); s.fpr[2][0], s.fpr[3][0] = 0x47E0000000000000, TWO
    run(s, A_(59, 25, 1, 2, 0, 3))
    check("fmuls overflow RN=0 -> max single", s.fpr[1][0], 0x47EFFFFFE0000000)
    # --- 33 enabled overflow adjust 1536 ---
    s = fresh(fpscr=OE); s.fpr[2][0], s.fpr[3][0] = 0x7FE0000000000000, TWO
    run(s, A_(63, 25, 1, 2, 0, 3))
    check("fmul OE=1 adjusted", (s.fpr[1][0], s.fpscr), (0x1FF0000000000000, FX | FEX | OX | OE | (0x04 << 12)))
    # --- 34-37 divide family ---
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = ONE, 0; run(s, A_(63, 18, 1, 2, 3, 0))
    check("fdiv 1/0", (s.fpr[1][0], s.fpscr), (PINF, FX | ZX | (0x05 << 12)))
    s = fresh(fpscr=ZE | (0x04 << 12)); s.fpr[1][0] = 7; s.fpr[2][0], s.fpr[3][0] = ONE, SIGN64
    run(s, A_(63, 18, 1, 2, 3, 0))
    check("fdiv ZE=1 no result", (s.fpr[1][0], s.fpscr), (7, FX | FEX | ZX | ZE | (0x04 << 12)))
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = 0, SIGN64; run(s, A_(63, 18, 1, 2, 3, 0))
    check("fdiv 0/0", (s.fpr[1][0], s.fpscr), (QNAN_DEFAULT, FX | VX | VXZDZ | (0x11 << 12)))
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = NINF, PINF; run(s, A_(63, 18, 1, 2, 3, 0))
    check("fdiv inf/inf", s.fpscr & VX_ALL, VXIDI)
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = PINF, 0; run(s, A_(63, 25, 1, 2, 0, 3))
    check("fmul inf*0", (s.fpr[1][0], s.fpscr & VX_ALL), (QNAN_DEFAULT, VXIMZ))
    # --- 38-40 NaN propagation ---
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = 0xFFF8000000000123, 0x7FF0000000000001
    run(s, A_(63, 21, 1, 2, 3, 0))
    check("fadd QNaN(A) beats SNaN(B)", (s.fpr[1][0], s.fpscr & VX_ALL), (0xFFF8000000000123, VXSNAN))
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = ONE, 0x7FF0000000000001
    run(s, A_(63, 25, 1, 3, 0, 2))
    check("fmul SNaN quieted", s.fpr[1][0], 0x7FF8000000000001)
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = ONE, 0x7FF8000000000000
    e = run(s, A_(63, 20, 1, 2, 3, 0))
    check("fsub frB NaN sign ambiguous", e.fpr_masks.get((1, 0)), SIGN64)
    # --- 41-43 underflow ---
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = 0x0010000000000000, 0x3FE0000000000000
    run(s, A_(63, 25, 1, 2, 0, 3))
    check("fmul exact denorm", (s.fpr[1][0], s.fpscr), (0x0008000000000000, 0x14 << 12))
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = 3, 0x3FE0000000000000
    run(s, A_(63, 25, 1, 2, 0, 3))
    check("fmul 1.5 ulp tiny -> 2", (s.fpr[1][0], s.fpscr), (2, FX | UX | XX | FR | FI | (0x14 << 12)))
    s = fresh(fpscr=UE); s.fpr[2][0], s.fpr[3][0] = 1, 0x3FE0000000000000
    run(s, A_(63, 25, 1, 2, 0, 3))
    check("fmul UE=1 adjust +1536", (s.fpr[1][0], s.fpscr), (0x5CC0000000000000, FX | FEX | UX | UE | (0x04 << 12)))
    # --- 44-45 NI mode ---
    s = fresh(fpscr=NI); s.fpr[2][0], s.fpr[3][0] = 0x0010000000000000, 0x3FE0000000000000
    e = run(s, A_(63, 25, 1, 2, 0, 3))
    check("NI tiny result -> zero", (s.fpr[1][0] & ~SIGN64, e.applied, bool(e.fpr_masks[(1, 0)] & SIGN64)), (0, True, True))
    s = fresh(fpscr=NI); s.fpr[2][0], s.fpr[3][0] = 1, ONE
    e = run(s, A_(63, 21, 1, 2, 3, 0))
    check("NI denorm operand zeroed: 1.0 exact", (s.fpr[1][0], s.fpscr & (FI | XX), e.fpscr_mask), (ONE, 0, 0))
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = 1, ONE; run(s, A_(63, 21, 1, 2, 3, 0))
    check("IEEE denorm operand kept: inexact", (s.fpr[1][0], s.fpscr & (FI | XX)), (ONE, FI | XX))
    # --- 46-50 fused multiply-add (PAGE 97) ---
    s = fresh(fpscr=2); s.fpr[2][0] = 0x3FF0000000000001; s.fpr[3][0] = 0xBFF0000000000000
    run(s, A_(63, 29, 1, 2, 3, 2))
    check("fmadd fused (1+2^-52)^2-1 RN=+inf", (s.fpr[1][0], s.fpscr & (FR | FI)), (0x3CC0000000000001, FR | FI))
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = ONE, ONE; run(s, A_(63, 31, 1, 2, 3, 2))
    check("fnmadd -(1*1+1)", (s.fpr[1][0], FPRF(s)), (0xC000000000000000, 0x08))
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = ONE, 0xFFF8000000000000; run(s, A_(63, 31, 1, 2, 3, 2))
    check("fnmadd NaN sign not negated", s.fpr[1][0], 0xFFF8000000000000)
    s = fresh(fpscr=2); s.fpr[2][0], s.fpr[3][0], s.fpr[4][0] = ONE, ONE, 0x3CA0000000000000
    run(s, A_(63, 31, 1, 2, 3, 4))
    check("fnmadd rounds before negation", s.fpr[1][0], 0xBFF0000000000001)
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = ONE, ONE; e = run(s, A_(59, 29, 1, 2, 3, 2))
    check("fmadds 2.0 ps1=ps0", (s.fpr[1][0], s.fpr[1][1], e), (TWO, TWO, None))
    s = fresh(); s.fpr[2][0], s.fpr[3][0], s.fpr[4][0] = PINF, 0x7FF8000000000000, 0
    e = run(s, A_(63, 29, 1, 2, 3, 4))
    check("fmadd inf*0+QNaN VXIMZ ambiguous", (s.fpr[1][0], bool(e.fpscr_mask & VXIMZ)), (0x7FF8000000000000, True))
    s = fresh(); s.fpr[2][0], s.fpr[3][0], s.fpr[4][0] = PINF, NINF, ONE
    run(s, A_(63, 28, 1, 2, 3, 4))
    check("fmsub inf*1 - (-inf) = +inf", (s.fpr[1][0], s.fpscr), (PINF, 0x05 << 12))
    s = fresh(); s.fpr[2][0], s.fpr[3][0], s.fpr[4][0] = PINF, PINF, ONE
    run(s, A_(63, 28, 1, 2, 3, 4))
    check("fmsub inf - inf VXISI", s.fpscr & VX_ALL, VXISI)
    # --- 51-55 compares (PAGE 397/398) ---
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = ONE, TWO; run(s, X_(63, 0, 1 << 2, 2, 3))
    check("fcmpu 1<2 crf1", (s.cr, s.fpscr), (0x08000000, 0x8 << 12))
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = 0, SIGN64; run(s, X_(63, 0, 7 << 2, 2, 3))
    check("fcmpu +0==-0 crf7", s.cr, 0x2)
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = 0x7FF0000000000001, ONE; run(s, X_(63, 0, 0, 2, 3))
    check("fcmpu SNaN", (s.cr, s.fpscr), (0x10000000, FX | VX | VXSNAN | (1 << 12)))
    s = fresh(); s.fpr[2][0], s.fpr[3][0] = 0x7FF8000000000000, ONE; run(s, X_(63, 32, 0, 2, 3))
    check("fcmpo QNaN VXVC", s.fpscr, FX | VX | VXVC | (1 << 12))
    s = fresh(fpscr=VE); s.fpr[2][0] = 0x7FF0000000000001; run(s, X_(63, 32, 0, 2, 2))
    check("fcmpo SNaN VE=1 no VXVC", s.fpscr & VX_ALL, VXSNAN)
    s = fresh(); s.fpr[2][0] = 0x7FF0000000000001; run(s, X_(63, 32, 0, 2, 2))
    check("fcmpo SNaN VE=0 VXVC", s.fpscr & VX_ALL, VXSNAN | VXVC)
    s = fresh(); e = run(s, X_(63, 0, 0, 2, 3, 1))
    check("fcmpu Rc=1 CR undefined", e.cr_mask, MASK32)
    # --- 56-62 fctiw / fctiwz (PAGE 399/400) ---
    def ctiw(val, fpscr=0, z=False):
        s = fresh(fpscr=fpscr); s.fpr[2][0] = val
        e = run(s, X_(63, 15 if z else 14, 1, 0, 2))
        return s, e
    s, e = ctiw(dbl(2.5))
    check("fctiw 2.5 nearest", (s.fpr[1][0] & MASK32, s.fpscr & ~FPRF_MASK), (2, FX | XX | FI))
    check("fctiw masks", (e.fpscr_mask, e.fpr_masks[(1, 0)]), (FPRF_MASK, 0xFFFFFFFF00000000))
    s, e = ctiw(dbl(2.5), fpscr=2)
    check("fctiw 2.5 +inf", (s.fpr[1][0] & MASK32, s.fpscr & (FR | FI)), (3, FR | FI))
    s, e = ctiw(dbl(-2.5), fpscr=3)
    check("fctiw -2.5 -inf", (s.fpr[1][0] & MASK32, s.fpscr & (FR | FI)), (0xFFFFFFFD, FR | FI))
    s, e = ctiw(dbl(-2.75), fpscr=0, z=True)
    check("fctiwz -2.75", (s.fpr[1][0] & MASK32, s.fpscr & (FR | FI)), (0xFFFFFFFE, FI))
    s, e = ctiw(dbl(2147483648.0))
    check("fctiw 2^31 saturates", (s.fpr[1][0] & MASK32, s.fpscr & ~FPRF_MASK), (0x7FFFFFFF, FX | VX | VXCVI))
    s, e = ctiw(dbl(2147483647.5))
    check("fctiw 2^31-0.5 rounds out of range", (s.fpr[1][0] & MASK32, s.fpscr & VXCVI), (0x7FFFFFFF, VXCVI))
    s, e = ctiw(dbl(-2147483648.0))
    check("fctiw -2^31 valid", (s.fpr[1][0] & MASK32, s.fpscr & VX_ALL), (0x80000000, 0))
    s, e = ctiw(0x7FF0000000000001)
    check("fctiw SNaN", (s.fpr[1][0] & MASK32, s.fpscr & VX_ALL), (0x80000000, VXSNAN | VXCVI))
    s, e = ctiw(dbl(2147483648.0), fpscr=NI)
    check("fctiw NI invalid -> 0x80000000", s.fpr[1][0] & MASK32, 0x80000000)
    s, e = ctiw(dbl(1e10), fpscr=VE)
    check("fctiw VE=1 no result, FPRF defined, FR/FI undefined", (s.fpr[1][0], e.fpscr_mask), (0, FR | FI))
    # --- 63-66 frsp (PAGE 418) ---
    s = fresh(); s.fpr[2][0] = 0x3FF0000000000001; e = run(s, X_(63, 12, 1, 0, 2))
    check("frsp 1+2^-52", (s.fpr[1][0], s.fpscr & (FI | XX)), (ONE, FI | XX))
    check("frsp ps1 undefined", e.fpr_masks, {(1, 1): MASK64})
    s = fresh(); s.fpr[2][0] = 0x3690000000000000; run(s, X_(63, 12, 1, 0, 2))
    check("frsp 2^-150 tie -> +0", (s.fpr[1][0], s.fpscr), (0, FX | UX | XX | FI | (0x02 << 12)))
    s = fresh(fpscr=2); s.fpr[2][0] = 0x3690000000000000; run(s, X_(63, 12, 1, 0, 2))
    check("frsp 2^-150 +inf -> 2^-149", (s.fpr[1][0], FPRF(s)), (0x36A0000000000000, 0x14))
    s = fresh(); s.fpr[2][0] = 0x47F0000000000000; run(s, X_(63, 12, 1, 0, 2))
    check("frsp 2^128 overflow", (s.fpr[1][0], s.fpscr & (OX | XX)), (PINF, OX | XX))
    s = fresh(); s.fpr[2][0] = 0xFFF0000020000000; run(s, X_(63, 12, 1, 0, 2))
    check("frsp SNaN quieted", (s.fpr[1][0], s.fpscr & VX_ALL), (0xFFF8000020000000, VXSNAN))
    # --- 67-69 fres / frsqrte (PAGE 416/419) ---
    s = fresh(); s.fpr[2][0] = 0; e = run(s, A_(59, 24, 1, 0, 2, 0))
    check("fres +0 -> +inf ZX", (s.fpr[1][0], s.fpr[1][1], s.fpscr & ~(FR | FI)), (PINF, PINF, FX | ZX | (0x05 << 12)))
    check("fres FR/FI undefined", e.fpscr_mask, FR | FI)
    s = fresh(); s.fpr[2][0] = NINF; run(s, A_(59, 24, 1, 0, 2, 0))
    check("fres -inf -> -0", s.fpr[1][0], SIGN64)
    s = fresh(); s.fpr[2][0] = TWO; e = run(s, A_(59, 24, 1, 0, 2, 0))
    check("fres 2.0 estimate representative", (s.fpr[1][0], FPRF(s)), (0x3FE0000000000000, 0x04))
    check("fres 2.0 masked", e.fpr_masks[(1, 0)], MASK64 & ~SIGN64)
    s = fresh(); s.fpr[2][0] = 0xBFF0000000000000; run(s, A_(63, 26, 1, 0, 2, 0))
    check("frsqrte -1 VXSQRT", (s.fpr[1][0], s.fpscr & VX_ALL), (QNAN_DEFAULT, VXSQRT))
    s = fresh(); s.fpr[2][0] = SIGN64; run(s, A_(63, 26, 1, 0, 2, 0))
    check("frsqrte -0 -> -inf", (s.fpr[1][0], s.fpscr & ZX), (NINF, ZX))
    s = fresh(); s.fpr[2][0] = dbl(4.0); e = run(s, A_(63, 26, 1, 0, 2, 0))
    check("frsqrte 4 representative 0.5", (s.fpr[1][0], FPRF(s)), (0x3FE0000000000000, 0x04))
    # --- 70-72 fsel (PAGE 421) ---
    for a_val, want in ((SIGN64, 3), (0x7FF8000000000000, 2), (0xBFF0000000000000, 2), (PINF, 3)):
        s = fresh(); s.fpr[5][0] = a_val; s.fpr[2][0], s.fpr[3][0] = 2, 3
        run(s, A_(63, 23, 1, 5, 2, 3))
        check("fsel A=%x" % a_val, s.fpr[1][0], want)
    # --- 73-76 moves (PAGE 394, 405, 410, 411) ---
    s = fresh(); s.fpr[2] = [0x7FF8000000000001, 5]; s.fpr[1][1] = 9
    e = run(s, X_(63, 72, 1, 0, 2)); check("fmr ps0 copy, ps1 undefined", (s.fpr[1][0], e.fpr_masks), (0x7FF8000000000001, {(1, 1): MASK64}))
    s = fresh(); s.fpr[2][0] = 0x7FF8000000000001; run(s, X_(63, 40, 1, 0, 2))
    check("fneg NaN sign flips", s.fpr[1][0], 0xFFF8000000000001)
    s = fresh(); s.fpr[2][0] = 0xFFF0000000000000; run(s, X_(63, 264, 1, 0, 2))
    check("fabs -inf", s.fpr[1][0], PINF)
    s = fresh(fpscr=FX | OX); s.fpr[2][0] = 0; run(s, X_(63, 136, 1, 0, 2, 1))
    check("fnabs. +0 and CR1", (s.fpr[1][0], s.cr, s.fpscr), (SIGN64, 0x09000000, FX | OX))
    # --- 77-84 FPSCR instructions (PAGE 457-474) ---
    s = fresh(); run(s, (63 << 26) | (0 << 23) | (0b1001 << 12) | (134 << 1))
    check("mtfsfi 0,9 -> FX OX", s.fpscr, FX | OX)
    s = fresh(); run(s, (63 << 26) | (0 << 23) | (0b0110 << 12) | (134 << 1))
    check("mtfsfi FEX/VX not from IMM", s.fpscr, 0)
    s = fresh(fpscr=VXSNAN | VX); run(s, (63 << 26) | (6 << 23) | (0b1000 << 12) | (134 << 1))
    check("mtfsfi VE with VXSNAN -> FEX, FX untouched", s.fpscr, VXSNAN | VX | FEX | VE)
    s = fresh(fpscr=VXSNAN | VX, msr=0x2032 | MSR_FE1); run(s, (63 << 26) | (6 << 23) | (0b1000 << 12) | (134 << 1))
    check("mtfsfi enabling VE traps", (s.exception, s.pc, s.fpscr & FEX), ("program", 0x700, FEX))
    s = fresh(); run(s, (63 << 26) | (1 << 23) | (0b1000 << 12) | (134 << 1))
    check("mtfsfi field1 UX: FX not altered", s.fpscr, UX)
    s = fresh(); s.fpr[4][0] = 0xFFFFFFFF; run(s, (63 << 26) | (0xFF << 17) | (4 << 11) | (711 << 1))
    check("mtfsf all ones", s.fpscr, 0xFFFFFFFF)
    s = fresh(fpscr=0x0000000F); s.fpr[4][0] = 0x60000000; run(s, (63 << 26) | (0x80 << 17) | (4 << 11) | (711 << 1) | 1)
    check("mtfsf FM=0x80 ignores FEX/VX of frB", (s.fpscr, s.cr), (0x0000000F, 0))
    s = fresh(); run(s, X_(63, 38, 7, 0, 0))
    check("mtfsb1 VXSNAN -> FX VX", s.fpscr, FX | VX | VXSNAN)
    s = fresh(); run(s, X_(63, 38, 1, 0, 0))
    check("mtfsb1 bit1 ignored", s.fpscr, 0)
    s = fresh(); run(s, X_(63, 38, 29, 0, 0))
    check("mtfsb1 NI no FX", s.fpscr, NI)
    s = fresh(fpscr=FX | VX | VXSNAN | 3); run(s, X_(63, 70, 7, 0, 0))
    check("mtfsb0 VXSNAN -> VX recomputed", s.fpscr, FX | 3)
    s = fresh(fpscr=FX | UX | ZX | VXSNAN | VX | FI); run(s, (63 << 26) | (2 << 23) | (1 << 18) | (64 << 1))
    check("mcrfs crfS=1", (s.cr, s.fpscr), (0x00D00000, FX | FI))   # field1 = UX ZX XX VXSNAN = 1101
    s = fresh(fpscr=FX | FEX | VX | OX | VXISI | VE); run(s, (63 << 26) | (0 << 23) | (0 << 18) | (64 << 1))
    check("mcrfs crfS=0 keeps FEX/VX by rule", (s.cr >> 28, s.fpscr), (0xF, FEX | VX | VXISI | VE))
    s = fresh(fpscr=0x12345678 & ~(FEX | VX)); s.fpscr = _summary(s.fpscr); want_f = s.fpscr
    e = run(s, X_(63, 583, 3, 0, 0, 1))
    check("mffs.", (s.fpr[3][0] & MASK32, s.cr >> 24 & 0xF, e.fpr_masks[(3, 0)]), (want_f, want_f >> 28, 0xFFFFFFFF00000000))
    # --- 85 reserved field ---
    s = fresh(); e = run(s, A_(63, 21, 1, 2, 3, 4))
    check("fadd C field reserved", (e.applied, s.pc), (False, 0x80003100))

    # --- D.6/D.7 exhaustive structure checks + host cross-check (sanity only, not vectors) ---
    rnd = random.Random(1)
    for _ in range(3000):
        w = rnd.getrandbits(32)
        dd = single_to_double(w)
        e8 = (w >> 23) & 0xFF
        if e8 != 255:
            check("D.6 value vs host", dd, dbl(struct.unpack(">f", struct.pack(">I", w))[0]))
        check("D.7(D.6(w)) == w", double_to_single(dd)[0], w)
    for _ in range(3000):
        a_bits, b_bits = rnd.getrandbits(64), rnd.getrandbits(64)
        for xo, fn in ((21, lambda x, y: x + y), (20, lambda x, y: x - y), (18, lambda x, y: x / y)):
            s = fresh(); s.fpr[2][0], s.fpr[3][0] = a_bits, b_bits
            ea, eb = struct.unpack(">d", a_bits.to_bytes(8, "big"))[0], struct.unpack(">d", b_bits.to_bytes(8, "big"))[0]
            if ea != ea or eb != eb:
                continue
            try:
                hv = fn(ea, eb)
            except ZeroDivisionError:
                continue
            run(s, A_(63, xo, 1, 2, 3, 0))
            if hv == hv:
                check("host cross-check xo=%d" % xo, s.fpr[1][0], dbl(hv))
        s = fresh(); s.fpr[2][0], s.fpr[3][0] = a_bits, b_bits
        ea, eb = struct.unpack(">d", a_bits.to_bytes(8, "big"))[0], struct.unpack(">d", b_bits.to_bytes(8, "big"))[0]
        if ea == ea and eb == eb and abs(ea * eb) < float("inf") and ea * eb != 0:
            run(s, A_(63, 25, 1, 2, 0, 3))
            check("host cross-check fmul", s.fpr[1][0], dbl(ea * eb))

    print("fp_B self-test: %d checks, %d failures" % (NCHK[0], len(FAIL)))
    for f_ in FAIL[:60]:
        print("  FAIL", f_)
    raise SystemExit(1 if FAIL else 0)
