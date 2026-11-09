"""Independent Gekko specification model, area "int", author B.

SOURCES (the only ones used):
  G = docs/reference/ppc/gekko_user_manual.txt  (cited as "PAGE N", the file's ===== PAGE N ===== marker)
  C = docs/reference/ppc/ppc_750cl.txt          (cited as "750CL PAGE N")
No GekkoForge implementation, test, or audit document was consulted.

COVERED
  primary  7 mulli, 8 subfic, 10 cmpli, 11 cmpi, 12 addic, 13 addic., 14 addi, 15 addis,
          20 rlwimi, 21 rlwinm, 23 rlwnm, 24 ori, 25 oris, 26 xori, 27 xoris, 28 andi., 29 andis.
  19/xo10 0 mcrf, 33 crnor, 129 crandc, 193 crxor, 225 crnand, 257 crand, 289 creqv, 417 crorc, 449 cror
  31/xo9  (XO form, OE = bit 21) 8 subfc, 10 addc, 11 mulhwu, 40 subf, 75 mulhw, 104 neg, 136 subfe,
          138 adde, 200 subfze, 202 addze, 232 subfme, 234 addme, 235 mullw, 266 add, 459 divwu, 491 divw
  31/xo10 (X form) 0 cmp, 19 mfcr, 24 slw, 26 cntlzw, 28 and, 32 cmpl, 60 andc, 124 nor, 144 mtcrf,
          284 eqv, 316 xor, 412 orc, 444 or, 476 nand, 512 mcrxr, 536 srw, 792 sraw, 824 srawi,
          922 extsh, 954 extsb
  NOT covered: tw (31/4) and twi (3) - they invoke the trap handler (exception area).

UNDEFINED-BEHAVIOUR CONTRACT
  step() raises UndefinedBehaviour when the manual leaves (part of) the outcome undefined.
    exc.applied == False : the whole instruction is boundedly undefined (invalid form / reserved bits
                           set); state was NOT modified at all.
    exc.applied == True  : every DEFINED effect (XER, CR0[SO], pc += 4, ...) HAS been applied to the
                           state; the fields listed in exc.undefined (a dict of field -> UNDEFINED) were
                           left untouched and must not be compared. exc.gpr_undefined is a tuple of GPR
                           indices, exc.cr_undefined_mask a u32 mask of CR bits whose value is undefined.

BIT NUMBERING: big-endian, bit 0 = MSB (PAGE 89 "The default byte and bit ordering is big-endian";
confirmed by extsb PAGE 392: rS[24-31] are "the low-order eight bits").

XER LAYOUT - EXTERNAL ASSUMPTION: neither manual in the corpus states the bit positions of SO/OV/CA
(PAGE 345 defers to "Section 2.1.5, XER Register" of the Programming Environments manual, which is not
in the corpus). The PowerPC architectural layout SO = XER[0], OV = XER[1], CA = XER[2] is assumed.
Consistent with (but not proven by) mcrxr PAGE 458, which moves XER[0-3] as a CR field.
"""
from __future__ import annotations

from gekko_state import MASK32, MASK64, State  # noqa: F401  (State used by tests / type hints)

AREA = "int"

# --- XER bits (EXTERNAL ASSUMPTION, see module docstring) --------------------------------------
XER_SO = 1 << (31 - 0)
XER_OV = 1 << (31 - 1)
XER_CA = 1 << (31 - 2)


class _UndefinedMarker:
    """Value marker for a result the manual declares undefined (PAGE 342 'Undefined')."""

    _inst = None

    def __new__(cls):
        if cls._inst is None:
            cls._inst = super().__new__(cls)
        return cls._inst

    def __repr__(self):
        return "UNDEFINED"


UNDEFINED = _UndefinedMarker()


class UndefinedBehaviour(Exception):
    def __init__(self, what, *, applied=False, gpr_undefined=(), cr_undefined_mask=0):
        super().__init__(what)
        self.what = what
        self.applied = applied
        self.gpr_undefined = tuple(gpr_undefined)
        self.cr_undefined_mask = cr_undefined_mask & MASK32
        self.undefined = {}
        for g in self.gpr_undefined:
            self.undefined["gpr[%d]" % g] = UNDEFINED
        if self.cr_undefined_mask:
            self.undefined["cr&0x%08X" % self.cr_undefined_mask] = UNDEFINED


