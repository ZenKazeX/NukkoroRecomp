"""PowerPC (Gekko) instruction word encoder for the differential harness.

Field packing only - no semantics. Big-endian bit numbering as in the Gekko manual
(bit 0 = MSB). Every encoder returns a u32.

The words are cross-checked independently at generation time: gen_vectors.py asks the
blind spec model's own decoder (spec `mnemonic(word)`, written from the manual) to name
each spec-compared word and refuses to emit a vector whose intended mnemonic disagrees.
"""
from __future__ import annotations

M32 = 0xFFFFFFFF


def _chk(value: int, bits: int, name: str) -> int:
    if value < 0 or value >= (1 << bits):
        raise ValueError(f"{name}={value} does not fit in {bits} bits")
    return value


def d_form(op: int, rd: int, ra: int, imm: int) -> int:
    """D-form: opcode | rD/rS/crfD.. (5) | rA (5) | 16-bit immediate (two's complement ok)."""
    return ((_chk(op, 6, "op") << 26) | (_chk(rd, 5, "rd") << 21) | (_chk(ra, 5, "ra") << 16)
            | (imm & 0xFFFF)) & M32


def cmp_imm(op: int, crfd: int, ra: int, imm: int, l_bit: int = 0) -> int:
    """cmpi (11) / cmpli (10): crfD bits 6-8, bit 9 reserved, L bit 10."""
    return ((op << 26) | (_chk(crfd, 3, "crfd") << 23) | (_chk(l_bit, 1, "L") << 21)
            | (_chk(ra, 5, "ra") << 16) | (imm & 0xFFFF)) & M32


def xo_form(rd: int, ra: int, rb: int, xo: int, oe: int = 0, rc: int = 0) -> int:
    """Primary 31 XO-form: rD | rA | rB | OE | 9-bit XO | Rc."""
    return ((31 << 26) | (_chk(rd, 5, "rd") << 21) | (_chk(ra, 5, "ra") << 16)
            | (_chk(rb, 5, "rb") << 11) | (_chk(oe, 1, "oe") << 10) | (_chk(xo, 9, "xo") << 1)
            | _chk(rc, 1, "rc")) & M32


def x_form(op: int, f1: int, f2: int, f3: int, xo: int, rc: int = 0) -> int:
    """X-form: opcode | 5 | 5 | 5 | 10-bit XO | Rc (f1 = rS/rD/crbD, f2 = rA/crbA, f3 = rB/SH/crbB)."""
    return ((_chk(op, 6, "op") << 26) | (_chk(f1, 5, "f1") << 21) | (_chk(f2, 5, "f2") << 16)
            | (_chk(f3, 5, "f3") << 11) | (_chk(xo, 10, "xo") << 1) | _chk(rc, 1, "rc")) & M32


def m_form(op: int, rs: int, ra: int, sh_rb: int, mb: int, me: int, rc: int = 0) -> int:
    """rlwimi (20) / rlwinm (21) / rlwnm (23)."""
    return ((op << 26) | (_chk(rs, 5, "rs") << 21) | (_chk(ra, 5, "ra") << 16)
            | (_chk(sh_rb, 5, "sh") << 11) | (_chk(mb, 5, "mb") << 6) | (_chk(me, 5, "me") << 1)
            | _chk(rc, 1, "rc")) & M32


def cmp_reg(xo: int, crfd: int, ra: int, rb: int, l_bit: int = 0, rc: int = 0) -> int:
    """cmp (0) / cmpl (32): crfD bits 6-8, bit 9 reserved, L bit 10, rA, rB, XO, bit 31 reserved."""
    return ((31 << 26) | (_chk(crfd, 3, "crfd") << 23) | (_chk(l_bit, 1, "L") << 21)
            | (_chk(ra, 5, "ra") << 16) | (_chk(rb, 5, "rb") << 11) | (xo << 1) | rc) & M32


