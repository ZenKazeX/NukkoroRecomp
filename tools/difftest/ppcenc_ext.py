"""Instruction-word encoders added for the F1b differential harness (field packing only).

Extends tools/difftest/ppcenc.py (F1a) without changing it: floating point, paired singles, system
and supervisor instructions. Big-endian bit numbering as in the Gekko manual (bit 0 = MSB).

Every word that is compared against the spec model is cross-checked at generation time by the
spec model's own decoder (gen_f1b.py spec_mnemonic_check), so these encoders are validated
independently of the runtime.
"""
from __future__ import annotations

from ppcenc import M32, _chk, x_form  # noqa: F401  (re-exported for gen_f1b)


def a_form(op: int, d: int, a: int, b: int, c: int, xo5: int, rc: int = 0) -> int:
    """A-form (primary 59/63/4): opcode | frD | frA | frB | frC | 5-bit XO | Rc."""
    return ((_chk(op, 6, "op") << 26) | (_chk(d, 5, "d") << 21) | (_chk(a, 5, "a") << 16)
            | (_chk(b, 5, "b") << 11) | (_chk(c, 5, "c") << 6) | (_chk(xo5, 5, "xo5") << 1)
            | _chk(rc, 1, "rc")) & M32


def fcmp(op: int, xo: int, crfd: int, a: int, b: int) -> int:
    """fcmpu/fcmpo (63, XO 0/32) and ps_cmpu0/o0/u1/o1 (4, XO 0/32/64/96): crfD bits 6-8."""
    return ((op << 26) | (_chk(crfd, 3, "crfd") << 23) | (_chk(a, 5, "a") << 16) | (_chk(b, 5, "b") << 11)
            | (xo << 1)) & M32


def mtfsb(bit: int, one: bool, rc: int = 0) -> int:
    return ((63 << 26) | (_chk(bit, 5, "bit") << 21) | ((38 if one else 70) << 1) | rc) & M32


def mtfsfi(crfd: int, imm: int, rc: int = 0) -> int:
    return ((63 << 26) | (_chk(crfd, 3, "crfd") << 23) | (_chk(imm, 4, "imm") << 12) | (134 << 1) | rc) & M32


def mtfsf(fm: int, b: int, rc: int = 0) -> int:
    return ((63 << 26) | (_chk(fm, 8, "fm") << 17) | (_chk(b, 5, "b") << 11) | (711 << 1) | rc) & M32


def mcrfs(crfd: int, crfs: int) -> int:
    return ((63 << 26) | (_chk(crfd, 3, "crfd") << 23) | (_chk(crfs, 3, "crfs") << 18) | (64 << 1)) & M32


def psq_d(op: int, d: int, a: int, w: int, i: int, offset: int) -> int:
    """psq_l/psq_lu/psq_st/psq_stu: op | frD | rA | W (bit 16) | I (17-19) | 12-bit offset."""
    return ((_chk(op, 6, "op") << 26) | (_chk(d, 5, "d") << 21) | (_chk(a, 5, "a") << 16)
            | (_chk(w, 1, "w") << 15) | (_chk(i, 3, "i") << 12) | (offset & 0xFFF)) & M32


def psq_x(xo6: int, d: int, a: int, b: int, w: int, i: int) -> int:
    """psq_lx/psq_stx/psq_lux/psq_stux: 4 | frD | rA | rB | W (bit 21) | I (22-24) | 6-bit XO | 0."""
    return ((4 << 26) | (_chk(d, 5, "d") << 21) | (_chk(a, 5, "a") << 16) | (_chk(b, 5, "b") << 11)
            | (_chk(w, 1, "w") << 10) | (_chk(i, 3, "i") << 7) | (_chk(xo6, 6, "xo6") << 1)) & M32


def tw(to: int, a: int, b: int) -> int:
    return x_form(31, to, a, b, 4)


def twi(to: int, a: int, imm: int) -> int:
    return ((3 << 26) | (_chk(to, 5, "to") << 21) | (_chk(a, 5, "a") << 16) | (imm & 0xFFFF)) & M32


SC = 0x44000002
RFI = (19 << 26) | (50 << 1)
ISYNC = (19 << 26) | (150 << 1)


def mtmsr(s: int) -> int:
    return x_form(31, s, 0, 0, 146)


def mfmsr(d: int) -> int:
    return x_form(31, d, 0, 0, 83)


def mtsr(sr: int, s: int) -> int:
    return ((31 << 26) | (_chk(s, 5, "s") << 21) | (_chk(sr, 4, "sr") << 16) | (210 << 1)) & M32


def mfsr(d: int, sr: int) -> int:
    return ((31 << 26) | (_chk(d, 5, "d") << 21) | (_chk(sr, 4, "sr") << 16) | (595 << 1)) & M32


def mtsrin(s: int, b: int) -> int:
    return x_form(31, s, 0, b, 242)


def mfsrin(d: int, b: int) -> int:
    return x_form(31, d, 0, b, 659)


def spr_split(number: int) -> tuple:
    """(bits 11-15, bits 16-20) of the swapped SPR/TBR field."""
    _chk(number, 10, "spr")
    return number & 0x1F, (number >> 5) & 0x1F


def mftb(d: int, number: int) -> int:
    lo, hi = spr_split(number)
    return ((31 << 26) | (_chk(d, 5, "d") << 21) | (lo << 16) | (hi << 11) | (371 << 1)) & M32


def spr_rc(word: int, rc: int) -> int:
    return (word & ~1 & M32) | (rc & 1)


def rlwinm(ra: int, rs: int, sh: int, mb: int, me: int, rc: int = 0) -> int:
    return ((21 << 26) | (_chk(rs, 5, "rs") << 21) | (_chk(ra, 5, "ra") << 16) | (_chk(sh, 5, "sh") << 11)
            | (_chk(mb, 5, "mb") << 6) | (_chk(me, 5, "me") << 1) | rc) & M32
