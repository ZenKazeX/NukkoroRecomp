"""Independent semantic model, author A, area "fp" (scalar floating point).

Sources: docs/reference/ppc/gekko_user_manual.txt ("PAGE N" = its "===== PAGE N =====" marker),
ppc_750cl.txt ("750CL PAGE N"), and IEEE 754-1985.  No GekkoForge implementation was read.

Covered (supports() is True):
  primary 48-55   lfs lfsu lfd lfdu stfs stfsu stfd stfdu
  primary 31      lfsx lfsux lfdx lfdux stfsx stfsux stfdx stfdux
  primary 59 (A)  fdivs fsubs fadds fsqrts(illegal) fres fmuls fmsubs fmadds fnmsubs fnmadds
  primary 63 (A)  fdiv fsub fadd fsqrt(illegal) fsel fmul frsqrte fmsub fmadd fnmsub fnmadd
  primary 63 (X)  fcmpu frsp fctiw fctiwz fcmpo mtfsb1 fneg mcrfs mtfsb0 fmr mtfsfi fnabs fabs
                  mffs mtfsf

Representation: fpr[n][0] (ps0) holds the operand as a 64-bit double image; single-precision
values are held as the exact double image ("all single-precision floating-point numbers are
maintained in double precision format in the floating-point register file" PAGE 550).

Undefined behaviour is never invented.  step() computes the instruction under EVERY reading the
manual leaves open (a choice tree: each ambiguous point enumerates its readings) and merges the
outcomes bit-by-bit.  If every location is determined, step() commits; otherwise it raises
UndefinedBehaviour WITHOUT touching the state.  The exception carries
    defined   : {location: (value, defined_mask)}  (mem: tuple of (kind, addr, width, value, mask))
    undefined : {location: undefined_mask or True}
    checks    : predicates over the post-state for bounded results (fres/frsqrte estimates)
and apply_defined(state) applies the defined bits.  step_with_markers() does that and returns
the undefined map.

Architecture definitions NOT in either manual (the Gekko manual defers them to "The Programming
Environments" manual, which is not on disk) are imported as PowerPC architecture definitions:
FPSCR bit layout (bit 20 reserved), FPRF encodings, RN encodings (00 nearest, 01 zero, 10 +inf,
11 -inf), FX/FEX/VX summary rules, FR/FI non-sticky, the generated QNaN 0x7FF8_0000_0000_0000,
MSR bit positions.  Their use is confined to those definitions; any SEMANTIC rule only the
missing manual defines is treated as undefined (choice tree), see the REASON strings below.
"""
from __future__ import annotations

import os
import sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gekko_state import MASK32, MASK64, State  # noqa: E402

AREA = "fp"
M = MASK32
SIGN = 1 << 63
QUIET = 1 << 51
INF_BITS = 0x7FF0000000000000
GEN_QNAN = 0x7FF8000000000000


# ======================================================================================
# Undefined-behaviour plumbing
# ======================================================================================
class UndefinedBehaviour(Exception):
    """Raised when the manual leaves (part of) the result undefined.  See module docstring."""

    def __init__(self, what, defined=None, undefined=("all",), checks=()):
        super().__init__(what)
        self.what = what
        self.defined = dict(defined or {})
        if isinstance(undefined, dict):
            self.undefined = dict(undefined)
        else:
            self.undefined = {k: True for k in undefined}
        self.checks = tuple(checks)

    def apply_defined(self, state):
        _apply(state, self.defined)

    def check(self, state):
        return all(c(state) for c in self.checks)


class _AllUndefined(Exception):
    """Boundedly undefined / invalid form: nothing is defined."""


# ======================================================================================
# Bit helpers (big-endian numbering, bit 0 = MSB)  PAGE 338-339 field positions
# ======================================================================================
def _bits(w, a, b):
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def _exts16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def _fb(n):  # FPSCR / MSR / CR bit n as a mask
    return 1 << (31 - n)


# FPSCR layout (architecture definition; consistent with PAGE 83 "bits 15-19 are the result bits",
# PAGE 82 "FPSCR[29]" = NI, PAGE 339 CR1 = FX,FEX,VX,OX, PAGE 457 mcrfs field contents)
FX, FEX, VX, OX, UX, ZX, XX = (_fb(i) for i in range(7))
VXSNAN, VXISI, VXIDI, VXZDZ, VXIMZ, VXVC = (_fb(i) for i in range(7, 13))
FR, FI = _fb(13), _fb(14)
FPRF_MASK = 0x1F << 12           # bits 15-19
FPCC_MASK = 0x0F << 12           # bits 16-19
RSV20 = _fb(20)
VXSOFT, VXSQRT, VXCVI = _fb(21), _fb(22), _fb(23)
VE, OE, UE, ZE, XE, NI = (_fb(i) for i in range(24, 30))
VX_ALL = VXSNAN | VXISI | VXIDI | VXZDZ | VXIMZ | VXVC | VXSOFT | VXSQRT | VXCVI
EXC_BITS = OX | UX | ZX | XX | VX_ALL

FPRF_QNAN, FPRF_NINF, FPRF_NNORM, FPRF_NDEN, FPRF_NZERO = 0b10001, 0b01001, 0b01000, 0b11000, 0b10010
FPRF_PZERO, FPRF_PDEN, FPRF_PNORM, FPRF_PINF = 0b00010, 0b10100, 0b00100, 0b00101

# MSR (Table 4-4 PAGE 164-165)
MSR_POW, MSR_ILE, MSR_EE, MSR_PR, MSR_FP, MSR_ME = _fb(13), _fb(15), _fb(16), _fb(17), _fb(18), _fb(19)
MSR_FE0, MSR_SE, MSR_BE, MSR_FE1, MSR_IP, MSR_IR = _fb(20), _fb(21), _fb(22), _fb(23), _fb(25), _fb(26)
MSR_DR, MSR_PM, MSR_RI, MSR_LE = _fb(27), _fb(29), _fb(30), _fb(31)
MSR_NAMED = (MSR_POW | MSR_ILE | MSR_EE | MSR_PR | MSR_FP | MSR_ME | MSR_FE0 | MSR_SE | MSR_BE
             | MSR_FE1 | MSR_IP | MSR_IR | MSR_DR | MSR_PM | MSR_RI | MSR_LE)
# SRR1[5-9,16-31] <- MSR (PAGE 163, 166); bit 24 is a reserved bit of unstated function class
# (PAGE 164 note: only FULL-function reserved bits are saved) -> left undefined.
# CROSSCHECK FIX (crosscheck_fp.py): bit 0 is 'Reserved. Full function' (PAGE 164 Table 4-4) and 'Full function
# reserved bits are saved in SRR1 when an exception occurs' (PAGE 164 note); rfi restores MSR[0] from SRR1[0]
# (PAGE 532) -> SRR1[0] <- MSR[0] is defined (was: undefined).
SRR1_COPY = _fb(0) | (0x1F << 22) | (0xFFFF & ~_fb(24))

HID2_PSE = _fb(2)                # Table 2-6 PAGE 67

# ======================================================================================
# REASON strings (each an interpretation point, with the page that forces it)
# ======================================================================================
R_PS1_DOUBLE = ("ps1 after a double-precision result is unspecified: 'The relationship between the "
                "internal format for paired single operands and that for double-precision floating-point "
                "operands is unspecified' PAGE 83")
R_PS1_PSE0 = "HID2[PSE]=0: 'frD <- result' defines no ps1 value (PAGE 83, PAGE 396)"
R_FRSP_PS1 = "frsp with HID2[PSE]=1: 'The value in frD(ps1) is undefined' PAGE 418"
R_FSEL_PS1 = ("fsel: paired-single source -> 'frD[ps1] is undefined'; double source -> ps1 unspecified "
              "PAGE 421, PAGE 83")
R_FMR_PS1 = ("fmr: paired-single source -> 'frD[ps1] is unchanged', double source -> 'the operand is "
             "copied' (ps1 unspecified, PAGE 83); the FPR carries no visible precision tag PAGE 405")
R_SINGLE_OPND = ("single-precision instruction with an operand not representable in single precision: "
                 "'Single-precision arithmetic instructions require all operands to be single-precision' "
                 "PAGE 82 -> undefined")
R_NORESULT_FRFI = ("FR/FI when no result is produced (VE=1 invalid / ZE=1 zero divide, 'No result if "
                   "FPSCR[VE]=1' PAGE 416) are not stated")
R_SUBNAN = ("sign of a NaN propagated from frB in a subtract: 'the contents of frB participate in the "
            "operation with its sign bit (bit 0) inverted' (PAGE 422) vs NaN results deferred to PEM ch.3 "
            "(Table 2-21 note 1, PAGE 85)")
R_NI_ZERO_SIGN = "NI operand zeroing: Table 2-21 'Zero A' does not state the sign of the zero PAGE 84"
R_NI_TAG = ("NI: a value in the single-denormalized range fed to a double-precision instruction may be a "
            "single-precision operand (PAGE 82 'Double-precision arithmetic instructions may have "
            "single-precision operands') or a double normalized one; Table 2-21 zeroes only denormalized "
            "operands and the FPR has no visible precision tag PAGE 84")
R_NI_INVALID = ("NI mode: 'some IEEE invalid operations are treated in a non-IEEE conforming manner' "
                "without saying which (PAGE 82)")
R_NI_RES_SIGN = ("NI denormal result -> 'Return zero' (Table 2-22 PAGE 85; detection before rounding, "
                 "750CL PAGE 82 note 1) does not state the sign")
R_NI_RES_FLAGS = "NI denormal result -> zero: whether UX/XX are signalled and FR/FI are not stated PAGE 85"
R_TINY = ("tininess before or after rounding is not stated for IEEE mode (IEEE 754-1985 s7.4 permits "
          "either; Gekko manual defers to PEM ch.3 PAGE 82)")
R_LOSS = ("underflow 'loss of accuracy' = inexact result or denormalization loss is not stated "
          "(IEEE 754-1985 s7.4 permits either; PAGE 82 defers the execution model to PEM ch.3)")
R_OVF_FR = ("FR after a disabled overflow (result +-inf or +-max) is not stated (PAGE 395 lists FR as affected, "
            "PAGE 399 defines it only as 'incremented when rounded')")
R_ADJ_RANGE = ("enabled overflow/underflow whose exponent-adjusted result is still out of range: "
               "not stated (PAGE 82 only mentions the adjusted exponent)")
R_TRAP_COMMIT = ("FP-enabled program exception: an instruction-caused exception is 'taken without "
                 "completing the exception-causing instruction' (PAGE 161) while a stored result is "
                 "implied by 'FPRF is set ... except for invalid operation exceptions when VE=1' (PAGE 395); "
                 "whether frD/CR are written is not stated")
R_SRR1_PROG = ("SRR1 exception-specific bits (1-4, 10-15) and bit 24 are defined in PEM ch.6, not in "
               "this manual (PAGE 163, 166, 175)")
R_ALIGN = ("alignment exception for a non-word-aligned floating-point load/store (PAGE 82, PAGE 175); "
           "DSISR/SRR1 settings are in PEM ch.6 (not on disk); DAR = EA (PAGE 59)")
R_FCTIW_HIGH = "fctiw/fctiwz/mffs: 'Bits 0-31 of frD are undefined' PAGE 399, 400, 460"
R_FCTIW_FPRF = "fctiw/fctiwz: 'FPSCR[FPRF] is undefined' PAGE 399-400"
R_FCTIW_FRFI = ("fctiw/fctiwz invalid conversion: FR/FI ('set if the result is incremented/inexact') unstated "
                "PAGE 399")
R_FCTIW_NAN = ("fctiw/fctiwz of a NaN with VE=0 in IEEE mode: result word not stated (only the NI "
               "column of Table 2-22 gives 0x80000000, PAGE 85)")
R_FCTIW_SNAN_CVI = "fctiw/fctiwz of an SNaN: whether VXCVI is set in addition to VXSNAN is not stated PAGE 399"
R_NI_INT_VE = "NI fctiw invalid with VE=1: Table 2-22 'Place (0x80000000)' vs 'No result' unstated PAGE 85"
R_EST = ("estimate: 'correct to a precision of one part in 4096 ... may vary between implementations' "
         "PAGE 416, 419")
R_EST_FRFI = "fres/frsqrte: 'FR (undefined), FI (undefined)' PAGE 417, 420"
R_EST_RANGE = ("fres estimate near the single-precision overflow/underflow threshold: OX/UX/value not stated "
               "(PAGE 417 lists OX, UX as affected)")
R_STFS_ZERO = "stfs of a double denormalized value: Table 2-41 'Store zero' does not state the sign PAGE 108"
R_STFS_TINY = ("stfs of a double normalized value with exponent < 874: Table 2-41 says 'Denormalize and "
               "Store' but denormalization takes '1 to 23 clock cycles' (PAGE 109); >23 shifts unstated")
R_FCMP_RC = ("fcmpu/fcmpo with Rc=1: 'crfD should be treated as undefined' PAGE 98; 'the BF field in an instruction "
             "encoding for these cases is considered undefined' PAGE 175 -> the written CR field is unknown")
R_NI_CMP = ("NI: whether Table 2-21 operand zeroing (PAGE 84, framed for arithmetic 'Do the operation') applies to "
            "the operand of a compare/select is not stated")
R_NI_UE = ("NI denormal result with FPSCR[UE]=1: Table 2-22 NI 'Return zero' (PAGE 85) vs the enabled-underflow "
           "adjusted result (PAGE 82 'adjusted exponent value ... when the corresponding exception enable bit is one')")
R_IMZ_NAN = ("fmadd family with frB a NaN and frA*frC = inf*0: whether VXIMZ is detected in addition to the NaN "
             "propagation is not stated (Table 2-21 note 1 defers priority to PEM ch.3, PAGE 85)")
R_MTFS_FEX = ("FPSCR-altering instruction with MSR[FE0|FE1] != 0 leaving FEX = 1 when it was already 1: whether a "
              "program exception is taken is not stated (PAGE 166 speaks of IEEE exceptions being taken)")
