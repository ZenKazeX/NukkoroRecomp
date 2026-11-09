"""Independent semantic model, author A, area "int".

Integer arithmetic / logical / compare / rotate / shift / count instructions of the
IBM Gekko, derived ONLY from docs/reference/ppc/gekko_user_manual.txt (cited as
"PAGE N" = the "===== PAGE N =====" marker) with ppc_750cl.txt used as a cross-check
(cited as "750CL PAGE N").

Covered:
  primary  7 mulli, 8 subfic, 10 cmpli, 11 cmpi, 12 addic, 13 addic., 14 addi, 15 addis,
           20 rlwimi, 21 rlwinm, 23 rlwnm, 24 ori, 25 oris, 26 xori, 27 xoris,
           28 andi., 29 andis.
  group 19 mcrf, crand, crandc, creqv, crnand, crnor, cror, crorc, crxor
  group 31 XO-form (OE/Rc): add, addc, adde, addme, addze, subf, subfc, subfe, subfme,
           subfze, neg, mullw, divw, divwu; (Rc only) mulhw, mulhwu
           X-form: cmp, cmpl, and, andc, or, orc, xor, nand, nor, eqv, extsb, extsh,
           cntlzw, slw, srw, sraw, srawi, mcrxr, mfcr, mtcrf
NOT covered (other areas): tw / twi (trap = exception area), branches, load/store,
  SPR moves, FP, paired-single.

Undefined behaviour is never invented: step() raises UndefinedBehaviour WITHOUT touching
the state; the exception carries the parts the manual does define (apply_defined()) and
the list of locations the manual leaves undefined.  step_with_markers() applies the
defined parts and returns {location: UNDEFINED}.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gekko_state import MASK32, MASK64, State  # noqa: E402

AREA = "int"


# --------------------------------------------------------------------------------------
# Undefined-behaviour plumbing
# --------------------------------------------------------------------------------------
class _UndefinedMarker:
    __slots__ = ()

    def __repr__(self):
        return "UNDEFINED"


UNDEFINED = _UndefinedMarker()


class UndefinedBehaviour(Exception):
    """Raised when the manual leaves (part of) the result undefined.

    what      : text including the PAGE citation
    defined   : dict of what the manual DOES define: optional keys
                'pc' (int), 'xer' (int), 'gpr' ({idx: val}), 'cr' (int) + 'cr_mask' (int:
                only CR bits under the mask are defined)
    undefined : tuple of location names left undefined ('all' = boundedly undefined)
    """

    def __init__(self, what, defined=None, undefined=("all",)):
        super().__init__(what)
        self.what = what
        self.defined = defined or {}
        self.undefined = tuple(undefined)

    def apply_defined(self, state):
        d = self.defined
        if "gpr" in d:
            for i, v in d["gpr"].items():
                state.gpr[i] = v & MASK32
        if "xer" in d:
            state.xer = d["xer"] & MASK32
        if "cr" in d:
            m = d.get("cr_mask", MASK32)
            state.cr = ((state.cr & ~m) | (d["cr"] & m)) & MASK32
        if "pc" in d:
            state.pc = d["pc"] & MASK32
            state.exception = None


# --------------------------------------------------------------------------------------
# Bit helpers.  Big-endian bit numbering: bit 0 is the MSB of a 32-bit value.
# PAGE 338-339 (Table 12-2 field positions), PAGE 341-342 (Table 12-3 notation).
# --------------------------------------------------------------------------------------
M = MASK32


def _bits(w, a, b):
    """Field w[a..b] (big-endian bit numbers, inclusive)."""
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def _sx32(v):
    v &= M
    return v - (1 << 32) if v & 0x80000000 else v


def _exts16(v):
    # EXTS(x): "Result of extending x on the left with sign bits" PAGE 341
    v &= 0xFFFF
    return ((v - 0x10000) if v & 0x8000 else v) & M


def _rotl(x, n):
    # ROTL(x, y): "rotating the value x left y positions, where x is 32 bits long" PAGE 342
    # (ROTL(rS, 32 - 0) in sraw/srw/srawi is a full rotation = identity, hence mod 32)
    x &= M
    n %= 32
    if n == 0:
        return x
    return ((x << n) | (x >> (32 - n))) & M


def _mask(x, y):
    # MASK(x, y): "Mask having ones in positions x through y (wrapping if x > y) and
    # zeros elsewhere." PAGE 341
    if x <= y:
        return (((1 << (y - x + 1)) - 1) << (31 - y)) & M
    return _mask(x, 31) | _mask(0, y)


# CR: eight 4-bit fields CR0-CR7 (PAGE 57); field n = CR bits 4n..4n+3
# ("CR[(4*crfD)-(4*crfD+3)] <- c || XER[SO]" PAGE 365); within a field the order is
# LT, GT, EQ, SO ("CR0[LT GT EQ S0] = 0b00 || store_performed || XER[SO]" PAGE 565).
def _get_field(cr, n):
    return (cr >> (28 - 4 * n)) & 0xF


def _set_field(cr, n, nib):
    sh = 28 - 4 * n
    return ((cr & ~(0xF << sh)) | ((nib & 0xF) << sh)) & M


def _crbit(cr, b):
    return (cr >> (31 - b)) & 1


def _set_crbit(cr, b, v):
    return ((cr & ~(1 << (31 - b))) | ((v & 1) << (31 - b))) & M


# XER.  The source set names the bits (SO, OV, CA: "The XER contains the summary overflow
# bit, integer carry bit, overflow bit" PAGE 41) and fixes that they live in XER[0-3]
# (mcrxr copies "XER[0-3]" into a CR field, PAGE 458) but defers the exact layout to
# "Section 2.1.5, XER Register" of the Programming Environments manual (PAGE 345), which is
# NOT in the source set.  INTERPRETATION: SO = XER[0], OV = XER[1], CA = XER[2] (the
# PowerPC architecture layout).  Change these three constants if that is disputed.
XER_SO = 0x80000000
XER_OV = 0x40000000
XER_CA = 0x20000000


def _so(xer):
    return 1 if xer & XER_SO else 0


def _ca(xer):
    return 1 if xer & XER_CA else 0


def _with_ca(xer, ca):
    return (xer | XER_CA) if ca else (xer & ~XER_CA & M)


def _with_ov(xer, ov):
    # OE=1: "The summary overflow bit (SO) and overflow bit (OV) in the integer exception
    # register are set to reflect an overflow condition of a 32-bit result. This can happen
    # only when OE = 1." PAGE 92.  INTERPRETATION: OV is written with the overflow status
    # (set or cleared); SO, the "summary" bit (PAGE 41), is only ever set (sticky) - the
    # manual gives mcrxr as the instruction that clears it (PAGE 458).
    if ov:
        return xer | XER_OV | XER_SO
    return xer & ~XER_OV & M


def _cr0(cr, result, xer):
    # Rc=1: "CR bits 0-2 are set to reflect the result as a signed quantity and CR bit 3
    # receives a copy of the summary overflow bit, XER[SO]." PAGE 339.  The copy is of
    # XER[SO] AFTER this instruction's own OE update: the 'o' suffix records "an overflow in
    # XER[OV] and CR0[SO] for integer instructions" PAGE 340.
    r = _sx32(result)
    c = 0b100 if r < 0 else (0b010 if r > 0 else 0b001)
    return _set_field(cr, 0, (c << 1) | _so(xer))


def _reserved(name, page):
    # "If instructions are encoded with incorrectly set bits in reserved fields, the results
    # on execution can be said to be boundedly undefined." PAGE 87
    raise UndefinedBehaviour(
        f"{name}: nonzero reserved field (encoding PAGE {page}) -> boundedly undefined PAGE 87")


def _sum3(a, b, c):
    """Unsigned 32-bit sum a + b + c (c in {0,1}).

    CA = carry out of bit 0: "'c' indicates a carry out in XER[CA]" PAGE 340.
    OV = the infinitely precise sum of the operands, each 32-bit operand taken as a signed
    integer and c as 0/1, is not representable as a 32-bit signed integer ("overflow
    condition of a 32-bit result" PAGE 92).  INTERPRETATION of the forms written as
    "not(rA) + rB + 1", "(rA) + XER[CA] + 0xFFFF_FFFF" etc.: the 32-bit terms are the
    operands of that sum (so 0xFFFF_FFFF counts as -1).
    """
    full = (a & M) + (b & M) + c
    s = _sx32(a) + _sx32(b) + c
    ov = 1 if (s < -(1 << 31) or s > (1 << 31) - 1) else 0
    return full & M, 1 if full >> 32 else 0, ov


# --------------------------------------------------------------------------------------
# Handlers.  Each returns a commit dict {'gpr': {i: v}, 'xer': v, 'cr': v}; step() commits
# atomically, so every operand is read before any destination is written (aliasing safe).
# --------------------------------------------------------------------------------------

# ---- primary-opcode D-form -----------------------------------------------------------
def _h_addi(st, w):
    # addi PAGE 348: "if rA = 0 then rD <- EXTS(SIMM) else rD <- (rA) + EXTS(SIMM)"
    d, a, imm = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 31)
    base = 0 if a == 0 else st.gpr[a]
    return {"gpr": {d: (base + _exts16(imm)) & M}}


def _h_addis(st, w):
    # addis PAGE 351: "if rA = 0 then rD <- (SIMM || (16)0) else rD <- (rA) + (SIMM || (16)0)"
    d, a, imm = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 31)
    base = 0 if a == 0 else st.gpr[a]
    return {"gpr": {d: (base + (imm << 16)) & M}}


def _h_addic(st, w, rc):
    # addic PAGE 349 / addic. PAGE 350: "rD <- (rA) + EXTS(SIMM)" - NO (rA|0) special case;
    # XER CA affected; addic. always sets CR0 (LT, GT, EQ, SO).
    d, a, imm = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 31)
    res, ca, _ = _sum3(st.gpr[a], _exts16(imm), 0)
    xer = _with_ca(st.xer, ca)
    c = {"gpr": {d: res}, "xer": xer}
    if rc:
        c["cr"] = _cr0(st.cr, res, xer)
    return c


def _h_subfic(st, w):
    # subfic PAGE 572: "rD <- not(rA) + EXTS(SIMM) + 1"; XER CA affected; no CR, no OV.
    d, a, imm = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 31)
    res, ca, _ = _sum3(~st.gpr[a] & M, _exts16(imm), 1)
    return {"gpr": {d: res}, "xer": _with_ca(st.xer, ca)}


def _h_mulli(st, w):
    # mulli PAGE 484: "The low-order 32-bits of the 48-bit product of the operands are placed
    # into rD. Both the operands and the product are interpreted as signed integers."
    # (pseudocode "prod[0-48] ... rD <- prod[16-48]" is garbled - 33 bits; the prose is used)
    d, a, imm = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 31)
    prod = _sx32(st.gpr[a]) * _sx32(_exts16(imm))
    return {"gpr": {d: prod & M}}


def _h_cmpi_l(st, w, logical):
    # cmpi PAGE 366 (signed, EXTS(SIMM)); cmpli PAGE 368 (unsigned, (16)0 || UIMM);
    # "CR[(4*crfD)-(4*crfD+3)] <- c || XER[SO]"; "If L = 1 the instruction form is invalid."
    crf, r9, L, a, imm = _bits(w, 6, 8), _bits(w, 9, 9), _bits(w, 10, 10), _bits(w, 11, 15), _bits(w, 16, 31)
    name = "cmpli" if logical else "cmpi"
    if L:
        # PAGE 87: "Gekko provides limited support for instructions represented in an invalid form."
        raise UndefinedBehaviour(f"{name} with L=1: invalid form PAGE {368 if logical else 366}, "
                                 "limited support for invalid forms PAGE 87")
    if r9:
        _reserved(name, 368 if logical else 366)
    if logical:
        x, y = st.gpr[a], imm
    else:
        x, y = _sx32(st.gpr[a]), _sx32(_exts16(imm))
    c = 0b100 if x < y else (0b010 if x > y else 0b001)
    return {"cr": _set_field(st.cr, crf, (c << 1) | _so(st.xer))}


def _h_logic_imm(st, w, op):
    # ori PAGE 491, oris PAGE 492, xori PAGE 581, xoris PAGE 582: no registers altered.
    # andi. PAGE 356, andis. PAGE 357: CR0 LT GT EQ SO always.
    # "Logical instructions do not affect XER[SO], XER[OV], or XER[CA]." PAGE 93
    s, a, ui = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 31)
    rs = st.gpr[s]
    if op == "ori":
        return {"gpr": {a: rs | ui}}
    if op == "oris":
        return {"gpr": {a: rs | (ui << 16)}}
    if op == "xori":
        return {"gpr": {a: rs ^ ui}}
    if op == "xoris":
        return {"gpr": {a: rs ^ (ui << 16)}}
    res = rs & (ui if op == "andi." else (ui << 16))
    return {"gpr": {a: res}, "cr": _cr0(st.cr, res, st.xer)}


# ---- M-form rotates -------------------------------------------------------------------
def _h_rlw(st, w, op):
    # rlwimi PAGE 533: "r <- ROTL(rS, SH); m <- MASK(MB, ME); rA <- (r & m) | (rA & not m)"
    # rlwinm PAGE 534: "rA <- r & m"
    # rlwnm  PAGE 536: "n <- rB[27-31]; r <- ROTL(rS, n); ... rA <- r & m"
    s, a, f, mb, me, rc = (_bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20),
                           _bits(w, 21, 25), _bits(w, 26, 30), _bits(w, 31, 31))
    n = _bits(st.gpr[f], 27, 31) if op == "rlwnm" else f
    r = _rotl(st.gpr[s], n)
    m = _mask(mb, me)
    if op == "rlwimi":
        res = (r & m) | (st.gpr[a] & ~m & M)
    else:
        res = r & m
    c = {"gpr": {a: res}}
    if rc:
        c["cr"] = _cr0(st.cr, res, st.xer)
    return c


# ---- group 19 ------------------------------------------------------------------------
_CR_OPS = {
    257: ("crand", lambda x, y: x & y),            # PAGE 370 "CR[crbA] & CR[crbB]"
    129: ("crandc", lambda x, y: x & (1 - y)),     # PAGE 371 "CR[crbA] & not CR[crbB]"
    289: ("creqv", lambda x, y: 1 - (x ^ y)),      # PAGE 372 "CR[crbA] == CR[crbB]"
    225: ("crnand", lambda x, y: 1 - (x & y)),     # PAGE 373 "not(CR[crbA] & CR[crbB])"
    33: ("crnor", lambda x, y: 1 - (x | y)),       # PAGE 374 "not(CR[crbA] | CR[crbB])"
    449: ("cror", lambda x, y: x | y),             # PAGE 375 "CR[crbA] | CR[crbB]"
    417: ("crorc", lambda x, y: x | (1 - y)),      # PAGE 376 "CR[crbA] | not CR[crbB]"
    193: ("crxor", lambda x, y: x ^ y),            # PAGE 377 "CR[crbA] xor CR[crbB]"
}
_CR_OP_PAGE = {257: 370, 129: 371, 289: 372, 225: 373, 33: 374, 449: 375, 417: 376, 193: 377}


def _h_crop(st, w, xo):
    name, fn = _CR_OPS[xo]
    if _bits(w, 31, 31):
        _reserved(name, _CR_OP_PAGE[xo])  # encoding shows bit 31 = 0 (Reserved)
    bd, ba, bb = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20)
    v = fn(_crbit(st.cr, ba), _crbit(st.cr, bb))
    return {"cr": _set_crbit(st.cr, bd, v)}


def _h_mcrf(st, w):
    # mcrf PAGE 456: "CR[4*crfD - 4*crfD+3] <- CR[4*crfS - 4*crfS+3]"; encoding:
    # 19 crfD 00 crfS 00 00000 0000000000 0 (reserved bits 9-10, 14-20, 31)
    if _bits(w, 9, 10) or _bits(w, 14, 20) or _bits(w, 31, 31):
        _reserved("mcrf", 456)
    d, s = _bits(w, 6, 8), _bits(w, 11, 13)
    return {"cr": _set_field(st.cr, d, _get_field(st.cr, s))}


# ---- group 31 XO-form add/subtract family ---------------------------------------------
# name: (page, operand builder (A, B, CA) -> (x, y, c), CA affected, has rB)
_ADDSUB = {
    266: ("add", 345, lambda A, B, C: (A, B, 0), False, True),              # rD <- (rA) + (rB)
    10: ("addc", 346, lambda A, B, C: (A, B, 0), True, True),               # rD <- (rA) + (rB)
    138: ("adde", 347, lambda A, B, C: (A, B, C), True, True),              # + XER[CA]
    234: ("addme", 352, lambda A, B, C: (A, M, C), True, False),            # + XER[CA] + 0xFFFF_FFFF
    202: ("addze", 353, lambda A, B, C: (A, 0, C), True, False),            # + XER[CA]
    40: ("subf", 569, lambda A, B, C: (~A & M, B, 1), False, True),         # not(rA) + (rB) + 1
    8: ("subfc", 570, lambda A, B, C: (~A & M, B, 1), True, True),          # not(rA) + (rB) + 1
    136: ("subfe", 571, lambda A, B, C: (~A & M, B, C), True, True),        # not(rA) + (rB) + XER[CA]
    232: ("subfme", 573, lambda A, B, C: (~A & M, M, C), True, False),      # not(rA) + XER[CA] + (32)1
    200: ("subfze", 574, lambda A, B, C: (~A & M, 0, C), True, False),      # not(rA) + XER[CA]
    104: ("neg", 487, lambda A, B, C: (~A & M, 0, 1), False, False),        # not(rA) + 1
}


def _h_addsub(st, w, xo):
    # Affected lists per page: CR0 LT GT EQ SO (if Rc=1); XER CA (c/e/me/ze forms);
    # XER SO OV (if OE=1).  neg PAGE 487: "If rA contains the most negative 32-bit number
    # (0x8000_0000), the result is the most negative number and if OE = 1, OV is set."
    name, page, build, ca_aff, has_rb = _ADDSUB[xo]
    d, a, b, oe, rc = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20), _bits(w, 21, 21), _bits(w, 31, 31)
    if not has_rb and b:
        _reserved(name, page)  # encoding "31 D A 0 0 0 0 0 OE xo Rc"
    x, y, c = build(st.gpr[a], st.gpr[b], _ca(st.xer))
    res, ca, ov = _sum3(x, y, c)
    xer = st.xer
    if ca_aff:
        xer = _with_ca(xer, ca)
    if oe:
        xer = _with_ov(xer, ov)
    out = {"gpr": {d: res}, "xer": xer}
    if rc:
        out["cr"] = _cr0(st.cr, res, xer)
    return out


# ---- multiply -------------------------------------------------------------------------
def _h_mullw(st, w):
    # mullw PAGE 485: low-order 32 bits of the product; "If OE = 1, then OV is set if the
    # product cannot be represented in 32 bits. Both the operands and the product are
    # interpreted as signed integers."  CR0 from the 32-bit result ("CR0 field may not
    # reflect the infinitely precise result if overflow occurs").
    d, a, b, oe, rc = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20), _bits(w, 21, 21), _bits(w, 31, 31)
    prod = _sx32(st.gpr[a]) * _sx32(st.gpr[b])
    res = prod & M
    xer = st.xer
    if oe:
        xer = _with_ov(xer, 1 if (prod < -(1 << 31) or prod > (1 << 31) - 1) else 0)
    out = {"gpr": {d: res}, "xer": xer}
    if rc:
        out["cr"] = _cr0(st.cr, res, xer)
    return out


def _h_mulhw(st, w, unsigned):
    # mulhw PAGE 482: "The high-order 32 bits of the 64-bit product of the operands are placed
    # into rD. Both the operands and the product are interpreted as signed integers."
    # mulhwu PAGE 483: unsigned, "except that if Rc = 1 the first three bits of CR0 field are
    # set by signed comparison of the result to zero."  Encoding "31 D A B 0 75|11 Rc": bit 21
    # is Reserved (no OE form).  XER not affected.
    name, page = ("mulhwu", 483) if unsigned else ("mulhw", 482)
    if _bits(w, 21, 21):
        _reserved(name, page)
    d, a, b, rc = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20), _bits(w, 31, 31)
    if unsigned:
        prod = st.gpr[a] * st.gpr[b]
    else:
        prod = _sx32(st.gpr[a]) * _sx32(st.gpr[b])
    res = ((prod & MASK64) >> 32) & M
    out = {"gpr": {d: res}}
    if rc:
        out["cr"] = _cr0(st.cr, res, st.xer)
    return out


# ---- divide ---------------------------------------------------------------------------
def _h_div(st, w, unsigned):
    # divw PAGE 385: quotient q with dividend = q*divisor + r, 0 <= r < |divisor| for a
    # non-negative dividend and -|divisor| < r <= 0 for a negative one (= truncation toward
    # zero).  "If an attempt is made to perform either of the divisions 0x8000_0000 / -1 or
    # <anything> / 0, then the contents of rD are undefined, as are the contents of the LT,
    # GT, and EQ bits of the CR0 field (if Rc = 1). In this case, if OE = 1 then OV is set."
    # divwu PAGE 386: unsigned, 0 <= r < divisor; CR0 by signed comparison of the result;
    # "<anything> / 0" leaves rD and CR0 LT/GT/EQ undefined, OV set if OE = 1.
    name, page = ("divwu", 386) if unsigned else ("divw", 385)
    d, a, b, oe, rc = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20), _bits(w, 21, 21), _bits(w, 31, 31)
    A, B = st.gpr[a], st.gpr[b]
    if unsigned:
        bad = B == 0
    else:
        bad = B == 0 or (A == 0x80000000 and B == 0xFFFFFFFF)
    if bad:
        xer = _with_ov(st.xer, 1) if oe else st.xer
        defined = {"pc": (st.pc + 4) & M, "xer": xer}
        undefined = [f"gpr[{d}]"]
        if rc:
            # CR0[SO] is still a copy of XER[SO] (PAGE 339); every other CR bit unchanged.
            defined["cr"] = (st.cr & 0x0FFFFFFF) | (_so(xer) << 28)
            defined["cr_mask"] = 0x1FFFFFFF
            undefined.append("cr[0:2]")
        raise UndefinedBehaviour(
            f"{name}: rD{' and CR0 LT/GT/EQ' if rc else ''} undefined for "
            f"{'<anything>/0' if B == 0 else '0x80000000/-1'} PAGE {page}",
            defined=defined, undefined=undefined)
    if unsigned:
        q = A // B
    else:
        sa, sb = _sx32(A), _sx32(B)
        q = abs(sa) // abs(sb)
        if (sa < 0) != (sb < 0):
            q = -q
    res = q & M
    xer = _with_ov(st.xer, 0) if oe else st.xer  # no overflow condition (PAGE 92)
    out = {"gpr": {d: res}, "xer": xer}
    if rc:
        out["cr"] = _cr0(st.cr, res, xer)
    return out


# ---- group 31 X-form ------------------------------------------------------------------
def _h_cmp(st, w, logical):
    # cmp PAGE 365 (signed) / cmpl PAGE 367 (unsigned): "CR[(4*crfD)-(4*crfD+3)] <- c ||
    # XER[SO]"; "If L = 1 the instruction form is invalid."; encoding bit 9 and bit 31 Reserved.
    name, page = ("cmpl", 367) if logical else ("cmp", 365)
    crf, r9, L, a, b, r31 = (_bits(w, 6, 8), _bits(w, 9, 9), _bits(w, 10, 10), _bits(w, 11, 15),
                             _bits(w, 16, 20), _bits(w, 31, 31))
    if L:
        raise UndefinedBehaviour(f"{name} with L=1: invalid form PAGE {page}, "
                                 "limited support for invalid forms PAGE 87")
    if r9 or r31:
        _reserved(name, page)
    if logical:
        x, y = st.gpr[a], st.gpr[b]
    else:
        x, y = _sx32(st.gpr[a]), _sx32(st.gpr[b])
    c = 0b100 if x < y else (0b010 if x > y else 0b001)
    return {"cr": _set_field(st.cr, crf, (c << 1) | _so(st.xer))}


_LOGIC = {
    28: ("and", 354, lambda s, b: s & b),              # rA <- (rS) & (rB)
    60: ("andc", 355, lambda s, b: s & ~b & M),         # rA <- (rS) & not(rB)
    444: ("or", 489, lambda s, b: s | b),               # rA <- (rS) | (rB)
    412: ("orc", 490, lambda s, b: (s | ~b) & M),       # rA <- (rS) | not(rB)
    316: ("xor", 580, lambda s, b: s ^ b),              # rA <- (rS) xor (rB)
    476: ("nand", 486, lambda s, b: ~(s & b) & M),      # rA <- not((rS) & (rB))
    124: ("nor", 488, lambda s, b: ~(s | b) & M),       # rA <- not((rS) | (rB))
    284: ("eqv", 391, lambda s, b: ~(s ^ b) & M),       # rA <- (rS) == (rB)
}


def _h_logic(st, w, xo):
    # CR0 if Rc = 1 (nand PAGE 486 lists no "Affected" line; the Rc definition PAGE 339
    # applies).  No XER bits: PAGE 93.
    _, _, fn = _LOGIC[xo]
    s, a, b, rc = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20), _bits(w, 31, 31)
    res = fn(st.gpr[s], st.gpr[b])
    out = {"gpr": {a: res}}
    if rc:
        out["cr"] = _cr0(st.cr, res, st.xer)
    return out


def _h_unary(st, w, op):
    # extsb PAGE 392: "S <- rS[24]; rA[24-31] <- rS[24-31]; rA[0-23] <- (24)S"
    # extsh PAGE 393: "S <- rS[16]; rA[16-31] <- rS[16-31]; rA[0-15] <- (16)S"
    #   (the prose "Bit 48 of rS" is a 64-bit leftover; the pseudocode rS[16] is used)
    # cntlzw PAGE 369: count of consecutive zero bits from bit 0, 0..32; "If Rc = 1, then LT
    #   is cleared in the CR0 field."
    page = {"extsb": 392, "extsh": 393, "cntlzw": 369}[op]
    s, a, b, rc = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20), _bits(w, 31, 31)
    if b:
        _reserved(op, page)  # encoding "31 S A 0 0 0 0 0 xo Rc"
    v = st.gpr[s]
    if op == "extsb":
        res = (v & 0xFF) | (0xFFFFFF00 if v & 0x80 else 0)
    elif op == "extsh":
        res = (v & 0xFFFF) | (0xFFFF0000 if v & 0x8000 else 0)
    else:
        n = 0
        while n < 32 and _bits(v, n, n) == 0:
            n += 1
        res = n
    out = {"gpr": {a: res}}
    if rc:
        out["cr"] = _cr0(st.cr, res, st.xer)
    return out


def _h_shift(st, w, op):
    s, a, f, rc = _bits(w, 6, 10), _bits(w, 11, 15), _bits(w, 16, 20), _bits(w, 31, 31)
    rs = st.gpr[s]
    xer = st.xer
    if op == "srawi":
        # srawi PAGE 540: "n <- SH; r <- ROTL(rS, 32 - n); m <- MASK(n, 31); S <- rS(0);
        # rA <- r & m | (32)S & not m; XER[CA] <- S(0) & ((r & not m) != 0)"
        n = f
        r = _rotl(rs, 32 - n)
        m = _mask(n, 31)
        S = _bits(rs, 0, 0)
        res = (r & m) | ((M if S else 0) & ~m & M)
        xer = _with_ca(xer, S & (1 if (r & ~m & M) else 0))
    else:
        rb = st.gpr[f]
        n = _bits(rb, 27, 31)
        big = _bits(rb, 26, 26)
        if op == "slw":
            # slw PAGE 538: "r <- ROTL(rS, n); if rB[26] = 0 then m <- MASK(0, 31 - n)
            # else m <- (32)0; rA <- r & m"
            r = _rotl(rs, n)
            m = 0 if big else _mask(0, 31 - n)
            res = r & m
        elif op == "srw":
            # srw PAGE 541: "r <- ROTL(rS, 32 - n); if rB[26] = 0 then m <- MASK(n, 31)
            # else m <- (32)0; rA <- r & m"
            r = _rotl(rs, 32 - n)
            m = 0 if big else _mask(n, 31)
            res = r & m
        else:
            # sraw PAGE 539: as srw for r/m; "S <- rS(0); rA <- r & m | (32)S & not m;
            # XER[CA] <- S & (r & not m[0-31] != 0)"; prose: "shift amounts from 32 to 63
            # give a result of 32 sign bits, and cause XER[CA] to receive the sign bit of rS"
            r = _rotl(rs, 32 - n)
            m = 0 if big else _mask(n, 31)
            S = _bits(rs, 0, 0)
            res = (r & m) | ((M if S else 0) & ~m & M)
            xer = _with_ca(xer, S & (1 if (r & ~m & M) else 0))
    out = {"gpr": {a: res & M}}
    if xer != st.xer or op in ("sraw", "srawi"):
        out["xer"] = xer
    if rc:
        out["cr"] = _cr0(st.cr, res & M, xer)
    return out


def _h_mcrxr(st, w):
    # mcrxr PAGE 458: "CR[4*crfD, 4*crfD+3] <- XER[0-3]; XER[0-3] <- 0b0000"; encoding
    # 31 crfD 00 00000 00000 512 0 (reserved bits 9-20, 31).
    if _bits(w, 9, 20) or _bits(w, 31, 31):
        _reserved("mcrxr", 458)
    crf = _bits(w, 6, 8)
    return {"cr": _set_field(st.cr, crf, _bits(st.xer, 0, 3)), "xer": st.xer & 0x0FFFFFFF}


def _h_mfcr(st, w):
    # mfcr PAGE 459: "rD <- CR"; encoding 31 D 00000 00000 19 0 (reserved bits 11-20, 31).
    if _bits(w, 11, 20) or _bits(w, 31, 31):
        _reserved("mfcr", 459)
    return {"gpr": {_bits(w, 6, 10): st.cr}}


def _h_mtcrf(st, w):
    # mtcrf PAGE 470: "mask <- (4)(CRM[0]) || (4)(CRM[1]) || ... (4)(CRM[7]);
    # CR <- (rS & mask) | (CR & not mask)"; encoding 31 S 0 CRM 0 144 0 (bits 12-19 = CRM;
    # reserved bits 11, 20, 31).
    if _bits(w, 11, 11) or _bits(w, 20, 20) or _bits(w, 31, 31):
        _reserved("mtcrf", 470)
    s, crm = _bits(w, 6, 10), _bits(w, 12, 19)
    mask = 0
    for i in range(8):
        if (crm >> (7 - i)) & 1:
            mask |= 0xF << (28 - 4 * i)
    return {"cr": ((st.gpr[s] & mask) | (st.cr & ~mask)) & M}


# --------------------------------------------------------------------------------------
# Decode tables.  Field positions: OPCD 0-5, XO 21-30 (X/XL form) or 22-30 (XO form with OE
# at 21) PAGE 339; opcode assignments from the per-instruction pages and Appendix A
# (PAGE 583-588).
# --------------------------------------------------------------------------------------
_PRIMARY = {
    7: ("mulli", _h_mulli),
    8: ("subfic", _h_subfic),
    10: ("cmpli", lambda st, w: _h_cmpi_l(st, w, True)),
    11: ("cmpi", lambda st, w: _h_cmpi_l(st, w, False)),
    12: ("addic", lambda st, w: _h_addic(st, w, 0)),
    13: ("addic.", lambda st, w: _h_addic(st, w, 1)),
    14: ("addi", _h_addi),
    15: ("addis", _h_addis),
    20: ("rlwimi", lambda st, w: _h_rlw(st, w, "rlwimi")),
    21: ("rlwinm", lambda st, w: _h_rlw(st, w, "rlwinm")),
    23: ("rlwnm", lambda st, w: _h_rlw(st, w, "rlwnm")),
    24: ("ori", lambda st, w: _h_logic_imm(st, w, "ori")),
    25: ("oris", lambda st, w: _h_logic_imm(st, w, "oris")),
    26: ("xori", lambda st, w: _h_logic_imm(st, w, "xori")),
    27: ("xoris", lambda st, w: _h_logic_imm(st, w, "xoris")),
    28: ("andi.", lambda st, w: _h_logic_imm(st, w, "andi.")),
    29: ("andis.", lambda st, w: _h_logic_imm(st, w, "andis.")),
}

_G31_X10 = {
    0: ("cmp", lambda st, w: _h_cmp(st, w, False)),
    32: ("cmpl", lambda st, w: _h_cmp(st, w, True)),
    19: ("mfcr", _h_mfcr),
    144: ("mtcrf", _h_mtcrf),
    512: ("mcrxr", _h_mcrxr),
    26: ("cntlzw", lambda st, w: _h_unary(st, w, "cntlzw")),
    954: ("extsb", lambda st, w: _h_unary(st, w, "extsb")),
    922: ("extsh", lambda st, w: _h_unary(st, w, "extsh")),
    24: ("slw", lambda st, w: _h_shift(st, w, "slw")),
    536: ("srw", lambda st, w: _h_shift(st, w, "srw")),
    792: ("sraw", lambda st, w: _h_shift(st, w, "sraw")),
    824: ("srawi", lambda st, w: _h_shift(st, w, "srawi")),
}
for _xo, (_nm, _pg, _fn) in _LOGIC.items():
    _G31_X10[_xo] = (_nm, (lambda x: (lambda st, w: _h_logic(st, w, x)))(_xo))

_G31_XO9 = {
    235: ("mullw", _h_mullw),
    75: ("mulhw", lambda st, w: _h_mulhw(st, w, False)),
    11: ("mulhwu", lambda st, w: _h_mulhw(st, w, True)),
    491: ("divw", lambda st, w: _h_div(st, w, False)),
    459: ("divwu", lambda st, w: _h_div(st, w, True)),
}
for _xo, _spec in _ADDSUB.items():
    _G31_XO9[_xo] = (_spec[0], (lambda x: (lambda st, w: _h_addsub(st, w, x)))(_xo))


def _decode(word):
    word &= M
    op = word >> 26
    if op in _PRIMARY:
        return _PRIMARY[op]
    if op == 19:
        xo = _bits(word, 21, 30)
        if xo == 0:
            return ("mcrf", _h_mcrf)
        if xo in _CR_OPS:
            return (_CR_OPS[xo][0], (lambda x: (lambda st, w: _h_crop(st, w, x)))(xo))
        return None
    if op == 31:
        xo10 = _bits(word, 21, 30)
        if xo10 in _G31_X10:
            return _G31_X10[xo10]
        xo9 = _bits(word, 22, 30)
        if xo9 in _G31_XO9:
            return _G31_XO9[xo9]
    return None


def supports(word):
    return _decode(word) is not None


def mnemonic(word):
    d = _decode(word)
    return d[0] if d else None


def step(state, word):
    d = _decode(word)
    if d is None:
        raise ValueError(f"int_A: word {word & M:08X} is not in area {AREA}")
    commit = d[1](state, word & M)  # may raise UndefinedBehaviour before any write
    for i, v in commit.get("gpr", {}).items():
        state.gpr[i] = v & M
    if "xer" in commit:
        state.xer = commit["xer"] & M
    if "cr" in commit:
        state.cr = commit["cr"] & M
    # Non-branching instruction: "the next instruction address is CIA + 4" PAGE 342
    state.pc = (state.pc + 4) & M
    state.exception = None


def step_with_markers(state, word):
    """Like step(), but on UndefinedBehaviour applies the defined parts and returns
    {location: UNDEFINED}; returns {} when everything is defined."""
    try:
        step(state, word)
        return {}
    except UndefinedBehaviour as u:
        u.apply_defined(state)
        return {loc: UNDEFINED for loc in u.undefined}


# ======================================================================================
# Self-test: hand-derived vectors (expected values worked out by hand from the manual text,
# not computed with the helpers above).
# ======================================================================================
if __name__ == "__main__":
    def D(op, d, a, imm):
        return ((op << 26) | (d << 21) | (a << 16) | (imm & 0xFFFF)) & M

    def XO(d, a, b, xo, oe=0, rc=0):
        return ((31 << 26) | (d << 21) | (a << 16) | (b << 11) | (oe << 10) | (xo << 1) | rc) & M

    def X(s, a, b, xo, rc=0):
        return ((31 << 26) | (s << 21) | (a << 16) | (b << 11) | (xo << 1) | rc) & M

    def MF(op, s, a, sh, mb, me, rc=0):
        return ((op << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc) & M

    def XL(d, a, b, xo):
        return ((19 << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1)) & M

    SO, OV, CA = XER_SO, XER_OV, XER_CA
    PC0 = 0x80003100

    # (name, setup, word, expect)   setup/expect keys: gpr {i: v}, xer, cr, pc
    # expect 'undef': (tuple of undefined locations or None for 'all', defined-dict subset)
    V = [
        # ---- addi / addis (rA|0)  PAGE 348, 351
        ("addi rA=0 uses literal 0", {"gpr": {0: 5}}, D(14, 3, 0, 0xFFFF), {"gpr": {3: 0xFFFFFFFF}, "pc": PC0 + 4}),
        ("addi -32768", {"gpr": {4: 0x10000}}, D(14, 3, 4, 0x8000), {"gpr": {3: 0x8000}}),
        ("addis rA=0 lis", {"gpr": {0: 7}}, D(15, 3, 0, 0x8000), {"gpr": {3: 0x80000000}}),
        ("addis -1<<16", {"gpr": {4: 0x12345678}}, D(15, 3, 4, 0xFFFF), {"gpr": {3: 0x12335678}}),
        # ---- addic / addic.  PAGE 349, 350
        ("addic carry out", {"gpr": {3: 0xFFFFFFFF}, "cr": 0x12345678}, D(12, 3, 3, 1),
         {"gpr": {3: 0}, "xer": CA, "cr": 0x12345678}),
        ("addic rA=0 is GPR0", {"gpr": {0: 0}, "xer": CA}, D(12, 0, 0, 0xFFFF), {"gpr": {0: 0xFFFFFFFF}, "xer": 0}),
        ("addic. CR0 EQ|SO", {"gpr": {4: 1}, "xer": SO, "cr": 0x9ABCDEF0}, D(13, 3, 4, 0xFFFF),
         {"gpr": {3: 0}, "xer": SO | CA, "cr": 0x3ABCDEF0}),
        # ---- add family  PAGE 345-347, 352, 353
        ("add no OE leaves XER", {"gpr": {4: 0x7FFFFFFF, 5: 1}, "xer": CA}, XO(3, 4, 5, 266),
         {"gpr": {3: 0x80000000}, "xer": CA}),
        ("addo. overflow", {"gpr": {4: 0x7FFFFFFF, 5: 1}, "xer": CA}, XO(3, 4, 5, 266, 1, 1),
         {"gpr": {3: 0x80000000}, "xer": SO | OV | CA, "cr": 0x90000000}),
        ("addo clears OV keeps SO", {"gpr": {4: 1, 5: 1}, "xer": SO | OV}, XO(3, 4, 5, 266, 1, 0),
         {"gpr": {3: 2}, "xer": SO}),
        ("addc CA", {"gpr": {4: 0xFFFFFFFF, 5: 0xFFFFFFFF}}, XO(3, 4, 5, 10), {"gpr": {3: 0xFFFFFFFE}, "xer": CA}),
        ("addco. 0x80000000+0x80000000", {"gpr": {4: 0x80000000, 5: 0x80000000}}, XO(3, 4, 5, 10, 1, 1),
         {"gpr": {3: 0}, "xer": SO | OV | CA, "cr": 0x30000000}),
        ("adde CA in and out", {"gpr": {4: 0xFFFFFFFF, 5: 0}, "xer": CA}, XO(3, 4, 5, 138), {"gpr": {3: 0}, "xer": CA}),
        ("addeo overflow via CA", {"gpr": {4: 0x7FFFFFFF, 5: 0}, "xer": CA}, XO(3, 4, 5, 138, 1),
         {"gpr": {3: 0x80000000}, "xer": SO | OV}),
        ("addmeo 0 CA=0", {"gpr": {4: 0}}, XO(3, 4, 0, 234, 1), {"gpr": {3: 0xFFFFFFFF}, "xer": 0}),
        ("addmeo 0x80000000 CA=0", {"gpr": {4: 0x80000000}}, XO(3, 4, 0, 234, 1),
         {"gpr": {3: 0x7FFFFFFF}, "xer": SO | OV | CA}),
        ("addze wrap", {"gpr": {4: 0xFFFFFFFF}, "xer": CA}, XO(3, 4, 0, 202), {"gpr": {3: 0}, "xer": CA}),
        ("addzeo. overflow", {"gpr": {4: 0x7FFFFFFF}, "xer": CA}, XO(3, 4, 0, 202, 1, 1),
         {"gpr": {3: 0x80000000}, "xer": SO | OV, "cr": 0x90000000}),
        ("add aliasing r3=r3+r3", {"gpr": {3: 0x40000000}}, XO(3, 3, 3, 266), {"gpr": {3: 0x80000000}}),
        # ---- subtract family  PAGE 569-574, 487
        ("subf rB-rA", {"gpr": {4: 5, 5: 3}, "xer": CA}, XO(3, 4, 5, 40), {"gpr": {3: 0xFFFFFFFE}, "xer": CA}),
        ("subfo overflow", {"gpr": {4: 1, 5: 0x80000000}}, XO(3, 4, 5, 40, 1), {"gpr": {3: 0x7FFFFFFF}, "xer": SO | OV}),
        ("subf aliasing r3-r3", {"gpr": {3: 0xDEADBEEF}}, XO(3, 3, 3, 40, 0, 1), {"gpr": {3: 0}, "cr": 0x20000000}),
        ("subfc 0-0 sets CA", {"gpr": {4: 0, 5: 0}}, XO(3, 4, 5, 8), {"gpr": {3: 0}, "xer": CA}),
        ("subfc 0-1 clears CA", {"gpr": {4: 1, 5: 0}, "xer": CA}, XO(3, 4, 5, 8), {"gpr": {3: 0xFFFFFFFF}, "xer": 0}),
        ("subfe CA=0", {"gpr": {4: 0, 5: 0}}, XO(3, 4, 5, 136), {"gpr": {3: 0xFFFFFFFF}, "xer": 0}),
        ("subfe CA=1", {"gpr": {4: 0, 5: 0}, "xer": CA}, XO(3, 4, 5, 136), {"gpr": {3: 0}, "xer": CA}),
        ("subfic 0-0", {"gpr": {4: 0}}, D(8, 3, 4, 0), {"gpr": {3: 0}, "xer": CA}),
        ("subfic 5-6", {"gpr": {4: 6}, "xer": CA}, D(8, 3, 4, 5), {"gpr": {3: 0xFFFFFFFF}, "xer": 0}),
        ("subfic -1-0 rA=0 is GPR0", {"gpr": {0: 0}}, D(8, 3, 0, 0xFFFF), {"gpr": {3: 0xFFFFFFFF}, "xer": CA}),
        ("subfme -1 CA=0", {"gpr": {4: 0xFFFFFFFF}}, XO(3, 4, 0, 232), {"gpr": {3: 0xFFFFFFFF}, "xer": 0}),
        ("subfme -1 CA=1", {"gpr": {4: 0xFFFFFFFF}, "xer": CA}, XO(3, 4, 0, 232), {"gpr": {3: 0}, "xer": CA}),
        ("subfmeo 0x7FFFFFFF", {"gpr": {4: 0x7FFFFFFF}}, XO(3, 4, 0, 232, 1),
         {"gpr": {3: 0x7FFFFFFF}, "xer": SO | OV | CA}),
        ("subfze 0 CA=1", {"gpr": {4: 0}, "xer": CA}, XO(3, 4, 0, 200), {"gpr": {3: 0}, "xer": CA}),
        ("subfzeo 0x80000000 CA=1", {"gpr": {4: 0x80000000}, "xer": CA}, XO(3, 4, 0, 200, 1),
         {"gpr": {3: 0x80000000}, "xer": SO | OV}),
        ("neg keeps CA", {"gpr": {4: 1}, "xer": CA}, XO(3, 4, 0, 104), {"gpr": {3: 0xFFFFFFFF}, "xer": CA}),
        ("nego. most negative aliasing", {"gpr": {3: 0x80000000}}, XO(3, 3, 0, 104, 1, 1),
         {"gpr": {3: 0x80000000}, "xer": SO | OV, "cr": 0x90000000}),
        ("nego 0 clears OV", {"gpr": {4: 0}, "xer": SO | OV}, XO(3, 4, 0, 104, 1), {"gpr": {3: 0}, "xer": SO}),
        # ---- multiply  PAGE 482-485
        ("mulli low bits", {"gpr": {4: 0x40000000}}, D(7, 3, 4, 0xFFFE), {"gpr": {3: 0x80000000}}),
        ("mulli -1*3", {"gpr": {4: 0xFFFFFFFF}}, D(7, 3, 4, 3), {"gpr": {3: 0xFFFFFFFD}}),
        ("mullwo 2^32 overflow", {"gpr": {4: 0x10000, 5: 0x10000}}, XO(3, 4, 5, 235, 1),
         {"gpr": {3: 0}, "xer": SO | OV}),
        ("mullwo -1*-2^31", {"gpr": {4: 0xFFFFFFFF, 5: 0x80000000}}, XO(3, 4, 5, 235, 1),
         {"gpr": {3: 0x80000000}, "xer": SO | OV}),
        ("mullwo -1*(2^31-1) fits", {"gpr": {4: 0xFFFFFFFF, 5: 0x7FFFFFFF}, "xer": OV}, XO(3, 4, 5, 235, 1),
         {"gpr": {3: 0x80000001}, "xer": 0}),
        ("mullw. no OE", {"gpr": {4: 0x10000, 5: 0x10000}, "xer": SO}, XO(3, 4, 5, 235, 0, 1),
         {"gpr": {3: 0}, "xer": SO, "cr": 0x30000000}),
        ("mulhw -1*-1", {"gpr": {4: 0xFFFFFFFF, 5: 0xFFFFFFFF}}, XO(3, 4, 5, 75), {"gpr": {3: 0}}),
        ("mulhwu -1*-1", {"gpr": {4: 0xFFFFFFFF, 5: 0xFFFFFFFF}}, XO(3, 4, 5, 11), {"gpr": {3: 0xFFFFFFFE}}),
        ("mulhw. -2^31*2", {"gpr": {4: 0x80000000, 5: 2}}, XO(3, 4, 5, 75, 0, 1),
         {"gpr": {3: 0xFFFFFFFF}, "cr": 0x80000000}),
        ("mulhwu. 2^31*2^31", {"gpr": {4: 0x80000000, 5: 0x80000000}}, XO(3, 4, 5, 11, 0, 1),
         {"gpr": {3: 0x40000000}, "cr": 0x40000000}),
        ("mulhwu. signed CR0 compare", {"gpr": {4: 0xFFFFFFFF, 5: 0xFFFFFFFF}}, XO(3, 4, 5, 11, 0, 1),
         {"gpr": {3: 0xFFFFFFFE}, "cr": 0x80000000}),
        ("mulhw with OE bit = reserved", {}, XO(3, 4, 5, 75, 1), {"undef": (("all",), {})}),
        # ---- divide  PAGE 385, 386
        ("divw -7/2 truncates", {"gpr": {4: 0xFFFFFFF9, 5: 2}}, XO(3, 4, 5, 491), {"gpr": {3: 0xFFFFFFFD}}),
        ("divw 7/-2", {"gpr": {4: 7, 5: 0xFFFFFFFE}}, XO(3, 4, 5, 491), {"gpr": {3: 0xFFFFFFFD}}),
        ("divwo. normal clears OV", {"gpr": {4: 100, 5: 10}, "xer": OV}, XO(3, 4, 5, 491, 1, 1),
         {"gpr": {3: 10}, "xer": 0, "cr": 0x40000000}),
        ("divwo. 0x80000000/-1 undefined", {"gpr": {3: 0x55, 4: 0x80000000, 5: 0xFFFFFFFF}, "cr": 0x0000000F},
         XO(3, 4, 5, 491, 1, 1),
         {"undef": (("gpr[3]", "cr[0:2]"), {"xer": SO | OV, "cr": 0x1000000F, "cr_mask": 0x1FFFFFFF, "pc": PC0 + 4})}),
        ("divw /0 undefined no OE", {"gpr": {4: 1, 5: 0}, "xer": CA}, XO(3, 4, 5, 491),
         {"undef": (("gpr[3]",), {"xer": CA, "pc": PC0 + 4})}),
        ("divwu 0xFFFFFFFF/2", {"gpr": {4: 0xFFFFFFFF, 5: 2}}, XO(3, 4, 5, 459), {"gpr": {3: 0x7FFFFFFF}}),
        ("divwu. signed CR0", {"gpr": {4: 0xFFFFFFFF, 5: 1}, "xer": SO}, XO(3, 4, 5, 459, 0, 1),
         {"gpr": {3: 0xFFFFFFFF}, "cr": 0x90000000}),
        ("divwuo /0 sets OV", {"gpr": {4: 9, 5: 0}}, XO(3, 4, 5, 459, 1),
         {"undef": (("gpr[3]",), {"xer": SO | OV, "pc": PC0 + 4})}),
        ("divwuo 10/3", {"gpr": {4: 10, 5: 3}, "xer": SO | OV}, XO(3, 4, 5, 459, 1), {"gpr": {3: 3}, "xer": SO}),
        # ---- compares  PAGE 365-368
        ("cmpw cr7 signed LT + SO", {"gpr": {4: 0xFFFFFFFF, 5: 1}, "xer": SO}, X(7 << 2, 4, 5, 0),
         {"cr": 0x00000009}),
        ("cmplw cr7 unsigned GT", {"gpr": {4: 0xFFFFFFFF, 5: 1}, "cr": 0xFFFFFFF0}, X(7 << 2, 4, 5, 32),
         {"cr": 0xFFFFFFF4}),
        ("cmpwi cr0 -1 EQ", {"gpr": {3: 0xFFFFFFFF}, "cr": 0xF0000000}, D(11, 0, 3, 0xFFFF), {"cr": 0x20000000}),
        ("cmplwi cr1 0xFFFF GT", {"gpr": {3: 0xFFFFFFFF}}, D(10, 1 << 2, 3, 0xFFFF), {"cr": 0x04000000}),
        ("cmp L=1 invalid", {}, X((0 << 2) | 1, 4, 5, 0), {"undef": (("all",), {})}),
        # ---- logical  PAGE 354-357, 391, 486-492, 580-582
        ("and.", {"gpr": {4: 0xF0F0F0F0, 5: 0xFF00FF00}, "xer": SO | CA}, X(4, 3, 5, 28, 1),
         {"gpr": {3: 0xF000F000}, "xer": SO | CA, "cr": 0x90000000}),
        ("andc", {"gpr": {4: 0xF0F0F0F0, 5: 0xFF00FF00}}, X(4, 3, 5, 60), {"gpr": {3: 0x00F000F0}}),
        ("or", {"gpr": {4: 0xF0F0F0F0, 5: 0xFF00FF00}}, X(4, 3, 5, 444), {"gpr": {3: 0xFFF0FFF0}}),
        ("orc", {"gpr": {4: 0xF0F0F0F0, 5: 0xFF00FF00}}, X(4, 3, 5, 412), {"gpr": {3: 0xF0FFF0FF}}),
        ("xor.", {"gpr": {4: 0xF0F0F0F0, 5: 0xFF00FF00}}, X(4, 3, 5, 316, 1), {"gpr": {3: 0x0FF00FF0}, "cr": 0x40000000}),
        ("nand", {"gpr": {4: 0xF0F0F0F0, 5: 0xFF00FF00}}, X(4, 3, 5, 476), {"gpr": {3: 0x0FFF0FFF}}),
        ("nor (not r4)", {"gpr": {4: 0xF0F0F0F0}}, X(4, 3, 4, 124), {"gpr": {3: 0x0F0F0F0F}}),
        ("eqv", {"gpr": {4: 0xF0F0F0F0, 5: 0xFF00FF00}}, X(4, 3, 5, 284), {"gpr": {3: 0xF00FF00F}}),
        ("andi. GT", {"gpr": {4: 0xFFFFFFFF}}, D(28, 4, 3, 0x8000), {"gpr": {3: 0x8000}, "cr": 0x40000000}),
        ("andis. LT", {"gpr": {4: 0xFFFFFFFF}, "xer": SO}, D(29, 4, 3, 0x8000), {"gpr": {3: 0x80000000}, "cr": 0x90000000}),
        ("ori", {"gpr": {4: 0x12340000}}, D(24, 4, 3, 0xFFFF), {"gpr": {3: 0x1234FFFF}}),
        ("oris", {"gpr": {4: 1}}, D(25, 4, 3, 0x8000), {"gpr": {3: 0x80000001}}),
        ("xori", {"gpr": {4: 0xFFFF}, "cr": 0x12345678}, D(26, 4, 3, 0xFFFF), {"gpr": {3: 0}, "cr": 0x12345678}),
        ("xoris", {"gpr": {4: 0xFFFFFFFF}}, D(27, 4, 3, 0xFFFF), {"gpr": {3: 0x0000FFFF}}),
        # ---- extsb / extsh / cntlzw  PAGE 392, 393, 369
        ("extsb negative", {"gpr": {4: 0x12345680}}, X(4, 3, 0, 954), {"gpr": {3: 0xFFFFFF80}}),
        ("extsb. positive", {"gpr": {4: 0xFFFFFF7F}}, X(4, 3, 0, 954, 1), {"gpr": {3: 0x7F}, "cr": 0x40000000}),
        ("extsh", {"gpr": {4: 0x00018000}}, X(4, 3, 0, 922), {"gpr": {3: 0xFFFF8000}}),
        ("cntlzw 0 -> 32", {"gpr": {4: 0}}, X(4, 3, 0, 26), {"gpr": {3: 32}}),
        ("cntlzw. 0x00010000 -> 15", {"gpr": {4: 0x00010000}}, X(4, 3, 0, 26, 1), {"gpr": {3: 15}, "cr": 0x40000000}),
        ("cntlzw. MSB -> 0 EQ", {"gpr": {4: 0x80000000}}, X(4, 3, 0, 26, 1), {"gpr": {3: 0}, "cr": 0x20000000}),
        ("extsb rB field reserved", {}, X(4, 3, 1, 954), {"undef": (("all",), {})}),
        # ---- rotates  PAGE 533-536, MASK PAGE 341
        ("rlwinm rotate+mask", {"gpr": {4: 0x12345678}}, MF(21, 4, 3, 8, 24, 31), {"gpr": {3: 0x12}}),
        ("rlwinm wrapping mask", {"gpr": {4: 0xFFFFFFFF}}, MF(21, 4, 3, 0, 30, 1), {"gpr": {3: 0xC0000003}}),
        ("rlwinm MB=ME+1 all ones", {"gpr": {4: 0x12345678}}, MF(21, 4, 3, 0, 16, 15), {"gpr": {3: 0x12345678}}),
        ("rlwinm. MB=ME single bit", {"gpr": {4: 0x00000001}}, MF(21, 4, 3, 31, 31, 31, 1),
         {"gpr": {3: 0}, "cr": 0x20000000}),
        ("rlwimi insert", {"gpr": {3: 0xAAAAAAAA, 4: 0x12345678}}, MF(20, 4, 3, 8, 24, 31), {"gpr": {3: 0xAAAAAA12}}),
        ("rlwimi aliasing rS=rA", {"gpr": {3: 0x0000FFFF}}, MF(20, 3, 3, 16, 0, 15), {"gpr": {3: 0xFFFFFFFF}}),
        ("rlwnm uses rB[27-31]", {"gpr": {4: 0x80000001, 5: 0xFFFFFFE4}}, MF(23, 4, 3, 5, 0, 31), {"gpr": {3: 0x18}}),
        # ---- shifts  PAGE 538-541
        ("slw 31", {"gpr": {4: 0xFFFFFFFF, 5: 31}}, X(4, 3, 5, 24), {"gpr": {3: 0x80000000}}),
        ("slw 32 -> 0", {"gpr": {4: 0xFFFFFFFF, 5: 32}}, X(4, 3, 5, 24), {"gpr": {3: 0}}),
        ("slw 0x40: only rB[26-31] used -> unchanged", {"gpr": {4: 0xFFFFFFFF, 5: 0x40}}, X(4, 3, 5, 24), {"gpr": {3: 0xFFFFFFFF}}),
        ("srw 31", {"gpr": {4: 0x80000000, 5: 31}}, X(4, 3, 5, 536), {"gpr": {3: 1}}),
        ("srw. 63 -> 0", {"gpr": {4: 0x80000000, 5: 63}}, X(4, 3, 5, 536, 1), {"gpr": {3: 0}, "cr": 0x20000000}),
        ("sraw 1 CA set", {"gpr": {4: 0x80000001, 5: 1}}, X(4, 3, 5, 792), {"gpr": {3: 0xC0000000}, "xer": CA}),
        ("sraw 1 CA clear", {"gpr": {4: 0x80000000, 5: 1}, "xer": CA}, X(4, 3, 5, 792), {"gpr": {3: 0xC0000000}, "xer": 0}),
        ("sraw 32 negative", {"gpr": {4: 0x80000000, 5: 32}}, X(4, 3, 5, 792), {"gpr": {3: 0xFFFFFFFF}, "xer": CA}),
        ("sraw 63 positive", {"gpr": {4: 0x7FFFFFFF, 5: 63}, "xer": CA}, X(4, 3, 5, 792), {"gpr": {3: 0}, "xer": 0}),
        ("sraw 0 copies, CA clear", {"gpr": {4: 0x80000001, 5: 0}, "xer": CA}, X(4, 3, 5, 792),
         {"gpr": {3: 0x80000001}, "xer": 0}),
        ("sraw aliasing rA=rB", {"gpr": {4: 0xFFFFFF00, 5: 4}}, X(4, 5, 5, 792), {"gpr": {5: 0xFFFFFFF0}, "xer": 0}),
        ("srawi 4 CA set", {"gpr": {4: 0xFFFFFFF1}}, X(4, 3, 4, 824), {"gpr": {3: 0xFFFFFFFF}, "xer": CA}),
        ("srawi 0 CA clear", {"gpr": {4: 0xFFFFFFF1}, "xer": CA | SO}, X(4, 3, 0, 824, 1),
         {"gpr": {3: 0xFFFFFFF1}, "xer": SO, "cr": 0x90000000}),
        ("srawi positive no CA", {"gpr": {4: 0x11}, "xer": CA}, X(4, 3, 4, 824), {"gpr": {3: 1}, "xer": 0}),
        # ---- CR moves / CR logical  PAGE 456, 458, 459, 470, 370-377
        ("mcrf 2<-5", {"cr": 0x00000A00}, XL(2 << 2, 5 << 2, 0, 0), {"cr": 0x00A00A00}),
        ("mcrxr cr3", {"xer": 0xE000007F}, X(3 << 2, 0, 0, 512), {"cr": 0x000E0000, "xer": 0x0000007F}),
        ("mfcr", {"cr": 0x12345678}, X(3, 0, 0, 19), {"gpr": {3: 0x12345678}}),
        ("mtcrf 0x81", {"gpr": {3: 0xFFFFFFFF}, "cr": 0x01234560}, (31 << 26) | (3 << 21) | (0x81 << 12) | (144 << 1),
         {"cr": 0xF123456F}),
        ("mtcr 0xFF", {"gpr": {3: 0x13579BDF}, "cr": 0xFFFFFFFF}, (31 << 26) | (3 << 21) | (0xFF << 12) | (144 << 1),
         {"cr": 0x13579BDF}),
        ("crclr 6 (crxor 6,6,6)", {"cr": 0xFFFFFFFF}, XL(6, 6, 6, 193), {"cr": 0xFDFFFFFF}),
        ("crset 0 (creqv 0,0,0)", {"cr": 0}, XL(0, 0, 0, 289), {"cr": 0x80000000}),
        ("crnot 31,2", {"cr": 0x20000001}, XL(31, 2, 2, 33), {"cr": 0x20000000}),
        ("crorc 1,2,3", {"cr": 0}, XL(1, 2, 3, 417), {"cr": 0x40000000}),
        ("crandc 4,0,1", {"cr": 0x80000000}, XL(4, 0, 1, 129), {"cr": 0x88000000}),
        ("crnand 5,0,1", {"cr": 0xC4000000}, XL(5, 0, 1, 225), {"cr": 0xC0000000}),
        ("crand 7,0,1", {"cr": 0xC0000000}, XL(7, 0, 1, 257), {"cr": 0xC1000000}),
        ("cror 8,9,10", {"cr": 0x00200000}, XL(8, 9, 10, 449), {"cr": 0x00A00000}),
        ("mfcr bit31 reserved", {}, X(3, 0, 0, 19, 1), {"undef": (("all",), {})}),
        ("addme rB reserved", {}, XO(3, 4, 1, 234), {"undef": (("all",), {})}),
    ]

    fails = 0
    for name, setup, word, expect in V:
        st = State()
        for k, v in setup.items():
            if k == "gpr":
                for i, gv in v.items():
                    st.gpr[i] = gv
            else:
                setattr(st, k, v)
        before = st.clone()
        if "undef" in expect:
            locs, defined = expect["undef"]
            try:
                step(st, word)
                print(f"FAIL {name}: expected UndefinedBehaviour")
                fails += 1
                continue
            except UndefinedBehaviour as u:
                if st.arch_dict() != before.arch_dict():
                    print(f"FAIL {name}: state modified before raise")
                    fails += 1
                    continue
                if "PAGE" not in u.what:
                    print(f"FAIL {name}: no PAGE citation in {u.what!r}")
                    fails += 1
                if tuple(u.undefined) != tuple(locs):
                    print(f"FAIL {name}: undefined {u.undefined} != {locs}")
                    fails += 1
                for k, v in defined.items():
                    if u.defined.get(k) != v:
                        print(f"FAIL {name}: defined[{k}] = {u.defined.get(k)!r} != {v!r}")
                        fails += 1
            continue
        try:
            step(st, word)
        except UndefinedBehaviour as u:
            print(f"FAIL {name}: unexpected UndefinedBehaviour {u.what}")
            fails += 1
            continue
        exp_pc = expect.get("pc", PC0 + 4)
        if st.pc != exp_pc:
            print(f"FAIL {name}: pc {st.pc:08X} != {exp_pc:08X}")
            fails += 1
        for i in range(32):
            want = expect.get("gpr", {}).get(i, before.gpr[i])
            if st.gpr[i] != want:
                print(f"FAIL {name}: gpr[{i}] {st.gpr[i]:08X} != {want:08X}")
                fails += 1
        for k in ("xer", "cr"):
            want = expect.get(k, getattr(before, k))
            if getattr(st, k) != want:
                print(f"FAIL {name}: {k} {getattr(st, k):08X} != {want:08X}")
                fails += 1
        if st.exception is not None:
            print(f"FAIL {name}: exception {st.exception}")
            fails += 1

    # step_with_markers: defined parts applied, markers returned (divw /0, OE=1, Rc=1)
    st = State()
    st.gpr[4], st.gpr[5], st.gpr[3], st.cr = 1, 0, 0x77, 0x2000000F
    mk = step_with_markers(st, XO(3, 4, 5, 491, 1, 1))
    if not (mk == {"gpr[3]": UNDEFINED, "cr[0:2]": UNDEFINED} and st.xer == SO | OV
            and st.cr == 0x3000000F and st.pc == PC0 + 4 and st.gpr[3] == 0x77):
        print(f"FAIL step_with_markers: {mk} xer={st.xer:08X} cr={st.cr:08X} pc={st.pc:08X}")
        fails += 1

    # Opcode identity: base words from each instruction page header x'....' (PAGE 345-582)
    HDR = {
        0x7C000214: "add", 0x7C000014: "addc", 0x7C000114: "adde", 0x38000000: "addi",
        0x30000000: "addic", 0x34000000: "addic.", 0x3C000000: "addis", 0x7C0001D4: "addme",
        0x7C000194: "addze", 0x7C000038: "and", 0x7C000078: "andc", 0x70000000: "andi.",
        0x74000000: "andis.", 0x7C000000: "cmp", 0x2C000000: "cmpi", 0x7C000040: "cmpl",
        0x28000000: "cmpli", 0x7C000034: "cntlzw", 0x4C000202: "crand", 0x4C000102: "crandc",
        0x4C000242: "creqv", 0x4C0001C2: "crnand", 0x4C000042: "crnor", 0x4C000382: "cror",
        0x4C000342: "crorc", 0x4C000182: "crxor", 0x7C0003D6: "divw", 0x7C000396: "divwu",
        0x7C000238: "eqv", 0x7C000774: "extsb", 0x7C000734: "extsh", 0x4C000000: "mcrf",
        0x7C000400: "mcrxr", 0x7C000026: "mfcr", 0x7C000120: "mtcrf", 0x7C000096: "mulhw",
        0x7C000016: "mulhwu", 0x1C000000: "mulli", 0x7C0001D6: "mullw", 0x7C0003B8: "nand",
        0x7C0000D0: "neg", 0x7C0000F8: "nor", 0x7C000378: "or", 0x7C000338: "orc",
        0x60000000: "ori", 0x64000000: "oris", 0x50000000: "rlwimi", 0x54000000: "rlwinm",
        0x5C000000: "rlwnm", 0x7C000030: "slw", 0x7C000630: "sraw", 0x7C000670: "srawi",
        0x7C000430: "srw", 0x7C000050: "subf", 0x7C000010: "subfc", 0x7C000110: "subfe",
        0x20000000: "subfic", 0x7C0001D0: "subfme", 0x7C000190: "subfze", 0x7C000278: "xor",
        0x68000000: "xori", 0x6C000000: "xoris",
    }
    for w, nm in HDR.items():
        if mnemonic(w) != nm:
            print(f"FAIL header {w:08X}: {mnemonic(w)} != {nm}")
            fails += 1
        if nm in ("add", "addc", "adde", "addme", "addze", "subf", "subfc", "subfe", "subfme",
                  "subfze", "neg", "mullw", "divw", "divwu") and mnemonic(w | 0x400) != nm:
            print(f"FAIL OE form of {nm} not decoded")
            fails += 1

    # Non-area group 19/31 extended opcodes (Appendix A, PAGE 583-588) must NOT decode here,
    # with OE bit both clear and set where it would alias a 9-bit XO.
    OTHER31 = [4, 20, 23, 54, 55, 83, 86, 87, 119, 146, 150, 151, 183, 210, 215, 242, 246, 247,
               278, 279, 306, 310, 311, 339, 343, 371, 375, 407, 438, 439, 467, 470, 533, 534, 535,
               566, 567, 595, 597, 598, 599, 631, 659, 661, 662, 663, 695, 725, 727, 759, 790, 854,
               918, 982, 983, 1014]
    for xo in OTHER31:
        w = (31 << 26) | (xo << 1)
        if supports(w):
            print(f"FAIL group31 XO {xo} wrongly supported as {mnemonic(w)}")
            fails += 1
    for w in (0x4E800020, 0x4E800420, 0x4C000064, 0x4C00012C, 0x0C000000, 0x00000000,
              0x48000000, 0x40000000, 0x44000002, 0x80000000, 0xFC000000, 0x10000000):
        if supports(w):
            print(f"FAIL {w:08X} wrongly supported as {mnemonic(w)}")
            fails += 1

    total = len(V) + 1
    print(f"{total} semantic vectors, {len(HDR)} opcode headers, {len(OTHER31)} exclusion checks; "
          f"failures: {fails}")
    sys.exit(1 if fails else 0)