def mcrxr(crfd: int) -> int:
    return ((31 << 26) | (_chk(crfd, 3, "crfd") << 23) | (512 << 1)) & M32


def mfcr(rd: int) -> int:
    return ((31 << 26) | (_chk(rd, 5, "rd") << 21) | (19 << 1)) & M32


def mtcrf(rs: int, crm: int) -> int:
    return ((31 << 26) | (_chk(rs, 5, "rs") << 21) | (_chk(crm, 8, "crm") << 12) | (144 << 1)) & M32


def mcrf(crfd: int, crfs: int) -> int:
    return ((19 << 26) | (_chk(crfd, 3, "crfd") << 23) | (_chk(crfs, 3, "crfs") << 18)) & M32


def cr_logical(xo: int, crbd: int, crba: int, crbb: int) -> int:
    return x_form(19, crbd, crba, crbb, xo)


def spr_field(number: int) -> int:
    """The 10-bit SPR field as encoded (bits 11-20): the two 5-bit halves swapped."""
    _chk(number, 10, "spr")
    return ((number >> 5) & 0x1F) | ((number & 0x1F) << 5)


def mfspr(rd: int, number: int) -> int:
    return ((31 << 26) | (_chk(rd, 5, "rd") << 21) | (spr_field(number) << 11) | (339 << 1)) & M32


def mtspr(rs: int, number: int) -> int:
    return ((31 << 26) | (_chk(rs, 5, "rs") << 21) | (spr_field(number) << 11) | (467 << 1)) & M32


def b(site: int, target: int, lk: int = 0) -> int:
    disp = (target - site) & M32
    signed = disp - (1 << 32) if disp & 0x80000000 else disp
    if signed < -(1 << 25) or signed >= (1 << 25) or signed & 3:
        raise ValueError("b displacement out of range")
    return ((18 << 26) | (disp & 0x03FFFFFC) | lk) & M32


def bc(bo: int, bi: int, site: int, target: int, lk: int = 0) -> int:
    disp = (target - site) & M32
    signed = disp - (1 << 32) if disp & 0x80000000 else disp
    if signed < -(1 << 15) or signed >= (1 << 15) or signed & 3:
        raise ValueError("bc displacement out of range")
    return ((16 << 26) | (_chk(bo, 5, "bo") << 21) | (_chk(bi, 5, "bi") << 16) | (disp & 0xFFFC)
            | lk) & M32


def bclr(bo: int, bi: int, lk: int = 0) -> int:
    return ((19 << 26) | (_chk(bo, 5, "bo") << 21) | (_chk(bi, 5, "bi") << 16) | (16 << 1) | lk) & M32


def bcctr(bo: int, bi: int, lk: int = 0) -> int:
    return ((19 << 26) | (_chk(bo, 5, "bo") << 21) | (_chk(bi, 5, "bi") << 16) | (528 << 1) | lk) & M32


BLR = bclr(20, 0)
MTCTR = lambda rs: mtspr(rs, 9)  # noqa: E731
MTLR = lambda rs: mtspr(rs, 8)   # noqa: E731
MFLR = lambda rd: mfspr(rd, 8)   # noqa: E731


def addi(rd: int, ra: int, imm: int) -> int:
    return d_form(14, rd, ra, imm)


def addis(rd: int, ra: int, imm: int) -> int:
    return d_form(15, rd, ra, imm)


def ori(ra: int, rs: int, uimm: int) -> int:
    return d_form(24, rs, ra, uimm)


def load_address(reg: int, address: int) -> list:
    """lis reg,hi ; ori reg,reg,lo - exact for any u32."""
    return [addis(reg, 0, (address >> 16) & 0xFFFF), ori(reg, reg, address & 0xFFFF)]


def cmplw(crfd: int, ra: int, rb: int) -> int:
    return cmp_reg(32, crfd, ra, rb)


def cmpw(crfd: int, ra: int, rb: int) -> int:
    return cmp_reg(0, crfd, ra, rb)