R_EXC_DISAGREE = "readings of the manual disagree on whether an exception is taken (see enumerated reasons PAGE 82-85)"
R_INDEXED_RC = "indexed FP load/store with Rc=1: 'causes CR0 to be set to an undefined value' PAGE 106"
R_RSV20 = "FPSCR bit 20 is reserved (architecture definition; fields PAGE 473); writing it is undefined"
R_LE = "floating-point load/store with MSR[LE]=1: little-endian data access is outside this model (PAGE 106)"
R_ILLEGAL = ("fsqrt/fsqrts: 'It does not support the optional fsqrt, fsqrts' -> illegal instruction "
             "program exception PAGE 87, 175")
R_FPU = "floating-point unavailable exception MSR[FP]=0 PAGE 175"


# ======================================================================================
# Double image helpers
# ======================================================================================
def _split(x):
    return (x >> 63) & 1, (x >> 52) & 0x7FF, x & ((1 << 52) - 1)


def _is_nan(x):
    _, e, f = _split(x)
    return e == 0x7FF and f != 0


def _is_snan(x):
    _, e, f = _split(x)
    return e == 0x7FF and f != 0 and not (f & QUIET)


def _pow2(k):
    return Fraction(1 << k) if k >= 0 else Fraction(1, 1 << -k)


def _val(x):
    """('nan'|'inf'|'zero'|'fin', sign, magnitude Fraction)"""
    s, e, f = _split(x)
    if e == 0x7FF:
        return ("nan" if f else "inf", s, None)
    if e == 0:
        if f == 0:
            return ("zero", s, Fraction(0))
        return ("fin", s, Fraction(f) * _pow2(-1074))
    return ("fin", s, Fraction((1 << 52) | f) * _pow2(e - 1075))


def _flog2(q):
    n, d = q.numerator, q.denominator
    e = n.bit_length() - d.bit_length()
    if _pow2(e) > q:
        e -= 1
    return e


def _enc(sign, val):
    """Exact double image of sign*val (val must be representable)."""
    if val == 0:
        return sign << 63
    e = _flog2(val)
    if e >= -1022:
        assert e <= 1023
        frac = (val / _pow2(e) - 1) * (1 << 52)
        assert frac.denominator == 1
        return (sign << 63) | ((e + 1023) << 52) | int(frac)
    fr = val * _pow2(1074)
    assert fr.denominator == 1
    return (sign << 63) | int(fr)


MAX_D = Fraction((1 << 53) - 1) * _pow2(1023 - 52)
MAX_S = Fraction((1 << 24) - 1) * _pow2(127 - 23)


def _single_repr(x):
    """True iff the double image is the exact image of some single-precision value (D.6 image)."""
    s, e, f = _split(x)
    low29 = f & ((1 << 29) - 1)
    if e == 0x7FF:
        return low29 == 0
    if e == 0:
        return f == 0
    E = e - 1023
    if E > 127 or E < -149:
        return False
    if E >= -126:
        return low29 == 0
    k = E + 149                      # fraction bits that stay above 2^-149
    return f & ((1 << (52 - k)) - 1) == 0


def _fprf(x, single):
    s, e, f = _split(x)
    if e == 0x7FF:
        return FPRF_QNAN if f else (FPRF_NINF if s else FPRF_PINF)
    if e == 0 and f == 0:
        return FPRF_NZERO if s else FPRF_PZERO
    denorm = (e == 0) if not single else (e - 1023 < -126)
    if denorm:
        return FPRF_NDEN if s else FPRF_PDEN
    return FPRF_NNORM if s else FPRF_PNORM


def _d6_load_single(word):
    """Single word -> double image.  lfs 'converted to floating-point double-precision' PAGE 434-437
    (D.6: bit mapping for zero/inf/NaN/normal, exact normalization for denormals)."""
    e = (word >> 23) & 0xFF
    frac = word & 0x7FFFFF
    s = word >> 31
    if e == 0 and frac != 0:
        return _enc(s, Fraction(frac) * _pow2(-149))
    w1 = (word >> 30) & 1
    fill = (0 if w1 else 0b111) if 0 < e < 255 else (0b111 if w1 else 0)
    return ((word >> 30) << 62) | (fill << 59) | ((word & 0x3FFFFFFF) << 29)


def _d7_store_single(env, x):
    """Double image -> single word, Table 2-41 'Store Floating-Point Single Behavior' PAGE 108-109.
    Returns (word, defined_mask)."""
    s, e, f = _split(x)
    if e == 0 and f != 0:
        # 'Double Denormalized: Store zero' PAGE 108
        sign = env.choose(R_STFS_ZERO, [s, 1 - s])     # CROSSCHECK FIX: either sign (was: [s, +])
        return sign << 31, M
    if e > 896 or e == 0:
        # 'Store' (no denormalization): WORD <- FRS[0-1] || FRS[5-34]  (PAGE 108, D.7 referenced PAGE 551)
        return (((x >> 62) << 30) | ((x >> 29) & 0x3FFFFFFF)) & M, M
    if e >= 874:
        # 'If (exp <= 896) then Denormalize and Store' PAGE 108; 'shifting the mantissa one bit at a
        # time ... 1 to 23 clock cycles' PAGE 109 -> truncating right shift.
        E = e - 1023
        fr = ((1 << 52) | f) >> (-126 - E)
        return ((s << 31) | ((fr >> 29) & 0x7FFFFF)) & M, M
    env.note(R_STFS_TINY)
    return s << 31, 0x80000000


# ======================================================================================
# IEEE rounding (exact, Fraction based)
# ======================================================================================
def _rnd(x, sign, rn):
    """Integer rounding of the magnitude x >= 0 under RN (sign-aware for directed modes)."""
    fl = x.numerator // x.denominator
    rem = x - fl
    if rem == 0:
        return fl
    if rn == 0:                          # round to nearest, ties to even
        two = rem * 2
        if two > 1:
            return fl + 1
        if two < 1:
            return fl
        return fl + (fl & 1)
    if rn == 1:                          # toward zero
        return fl
    if rn == 2:                          # toward +infinity
        return fl + 1 if sign == 0 else fl
    return fl + 1 if sign == 1 else fl   # toward -infinity


def _round(mag, sign, single, rn):
    P, EMIN, EMAX = (24, -126, 127) if single else (53, -1022, 1023)
    e = _flog2(mag)
    qu = _pow2(e - P + 1)
    vu = _rnd(mag / qu, sign, rn) * qu               # unbounded-exponent rounding
    qb = _pow2(max(e, EMIN) - P + 1)
    vb = _rnd(mag / qb, sign, rn) * qb               # rounding in the real format (denormal grid)
    lim = _pow2(EMIN)
    return {"val": vb, "unb": vu, "overflow": vu >= _pow2(EMAX + 1), "tiny_before": mag < lim,
            "tiny_after": vu < lim, "inexact": vb != mag, "incr": vb > mag}


# ======================================================================================
# Execution environment
# ======================================================================================
class _Ctx:
    def __init__(self, script):
        self.script = script
        self.trace = []

    def choose(self, options):
        k = len(self.trace)
        i = self.script[k] if k < len(self.script) else 0
        self.trace.append(len(options))
        return options[i]


class _Env:
    def __init__(self, st, word, ctx):
        self.st = st
        self.w = word
        self.ctx = ctx
        self.cia = st.pc & M
        self.fpscr = st.fpscr & M
        self.fpscr_before = self.fpscr
        self.fpscr_undef = 0
        self.fpscr_touched = False
        self.cr = st.cr & M
        self.cr_undef = 0
        self.cr_touched = False
        self.vals = {}
        self.masks = {}
        self.mem_ops = []
        self.checks = []
        self.reasons = []
        self.signaled = 0
        self.done = False

    # -- interpretation points
    def choose(self, reason, options):
        self.reasons.append(reason)
        return self.ctx.choose(options)

    def note(self, reason):
        self.reasons.append(reason)

    # -- register writes
    def write_ps(self, i, slot, value, undef=0, reason=None):
        loc = f"fpr[{i}].ps{slot}"
        self.vals[loc] = value & MASK64
        self.masks[loc] = MASK64 & ~undef
        if undef and reason:
            self.note(reason)

    def ps1_unspecified(self, i, reason):
        self.write_ps(i, 1, self.st.fpr[i][1], undef=MASK64, reason=reason)

    def put_result(self, D, bits, single, mask=MASK64):
        """Write a result to frD; ps1 per HID2[PSE] (PAGE 83, 396: 'placed in both frD(ps0) and frD(ps1)')."""
        self.write_ps(D, 0, bits, undef=MASK64 & ~mask)
        if single and (self.st.hid2 & HID2_PSE):
            self.write_ps(D, 1, bits, undef=MASK64 & ~mask)
        elif single:
            self.ps1_unspecified(D, R_PS1_PSE0)
        else:
            self.ps1_unspecified(D, R_PS1_DOUBLE)
        if mask == MASK64:
            self.set_fprf(_fprf(bits, single))
        else:
            self.undef_fprf()

    # -- FPSCR helpers
    def set_exc(self, bits):
        """Sticky exception bits; FX is set when an exception bit changes from 0 to 1 (PAGE 473)."""
        self.signaled |= bits
        new = bits & ~self.fpscr
        if new:
            self.fpscr |= new | FX

    def set_fprf(self, code):
        self.fpscr = (self.fpscr & ~FPRF_MASK) | (code << 12)
        self.fpscr_undef &= ~FPRF_MASK

    def undef_fprf(self, reason=None):
        self.fpscr_undef |= FPRF_MASK
        if reason:
            self.note(reason)

    def set_fr_fi(self, fr, fi):
        self.fpscr = (self.fpscr & ~(FR | FI)) | (FR if fr else 0) | (FI if fi else 0)
        self.fpscr_undef &= ~(FR | FI)

    def undef_fr_fi(self, reason):
        self.fpscr_undef |= FR | FI
        self.note(reason)

    def summaries(self):
        f = self.fpscr
        f = (f | VX) if (f & VX_ALL) else (f & ~VX)
        fex = ((f & VX and f & VE) or (f & OX and f & OE) or (f & UX and f & UE)
               or (f & ZX and f & ZE) or (f & XX and f & XE))
        self.fpscr = (f | FEX) if fex else (f & ~FEX)

    # -- completion
    def set_cr1(self):
        # 'For floating-point instructions, CR bits 4-7 are set to reflect FX, FEX, VX, OX' PAGE 339
        self.cr = (self.cr & ~0x0F000000) | (((self.fpscr >> 28) & 0xF) << 24)
        self.cr_touched = True

    def finish_fp(self, rc, fpscr_move=False):
        """End of an instruction that may alter the FPSCR."""
        self.summaries()
        self.fpscr_touched = True
        if rc:
            self.set_cr1()
        self.maybe_trap(fpscr_move)

    def finish_plain(self, rc):
        """End of an instruction that does not alter the FPSCR (moves, fsel, mffs) PAGE 100, 394."""
        if rc:
            self.set_cr1()

    def maybe_trap(self, fpscr_move=False):
        # 'IEEE floating-point enabled exceptions ... are ignored when both MSR[FE0] and MSR[FE1] are
        # cleared. If either bit is set, all IEEE enabled floating-point exceptions are taken and cause a
        # program exception.' PAGE 166; precise in Gekko PAGE 175.
        msr = self.st.msr
        if not (msr & (MSR_FE0 | MSR_FE1)) or not (self.fpscr & FEX):
            return
        if fpscr_move and (self.fpscr_before & FEX):
            # CROSSCHECK FIX: an FPSCR move (mtfsb0/mtfsb1/mtfsf/mtfsfi/mcrfs) that leaves FEX = 1 when it was
            # already 1 - whether a program exception is taken is not stated (was: taken iff an enabled
            # exception bit changed 0 -> 1).  FEX 0 -> 1 is taken (below).
            raise _AllUndefined(R_MTFS_FEX)
        en = ((VX_ALL if self.fpscr & VE else 0) | (OX if self.fpscr & OE else 0)
              | (UX if self.fpscr & UE else 0) | (ZX if self.fpscr & ZE else 0) | (XX if self.fpscr & XE else 0))
        if (self.fpscr_before & FEX) and not (self.signaled & en):
            return
        other = [k for k in self.vals if k.startswith("fpr[")] or self.cr_touched
        if other:
            keep = self.choose(R_TRAP_COMMIT, [True, False])
            if not keep:
                for k in [k for k in self.vals if k.startswith("fpr[")]:
                    del self.vals[k]
                    self.masks.pop(k, None)
                self.cr, self.cr_undef, self.cr_touched = self.st.cr & M, 0, False
        self.enter("program", 0x700, R_SRR1_PROG)

    def enter(self, name, vector, reason):
        """Exception entry: SRR0 = the excepting instruction (PAGE 161, 163), SRR1[5-9,16-31] <- MSR
        (PAGE 166), MSR per Table 4-6 (PAGE 168), vector per MSR[IP] (PAGE 166 step 5)."""
        msr = self.st.msr & M
        self.vals["srr0"] = self.cia
        self.vals["srr1"] = msr & SRR1_COPY
        self.masks["srr1"] = SRR1_COPY
        new = msr & (MSR_ILE | MSR_ME | MSR_IP)
        if msr & MSR_ILE:
            new |= MSR_LE
        self.vals["msr"] = new
        self.masks["msr"] = MSR_NAMED
        self.vals["pc"] = ((0xFFF00000 if msr & MSR_IP else 0) | vector) & M
        self.vals["exception"] = name
        self.done = True
        self.note(reason)

    def finalize(self):
        if not self.done:
            self.vals["pc"] = (self.cia + 4) & M     # sequential instruction PAGE 342
            self.vals["exception"] = None
        if self.fpscr_touched:
            self.vals["fpscr"] = self.fpscr & M
            self.masks["fpscr"] = M & ~self.fpscr_undef
        if self.cr_touched or self.cr_undef:
            self.vals["cr"] = self.cr & M
            self.masks["cr"] = M & ~self.cr_undef
        if self.mem_ops:
            self.vals["mem"] = tuple(tuple(o) for o in self.mem_ops)


