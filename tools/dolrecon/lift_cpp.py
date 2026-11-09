"""C++ reconstruction of leaf functions (docs/DOLRECON.md "C++ reconstruction").

    python -B -m dolrecon.lift_cpp <project dir> <out .cpp> [--limit N]      (run from source/tools)

Input: a corrected, byte-exact project (dolrecon.corrections), its symbols and the original bytes. Output: one C++ source
(runtime/tests/dolrecon_lift.h interface) with every LIFTABLE leaf function and the kLifted table, plus
<out>.report.json (coverage, and every refusal by reason).

Liftable = no call (bl / bctrl / ...), no indirect jump, every branch target inside the function, every instruction
in the supported set below with an exact C++ meaning. Anything else is refused and counted - never approximated:
  integer   addi addis addic(.) subfic mulli ori oris xori xoris andi. andis. rlwimi rlwinm rlwnm cmpi cmpli
            add addc adde addze addme subf subfc subfe subfze subfme neg mullw mulhw mulhwu divw divwu and andc or orc
            xor nand nor eqv slw srw sraw srawi cntlzw extsb extsh cmp cmpl (OE=0 forms; Rc=1 sets CR0)
  memory    lwz lbz lhz lha stw stb sth (+u, +x, +ux) lmw stmw lwbrx lhbrx stwbrx sthbrx dcbz;
            lfd lfdu lfdx stfd stfdu stfdx (bit copies) lfs lfsu lfsx (architecture single->double, bit exact)
            stfs/stfsu/stfsx (the architecture's store conversion for every double) and stfiwx
  fp moves  fmr fneg fabs fnabs (Rc=0)
  cr / spr  crand crandc creqv crnand crnor cror crorc crxor mcrf mfcr mtcrf mfspr/mtspr of LR CTR XER
  flow      b / bc / bclr (blr, conditional returns), CTR decrementing forms, all inside the function
  no-ops    sync isync eieio dcbf dcbst dcbt dcbtst icbi
  fp        scalar arithmetic (op 59 single, op 63 double), fsel frsp fctiw fctiwz fcmpu fcmpo mffs mtfsf mtfsb0/1
            mtfsfi mcrfs and record forms, through runtime/tests/dolrecon_fp.h (an independent formulation of the
            Gekko rules: NaN priority, FPSCR[NI], single rounding with exact fused tie-breaks)
  ps        paired singles: psq_l / psq_st (+u, +x, +ux) through GQR0-7 (kept in the state), the ps_ arithmetic
            (two single-precision lanes by the same rules), ps_sum0/1, ps_muls0/1, ps_madds0/1, ps_sel, ps_cmpu/o0/1,
            ps_neg / mr / nabs / abs, ps_merge00/01/10/11, dcbz_l
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys

from . import audit as auditmod
from . import dol as dolmod


class Refuse(Exception):
    pass


def bits(w, a, b):  # PowerPC bit numbering: bit 0 = MSB
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def hx(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


class Fn:
    """Lifts one function: collects statements, register uses and labels."""

    def __init__(self, image, start, size, name, symbols_at, ctx=None):
        self.image, self.start, self.size, self.name = image, start, size, name
        self.symbols_at = symbols_at
        # ctx: {"entries": {address: name} every callable start (functions + labelled entries),
        #       "jt_targets": set of code addresses some data word points at (switch tables, callbacks)}
        self.ctx = ctx or {"entries": {}, "jt_targets": set()}
        self.callees = set()       # direct call / tail-call targets (resolved to lift_ or dispatch at render)
        self.calls = False
        # Pointer flow for TEST INPUTS only (never for semantics): origin[n] = the input register rN's value derives
        # from through copies / constant offsets (mr, addi), None once computed otherwise or clobbered by a call.
        self.origin = {}
        self.call_sites = []       # (target, {arg register: origin input register}) for interprocedural pointers
        self.stmts: list[tuple[int, str]] = []
        self.gpr_read, self.gpr_written = set(), set()
        self.gpr_in = set()
        self.fpr_in, self.fpr_written = set(), set()
        self.fpr_from_lfs = {}  # fpr -> True when its value is the bit-exact result of an lfs (single-exact)
        self.base_regs = set()
        self.index_regs = set()
        self.ps1_written = set()
        self.ps1_read = set()
        self.ctr_loop = False
        self.sda = {13: 0x805EC500, 2: 0x805FA780}  # EABI small-data bases (the harness checks them)
        self.uses_cr = self.uses_xer = self.uses_ctr = self.uses_lr = self.uses_fpscr = False
        self.labels = set()

    # ---- register helpers (track read-before-write = inputs) --------------------------------------------------
    def origin_of(self, n):
        if n in self.origin:
            return self.origin[n]
        return n if n not in self.gpr_written else None

    def mark_base(self, n):
        o = self.origin_of(n)
        if o is not None and not (o in self.sda and o not in self.gpr_written):
            self.base_regs.add(o)

    def rg(self, n):
        if n in self.sda and n not in self.gpr_written:
            return f"{hx(self.sda[n])} /* r{n}: small-data base */"
        if n not in self.gpr_written:
            self.gpr_in.add(n)
        self.gpr_read.add(n)
        return f"r{n}"

    def wg(self, n, origin=None):
        self.gpr_written.add(n)
        self.origin[n] = origin
        return f"r{n}"

    def ra0(self, n):  # (rA|0)
        return "0u" if n == 0 else self.rg(n)

    def rf(self, n):
        if n not in self.fpr_written:
            self.fpr_in.add(n)
        return f"f{n}"

    def wf(self, n, from_lfs=False):
        self.fpr_written.add(n)
        self.fpr_from_lfs[n] = from_lfs
        return f"f{n}"

    def lfs_into(self, pc, n, expr):
        """lfs family: the expanded single goes to ps0 AND ps1 (Gekko)."""
        self.emit(pc, f"{self.wf(n, True)} = {expr};")
        self.ps1_written.add(n)
        self.emit(pc, f"p{n} = f{n};  // lfs also fills ps1")

    def ea_of(self, ra, si):
        """(rA|0) + d, folded to an absolute address (with its symbol) when rA is a small-data base."""
        if ra in self.sda and ra not in self.gpr_written:
            v = (self.sda[ra] + si) & 0xFFFFFFFF
            return f"{hx(v)}{self.sym(v)}"
        if ra != 0:
            self.mark_base(ra)
        return f"{self.ra0(ra)} + {hx(si)}"

    def emit(self, pc, text):
        self.stmts.append((pc, text))

    def sym(self, v):
        n = self.symbols_at.get(v & 0xFFFFFFFF)
        return f" /* {n} */" if n else ""

    def cr0(self, pc, reg):
        self.uses_cr = self.uses_xer = True
        self.emit(pc, f"set_crf(cr, 0, crf_signed(s32({reg}), 0, xer));")

    # ---- the instruction set ---------------------------------------------------------------------------------
    def lift_one(self, pc, w):
        op = w >> 26
        rd, ra, rb = bits(w, 6, 10), bits(w, 11, 15), bits(w, 16, 20)
        rc = w & 1
        imm = w & 0xFFFF
        si = simm(w)
        E = self.emit
        if op == 14:  # addi / li
            if ra == 0:
                E(pc, f"{self.wg(rd)} = {hx(si)};{self.sym(si)}")
            else:
                o = self.origin_of(ra)
                a = self.rg(ra)
                E(pc, f"{self.wg(rd, o)} = {a} + {hx(si)};")
        elif op == 15:  # addis / lis
            if ra == 0:
                E(pc, f"{self.wg(rd)} = {hx(imm << 16)};")
            else:
                a = self.rg(ra)
                E(pc, f"{self.wg(rd)} = {a} + {hx(imm << 16)};")
        elif op in (12, 13):  # addic, addic.
            a = self.rg(ra)
            self.uses_xer = True
            E(pc, f"{{ const u64 t = u64({a}) + u64({hx(si)}); set_ca(xer, (t >> 32) != 0); {self.wg(rd)} = u32(t); }}")
            if op == 13:
                self.cr0(pc, f"r{rd}")
        elif op == 8:  # subfic
            a = self.rg(ra)
            self.uses_xer = True
            E(pc, f"{{ const u64 t = u64(~{a}) + u64({hx(si)}) + 1u; set_ca(xer, (t >> 32) != 0); {self.wg(rd)} = u32(t); }}")
        elif op == 7:  # mulli
            a = self.rg(ra)
            # Unsigned multiply: the low 32 bits of the product are the same, and unsigned overflow is defined
            # (s32 * s32 overflowing is undefined behaviour in C++ - a passing test would have proved nothing).
            E(pc, f"{self.wg(rd)} = {a} * {hx(si)};")
        elif op in (24, 25, 26, 27, 28, 29):
            s = self.rg(rd)
            v = imm << 16 if op in (25, 27, 29) else imm
            o = {24: "|", 25: "|", 26: "^", 27: "^", 28: "&", 29: "&"}[op]
            E(pc, f"{self.wg(ra)} = {s} {o} {hx(v)};")
            if op in (28, 29):
                self.cr0(pc, f"r{ra}")
        elif op in (20, 21, 23):  # rlwimi rlwinm rlwnm
            s = self.rg(rd)
            mb, me = bits(w, 21, 25), bits(w, 26, 30)
            m = f"mask32({mb}u, {me}u)"
            sh = f"{bits(w, 16, 20)}u" if op != 23 else f"({self.rg(rb)} & 31u)"
            if op == 20:
                old = self.rg(ra)
                E(pc, f"{self.wg(ra)} = (rotl32({s}, {sh}) & {m}) | ({old} & ~{m});")
            else:
                E(pc, f"{self.wg(ra)} = rotl32({s}, {sh}) & {m};")
            if rc:
                self.cr0(pc, f"r{ra}")
        elif op in (10, 11):  # cmpli cmpi
            crf = bits(w, 6, 8)
            if bits(w, 10, 10):
                raise Refuse("cmp_L1")
            a = self.rg(ra)
            self.uses_cr = self.uses_xer = True
            if op == 11:
                E(pc, f"set_crf(cr, {crf}, crf_signed(s32({a}), {si}, xer));")
            else:
                E(pc, f"set_crf(cr, {crf}, crf_unsigned({a}, {hx(imm)}, xer));")
        elif op in (32, 33, 34, 35, 40, 41, 42, 43, 36, 37, 38, 39, 44, 45):
            self.dform_int(pc, op, rd, ra, si)
        elif op in (46, 47):  # lmw stmw
            sda_base = ra in self.sda and ra not in self.gpr_written
            if ra != 0 and not sda_base:
                self.mark_base(ra)
            base = hx(self.sda[ra]) if sda_base else self.ra0(ra)
            for i, r in enumerate(range(rd, 32)):
                if op == 46:
                    if r == ra:
                        raise Refuse("lmw_overwrites_base")
                    E(pc, f"{self.wg(r)} = m.r32({base} + {hx(si + 4 * i)});")
                else:
                    E(pc, f"m.w32({base} + {hx(si + 4 * i)}, {self.rg(r)});")
        elif op in (48, 49, 50, 51, 52, 53, 54, 55):
            self.dform_fp(pc, op, rd, ra, si)
        elif op == 31:
            self.x31(pc, w, rd, ra, rb, rc)
        elif op == 63:
            self.fp63(pc, w)
        elif op == 59:
            self.fp59(pc, w)
        elif op == 4:
            self.ps4(pc, w)
        elif op in (56, 57, 60, 61):
            self.psq_dform(pc, w, op)
        elif op == 19:
            self.x19(pc, w)
        elif op in (16, 18):
            self.branch(pc, w)
        else:
            raise Refuse(f"op{op}")

    def dform_int(self, pc, op, rd, ra, si):
        load = op in (32, 33, 34, 35, 40, 41, 42, 43)
        update = op in (33, 35, 41, 43, 37, 39, 45)
        if update and (ra == 0 or (load and ra == rd)):
            raise Refuse("invalid_update_form")
        ea = self.ea_of(ra, si)
        E = self.emit
        if update:
            E(pc, f"{{ const u32 ea = {ea};")
            ea = "ea"
        if load:
            width = {32: 32, 33: 32, 34: 8, 35: 8, 40: 16, 41: 16, 42: 16, 43: 16}[op]
            expr = f"m.r{width}({ea})" if op not in (42, 43) else f"u32(s32(s16(m.r16({ea}))))"
            if width != 32 and op not in (42, 43):
                expr = f"u32({expr})"
            E(pc, f"{self.wg(rd)} = {expr};")
        else:
            width = {36: 32, 37: 32, 38: 8, 39: 8, 44: 16, 45: 16}[op]
            val = self.rg(rd)
            cast = {32: "", 8: "u8", 16: "u16"}[width]
            E(pc, f"m.w{width}({ea}, {cast}({val}));")
        if update:
            E(pc, f"{self.wg(ra)} = ea; }}")

    def dform_fp(self, pc, op, fd, ra, si):
        update = op in (49, 51, 53, 55)
        if update and ra == 0:
            raise Refuse("invalid_update_form")
        ea = self.ea_of(ra, si)
        E = self.emit
        if update:
            E(pc, f"{{ const u32 ea = {ea};")
            ea = "ea"
        if op in (48, 49):
            self.lfs_into(pc, fd, f"single_to_double_bits(m.r32({ea}))")
        elif op in (50, 51):
            E(pc, f"{self.wf(fd)} = m.r64({ea});")
        elif op in (52, 53):
            E(pc, f"m.w32({ea}, double_to_single_bits({self.rf(fd)}));")
        else:
            E(pc, f"m.w64({ea}, {self.rf(fd)});")
        if update:
            E(pc, f"{self.wg(ra)} = ea; }}")

    def x31(self, pc, w, rd, ra, rb, rc):
        xo10, xo9, oe = bits(w, 21, 30), bits(w, 22, 30), bits(w, 21, 21)
        E = self.emit
        arith = {266: "add", 10: "addc", 138: "adde", 202: "addze", 234: "addme", 40: "subf", 8: "subfc",
                 136: "subfe", 200: "subfze", 232: "subfme", 104: "neg", 235: "mullw", 75: "mulhw", 11: "mulhwu",
                 491: "divw", 459: "divwu"}
        if xo9 in arith and xo10 not in (0, 32):
            if oe:
                raise Refuse("oe_form")
            name = arith[xo9]
            a = self.rg(ra) if name not in () else None
            b = self.rg(rb) if name in ("add", "addc", "adde", "subf", "subfc", "subfe", "mullw", "mulhw",
                                        "mulhwu", "divw", "divwu") else None
            d = f"r{rd}"
            if name in ("addc", "adde", "addze", "addme", "subfc", "subfe", "subfze", "subfme"):
                self.uses_xer = True
            expr = {
                "add": f"{d} = {a} + {b};",
                "addc": f"{{ const u64 t = u64({a}) + u64({b}); set_ca(xer, (t >> 32) != 0); {d} = u32(t); }}",
                "adde": f"{{ const u64 t = u64({a}) + u64({b}) + ca_of(xer); set_ca(xer, (t >> 32) != 0); {d} = u32(t); }}",
                "addze": f"{{ const u64 t = u64({a}) + ca_of(xer); set_ca(xer, (t >> 32) != 0); {d} = u32(t); }}",
                "addme": f"{{ const u64 t = u64({a}) + ca_of(xer) + 0xFFFFFFFFull; set_ca(xer, (t >> 32) != 0); {d} = u32(t); }}",
                "subf": f"{d} = {b} - {a};",
                "subfc": f"{{ const u64 t = u64(~{a}) + u64({b}) + 1u; set_ca(xer, (t >> 32) != 0); {d} = u32(t); }}",
                "subfe": f"{{ const u64 t = u64(~{a}) + u64({b}) + ca_of(xer); set_ca(xer, (t >> 32) != 0); {d} = u32(t); }}",
                "subfze": f"{{ const u64 t = u64(~{a}) + ca_of(xer); set_ca(xer, (t >> 32) != 0); {d} = u32(t); }}",
                "subfme": f"{{ const u64 t = u64(~{a}) + ca_of(xer) + 0xFFFFFFFFull; set_ca(xer, (t >> 32) != 0); {d} = u32(t); }}",
                "neg": f"{d} = 0u - {a};",
                "mullw": f"{d} = u32(s64(s32({a})) * s64(s32({b})));",
                "mulhw": f"{d} = u32((s64(s32({a})) * s64(s32({b}))) >> 32);",
                "mulhwu": f"{d} = u32((u64({a}) * u64({b})) >> 32);",
                "divw": f"{d} = (s32({b}) == 0 || ({a} == 0x80000000u && s32({b}) == -1)) ? (s32({b}) == 0 ? (s32({a}) < 0 ? 0xFFFFFFFFu : 0u) : 0xFFFFFFFFu) : u32(s32({a}) / s32({b}));",
                "divwu": f"{d} = {b} == 0u ? 0u : {a} / {b};",
            }[name]
            self.wg(rd)
            E(pc, expr)
            if rc:
                self.cr0(pc, d)
            return
        logic = {28: "&", 60: "&~", 444: "|", 412: "|~", 316: "^", 476: "nand", 124: "nor", 284: "eqv"}
        if xo10 in logic:
            keep = self.origin_of(rd) if (xo10 == 444 and rd == rb) else None  # mr rA, rS
            s, b = self.rg(rd), self.rg(rb)
            o = logic[xo10]
            expr = {"&~": f"{s} & ~{b}", "|~": f"{s} | ~{b}", "nand": f"~({s} & {b})", "nor": f"~({s} | {b})",
                    "eqv": f"~({s} ^ {b})"}.get(o, f"{s} {o} {b}")
            E(pc, f"{self.wg(ra, keep)} = {expr};")
            if rc:
                self.cr0(pc, f"r{ra}")
            return
        if xo10 in (24, 536, 792, 824, 26, 954, 922):
            s = self.rg(rd)
            if xo10 == 24:
                b = self.rg(rb)
                E(pc, f"{self.wg(ra)} = ({b} & 32u) ? 0u : ({s} << ({b} & 31u));")
            elif xo10 == 536:
                b = self.rg(rb)
                E(pc, f"{self.wg(ra)} = ({b} & 32u) ? 0u : ({s} >> ({b} & 31u));")
            elif xo10 == 792:
                b = self.rg(rb)
                self.uses_xer = True
                E(pc, f"{{ const u32 n = {b} & 63u; const s32 v = s32({s}); if (n >= 32u) {{ set_ca(xer, v < 0); "
                      f"{self.wg(ra)} = v < 0 ? 0xFFFFFFFFu : 0u; }} else {{ set_ca(xer, v < 0 && ({s} & ((1u << n) - 1u)) != 0u); "
                      f"r{ra} = u32(v >> n); }} }}")
            elif xo10 == 824:
                sh = rb
                self.uses_xer = True
                E(pc, f"{{ const s32 v = s32({s}); set_ca(xer, v < 0 && ({s} & {hx((1 << sh) - 1)}) != 0u); "
                      f"{self.wg(ra)} = u32(v >> {sh}); }}")
            elif xo10 == 26:
                E(pc, f"{self.wg(ra)} = cntlz({s});")
            elif xo10 == 954:
                E(pc, f"{self.wg(ra)} = u32(s32(s8(u8({s}))));")
            else:
                E(pc, f"{self.wg(ra)} = u32(s32(s16(u16({s}))));")
            if rc:
                self.cr0(pc, f"r{ra}")
            return
        if xo10 in (0, 32):  # cmp cmpl
            if bits(w, 10, 10):
                raise Refuse("cmp_L1")
            crf = bits(w, 6, 8)
            a, b = self.rg(ra), self.rg(rb)
            self.uses_cr = self.uses_xer = True
            if xo10 == 0:
                E(pc, f"set_crf(cr, {crf}, crf_signed(s32({a}), s32({b}), xer));")
            else:
                E(pc, f"set_crf(cr, {crf}, crf_unsigned({a}, {b}, xer));")
            return
        if rc and xo10 not in (150,):
            raise Refuse(f"x31_{xo10}_rc")
        idx_loads = {23: (32, False, False), 55: (32, True, False), 87: (8, False, False), 119: (8, True, False),
                     279: (16, False, False), 311: (16, True, False), 343: (16, False, True), 375: (16, True, True)}
        idx_stores = {151: (32, False), 183: (32, True), 215: (8, False), 247: (8, True), 407: (16, False),
                      439: (16, True)}
        if xo10 in idx_loads or xo10 in idx_stores or xo10 in (534, 790, 662, 918, 1014, 535, 567, 599, 631,
                                                               663, 695, 727, 759, 983):
            update = (xo10 in idx_loads and idx_loads[xo10][1]) or (xo10 in idx_stores and idx_stores[xo10][1]) or \
                xo10 in (567, 631, 695, 759)
            is_load = xo10 in idx_loads or xo10 in (534, 790, 535, 567, 599, 631)
            if update and (ra == 0 or (is_load and xo10 in idx_loads and ra == rd)):
                raise Refuse("invalid_update_form")
            if ra != 0:
                self.mark_base(ra)
            o = self.origin_of(rb)
            if o is not None:
                self.index_regs.add(o)
            ea_expr = f"{self.ra0(ra)} + {self.rg(rb)}"
            if update:
                E(pc, f"{{ const u32 ea = {ea_expr};")
                ea = "ea"
            else:
                ea = f"({ea_expr})"
            if xo10 in idx_loads:
                width, _, alg = idx_loads[xo10]
                expr = f"u32(s32(s16(m.r16({ea}))))" if alg else (f"m.r32({ea})" if width == 32 else f"u32(m.r{width}({ea}))")
                E(pc, f"{self.wg(rd)} = {expr};")
            elif xo10 in idx_stores:
                width, _ = idx_stores[xo10]
                cast = {32: "", 8: "u8", 16: "u16"}[width]
                E(pc, f"m.w{width}({ea}, {cast}({self.rg(rd)}));")
            elif xo10 == 534:
                E(pc, f"{self.wg(rd)} = bswap32(m.r32({ea}));")
            elif xo10 == 790:
                E(pc, f"{self.wg(rd)} = bswap16(m.r16({ea}));")
            elif xo10 == 662:
                E(pc, f"m.w32({ea}, bswap32({self.rg(rd)}));")
            elif xo10 == 918:
                E(pc, f"m.w16({ea}, u16(bswap16({self.rg(rd)})));")
            elif xo10 == 1014:  # dcbz
                E(pc, f"{{ const u32 z = {ea} & ~31u; for (u32 i = 0; i < 32u; i += 8u) m.w64(z + i, 0u); }}")
            elif xo10 in (535, 567):
                self.lfs_into(pc, rd, f"single_to_double_bits(m.r32({ea}))")
            elif xo10 in (599, 631):
                E(pc, f"{self.wf(rd)} = m.r64({ea});")
            elif xo10 in (663, 695):
                E(pc, f"m.w32({ea}, double_to_single_bits({self.rf(rd)}));")
            elif xo10 in (727, 759):
                E(pc, f"m.w64({ea}, {self.rf(rd)});")
            else:  # 983 stfiwx
                E(pc, f"m.w32({ea}, u32({self.rf(rd)}));")
            if update:
                E(pc, f"{self.wg(ra)} = ea; }}")
            return
        if xo10 == 19:  # mfcr
            self.uses_cr = True
            E(pc, f"{self.wg(rd)} = cr;")
            return
        if xo10 == 144:  # mtcrf
            crm = bits(w, 12, 19)
            mask = 0
            for i in range(8):
                if crm & (0x80 >> i):
                    mask |= 0xF << (28 - 4 * i)
            self.uses_cr = True
            E(pc, f"cr = (cr & {hx(~mask)}) | ({self.rg(rd)} & {hx(mask)});")
            return
        if xo10 in (339, 467):
            spr = (bits(w, 16, 20) << 5) | bits(w, 11, 15)
            if 912 <= spr <= 919:  # GQR0-7: kept in the state (not cached in a local)
                if xo10 == 339:
                    E(pc, f"{self.wg(rd)} = s.gqr[{spr - 912}];")
                else:
                    E(pc, f"s.gqr[{spr - 912}] = {self.rg(rd)};")
                return
            reg = {1: "xer", 8: "lr", 9: "ctr"}.get(spr)
            if reg is None:
                raise Refuse(f"spr{spr}")
            setattr(self, f"uses_{reg}", True)
            if xo10 == 339:
                E(pc, f"{self.wg(rd)} = {reg};")
            else:
                E(pc, f"{reg} = {self.rg(rd)};")
            return
        if xo10 in (598, 854, 86, 54, 278, 246, 982):  # sync eieio dcbf dcbst dcbt dcbtst icbi
            E(pc, "/* cache / ordering: no effect on the state this function computes */")
            return
        raise Refuse(f"x31_{xo10}")

    # ---- scalar floating point (runtime/tests/dolrecon_fp.h) ----------------------------------------------------
    FP_OPS = {18: "Div", 20: "Sub", 21: "Add", 22: "Sqrt", 24: "Res", 25: "Mul", 26: "Rsqrte", 28: "MSub", 29: "MAdd",
              30: "NMSub", 31: "NMAdd"}
    FP_USES = {"Div": "ab", "Sub": "ab", "Add": "ab", "Sqrt": "b", "Res": "b", "Mul": "ac", "Rsqrte": "b",
               "MSub": "abc", "MAdd": "abc", "NMSub": "abc", "NMAdd": "abc", "Round": "b"}

    # ---- paired singles --------------------------------------------------------------------------------------------
    def rp(self, n):  # ps1 of FPR n
        if n not in self.ps1_written:
            self.ps1_read.add(n)
        return f"p{n}"

    def wp(self, n):
        self.ps1_written.add(n)
        return f"p{n}"

    def psq(self, pc, fd, ea_expr, w_bit, gqr_index, load, update_ra):
        E = self.emit
        g = f"s.gqr[{gqr_index}]"
        if update_ra is not None:
            E(pc, f"{{ const u32 ea = {ea_expr};")
            ea = "ea"
        else:
            ea = f"({ea_expr})"
        if load:
            if w_bit:
                E(pc, f"{self.wf(fd)} = fp::psq_load_element(m, {ea}, {g}); {self.wp(fd)} = 0x3FF0000000000000ull;")
            else:
                E(pc, f"{{ const u64 e0 = fp::psq_load_element(m, {ea}, {g}); const u64 e1 = fp::psq_load_element(m, "
                      f"{ea} + fp::element_size(({g} >> 16) & 7u), {g}); {self.wf(fd)} = e0; {self.wp(fd)} = e1; }}")
        else:
            E(pc, f"fp::psq_store_element(m, {ea}, {self.rf(fd)}, {g});")
            if not w_bit:
                E(pc, f"fp::psq_store_element(m, {ea} + fp::element_size({g} & 7u), {self.rp(fd)}, {g});")
        if update_ra is not None:
            E(pc, f"{self.wg(update_ra)} = ea; }}")

    def psq_dform(self, pc, w, op):
        fd, ra = bits(w, 6, 10), bits(w, 11, 15)
        update = op in (57, 61)
        if update and ra == 0:
            raise Refuse("invalid_update_form")
        disp = w & 0xFFF
        if disp & 0x800:
            disp -= 0x1000
        ea = self.ea_of(ra, disp)
        self.psq(pc, fd, ea, (w >> 15) & 1, (w >> 12) & 7, op in (56, 57), ra if update else None)

    PS_PAIR = {18: ("Div", "ab"), 20: ("Sub", "ab"), 21: ("Add", "ab"), 24: ("Res", "b"), 25: ("Mul", "ac"),
               26: ("Rsqrte", "b"), 28: ("MSub", "abc"), 29: ("MAdd", "abc"), 30: ("NMSub", "abc"),
               31: ("NMAdd", "abc")}

    def ps4(self, pc, w):
        E = self.emit
        fd, fa, fb, fc = bits(w, 6, 10), bits(w, 11, 15), bits(w, 16, 20), bits(w, 21, 25)
        xo6 = (w >> 1) & 0x3F
        if xo6 in (6, 38, 7, 39):  # psq_lx psq_lux psq_stx psq_stux
            update = xo6 in (38, 39)
            if update and fa == 0:
                raise Refuse("invalid_update_form")
            if fa != 0:
                self.mark_base(fa)
            o = self.origin_of(fb)
            if o is not None:
                self.index_regs.add(o)
            ea = f"{self.ra0(fa)} + {self.rg(fb)}"
            self.psq(pc, fd, ea, (w >> 10) & 1, (w >> 7) & 7, xo6 in (6, 38), fa if update else None)
            return
        xo5 = (w >> 1) & 0x1F
        self.uses_fpscr = True

        def lane(name, uses, a, b_, c):
            return (f"fp::arith(fpscr, fp::Op::{name}, true, {a if 'a' in uses else '0u'}, "
                    f"{b_ if 'b' in uses else '0u'}, {c if 'c' in uses else '0u'})")

        if xo5 in self.PS_PAIR:
            name, uses = self.PS_PAIR[xo5]
            A0, A1 = (self.rf(fa), self.rp(fa)) if "a" in uses else ("0u", "0u")
            B0, B1 = (self.rf(fb), self.rp(fb)) if "b" in uses else ("0u", "0u")
            C0, C1 = (self.rf(fc), self.rp(fc)) if "c" in uses else ("0u", "0u")
            E(pc, f"{{ const u64 t0 = {lane(name, uses, A0, B0, C0)}; const u64 t1 = {lane(name, uses, A1, B1, C1)}; "
                  f"{self.wf(fd)} = t0; {self.wp(fd)} = t1; }}")
        elif xo5 in (12, 13, 14, 15):  # ps_muls0/1, ps_madds0/1: frC's ps0 (0) or ps1 (1) in both lanes
            c = self.rf(fc) if xo5 in (12, 14) else self.rp(fc)
            A0, A1 = self.rf(fa), self.rp(fa)
            if xo5 in (12, 13):
                E(pc, f"{{ const u64 t0 = {lane('Mul', 'ac', A0, '0u', c)}; const u64 t1 = {lane('Mul', 'ac', A1, '0u', c)}; "
                      f"{self.wf(fd)} = t0; {self.wp(fd)} = t1; }}")
            else:
                B0, B1 = self.rf(fb), self.rp(fb)
                E(pc, f"{{ const u64 t0 = {lane('MAdd', 'abc', A0, B0, c)}; const u64 t1 = {lane('MAdd', 'abc', A1, B1, c)}; "
                      f"{self.wf(fd)} = t0; {self.wp(fd)} = t1; }}")
        elif xo5 in (10, 11):  # ps_sum0 / ps_sum1
            sumx = lane("Add", "ab", self.rf(fa), self.rp(fb), "0u")
            if xo5 == 10:
                E(pc, f"{{ const u64 t0 = {sumx}; const u64 t1 = {self.rp(fc)}; {self.wf(fd)} = t0; {self.wp(fd)} = t1; }}")
            else:
                E(pc, f"{{ const u64 t1 = {sumx}; const u64 t0 = {self.rf(fc)}; {self.wf(fd)} = t0; {self.wp(fd)} = t1; }}")
        elif xo5 == 23:  # ps_sel
            E(pc, f"{{ const u64 t0 = fp::sel({self.rf(fa)}, {self.rf(fb)}, {self.rf(fc)}); const u64 t1 = "
                  f"fp::sel({self.rp(fa)}, {self.rp(fb)}, {self.rp(fc)}); {self.wf(fd)} = t0; {self.wp(fd)} = t1; }}")
        else:
            xo = (w >> 1) & 0x3FF
            if xo in (0, 32, 64, 96):
                hi = xo >= 64
                a = self.rp(fa) if hi else self.rf(fa)
                b_ = self.rp(fb) if hi else self.rf(fb)
                self.uses_cr = True
                E(pc, f"set_crf(cr, {bits(w, 6, 8)}, fp::compare({a}, {b_}, {'true' if xo in (32, 96) else 'false'}, fpscr));")
                return
            if xo in (40, 72, 136, 264):
                opx = {40: " ^ 0x8000000000000000ull", 72: "", 136: " | 0x8000000000000000ull",
                       264: " & 0x7FFFFFFFFFFFFFFFull"}[xo]
                E(pc, f"{{ const u64 t0 = {self.rf(fb)}{opx}; const u64 t1 = {self.rp(fb)}{opx}; "
                      f"{self.wf(fd)} = t0; {self.wp(fd)} = t1; }}")
            elif xo in (528, 560, 592, 624):
                first = self.rp(fa) if xo in (592, 624) else self.rf(fa)
                second = self.rp(fb) if xo in (560, 624) else self.rf(fb)
                E(pc, f"{{ const u64 t0 = {first}; const u64 t1 = {second}; {self.wf(fd)} = t0; {self.wp(fd)} = t1; }}")
            elif xo == 1014:  # dcbz_l
                if fa != 0:
                    self.mark_base(fa)
                E(pc, f"{{ const u32 z = ({self.ra0(fa)} + {self.rg(fb)}) & ~31u; for (u32 i = 0; i < 32u; i += 4u) "
                      f"m.w32(z + i, 0u); }}")
                return
            else:
                raise Refuse(f"ps_{xo}")
        self.cr1(pc, w)

    def cr1(self, pc, w):
        if w & 1:
            self.uses_cr = True
            self.emit(pc, "set_crf(cr, 1, fpscr >> 28);")

    def fp_call(self, pc, op_name, single, fd, fa, fb, fc, dup_ps1):
        uses = self.FP_USES[op_name]
        a = self.rf(fa) if "a" in uses else "0u"
        b_ = self.rf(fb) if "b" in uses else "0u"
        c = self.rf(fc) if "c" in uses else "0u"
        self.uses_fpscr = True
        self.emit(pc, f"{self.wf(fd)} = fp::arith(fpscr, fp::Op::{op_name}, {'true' if single else 'false'}, "
                      f"{a}, {b_}, {c});")
        if dup_ps1:
            self.ps1_written.add(fd)
            self.emit(pc, f"p{fd} = f{fd};  // single precision fills ps1 too")

    def fp59(self, pc, w):
        xo = bits(w, 26, 30)
        name = self.FP_OPS.get(xo)
        if name is None or xo == 26:
            raise Refuse(f"op59_{xo}")
        fd, fa, fb, fc = bits(w, 6, 10), bits(w, 11, 15), bits(w, 16, 20), bits(w, 21, 25)
        self.fp_call(pc, name, True, fd, fa, fb, fc, True)
        self.cr1(pc, w)

    def fp63(self, pc, w):
        fd, fa, fb, fc = bits(w, 6, 10), bits(w, 11, 15), bits(w, 16, 20), bits(w, 21, 25)
        xo5 = bits(w, 26, 30)
        E = self.emit
        if xo5 in (18, 20, 21, 22, 25, 26, 28, 29, 30, 31):
            self.fp_call(pc, self.FP_OPS[xo5], False, fd, fa, fb, fc, False)
            self.cr1(pc, w)
            return
        if xo5 == 23:  # fsel
            E(pc, f"{self.wf(fd)} = fp::fsel({self.rf(fa)}, {self.rf(fb)}, {self.rf(fc)});")
            self.cr1(pc, w)
            return
        xo = bits(w, 21, 30)
        self.uses_fpscr = True
        if xo in (0, 32):  # fcmpu fcmpo
            crf = bits(w, 6, 8)
            self.uses_cr = True
            E(pc, f"set_crf(cr, {crf}, fp::compare({self.rf(fa)}, {self.rf(fb)}, {'true' if xo == 32 else 'false'}, fpscr));")
            return
        if xo == 12:  # frsp: ps0 only
            self.fp_call(pc, "Round", True, fd, 0, fb, 0, False)
            self.cr1(pc, w)
            return
        if xo in (14, 15):
            E(pc, f"{self.wf(fd)} = fp::fctiw(fpscr, {self.rf(fb)}, {'true' if xo == 15 else 'false'});")
        elif xo == 72:
            E(pc, f"{self.wf(fd)} = {self.rf(fb)};")
        elif xo == 40:
            E(pc, f"{self.wf(fd)} = {self.rf(fb)} ^ 0x8000000000000000ull;")
        elif xo == 264:
            E(pc, f"{self.wf(fd)} = {self.rf(fb)} & 0x7FFFFFFFFFFFFFFFull;")
        elif xo == 136:
            E(pc, f"{self.wf(fd)} = {self.rf(fb)} | 0x8000000000000000ull;")
        elif xo == 583:  # mffs
            E(pc, f"{self.wf(fd)} = 0xFFF8000000000000ull | fpscr;")
        elif xo == 711:  # mtfsf
            fm = (w >> 17) & 0xFF
            mask = 0
            for i in range(8):
                if fm & (0x80 >> i):
                    mask |= 0xF << (28 - 4 * i)
            E(pc, f"fpscr = (fpscr & {hx(~mask)}) | (u32({self.rf(fb)}) & {hx(mask)});")
        elif xo == 38:
            E(pc, f"fpscr |= {hx(1 << (31 - fd))};")
        elif xo == 70:
            E(pc, f"fpscr &= {hx(~(1 << (31 - fd)))};")
        elif xo == 134:  # mtfsfi
            field, value = (w >> 23) & 7, (w >> 12) & 0xF
            shift = 28 - 4 * field
            E(pc, f"fpscr = (fpscr & {hx(~(0xF << shift))}) | {hx(value << shift)};")
        elif xo == 64:  # mcrfs
            self.uses_cr = True
            E(pc, f"set_crf(cr, {(w >> 23) & 7}, (fpscr >> {28 - 4 * ((w >> 18) & 7)}) & 0xFu);")
            return
        else:
            raise Refuse(f"op63_{xo}")
        self.cr1(pc, w)

    def x19(self, pc, w):
        xo = bits(w, 21, 30)
        E = self.emit
        crops = {257: "a & b", 129: "a & ~b", 289: "~(a ^ b)", 225: "~(a & b)", 33: "~(a | b)", 449: "a | b",
                 417: "a | ~b", 193: "a ^ b"}
        if xo in crops:
            bt, ba, bb = bits(w, 6, 10), bits(w, 11, 15), bits(w, 16, 20)
            self.uses_cr = True
            E(pc, f"{{ const u32 a = (cr >> {31 - ba}) & 1u, b = (cr >> {31 - bb}) & 1u; const u32 v = ({crops[xo]}) & 1u; "
                  f"cr = (cr & ~(1u << {31 - bt})) | (v << {31 - bt}); }}")
            return
        if xo == 0:  # mcrf
            d, s = bits(w, 6, 8), bits(w, 11, 13)
            self.uses_cr = True
            E(pc, f"set_crf(cr, {d}, (cr >> {(7 - s) * 4}) & 0xFu);")
            return
        if xo == 150:
            E(pc, "/* isync */")
            return
        if xo == 16:
            if w & 1:  # blrl: call through LR
                if not (bits(w, 6, 10) & 4):
                    raise Refuse("ctr_decrement_with_link")
                cond = self.condition(w)
                self.uses_lr = True
                self.call(pc, cond, "lr_target")
                return
            cond = self.condition(w)
            if cond == "true":
                E(pc, "goto done;")
            else:
                E(pc, f"if ({cond}) goto done;")
            return
        if xo == 528:  # bcctr / bcctrl
            bo = bits(w, 6, 10)
            if not (bo & 4):
                raise Refuse("bcctr_decrement")  # invalid form
            cond = self.condition(w)
            self.uses_ctr = True
            if w & 1:
                self.call(pc, cond, "ctr_target")
            else:
                self.indirect_jump(pc, cond)
            return
        raise Refuse(f"x19_{xo}")

    # ---- calls ---------------------------------------------------------------------------------------------
    def call(self, pc, cond, target):
        """target: an address (direct), "ctr_target" or "lr_target". The callee sees the whole state: every local is
        spilled to s before and reloaded after. A callee's fault (no lifted target, budget, return elsewhere)
        unwinds every caller."""
        self.calls = True
        self.uses_lr = True
        if isinstance(target, int):
            self.call_sites.append((target, {r: self.origin_of(r) for r in range(3, 11)}))
        # A call clobbers the volatile registers (r0, r3-r12); the EABI preserves r1, r2, r13 and r14-r31, so their
        # pointer origin survives (found with GPT-6-Astra, 2026-10-31: fn_800DF838 keeps its r4 pointer in r31 across
        # a call and hands it to a copy loop - treated as an integer, the test fed it 0x80000000).
        clobber = {n: None for n in (0, *range(3, 13))}
        if isinstance(target, int):
            self.callees.add(target)
            invoke = f"@@CALL:{target:08X}@@"
        elif target == "ctr_target":
            invoke = "dispatch(m, s, s.ctr);"
        else:
            invoke = "{ const u32 t = s.lr; s.lr = " + hx(pc + 4) + "; dispatch(m, s, t); }"
        body = ["@@SPILL@@"]
        if target != "lr_target":
            body.append(f"s.lr = {hx(pc + 4)};")
        body += [invoke, "if (s.fault) return;", "@@RELOAD@@"]
        self.origin.update(clobber)
        if cond != "true":
            self.emit(pc, f"if ({cond}) {{")
            for b in body:
                self.emit(pc, "    " + b)
            self.emit(pc, "}")
        else:
            for b in body:
                self.emit(pc, b)

    def tail_call(self, pc, cond, target):
        """b / bc to another function's start: the callee returns straight to our caller."""
        self.callees.add(target)
        self.call_sites.append((target, {r: self.origin_of(r) for r in range(3, 11)}))
        body = ["@@SPILL@@", f"@@CALL:{target:08X}@@", "return;"]
        if cond != "true":
            self.emit(pc, f"if ({cond}) {{")
            for b in body:
                self.emit(pc, "    " + b)
            self.emit(pc, "}")
        else:
            for b in body:
                self.emit(pc, b)

    def indirect_jump(self, pc, cond):
        """bctr: a switch inside this function (cases = every in-function address a data word points at), else a
        tail jump to whatever function CTR names."""
        cases = sorted(t for t in self.ctx["jt_targets"] if self.start <= t < self.start + self.size)
        lines = []
        if cases:
            lines.append("switch (ctr) {")
            for t in cases:
                self.labels.add(t)
                lines.append(f"case {hx(t)}: goto L_{t:08X};")
            lines.append("default: break;")
            lines.append("}")
        lines += ["@@SPILL@@", "dispatch(m, s, s.ctr);", "return;"]
        if cond != "true":
            self.emit(pc, f"if ({cond}) {{")
            for b in lines:
                self.emit(pc, "    " + b)
            self.emit(pc, "}")
        else:
            for b in lines:
                self.emit(pc, b)

    def condition(self, w):
        bo, bi = bits(w, 6, 10), bits(w, 11, 15)
        parts = []
        if not (bo & 4):
            self.uses_ctr = True
            self.ctr_loop = True
            self.emit(None, "--ctr;")  # placed by the caller before the test (see branch())
            parts.append("ctr != 0u" if not (bo & 2) else "ctr == 0u")
        if not (bo & 16):
            self.uses_cr = True
            parts.append(f"((cr >> {31 - bi}) & 1u) == {1 if bo & 8 else 0}u")
        return " && ".join(parts) if parts else "true"

    def branch(self, pc, w):
        op = w >> 26
        kind, target, link = auditmod.decode_flow(w, pc)
        if (w >> 1) & 1:
            raise Refuse("absolute_branch")
        if op == 16 and not (bits(w, 6, 10) & 4) and link:
            raise Refuse("ctr_decrement_with_link")
        if link:
            if op == 16 and target == pc + 4 and bits(w, 6, 10) == 20:  # bcl 20,31,$+4: read the PC into LR
                self.uses_lr = True
                self.emit(pc, f"lr = {hx(pc + 4)};")
                return
            cond = "true" if op == 18 else self.condition(w)
            if target not in self.ctx["entries"]:
                raise Refuse("call_to_non_entry")
            self.call(pc, cond, target)
            return
        inside = self.start <= target < self.start + self.size
        if not inside:
            if target not in self.ctx["entries"]:
                raise Refuse("branch_outside_to_non_entry")
            cond = "true" if op == 18 else self.condition(w)
            self.tail_call(pc, cond, target)
            return
        self.labels.add(target)
        budget = "if (--s.budget <= 0) { s.fault = kFaultBudget; return; } " if target <= pc else ""
        if op == 18:
            self.emit(pc, f"{budget}goto L_{target:08X};")
            return
        cond = self.condition(w)
        if cond == "true":
            self.emit(pc, f"{budget}goto L_{target:08X};")
        elif budget:
            self.emit(pc, f"if ({cond}) {{ {budget}goto L_{target:08X}; }}")
        else:
            self.emit(pc, f"if ({cond}) goto L_{target:08X};")

    # ---- whole function ------------------------------------------------------------------------------------
    def lift(self):
        for pc in range(self.start, self.start + self.size, 4):
            self.lift_one(pc, self.image.word(pc))
        last = self.image.word(self.start + self.size - 4)
        kind, _, link = auditmod.decode_flow(last, self.start + self.size - 4)
        if kind not in ("b", "blr", "bctr") or link:
            raise Refuse("falls_through_end")
        return self

    def render(self, asm_lines):
        gin = sorted(self.gpr_in)
        gall = sorted(self.gpr_read | self.gpr_written)
        fall = sorted(self.fpr_in | self.fpr_written)
        self.uses_lr = True  # every function checks its return against its entry LR
        out = [f"// {self.name} @ 0x{self.start:08X}, {self.size // 4} instructions"
               f"{' (calls)' if self.calls else ' (leaf)'}; lifted, verified by dolrecon_lift_tests",
               f"void lift_{self.start:08X}(Mem& m, State& s) {{",
               "    if (--s.budget <= 0) { s.fault = kFaultBudget; return; }",
               f"    trace_enter(s, {hx(self.start)});",
               "    const u32 entry_lr = s.lr;"]
        gall = [n for n in gall if n not in self.sda or n in self.gpr_written]
        decl = [f"u32 r{n} = s.r[{n}];" for n in gall] + [f"u64 f{n} = s.f[{n}];" for n in fall]
        pall = sorted(self.ps1_written | self.ps1_read)
        decl += [f"u64 p{n} = s.p[{n}];" for n in pall]
        regs = [n for n in ("cr", "xer", "ctr", "lr", "fpscr") if getattr(self, "uses_" + n)]
        decl += [f"u32 {n} = s.{n};" for n in regs]
        for d in decl:
            out.append("    " + d)
        spill = [f"s.r[{n}] = r{n};" for n in gall] + [f"s.f[{n}] = f{n};" for n in fall] + \
                [f"s.p[{n}] = p{n};" for n in pall] + [f"s.{n} = {n};" for n in regs]
        reload = [f"r{n} = s.r[{n}];" for n in gall] + [f"f{n} = s.f[{n}];" for n in fall] + \
                 [f"p{n} = s.p[{n}];" for n in pall] + [f"{n} = s.{n};" for n in regs]
        resolve = self.ctx.get("resolve", lambda t: None)
        expanded = []
        for pc, text in self.stmts:
            indent = text[:len(text) - len(text.lstrip())]
            core = text.strip()
            if core == "@@SPILL@@":
                expanded += [(pc, indent + x) for x in spill]
            elif core == "@@RELOAD@@":
                expanded += [(pc, indent + x) for x in reload]
            elif core.startswith("@@CALL:"):
                t = int(core[7:15], 16)
                fn = resolve(t)
                expanded.append((pc, indent + (f"lift_{fn:08X}(m, s);" if fn is not None
                                               else f"dispatch(m, s, {hx(t)});  // callee not lifted")))
            else:
                expanded.append((pc, text))
        self.stmts = expanded
        for pc, text in self.stmts:
            if pc is not None and pc in self.labels:
                out.append(f"L_{pc:08X}:")
                self.labels.discard(pc)
            comment = f"  // {asm_lines.get(pc, '')}" if pc is not None and asm_lines.get(pc) else ""
            out.append(f"    {text}{comment}")
        out.append("done:;")
        out.append("    if (lr != entry_lr) { s.fault = kFaultReturnElsewhere; s.fault_address = lr; }")
        for n in sorted(self.gpr_written):
            out.append(f"    s.r[{n}] = r{n};")
        for n in sorted(self.fpr_written):
            out.append(f"    s.f[{n}] = f{n};")
        for n in sorted(self.ps1_written):
            out.append(f"    s.p[{n}] = p{n};")
        for n in regs:
            out.append(f"    s.{n} = {n};")
        out.append("}")
        return "\n".join(out)


