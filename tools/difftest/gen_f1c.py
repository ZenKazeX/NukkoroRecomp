"""F1c fixture generator: host-stack integrity shapes and long-run canary programs (test-only).

Writes a synthetic DOL whose rows are guest loops closed through every control-transfer shape
tools/ppcrecomp/main.cpp emits, plus the long-run canary programs, and runs the REAL ppcrecomp
with the production arguments (CMakeLists.txt SHADOW_RECOMP_ARGS: --dol --functions --out --shards
--report; the gap sweep stays ON, no --no-gap-sweep / --no-wide-entries).

Outputs (into --out, deleted and recreated):
  f1c.dol, functions.csv, recomp_000..N.cpp, recomp_table.cpp,
  recomp_decls.h, recomp_layout.csv, recomp_report.txt, ppcrecomp.log
  f1c_fixture.h     code rows, shape table and canary program table for the C++ drivers
  f1c_fixture.json  the same metadata for run_f1c_stack.py / census_f1c.py / run_f1c_canary.py

Shapes (every one is an O(1)-state guest loop; r3 counts iterations up to r5, r6 is the data table):
  intra        loop closed by an in-record back edge (goto + poll)                    control
  xrec_direct  record A `b` forward into record B, B `blt` back into A's loop head (cross-record direct
               back edge: poll + `fn_A_eHEAD(c); return;`)
  eof_fall     record A runs off its end into record B (EndOfFunction `fn_B(c); return;`), B branches back
  bctr_local   bctr whose target is a hot label of its own record (switch goto + poll)  control
  bctr_default cycle C -> D -> C made only of bctr to other records' declared entries
               (`recomp::jump_indirect(c, tmp); return;`, no poll)
  resume_else  E `bl F`; F `blr` with LR = E's entry, so E sees pc != link (resume_elsewhere)
  bclr_cond    as resume_else, F returns with a taken conditional bclr (beqlr)
  bclrl        as resume_else, F leaves through bclrl (LR = E's entry, links to F)
  bclr_top     top-level function returning to its own entry with blr (hands to Cpu::run)  control
  hle_xrec     A `bl H` (H served by an HLE hook), B `blt` back into A (the fn_803D946C shape)
  bl_xrec      A `bl L` (translated leaf), B `blt` back into A
  xrec_ring    eight records, one lap crossing every LK=0 transfer kind to another record in order: forward
               `b`, end-of-record fall-through, `bctr`, a `bl` whose callee returns elsewhere
               (resume_elsewhere), `b`, `bctr`, and the backward cross-record `blt` (R12, 2026-09-28)

Each shape has a tiny variant (`_t`) and a game-shaped variant (`_g`: stack frame, saved LR/non-volatiles,
guest memory traffic and an indirect call per iteration) because MSVC tail-jumps tiny bodies and a tiny
fixture alone passes vacuously (verify-r2 host-stack-cross-record). census_f1c.py reports which transfer
sites compiled to CALL + epilogue + RET in the linked driver.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ppcenc as E            # noqa: E402
import ppcenc_ext as X        # noqa: E402
import emitter_classes as EC  # noqa: E402

SCHEMA = "gekko-f1c-fixture/1"
M32 = 0xFFFFFFFF
SENTINEL = 0x80009000         # untranslated: the production dispatcher stops there
CODE_BASE = 0x80010000
DATA_BASE = 0x80300000        # r6 table / canary data
STACK_TOP = 0x80200000        # guest r1
LEAF = 0x80010000             # translated leaf: addi r7,r7,1; blr
HLE_CALLEE = 0x80010040       # translated leaf, HLE-served in the hle shapes
SHAPE_BASE = 0x80010100
SHAPE_SLOT = 0x400
CANARY_BASE = 0x80030000
FILLER_BASE = 0x80400000       # never executed (above the data window and the guest stack)
FILLER_SLOT = 0x180
OS_CONTEXT = 0x80250000       # the current OSContext the native OS exception entry saves into (low memory 0xD4)


def lwz(d, a, off):
    return E.d_form(32, d, a, off)


def stw(s, a, off):
    return E.d_form(36, s, a, off)


def stwu(s, a, off):
    return E.d_form(37, s, a, off)


def mr(a, s):
    return E.x_form(31, s, a, s, 444)       # or rA,rS,rS


def cmplwi(crf, a, imm):
    return E.cmp_imm(10, crf, a, imm)


def li(d, imm):
    return E.addi(d, 0, imm)


class Asm:
    """Minimal two-pass assembler: words or callables (site, labels) -> word; `cut` starts a new row."""

    def __init__(self, base: int):
        self.base = base
        self.ops = []
        self.labels = {}
        self.cuts = [0]

    @property
    def pc(self) -> int:
        return self.base + 4 * len(self.ops)

    def L(self, name: str):
        assert name not in self.labels, name
        self.labels[name] = self.pc

    def cut(self):
        if self.cuts[-1] != len(self.ops):
            self.cuts.append(len(self.ops))

    def w(self, *words):
        for word in words:
            self.ops.append(word)

    def br(self, kind: str, label: str):
        conds = {"blt": (12, 0), "bge": (4, 0), "beq": (12, 2), "bne": (4, 2)}
        if kind == "b":
            self.ops.append(lambda site, labels: E.b(site, labels[label]))
        elif kind == "bl_abs":
            self.ops.append(lambda site, labels: E.b(site, int(label), 1))
        elif kind == "bl":
            self.ops.append(lambda site, labels: E.b(site, labels[label], 1))
        else:
            bo, bi = conds[kind]
            self.ops.append(lambda site, labels: E.bc(bo, bi, site, labels[label]))

    def br_bdnz(self, label: str):
        self.ops.append(lambda site, labels: E.bc(16, 0, site, labels[label]))

    def words(self) -> list:
        out = []
        for i, op in enumerate(self.ops):
            out.append(op(self.base + 4 * i, self.labels) if callable(op) else op)
        return out

    def rows(self, name: str) -> list:
        words = self.words()
        cuts = self.cuts + [len(words)]
        rows = []
        for k, (a, b) in enumerate(zip(cuts, cuts[1:])):
            rows.append({"address": self.base + 4 * a, "words": words[a:b], "name": f"{name}_{k}"})
        return rows


# ---- shape building blocks -----------------------------------------------------------------------
# style: "t" tiny, "g" game frame (the fn_803D946C prologue and loop body), "b" game frame plus a ~100
# instruction body executed on every iteration (loads/stores, compares and forward branches, a bdnz loop, bl
# and bctrl to the translated leaf).
def prologue(a: Asm):
    """A realistic non-leaf frame (the fn_803D946C prologue): stwu, save LR and three non-volatiles."""
    a.w(stwu(1, 1, -0x30), E.MFLR(0), stw(0, 1, 0x34), stw(31, 1, 0x2C), stw(30, 1, 0x28), stw(29, 1, 0x24),
        mr(29, 6), li(30, 0))


def epilogue(a: Asm):
    a.w(lwz(0, 1, 0x34), E.MTLR(0), lwz(31, 1, 0x2C), lwz(30, 1, 0x28), lwz(29, 1, 0x24), E.addi(1, 1, 0x30),
        E.BLR)


def game_traffic(a: Asm):
    """Guest memory traffic and an indirect call per iteration (the fn_803D946C loop body)."""
    a.w(lwz(12, 6, 0x00), E.addi(30, 30, 1), stw(30, 1, 0x10), lwz(0, 1, 0x10), stw(0, 6, 0x40),
        E.MTCTR(12), E.bcctr(20, 0, 1))


def big_traffic(a: Asm, seed: int, size: int = 100):
    """Executed on every iteration; touches only r0, r8-r12, LR, CTR, CR and the data window r6+0x40..0x7F."""
    import random
    rng = random.Random(seed)
    start = len(a.ops)
    n = 0
    while len(a.ops) - start < size:
        pick = rng.randrange(7)
        off = rng.randrange(0x40, 0x80, 4)
        n += 1
        if pick == 0:
            a.w(lwz(9, 6, off), E.addi(9, 9, rng.randrange(1, 100)), stw(9, 6, off))
        elif pick == 1:
            a.w(lwz(9, 6, off), X.rlwinm(9, 9, rng.randrange(32), 0, 31), cmplwi(0, 9, rng.randrange(256)))
            a.br(rng.choice(("beq", "bne", "blt", "bge")), f"bt{seed}_{n}")
            a.w(stw(9, 6, off), E.addi(10, 10, 1))
            a.L(f"bt{seed}_{n}")
        elif pick == 2:
            a.br("bl_abs", str(LEAF))
        elif pick == 3:
            a.w(lwz(12, 6, 0), E.MTCTR(12), E.bcctr(20, 0, 1))
        elif pick == 4:
            a.w(li(0, rng.randrange(2, 5)), E.MTCTR(0))
            a.L(f"bl{seed}_{n}")
            a.w(lwz(9, 6, off), E.addi(9, 9, 1), stw(9, 6, off))
            a.br_bdnz(f"bl{seed}_{n}")
        elif pick == 5:
            a.w(E.xo_form(9, 10, 11, 266), E.xo_form(10, 9, 12, 40), E.cmplw(0, 9, 10))
            a.br("blt", f"bg{seed}_{n}")
            a.w(E.x_form(31, 9, 9, 10, 316))      # xor r9,r9,r10
            a.L(f"bg{seed}_{n}")
        else:
            a.w(E.ori(8, 10, rng.randrange(0x10000)), E.x_form(31, 8, 9, 11, 28), stw(9, 6, off))


def traffic(a: Asm, style: str, seed: int):
    if style == "g":
        game_traffic(a)
    elif style == "b":
        game_traffic(a)
        big_traffic(a, seed)


def exit_to_sentinel(a: Asm):
    a.w(lwz(0, 6, 16), E.MTLR(0), E.BLR)


def shape_intra(a: Asm, style: str, seed: int):
    if style != "t":
        prologue(a)
    a.w(li(3, 0))
    a.L("head")
    a.w(E.addi(3, 3, 1))
    traffic(a, style, seed)
    a.w(E.cmplw(0, 3, 5))
    a.br("blt", "head")
    if style != "t":
        epilogue(a)
    else:
        a.w(E.BLR)


def shape_xrec_direct(a: Asm, style: str, seed: int):
    if style != "t":
        prologue(a)
    a.w(li(3, 0))
    a.L("head")
    a.w(E.addi(3, 3, 1))
    traffic(a, style, seed)
    a.br("b", "B")
    a.cut()
    a.L("B")
    a.w(E.cmplw(0, 3, 5))
    a.br("blt", "head")
    if style != "t":
        epilogue(a)
    else:
        a.w(E.BLR)


def shape_eof_fall(a: Asm, style: str, seed: int):
    if style != "t":
        prologue(a)
    a.w(li(3, 0))
    a.L("head")
    a.w(E.addi(3, 3, 1))
    if style == "t":
        a.w(E.addi(4, 4, 1))
    traffic(a, style, seed)
    a.cut()                          # the record ends on a non-branch: EndOfFunction fall-through
    a.w(E.cmplw(0, 3, 5))
    a.br("blt", "head")
    if style != "t":
        epilogue(a)
    else:
        a.w(E.BLR)


def shape_bl_xrec(a: Asm, style: str, seed: int, callee: int):
    if style != "t":
        prologue(a)
    else:
        a.w(E.MFLR(31))
    a.w(li(3, 0))
    a.L("head")
    a.w(E.addi(3, 3, 1))
    if style == "b":
        big_traffic(a, seed)
    if style != "t":
        a.w(lwz(0, 29, 0x00), stw(0, 1, 0x10), lwz(4, 1, 0x10), stw(3, 29, 0x40))
    a.br("bl_abs", str(callee))
    a.cut()                          # functions.csv starts a record after bl (as in the game)
    a.w(E.cmplw(0, 3, 5))
    a.br("blt", "head")
    if style != "t":
        epilogue(a)
    else:
        a.w(E.MTLR(31), E.BLR)


def shape_bctr_local(a: Asm, style: str, seed: int):
    if style != "t":
        prologue(a)
    a.w(li(3, 0))
    a.w(cmplwi(0, 5, 0))
    a.br("bne", "head")             # makes `head` a hot label (a direct branch target)
    a.br("b", "done")
    a.L("head")
    a.w(E.addi(3, 3, 1))
    traffic(a, style, seed)
    a.w(E.cmplw(0, 3, 5))
    a.br("bge", "done")
    a.w(lwz(12, 6, 20), E.MTCTR(12), E.bcctr(20, 0))   # CTR = head (table +20): backward local bctr
    a.L("done")
    if style != "t":
        epilogue(a)
    else:
        a.w(E.BLR)


def shape_bctr_default(a: Asm, style: str, seed: int):
    # C (row 0): count, exit test, bctr to D.   D (row 1): bctr back to C's entry.
    a.L("C")
    if style != "t":
        a.w(stwu(1, 1, -0x20), E.MFLR(0), stw(0, 1, 0x24), stw(31, 1, 0x1C))
        a.w(lwz(12, 6, 0), E.MTCTR(12), E.bcctr(20, 0, 1))          # indirect call to the leaf
        if style == "b":
            big_traffic(a, seed)
        a.w(lwz(0, 1, 0x24), E.MTLR(0), lwz(31, 1, 0x1C), E.addi(1, 1, 0x20))
    a.w(E.addi(3, 3, 1), E.cmplw(0, 3, 5))
    a.br("bge", "exit")
    a.w(lwz(12, 6, 4), E.MTCTR(12), E.bcctr(20, 0))
    a.L("exit")
    exit_to_sentinel(a)
    a.cut()
    a.L("D")
    if style != "t":
        a.w(stwu(1, 1, -0x20), stw(3, 1, 0x10), lwz(4, 1, 0x10), stw(4, 6, 0x40), E.addi(1, 1, 0x20))
        if style == "b":
            big_traffic(a, seed + 1)
    a.w(E.addi(4, 4, 1), lwz(12, 6, 8), E.MTCTR(12), E.bcctr(20, 0))


def shape_resume(a: Asm, style: str, seed: int, leave: str):
    # E (row 0): count, exit test, bl F.   F (row 1): return to E's entry through `leave`.
    a.L("E")
    if style != "t":
        a.w(stwu(1, 1, -0x20), stw(31, 1, 0x1C))
        a.w(lwz(12, 6, 0), E.MTCTR(12), E.bcctr(20, 0, 1))
        if style == "b":
            big_traffic(a, seed)
        a.w(lwz(31, 1, 0x1C), E.addi(1, 1, 0x20))
    a.w(E.addi(3, 3, 1), E.cmplw(0, 3, 5))
    a.br("bge", "exit")
    a.br("bl", "F")
    a.w(E.BLR)                       # link address: never reached
    a.L("exit")
    exit_to_sentinel(a)
    a.cut()
    a.L("F")
    if style != "t":
        a.w(stwu(1, 1, -0x20), stw(3, 1, 0x10), lwz(4, 1, 0x10), stw(4, 6, 0x40), E.addi(1, 1, 0x20))
        if style == "b":
            big_traffic(a, seed + 1)
    a.w(E.addi(4, 4, 1), lwz(0, 6, 12), E.MTLR(0))
    if leave == "blr":
        a.w(E.BLR)
    elif leave == "beqlr":
        a.w(E.cmplw(0, 4, 4), E.bclr(12, 2), E.BLR)
    elif leave == "blrl":
        a.w(E.bclr(20, 0, 1), E.BLR)
    else:
        raise ValueError(leave)


def shape_resume_ind(a: Asm, style: str, seed: int):
    # E (row 0): [frame] count, exit test, [traffic], bctrl F.   F (row 1): [pop E's frame] LR <- E, blr.
    # The call site is recomp::call_indirect + `if (c.pc != link) { recomp::resume_elsewhere(...); return; }`.
    a.L("E")
    if style != "t":
        a.w(stwu(1, 1, -0x30), E.MFLR(0), stw(0, 1, 0x34), stw(31, 1, 0x2C))
    a.w(E.addi(3, 3, 1), E.cmplw(0, 3, 5))
    a.br("bge", "exit")
    if style == "g":
        a.w(stw(3, 1, 0x10), lwz(0, 1, 0x10), stw(0, 6, 0x40))
    elif style == "b":
        big_traffic(a, seed)
    a.w(lwz(12, 6, 24), E.MTCTR(12), E.bcctr(20, 0, 1))
    a.br("b", "exit")                # link address: never reached
    a.L("exit")
    if style != "t":
        a.w(E.addi(1, 1, 0x30))
    exit_to_sentinel(a)
    a.cut()
    a.L("F")
    a.w(E.addi(4, 4, 1))
    if style != "t":
        a.w(E.addi(1, 1, 0x30))      # unwind E's frame: the guest resumes E from its entry (a longjmp)
    a.w(lwz(0, 6, 12), E.MTLR(0), E.BLR)


def load_label_address(a: Asm, reg: int, label: str):
    """lis/ori of a label's address (resolved when the words are assembled)."""
    a.ops.append(lambda site, labels: E.load_address(reg, labels[label])[0])
    a.ops.append(lambda site, labels: E.load_address(reg, labels[label])[1])