def _fp_avail(env):
    # 'A floating-point unavailable exception occurs when ... an attempt is made to execute a
    # floating-point instruction (including floating-point load, store, or move instructions), and ...
    # MSR[FP] = 0' PAGE 175 (vector 0x800).
    if env.st.msr & MSR_FP:
        return True
    env.enter("fp_unavailable", 0x800, R_FPU + "; " + R_SRR1_PROG)
    return False


def _rsv(w, fields, name, page):
    for a, b in fields:
        if _bits(w, a, b):
            raise _AllUndefined(f"{name}: reserved bits {a}-{b} not zero -> boundedly undefined "
                                f"(PAGE 87 s2.3.1.1; format PAGE {page})")


# ======================================================================================
# Result delivery (one rounding of the exact result)
# ======================================================================================
def _deliver(env, sign, mag, single, rn, xx=True):
    """Round exact nonzero finite sign*mag into the target precision.  Updates OX/UX/XX/FR/FI (not
    FPRF).  Returns (bits, defined_mask).  'The result is rounded to (single|double)-precision under
    control of the floating-point rounding control field RN' PAGE 395-423; IEEE 754-1985 s4, s7."""
    ADJ = 192 if single else 1536                     # IEEE 754-1985 s7.3/7.4 exponent adjust
    f = env.fpscr
    r = _round(mag, sign, single, rn)
    if r["overflow"]:
        env.set_exc(OX)
        if f & OE:
            r2 = _round(mag / _pow2(ADJ), sign, single, rn)
            if r2["overflow"] or r2["tiny_before"] or r2["tiny_after"]:
                env.undef_fr_fi(R_ADJ_RANGE)
                if xx and env.choose(R_ADJ_RANGE, [False, True]):
                    env.set_exc(XX)
                return sign << 63, SIGN
            if r2["inexact"] and xx:
                env.set_exc(XX)
            env.set_fr_fi(r2["incr"], r2["inexact"])
            return _enc(sign, r2["val"]), MASK64
        if xx:
            env.set_exc(XX)                           # IEEE 754-1985 s7.5 (overflow without trap)
        big = MAX_S if single else MAX_D
        to_inf = rn == 0 or (rn == 2 and sign == 0) or (rn == 3 and sign == 1)
        bits = (sign << 63) | INF_BITS if to_inf else _enc(sign, big)
        env.fpscr |= FI
        env.fpscr_undef |= FR
        env.note(R_OVF_FR)
        return bits, MASK64
    if f & NI:
        # 'Denormalized: Return zero' Table 2-22 PAGE 85; 'The detection of a denorm result occurs prior
        # to rounding' 750CL PAGE 82 note 1.
        if r["tiny_before"] and (f & UE) and env.choose(R_NI_UE, [False, True]):
            tiny = True                                # CROSSCHECK FIX: UE=1 adjusted-result reading (R_NI_UE)
        elif r["tiny_before"]:
            zsign = env.choose(R_NI_RES_SIGN, [sign, 1 - sign])   # CROSSCHECK FIX: either sign (was: [sign, +])
            if env.choose(R_NI_RES_FLAGS, [False, True]):
                env.set_exc(UX)
            if xx and env.choose(R_NI_RES_FLAGS, [False, True]):
                env.set_exc(XX)
            env.undef_fr_fi(R_NI_RES_FLAGS)
            return zsign << 63, MASK64
        else:
            tiny = False
    else:
        tb, ta = r["tiny_before"], r["tiny_after"]
        tiny = tb if tb == ta else env.choose(R_TINY, [tb, ta])
    if tiny:
        if f & UE:
            env.set_exc(UX)
            r2 = _round(mag * _pow2(ADJ), sign, single, rn)
            if r2["overflow"] or r2["tiny_before"] or r2["tiny_after"]:
                env.undef_fr_fi(R_ADJ_RANGE)
                if xx and env.choose(R_ADJ_RANGE, [False, True]):
                    env.set_exc(XX)
                return sign << 63, SIGN
            if r2["inexact"] and xx:
                env.set_exc(XX)
            env.set_fr_fi(r2["incr"], r2["inexact"])
            return _enc(sign, r2["val"]), MASK64
        li = r["inexact"]
        ld = r["val"] != r["unb"]
        loss = li if li == ld else env.choose(R_LOSS, [li, ld])
        if loss:
            env.set_exc(UX)
    if r["inexact"] and xx:
        env.set_exc(XX)
    env.set_fr_fi(r["incr"], r["inexact"])
    return _enc(sign, r["val"]), MASK64


def _ni_operand(env, bits, v, single_instr, cmp_choice=None):
    """Table 2-21 (PAGE 84): IEEE mode 'Normalize' (the mathematical value is kept), NI mode 'Zero'.
    cmp_choice: a list shared by the operands of one compare/select; zeroing is then an interpretation point
    (R_NI_CMP), chosen once per instruction."""
    if v[0] != "fin":
        return v
    s, e, _ = _split(bits)
    if e == 0:
        zero = True                                  # double denormalized
    elif e <= 896 and _single_repr(bits):             # exact single denormalized value
        zero = True if single_instr else env.choose(R_NI_TAG, [False, True])
    else:
        return v
    if zero and cmp_choice is not None:
        if not cmp_choice:
            cmp_choice.append(env.choose(R_NI_CMP, [True, False]))
        zero = cmp_choice[0]
    if not zero:
        return v
    # CROSSCHECK FIX: 'Zero A' (PAGE 84) states no sign -> either sign is a reading (was: [s, +])
    return ("zero", env.choose(R_NI_ZERO_SIGN, [s, 1 - s]), Fraction(0))


def _invalid(env, bit, D, single):
    """Invalid operation other than SNaN propagation."""
    f = env.fpscr
    if f & NI:
        if env.choose(R_NI_INVALID, [True, False]):
            env.set_exc(bit)
        env.write_ps(D, 0, env.st.fpr[D][0], undef=MASK64, reason=R_NI_INVALID)
        env.write_ps(D, 1, env.st.fpr[D][1], undef=MASK64)
        env.undef_fprf()
        env.undef_fr_fi(R_NI_INVALID)
        return
    env.set_exc(bit)
    if f & VE:
        # 'No result if FPSCR[VE] = 1' PAGE 416; FPRF 'except for invalid operation exceptions when VE=1'
        env.undef_fr_fi(R_NORESULT_FRFI)
        return
    # generated QNaN; 'QNaNs that are generated as the result of a disabled invalid operation exception
    # have a sign bit of zero' PAGE 412
    env.put_result(D, GEN_QNAN, single)
    env.set_fr_fi(0, 0)


# ======================================================================================
# Exact operations  (IEEE 754-1985 s5, s6.3 zero signs, s7.1 invalid, s7.2 divide by zero)
# ======================================================================================
def _signed(v):
    return -v[2] if v[1] else v[2]


def _op_add(x, y, rn):
    if x[0] == "inf" and y[0] == "inf":
        return ("invalid", VXISI) if x[1] != y[1] else ("inf", x[1])
    if x[0] == "inf":
        return ("inf", x[1])
    if y[0] == "inf":
        return ("inf", y[1])
    S = _signed(x) + _signed(y)
    if S == 0:
        if x[0] == "zero" and y[0] == "zero" and x[1] == y[1]:
            return ("zero", x[1])
        return ("zero", 1 if rn == 3 else 0)
    return ("fin", 1 if S < 0 else 0, abs(S))


def _op_mul(x, y):
    s = x[1] ^ y[1]
    if (x[0] == "zero" and y[0] == "inf") or (x[0] == "inf" and y[0] == "zero"):
        return ("invalid", VXIMZ)
    if x[0] == "inf" or y[0] == "inf":
        return ("inf", s)
    if x[0] == "zero" or y[0] == "zero":
        return ("zero", s)
    return ("fin", s, x[2] * y[2])


def _op_div(x, y):
    s = x[1] ^ y[1]
    if x[0] == "zero" and y[0] == "zero":
        return ("invalid", VXZDZ)
    if x[0] == "inf" and y[0] == "inf":
        return ("invalid", VXIDI)
    if x[0] == "inf":
        return ("inf", s)
    if y[0] == "inf":
        return ("zero", s)
    if y[0] == "zero":
        return ("zdiv", s)
    if x[0] == "zero":
        return ("zero", s)
    return ("fin", s, x[2] / y[2])


def _neg(v):
    return (v[0], 1 - v[1], v[2])


# name -> (operand letters in priority order, reserved fields, page)
_ARITH = {
    "add": ("AB", [(21, 25)]), "sub": ("AB", [(21, 25)]), "div": ("AB", [(21, 25)]),
    "mul": ("AC", [(16, 20)]),
    "madd": ("ABC", []), "msub": ("ABC", []), "nmadd": ("ABC", []), "nmsub": ("ABC", []),
}


def _h_arith(env, w, op, single, name, page):
    st = env.st
    D, A, B, C, rc = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20), _bits(w, 21, 25), w & 1
    letters, rsv = _ARITH[op]
    _rsv(w, rsv, name, page)
    if not _fp_avail(env):
        return
    reg = {"A": A, "B": B, "C": C}
    bits = [st.fpr[reg[L]][0] for L in letters]
    if single and not all(_single_repr(b) for b in bits):
        raise _AllUndefined(R_SINGLE_OPND)
    f = env.fpscr
    rn = f & 3
    # ---- NaN operands: Table 2-21 rows 'Operand A NaN / B NaN / C NaN -> QNaN' (both modes), priority by
    # row order A, B, C (PAGE 84-85); SNaN -> VXSNAN; 'Return QNaN' Table 2-22 PAGE 85.
    nan_i = next((i for i, b in enumerate(bits) if _is_nan(b)), None)
    if nan_i is not None:
        snan = any(_is_snan(b) for b in bits)
        if snan:
            env.set_exc(VXSNAN)
        imz = False
        if len(letters) == 3 and not _is_nan(bits[0]) and not _is_nan(bits[2]):
            # CROSSCHECK FIX (R_IMZ_NAN): frB NaN with frA*frC = inf*0 -> VXIMZ detection is enumerated
            va, vc = _val(bits[0]), _val(bits[2])
            if f & NI:
                va, vc = _ni_operand(env, bits[0], va, single), _ni_operand(env, bits[2], vc, single)
            if {va[0], vc[0]} == {"inf", "zero"} and env.choose(R_IMZ_NAN, [False, True]):
                env.set_exc(VXIMZ)
                imz = True
        if (snan or imz) and (f & VE):
            env.undef_fr_fi(R_NORESULT_FRFI)          # 'No result if FPSCR[VE] = 1' PAGE 416
        else:
            res = bits[nan_i] | QUIET
            # CROSSCHECK FIX: fnmsub also subtracts frB (PAGE 414) -> same R_SUBNAN point (was: sub, msub only)
            if letters[nan_i] == "B" and op in ("sub", "msub", "nmsub"):
                if env.choose(R_SUBNAN, [False, True]):
                    res ^= SIGN
            # fnmadd/fnmsub: 'QNaNs propagate with no effect on their sign bit' / 'SNaNs ... retain the
            # sign bit of the SNaN' PAGE 412, 414
            env.put_result(D, res, single)
            env.set_fr_fi(0, 0)
        env.finish_fp(rc)
        return
    vals = [_val(b) for b in bits]
    if f & NI:
        vals = [_ni_operand(env, b, v, single) for b, v in zip(bits, vals)]
    x = dict(zip(letters, vals))
    if op == "add":                                   # PAGE 395-396
        out = _op_add(x["A"], x["B"], rn)
    elif op == "sub":                                 # 'frB participate ... with its sign bit inverted' PAGE 422
        out = _op_add(x["A"], _neg(x["B"]), rn)
    elif op == "mul":                                 # PAGE 408-409
        out = _op_mul(x["A"], x["C"])
    elif op == "div":                                 # PAGE 401-402
        out = _op_div(x["A"], x["B"])
    else:
        # 'These instructions combine multiply and add operations without an intermediate rounding
        # operation' PAGE 97; frD <- (frA*frC) +- frB PAGE 403-415
        p = _op_mul(x["A"], x["C"])
        if p[0] == "invalid":
            out = p
        else:
            pv = (p[0], p[1], p[2] if p[0] == "fin" else Fraction(0))
            b = x["B"] if op in ("madd", "nmadd") else _neg(x["B"])
            out = _op_add(pv, b, rn)
    neg = op in ("nmadd", "nmsub")                    # 'rounded ... then negated' PAGE 412-415
    kind = out[0]
    if kind == "invalid":
        _invalid(env, out[1], D, single)
    elif kind == "zdiv":
        env.set_exc(ZX)
        if f & ZE:
            env.undef_fr_fi(R_NORESULT_FRFI)          # 'No result if FPSCR[ZE] = 1' PAGE 416
        else:
            env.put_result(D, (out[1] << 63) | INF_BITS, single)
            env.set_fr_fi(0, 0)
    elif kind in ("inf", "zero"):
        s = out[1] ^ (1 if neg else 0)
        env.put_result(D, (s << 63) | (INF_BITS if kind == "inf" else 0), single)
        env.set_fr_fi(0, 0)
    else:
        res, mask = _deliver(env, out[1], out[2], single, rn)
        if neg:
            res ^= SIGN
        env.put_result(D, res, single, mask)
    env.finish_fp(rc)


