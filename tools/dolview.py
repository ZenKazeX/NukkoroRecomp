#!/usr/bin/env python
"""Inspect the PAL main.dol: map guest addresses, dump words, disassemble PowerPC.

Usage:
  python tools/dolview.py disasm 0x80372640 40
  python tools/dolview.py hex 0x80003154 32
  python tools/dolview.py find 0x4E800020
"""
import struct
import sys
from pathlib import Path

DOL = Path(__file__).resolve().parents[2] / "game" / "sys" / "main.dol"

SPR_NAMES = {
    1: "XER", 8: "LR", 9: "CTR", 18: "DSISR", 19: "DAR", 22: "DEC", 25: "SDR1",
    26: "SRR0", 27: "SRR1", 272: "SPRG0", 273: "SPRG1", 274: "SPRG2", 275: "SPRG3",
    284: "TBL", 285: "TBU", 287: "PVR",
    528: "IBAT0U", 529: "IBAT0L", 530: "IBAT1U", 531: "IBAT1L",
    532: "IBAT2U", 533: "IBAT2L", 534: "IBAT3U", 535: "IBAT3L",
    536: "DBAT0U", 537: "DBAT0L", 538: "DBAT1U", 539: "DBAT1L",
    540: "DBAT2U", 541: "DBAT2L", 542: "DBAT3U", 543: "DBAT3L",
    912: "GQR0", 913: "GQR1", 914: "GQR2", 915: "GQR3", 916: "GQR4",
    917: "GQR5", 918: "GQR6", 919: "GQR7", 920: "HID2", 921: "WPAR",
    922: "DMAU", 923: "DMAL", 1008: "HID0", 1009: "HID1", 1010: "IABR",
    1011: "HID4", 1013: "DABR", 1017: "L2CR", 1019: "ICTC",
    1020: "THRM1", 1021: "THRM2", 1022: "THRM3",
}


class Image:
    def __init__(self, path):
        raw = open(path, "rb").read()
        self.raw = raw
        self.sections = []
        for i in range(7):
            off = struct.unpack_from(">I", raw, i * 4)[0]
            addr = struct.unpack_from(">I", raw, 0x48 + i * 4)[0]
            size = struct.unpack_from(">I", raw, 0x90 + i * 4)[0]
            if size:
                self.sections.append((addr, size, off, True))
        for i in range(11):
            off = struct.unpack_from(">I", raw, 0x1C + i * 4)[0]
            addr = struct.unpack_from(">I", raw, 0x64 + i * 4)[0]
            size = struct.unpack_from(">I", raw, 0xAC + i * 4)[0]
            if size:
                self.sections.append((addr, size, off, False))
        self.bss = struct.unpack_from(">I", raw, 0xD8)[0]
        self.bss_size = struct.unpack_from(">I", raw, 0xDC)[0]
        self.entry = struct.unpack_from(">I", raw, 0xE0)[0]

    def read32(self, address):
        for addr, size, off, _x in self.sections:
            if addr <= address < addr + size - 3:
                return struct.unpack_from(">I", self.raw, off + (address - addr))[0]
        return None

    def read_bytes(self, address, count):
        out = bytearray()
        for i in range(count):
            for addr, size, off, _x in self.sections:
                if addr <= address + i < addr + size:
                    out.append(self.raw[off + (address + i - addr)])
                    break
            else:
                out.append(0)
        return bytes(out)


def sx(value, bits):
    mask = 1 << (bits - 1)
    return (value ^ mask) - mask


def spr_of(instr):
    return ((instr >> 16) & 0x1F) | (((instr >> 11) & 0x1F) << 5)


def spr_name(number):
    return SPR_NAMES.get(number, "spr%d" % number)


CMP_CR = lambda i: (i >> 23) & 7