def shape_xrec_ring(a: Asm, style: str, seed: int):
    # R0 -b-> R1 -fall-> R2 -bctr-> R3 -bl R4; R4 blr to R5 (R3: resume_elsewhere)-> R5 -b-> R6 -bctr-> R7 -blt-> R0.
    # Seven LK=0 transfers to other records per lap; before R12 each was a host call whose frame depended on
    # MSVC's tail-call choice. Targets are lis/ori constants, so the fixed eight-word data table is untouched.
    if style != "t":
        prologue(a)
    else:
        a.w(E.MFLR(31))
    a.w(li(3, 0))
    a.L("head")
    a.w(E.addi(3, 3, 1))
    traffic(a, style, seed)
    a.br("b", "R1")
    a.cut()
    a.L("R1")
    a.w(E.addi(4, 4, 1))
    a.cut()                          # R1 ends on a non-branch: EndOfFunction fall-through into R2
    a.L("R2")
    a.w(E.addi(4, 4, 1))
    load_label_address(a, 12, "R3")
    a.w(E.MTCTR(12), E.bcctr(20, 0))
    a.cut()
    a.L("R3")
    a.w(E.addi(4, 4, 1))
    a.br("bl", "R4")
    a.w(E.BLR)                       # link address: never reached (R4 returns to R5)
    a.cut()
    a.L("R4")
    load_label_address(a, 0, "R5")
    a.w(E.MTLR(0), E.BLR)
    a.cut()
    a.L("R5")
    a.w(E.addi(4, 4, 1))
    a.br("b", "R6")
    a.cut()
    a.L("R6")
    a.w(E.addi(4, 4, 1))
    load_label_address(a, 12, "R7")
    a.w(E.MTCTR(12), E.bcctr(20, 0))
    a.cut()
    a.L("R7")
    a.w(E.cmplw(0, 3, 5))
    a.br("blt", "head")
    if style != "t":
        epilogue(a)
    else:
        a.w(E.MTLR(31), E.BLR)