# ======================================================================================
# fres / frsqrte (estimates), frsp, fctiw, fsel, fsqrt
# ======================================================================================
def _h_fres(env, w):
    st = env.st
    D, B, rc = _bits(w, 6, 10), _bits(w, 16, 20), w & 1
    _rsv(w, [(11, 15), (21, 25)], "fres", 416)
    if not _fp_avail(env):
        return
    b = st.fpr[B][0]
    if not _single_repr(b):
        raise _AllUndefined(R_SINGLE_OPND + " (fres is single-precision, PAGE 83)")
    f = env.fpscr
    env.undef_fr_fi(R_EST_FRFI)
    if _is_nan(b):
        # 'SNaN -> QNaN** VXSNAN', 'QNaN -> QNaN None', '** No result if FPSCR[VE] = 1' PAGE 416
        if _is_snan(b):
            env.set_exc(VXSNAN)
        if not (_is_snan(b) and f & VE):
            env.put_result(D, b | QUIET, True)
        env.finish_fp(rc)
        return
    v = _val(b)
    if f & NI:
        v = _ni_operand(env, b, v, True)
    kind, s, m = v
    if kind == "zero":
        env.set_exc(ZX)                               # '-0 -> -inf * ZX', '+0 -> +inf * ZX' PAGE 416
        if not (f & ZE):
            env.put_result(D, (s << 63) | INF_BITS, True)
    elif kind == "inf":
        env.put_result(D, s << 63, True)              # '+-inf -> +-0 None' PAGE 416
    else:
        r = 1 / m
        lo, hi = r * Fraction(4095, 4096), r * Fraction(4097, 4096)   # PAGE 416 bound
        pse = bool(st.hid2 & HID2_PSE)
        min_norm = _pow2(-126)
        if hi <= MAX_S and lo >= min_norm:
            # CROSSCHECK FIX: |estimate - 1/x| <= |1/x|/4096 (PAGE 416) forces the sign of 1/x -> sign bit defined
            env.write_ps(D, 0, s << 63, undef=MASK64 & ~SIGN, reason=R_EST)
            if pse:
                env.write_ps(D, 1, s << 63, undef=MASK64 & ~SIGN)
            else:
                env.ps1_unspecified(D, R_PS1_PSE0)
            env.set_fprf(FPRF_NNORM if s else FPRF_PNORM)

            def check(state, D=D, s=s, lo=lo, hi=hi, pse=pse):
                x = state.fpr[D][0]
                if not _single_repr(x):
                    return False
                k, vs, vm = _val(x)
                if k != "fin" or vs != s or not (lo <= vm <= hi):
                    return False
                return (not pse) or state.fpr[D][1] == x
            env.checks.append(check)
        else:
            if lo > MAX_S:
                env.set_exc(OX)
            elif hi > MAX_S and env.choose(R_EST_RANGE, [False, True]):
                env.set_exc(OX)
            if lo < min_norm:
                if (f & UE) and hi < min_norm:
                    env.set_exc(UX)
                elif env.choose(R_EST_RANGE, [False, True]):
                    env.set_exc(UX)
            env.put_result(D, s << 63, True, SIGN)
            env.note(R_EST_RANGE)
    env.finish_fp(rc)


def _h_frsqrte(env, w):
    st = env.st
    D, B, rc = _bits(w, 6, 10), _bits(w, 16, 20), w & 1
    _rsv(w, [(11, 15), (21, 25)], "frsqrte", 419)
    if not _fp_avail(env):
        return
    b = st.fpr[B][0]
    f = env.fpscr
    env.undef_fr_fi(R_EST_FRFI)
    if _is_nan(b):
        if _is_snan(b):
            env.set_exc(VXSNAN)                       # 'SNaN -> QNaN** VXSNAN' PAGE 419
        if not (_is_snan(b) and f & VE):
            env.put_result(D, b | QUIET, False)
        env.finish_fp(rc)
        return
    v = _val(b)
    if f & NI:
        v = _ni_operand(env, b, v, False)
    kind, s, m = v
    if kind in ("inf", "fin") and s == 1:
        _invalid(env, VXSQRT, D, False)               # '-inf -> QNaN** VXSQRT', '<0 -> QNaN** VXSQRT'
        env.fpscr_undef |= FR | FI
    elif kind == "zero":
        env.set_exc(ZX)                               # '+-0 -> +-inf * ZX' PAGE 419
        if not (f & ZE):
            env.put_result(D, (s << 63) | INF_BITS, False)
    elif kind == "inf":
        env.put_result(D, 0, False)                   # '+inf -> +0' PAGE 419
    else:
        # CROSSCHECK FIX: the estimate of 1/sqrt(x) > 0 is positive (PAGE 419 bound) -> sign bit defined (0)
        env.write_ps(D, 0, 0, undef=MASK64 & ~SIGN, reason=R_EST)
        env.ps1_unspecified(D, R_PS1_DOUBLE)
        env.set_fprf(FPRF_PNORM)
        lo2, hi2 = Fraction(4095, 4096) ** 2, Fraction(4097, 4096) ** 2

        def check(state, D=D, m=m):
            k, vs, vm = _val(state.fpr[D][0])
            return k == "fin" and vs == 0 and lo2 <= vm * vm * m <= hi2
        env.checks.append(check)
    env.finish_fp(rc)


def _h_frsp(env, w):
    st = env.st
    D, B, rc = _bits(w, 6, 10), _bits(w, 16, 20), w & 1
    _rsv(w, [(11, 15)], "frsp", 418)
    if not _fp_avail(env):
        return
    b = st.fpr[B][0]
    f = env.fpscr
    pse = bool(st.hid2 & HID2_PSE)

    def put(bits, mask=MASK64):
        # 'rounded to single-precision using the rounding mode specified by FPSCR[RN] and placed into
        # frD(ps0). The value in frD(ps1) is undefined.' PAGE 418
        env.write_ps(D, 0, bits, undef=MASK64 & ~mask)
        env.ps1_unspecified(D, R_FRSP_PS1 if pse else R_PS1_DOUBLE)
        if mask == MASK64:
            env.set_fprf(_fprf(bits, True))
        else:
            env.undef_fprf()

    if _is_nan(b):
        if _is_snan(b):
            env.set_exc(VXSNAN)
        if _is_snan(b) and f & VE:
            env.undef_fr_fi(R_NORESULT_FRFI)
        else:
            # 'Single QNaN, SNaN: Return QNaN' Table 2-22 PAGE 85; single image keeps FRS[0-34]
            put((b | QUIET) & ~((1 << 29) - 1))
            env.set_fr_fi(0, 0)
        env.finish_fp(rc)
        return
    v = _val(b)
    if f & NI:
        v = _ni_operand(env, b, v, False)
    kind, s, m = v
    if kind == "inf":
        put((s << 63) | INF_BITS)
        env.set_fr_fi(0, 0)
    elif kind == "zero":
        put(s << 63)
        env.set_fr_fi(0, 0)
    else:
        bits, mask = _deliver(env, s, m, True, f & 3)
        put(bits, mask)
    env.finish_fp(rc)


def _h_fctiw(env, w, toward_zero):
    st = env.st
    D, B, rc = _bits(w, 6, 10), _bits(w, 16, 20), w & 1
    _rsv(w, [(11, 15)], "fctiwz" if toward_zero else "fctiw", 400 if toward_zero else 399)
    if not _fp_avail(env):
        return
    b = st.fpr[B][0]
    f = env.fpscr
    ni = f & NI
    rn = 1 if toward_zero else (f & 3)                 # PAGE 399 'FPSCR[RN]'; PAGE 400 'round toward zero'
    nan = _is_nan(b)
    if nan:
        if _is_snan(b):
            env.set_exc(VXSNAN)
            if env.choose(R_FCTIW_SNAN_CVI, [True, False]):
                env.set_exc(VXCVI)
        else:
            env.set_exc(VXCVI)                         # IEEE 754-1985 s7.1 (7)
        inv_sign = b >> 63
    else:
        v = _val(b)
        if ni:
            v = _ni_operand(env, b, v, False)
        kind, s, m = v
        if kind == "inf":
            env.set_exc(VXCVI)
            inv_sign = s
        else:
            if kind == "zero":
                n, inexact, incr = 0, False, False
            else:
                n = _rnd(m, s, rn)
                inexact, incr = n != m, n > m
            N = -n if s else n
            if N > 0x7FFFFFFF or N < -0x80000000:
                env.set_exc(VXCVI)
                inv_sign = s
            else:
                # '... placed in bits 32-63 of frD. Bits 0-31 of frD are undefined.' 'FPSCR[FR] is set if the
                # result is incremented when rounded. FPSCR[FI] is set if the result is inexact.' PAGE 399
                if inexact:
                    env.set_exc(XX)
                env.set_fr_fi(incr, inexact)
                env.write_ps(D, 0, N & M, undef=0xFFFFFFFF00000000, reason=R_FCTIW_HIGH)
                env.ps1_unspecified(D, R_PS1_DOUBLE)
                env.undef_fprf(R_FCTIW_FPRF)
                env.finish_fp(rc)
                return
    env.undef_fr_fi(R_FCTIW_FRFI)
    if f & VE:
        if ni:
            env.write_ps(D, 0, st.fpr[D][0], undef=MASK64, reason=R_NI_INT_VE)
            env.ps1_unspecified(D, R_PS1_DOUBLE)
            env.undef_fprf(R_NI_INT_VE)
        # IEEE mode: no result; FPRF defined only 'for trap-enabled invalid operation exceptions' PAGE 399
    else:
        low_undef = 0
        if ni:
            low = 0x80000000                           # Table 2-22 NI 'Place (0x80000000)' 750CL PAGE 82
        elif nan:
            low, low_undef = 0, M
            env.note(R_FCTIW_NAN)
        else:
            # 'greater than 2^31 - 1 ... 0x7FFF_FFFF', 'less than -2^31 ... 0x8000_0000' PAGE 399
            low = 0x7FFFFFFF if inv_sign == 0 else 0x80000000
        env.write_ps(D, 0, low, undef=0xFFFFFFFF00000000 | low_undef, reason=R_FCTIW_HIGH)
        env.ps1_unspecified(D, R_PS1_DOUBLE)
        env.undef_fprf(R_FCTIW_FPRF)
    env.finish_fp(rc)


def _h_fsel(env, w):
    st = env.st
    D, A, B, C, rc = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20), _bits(w, 21, 25), w & 1
    if not _fp_avail(env):
        return
    a = st.fpr[A][0]
    # 'If the operand is greater than or equal to zero, frD is set to frC. If the operand is less than zero
    # or is a NaN, frD is set to frB. The comparison ignores the sign of zero' PAGE 421
    if _is_nan(a):
        pick = B
    else:
        v = _val(a)
        if env.fpscr & NI:
            v = _ni_operand(env, a, v, False, [])     # CROSSCHECK FIX: zeroing is a choice for fsel
        pick = C if (v[0] == "zero" or v[1] == 0) else B
    env.write_ps(D, 0, st.fpr[pick][0])
    env.ps1_unspecified(D, R_FSEL_PS1)
    env.finish_plain(rc)


def _h_fsqrt(env, w, name):
    # 'It does not support the optional fsqrt, fsqrts' -> 'illegal instruction error handler (part of the
    # program exception)' PAGE 87; illegal instruction has priority over FP unavailable (Table 4-3 PAGE 162)
    env.enter("program", 0x700, R_ILLEGAL + "; " + R_SRR1_PROG)


# ======================================================================================
# Compares, moves, FPSCR instructions
# ======================================================================================
def _cmp_key(v):
    if v[0] == "inf":
        return (1 if v[1] == 0 else -1, Fraction(0))
    return (0, _signed(v))


def _h_fcmp(env, w, ordered):
    st = env.st
    crf, A, B, rc = _bits(w, 6, 8), _bits(w, 11, 15), _bits(w, 16, 20), w & 1
    _rsv(w, [(9, 10)], "fcmpo" if ordered else "fcmpu", 397 if ordered else 398)
    if not _fp_avail(env):
        return
    a, b = st.fpr[A][0], st.fpr[B][0]
    f = env.fpscr
    anynan = _is_nan(a) or _is_nan(b)
    snan = _is_snan(a) or _is_snan(b)
    if anynan:
        c = 0b0001
    else:
        va, vb = _val(a), _val(b)
        if f & NI:
            # CROSSCHECK FIX: zeroing itself is an interpretation point for compares (was: always applied)
            cache = []
            va = _ni_operand(env, a, va, False, cache)
            vb = _ni_operand(env, b, vb, False, cache)
        ka, kb = _cmp_key(va), _cmp_key(vb)
        c = 0b1000 if ka < kb else (0b0100 if ka > kb else 0b0010)
    # 'FPCC <- c; CR[(4*crfD)-(4*crfD+3)] <- c' PAGE 397-398
    env.fpscr = (env.fpscr & ~FPCC_MASK) | (c << 12)
    sh = 28 - 4 * crf
    env.cr = (env.cr & ~(0xF << sh)) | (c << sh)
    env.cr_touched = True
    if rc:
        env.cr_undef |= M                              # CROSSCHECK FIX: whole CR (PAGE 175 BF undefined)
        env.note(R_FCMP_RC)
    if snan:
        env.set_exc(VXSNAN)                            # PAGE 397-398
    if ordered and ((snan and not (f & VE)) or (anynan and not snan)):
        # 'if VE = 0 then VXVC <- 1 / else if ((frA) is a QNaN or (frB) is a QNaN) then VXVC <- 1' PAGE 397
        if not (f & NI) or env.choose(R_NI_INVALID, [True, False]):
            env.set_exc(VXVC)
    env.finish_fp(False)


def _h_fmove(env, w, kind):
    st = env.st
    D, B, rc = _bits(w, 6, 10), _bits(w, 16, 20), w & 1
    _rsv(w, [(11, 15)], kind, {"fmr": 405, "fneg": 411, "fabs": 394, "fnabs": 410}[kind])
    if not _fp_avail(env):
        return
    b = st.fpr[B][0]
    if kind == "fmr":
        r = b                                         # 'The content of register frB is placed into frD' PAGE 405
    elif kind == "fneg":
        r = b ^ SIGN                                  # 'with bit 0 inverted' PAGE 411
    elif kind == "fabs":
        r = b & ~SIGN                                 # 'with bit 0 cleared' PAGE 394
    else:
        r = b | SIGN                                  # 'with bit 0 set' PAGE 410
    env.write_ps(D, 0, r)
    env.ps1_unspecified(D, R_FMR_PS1 if kind == "fmr" else R_PS1_DOUBLE)
    env.finish_plain(rc)                              # 'does not alter the FPSCR' PAGE 394, 410, 411