# --- field helpers (big-endian bit numbering, PAGE 89) ------------------------------------------
def _f(word, a, b):
    """Bits a..b (inclusive, big-endian numbering) of a 32-bit word."""
    return (word >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def _bit(value, n):
    """value[n], big-endian numbering (Table 12-3 'x[n]', PAGE 341)."""
    return (value >> (31 - n)) & 1


def _s32(x):
    x &= MASK32
    return x - (1 << 32) if x & 0x80000000 else x


def _exts16(imm):
    # EXTS(SIMM): "extending x on the left with sign bits" PAGE 341; d/SIMM sign-extended to 32 bits PAGE 338
    imm &= 0xFFFF
    return (imm - 0x10000) if imm & 0x8000 else imm


def _rotl(x, n):
    # ROTL(x, y): "rotating the value x left y positions, where x is 32 bits long" PAGE 342.
    # A rotation by 32 is the identity (32 positions of a 32-bit value).
    x &= MASK32
    n %= 32
    return ((x << n) | (x >> (32 - n))) & MASK32 if n else x


def _mask(x, y):
    # MASK(x, y): "Mask having ones in positions x through y (wrapping if x > y) and zeros elsewhere" PAGE 341
    m = 0
    if x <= y:
        positions = range(x, y + 1)
    else:
        positions = list(range(x, 32)) + list(range(0, y + 1))
    for p in positions:
        m |= 1 << (31 - p)
    return m


def _ca(state):
    return 1 if state.xer & XER_CA else 0


def _set_ca(state, ca):
    state.xer = (state.xer & ~XER_CA & MASK32) | (XER_CA if ca else 0)


def _set_ov(state, ov):
    # 'o': "record of an overflow in XER[OV] and CR0[SO]" PAGE 340.
    # OV reflects the overflow of THIS result (cleared when none) - PAGE 92 "SO and OV ... are set to
    # reflect an overflow condition of a 32-bit result. This can happen only when OE = 1."
    # SO is the *summary* overflow bit (PAGE 339 / PAGE 41 "summary overflow bit"): OR-ed in, never
    # cleared by an arithmetic instruction (only mcrxr clears it here, PAGE 458).  [interpretation]
    state.xer &= ~XER_OV & MASK32
    if ov:
        state.xer |= XER_OV | XER_SO


def _set_crf(state, n, field4):
    # CR[(4*crfD)-(4*crfD+3)] PAGE 365; field bits are LT, GT, EQ, SO (cmp pseudocode c || XER[SO])
    shift = 28 - 4 * n
    state.cr = (state.cr & ~(0xF << shift) & MASK32) | ((field4 & 0xF) << shift)


def _get_crf(cr, n):
    return (cr >> (28 - 4 * n)) & 0xF


def _so(state):
    return 1 if state.xer & XER_SO else 0


def _cr0(state, result):
    # Rc = 1: "CR bits 0-2 are set to reflect the result as a signed quantity and CR bit 3 receives a
    # copy of the summary overflow bit, XER[SO]" PAGE 339. The copy is of XER[SO] AFTER this
    # instruction's own OV/SO update: PAGE 340 'o' records the overflow "in XER[OV] and CR0[SO]".
    v = _s32(result)
    c = 0b100 if v < 0 else (0b010 if v > 0 else 0b001)
    _set_crf(state, 0, (c << 1) | _so(state))


def _reserved(name, page, word, *ranges):
    # "If a reserved field does not have all bits cleared ... the instruction form is invalid" PAGE 337;
    # "incorrectly set bits in reserved fields ... boundedly undefined" PAGE 87.
    for a, b in ranges:
        if _f(word, a, b):
            raise UndefinedBehaviour(
                "%s: reserved bits %d-%d not zero -> invalid form, boundedly undefined (PAGE %d, PAGE 337, PAGE 87)"
                % (name, a, b, page))


def _next(state):
    # Non-branching instruction: NIA = CIA + 4 (PAGE 342). No integer instruction takes an exception.
    state.pc = (state.pc + 4) & MASK32
    state.exception = None


# --- decode tables ------------------------------------------------------------------------------
_PRIMARY = {7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.", 14: "addi",
            15: "addis", 20: "rlwimi", 21: "rlwinm", 23: "rlwnm", 24: "ori", 25: "oris", 26: "xori",
            27: "xoris", 28: "andi.", 29: "andis."}
# group 19, XO in bits 21-30 (Appendix A PAGE 584-585)
_G19 = {0: "mcrf", 33: "crnor", 129: "crandc", 193: "crxor", 225: "crnand", 257: "crand", 289: "creqv",
        417: "crorc", 449: "cror"}
# group 31 XO form: OE = bit 21, XO = bits 22-30 (Table 12-2 'OE (21)', PAGE 339; XO '22-30' PAGE 339)
_G31_XO = {8: "subfc", 10: "addc", 11: "mulhwu", 40: "subf", 75: "mulhw", 104: "neg", 136: "subfe",
           138: "adde", 200: "subfze", 202: "addze", 232: "subfme", 234: "addme", 235: "mullw", 266: "add",
           459: "divwu", 491: "divw"}
# group 31 X form: XO = bits 21-30
_G31_X = {0: "cmp", 19: "mfcr", 24: "slw", 26: "cntlzw", 28: "and", 32: "cmpl", 60: "andc", 124: "nor",
          144: "mtcrf", 284: "eqv", 316: "xor", 412: "orc", 444: "or", 476: "nand", 512: "mcrxr",
          536: "srw", 792: "sraw", 824: "srawi", 922: "extsh", 954: "extsb"}


def mnemonic(word):
    """Base mnemonic of a supported word (without o/. suffixes), or None."""
    word &= MASK32
    op = word >> 26
    if op in _PRIMARY:
        return _PRIMARY[op]
    if op == 19:
        return _G19.get(_f(word, 21, 30))
    if op == 31:
        x10 = _f(word, 21, 30)
        if x10 in _G31_X:
            return _G31_X[x10]
        return _G31_XO.get(_f(word, 22, 30))
    return None


def supports(word):
    return mnemonic(word) is not None


# --- instruction semantics ----------------------------------------------------------------------
def _arith(state, name, d, terms_u, terms_s, *, ca_affected, oe, rc):
    """Shared XO-form / D-form addition.

    terms_u are the operands of the pseudocode sum as unsigned 32-bit bit strings (carry-in as 0/1);
    terms_s are the SAME operands interpreted as signed integers (PAGE 340 '+' is two's-complement
    addition). [interpretation] CA = carry out of bit 0 of the unsigned sum ('c' PAGE 340 "a carry out
    in XER[CA]"); OV = the signed sum is not representable in 32 bits (PAGE 92 "overflow condition of
    a 32-bit result"). For subf-family operands ¬(rA) is -rA-1 and (32)1 / 0xFFFF_FFFF is -1, exactly
    as the pseudocode "¬(rA) + XER[CA] – 1" (PAGE 573) and "(rA) + XER[CA] – 1" (PAGE 352) write it.
    """
    total_u = sum(terms_u)
    result = total_u & MASK32
    total_s = sum(terms_s)
    assert (total_s & MASK32) == result, name
    state.gpr[d] = result
    if ca_affected:
        _set_ca(state, total_u >> 32)
    if oe:
        _set_ov(state, not (-(1 << 31) <= total_s <= (1 << 31) - 1))
    if rc:
        _cr0(state, result)


def _exec_primary(state, word, op):
    d = _f(word, 6, 10)       # rD / rS / crfD..L  (PAGE 338-339)
    a = _f(word, 11, 15)      # rA
    imm = _f(word, 16, 31)    # SIMM / UIMM
    ra = state.gpr[a]
    rs = state.gpr[d]

    if op == 14:  # addi PAGE 348: "if rA = 0 then rD <- EXTS(SIMM) else rD <- (rA) + EXTS(SIMM)"; no XER/CR
        state.gpr[d] = (_exts16(imm) + (ra if a else 0)) & MASK32
    elif op == 15:  # addis PAGE 351: (rA|0) + (SIMM || (16)0); "Other registers altered: None"
        state.gpr[d] = (((imm << 16) & MASK32) + (ra if a else 0)) & MASK32
    elif op == 12:  # addic PAGE 349: rD <- (rA) + EXTS(SIMM) (rA, NOT rA|0); XER[CA] affected
        _arith(state, "addic", d, [ra, _exts16(imm) & MASK32], [_s32(ra), _exts16(imm)],
               ca_affected=True, oe=False, rc=False)
    elif op == 13:  # addic. PAGE 350: as addic, plus CR0 LT GT EQ SO; no OV (no OE field)
        _arith(state, "addic.", d, [ra, _exts16(imm) & MASK32], [_s32(ra), _exts16(imm)],
               ca_affected=True, oe=False, rc=True)
    elif op == 8:  # subfic PAGE 572: rD <- ¬(rA) + EXTS(SIMM) + 1; XER[CA] affected; no CR
        nra = (~ra) & MASK32
        _arith(state, "subfic", d, [nra, _exts16(imm) & MASK32, 1], [_s32(nra), _exts16(imm), 1],
               ca_affected=True, oe=False, rc=False)
    elif op == 7:  # mulli PAGE 484: "The low-order 32-bits of the 48-bit product" (signed); no XER/CR.
        # [interpretation] the pseudocode "rD <- prod[16-48]" is a 33-bit slice typo; the text governs.
        state.gpr[d] = (_s32(ra) * _exts16(imm)) & MASK32
    elif op in (10, 11):  # cmpli PAGE 368 / cmpi PAGE 366
        name = "cmpi" if op == 11 else "cmpli"
        page = 366 if op == 11 else 368
        _reserved(name, page, word, (9, 9))
        if _f(word, 10, 10):
            # "If L = 1 the instruction form is invalid." PAGE 366/368; results not given in corpus (PAGE 337)
            raise UndefinedBehaviour("%s: L = 1 -> invalid form (PAGE %d, PAGE 337)" % (name, page))
        crf = _f(word, 6, 8)
        if op == 11:  # signed compare with EXTS(SIMM)
            x, y = _s32(ra), _exts16(imm)
        else:         # unsigned compare with (16)0 || UIMM
            x, y = ra, imm
        c = 0b100 if x < y else (0b010 if x > y else 0b001)
        _set_crf(state, crf, (c << 1) | _so(state))  # "CR[...] <- c || XER[SO]"
    elif op == 24:  # ori PAGE 491: rA <- (rS) | ((16)0 || UIMM); no CR
        state.gpr[a] = rs | imm
    elif op == 25:  # oris PAGE 492: rA <- (rS) | (UIMM || (16)0)
        state.gpr[a] = (rs | (imm << 16)) & MASK32
    elif op == 26:  # xori PAGE 581
        state.gpr[a] = rs ^ imm
    elif op == 27:  # xoris PAGE 582
        state.gpr[a] = (rs ^ (imm << 16)) & MASK32
    elif op == 28:  # andi. PAGE 356: rA <- (rS) & ((16)0 || UIMM); CR0 LT GT EQ SO always; XER untouched (PAGE 93)
        state.gpr[a] = rs & imm
        _cr0(state, state.gpr[a])
    elif op == 29:  # andis. PAGE 357: rA <- (rS) & (UIMM || (16)0)
        state.gpr[a] = rs & (imm << 16)
        _cr0(state, state.gpr[a])
    elif op in (20, 21, 23):
        sh_or_b = _f(word, 16, 20)
        mb = _f(word, 21, 25)
        me = _f(word, 26, 30)
        rc = _f(word, 31, 31)
        if op == 23:  # rlwnm PAGE 536: n <- rB[27-31] (only the low-order five bits of rB)
            n = _f(state.gpr[sh_or_b], 27, 31)
        else:         # rlwimi PAGE 533 / rlwinm PAGE 534: n <- SH
            n = sh_or_b
        r = _rotl(rs, n)
        m = _mask(mb, me)
        if op == 20:  # rlwimi: rA <- (r & m) | (rA & ¬m)   (old rA read before the write)
            res = (r & m) | (ra & ~m & MASK32)
        else:         # rlwinm / rlwnm: rA <- r & m
            res = r & m
        state.gpr[a] = res
        if rc:
            _cr0(state, res)
    else:  # pragma: no cover
        raise AssertionError(op)


def _exec_g19(state, word, xo):
    name = _G19[xo]
    if xo == 0:  # mcrf PAGE 456: CR[4*crfD..+3] <- CR[4*crfS..+3]
        _reserved("mcrf", 456, word, (9, 10), (14, 20), (31, 31))
        _set_crf(state, _f(word, 6, 8), _get_crf(state.cr, _f(word, 11, 13)))
        return
    # CR logical ops, XL form; bit 31 reserved (encoding "crbB <xo> 0", PAGE 370-377)
    _reserved(name, {257: 370, 129: 371, 289: 372, 225: 373, 33: 374, 449: 375, 417: 376, 193: 377}[xo],
              word, (31, 31))
    bd, ba, bb = _f(word, 6, 10), _f(word, 11, 15), _f(word, 16, 20)
    x, y = _bit(state.cr, ba), _bit(state.cr, bb)   # both sources read before the write
    if xo == 257:    # crand  PAGE 370: CR[crbA] & CR[crbB]
        v = x & y
    elif xo == 129:  # crandc PAGE 371: CR[crbA] & ¬CR[crbB]
        v = x & (1 - y)
    elif xo == 289:  # creqv  PAGE 372: CR[crbA] ≡ CR[crbB]
        v = 1 - (x ^ y)
    elif xo == 225:  # crnand PAGE 373: ¬(CR[crbA] & CR[crbB])
        v = 1 - (x & y)
    elif xo == 33:   # crnor  PAGE 374: ¬(CR[crbA] | CR[crbB])
        v = 1 - (x | y)
    elif xo == 449:  # cror   PAGE 375: CR[crbA] | CR[crbB]
        v = x | y
    elif xo == 417:  # crorc  PAGE 376: CR[crbA] | ¬CR[crbB]
        v = x | (1 - y)
    elif xo == 193:  # crxor  PAGE 377: CR[crbA] ⊕ CR[crbB]
        v = x ^ y
    else:  # pragma: no cover
        raise AssertionError(xo)
    bitmask = 1 << (31 - bd)
    state.cr = (state.cr & ~bitmask & MASK32) | (bitmask if v else 0)   # only bit crbD affected


def _exec_g31_xo(state, word, xo):
    name = _G31_XO[xo]
    d, a, b = _f(word, 6, 10), _f(word, 11, 15), _f(word, 16, 20)
    oe, rc = _f(word, 21, 21), _f(word, 31, 31)
    ra, rb = state.gpr[a], state.gpr[b]
    nra = (~ra) & MASK32
    ca = _ca(state)

    if xo == 266:  # add PAGE 345: rD <- (rA) + (rB); CR0 if Rc; SO OV if OE; CA NOT affected
        _arith(state, name, d, [ra, rb], [_s32(ra), _s32(rb)], ca_affected=False, oe=oe, rc=rc)
    elif xo == 10:  # addc PAGE 346: rD <- (rA) + (rB); CA affected
        _arith(state, name, d, [ra, rb], [_s32(ra), _s32(rb)], ca_affected=True, oe=oe, rc=rc)
    elif xo == 138:  # adde PAGE 347: rD <- (rA) + (rB) + XER[CA]
        _arith(state, name, d, [ra, rb, ca], [_s32(ra), _s32(rb), ca], ca_affected=True, oe=oe, rc=rc)
    elif xo == 234:  # addme PAGE 352: (rA) + XER[CA] + 0xFFFF_FFFF ; bits 16-20 reserved
        _reserved(name, 352, word, (16, 20))
        _arith(state, name, d, [ra, ca, MASK32], [_s32(ra), ca, -1], ca_affected=True, oe=oe, rc=rc)
    elif xo == 202:  # addze PAGE 353: (rA) + XER[CA] ; bits 16-20 reserved
        _reserved(name, 353, word, (16, 20))
        _arith(state, name, d, [ra, ca], [_s32(ra), ca], ca_affected=True, oe=oe, rc=rc)
    elif xo == 40:  # subf PAGE 569: rD <- ¬(rA) + (rB) + 1; CA NOT affected
        _arith(state, name, d, [nra, rb, 1], [_s32(nra), _s32(rb), 1], ca_affected=False, oe=oe, rc=rc)
    elif xo == 8:  # subfc PAGE 570: rD <- ¬(rA) + (rB) + 1; CA affected
        _arith(state, name, d, [nra, rb, 1], [_s32(nra), _s32(rb), 1], ca_affected=True, oe=oe, rc=rc)
    elif xo == 136:  # subfe PAGE 571: rD <- ¬(rA) + (rB) + XER[CA]
        _arith(state, name, d, [nra, rb, ca], [_s32(nra), _s32(rb), ca], ca_affected=True, oe=oe, rc=rc)
    elif xo == 232:  # subfme PAGE 573: ¬(rA) + XER[CA] + (32)1 ; bits 16-20 reserved
        _reserved(name, 573, word, (16, 20))
        _arith(state, name, d, [nra, ca, MASK32], [_s32(nra), ca, -1], ca_affected=True, oe=oe, rc=rc)
    elif xo == 200:  # subfze PAGE 574: ¬(rA) + XER[CA] ; bits 16-20 reserved
        _reserved(name, 574, word, (16, 20))
        _arith(state, name, d, [nra, ca], [_s32(nra), ca], ca_affected=True, oe=oe, rc=rc)
    elif xo == 104:  # neg PAGE 487: rD <- ¬(rA) + 1; 0x8000_0000 -> itself and OV set if OE; CA NOT affected
        _reserved(name, 487, word, (16, 20))
        _arith(state, name, d, [nra, 1], [_s32(nra), 1], ca_affected=False, oe=oe, rc=rc)
    elif xo == 235:  # mullw PAGE 485: low-order 32 bits of the product; OV if the signed product
        # "cannot be represented in 32 bits". [interpretation] "prod[16-48]" slice is a typo; text governs.
        p = _s32(ra) * _s32(rb)
        state.gpr[d] = p & MASK32
        if oe:
            _set_ov(state, not (-(1 << 31) <= p <= (1 << 31) - 1))
        if rc:
            _cr0(state, state.gpr[d])
    elif xo in (75, 11):  # mulhw PAGE 482 / mulhwu PAGE 483: high-order 32 bits of the 64-bit product
        # bit 21 is "Reserved" in both encodings (no OE form exists).
        _reserved(name, 482 if xo == 75 else 483, word, (21, 21))
        if xo == 75:  # "Both the operands and the product are interpreted as signed integers"
            state.gpr[d] = ((_s32(ra) * _s32(rb)) & MASK64) >> 32
        else:         # unsigned operands; CR0 still by signed comparison of the result to zero
            state.gpr[d] = (ra * rb) >> 32
        if rc:
            _cr0(state, state.gpr[d])
    elif xo in (491, 459):  # divw PAGE 385 / divwu PAGE 386
        page = 385 if xo == 491 else 386
        if xo == 491:
            x, y = _s32(ra), _s32(rb)
            bad = (y == 0) or (x == -(1 << 31) and y == -1)
        else:
            x, y = ra, rb
            bad = (y == 0)
        if bad:
            # "then the contents of rD are undefined, as are the contents of the LT, GT, and EQ bits of the
            # CR0 field (if Rc = 1). In this case, if OE = 1 then OV is set." PAGE 385 / PAGE 386.
            # Defined parts: OV (and summary SO) if OE=1; CR0[SO] = XER[SO] if Rc=1; XER[CA] untouched.
            if oe:
                _set_ov(state, True)
            if rc:
                state.cr = (state.cr & ~(1 << 28) & MASK32) | (_so(state) << 28)
            _next(state)
            raise UndefinedBehaviour(
                "%s: division %s -> rD%s undefined (PAGE %d)"
                % (name, "0x8000_0000 / -1" if y != 0 else "<anything> / 0",
                   " and CR0 LT/GT/EQ" if rc else "", page),
                applied=True, gpr_undefined=(d,), cr_undefined_mask=(0xE0000000 if rc else 0))
        if xo == 491:
            # quotient: dividend = quotient*divisor + r, 0 <= r < |divisor| for dividend >= 0,
            # -|divisor| < r <= 0 for dividend < 0  -> truncation toward zero (PAGE 385)
            q = abs(x) // abs(y)
            if (x < 0) != (y < 0):
                q = -q
        else:
            q = x // y  # unsigned; 0 <= r < divisor (PAGE 386)
        state.gpr[d] = q & MASK32
        if oe:
            _set_ov(state, False)   # [interpretation] OV reflects "overflow condition of a 32-bit result" PAGE 92
        if rc:
            _cr0(state, state.gpr[d])
    else:  # pragma: no cover
        raise AssertionError(xo)


def _exec_g31_x(state, word, xo):
    name = _G31_X[xo]
    s, a, b = _f(word, 6, 10), _f(word, 11, 15), _f(word, 16, 20)
    rc = _f(word, 31, 31)
    rs, ra, rb = state.gpr[s], state.gpr[a], state.gpr[b]

    if xo in (0, 32):  # cmp PAGE 365 / cmpl PAGE 367
        page = 365 if xo == 0 else 367
        _reserved(name, page, word, (9, 9), (31, 31))
        if _f(word, 10, 10):
            raise UndefinedBehaviour("%s: L = 1 -> invalid form (PAGE %d, PAGE 337)" % (name, page))
        crf = _f(word, 6, 8)
        if xo == 0:
            x, y = _s32(ra), _s32(rb)   # "treating the operands as signed integers"
        else:
            x, y = ra, rb               # "treating the operands as unsigned integers"
        c = 0b100 if x < y else (0b010 if x > y else 0b001)
        _set_crf(state, crf, (c << 1) | _so(state))
        return
    if xo == 19:  # mfcr PAGE 459: rD <- CR; bits 11-20 and 31 reserved
        _reserved(name, 459, word, (11, 20), (31, 31))
        state.gpr[s] = state.cr
        return
    if xo == 144:  # mtcrf PAGE 470: mask <- (4)CRM[0] || ... || (4)CRM[7]; CR <- (rS & mask) | (CR & ¬mask)
        _reserved(name, 470, word, (11, 11), (20, 20), (31, 31))
        crm = _f(word, 12, 19)
        mask = 0
        for i in range(8):
            if _bit(crm << 24, i):
                mask |= 0xF << (28 - 4 * i)
        state.cr = (rs & mask) | (state.cr & ~mask & MASK32)
        return
    if xo == 512:  # mcrxr PAGE 458: CR[4*crfD..+3] <- XER[0-3]; XER[0-3] <- 0b0000
        _reserved(name, 458, word, (9, 20), (31, 31))
        _set_crf(state, _f(word, 6, 8), state.xer >> 28)
        state.xer &= 0x0FFFFFFF
        return

    # Logical / shift / count ops write rA; logical ops "do not affect XER[SO], XER[OV], or XER[CA]" PAGE 93
    if xo == 28:     # and  PAGE 354
        res = rs & rb
    elif xo == 60:   # andc PAGE 355
        res = rs & ~rb & MASK32
    elif xo == 444:  # or   PAGE 489
        res = rs | rb
    elif xo == 412:  # orc  PAGE 490
        res = (rs | ~rb) & MASK32
    elif xo == 124:  # nor  PAGE 488
        res = ~(rs | rb) & MASK32
    elif xo == 476:  # nand PAGE 486 [interpretation: its "Affected:" line is missing; Rc rule PAGE 339 applies]
        res = ~(rs & rb) & MASK32
    elif xo == 284:  # eqv  PAGE 391: (rS) ≡ (rB)
        res = ~(rs ^ rb) & MASK32
    elif xo == 316:  # xor  PAGE 580
        res = rs ^ rb
    elif xo == 954:  # extsb PAGE 392: S <- rS[24]; rA[24-31] <- rS[24-31]; rA[0-23] <- (24)S
        _reserved(name, 392, word, (16, 20))
        res = (rs & 0xFF) | (0xFFFFFF00 if _bit(rs, 24) else 0)
    elif xo == 922:  # extsh PAGE 393: S <- rS[16]; rA[16-31] <- rS[16-31]; rA[0-15] <- (16)S
        # [interpretation] text "Bit 48 of rS" is a 64-bit leftover; the 32-bit pseudocode (PAGE 343) governs.
        _reserved(name, 393, word, (16, 20))
        res = (rs & 0xFFFF) | (0xFFFF0000 if _bit(rs, 16) else 0)
    elif xo == 26:   # cntlzw PAGE 369: n <- 0; do while n < 32: if rS[n] = 1 then leave; n <- n + 1
        _reserved(name, 369, word, (16, 20))
        n = 0
        while n < 32:
            if _bit(rs, n):
                break
            n += 1
        res = n
    elif xo == 24:   # slw PAGE 538: n <- rB[27-31]; r <- ROTL(rS,n); m <- MASK(0,31-n) if rB[26]=0 else (32)0
        n = _f(rb, 27, 31)
        r = _rotl(rs, n)
        m = _mask(0, 31 - n) if _bit(rb, 26) == 0 else 0
        res = r & m
    elif xo == 536:  # srw PAGE 541: r <- ROTL(rS, 32-n); m <- MASK(n,31) if rB[26]=0 else (32)0
        n = _f(rb, 27, 31)
        r = _rotl(rs, 32 - n)
        m = _mask(n, 31) if _bit(rb, 26) == 0 else 0
        res = r & m
    elif xo in (792, 824):  # sraw PAGE 539 / srawi PAGE 540
        if xo == 792:
            n = _f(rb, 27, 31)
            m = _mask(n, 31) if _bit(rb, 26) == 0 else 0
        else:
            n = b  # SH field, bits 16-20
            m = _mask(n, 31)
        r = _rotl(rs, 32 - n)
        sgn = _bit(rs, 0)
        res = (r & m) | ((MASK32 if sgn else 0) & ~m & MASK32)   # rA <- r & m | (32)S & ¬m
        # XER[CA] <- S & ((r & ¬m) ≠ 0)  - "set if rS contains a negative number and any 1 bits are shifted
        # out of position 31"; for sraw amounts 32-63 "cause XER[CA] to receive the sign bit of rS".
        _set_ca(state, sgn & (1 if (r & ~m & MASK32) != 0 else 0))
    else:  # pragma: no cover
        raise AssertionError(xo)
    state.gpr[a] = res
    if rc:
        _cr0(state, res)


def step(state, word):
    """Execute the integer instruction `word` located at state.pc."""
    word &= MASK32
    op = word >> 26
    if op in _PRIMARY:
        _exec_primary(state, word, op)
    elif op == 19 and _f(word, 21, 30) in _G19:
        _exec_g19(state, word, _f(word, 21, 30))
    elif op == 31 and _f(word, 21, 30) in _G31_X:
        _exec_g31_x(state, word, _f(word, 21, 30))
    elif op == 31 and _f(word, 22, 30) in _G31_XO:
        _exec_g31_xo(state, word, _f(word, 22, 30))
    else:
        raise ValueError("int_B does not support 0x%08X" % word)
    _next(state)


# ================================================================================================
# Self test: hand-derived vectors
# ================================================================================================
if __name__ == "__main__":
    import sys

    # ---- encoders built from the manual's field layouts (PAGE 338-339, Appendix A PAGE 583-588) ----
    def enc_d(op, d, a, imm):
        return ((op << 26) | (d << 21) | (a << 16) | (imm & 0xFFFF)) & MASK32

    def enc_xo(xo, d, a, b, oe=0, rc=0):
        return (31 << 26) | (d << 21) | (a << 16) | (b << 11) | (oe << 10) | (xo << 1) | rc

    def enc_x(xo, s, a, b, rc=0):
        return (31 << 26) | (s << 21) | (a << 16) | (b << 11) | (xo << 1) | rc

    def enc_m(op, s, a, sh, mb, me, rc=0):
        return (op << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc

    def enc_xl(xo, d, a, b):
        return (19 << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1)

    def enc_cmp(xo, crf, a, b, L=0):
        return (31 << 26) | (crf << 23) | (L << 21) | (a << 16) | (b << 11) | (xo << 1)

    def enc_cmpi(op, crf, a, imm, L=0):
        return (op << 26) | (crf << 23) | (L << 21) | (a << 16) | (imm & 0xFFFF)

    SO, OV, CA = XER_SO, XER_OV, XER_CA
    fails = []
    count = [0]

    def mk(gpr=None, xer=0, cr=0):
        st = State()
        st.pc = 0x80003100
        st.xer = xer
        st.cr = cr
        for k, v in (gpr or {}).items():
            st.gpr[k] = v & MASK32
        return st

    def check(label, word, pre, exp_gpr=None, exp_xer=None, exp_cr=None):
        count[0] += 1
        st = mk(**pre)
        before = st.clone()
        try:
            step(st, word)
        except Exception as e:  # noqa: BLE001
            fails.append("%s: raised %r" % (label, e))
            return
        errs = []
        for k, v in (exp_gpr or {}).items():
            if st.gpr[k] != v & MASK32:
                errs.append("gpr[%d]=0x%08X want 0x%08X" % (k, st.gpr[k], v & MASK32))
        exp_xer = before.xer if exp_xer is None else exp_xer
        exp_cr = before.cr if exp_cr is None else exp_cr
        if st.xer != exp_xer:
            errs.append("xer=0x%08X want 0x%08X" % (st.xer, exp_xer))
        if st.cr != exp_cr:
            errs.append("cr=0x%08X want 0x%08X" % (st.cr, exp_cr))
        if st.pc != (before.pc + 4) & MASK32:
            errs.append("pc not advanced")
        if st.mem.log:
            errs.append("memory touched")  # PAGE 86: "Arithmetic and logical instructions do not read or modify memory"
        if st.exception is not None:
            errs.append("exception %r" % st.exception)
        # untouched GPRs must stay untouched
        for i in range(32):
            if i not in (exp_gpr or {}) and st.gpr[i] != before.gpr[i]:
                errs.append("gpr[%d] clobbered" % i)
        if errs:
            fails.append("%s (0x%08X): %s" % (label, word, "; ".join(errs)))

    def check_undef(label, word, pre, applied, exp_xer=None, exp_cr_defined=None, gpr_undef=(), crmask=0):
        count[0] += 1
        st = mk(**pre)
        before = st.clone()
        try:
            step(st, word)
        except UndefinedBehaviour as e:
            errs = []
            if e.applied != applied:
                errs.append("applied=%r" % e.applied)
            if "PAGE" not in e.what:
                errs.append("no page citation")
            if not applied:
                if st.arch_dict() != before.arch_dict():
                    errs.append("state modified on invalid form")
            else:
                if tuple(e.gpr_undefined) != tuple(gpr_undef):
                    errs.append("gpr_undefined=%r" % (e.gpr_undefined,))
                if e.cr_undefined_mask != crmask:
                    errs.append("cr mask 0x%08X" % e.cr_undefined_mask)
                if exp_xer is not None and st.xer != exp_xer:
                    errs.append("xer=0x%08X want 0x%08X" % (st.xer, exp_xer))
                if exp_cr_defined is not None and (st.cr & ~crmask & MASK32) != exp_cr_defined:
                    errs.append("defined cr bits=0x%08X want 0x%08X" % (st.cr & ~crmask & MASK32, exp_cr_defined))
                if st.pc != before.pc + 4:
                    errs.append("pc not advanced")
                for i in range(32):
                    if st.gpr[i] != before.gpr[i]:
                        errs.append("gpr[%d] was written (undefined value invented)" % i)
            if errs:
                fails.append("%s: %s" % (label, "; ".join(errs)))
            return
        except Exception as e:  # noqa: BLE001
            fails.append("%s: wrong exception %r" % (label, e))
            return
        fails.append("%s: did not raise UndefinedBehaviour" % label)

    # ---- decode: every x'....' base opcode printed in the chapter-12 headers ----
    for hexw, nm in [(0x7C000214, "add"), (0x7C000014, "addc"), (0x7C000114, "adde"), (0x38000000, "addi"),
                     (0x30000000, "addic"), (0x34000000, "addic."), (0x3C000000, "addis"), (0x7C0001D4, "addme"),
                     (0x7C000194, "addze"), (0x7C000038, "and"), (0x7C000078, "andc"), (0x70000000, "andi."),
                     (0x74000000, "andis."), (0x7C000000, "cmp"), (0x2C000000, "cmpi"), (0x7C000040, "cmpl"),
                     (0x28000000, "cmpli"), (0x7C000034, "cntlzw"), (0x4C000202, "crand"), (0x4C000102, "crandc"),
                     (0x4C000242, "creqv"), (0x4C0001C2, "crnand"), (0x4C000042, "crnor"), (0x4C000382, "cror"),
                     (0x4C000342, "crorc"), (0x4C000182, "crxor"), (0x7C0003D6, "divw"), (0x7C000396, "divwu"),
                     (0x7C000238, "eqv"), (0x7C000774, "extsb"), (0x7C000734, "extsh"), (0x4C000000, "mcrf"),
                     (0x7C000400, "mcrxr"), (0x7C000026, "mfcr"), (0x7C000120, "mtcrf"), (0x7C000096, "mulhw"),
                     (0x7C000016, "mulhwu"), (0x1C000000, "mulli"), (0x7C0001D6, "mullw"), (0x7C0003B8, "nand"),
                     (0x7C0000D0, "neg"), (0x7C0000F8, "nor"), (0x7C000378, "or"), (0x7C000338, "orc"),
                     (0x60000000, "ori"), (0x64000000, "oris"), (0x50000000, "rlwimi"), (0x54000000, "rlwinm"),
                     (0x5C000000, "rlwnm"), (0x7C000030, "slw"), (0x7C000630, "sraw"), (0x7C000670, "srawi"),
                     (0x7C000430, "srw"), (0x7C000050, "subf"), (0x7C000010, "subfc"), (0x7C000110, "subfe"),
                     (0x20000000, "subfic"), (0x7C0001D0, "subfme"), (0x7C000190, "subfze"), (0x7C000278, "xor"),
                     (0x68000000, "xori"), (0x6C000000, "xoris")]:
        count[0] += 1
        if mnemonic(hexw) != nm:
            fails.append("decode x'%08X' -> %r want %r" % (hexw, mnemonic(hexw), nm))
    for w in (0x7C000008, 0x0C000000, 0x7C0000A6, 0x4C000020, 0x00000000, 0x7C0004AC):  # tw, twi, mfmsr, bclr, 0, sync
        count[0] += 1
        if supports(w):
            fails.append("supports(0x%08X) should be False" % w)

    # ---- addi / addis (PAGE 348, 351) ----
    check("addi rA=0 uses 0", enc_d(14, 3, 0, -1), dict(gpr={0: 0x12345678}, xer=CA), {3: 0xFFFFFFFF})
    check("addi no carry/ov effect", enc_d(14, 3, 4, 0x7FFF), dict(gpr={4: 0x7FFFFFFF}, xer=SO | CA), {3: 0x80007FFE})
    check("addis rA=0 (lis)", enc_d(15, 3, 0, 0x8000), dict(gpr={0: 5}), {3: 0x80000000})
    check("addis wrap alias", enc_d(15, 5, 5, 0xFFFF), dict(gpr={5: 0x00010000}), {5: 0})
    # ---- addic / addic. / subfic (PAGE 349, 350, 572) ----
    check("addic carry", enc_d(12, 3, 3, 1), dict(gpr={3: 0xFFFFFFFF}), {3: 0}, exp_xer=CA)
    check("addic rA=0 is GPR0 (no carry)", enc_d(12, 0, 0, -1), dict(gpr={0: 0}, xer=CA), {0: 0xFFFFFFFF}, exp_xer=0)
    check("addic -1 carry", enc_d(12, 0, 0, -1), dict(gpr={0: 5}), {0: 4}, exp_xer=CA)
    check("addic. GT + SO copy, no OV", enc_d(13, 3, 4, -1), dict(gpr={4: 0x80000000}, xer=SO),
          {3: 0x7FFFFFFF}, exp_xer=SO | CA, exp_cr=0x50000000)
    check("addic. zero", enc_d(13, 3, 3, 1), dict(gpr={3: 0xFFFFFFFF}, cr=0x0000000F),
          {3: 0}, exp_xer=CA, exp_cr=0x2000000F)
    check("subfic 0-0 carries", enc_d(8, 3, 4, 0), dict(gpr={4: 0}), {3: 0}, exp_xer=CA)
    check("subfic 5-6 borrow", enc_d(8, 3, 4, 5), dict(gpr={4: 6}, xer=CA), {3: 0xFFFFFFFF}, exp_xer=0)
    check("subfic -1-(-1)", enc_d(8, 4, 4, -1), dict(gpr={4: 0xFFFFFFFF}), {4: 0}, exp_xer=CA)
    # ---- mulli (PAGE 484) ----
    check("mulli signed low32", enc_d(7, 3, 4, -3), dict(gpr={4: 0x40000000}), {3: 0x40000000})
    check("mulli -1*-1", enc_d(7, 3, 3, -1), dict(gpr={3: 0xFFFFFFFF}, xer=CA), {3: 1})
    # ---- compares (PAGE 365-368) ----
    check("cmpi EQ + SO", enc_cmpi(11, 0, 3, -1), dict(gpr={3: 0xFFFFFFFF}, xer=SO, cr=0x0FFFFFFF),
          exp_cr=0x3FFFFFFF)
    check("cmpli cr7 GT", enc_cmpi(10, 7, 3, 0xFFFF), dict(gpr={3: 0xFFFFFFFF}, cr=0x1234567F), exp_cr=0x12345674)
    check("cmp signed LT", enc_cmp(0, 1, 3, 4), dict(gpr={3: 0x80000000, 4: 1}), exp_cr=0x08000000)
    check("cmpl unsigned GT", enc_cmp(32, 1, 3, 4), dict(gpr={3: 0x80000000, 4: 1}), exp_cr=0x04000000)
    check("cmp self EQ", enc_cmp(0, 6, 9, 9), dict(gpr={9: 0xDEADBEEF}, cr=0xFFFFFFFF), exp_cr=0xFFFFFF2F)
    check_undef("cmp L=1 invalid", enc_cmp(0, 0, 3, 4, L=1), dict(gpr={3: 1, 4: 2}), applied=False)
    check_undef("cmpi L=1 invalid", enc_cmpi(11, 0, 3, 0, L=1), dict(gpr={3: 1}), applied=False)
    check_undef("cmpl bit31 reserved", enc_cmp(32, 0, 3, 4) | 1, dict(gpr={3: 1, 4: 2}), applied=False)
    # ---- immediate logicals (PAGE 356, 357, 491, 492, 581, 582) ----
    check("andi. GT no XER", enc_d(28, 4, 3, 0x8000), dict(gpr={4: 0xFFFF8000}, xer=CA | OV), {3: 0x8000},
          exp_cr=0x40000000)
    check("andis. LT", enc_d(29, 4, 3, 0x8000), dict(gpr={4: 0x80000000}), {3: 0x80000000}, exp_cr=0x80000000)
    check("andi. zero + SO", enc_d(28, 4, 3, 0x00FF), dict(gpr={4: 0xFFFFFF00}, xer=SO), {3: 0}, exp_cr=0x30000000)
    check("ori nop keeps CR", enc_d(24, 0, 0, 0), dict(gpr={0: 7}, cr=0xFFFFFFFF), {0: 7})
    check("oris", enc_d(25, 4, 3, 0x8001), dict(gpr={4: 0x00001234}), {3: 0x80011234})
    check("xoris", enc_d(27, 4, 4, 0xFFFF), dict(gpr={4: 0x0000FFFF}), {4: 0xFFFFFFFF})
    check("xori", enc_d(26, 4, 3, 0xFFFF), dict(gpr={4: 0x1234FFFF}), {3: 0x12340000})
    # ---- rotates (PAGE 533-536) ----
    check("rlwinm extract byte", enc_m(21, 4, 3, 8, 24, 31), dict(gpr={4: 0x12345678}), {3: 0x12})
    check("rlwinm wrap mask MASK(31,0)", enc_m(21, 4, 3, 0, 31, 0), dict(gpr={4: 0xFFFFFFFF}), {3: 0x80000001})
    check("rlwinm MASK(1,0)=all ones", enc_m(21, 4, 3, 4, 1, 0), dict(gpr={4: 0x80000001}), {3: 0x18})
    check("rlwinm. rotate GT", enc_m(21, 4, 3, 1, 0, 31, 1), dict(gpr={4: 0x80000000}, xer=SO), {3: 1},
          exp_cr=0x50000000)
    check("rlwinm single bit MB=ME", enc_m(21, 4, 3, 0, 5, 5), dict(gpr={4: 0xFFFFFFFF}), {3: 0x04000000})
    check("rlwimi insert", enc_m(20, 4, 3, 8, 24, 31), dict(gpr={3: 0xAAAAAAAA, 4: 0x12345678}), {3: 0xAAAAAA12})
    check("rlwimi alias rA=rS", enc_m(20, 3, 3, 16, 0, 15), dict(gpr={3: 0x1234ABCD}), {3: 0xABCDABCD})
    check("rlwnm low-5 of rB", enc_m(23, 4, 3, 5, 0, 31), dict(gpr={4: 0x12345678, 5: 0xFFFFFFE4}), {3: 0x23456781})
    check("rlwnm rB=32 -> rotate 0", enc_m(23, 4, 3, 5, 0, 31), dict(gpr={4: 0x12345678, 5: 32}), {3: 0x12345678})
    # ---- shifts (PAGE 538-541) ----
    check("slw 31", enc_x(24, 4, 3, 5), dict(gpr={4: 3, 5: 31}), {3: 0x80000000})
    check("slw 32 -> 0", enc_x(24, 4, 3, 5), dict(gpr={4: 3, 5: 32}), {3: 0})
    check("slw 63 -> 0", enc_x(24, 4, 3, 5), dict(gpr={4: 3, 5: 63}), {3: 0})
    check("slw 0x40: rB[0-25] ignored -> shift 0", enc_x(24, 4, 3, 5), dict(gpr={4: 3, 5: 0x40}), {3: 3})
    check("slw. alias rA=rB", enc_x(24, 4, 5, 5, 1), dict(gpr={4: 0x40000000, 5: 1}), {5: 0x80000000},
          exp_cr=0x80000000)
    check("srw 4", enc_x(536, 4, 3, 5), dict(gpr={4: 0x80000000, 5: 4}), {3: 0x08000000})
    check("srw 0x21 -> 0", enc_x(536, 4, 3, 5), dict(gpr={4: 0xFFFFFFFF, 5: 0x21}), {3: 0})
    check("srw 0xFFFFFFC3 -> 3", enc_x(536, 4, 3, 5), dict(gpr={4: 0x80000000, 5: 0xFFFFFFC3}), {3: 0x10000000})
    check("sraw 1 CA", enc_x(792, 4, 3, 5), dict(gpr={4: 0x80000001, 5: 1}), {3: 0xC0000000}, exp_xer=CA)
    check("sraw 0 clears CA", enc_x(792, 4, 3, 5), dict(gpr={4: 0x80000001, 5: 0}, xer=CA | SO), {3: 0x80000001},
          exp_xer=SO)
    check("sraw 32 neg", enc_x(792, 4, 3, 5), dict(gpr={4: 0x80000000, 5: 32}), {3: 0xFFFFFFFF}, exp_xer=CA)
    check("sraw 0x41: rB[0-25] ignored -> 1", enc_x(792, 4, 3, 5), dict(gpr={4: 0x80000000, 5: 0x41}),
          {3: 0xC0000000}, exp_xer=0)
    check("sraw 63 pos", enc_x(792, 4, 3, 5), dict(gpr={4: 0x7FFFFFFF, 5: 63}, xer=CA), {3: 0}, exp_xer=0)
    check("sraw neg no 1s out", enc_x(792, 4, 3, 5), dict(gpr={4: 0xFFFFFFF0, 5: 4}, xer=CA), {3: 0xFFFFFFFF},
          exp_xer=0)
    check("sraw. alias rA=rB", enc_x(792, 4, 5, 5, 1), dict(gpr={4: 0xFFFFFF01, 5: 8}), {5: 0xFFFFFFFF},
          exp_xer=CA, exp_cr=0x80000000)
    check("srawi 31 of 0x80000000: CA=0", enc_x(824, 4, 3, 31), dict(gpr={4: 0x80000000}, xer=CA),
          {3: 0xFFFFFFFF}, exp_xer=0)
    check("srawi 0 negative CA cleared", enc_x(824, 3, 3, 0), dict(gpr={3: 0xF0000000}, xer=CA), {3: 0xF0000000},
          exp_xer=0)
    check("srawi. 1", enc_x(824, 4, 3, 1, 1), dict(gpr={4: 0x80000003}), {3: 0xC0000001}, exp_xer=CA,
          exp_cr=0x80000000)
    check("srawi positive", enc_x(824, 4, 3, 4), dict(gpr={4: 0x7FFFFFFF}), {3: 0x07FFFFFF}, exp_xer=0)
    # ---- count / extend (PAGE 369, 392, 393) ----
    check("cntlzw 0", enc_x(26, 4, 3, 0), dict(gpr={4: 0}), {3: 32})
    check("cntlzw 1", enc_x(26, 4, 3, 0), dict(gpr={4: 1}), {3: 31})
    check("cntlzw msb", enc_x(26, 4, 4, 0), dict(gpr={4: 0x80000000}), {4: 0})
    check("cntlzw. EQ", enc_x(26, 4, 3, 0, 1), dict(gpr={4: 0x80000000}, cr=0x80000000), {3: 0}, exp_cr=0x20000000)
    check("cntlzw. GT, LT clear", enc_x(26, 4, 3, 0, 1), dict(gpr={4: 0}), {3: 32}, exp_cr=0x40000000)
    check("extsb", enc_x(954, 4, 3, 0), dict(gpr={4: 0x12345680}), {3: 0xFFFFFF80})
    check("extsb positive", enc_x(954, 4, 4, 0), dict(gpr={4: 0xFFFFFF7F}), {4: 0x0000007F})
    check("extsh", enc_x(922, 4, 3, 0), dict(gpr={4: 0x00017FFF}), {3: 0x00007FFF})
    check("extsh. LT", enc_x(922, 4, 3, 0, 1), dict(gpr={4: 0x00008000}, xer=SO), {3: 0xFFFF8000}, exp_cr=0x90000000)
    check_undef("cntlzw reserved rB field", enc_x(26, 4, 3, 1), dict(gpr={4: 1}), applied=False)
    # ---- register logicals (PAGE 354, 355, 486, 488-490, 391, 580) ----
    P = dict(gpr={4: 0xF0F0F0F0, 5: 0xFF00FF00}, xer=CA | OV | SO)
    check("and.", enc_x(28, 4, 3, 5, 1), P, {3: 0xF000F000}, exp_cr=0x90000000)
    check("andc", enc_x(60, 4, 3, 5), P, {3: 0x00F000F0})
    check("or", enc_x(444, 4, 3, 5), P, {3: 0xFFF0FFF0})
    check("orc.", enc_x(412, 4, 3, 5, 1), P, {3: 0xF0FFF0FF}, exp_cr=0x90000000)
    check("nor", enc_x(124, 4, 3, 5), P, {3: 0x000F000F})
    check("nand.", enc_x(476, 4, 3, 5, 1), P, {3: 0x0FFF0FFF}, exp_cr=0x50000000)
    check("eqv", enc_x(284, 4, 3, 5), P, {3: 0xF00FF00F})
    check("xor. self -> EQ", enc_x(316, 4, 4, 4, 1), P, {4: 0}, exp_cr=0x30000000)
    check("nor mr-not alias", enc_x(124, 3, 3, 3), dict(gpr={3: 0x0000FFFF}), {3: 0xFFFF0000})
    # ---- add family (PAGE 345-347, 352, 353) ----
    check("add. no OE: no OV", enc_xo(266, 3, 4, 5, 0, 1), dict(gpr={4: 0x7FFFFFFF, 5: 1}, xer=CA),
          {3: 0x80000000}, exp_xer=CA, exp_cr=0x80000000)
    check("addo overflow sets OV SO", enc_xo(266, 3, 4, 5, 1, 0), dict(gpr={4: 0x7FFFFFFF, 5: 1}),
          {3: 0x80000000}, exp_xer=OV | SO)
    check("addo clears OV keeps SO", enc_xo(266, 3, 4, 5, 1, 0), dict(gpr={4: 1, 5: 1}, xer=OV | SO), {3: 2},
          exp_xer=SO)
    check("addo. CR0 SO = new SO", enc_xo(266, 3, 4, 5, 1, 1), dict(gpr={4: 0x80000000, 5: 0xFFFFFFFF}),
          {3: 0x7FFFFFFF}, exp_xer=OV | SO, exp_cr=0x50000000)
    check("add alias r3,r3,r3", enc_xo(266, 3, 3, 3), dict(gpr={3: 0x40000001}), {3: 0x80000002})
    check("addco. -1+1", enc_xo(10, 3, 4, 5, 1, 1), dict(gpr={4: 0xFFFFFFFF, 5: 1}), {3: 0}, exp_xer=CA,
          exp_cr=0x20000000)
    check("addco. min+min", enc_xo(10, 3, 4, 5, 1, 1), dict(gpr={4: 0x80000000, 5: 0x80000000}), {3: 0},
          exp_xer=CA | OV | SO, exp_cr=0x30000000)
    check("adde CA in carry out", enc_xo(138, 3, 4, 5), dict(gpr={4: 0xFFFFFFFF, 5: 0}, xer=CA), {3: 0},
          exp_xer=CA)
    check("addeo CA in overflow", enc_xo(138, 3, 4, 5, 1), dict(gpr={4: 0x7FFFFFFF, 5: 0}, xer=CA),
          {3: 0x80000000}, exp_xer=OV | SO)
    check("adde CA=0 no carry", enc_xo(138, 3, 4, 5), dict(gpr={4: 0xFFFFFFFF, 5: 0}), {3: 0xFFFFFFFF},
          exp_xer=0)
    check("addme 0 CA=0", enc_xo(234, 3, 4, 0), dict(gpr={4: 0}), {3: 0xFFFFFFFF}, exp_xer=0)
    check("addme 0 CA=1", enc_xo(234, 3, 4, 0), dict(gpr={4: 0}, xer=CA), {3: 0}, exp_xer=CA)
    check("addmeo min CA=0", enc_xo(234, 3, 4, 0, 1), dict(gpr={4: 0x80000000}), {3: 0x7FFFFFFF},
          exp_xer=CA | OV | SO)
    check("addze -1 CA=1", enc_xo(202, 3, 4, 0), dict(gpr={4: 0xFFFFFFFF}, xer=CA), {3: 0}, exp_xer=CA)
    check("addzeo. max CA=1", enc_xo(202, 3, 4, 0, 1, 1), dict(gpr={4: 0x7FFFFFFF}, xer=CA), {3: 0x80000000},
          exp_xer=OV | SO, exp_cr=0x90000000)
    check_undef("addze reserved bits 16-20", enc_xo(202, 3, 4, 1), dict(gpr={4: 1}), applied=False)
    # ---- subtract family (PAGE 569-574, 487) ----
    check("subf 0-1", enc_xo(40, 3, 4, 5), dict(gpr={4: 1, 5: 0}, xer=CA), {3: 0xFFFFFFFF}, exp_xer=CA)
    check("subfo min-1 overflow", enc_xo(40, 3, 4, 5, 1), dict(gpr={4: 1, 5: 0x80000000}), {3: 0x7FFFFFFF},
          exp_xer=OV | SO)
    check("subfo 0-min overflow", enc_xo(40, 3, 4, 5, 1), dict(gpr={4: 0x80000000, 5: 0}), {3: 0x80000000},
          exp_xer=OV | SO)
    check("subf. self alias EQ", enc_xo(40, 3, 3, 3, 0, 1), dict(gpr={3: 0x1234}), {3: 0}, exp_cr=0x20000000)
    check("subfc 0-0 CA=1", enc_xo(8, 3, 4, 5), dict(gpr={4: 0, 5: 0}), {3: 0}, exp_xer=CA)
    check("subfc 0-1 CA=0", enc_xo(8, 3, 4, 5), dict(gpr={4: 1, 5: 0}, xer=CA), {3: 0xFFFFFFFF}, exp_xer=0)
    check("subfc alias CA=1", enc_xo(8, 3, 3, 3), dict(gpr={3: 5}), {3: 0}, exp_xer=CA)
    check("subfe 0,0 CA=0", enc_xo(136, 3, 4, 5), dict(gpr={4: 0, 5: 0}), {3: 0xFFFFFFFF}, exp_xer=0)
    check("subfe 0,0 CA=1", enc_xo(136, 3, 4, 5), dict(gpr={4: 0, 5: 0}, xer=CA), {3: 0}, exp_xer=CA)
    check("subfeo", enc_xo(136, 3, 4, 5, 1), dict(gpr={4: 0, 5: 0x80000000}), {3: 0x7FFFFFFF},
          exp_xer=CA | OV | SO)
    check("subfme -1 CA=0", enc_xo(232, 3, 4, 0), dict(gpr={4: 0xFFFFFFFF}), {3: 0xFFFFFFFF}, exp_xer=0)
    check("subfme -1 CA=1", enc_xo(232, 3, 4, 0), dict(gpr={4: 0xFFFFFFFF}, xer=CA), {3: 0}, exp_xer=CA)
    check("subfmeo max CA=0", enc_xo(232, 3, 4, 0, 1), dict(gpr={4: 0x7FFFFFFF}), {3: 0x7FFFFFFF},
          exp_xer=CA | OV | SO)
    check("subfze 0 CA=1", enc_xo(200, 3, 4, 0), dict(gpr={4: 0}, xer=CA), {3: 0}, exp_xer=CA)
    check("subfzeo min CA=1", enc_xo(200, 3, 4, 0, 1), dict(gpr={4: 0x80000000}, xer=CA), {3: 0x80000000},
          exp_xer=OV | SO)
    check("subfze 1 CA=0", enc_xo(200, 3, 4, 0), dict(gpr={4: 1}), {3: 0xFFFFFFFE}, exp_xer=0)
    check("nego min", enc_xo(104, 3, 4, 0, 1), dict(gpr={4: 0x80000000}, xer=CA), {3: 0x80000000},
          exp_xer=CA | OV | SO)
    check("nego. 0 clears OV", enc_xo(104, 3, 3, 0, 1, 1), dict(gpr={3: 0}, xer=OV), {3: 0}, exp_xer=0,
          exp_cr=0x20000000)
    check("neg 1", enc_xo(104, 3, 4, 0), dict(gpr={4: 1}), {3: 0xFFFFFFFF})
    # ---- multiply (PAGE 482-485) ----
    check("mullw wraps, no OE", enc_xo(235, 3, 4, 5), dict(gpr={4: 0x10000, 5: 0x10000}), {3: 0})
    check("mullwo overflow", enc_xo(235, 3, 4, 5, 1), dict(gpr={4: 0x10000, 5: 0x10000}), {3: 0},
          exp_xer=OV | SO)
    check("mullwo -1*-1", enc_xo(235, 3, 4, 5, 1), dict(gpr={4: 0xFFFFFFFF, 5: 0xFFFFFFFF}, xer=OV), {3: 1},
          exp_xer=0)
    check("mullwo min*-1", enc_xo(235, 3, 4, 5, 1, 1), dict(gpr={4: 0x80000000, 5: 0xFFFFFFFF}),
          {3: 0x80000000}, exp_xer=OV | SO, exp_cr=0x90000000)
    check("mullwo min*1 ok", enc_xo(235, 3, 4, 5, 1), dict(gpr={4: 0x80000000, 5: 1}), {3: 0x80000000}, exp_xer=0)
    check("mulhw min*min", enc_xo(75, 3, 4, 5), dict(gpr={4: 0x80000000, 5: 0x80000000}), {3: 0x40000000})
    check("mulhw -1*1", enc_xo(75, 3, 4, 5), dict(gpr={4: 0xFFFFFFFF, 5: 1}), {3: 0xFFFFFFFF})
    check("mulhwu max*max", enc_xo(11, 3, 4, 5), dict(gpr={4: 0xFFFFFFFF, 5: 0xFFFFFFFF}), {3: 0xFFFFFFFE})
    check("mulhwu. signed CR0", enc_xo(11, 3, 4, 5, 0, 1), dict(gpr={4: 0xFFFFFFFF, 5: 0xFFFFFFFF}),
          {3: 0xFFFFFFFE}, exp_cr=0x80000000)
    check("mulhwu -1*1 = 0", enc_xo(11, 3, 3, 4), dict(gpr={3: 0xFFFFFFFF, 4: 1}), {3: 0})
    check_undef("mulhw bit21 reserved", enc_xo(75, 3, 4, 5, 1), dict(gpr={4: 2, 5: 3}), applied=False)
    # ---- divide (PAGE 385, 386) ----
    check("divw -7/2", enc_xo(491, 3, 4, 5), dict(gpr={4: 0xFFFFFFF9, 5: 2}), {3: 0xFFFFFFFD})
    check("divw 7/-2", enc_xo(491, 3, 4, 5), dict(gpr={4: 7, 5: 0xFFFFFFFE}), {3: 0xFFFFFFFD})
    check("divw -7/-2", enc_xo(491, 3, 4, 5), dict(gpr={4: 0xFFFFFFF9, 5: 0xFFFFFFFE}), {3: 3})
    check("divwo min/1 ok clears OV", enc_xo(491, 3, 4, 5, 1, 1), dict(gpr={4: 0x80000000, 5: 1}, xer=OV | SO),
          {3: 0x80000000}, exp_xer=SO, exp_cr=0x90000000)
    check("divwu max/2", enc_xo(459, 3, 4, 5), dict(gpr={4: 0xFFFFFFFF, 5: 2}), {3: 0x7FFFFFFF})
    check("divwu. signed CR0", enc_xo(459, 3, 4, 5, 0, 1), dict(gpr={4: 0xFFFFFFFE, 5: 1}), {3: 0xFFFFFFFE},
          exp_cr=0x80000000)
    check("divwu min/-1 is fine unsigned", enc_xo(459, 3, 4, 5, 1), dict(gpr={4: 0x80000000, 5: 0xFFFFFFFF}),
          {3: 0}, exp_xer=0)
    check_undef("divwo min/-1", enc_xo(491, 3, 4, 5, 1, 0), dict(gpr={3: 0x5555, 4: 0x80000000, 5: 0xFFFFFFFF}),
                applied=True, exp_xer=OV | SO, gpr_undef=(3,))
    check_undef("divw. x/0 SO copy defined", enc_xo(491, 3, 4, 5, 0, 1),
                dict(gpr={4: 1, 5: 0}, xer=SO | CA, cr=0x0000FFFF),
                applied=True, exp_xer=SO | CA, exp_cr_defined=0x1000FFFF, gpr_undef=(3,), crmask=0xE0000000)
    check_undef("divwuo. x/0", enc_xo(459, 3, 4, 5, 1, 1), dict(gpr={4: 1, 5: 0}),
                applied=True, exp_xer=OV | SO, exp_cr_defined=0x10000000, gpr_undef=(3,), crmask=0xE0000000)
    check_undef("divw x/0 no OE no Rc", enc_xo(491, 3, 4, 5), dict(gpr={4: 9, 5: 0}, xer=OV),
                applied=True, exp_xer=OV, exp_cr_defined=0, gpr_undef=(3,), crmask=0)
    # ---- CR moves (PAGE 456, 458, 459, 470) ----
    check("mcrxr cr2", (31 << 26) | (2 << 23) | (512 << 1), dict(xer=0xE000007F, cr=0xFFFFFFFF),
          exp_xer=0x0000007F, exp_cr=0xFFEFFFFF)
    check("mcrxr cr0 clears SO", (31 << 26) | (0 << 23) | (512 << 1), dict(xer=0x9000FF00), exp_xer=0x0000FF00,
          exp_cr=0x90000000)
    check_undef("mcrxr reserved", (31 << 26) | (2 << 23) | (1 << 11) | (512 << 1), dict(xer=SO), applied=False)
    check("mfcr", (31 << 26) | (3 << 21) | (19 << 1), dict(cr=0x12345678), {3: 0x12345678})
    check("mtcrf 0x81", (31 << 26) | (3 << 21) | (0x81 << 12) | (144 << 1), dict(gpr={3: 0xFFFFFFFF}),
          exp_cr=0xF000000F)
    check("mtcrf 0xFF (mtcr)", (31 << 26) | (3 << 21) | (0xFF << 12) | (144 << 1),
          dict(gpr={3: 0x89ABCDEF}, cr=0x11111111), exp_cr=0x89ABCDEF)
    check("mtcrf 0 no-op", (31 << 26) | (3 << 21) | (0 << 12) | (144 << 1), dict(gpr={3: 0xFFFFFFFF}, cr=0x5A5A5A5A),
          exp_cr=0x5A5A5A5A)
    check("mtcrf 0x40 field1", (31 << 26) | (3 << 21) | (0x40 << 12) | (144 << 1),
          dict(gpr={3: 0x0A000000}, cr=0x0F000000), exp_cr=0x0A000000)
    check("mcrf cr7<-cr0", (19 << 26) | (7 << 23) | (0 << 18), dict(cr=0xA0000005), exp_cr=0xA000000A)
    check("mcrf self", (19 << 26) | (3 << 23) | (3 << 18), dict(cr=0x12345678), exp_cr=0x12345678)
    check_undef("mcrf reserved bit 20", (19 << 26) | (7 << 23) | (1 << 11), dict(cr=1), applied=False)
    # ---- CR logical (PAGE 370-377) ----
    C = 0xA0000000  # bit0=1 bit1=0 bit2=1 bit3=0
    check("crand 31<-0&2", enc_xl(257, 31, 0, 2), dict(cr=C), exp_cr=C | 1)
    check("crand 31<-0&1 clears", enc_xl(257, 31, 0, 1), dict(cr=C | 1), exp_cr=C)
    check("crandc 4<-0&~1", enc_xl(129, 4, 0, 1), dict(cr=C), exp_cr=C | 0x08000000)
    check("creqv crset 6", enc_xl(289, 6, 6, 6), dict(cr=0), exp_cr=0x02000000)
    check("crnand 2<-~(0&2)", enc_xl(225, 2, 0, 2), dict(cr=C), exp_cr=0x80000000)
    check("crnor crnot 3<-~0", enc_xl(33, 3, 0, 0), dict(cr=C), exp_cr=C)
    check("crnor 1<-~(1|3)", enc_xl(33, 1, 1, 3), dict(cr=C), exp_cr=C | 0x40000000)
    check("cror crmove 30<-2", enc_xl(449, 30, 2, 2), dict(cr=C), exp_cr=C | 2)
    check("crorc 3<-1|~3", enc_xl(417, 3, 1, 3), dict(cr=C), exp_cr=C | 0x10000000)
    check("crxor crclr 0", enc_xl(193, 0, 0, 0), dict(cr=C), exp_cr=0x20000000)
    check("crxor 5<-0^2", enc_xl(193, 5, 0, 2), dict(cr=0xA4000000), exp_cr=0xA0000000)
    check_undef("cror bit31 set", enc_xl(449, 30, 2, 2) | 1, dict(cr=C), applied=False)

    count[0] += 1
    try:
        step(mk(), 0x7C000008)  # tw is not this area
        fails.append("step(tw) did not raise")
    except ValueError:
        pass

    # ---- derived cross-check: sraw pseudocode vs text for all amounts 32..63 (PAGE 539) ----
    for amt in range(32, 64):
        for v in (0x80000000, 0x7FFFFFFF, 0xFFFFFFFF, 0, 1):
            count[0] += 1
            st = mk(gpr={4: v, 5: amt})
            step(st, enc_x(792, 4, 3, 5))
            sgn = v >> 31
            if st.gpr[3] != (MASK32 if sgn else 0) or bool(st.xer & CA) != bool(sgn):
                fails.append("sraw text rule amt=%d v=0x%08X" % (amt, v))

    if fails:
        print("FAIL %d of %d" % (len(fails), count[0]))
        for f in fails:
            print("  " + f)
        sys.exit(1)
    print("int_B self-test: %d vectors PASS" % count[0])