def disasm(instr, pc):
    op = instr >> 26
    d = (instr >> 21) & 31
    a = (instr >> 16) & 31
    b = (instr >> 11) & 31
    c = (instr >> 6) & 31
    simm = sx(instr & 0xFFFF, 16)
    uimm = instr & 0xFFFF
    rc = "." if instr & 1 else ""
    oe = "o" if (instr >> 10) & 1 else ""

    if op == 3:
        return "twi %d,r%d,%d" % (d, a, simm)
    if op == 7:
        return "mulli r%d,r%d,%d" % (d, a, simm)
    if op == 8:
        return "subfic r%d,r%d,%d" % (d, a, simm)
    if op == 10:
        return "cmplwi cr%d,r%d,0x%X" % (CMP_CR(instr), a, uimm)
    if op == 11:
        return "cmpwi cr%d,r%d,%d" % (CMP_CR(instr), a, simm)
    if op == 12:
        return "addic r%d,r%d,%d" % (d, a, simm)
    if op == 13:
        return "addic. r%d,r%d,%d" % (d, a, simm)
    if op == 14:
        return "li r%d,%d" % (d, simm) if a == 0 else "addi r%d,r%d,%d" % (d, a, simm)
    if op == 15:
        return "lis r%d,0x%X" % (d, uimm) if a == 0 else "addis r%d,r%d,0x%X" % (d, a, uimm)
    if op == 16:
        target = sx(instr & 0xFFFC, 16) + (0 if (instr >> 1) & 1 else pc)
        link = "l" if instr & 1 else ""
        absolute = "a" if (instr >> 1) & 1 else ""
        return "bc%s%s %d,%d,0x%08X" % (link, absolute, d, a, target & 0xFFFFFFFF)
    if op == 17:
        return "sc"
    if op == 18:
        target = sx(instr & 0x03FFFFFC, 26) + (0 if (instr >> 1) & 1 else pc)
        link = "l" if instr & 1 else ""
        absolute = "a" if (instr >> 1) & 1 else ""
        return "b%s%s 0x%08X" % (link, absolute, target & 0xFFFFFFFF)
    if op == 19:
        x = (instr >> 1) & 0x3FF
        names = {0: "mcrf", 16: "bclr", 50: "rfi", 150: "isync", 528: "bcctr",
                 33: "crnor", 129: "crandc", 193: "crxor", 225: "crnand",
                 257: "crand", 289: "creqv", 417: "crorc", 449: "cror"}
        if x == 16 and d == 20:
            return "blr" + ("l" if instr & 1 else "")
        if x == 528 and d == 20:
            return "bctr" + ("l" if instr & 1 else "")
        if x in (16, 528):
            return "%s %d,%d" % (names[x], d, a)
        return names.get(x, "op19.%d" % x)
    if op == 20:
        return "rlwimi%s r%d,r%d,%d,%d,%d" % (rc, a, d, b, (instr >> 6) & 31, (instr >> 1) & 31)
    if op == 21:
        mb, me, sh = (instr >> 6) & 31, (instr >> 1) & 31, b
        if mb == 0 and me == 31 - sh:
            return "slwi%s r%d,r%d,%d" % (rc, a, d, sh)
        if me == 31 and mb == 32 - sh and sh:
            return "srwi%s r%d,r%d,%d" % (rc, a, d, 32 - sh)
        return "rlwinm%s r%d,r%d,%d,%d,%d" % (rc, a, d, sh, mb, me)
    if op == 23:
        return "rlwnm%s r%d,r%d,r%d,%d,%d" % (rc, a, d, b, (instr >> 6) & 31, (instr >> 1) & 31)
    if op == 24:
        return "nop" if instr == 0x60000000 else "ori r%d,r%d,0x%X" % (a, d, uimm)
    if op == 25:
        return "oris r%d,r%d,0x%X" % (a, d, uimm)
    if op == 26:
        return "xori r%d,r%d,0x%X" % (a, d, uimm)
    if op == 27:
        return "xoris r%d,r%d,0x%X" % (a, d, uimm)
    if op == 28:
        return "andi. r%d,r%d,0x%X" % (a, d, uimm)
    if op == 29:
        return "andis. r%d,r%d,0x%X" % (a, d, uimm)
    if op == 31:
        x = (instr >> 1) & 0x3FF
        simple = {
            0: "cmpw", 32: "cmplw", 8: "subfc", 10: "addc", 11: "mulhwu", 19: "mfcr",
            20: "lwarx", 23: "lwzx", 24: "slw", 26: "cntlzw", 28: "and", 40: "subf",
            54: "dcbst", 55: "lwzux", 60: "andc", 75: "mulhw", 83: "mfmsr", 86: "dcbf",
            87: "lbzx", 104: "neg", 119: "lbzux", 124: "nor", 136: "subfe", 138: "adde",
            144: "mtcrf", 146: "mtmsr", 150: "stwcx.", 151: "stwx", 183: "stwux",
            200: "subfze", 202: "addze", 210: "mtsr", 215: "stbx", 232: "subfme",
            234: "addme", 235: "mullw", 246: "dcbtst", 247: "stbux", 266: "add",
            278: "dcbt", 279: "lhzx", 284: "eqv", 306: "tlbie", 311: "lhzux", 316: "xor",
            343: "lhax", 371: "mftb", 375: "lhaux", 407: "sthx", 412: "orc", 439: "sthux",
            444: "or", 459: "divwu", 470: "dcbi", 476: "nand", 491: "divw", 512: "mcrxr",
            533: "lswx", 534: "lwbrx", 535: "lfsx", 536: "srw", 566: "tlbsync",
            595: "mfsr", 597: "lswi", 598: "sync", 599: "lfdx", 631: "lfdux",
            661: "stswx", 662: "stwbrx", 663: "stfsx", 695: "stfsux", 725: "stswi",
            727: "stfdx", 759: "stfdux", 790: "lhbrx", 792: "sraw", 824: "srawi",
            854: "eieio", 918: "sthbrx", 922: "extsh", 954: "extsb", 982: "icbi",
            983: "stfiwx", 1014: "dcbz",
        }
        if x == 339:
            return "mfspr r%d,%s" % (d, spr_name(spr_of(instr)))
        if x == 467:
            return "mtspr %s,r%d" % (spr_name(spr_of(instr)), d)
        if x == 444 and d == b:
            return "mr%s r%d,r%d" % (rc, a, d)
        base = simple.get(x & 0x1FF) if (x & 0x200) and (x & 0x1FF) in simple else None
        if base:
            return "%s%s%s r%d,r%d,r%d" % (base, oe, rc, d, a, b)
        name = simple.get(x)
        if name:
            if x in (0, 32):
                return "%s cr%d,r%d,r%d" % (name, CMP_CR(instr), a, b)
            if x in (26, 922, 954):
                return "%s%s r%d,r%d" % (name, rc, a, d)
            if x in (19, 83):
                return "%s r%d" % (name, d)
            if x == 824:
                return "srawi%s r%d,r%d,%d" % (rc, a, d, b)
            if x in (144,):
                return "mtcrf 0x%02X,r%d" % ((instr >> 12) & 0xFF, d)
            return "%s%s r%d,r%d,r%d" % (name, rc, d, a, b)
        return "op31.%d" % x
    loads = {32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu",
             38: "stb", 39: "stbu", 40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau",
             44: "sth", 45: "sthu", 46: "lmw", 47: "stmw"}
    if op in loads:
        return "%s r%d,%d(r%d)" % (loads[op], d, simm, a)
    fl = {48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu",
          54: "stfd", 55: "stfdu"}
    if op in fl:
        return "%s f%d,%d(r%d)" % (fl[op], d, simm, a)
    if op in (56, 57, 60, 61):
        names = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}
        return "%s f%d,%d(r%d),%d,%d" % (names[op], d, sx(instr & 0xFFF, 12), a,
                                         (instr >> 15) & 1, (instr >> 12) & 7)
    if op == 59:
        x = (instr >> 1) & 0x1F
        names = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres",
                 25: "fmuls", 28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}
        return "%s%s f%d,f%d,f%d,f%d" % (names.get(x, "op59.%d" % x), rc, d, a, c, b)
    if op == 63:
        x5 = (instr >> 1) & 0x1F
        aform = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel",
                 25: "fmul", 26: "frsqrte", 28: "fmsub", 29: "fmadd", 30: "fnmsub",
                 31: "fnmadd"}
        if x5 in aform:
            return "%s%s f%d,f%d,f%d,f%d" % (aform[x5], rc, d, a, c, b)
        x = (instr >> 1) & 0x3FF
        names = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo",
                 38: "mtfsb1", 40: "fneg", 64: "mcrfs", 70: "mtfsb0", 72: "fmr",
                 134: "mtfsfi", 136: "fnabs", 264: "fabs", 583: "mffs", 711: "mtfsf"}
        if x in (0, 32):
            return "%s cr%d,f%d,f%d" % (names[x], CMP_CR(instr), a, b)
        return "%s%s f%d,f%d" % (names.get(x, "op63.%d" % x), rc, d, b)
    if op == 4:
        x6 = (instr >> 1) & 0x3F
        if x6 in (6, 7, 38, 39):
            n = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}[x6]
            return "%s f%d,r%d,r%d,%d,%d" % (n, d, a, b, (instr >> 10) & 1, (instr >> 7) & 7)
        x5 = (instr >> 1) & 0x1F
        aform = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1",
                 14: "ps_madds0", 15: "ps_madds1", 18: "ps_div", 20: "ps_sub",
                 21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul",
                 26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub",
                 31: "ps_nmadd"}
        if x5 in aform:
            return "%s%s f%d,f%d,f%d,f%d" % (aform[x5], rc, d, a, c, b)
        x = (instr >> 1) & 0x3FF
        names = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1",
                 72: "ps_mr", 96: "ps_cmpo1", 136: "ps_nabs", 264: "ps_abs",
                 528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10",
                 624: "ps_merge11", 1014: "dcbz_l"}
        return "%s f%d,f%d,f%d" % (names.get(x, "op4.%d" % x), d, a, b)
    return "?? op%d" % op