def _h_mffs(env, w):
    st = env.st
    D, rc = _bits(w, 6, 10), w & 1
    _rsv(w, [(11, 20)], "mffs", 460)
    if not _fp_avail(env):
        return
    # 'frD[32-63] <- FPSCR ... The high-order bits of register frD are undefined.' PAGE 460
    env.write_ps(D, 0, st.fpscr & M, undef=0xFFFFFFFF00000000, reason=R_FCTIW_HIGH)
    env.ps1_unspecified(D, R_PS1_DOUBLE)
    env.finish_plain(rc)


_MCRFS_CLEAR = {0: FX | OX, 1: UX | ZX | XX | VXSNAN, 2: VXISI | VXIDI | VXZDZ | VXIMZ, 3: VXVC,
                5: VXSOFT | VXSQRT | VXCVI}          # 'Affected' lists PAGE 457


def _h_mcrfs(env, w):
    crfD, crfS = _bits(w, 6, 8), _bits(w, 11, 13)
    _rsv(w, [(9, 10), (14, 20), (31, 31)], "mcrfs", 457)
    if not _fp_avail(env):
        return
    # 'The contents of FPSCR field crfS are copied to CR field crfD. All exception bits copied (except FEX
    # and VX) are cleared in the FPSCR.' PAGE 457; FEX/VX follow 'the usual rule' PAGE 473
    field = (env.fpscr >> (28 - 4 * crfS)) & 0xF
    sh = 28 - 4 * crfD
    env.cr = (env.cr & ~(0xF << sh)) | (field << sh)
    env.cr_touched = True
    env.fpscr &= ~_MCRFS_CLEAR.get(crfS, 0)
    env.finish_fp(False, fpscr_move=True)


def _h_mtfsb(env, w, one):
    crbD, rc = _bits(w, 6, 10), w & 1
    _rsv(w, [(11, 20)], "mtfsb1" if one else "mtfsb0", 472 if one else 471)
    if not _fp_avail(env):
        return
    bit = _fb(crbD)
    if crbD in (1, 2):
        pass                                           # 'Bits 1 and 2 (FEX and VX) cannot be explicitly
                                                       # cleared/set' PAGE 471-472
    elif crbD == 20:
        env.fpscr_undef |= RSV20
        env.note(R_RSV20)
    elif one:
        if bit & EXC_BITS:
            env.set_exc(bit)                           # 'Affected: FPSCR bit crbD and FX' PAGE 472
        else:
            env.fpscr |= bit
    else:
        env.fpscr &= ~bit                              # 'Bit crbD of the FPSCR is cleared' PAGE 471
    env.finish_fp(rc, fpscr_move=True)


def _mtfs_fields(env, mask, src, rc):
    # 'FPSCR[FX] is altered only if FM[0] = 1 ... bits 0 (FX) and 3 (OX) are set to the values of frB[32]
    # and frB[35] ... Bits 1 and 2 (FEX and VX) are set according to the usual rule' PAGE 473-474
    before = env.fpscr
    env.fpscr = (before & ~mask) | (src & mask)
    env.signaled |= env.fpscr & ~before & EXC_BITS
    if mask & RSV20:
        env.fpscr_undef |= RSV20
        env.note(R_RSV20)
    env.finish_fp(rc, fpscr_move=True)


def _h_mtfsf(env, w):
    st = env.st
    fm, B, rc = _bits(w, 7, 14), _bits(w, 16, 20), w & 1
    _rsv(w, [(6, 6), (15, 15)], "mtfsf", 473)
    if not _fp_avail(env):
        return
    mask = 0
    for i in range(8):                                 # 'If FM[i] = 1, FPSCR field i ... is set' PAGE 473
        if (fm >> (7 - i)) & 1:
            mask |= 0xF << (28 - 4 * i)
    _mtfs_fields(env, mask, st.fpr[B][0] & M, rc)      # 'The low-order 32 bits of frB' PAGE 473


def _h_mtfsfi(env, w):
    crfD, imm, rc = _bits(w, 6, 8), _bits(w, 16, 19), w & 1
    _rsv(w, [(9, 15), (20, 20)], "mtfsfi", 474)
    if not _fp_avail(env):
        return
    sh = 28 - 4 * crfD                                 # 'FPSCR[crfD] <- IMM' PAGE 474
    _mtfs_fields(env, 0xF << sh, imm << sh, rc)


# ======================================================================================
# Loads and stores
# ======================================================================================
def _alignment(env, ea):
    env.enter("alignment", 0x600, R_ALIGN)
    # CROSSCHECK FIX: 'After a DSI or an alignment exception, DAR is set to the effective address (EA) generated
    # by the faulting instruction' PAGE 59 (was: undefined)
    env.vals["dar"] = ea & M
    env.vals["dsisr"] = env.st.dsisr & M
    env.masks["dsisr"] = 0


def _h_lstf(env, w, store, single, update, indexed, name, page):
    st = env.st
    D, A = _bits(w, 6, 10), _bits(w, 11, 15)
    if update and A == 0:
        raise _AllUndefined(f"{name}: 'If rA = 0, the instruction form is invalid.' PAGE {page}; "
                            "invalid forms: 'limited support' PAGE 87")
    if not _fp_avail(env):
        return
    if indexed:
        off = st.gpr[_bits(w, 16, 20)]                 # EA <- (rA|0) + (rB)
    else:
        off = _exts16(w)                               # EA <- (rA|0) + EXTS(d)
    base = st.gpr[A] if (A != 0 or update) else 0
    ea = (base + off) & M
    if st.msr & MSR_LE:
        raise _AllUndefined(R_LE)
    if indexed and (w & 1):
        # CROSSCHECK FIX: moved before the alignment check - PAGE 106 'executing one of these invalid instruction
        # forms causes CR0 to be set to an undefined value'; whether that happens when the (dispatched) instruction
        # takes an alignment exception is not stated -> CR0 undefined in that case too.
        env.cr_undef |= 0xF0000000
        env.note(R_INDEXED_RC)
    if ea & 3:
        _alignment(env, ea)                           # 'not word-aligned' PAGE 82, 175
        return
    if store:
        x = st.fpr[D][0]
        if single:
            # 'MEM(EA, 4) <- SINGLE(frS)' PAGE 551-554; with PSE=1 'store the ps0 value only' PAGE 83
            word, mask = _d7_store_single(env, x)
            env.mem_ops.append(["w", ea, 4, word, mask])
        else:
            env.mem_ops.append(["w", ea, 8, x, MASK64])   # 'MEM(EA, 8) <- (frS)' PAGE 546-549
    else:
        if single:
            word = st.mem._get(ea, 4)
            env.mem_ops.append(["r", ea, 4, word, M])
            val = _d6_load_single(word)
            # PSE=1: 'placed into frD(ps0) and replicated in frD(ps1)' PAGE 434-437
            env.write_ps(D, 0, val)
            if st.hid2 & HID2_PSE:
                env.write_ps(D, 1, val)
            else:
                env.ps1_unspecified(D, R_PS1_PSE0)
        else:
            q = st.mem._get(ea, 8)
            env.mem_ops.append(["r", ea, 8, q, MASK64])
            env.write_ps(D, 0, q)                     # 'frD <- MEM(EA, 8)' PAGE 430-433
            # CROSSCHECK FIX: with HID2[PSE]=1 'psq_l frX,0(r1),0,0 / lfd frX,8(r1)' 'is guaranteed to restore the
            # state of floating-point register frX regardless of its format' and 'restoration of the ps1 value of a
            # paired single operand is not exact ... Denorm ... restored value will also be the value 0' (PAGE 84):
            # ps1 loaded by psq_l survives lfd -> ps1 unchanged (was: unspecified).  PSE=0: ps1 unspecified.
            if not (st.hid2 & HID2_PSE):
                env.ps1_unspecified(D, R_PS1_DOUBLE)
    if update:
        env.vals[f"gpr[{A}]"] = ea                    # 'EA is placed into rA' PAGE 431


# ======================================================================================
# Decoder
# ======================================================================================
_PRIMARY = {
    48: ("lfs", lambda e, w: _h_lstf(e, w, False, True, False, False, "lfs", 434)),
    49: ("lfsu", lambda e, w: _h_lstf(e, w, False, True, True, False, "lfsu", 435)),
    50: ("lfd", lambda e, w: _h_lstf(e, w, False, False, False, False, "lfd", 430)),
    51: ("lfdu", lambda e, w: _h_lstf(e, w, False, False, True, False, "lfdu", 431)),
    52: ("stfs", lambda e, w: _h_lstf(e, w, True, True, False, False, "stfs", 551)),
    53: ("stfsu", lambda e, w: _h_lstf(e, w, True, True, True, False, "stfsu", 552)),
    54: ("stfd", lambda e, w: _h_lstf(e, w, True, False, False, False, "stfd", 546)),
    55: ("stfdu", lambda e, w: _h_lstf(e, w, True, False, True, False, "stfdu", 547)),
}
_G31 = {
    535: ("lfsx", lambda e, w: _h_lstf(e, w, False, True, False, True, "lfsx", 437)),
    567: ("lfsux", lambda e, w: _h_lstf(e, w, False, True, True, True, "lfsux", 436)),
    599: ("lfdx", lambda e, w: _h_lstf(e, w, False, False, False, True, "lfdx", 433)),
    631: ("lfdux", lambda e, w: _h_lstf(e, w, False, False, True, True, "lfdux", 432)),
    663: ("stfsx", lambda e, w: _h_lstf(e, w, True, True, False, True, "stfsx", 554)),
    695: ("stfsux", lambda e, w: _h_lstf(e, w, True, True, True, True, "stfsux", 553)),
    727: ("stfdx", lambda e, w: _h_lstf(e, w, True, False, False, True, "stfdx", 549)),
    759: ("stfdux", lambda e, w: _h_lstf(e, w, True, False, True, True, "stfdux", 548)),
}


def _ar(op, single, name, page):
    return (name, lambda e, w: _h_arith(e, w, op, single, name, page))


_A59 = {
    18: _ar("div", True, "fdivs", 402), 20: _ar("sub", True, "fsubs", 423), 21: _ar("add", True, "fadds", 396),
    22: ("fsqrts", lambda e, w: _h_fsqrt(e, w, "fsqrts")), 24: ("fres", _h_fres),
    25: _ar("mul", True, "fmuls", 409), 28: _ar("msub", True, "fmsubs", 407), 29: _ar("madd", True, "fmadds", 404),
    30: _ar("nmsub", True, "fnmsubs", 415), 31: _ar("nmadd", True, "fnmadds", 413),
}
_A63 = {
    18: _ar("div", False, "fdiv", 401), 20: _ar("sub", False, "fsub", 422), 21: _ar("add", False, "fadd", 395),
    22: ("fsqrt", lambda e, w: _h_fsqrt(e, w, "fsqrt")), 23: ("fsel", _h_fsel),
    25: _ar("mul", False, "fmul", 408), 26: ("frsqrte", _h_frsqrte),
    28: _ar("msub", False, "fmsub", 406), 29: _ar("madd", False, "fmadd", 403),
    30: _ar("nmsub", False, "fnmsub", 414), 31: _ar("nmadd", False, "fnmadd", 412),
}
_X63 = {
    0: ("fcmpu", lambda e, w: _h_fcmp(e, w, False)), 12: ("frsp", _h_frsp),
    14: ("fctiw", lambda e, w: _h_fctiw(e, w, False)), 15: ("fctiwz", lambda e, w: _h_fctiw(e, w, True)),
    32: ("fcmpo", lambda e, w: _h_fcmp(e, w, True)), 38: ("mtfsb1", lambda e, w: _h_mtfsb(e, w, True)),
    40: ("fneg", lambda e, w: _h_fmove(e, w, "fneg")), 64: ("mcrfs", _h_mcrfs),
    70: ("mtfsb0", lambda e, w: _h_mtfsb(e, w, False)), 72: ("fmr", lambda e, w: _h_fmove(e, w, "fmr")),
    134: ("mtfsfi", _h_mtfsfi), 136: ("fnabs", lambda e, w: _h_fmove(e, w, "fnabs")),
    264: ("fabs", lambda e, w: _h_fmove(e, w, "fabs")), 583: ("mffs", _h_mffs), 711: ("mtfsf", _h_mtfsf),
}


def _decode(word):
    word &= M
    op = word >> 26
    if op in _PRIMARY:
        return _PRIMARY[op]
    if op == 31:
        return _G31.get(_bits(word, 21, 30))
    if op == 59:
        return _A59.get(_bits(word, 26, 30))
    if op == 63:
        xo5 = _bits(word, 26, 30)
        if xo5 >= 16:
            return _A63.get(xo5)
        return _X63.get(_bits(word, 21, 30))
    return None


def supports(word):
    return _decode(word) is not None


def mnemonic(word):
    d = _decode(word)
    return d[0] if d else None


# ======================================================================================
# Driver: explore every reading, merge, commit or raise
# ======================================================================================
def _full(loc):
    return MASK64 if loc.startswith("fpr[") else M


def _get_loc(state, loc):
    if loc.startswith("fpr["):
        return state.fpr[int(loc[4:loc.index("]")])][int(loc[-1])]
    if loc.startswith("gpr["):
        return state.gpr[int(loc[4:-1])]
    return getattr(state, loc)


def _set_loc(state, loc, v):
    if loc.startswith("fpr["):
        state.fpr[int(loc[4:loc.index("]")])][int(loc[-1])] = v
    elif loc.startswith("gpr["):
        state.gpr[int(loc[4:-1])] = v
    else:
        setattr(state, loc, v)


def _apply(state, defined):
    if "mem" in defined:
        for kind, addr, width, value, mask in defined["mem"]:
            if kind == "r":
                state.mem.log.append(("r", addr, width, value))
            else:
                old = state.mem._get(addr, width)
                state.mem.write(addr, width, (old & ~mask) | (value & mask))
    for loc in sorted(k for k in defined if k not in ("mem", "pc", "exception")):
        v, m = defined[loc]
        old = _get_loc(state, loc)
        _set_loc(state, loc, ((old & ~m) | (v & m)) & _full(loc))
    if "pc" in defined:
        v, m = defined["pc"]
        state.pc = ((state.pc & ~m) | (v & m)) & M
    if "exception" in defined:
        state.exception = defined["exception"][0]