def shape_bclr_top(a: Asm, style: str, seed: int):
    a.L("G")
    if style != "t":
        a.w(stwu(1, 1, -0x20), stw(31, 1, 0x1C))
        a.w(lwz(12, 6, 0), E.MTCTR(12), E.bcctr(20, 0, 1))
        if style == "b":
            big_traffic(a, seed)
        a.w(lwz(31, 1, 0x1C), E.addi(1, 1, 0x20))
    a.w(E.addi(3, 3, 1), E.cmplw(0, 3, 5))
    a.br("bge", "exit")
    a.w(lwz(0, 6, 20), E.MTLR(0), E.BLR)
    a.L("exit")
    exit_to_sentinel(a)


# (name, transfer kind under test, control: the loop is stack-neutral by construction)
SHAPES = [
    ("intra", "in-record back edge (goto + poll)", True),
    ("xrec_direct", "cross-record direct back edge (poll + fn_A_eHEAD; return) / forward cross-record b", False),
    ("eof_fall", "EndOfFunction fall-through into the next record (fn_B; return)", False),
    ("bctr_local", "bctr to a backward hot label of its own record (switch goto + poll)", True),
    ("bctr_default", "bctr default path recomp::jump_indirect to another record's declared entry", False),
    ("resume_else", "bl callee returns elsewhere (blr to the caller's entry): resume_elsewhere", False),
    ("bclr_cond", "bl callee returns elsewhere through a taken conditional bclr: resume_elsewhere", False),
    ("bclrl", "bl callee leaves through bclrl to the caller's entry: resume_elsewhere", False),
    ("resume_ind", "bctrl (call_indirect) callee returns to the caller's entry: resume_elsewhere", False),
    ("bclr_top", "top-level blr to its own entry (returns to Cpu::run)", True),
    ("hle_xrec", "cross-record loop whose call is HLE-served (bl H), fall-through, back edge", False),
    ("bl_xrec", "cross-record loop through bl to a translated leaf, fall-through, back edge", False),
    ("xrec_ring", "eight-record cycle through every LK=0 transfer kind (b, fall-through, bctr, resume_elsewhere)",
     False),
]
STYLES = ("t", "g", "b")