def main():
    image = Image(DOL)
    if len(sys.argv) < 2:
        print("entry 0x%08X bss 0x%08X+0x%X" % (image.entry, image.bss, image.bss_size))
        for addr, size, off, x in image.sections:
            print("  %s 0x%08X size 0x%X file 0x%X" % ("text" if x else "data", addr, size, off))
        return
    mode = sys.argv[1]
    if mode == "disasm":
        start = int(sys.argv[2], 0)
        count = int(sys.argv[3], 0) if len(sys.argv) > 3 else 32
        for index in range(count):
            pc = start + index * 4
            word = image.read32(pc)
            if word is None:
                print("%08X: <unmapped>" % pc)
                continue
            print("%08X: %08X  %s" % (pc, word, disasm(word, pc)))
    elif mode == "hex":
        start = int(sys.argv[2], 0)
        count = int(sys.argv[3], 0) if len(sys.argv) > 3 else 64
        data = image.read_bytes(start, count)
        for index in range(0, len(data), 16):
            chunk = data[index:index + 16]
            text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
            print("%08X: %-48s %s" % (start + index, chunk.hex(" "), text))
    elif mode == "find":
        needle = int(sys.argv[2], 0)
        for addr, size, off, _x in image.sections:
            for index in range(0, size - 3, 4):
                if struct.unpack_from(">I", image.raw, off + index)[0] == needle:
                    print("%08X" % (addr + index))
    else:
        print("unknown mode")


if __name__ == "__main__":
    main()