def _explore(state, word, handler):
    envs = []
    stack = [[]]
    while stack:
        script = stack.pop()
        ctx = _Ctx(script)
        env = _Env(state, word, ctx)
        handler(env, word)
        env.finalize()
        envs.append(env)
        for k in range(len(script), len(ctx.trace)):
            base = script + [0] * (k - len(script))
            for alt in range(1, ctx.trace[k]):
                stack.append(base + [alt])
        if len(envs) > 4096:
            raise _AllUndefined("interpretation tree too large")
    defined, undefined = {}, {}
    locs = set()
    for e in envs:
        locs.update(e.vals)
    for loc in sorted(locs):
        if loc == "mem":
            ops = [e.vals.get("mem", ()) for e in envs]
            shapes = [tuple(o[:3] for o in x) for x in ops]
            if any(s != shapes[0] for s in shapes):
                undefined["mem"] = True
                continue
            merged = []
            for i, (kind, addr, width) in enumerate(shapes[0]):
                full = (1 << (8 * width)) - 1
                vs = [x[i][3] for x in ops]
                m = full
                for x in ops:
                    m &= x[i][4]
                for v in vs:
                    m &= ~(v ^ vs[0]) & full
                merged.append((kind, addr, width, vs[0] & m, m))
                if m != full:
                    undefined[f"mem[{i}]"] = full & ~m
            defined["mem"] = tuple(merged)
            continue
        if loc == "exception":
            vs = [e.vals.get(loc, state.exception) for e in envs]
            if all(v == vs[0] for v in vs):
                defined[loc] = (vs[0], 1)
            else:
                # CROSSCHECK FIX: when the readings disagree on whether (or which) exception is taken, the
                # control flow itself is undefined -> the whole outcome is undefined (was: partial with
                # exception/pc/srr0/srr1/msr bits undefined).
                raise _AllUndefined(R_EXC_DISAGREE + ": " + " | ".join(
                    dict.fromkeys(r for e in envs for r in e.reasons)))
            continue
        full = _full(loc)
        vs = [e.vals.get(loc, _get_loc(state, loc)) & full for e in envs]
        m = full
        for e in envs:
            if loc in e.vals:
                m &= e.masks.get(loc, full)
        for v in vs:
            m &= ~(v ^ vs[0]) & full
        defined[loc] = (vs[0] & m, m)
        if m != full:
            undefined[loc] = full & ~m
    checks = [c for e in envs for c in e.checks]
    reasons = []
    for e in envs:
        for r in e.reasons:
            if r not in reasons:
                reasons.append(r)
    return defined, undefined, checks, reasons


def step(state, word):
    d = _decode(word)
    if d is None:
        raise ValueError(f"fp_A: word {word & M:08X} is not in area {AREA}")
    try:
        defined, undefined, checks, reasons = _explore(state, word & M, d[1])
    except _AllUndefined as u:
        raise UndefinedBehaviour(f"{d[0]}: {u}", {}, ("all",)) from None
    if not undefined and not checks:
        _apply(state, defined)
        return
    raise UndefinedBehaviour(f"{d[0]}: " + " | ".join(reasons), defined, undefined, checks)


def step_with_markers(state, word):
    """Like step(), but on UndefinedBehaviour applies the defined parts and returns the undefined map
    ({location: undefined_mask or True}) plus the bounded-result checks; returns ({}, ()) when defined."""
    try:
        step(state, word)
        return {}, ()
    except UndefinedBehaviour as u:
        u.apply_defined(state)
        return u.undefined, u.checks


