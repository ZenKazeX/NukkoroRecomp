"""Gekko / PowerPC instruction effects shared by the dolrecon analyses (one decoder instead of one per tool).

Answer 5 of GPT-6-Astra's review (2026-11-01, .build/dolrecon/astra/answer5.md) found the separate decoders in
vcalls, names.scan_object_code, strrefs and rwheaders disagreeing with the ISA (indexed update stores and loads,
paired-single update forms, string loads, record forms). The effects here follow the PowerPC 750CL / Gekko user
manual; tools/test_dolrecon_evidence.py PpcEffectTests pins every family.

gpr_writes(w) -> tuple of GPRs the instruction may write, or None when the instruction is not decoded (the caller
must then drop every fact). Floating-point and paired-single destinations are FPRs and are not listed.
branch(w, pc) -> Branch or None for a non-branch.
"""
from __future__ import annotations

from dataclasses import dataclass

VOLATILE = frozenset((0, *range(3, 13)))        # EABI: a call may change r0, r3-r12; r1, r2, r13-r31 survive

# primary opcode -> GPR fields written ("d" bits 21-25, "a" bits 16-20)
_PRIMARY = {
    2: "", 3: "", 7: "d", 8: "d", 10: "", 11: "", 12: "d", 13: "d", 14: "d", 15: "d", 16: "", 17: "", 18: "",
    19: "", 20: "a", 21: "a", 23: "a", 24: "a", 25: "a", 26: "a", 27: "a", 28: "a", 29: "a",
    32: "d", 33: "da", 34: "d", 35: "da", 36: "", 37: "a", 38: "", 39: "a", 40: "d", 41: "da", 42: "d", 43: "da",
    44: "", 45: "a", 47: "", 48: "", 49: "a", 50: "", 51: "a", 52: "", 53: "a", 54: "", 55: "a",
    56: "", 57: "a", 59: "", 60: "", 61: "a", 63: "",
}
# opcode 31, 10-bit extended opcode -> fields written
_X31 = {
    0: "", 32: "", 4: "", 144: "", 146: "", 210: "", 242: "", 467: "", 54: "", 86: "", 246: "", 278: "", 438: "",
    470: "", 982: "", 1014: "", 598: "", 854: "", 306: "", 566: "", 512: "",
    151: "", 215: "", 407: "", 918: "", 662: "", 150: "", 661: "", 725: "", 663: "", 727: "", 983: "",  # stores
    183: "a", 247: "a", 439: "a", 695: "a", 759: "a",                                              # store update
    23: "d", 87: "d", 279: "d", 343: "d", 790: "d", 534: "d", 20: "d", 535: "", 599: "",          # loads
    55: "da", 119: "da", 311: "da", 375: "da", 567: "a", 631: "a",                                  # load update
    19: "d", 83: "d", 339: "d", 371: "d", 595: "d", 659: "d", 310: "d",                             # mf*, eciwx
    28: "a", 60: "a", 124: "a", 284: "a", 316: "a", 412: "a", 444: "a", 476: "a", 24: "a", 536: "a",
    792: "a", 824: "a", 26: "a", 954: "a", 922: "a",                                                # logical/shift
}
# opcode 31 XO-form arithmetic: 9-bit extended opcode (the 10th bit is OE) -> writes rD
_XO31 = {266, 10, 138, 234, 202, 40, 8, 136, 232, 200, 104, 235, 75, 11, 491, 459}


def fields(w):
    return w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 0xFFFF


def simm(v):
    return v - 0x10000 if v & 0x8000 else v


def _regs(spec, d, a):
    return tuple(dict.fromkeys(d if c == "d" else a for c in spec))


def gpr_writes(w):
    op, d, a, b, _ = fields(w)
    if op == 46:                                           # lmw rD..r31
        return tuple(range(d, 32))
    if op == 4:                                            # paired singles: only psq_lux / psq_stux write rA
        xo6 = (w >> 1) & 0x3F
        if xo6 in (38, 39):
            return (a,)
        return () if xo6 in (6, 7) or (w >> 1) & 0x3FF in (1014,) or (w >> 1) & 0x1F in range(10, 32) \
            or (w >> 1) & 0x3FF in (0, 32, 64, 96, 40, 72, 136, 264, 528, 560, 592, 624) else None
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo == 597:                                      # lswi: ceil(NB/4) registers from rD, wrapping at 32
            nb = b or 32                                   # NB = 0 means 32 bytes
            return tuple((d + i) % 32 for i in range((nb + 3) // 4))
        if xo == 533:                                      # lswx: the count is in XER - unknown registers
            return None
        if xo in _X31:
            return _regs(_X31[xo], d, a)
        if xo & 0x1FF in _XO31:
            return (d,)
        return None
    spec = _PRIMARY.get(op)
    return None if spec is None else _regs(spec, d, a)


def is_mr(w):
    """(True, rA, rS) for `or rA,rS,rS` - mr, also its record form mr. (a copy that sets cr0)."""
    op, s, a, b, _ = fields(w)
    return op == 31 and (w >> 1) & 0x3FF == 444 and s == b, a, s


@dataclass(frozen=True)
class Branch:
    kind: str           # "b", "bc", "bclr", "bcctr"
    target: int | None  # direct target (None for bclr / bcctr)
    link: bool          # LK: a call
    always: bool        # unconditional (b, or BO "branch always")


def branch(w, pc):
    op = w >> 26
    if op == 18:
        li = ((w & 0x3FFFFFC) ^ 0x2000000) - 0x2000000
        return Branch("b", (li if w & 2 else pc + li) & 0xFFFFFFFF, bool(w & 1), True)
    if op == 16:
        bd = simm(w & 0xFFFC)
        return Branch("bc", (bd if w & 2 else pc + bd) & 0xFFFFFFFF, bool(w & 1), (w >> 21) & 0x14 == 0x14)
    if op == 19 and (w >> 1) & 0x3FF in (16, 528):
        return Branch("bclr" if (w >> 1) & 0x3FF == 16 else "bcctr", None, bool(w & 1), (w >> 21) & 0x14 == 0x14)
    return None