def fixup_condition_order(fn: Fn):
    """condition() emits '--ctr;' with pc None just before the branch statement; move it to the branch's pc so a label
    on the branch lands before the decrement."""
    stmts = fn.stmts
    for i in range(len(stmts) - 1):
        if stmts[i][0] is None and stmts[i][1] == "--ctr;":
            stmts[i] = (stmts[i + 1][0], "--ctr;")
            stmts[i + 1] = (None, stmts[i + 1][1])
    return fn


def load_asm_text(project):
    from . import callgraph
    lines = {}
    line_re = re.compile(r"^/\* ([0-9A-F]{8}) [0-9A-F]{8}  (?:[0-9A-F]{2} ){4}\*/\t(.*)$")
    asm_dir = os.path.join(project, "out", "asm")
    for name in os.listdir(asm_dir):
        if callgraph.TEXT_FILE.search(name):  # incl. duplicate units (_text_N.s) and __init_cpp_exceptions.s
            with open(os.path.join(asm_dir, name), encoding="utf-8") as f:
                for line in f:
                    m = line_re.match(line.rstrip("\n"))
                    if m:
                        lines[int(m.group(1), 16)] = m.group(2).replace("*/", "* /")
    return lines


def jump_table_targets(project, image) -> set:
    """Every code address an ADDR32 relocation in DATA points at: switch-table cases and callbacks (from the
    corrected project's objects, so it is dtk's relocation analysis checked by the byte-exact rebuild)."""
    from . import rebuild
    blocks = rebuild.plan(os.path.join(project, "out", "asm"))
    text = [s for s in image.sections if s.kind == "text"]
    out = set()
    for b in blocks:
        if b.section in (".text", ".init") or b.section in rebuild.BSS_SECTIONS:
            continue
        path = os.path.join(project, "build", "obj", b.unit + ".o")
        if not os.path.exists(path):
            continue
        for sec, off, rtype, sym in rebuild._relocs(path):
            if sec != b.section or rtype != 1:
                continue
            v = image.word(b.start + off)
            if any(t.address <= v < t.address_end for t in text):
                out.add(v)
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("project")
    ap.add_argument("out_dir")
    ap.add_argument("--shards", type=int, default=16)
    args = ap.parse_args(argv)
    image = dolmod.load_file(os.path.join(args.project, "main.dol"))
    syms = auditmod.load_symbols(os.path.join(args.project, "config", "symbols.txt"))
    symbols_at = {}
    for kind in ("object", "label", "function"):
        for a, s_, n, _ in syms[kind]:
            symbols_at.setdefault(a, n)
    asm = load_asm_text(args.project)
    # Callable entries: every function, and every labelled second entry inside one (e.g. _savegpr_25), lifted from
    # the label to the end of its function.
    funcs = [(a, s_, n) for a, s_, n, _ in syms["function"] if s_]
    starts = sorted(a for a, _, _ in funcs)
    size_of = {a: s_ for a, s_, _ in funcs}
    import bisect
    units = list(funcs)
    for a, _, n, sec in syms["label"]:
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and starts[i] < a < starts[i] + size_of[starts[i]]:
            units.append((a, starts[i] + size_of[starts[i]] - a, n))
    entries = {a: n for a, _, n in units}
    ctx = {"entries": entries, "jt_targets": jump_table_targets(args.project, image)}
    refused = collections.Counter()
    refused_at = {}
    lifted = []
    for a, size, name in sorted(units):
        fn = Fn(image, a, size, name, symbols_at, ctx)
        try:
            fn.lift()
        except Refuse as e:
            refused[str(e)] += 1
            refused_at[a] = str(e)
            continue
        fixup_condition_order(fn)
        lifted.append(fn)
    lifted_set = {fn.start for fn in lifted}
    ctx["resolve"] = lambda t: t if t in lifted_set else None
    # Interprocedural pointer inputs (test inputs only): a caller's input register that reaches a callee's pointer
    # parameter unchanged is a pointer too. Iterated to a fixpoint.
    by_start = {fn.start: fn for fn in lifted}
    for _ in range(12):
        changed = False
        for fn in lifted:
            for target, origins in fn.call_sites:
                callee = by_start.get(target)
                if callee is None:
                    continue
                for r in range(3, 11):
                    o = origins.get(r)
                    if r in callee.base_regs and o is not None and o not in fn.base_regs and o not in (2, 13):
                        fn.base_regs.add(o)
                        changed = True
        if not changed:
            break
    # How many lifted functions have every direct callee lifted (indirect calls are resolved at run time).
    complete = sum(1 for fn in lifted if all(c in lifted_set for c in fn.callees))
    os.makedirs(args.out_dir, exist_ok=True)
    for name in os.listdir(args.out_dir):
        if name.startswith("dolrecon_lifted_") and (name.endswith(".cpp") or name.endswith(".h")):
            os.remove(os.path.join(args.out_dir, name))
    # The lifted functions run on their own Mem / State in dolrecon_lift_tests, which links the reference interpreter
    # and never the recompiled shards: the guest addresses they name are not entries the shipping build's native code
    # makes, so they must not become liveness roots of the generator (tools/ppcrecomp/liveness_roots.py verifies the
    # declaration: tests/ only, no generated-code include).
    head = ("// GENERATED by tools/dolrecon/lift_cpp.py from the corrected PAL project - do not edit; regenerate.\n"
            f"// input main.dol SHA-256 {image.sha256}\n"
            "// liveness-roots: reference-interpreter-only\n")
    with open(os.path.join(args.out_dir, "dolrecon_lifted_decls.h"), "w", newline="\n") as f:
        f.write(head + '#pragma once\n#include "dolrecon_lift.h"\n\nnamespace dolrecon {\n')
        for fn in lifted:
            f.write(f"void lift_{fn.start:08X}(Mem& m, State& s);\n")
        f.write("}  // namespace dolrecon\n")
    per = (len(lifted) + args.shards - 1) // args.shards
    for k in range(args.shards):
        chunk = lifted[k * per:(k + 1) * per]
        with open(os.path.join(args.out_dir, f"dolrecon_lifted_{k:02d}.cpp"), "w", newline="\n") as f:
            f.write(head + '#include "dolrecon_lifted_decls.h"\n\nnamespace dolrecon {\n\n')
            f.write("#if defined(_MSC_VER)\n#pragma warning(disable : 4102 4100 4319)  // unused labels / parameters, "
                    "deliberate 32-bit complements\n#endif\n\n")
            for fn in chunk:
                f.write(fn.render(asm) + "\n\n")
            f.write("}  // namespace dolrecon\n")

    def mask(regs):
        return f"0x{sum(1 << r for r in regs):08X}u"

    with open(os.path.join(args.out_dir, "dolrecon_lifted_table.cpp"), "w", newline="\n") as f:
        f.write(head + '#include "dolrecon_lifted_decls.h"\n\nnamespace dolrecon {\n\nconst Lifted kLifted[] = {\n')
        for fn in lifted:
            f.write(f"    {{0x{fn.start:08X}u, \"{fn.name}\", lift_{fn.start:08X}, {mask(fn.gpr_in)}, "
                    f"{mask(fn.gpr_written)}, {mask(fn.fpr_in)}, {mask(fn.fpr_written)}, {mask(fn.ps1_written)}, "
                    f"{mask(fn.base_regs)}, {mask(fn.index_regs)}, {1 if fn.ctr_loop else 0}u, {fn.size // 4}u, "
                    f"{1 if fn.calls else 0}u}},\n")
        f.write(f"}};\nconst std::size_t kLiftedCount = {len(lifted)};\n\n")
        f.write("// Indirect transfers (bctrl, bctr out of a function, blrl, calls to callees that were not lifted): the\n"
                "// lifted function at exactly that address, else a recorded fault - never a guess.\n"
                "void dispatch(Mem& m, State& s, u32 target) {\n"
                "    std::size_t lo = 0, hi = kLiftedCount;\n"
                "    while (lo < hi) {\n"
                "        const std::size_t mid = (lo + hi) / 2;\n"
                "        if (kLifted[mid].address < target) lo = mid + 1; else hi = mid;\n"
                "    }\n"
                "    if (lo < kLiftedCount && kLifted[lo].address == target) { kLifted[lo].fn(m, s); return; }\n"
                "    s.fault = kFaultNoTarget;\n"
                "    s.fault_address = target;\n"
                "}\n\n}  // namespace dolrecon\n")
    total = len(units)
    report = {"input_sha256": image.sha256, "functions": len(funcs), "entries_including_labels": total,
              "lifted": len(lifted), "lifted_instructions": sum(fn.size // 4 for fn in lifted),
              "lifted_with_calls": sum(1 for fn in lifted if fn.calls),
              "lifted_with_every_direct_callee_lifted": complete,
              "refused_by_reason": dict(refused.most_common()),
              "refused": {f"{a:08X}": r for a, r in sorted(refused_at.items())}}
    with open(os.path.join(args.out_dir, "dolrecon_lift_report.json"), "w") as f:
        json.dump(report, f, indent=1)
    print(json.dumps({k: v for k, v in report.items() if k not in ("refused_by_reason", "refused")}, indent=1))
    print("top refusals:", refused.most_common(15))


if __name__ == "__main__":
    main(sys.argv[1:])