class Fixture:
    def __init__(self):
        self.rows = {}
        self.shapes = []
        self.canary = []
        self.os_handlers = {}   # the canary's OS exception handlers: name -> address
        self.filler_rows = 0
        self.filler_instructions = 0

    def build_filler(self, instructions: int, seed: int):
        """Game-sized translation units. Never executed: plain non-leaf guest functions (frames, loads and
        stores, compares and forward branches, bdnz loops, bl calls between filler functions, bctrl through a
        loaded pointer) so each generated shard carries a production-like amount of code. A production shard
        holds ~30k guest instructions (build-native/generated: 1,956,418 over 64 shards) and MSVC's inlining
        and tail-call decisions for a function depend on the translation unit it is compiled in: the
        production image calls needs_dispatch and jump_indirect out of line (verify-r2 census), a small
        fixture inlines them."""
        import random
        rng = random.Random(seed)
        per_fn = 96
        count = max(1, instructions // per_fn)
        addresses = [FILLER_BASE + i * FILLER_SLOT for i in range(count)]
        for index, base in enumerate(addresses):
            a = Asm(base)
            a.w(stwu(1, 1, -0x40), E.MFLR(0), stw(0, 1, 0x44), stw(31, 1, 0x3C), stw(30, 1, 0x38),
                stw(29, 1, 0x34), mr(31, 3), mr(30, 4))
            label = 0
            while len(a.ops) < per_fn - 12:
                pick = rng.randrange(8)
                r = rng.choice((0, 5, 6, 7, 8, 9, 10, 11, 12))
                off = rng.randrange(0, 0x40, 4)
                if pick == 0:
                    a.w(lwz(r, 31, off), E.addi(r, r, rng.randrange(1, 100)), stw(r, 30, off))
                elif pick == 1:
                    a.w(lwz(9, 31, off), X.rlwinm(9, 9, rng.randrange(32), 0, 31), cmplwi(0, 9, rng.randrange(256)))
                    name = f"f{label}"
                    label += 1
                    a.br(rng.choice(("beq", "bne", "blt", "bge")), name)
                    a.w(stw(9, 31, off), E.addi(29, 29, 1))
                    a.L(name)
                elif pick == 2 and index + 1 < count:
                    target = addresses[rng.randrange(count)]
                    a.w(mr(3, 31), E.addi(4, 1, 0x10))
                    a.br("bl_abs", str(target))
                elif pick == 3:
                    a.w(lwz(12, 31, off), E.MTCTR(12), E.bcctr(20, 0, 1))
                elif pick == 4:
                    a.w(li(0, rng.randrange(2, 8)), E.MTCTR(0))
                    name = f"l{label}"
                    label += 1
                    a.L(name)
                    a.w(lwz(9, 30, off), E.addi(9, 9, 1), stw(9, 30, off))
                    a.br_bdnz(name)
                elif pick == 5:
                    a.w(E.xo_form(9, 31, 30, 266), E.xo_form(10, 9, 31, 40), E.cmplw(0, 9, 10))
                    name = f"g{label}"
                    label += 1
                    a.br("blt", name)
                    a.w(E.xo_form(9, 9, 10, 235))
                    a.L(name)
                elif pick == 6:
                    a.w(lwz(3, 31, off), cmplwi(0, 3, 0))
                    name = f"z{label}"
                    label += 1
                    a.br("beq", name)
                    a.w(lwz(4, 3, 4), stw(4, 1, 0x18))
                    a.L(name)
                else:
                    a.w(E.ori(8, 31, rng.randrange(0x10000)), E.x_form(31, 8, 9, 31, 28), stw(9, 1, 0x1C))
            a.w(lwz(0, 1, 0x44), E.MTLR(0), lwz(31, 1, 0x3C), lwz(30, 1, 0x38), lwz(29, 1, 0x34),
                E.addi(1, 1, 0x40), E.BLR)
            assert 4 * len(a.ops) <= FILLER_SLOT
            self.add_rows(a.rows(f"filler{index}"))
            self.filler_rows += 1
            self.filler_instructions += len(a.ops)

    def add_rows(self, rows):
        for row in rows:
            assert row["address"] not in self.rows, hex(row["address"])
            self.rows[row["address"]] = row

    def build_support(self):
        leaf = Asm(LEAF)
        leaf.w(E.addi(7, 7, 1), E.BLR)
        self.add_rows(leaf.rows("leaf"))
        hle = Asm(HLE_CALLEE)
        hle.w(E.addi(7, 7, 1), E.BLR)
        self.add_rows(hle.rows("hle_callee"))

    def build_shapes(self, base: int):
        slot = base
        for name, kind, control in SHAPES:
            for style in STYLES:
                full = f"{name}_{style}"
                seed = slot
                a = Asm(slot)
                table = {0: LEAF, 16: SENTINEL}
                hook = 0
                if name == "intra":
                    shape_intra(a, style, seed)
                elif name == "xrec_direct":
                    shape_xrec_direct(a, style, seed)
                elif name == "eof_fall":
                    shape_eof_fall(a, style, seed)
                elif name == "bctr_local":
                    shape_bctr_local(a, style, seed)
                    table[20] = a.labels["head"]
                elif name == "bctr_default":
                    shape_bctr_default(a, style, seed)
                    table[4] = a.labels["D"]
                    table[8] = a.labels["C"]
                elif name in ("resume_else", "bclr_cond", "bclrl"):
                    leave = {"resume_else": "blr", "bclr_cond": "beqlr", "bclrl": "blrl"}[name]
                    shape_resume(a, style, seed, leave)
                    table[12] = a.labels["E"]
                elif name == "resume_ind":
                    shape_resume_ind(a, style, seed)
                    table[12] = a.labels["E"]
                    table[24] = a.labels["F"]
                elif name == "bclr_top":
                    shape_bclr_top(a, style, seed)
                    table[20] = a.labels["G"]
                elif name == "hle_xrec":
                    shape_bl_xrec(a, style, seed, HLE_CALLEE)
                    hook = HLE_CALLEE
                elif name == "bl_xrec":
                    shape_bl_xrec(a, style, seed, LEAF)
                elif name == "xrec_ring":
                    shape_xrec_ring(a, style, seed)
                rows = a.rows(full)
                assert 4 * len(a.ops) <= SHAPE_SLOT, (full, len(a.ops))
                self.add_rows(rows)
                self.shapes.append({"name": full, "base": name, "kind": kind, "style": style,
                                    "control": control, "entry": slot, "hook": hook,
                                    "table": {str(k): v for k, v in sorted(table.items())},
                                    "rows": [r["address"] for r in rows],
                                    "labels": {k: v for k, v in a.labels.items()}})
                slot += SHAPE_SLOT


def write_dol(path: Path, fx: Fixture):
    text0_end = max(a + 4 * len(r["words"]) for a, r in fx.rows.items())
    size0 = (text0_end - CODE_BASE + 0xFF) & ~0xFF
    image = bytearray(0x100 + size0)
    struct.pack_into(">I", image, 0x00, 0x100)
    struct.pack_into(">I", image, 0x48, CODE_BASE)
    struct.pack_into(">I", image, 0x90, size0)
    struct.pack_into(">I", image, 0xE0, CODE_BASE)
    for address, row in fx.rows.items():
        assert address >= CODE_BASE, f"row {address:08X} below the code base (no real-mode rows exist)"
        base = 0x100 + address - CODE_BASE
        for i, word in enumerate(row["words"]):
            struct.pack_into(">I", image, base + 4 * i, word)
    path.write_bytes(bytes(image))


def write_header(path: Path, fx: Fixture, functions: set):
    lines = ["// Generated by tools/difftest/gen_f1c.py - do not edit.", "#pragma once", "#include <cstddef>",
             "#include \"gekko/types.h\"", "namespace f1c {", "using gekko::u32;"]
    for address in sorted(fx.rows):
        words = fx.rows[address]["words"]
        lines.append(f"inline constexpr u32 kRow_{address:08X}[] = {{" + ",".join(f"0x{w:08X}u" for w in words) + "};")
    lines.append("struct CodeRow { u32 address; std::size_t count; const u32* words; };")
    lines.append("inline constexpr CodeRow kRows[] = {")
    for address in sorted(fx.rows):
        lines.append(f"    {{0x{address:08X}u, {len(fx.rows[address]['words'])}u, kRow_{address:08X}}},")
    lines.append("};")
    lines.append("struct Shape { const char* name; u32 entry; u32 hook; u32 table[8]; bool control; };")
    lines.append("inline constexpr Shape kShapes[] = {")
    for s in fx.shapes:
        table = [0] * 8
        for k, v in s["table"].items():
            table[int(k) // 4] = v
        lines.append(f"    {{\"{s['name']}\", 0x{s['entry']:08X}u, 0x{s['hook']:08X}u, {{" +
                     ",".join(f"0x{v:08X}u" for v in table) + f"}}, {'true' if s['control'] else 'false'}}},")
    lines.append("};")
    lines.append("struct CanaryProgram { const char* name; u32 entry; u32 table[16]; };")
    lines.append("inline constexpr CanaryProgram kCanary[] = {")
    for p in fx.canary:
        table = [0] * 16
        for k, v in p["table"].items():
            table[int(k) // 4] = v
        lines.append(f"    {{\"{p['name']}\", 0x{p['entry']:08X}u, {{" + ",".join(f"0x{v:08X}u" for v in table) + "}},")
    if not fx.canary:
        lines.append("    {\"none\", 0u, {0}},")
    lines.append("};")
    for p in fx.canary:
        for key in ("inner_a", "inner_b", "dec_reload", "poll_word", "straddle", "prefix_instructions",
                    "loop_instructions"):
            if key in p:
                lines.append(f"inline constexpr u32 k_{p['name']}_{key} = {p[key]}u;")
        for label, address in sorted(p["labels"].items()):
            lines.append(f"inline constexpr u32 k_{p['name']}_label_{label} = 0x{address:08X}u;")
    lines.append(f"inline constexpr u32 kSentinel = 0x{SENTINEL:08X}u;")
    lines.append(f"inline constexpr u32 kDataBase = 0x{DATA_BASE:08X}u;")
    lines.append(f"inline constexpr u32 kStackTop = 0x{STACK_TOP:08X}u;")
    lines.append(f"inline constexpr u32 kLeaf = 0x{LEAF:08X}u;")
    lines.append(f"inline constexpr u32 kHleCallee = 0x{HLE_CALLEE:08X}u;")
    lines.append(f"inline constexpr u32 kOsContext = 0x{OS_CONTEXT:08X}u;")
    for name, address in sorted(fx.os_handlers.items()):
        lines.append(f"inline constexpr u32 k{name}Handler = 0x{address:08X}u;")
    lines.append("}  // namespace f1c")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_generated(out: Path):
    import re
    table = (out / "recomp_table.cpp").read_text(encoding="utf-8")
    functions = set(int(m.group(1), 16) for m in re.finditer(r"\{0x([0-9A-F]+)u, 0x[0-9A-F]+u, &fn_", table))
    ranges = [(int(a, 16), int(b, 16)) for a, b in
              re.findall(r"\{0x([0-9A-F]+)u, 0x([0-9A-F]+)u, &fn_[0-9A-F]+_at\}", table)]
    return functions, ranges


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--shards", type=int, default=8)
    parser.add_argument("--filler", type=int, default=0,
                        help="guest instructions of never-executed filler per shard (game-sized TUs)")
    parser.add_argument("--seed", type=int, default=20260916)
    args = parser.parse_args()
    out = args.out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    fx = Fixture()
    fx.build_support()
    fx.build_shapes(SHAPE_BASE)
    if args.filler:
        fx.build_filler(args.filler * args.shards, args.seed)
    import f1c_canary_programs as CP
    CP.build(fx)

    bad = sorted({f"{w:08X}" for r in fx.rows.values() for w in r["words"] if EC.emitter_class(w) is None})
    if bad:
        print("rows contain words the emitter catalogue rejects: " + " ".join(bad[:20]), file=sys.stderr)
        return 3

    dol = out / "f1c.dol"
    write_dol(dol, fx)
    with (out / "functions.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("addr_hex", "addr", "name", "size", "ins", "tags", "strings", "callees"))
        for address in sorted(fx.rows):
            row = fx.rows[address]
            writer.writerow((hex(address), address, f"{row['name']}_{address:08X}", len(row["words"]) * 4,
                             len(row["words"]), "", "", ""))
    command = [str(args.generator.resolve()), "--dol", str(dol), "--functions", str(out / "functions.csv"),
               "--out", str(out), "--shards", str(args.shards), "--report", str(out / "recomp_report.txt")]
    result = subprocess.run(command, capture_output=True, text=True)
    (out / "ppcrecomp.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
        return result.returncode
    functions, ranges = parse_generated(out)
    rejected = sorted(a for a in fx.rows if a not in functions)
    if rejected:
        print("ppcrecomp rejected rows the fixture needs translated:", file=sys.stderr)
        for a in rejected:
            print(f"  {a:08X} {fx.rows[a]['name']}", file=sys.stderr)
        print((out / "recomp_report.txt").read_text(encoding="utf-8")[:4000], file=sys.stderr)
        return 4
    for i in range(args.shards):
        if not (out / f"recomp_{i:03d}.cpp").exists():
            print(f"missing generated shard {i}", file=sys.stderr)
            return 6
    write_header(out / "f1c_fixture.h", fx, functions)
    manifest = {"schema": SCHEMA, "generator": str(args.generator.resolve()),
                "generator_sha256": hashlib.sha256(args.generator.read_bytes()).hexdigest(),
                "generator_command": command, "shards": args.shards, "filler_per_shard": args.filler,
                "filler_rows": fx.filler_rows, "filler_instructions": fx.filler_instructions, "seed": args.seed,
                "rows": {f"{a:08X}": {"name": r["name"], "words": len(r["words"])} for a, r in sorted(fx.rows.items())},
                "shapes": fx.shapes, "canary": fx.canary}
    (out / "f1c_fixture.json").write_text(json.dumps(manifest, indent=1, sort_keys=True), encoding="utf-8")
    print(f"F1c fixture: {len(fx.shapes)} shapes, {len(fx.canary)} canary programs, {len(fx.rows)} rows, "
          f"{args.shards} shards -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
