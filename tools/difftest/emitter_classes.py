"""Catalogue of every instruction class tools/ppcrecomp/main.cpp can emit (F1b coverage accounting).

Derived from the structure of the emitter's decode switches only (emit_instruction,
emit_load_store_d, emit_group4/19/31/59/63): which words it ACCEPTS and which distinct lowering each
one gets. No semantics. A "class" is the mnemonic plus the variants that change the emitted code:
OE ('o'), Rc ('.'), LK ('l' / '[l]') and AA ('a' / '[a]'). Words the emitter accepts in an invalid
form (a reserved bit set that the emitter does not decode) map to their base class.

  emitter_class(word) -> class name, or None when main.cpp rejects the word
  all_classes()       -> every class name the catalogue can produce, sorted
"""
from __future__ import annotations

XO_ARITH = {266: "add", 10: "addc", 138: "adde", 234: "addme", 202: "addze", 40: "subf", 8: "subfc",
            136: "subfe", 232: "subfme", 200: "subfze", 104: "neg", 235: "mullw", 491: "divw", 459: "divwu"}
XO_NO_OE = {75: "mulhw", 11: "mulhwu"}
X_LOGIC = {28: "and", 60: "andc", 444: "or", 412: "orc", 316: "xor", 476: "nand", 124: "nor", 284: "eqv",
           24: "slw", 536: "srw", 792: "sraw", 824: "srawi", 26: "cntlzw", 922: "extsh", 954: "extsb"}
G31_PLAIN = {0: "cmp", 32: "cmpl", 4: "tw", 19: "mfcr", 20: "lwarx", 23: "lwzx", 55: "lwzux", 83: "mfmsr",
             87: "lbzx", 119: "lbzux", 144: "mtcrf", 146: "mtmsr", 150: "stwcx", 151: "stwx", 183: "stwux",
             210: "mtsr", 242: "mtsrin", 215: "stbx", 247: "stbux", 279: "lhzx", 311: "lhzux", 339: "mfspr",
             343: "lhax", 371: "mftb", 375: "lhaux", 407: "sthx", 439: "sthux", 467: "mtspr", 512: "mcrxr",
             534: "lwbrx", 535: "lfsx", 567: "lfsux", 595: "mfsr", 599: "lfdx", 631: "lfdux", 659: "mfsrin",
             662: "stwbrx", 663: "stfsx", 695: "stfsux", 727: "stfdx", 759: "stfdux", 790: "lhbrx",
             918: "sthbrx", 983: "stfiwx", 1014: "dcbz", 310: "eciwx", 438: "ecowx",
             54: "dcbst", 86: "dcbf", 246: "dcbtst", 278: "dcbt", 470: "dcbi", 982: "icbi", 598: "sync",
             566: "tlbsync", 854: "eieio", 306: "tlbie", 370: "tlbia",
             # string load/store, lowered since repair L0r0-03 (fail()ed before)
             597: "lswi", 533: "lswx", 725: "stswi", 661: "stswx"}
D_PRIMARY = {7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.", 14: "addi", 15: "addis",
             24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis.", 3: "twi",
             32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
             40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
             48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
             56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}
M_PRIMARY = {20: "rlwimi", 21: "rlwinm", 23: "rlwnm"}
G19 = {0: "mcrf", 50: "rfi", 150: "isync", 33: "crnor", 129: "crandc", 193: "crxor", 225: "crnand", 257: "crand",
       289: "creqv", 417: "crorc", 449: "cror"}
A59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls", 28: "fmsubs", 29: "fmadds",
       30: "fnmsubs", 31: "fnmadds"}
A63 = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte", 28: "fmsub",
       29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
X63 = {0: "fcmpu", 32: "fcmpo", 12: "frsp", 14: "fctiw", 15: "fctiwz", 38: "mtfsb1", 70: "mtfsb0", 40: "fneg",
       64: "mcrfs", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs", 583: "mffs", 711: "mtfsf"}
X63_NO_RC = {0, 32, 64}
A4 = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0", 15: "ps_madds1",
      18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul", 26: "ps_rsqrte",
      28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd"}
X4 = {0: "ps_cmpu0", 32: "ps_cmpo0", 64: "ps_cmpu1", 96: "ps_cmpo1", 40: "ps_neg", 72: "ps_mr", 136: "ps_nabs",
      264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10", 624: "ps_merge11", 1014: "dcbz_l"}
XW4 = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}
RC_X4 = set(X4.values())   # emit_group4 applies emit_cr1 after every X-form case


def emitter_class(word: int):
    w = word & 0xFFFFFFFF
    op = w >> 26
    rc = w & 1
    if op in D_PRIMARY:
        return D_PRIMARY[op]
    if op in M_PRIMARY:
        return M_PRIMARY[op] + ("." if rc else "")
    if op == 16:
        return "bc" + ("l" if rc else "") + ("a" if w & 2 else "")
    if op == 18:
        return "b" + ("l" if rc else "") + ("a" if w & 2 else "")
    if op == 17:
        return "sc"
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo == 16:
            return "bclr" + ("l" if rc else "")
        if xo == 528:
            return "bcctr" + ("l" if rc else "")
        return G19.get(xo)
    if op == 31:
        xo10 = (w >> 1) & 0x3FF
        xo9 = xo10 & 0x1FF
        if xo9 in XO_ARITH:
            return XO_ARITH[xo9] + ("o" if xo10 & 0x200 else "") + ("." if rc else "")
        if xo10 in XO_NO_OE:
            return XO_NO_OE[xo10] + ("." if rc else "")
        if xo10 in X_LOGIC:
            return X_LOGIC[xo10] + ("." if rc else "")
        return G31_PLAIN.get(xo10)
    if op == 59:
        name = A59.get((w >> 1) & 0x1F)
        return name + ("." if rc else "") if name else None
    if op == 63:
        xo5 = (w >> 1) & 0x1F
        if xo5 in A63:
            return A63[xo5] + ("." if rc else "")
        xo10 = (w >> 1) & 0x3FF
        name = X63.get(xo10)
        if name is None:
            return None
        return name + ("." if rc and xo10 not in X63_NO_RC else "")
    if op == 4:
        xo6 = (w >> 1) & 0x3F
        if xo6 in XW4:
            return XW4[xo6]
        xo5 = (w >> 1) & 0x1F
        if xo5 in A4:
            return A4[xo5] + ("." if rc else "")
        name = X4.get((w >> 1) & 0x3FF)
        if name is None:
            return None
        return name + ("." if rc else "")
    return None


def all_classes():
    out = set(D_PRIMARY.values())
    for n in M_PRIMARY.values():
        out |= {n, n + "."}
    for base in ("bc", "b"):
        out |= {base, base + "l", base + "a", base + "la"}
    out |= {"sc", "bclr", "bclrl", "bcctr", "bcctrl"} | set(G19.values())
    for n in XO_ARITH.values():
        out |= {n, n + ".", n + "o", n + "o."}
    for n in list(XO_NO_OE.values()) + list(X_LOGIC.values()):
        out |= {n, n + "."}
    out |= set(G31_PLAIN.values())
    for n in list(A59.values()) + list(A63.values()) + list(A4.values()) + list(X4.values()):
        out |= {n, n + "."}
    for xo, n in X63.items():
        out |= {n} if xo in X63_NO_RC else {n, n + "."}
    out |= set(XW4.values())
    return sorted(out)