# ======================================================================================
# Self-test: hand-derived vectors (expected values worked out by hand from the manual text and
# IEEE 754-1985, NOT computed with the helpers above).
# ======================================================================================
if __name__ == "__main__":
    import traceback

    PASS = [0]
    FAIL = []

    def D_(op, d, a, imm):
        return ((op << 26) | (d << 21) | (a << 16) | (imm & 0xFFFF)) & M

    def X_(op, d, a, b, xo, rc=0):
        return ((op << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | rc) & M

    def A_(op, d, a, b, c, xo, rc=0):
        return ((op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1) | rc) & M

    def new(fpscr=0, pse=True, msr=0x00002032):
        s = State()
        s.fpscr = fpscr
        s.hid2 = 0xE0000000 if pse else 0xC0000000
        s.msr = msr
        return s

    def ok(name, cond):
        if cond:
            PASS[0] += 1
        else:
            FAIL.append(name)
            print("FAIL:", name)

    def run(s, w):
        return step_with_markers(s, w)

    def vec(name, fn):
        try:
            fn()
        except Exception:
            FAIL.append(name + " (exception)")
            print("FAIL:", name)
            traceback.print_exc()

    PC = 0x80003100
    PS1 = {"fpr[%d].ps1": MASK64}

    # ---------------- loads ----------------
    def t1():  # lfs f1,8(r3) PSE=1: 1.5f = 0x3FC00000 -> 0x3FF8000000000000 in ps0 and ps1
        s = new(); s.gpr[3] = 0x80001000; s.mem.load(0x80001008, bytes.fromhex("3FC00000"))
        u, _ = run(s, D_(48, 1, 3, 8))
        ok("lfs 1.5f", u == {} and s.fpr[1] == [0x3FF8000000000000] * 2 and s.pc == PC + 4
           and s.mem.log == [("r", 0x80001008, 4, 0x3FC00000)])
    vec("t1", t1)

    def t2():  # lfs denormal single 0x00000001 = 2^-149 -> double exponent 1023-149 = 874 = 0x36A
        s = new(); s.mem.load(0x2000, bytes.fromhex("00000001"))
        u, _ = run(s, D_(48, 2, 0, 0x2000))
        ok("lfs denorm", u == {} and s.fpr[2][0] == 0x36A0000000000000)
    vec("t2", t2)

    def t3():  # lfs SNaN 0x7F800001: FRT = 01 111 || WORD[2-31] || 29 zeros = 0x7FF0000020000000 (no quieting)
        s = new(); s.mem.load(0x2000, bytes.fromhex("7F800001"))
        u, _ = run(s, D_(48, 2, 0, 0x2000))
        ok("lfs SNaN kept", u == {} and s.fpr[2][0] == 0x7FF0000020000000)
    vec("t3", t3)

    def t4():  # lfs -0.0f -> 0x8000000000000000; lfs -inf 0xFF800000 -> 0xFFF0000000000000
        s = new(); s.mem.load(0x2000, bytes.fromhex("80000000FF800000"))
        run(s, D_(48, 4, 0, 0x2000)); run(s, D_(48, 5, 0, 0x2004))
        ok("lfs -0/-inf", s.fpr[4][0] == 0x8000000000000000 and s.fpr[5][0] == 0xFFF0000000000000)
    vec("t4", t4)

    def t5():  # lfd f2,0x1000(0): ps0 = 8 bytes; PSE=1 ps1 unchanged (PAGE 84), PSE=0 ps1 unspecified
        s = new(); s.mem.load(0x1000, bytes.fromhex("0123456789ABCDEF")); s.fpr[2][1] = 0x55
        u, _ = run(s, D_(50, 2, 0, 0x1000))
        s0 = new(pse=False); s0.mem.load(0x1000, bytes.fromhex("0123456789ABCDEF"))
        u0, _ = run(s0, D_(50, 2, 0, 0x1000))
        ok("lfd", u == {} and s.fpr[2] == [0x0123456789ABCDEF, 0x55] and s.pc == PC + 4
           and u0 == {"fpr[2].ps1": MASK64})
    vec("t5", t5)

    def t6():  # lfsu with rA = 0 is an invalid form
        s = new()
        try:
            step(s, D_(49, 1, 0, 8)); ok("lfsu rA=0", False)
        except UndefinedBehaviour as u:
            ok("lfsu rA=0", u.undefined == {"all": True} and s.pc == PC)
    vec("t6", t6)

    def t7():  # lfsux f1,r4,r5: EA = 0x80002004, -1.0f -> 0xBFF0000000000000, r4 <- EA
        s = new(); s.gpr[4] = 0x80002000; s.gpr[5] = 4; s.mem.load(0x80002004, bytes.fromhex("BF800000"))
        u, _ = run(s, X_(31, 1, 4, 5, 567))
        ok("lfsux", u == {} and s.fpr[1] == [0xBFF0000000000000] * 2 and s.gpr[4] == 0x80002004)
    vec("t7", t7)

    def t8():  # lfs at a misaligned EA -> alignment exception at 0x600, MSR <- ME/IP/ILE only
        s = new(msr=0x00003032); s.gpr[3] = 0x80001001
        u, _ = run(s, D_(48, 1, 3, 0))
        ok("lfs misaligned", s.exception == "alignment" and s.pc == 0x600 and s.srr0 == PC
           and s.msr == 0x1000 and s.dar == 0x80001001 and "dsisr" in u and "srr1" in u and s.fpr[1] == [0, 0])
    vec("t8", t8)

    def t9():  # MSR[FP]=0 -> FP unavailable at 0x800 (IP=1 -> 0xFFF00800)
        s = new(msr=0x00000040); s.fpr[1][0] = 0x3FF0000000000000
        u, _ = run(s, A_(63, 3, 1, 1, 0, 21))
        ok("fp unavailable", s.exception == "fp_unavailable" and s.pc == 0xFFF00800 and s.msr == 0x40
           and s.fpr[3] == [0, 0] and s.fpscr == 0)
    vec("t9", t9)

    def t10():  # lfdx with Rc=1: CR0 undefined, rest defined
        s = new(); s.gpr[4] = 0x3000; s.mem.load(0x3000, bytes.fromhex("4000000000000000"))
        u, _ = run(s, X_(31, 6, 0, 4, 599, 1))
        ok("lfdx Rc=1", u.get("cr") == 0xF0000000 and s.fpr[6][0] == 0x4000000000000000)
    vec("t10", t10)

    # ---------------- stores ----------------
    def t11():  # stfs 1.5 -> 0x3FC00000
        s = new(); s.fpr[1][0] = 0x3FF8000000000000
        u, _ = run(s, D_(52, 1, 0, 0x2000))
        ok("stfs 1.5", u == {} and s.mem._get(0x2000, 4) == 0x3FC00000 and s.mem.log == [("w", 0x2000, 4, 0x3FC00000)])
    vec("t11", t11)

    def t12():  # stfs 2^-127 (exp 896): one shift -> single denormal 0x00400000
        s = new(); s.fpr[1][0] = 0x3800000000000000
        u, _ = run(s, D_(52, 1, 0, 0x2000))
        ok("stfs denormalize", u == {} and s.mem._get(0x2000, 4) == 0x00400000)
    vec("t12", t12)

    def t13():  # stfs +double denormal -> 'Store zero': magnitude 0, sign unstated (PAGE 108)
        s = new(); s.fpr[1][0] = 0x0000000000000001
        u, _ = run(s, D_(52, 1, 0, 0x2000))
        ok("stfs +ddenorm", u == {"mem[0]": 0x80000000} and (s.mem._get(0x2000, 4) & 0x7FFFFFFF) == 0)
    vec("t13", t13)

    def t14():  # stfs -double denormal: sign of the stored zero undefined, other 31 bits zero
        s = new(); s.fpr[1][0] = 0x8000000000000001
        u, _ = run(s, D_(52, 1, 0, 0x2000))
        ok("stfs -ddenorm", u == {"mem[0]": 0x80000000} and (s.mem._get(0x2000, 4) & 0x7FFFFFFF) == 0)
    vec("t14", t14)

    def t15():  # stfs 2^1023 (0x7FE0...): WORD = FRS[0-1]||FRS[5-34] = 0x7F000000
        s = new(); s.fpr[1][0] = 0x7FE0000000000000
        u, _ = run(s, D_(52, 1, 0, 0x2000))
        ok("stfs >1150", u == {} and s.mem._get(0x2000, 4) == 0x7F000000)
    vec("t15", t15)

    def t16():  # stfsu f2,-4(r7): EA = r7-4, r7 <- EA; stfdux f3,r8,r9
        s = new(); s.gpr[7] = 0x80004004; s.fpr[2][0] = 0xC000000000000000
        s.gpr[8] = 0x80005000; s.gpr[9] = 8; s.fpr[3][0] = 0x1122334455667788
        u1, _ = run(s, D_(53, 2, 7, -4)); u2, _ = run(s, X_(31, 3, 8, 9, 759))
        ok("stfsu/stfdux", u1 == {} and u2 == {} and s.gpr[7] == 0x80004000 and s.mem._get(0x80004000, 4) == 0xC0000000
           and s.gpr[8] == 0x80005008 and s.mem._get(0x80005008, 8) == 0x1122334455667788)
    vec("t16", t16)

    # ---------------- arithmetic ----------------
    def t17():  # fadd f3,f1,f2: 1+2 = 3 -> 0x4008000000000000, FPRF +normal (0x4000), ps1 unspecified
        s = new(); s.fpr[1][0] = 0x3FF0000000000000; s.fpr[2][0] = 0x4000000000000000
        u, _ = run(s, A_(63, 3, 1, 2, 0, 21))
        ok("fadd 1+2", u == {"fpr[3].ps1": MASK64} and s.fpr[3][0] == 0x4008000000000000 and s.fpscr == 0x4000)
    vec("t17", t17)

    def t18():  # fadds 0.1f+0.2f, PSE=1: 40265319*2^-27 -> 10066330*2^-25 = 0.3f (0x3E99999A)
        s = new(); s.fpr[1][0] = 0x3FB99999A0000000; s.fpr[2][0] = 0x3FC99999A0000000
        u, _ = run(s, A_(59, 3, 1, 2, 0, 21))
        ok("fadds 0.1f+0.2f", u == {} and s.fpr[3] == [0x3FD3333340000000] * 2 and s.fpscr == 0x82064000)
    vec("t18", t18)

    def t19():  # fadds with a non-single operand is undefined (PAGE 82)
        s = new(); s.fpr[1][0] = 0x3FB999999999999A; s.fpr[2][0] = 0x3FF0000000000000
        try:
            step(s, A_(59, 3, 1, 2, 0, 21)); ok("fadds double operand", False)
        except UndefinedBehaviour as u:
            ok("fadds double operand", u.undefined == {"all": True})
    vec("t19", t19)

    def t20():  # fsub 1-1: +0 under RN=0, -0 under RN=3 (IEEE 754-1985 s6.3)
        s = new(); s.fpr[1][0] = 0x3FF0000000000000
        run(s, A_(63, 3, 1, 1, 0, 20))
        s2 = new(fpscr=3); s2.fpr[1][0] = 0x3FF0000000000000
        run(s2, A_(63, 3, 1, 1, 0, 20))
        ok("fsub zero sign", s.fpr[3][0] == 0 and s.fpscr == 0x2000
           and s2.fpr[3][0] == 0x8000000000000000 and s2.fpscr == 0x12003)
    vec("t20", t20)

    def t21():  # fmul 0*inf, VE=0 -> QNaN 0x7FF8.., FPSCR FX|VX|VXIMZ|FPRF QNaN = 0xA0111000
        s = new(); s.fpr[1][0] = 0; s.fpr[2][0] = INF_BITS
        run(s, A_(63, 3, 1, 0, 2, 25))
        ok("fmul 0*inf", s.fpr[3][0] == 0x7FF8000000000000 and s.fpscr == 0xA0111000)
    vec("t21", t21)

    def t22():  # same with VE=1: no result, FPRF unchanged, FEX set; FR/FI undefined (were 0 -> masked)
        s = new(fpscr=VE); s.fpr[1][0] = 0; s.fpr[2][0] = INF_BITS; s.fpr[3][0] = 0x1234
        u, _ = run(s, A_(63, 3, 1, 0, 2, 25))
        ok("fmul 0*inf VE=1", s.fpr[3][0] == 0x1234 and s.fpscr == 0xE0100080 and u == {"fpscr": FR | FI}
           and s.exception is None)
    vec("t22", t22)

    def t23():  # fdiv 1/0 -> +inf, FX|ZX|FPRF +inf = 0x84005000
        s = new(); s.fpr[1][0] = 0x3FF0000000000000; s.fpr[2][0] = 0
        run(s, A_(63, 3, 1, 2, 0, 18))
        ok("fdiv 1/0", s.fpr[3][0] == INF_BITS and s.fpscr == 0x84005000)
    vec("t23", t23)

    def t24():  # fdiv 0/0 -> QNaN, FX|VX|VXZDZ|QNaN = 0xA0211000
        s = new()
        run(s, A_(63, 3, 1, 2, 0, 18))
        ok("fdiv 0/0", s.fpr[3][0] == GEN_QNAN and s.fpscr == 0xA0211000)
    vec("t24", t24)

    def t25():  # fdiv SNaN/1 -> quieted operand 0x7FF8000000000001, FX|VX|VXSNAN|QNaN = 0xA1011000
        s = new(); s.fpr[1][0] = 0x7FF0000000000001; s.fpr[2][0] = 0x3FF0000000000000
        run(s, A_(63, 3, 1, 2, 0, 18))
        ok("fdiv SNaN", s.fpr[3][0] == 0x7FF8000000000001 and s.fpscr == 0xA1011000)
    vec("t25", t25)

    def t26():  # fadd QNaN(A) + SNaN(B): frA wins (row order), VXSNAN still set
        s = new(); s.fpr[1][0] = 0x7FF8000000000005; s.fpr[2][0] = 0xFFF0000000000009
        run(s, A_(63, 3, 1, 2, 0, 21))
        ok("fadd NaN priority", s.fpr[3][0] == 0x7FF8000000000005 and s.fpscr == 0xA1011000)
    vec("t26", t26)

    def t27():  # fsub 1 - QNaN(B): NaN sign from frB is an interpretation point, rest defined
        s = new(); s.fpr[1][0] = 0x3FF0000000000000; s.fpr[2][0] = 0x7FF8000000000007
        u, _ = run(s, A_(63, 3, 1, 2, 0, 20))
        ok("fsub NaN(B) sign", u.get("fpr[3].ps0") == SIGN and (s.fpr[3][0] & ~SIGN) == 0x7FF8000000000007
           and s.fpscr == 0x11000)
    vec("t27", t27)

    def t28():  # fnmadd with -QNaN in A: sign unaffected (PAGE 412)
        s = new(); s.fpr[1][0] = 0xFFF8000000000000; s.fpr[2][0] = 0x3FF0000000000000
        run(s, A_(63, 3, 1, 2, 2, 31))
        ok("fnmadd QNaN sign", s.fpr[3][0] == 0xFFF8000000000000 and s.fpscr == 0x11000)
    vec("t28", t28)

    def t29():  # fnmadd 2*3+1 -> -7 = 0xC01C000000000000, FPRF -normal 0x8000
        s = new(); s.fpr[1][0] = 0x4000000000000000; s.fpr[3][0] = 0x4008000000000000; s.fpr[2][0] = 0x3FF0000000000000
        run(s, A_(63, 4, 1, 2, 3, 31))
        ok("fnmadd -7", s.fpr[4][0] == 0xC01C000000000000 and s.fpscr == 0x8000)
    vec("t29", t29)

    def t30():  # fmadd fused: (1+2^-52)^2 - (1+2^-51) = 2^-104 exactly -> 0x3970000000000000, no XX
        s = new(); s.fpr[1][0] = 0x3FF0000000000001; s.fpr[2][0] = 0xBFF0000000000002
        run(s, A_(63, 3, 1, 2, 1, 29))
        ok("fmadd fused", s.fpr[3][0] == 0x3970000000000000 and s.fpscr == 0x4000)
    vec("t30", t30)

    def t31():  # fmadds fused PSE=1: (1+2^-23)^2 - (1+2^-22) = 2^-46 -> 0x3D10000000000000 in ps0/ps1
        s = new(); s.fpr[1][0] = 0x3FF0000020000000; s.fpr[2][0] = 0xBFF0000040000000
        u, _ = run(s, A_(59, 3, 1, 2, 1, 29))
        ok("fmadds fused", u == {} and s.fpr[3] == [0x3D10000000000000] * 2 and s.fpscr == 0x4000)
    vec("t31", t31)

    def t32():  # fnmadd RN=+inf: exact d + 2^-60 rounds UP to ...556, then negated -> 0xBFD5555555555556
        s = new(fpscr=2); s.fpr[1][0] = 0x3FF0000000000000; s.fpr[3][0] = 0x3FD5555555555555
        s.fpr[2][0] = 0x3C30000000000000
        run(s, A_(63, 4, 1, 2, 3, 31))
        ok("fnmadd round-then-negate", s.fpr[4][0] == 0xBFD5555555555556 and s.fpscr == 0x82068002)
    vec("t32", t32)

    def t33():  # fdiv 1/3 under RN=0 (...555, FI) / RN=2 (...556, FR|FI)
        s = new(); s.fpr[1][0] = 0x3FF0000000000000; s.fpr[2][0] = 0x4008000000000000
        run(s, A_(63, 3, 1, 2, 0, 18))
        s2 = new(fpscr=2); s2.fpr[1][0] = 0x3FF0000000000000; s2.fpr[2][0] = 0x4008000000000000
        run(s2, A_(63, 3, 1, 2, 0, 18))
        ok("fdiv 1/3 RN", s.fpr[3][0] == 0x3FD5555555555555 and s.fpscr == 0x82024000
           and s2.fpr[3][0] == 0x3FD5555555555556 and s2.fpscr == 0x82064002)
    vec("t33", t33)

    def t34():  # fmul OE=1 overflow: 2^1000*2^1000 = 2^2000 -> 2^464 (0x5CF0...), FX|FEX|OX|+normal|OE
        s = new(fpscr=OE); s.fpr[1][0] = 0x7E70000000000000
        run(s, A_(63, 3, 1, 0, 1, 25))
        ok("fmul OE=1", s.fpr[3][0] == 0x5CF0000000000000 and s.fpscr == 0xD0004040 and s.exception is None)
    vec("t34", t34)

    def t35():  # fmul UE=1 underflow: 2^-1000*2^-100 = 2^-1100 -> 2^436 (0x5B30...), FX|FEX|UX|+normal|UE
        s = new(fpscr=UE); s.fpr[1][0] = 0x0170000000000000; s.fpr[2][0] = 0x39B0000000000000
        run(s, A_(63, 3, 1, 0, 2, 25))
        ok("fmul UE=1", s.fpr[3][0] == 0x5B30000000000000 and s.fpscr == 0xC8004020)
    vec("t35", t35)

    def t36():  # fmul OE=0 overflow RN=0 -> +inf; FR undefined; FX|OX|XX|FI|+inf = 0x92025000
        s = new(); s.fpr[1][0] = 0x7E70000000000000
        u, _ = run(s, A_(63, 3, 1, 0, 1, 25))
        ok("fmul overflow", s.fpr[3][0] == INF_BITS and u == {"fpr[3].ps1": MASK64, "fpscr": FR}
           and s.fpscr & ~FR == 0x92025000)
    vec("t36", t36)

    # ---------------- frsp ----------------
    def t37():  # frsp 1/3 -> 0x3EAAAAAB image 0x3FD5555560000000, FR|FI|XX|FX|+normal; ps1 undefined (PSE=1)
        s = new(); s.fpr[1][0] = 0x3FD5555555555555
        u, _ = run(s, X_(63, 2, 0, 1, 12))
        ok("frsp 1/3", s.fpr[2][0] == 0x3FD5555560000000 and s.fpscr == 0x82064000 and u == {"fpr[2].ps1": MASK64})
    vec("t37", t37)

    def t38():  # frsp 2^1023 with RN=1 -> single max image 0x47EFFFFFE0000000, OX|XX|FI, FR undefined
        s = new(fpscr=1); s.fpr[1][0] = 0x7FE0000000000000
        u, _ = run(s, X_(63, 2, 0, 1, 12))
        ok("frsp overflow RN=1", s.fpr[2][0] == 0x47EFFFFFE0000000 and u.get("fpscr") == FR
           and s.fpscr & ~FR == 0x92024001)
    vec("t38", t38)

    def t39():  # frsp 1.5*2^-149 -> tie to even 2^-148 (0x36B0...), UX|XX|FR|FI|+denorm = 0x8A074000
        s = new(); s.fpr[1][0] = 0x36A8000000000000
        u, _ = run(s, X_(63, 2, 0, 1, 12))
        ok("frsp underflow", s.fpr[2][0] == 0x36B0000000000000 and s.fpscr == 0x8A074000)
    vec("t39", t39)

    def t40():  # frsp SNaN double 0x7FF0000000000123 -> single QNaN image 0x7FF8000000000000, VXSNAN
        s = new(); s.fpr[1][0] = 0x7FF0000000000123
        run(s, X_(63, 2, 0, 1, 12))
        ok("frsp SNaN", s.fpr[2][0] == 0x7FF8000000000000 and s.fpscr == 0xA1011000)
    vec("t40", t40)

    # ---------------- fctiw ----------------
    def t41():  # fctiwz 2.7 -> 2 (low word), FX|XX|FI; high word, FPRF, ps1 undefined
        s = new(); s.fpr[1][0] = 0x400599999999999A
        u, _ = run(s, X_(63, 2, 0, 1, 15))
        ok("fctiwz 2.7", (s.fpr[2][0] & M) == 2 and (s.fpscr & ~FPRF_MASK) == 0x82020000
           and u.get("fpscr") == FPRF_MASK and u.get("fpr[2].ps0") == 0xFFFFFFFF00000000)
    vec("t41", t41)

    def t42():  # fctiw -2.5 RN=0 -> -2 (ties to even) 0xFFFFFFFE, not incremented -> FR=0
        s = new(); s.fpr[1][0] = 0xC004000000000000
        run(s, X_(63, 2, 0, 1, 14))
        ok("fctiw -2.5", (s.fpr[2][0] & M) == 0xFFFFFFFE and (s.fpscr & ~FPRF_MASK) == 0x82020000)
    vec("t42", t42)

    def t43():  # fctiw 3e9 -> 0x7FFFFFFF, FX|VX|VXCVI
        s = new(); s.fpr[1][0] = 0x41E65A0BC0000000
        u, _ = run(s, X_(63, 2, 0, 1, 14))
        ok("fctiw 3e9", (s.fpr[2][0] & M) == 0x7FFFFFFF and (s.fpscr & ~(FPRF_MASK | FR | FI)) == 0xA0000100)
    vec("t43", t43)

    def t44():  # fctiw 3e9 with NI=1 -> 0x80000000 (Table 2-22 NI column)
        s = new(fpscr=NI); s.fpr[1][0] = 0x41E65A0BC0000000
        run(s, X_(63, 2, 0, 1, 14))
        ok("fctiw NI 3e9", (s.fpr[2][0] & M) == 0x80000000)
    vec("t44", t44)

    def t45():  # fctiw -2^31 exactly -> 0x80000000 valid, exact: no flags
        s = new(); s.fpr[1][0] = 0xC1E0000000000000
        run(s, X_(63, 2, 0, 1, 14))
        ok("fctiw -2^31", (s.fpr[2][0] & M) == 0x80000000 and (s.fpscr & ~FPRF_MASK) == 0)
    vec("t45", t45)

    # ---------------- compares ----------------
    def t46():  # fcmpu cr3,f1,f2: 1 < 2 -> CR field 3 = 1000, FPCC = 1000
        s = new(); s.fpr[1][0] = 0x3FF0000000000000; s.fpr[2][0] = 0x4000000000000000
        u, _ = run(s, X_(63, 3 << 2, 1, 2, 0))
        ok("fcmpu <", u == {} and s.cr == 0x00080000 and s.fpscr == 0x8000)
    vec("t46", t46)

    def t47():  # fcmpu +0 vs -0 -> equal (0010) PAGE 98
        s = new(); s.fpr[2][0] = SIGN
        run(s, X_(63, 0, 1, 2, 0))
        ok("fcmpu +0 -0", s.cr == 0x20000000 and s.fpscr == 0x2000)
    vec("t47", t47)

    def t48():  # fcmpo QNaN vs 1 -> unordered, VXVC: FX|VX|VXVC|FU = 0xA0081000
        s = new(); s.fpr[1][0] = 0x7FF8000000000000; s.fpr[2][0] = 0x3FF0000000000000
        run(s, X_(63, 0, 1, 2, 32))
        ok("fcmpo QNaN", s.cr == 0x10000000 and s.fpscr == 0xA0081000)
    vec("t48", t48)

    def t49():  # fcmpo SNaN VE=1 -> VXSNAN only (no VXVC), FEX: 0xE1001080
        s = new(fpscr=VE); s.fpr[1][0] = 0x7FF0000000000001
        run(s, X_(63, 0, 1, 2, 32))
        ok("fcmpo SNaN VE=1", s.cr == 0x10000000 and s.fpscr == 0xE1001080 and s.exception is None)
    vec("t49", t49)

    def t50():  # same with MSR[FE0]=1 -> program exception at 0x700; FPSCR defined, CR write undefined
        s = new(fpscr=VE, msr=0x00002832); s.fpr[1][0] = 0x7FF0000000000001
        u, _ = run(s, X_(63, 0, 1, 2, 32))
        ok("fcmpo trap", s.exception == "program" and s.pc == 0x700 and s.srr0 == PC and s.fpscr == 0xE1001080
           and u.get("cr") == 0x10000000 and s.msr == 0)   # kept (0001) vs discarded (0000) CR0
    vec("t50", t50)

    def t51():  # fcmpu with Rc=1: the CR field written is unknown (PAGE 175) -> CR undefined, FPCC defined
        s = new(); s.fpr[1][0] = 0x4000000000000000; s.fpr[2][0] = 0x3FF0000000000000
        u, _ = run(s, X_(63, 1 << 2, 1, 2, 0, 1))
        ok("fcmpu Rc=1", u == {"cr": 0xFFFFFFFF} and s.fpscr == 0x4000)
    vec("t51", t51)

    # ---------------- FPSCR instructions ----------------
    def t52():  # mtfsfi 7,3 -> RN = 3 ; mtfsfi 0,0b1001 -> FX=1, OX=1 (from IMM), FEX/VX by rule
        s = new()
        run(s, X_(63, 7 << 2, 0, 3 << 1, 134))
        a = s.fpscr
        s2 = new(); run(s2, X_(63, 0, 0, 9 << 1, 134))
        ok("mtfsfi", a == 3 and s2.fpscr == 0x90000000)
    vec("t52", t52)

    def t53():  # mtfsf FM=0xFF, frB low = 0xFFFFFFFF: all set, FEX/VX by rule (=1), bit 20 undefined
        s = new(); s.fpr[5][0] = 0xFFFFFFFF
        u, _ = run(s, ((63 << 26) | (0xFF << 17) | (5 << 11) | (711 << 1)))
        ok("mtfsf all", u == {"fpscr": RSV20} and (s.fpscr | RSV20) == 0xFFFFFFFF)
    vec("t53", t53)

    def t54():  # mtfsf FM=0x80 (field 0) frB = 0x40000000 (FEX only): FX=0, OX=0, FEX by rule = 0
        s = new(fpscr=0x90000000); s.fpr[5][0] = 0x40000000
        run(s, ((63 << 26) | (0x80 << 17) | (5 << 11) | (711 << 1) | 1))
        ok("mtfsf field0", s.fpscr == 0 and s.cr == 0)
    vec("t54", t54)

    def t55():  # mtfsf FM=0x40 (field 1) sets UX 0->1: FX NOT altered (FM[0]=0) PAGE 473
        s = new(); s.fpr[5][0] = 0x08000000
        run(s, ((63 << 26) | (0x40 << 17) | (5 << 11) | (711 << 1)))
        ok("mtfsf no FX", s.fpscr == 0x08000000)
    vec("t55", t55)

    def t56():  # mtfsb1 24 (VE) with VXSNAN set: FEX 1, FX unchanged -> 0xE1000080 ; mtfsb0 7 -> VX clears
        s = new(fpscr=0xA1000000)
        run(s, X_(63, 24, 0, 0, 38))
        a = s.fpscr
        s2 = new(fpscr=0xA1000000); run(s2, X_(63, 7, 0, 0, 70))
        s3 = new(fpscr=0xA1000000); run(s3, X_(63, 2, 0, 0, 70))
        ok("mtfsb", a == 0xE1000080 and s2.fpscr == 0x80000000 and s3.fpscr == 0xA1000000)
    vec("t56", t56)

    def t57():  # mtfsb1 3 (OX) with OE=1 and FE1=1 -> FX|FEX|OX then program exception
        s = new(fpscr=OE, msr=0x00002132)
        u, _ = run(s, X_(63, 3, 0, 0, 38))
        ok("mtfsb1 trap", s.fpscr == 0xD0000040 and s.exception == "program" and s.pc == 0x700 and s.srr0 == PC)
    vec("t57", t57)

    def t58():  # mcrfs 1,1 from 0x2F000000 (VX|UX|ZX|XX|VXSNAN): CR1 <- 1111, bits cleared, VX recomputed
        s = new(fpscr=0x2F000000)
        run(s, X_(63, 1 << 2, 1 << 2, 0, 64))
        ok("mcrfs", s.cr == 0x0F000000 and s.fpscr == 0)
    vec("t58", t58)

    def t59():  # mcrfs 0,0 from FX|OX: CR0 <- 1001, FX and OX cleared
        s = new(fpscr=0x90000000)
        run(s, X_(63, 0, 0, 0, 64))
        ok("mcrfs FX", s.cr == 0x90000000 and s.fpscr == 0)
    vec("t59", t59)

    def t60():  # mffs. f5: low word = FPSCR, high word and ps1 undefined, CR1 <- FPSCR[0-3]
        s = new(fpscr=0x82064000)
        u, _ = run(s, X_(63, 5, 0, 0, 583, 1))
        ok("mffs.", (s.fpr[5][0] & M) == 0x82064000 and u == {"fpr[5].ps0": 0xFFFFFFFF00000000, "fpr[5].ps1": MASK64}
           and s.cr == 0x08000000)
    vec("t60", t60)

    # ---------------- moves / fsel ----------------
    def t61():  # fneg. f1,f2 on QNaN: sign flips, FPSCR untouched, CR1 from FPSCR (0x9...)
        s = new(fpscr=0x90000000); s.fpr[2][0] = 0x7FF8000000000000
        run(s, X_(63, 1, 0, 2, 40, 1))
        ok("fneg.", s.fpr[1][0] == 0xFFF8000000000000 and s.fpscr == 0x90000000 and s.cr == 0x09000000)
    vec("t61", t61)

    def t62():  # fabs -0 -> +0 ; fnabs 1.0 -> -1.0 ; fmr copies ps0
        s = new(); s.fpr[2][0] = SIGN; s.fpr[3][0] = 0x3FF0000000000000; s.fpr[4][0] = 0x0123456789ABCDEF
        run(s, X_(63, 1, 0, 2, 264)); run(s, X_(63, 5, 0, 3, 136)); u, _ = run(s, X_(63, 6, 0, 4, 72))
        ok("fabs/fnabs/fmr", s.fpr[1][0] == 0 and s.fpr[5][0] == 0xBFF0000000000000
           and s.fpr[6][0] == 0x0123456789ABCDEF and u == {"fpr[6].ps1": MASK64})
    vec("t62", t62)

    def t63():  # fsel: A=-0 -> frC ; A=NaN -> frB ; A=-1 -> frB
        s = new(); s.fpr[2][0] = 0x1111; s.fpr[3][0] = 0x2222
        s.fpr[1][0] = SIGN; run(s, A_(63, 4, 1, 2, 3, 23)); r1 = s.fpr[4][0]
        s.fpr[1][0] = 0x7FF8000000000000; run(s, A_(63, 4, 1, 2, 3, 23)); r2 = s.fpr[4][0]
        s.fpr[1][0] = 0xBFF0000000000000; run(s, A_(63, 4, 1, 2, 3, 23)); r3 = s.fpr[4][0]
        ok("fsel", (r1, r2, r3) == (0x2222, 0x1111, 0x1111))
    vec("t63", t63)

    # ---------------- estimates ----------------
    def t64():  # fres 0 -> +inf ZX (PSE=1 -> ps1 = ps0), FR/FI undefined
        s = new()
        u, _ = run(s, A_(59, 2, 0, 1, 0, 24))
        ok("fres +0", s.fpr[2] == [INF_BITS] * 2 and (s.fpscr & ~(FR | FI)) == 0x84005000 and u == {"fpscr": FR | FI})
    vec("t64", t64)

    def t65():  # fres 2.0f: estimate bounded; 0.5 passes the check, 0.5003 (> 1/4096) fails
        s = new(); s.fpr[1][0] = 0x4000000000000000
        try:
            step(s, A_(59, 2, 0, 1, 0, 24)); ok("fres 2", False)
        except UndefinedBehaviour as u:
            t = s.clone(); u.apply_defined(t); t.fpr[2] = [0x3FE0000000000000] * 2
            bad = s.clone(); u.apply_defined(bad); bad.fpr[2] = [0x3FE0028F60000000] * 2
            ok("fres 2", u.check(t) and not u.check(bad) and (t.fpscr & FPRF_MASK) == 0x4000)
    vec("t65", t65)

    def t66():  # frsqrte -1 -> QNaN VXSQRT: FX|VX|VXSQRT|QNaN = 0xA0011200 ; +inf -> +0
        s = new(); s.fpr[1][0] = 0xBFF0000000000000
        run(s, A_(63, 2, 0, 1, 0, 26))
        s2 = new(); s2.fpr[1][0] = INF_BITS; run(s2, A_(63, 2, 0, 1, 0, 26))
        ok("frsqrte special", s.fpr[2][0] == GEN_QNAN and (s.fpscr & ~(FR | FI)) == 0xA0011200
           and s2.fpr[2][0] == 0 and (s2.fpscr & ~(FR | FI)) == 0x2000)
    vec("t66", t66)

    def t67():  # frsqrte 4.0: 0.5 passes, 0.51 fails
        s = new(); s.fpr[1][0] = 0x4010000000000000
        try:
            step(s, A_(63, 2, 0, 1, 0, 26)); ok("frsqrte 4", False)
        except UndefinedBehaviour as u:
            t = s.clone(); u.apply_defined(t); t.fpr[2][0] = 0x3FE0000000000000
            bad = t.clone(); bad.fpr[2][0] = 0x3FE051EB851EB852
            ok("frsqrte 4", u.check(t) and not u.check(bad))
    vec("t67", t67)

    def t68():  # fsqrt is illegal on Gekko -> program exception even with MSR[FP]=0
        s = new(msr=0)
        u, _ = run(s, A_(63, 2, 0, 1, 0, 22))
        ok("fsqrt illegal", s.exception == "program" and s.pc == 0x700 and s.srr0 == PC)
    vec("t68", t68)

    # ---------------- non-IEEE mode ----------------
    def t69():  # NI: 1.0 + double denormal -> denormal zeroed -> exactly 1.0, no XX (IEEE mode would set XX)
        s = new(fpscr=NI); s.fpr[1][0] = 0x3FF0000000000000; s.fpr[2][0] = 0x0000000000000001
        run(s, A_(63, 3, 1, 2, 0, 21))
        s2 = new(); s2.fpr[1][0] = 0x3FF0000000000000; s2.fpr[2][0] = 0x0000000000000001
        run(s2, A_(63, 3, 1, 2, 0, 21))
        ok("NI operand zero", s.fpr[3][0] == 0x3FF0000000000000 and s.fpscr == 0x4004
           and s2.fpr[3][0] == 0x3FF0000000000000 and s2.fpscr == 0x82024000)
    vec("t69", t69)

    def t70():  # NI: 2^-600*2^-600 tiny before rounding -> +0; UX/XX/FR/FI (and FX/FEX, VX no) undefined
        s = new(fpscr=NI); s.fpr[1][0] = 0x1A70000000000000
        u, _ = run(s, A_(63, 3, 1, 0, 1, 25))
        ok("NI denormal result", s.fpr[3][0] == 0 and (s.fpscr & FPRF_MASK) == 0x2000
           and u.get("fpscr", 0) & (UX | XX | FR | FI) == (UX | XX | FR | FI))
    vec("t70", t70)

    def t71():  # IEEE tiny exact result: 2^-600*2^-474 = 2^-1074 exact -> denormal, no UX (not inexact)
        s = new(); s.fpr[1][0] = 0x1A70000000000000; s.fpr[2][0] = 0x2250000000000000
        u, _ = run(s, A_(63, 3, 1, 0, 2, 25))
        ok("exact denormal", s.fpr[3][0] == 1 and s.fpscr == 0x14000 and u == {"fpr[3].ps1": MASK64})
    vec("t71", t71)

    def t72():  # aliasing: fmul f1,f1,f1 with 3.0 -> 9.0 (operands read before the write)
        s = new(); s.fpr[1][0] = 0x4008000000000000
        run(s, A_(63, 1, 1, 0, 1, 25))
        ok("alias fmul", s.fpr[1][0] == 0x4022000000000000)
    vec("t72", t72)

    def t73():  # reserved field nonzero (fadd with frC != 0) -> boundedly undefined
        s = new()
        try:
            step(s, A_(63, 3, 1, 2, 5, 21)); ok("fadd reserved", False)
        except UndefinedBehaviour as u:
            ok("fadd reserved", u.undefined == {"all": True})
    vec("t73", t73)

    print(f"fp_A self-test: {PASS[0]} passed, {len(FAIL)} failed")
    sys.exit(1 if FAIL else 0)
