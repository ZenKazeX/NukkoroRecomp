"""Differential-harness vector generator (task F1a: integer, load/store and branch vectors).

Emits into --out (which is DELETED and recreated: ppcrecomp's plan_shards reuses an existing
recomp_layout.csv, so a stale directory could change shard placement):

  diff.dol            synthetic DOL, text0 at 0x80010000 holding every guest row
  functions.csv       one record per row (the shape extract/functions.csv has)
  vectors.jsonl       one JSON object per vector: rows, entry, initial state, memory, metadata
  vectors.txt         the same vectors in the line format difftest_driver.cpp parses
  manifest.json       generator identity, arguments, counts, accepted-row check
  recomp_000..N.cpp, recomp_table.cpp, recomp_decls.h, recomp_layout.csv, recomp_report.txt
                      produced by running the REAL ppcrecomp with the production arguments:
                      --dol --functions --out --shards N --report (gap sweep ON, i.e. no
                      --no-gap-sweep, exactly as CMakeLists.txt SHADOW_RECOMP_ARGS)

Every vector ends when guest control reaches the untranslated sentinel 0x80009000 (LR on entry).

Families (all deterministic in --seed):
  int1   one integer instruction + blr, random/edge operands, forced register aliasing, OE/Rc
  spr1   mfspr/mtspr on SPRs the spec State carries (+ PVR/HID1 writes) + blr
  clock  mfspr DEC/TBL/TBU, mftb, mtspr DEC/TBL/TBU after straight runs (native time: TB and DEC never move
         inside a vector - guest code takes no game time - so every read returns the initial or the written
         value; the runs crossed TB ticks of the retired instruction clock)
  ls1    one load/store instruction + blr, EAs placed on 16 KiB host-page edges in the data window
  lsseq  store-then-load overlaps, lwarx/stwcx. pairs, update-form chains
  seq    2-8 straight integer instructions with def-use chains
  prefix words[0..k) + blr of every seq/lsseq vector (first-divergence localisation by prefix)
  entry  a seq/lsseq/branch vector entered mid-block (dispatcher find_range -> cold kEntrySkip)
  br     bc/bclr/bcctr/b/bl/bcl/bclrl/bcctrl shapes, CR-fusion adjacency, CTR loops, backward
         bcctr switch, cross-record transfers, a gap-sweep-recovered root
  alias  systematic register aliasing for every integer template with aliasing-sensitive values
  hint   emitter no-op hints (dcbf/dcbst/dcbi/icbi) - the spec has no model: engine-only
  invalid reserved-bit / invalid forms: the spec says boundedly undefined: engine-only
  str1   one string instruction (lswi/lswx/stswi/stswx, PAGE 448/449/561/562) + blr: NB 0..31, XER n
         0..127, register wrap r31 -> r0, EAs on the 16 KiB host-page edge, invalid forms engine-only
  strseq store-then-load string round trips (i-forms, x-forms sharing XER n, r30..r0 wrap) (+ prefixes
         and mid-block entries)
  strcall a caller `bl`s a string row, and a function whose rare path holds the string word, taken and
         not taken, entered directly and through a caller (repair L0r0-03: before it ppcrecomp rejected
         every such function, so the generator exited 4 and callers left native code NotRecompiled)

The string families are generated LAST so every earlier family keeps its exact vectors, ids and
addresses (the known-candidate fingerprints in known_candidates_f1a.json are exact per vector).
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ppcenc as E  # noqa: E402

SCHEMA = "gekko-difftest-vector/1"
SENTINEL = 0x80009000
CODE_BASE = 0x80010000
SLOT = 0x40
DATA_BASE = 0x80300000          # the data window (an address of the game's window)
DATA_SIZE = 0x8000              # crosses the 16 KiB host page boundary at 0x80304000
MSR = 0x00003032                # FP | ME | IR | DR | RI, EE = 0 (Cpu::reset value)
HID0 = 0x0011C464
HID2 = 0xE0000000
M32 = 0xFFFFFFFF

EDGE32 = [0, 1, 2, 3, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE,
          0x00007FFF, 0x00008000, 0x0000FFFF, 0x00010000, 0xFFFF8000, 0xFFFF7FFF, 0x7FFF8000,
          0x40000000, 0xC0000000, 0x0000001F, 0x00000020, 0x0000003F, 0x00000040]
EDGE16 = [0, 1, -1, 2, 0x7FFF, -0x8000, 0x7FFE, -0x7FFF, 0x00FF, 0x0100, -0x0100, 0x1F, 0x20]


class Gen:
    def __init__(self, seed: int, scale: float):
        self.rng = random.Random(seed)
        self.seed = seed
        self.scale = scale
        self.rows = {}          # address -> {"name", "words"}
        self.dedupe = {}        # tuple(words) -> address (position-independent rows only)
        self.next_slot = CODE_BASE
        self.vectors = []
        self.counter = {}

    # ---- layout ------------------------------------------------------------------
    def alloc(self, count_words: int) -> int:
        address = self.next_slot
        slots = max(1, (count_words * 4 + SLOT - 1) // SLOT)
        self.next_slot += slots * SLOT
        return address

    def put_row(self, address: int, words, name: str):
        assert address not in self.rows
        self.rows[address] = {"name": name, "words": [w & M32 for w in words]}

    def row_pic(self, words, name: str) -> int:
        """A position-independent row (no PC-relative branch): shared between vectors."""
        key = tuple(w & M32 for w in words)
        if key in self.dedupe:
            return self.dedupe[key]
        address = self.alloc(len(words))
        self.put_row(address, words, name)
        self.dedupe[key] = address
        return address

    def new_id(self, family: str) -> str:
        n = self.counter.get(family, 0)
        self.counter[family] = n + 1
        return f"{family}-{n:05d}"

    # ---- state -------------------------------------------------------------------
    def v32(self) -> int:
        r = self.rng.random()
        if r < 0.45:
            return self.rng.choice(EDGE32)
        if r < 0.60:
            k = self.rng.randrange(32)
            return ((1 << k) + self.rng.choice((-1, 0, 1))) & M32
        return self.rng.getrandbits(32)

    def imm16(self) -> int:
        return self.rng.choice(EDGE16) if self.rng.random() < 0.5 else self.rng.randrange(-0x8000, 0x8000)

    def base_state(self) -> dict:
        rng = self.rng
        st = {
            "gpr": [self.v32() for _ in range(32)],
            "fpr": [[rng.getrandbits(64), rng.getrandbits(64)] for _ in range(32)],
            "cr": rng.getrandbits(32),
            "xer": (rng.randrange(8) << 29) | (rng.randrange(128) if rng.random() < 0.5 else 0),
            "lr": SENTINEL,
            "ctr": self.v32(),
            "msr": MSR,
            "fpscr": rng.getrandbits(32) & ~0x00000800 & M32,
            "tb": rng.getrandbits(64) >> rng.choice((0, 1, 32, 40)),
            "residue": rng.randrange(12),   # the retired instruction clock's prescaler phase: no effect any more
            "dec": rng.choice((0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, rng.getrandbits(32))),
            # SPRs the spec State carries: SRR0 26, SRR1 27, DAR 19, DSISR 18, SPRG0-3 272-275.
            "spr": {26: rng.getrandbits(32) & ~3 & M32, 27: rng.getrandbits(32), 19: rng.getrandbits(32),
                    18: rng.getrandbits(32), 272: rng.getrandbits(32), 273: rng.getrandbits(32),
                    274: rng.getrandbits(32), 275: rng.getrandbits(32)},
            "reserve": rng.random() < 0.5,
            "reserve_addr": (DATA_BASE + rng.randrange(DATA_SIZE)) & ~31 & M32,
        }
        return st

    def reg(self, avoid=()):
        choices = [r for r in (0, 1, 2, 3, 4, 5, 13, 30, 31) if r not in avoid] + \
                  [r for r in range(32) if r not in avoid]
        return self.rng.choice(choices)

    # ---- vector emission ---------------------------------------------------------
    def add(self, family, rows, entry, state, words_note, mem=None, spec="compare", reason="",
            max_steps=4096, parent=None, prefix_k=None, mnemonic=None, expect_mnemonic=None):
        vid = self.new_id(family)
        v = {
            "schema": SCHEMA, "id": vid, "family": family, "seed": self.seed,
            "rows": rows, "entry": entry, "state": state, "mem": mem or [],
            "max_steps": max_steps, "spec": spec, "reason": reason, "note": words_note,
            "parent": parent, "prefix_k": prefix_k, "mnemonic": mnemonic,
            "expect_mnemonic": expect_mnemonic,
        }
        self.vectors.append(v)
        return v

    # ================================================================================
    # int1
    # ================================================================================
    def int_word(self):
        """One integer instruction. Returns (word, mnemonic_base, spec_mode, reason)."""
        rng = self.rng
        kind = rng.randrange(12)
        d, a, b = self.reg(), self.reg(), self.reg()
        alias = rng.random()
        if alias < 0.15:
            a = d
        elif alias < 0.30:
            b = d
        elif alias < 0.40:
            b = a
        elif alias < 0.48:
            a = b = d
        if kind == 0:
            op, name = rng.choice(((7, "mulli"), (8, "subfic"), (12, "addic"), (13, "addic."),
                                   (14, "addi"), (15, "addis")))
            return E.d_form(op, d, a, self.imm16()), name
        if kind == 1:
            op, name = rng.choice(((24, "ori"), (25, "oris"), (26, "xori"), (27, "xoris"),
                                   (28, "andi."), (29, "andis.")))
            return E.d_form(op, d, a, self.imm16()), name
        if kind == 2:
            op, name = rng.choice(((10, "cmpli"), (11, "cmpi")))
            return E.cmp_imm(op, rng.randrange(8), a, self.imm16()), name
        if kind == 3:
            op, name = rng.choice(((20, "rlwimi"), (21, "rlwinm"), (23, "rlwnm")))
            sh = rng.choice((0, 1, 31, rng.randrange(32))) if op != 23 else b
            mb = rng.choice((0, 31, rng.randrange(32)))
            me = rng.choice((0, 31, mb, (mb - 1) % 32, rng.randrange(32)))
            return E.m_form(op, d, a, sh, mb, me, rng.randrange(2)), name
        if kind in (4, 5):
            xo, name, unary, oe_ok = rng.choice((
                (266, "add", False, True), (10, "addc", False, True), (138, "adde", False, True),
                (234, "addme", True, True), (202, "addze", True, True), (40, "subf", False, True),
                (8, "subfc", False, True), (136, "subfe", False, True), (232, "subfme", True, True),
                (200, "subfze", True, True), (104, "neg", True, True), (235, "mullw", False, True),
                (75, "mulhw", False, False), (11, "mulhwu", False, False), (491, "divw", False, True),
                (459, "divwu", False, True)))
            if unary:
                b = 0
            oe = rng.randrange(2) if oe_ok else 0
            return E.xo_form(d, a, b, xo, oe, rng.randrange(2)), name
        if kind in (6, 7):
            xo, name, unary = rng.choice((
                (28, "and", False), (60, "andc", False), (444, "or", False), (412, "orc", False),
                (316, "xor", False), (476, "nand", False), (124, "nor", False), (284, "eqv", False),
                (24, "slw", False), (536, "srw", False), (792, "sraw", False), (824, "srawi", False),
                (26, "cntlzw", True), (922, "extsh", True), (954, "extsb", True)))
            if unary:
                b = 0
            if xo == 824:
                b = rng.choice((0, 1, 31, rng.randrange(32)))
            return E.x_form(31, d, a, b, xo, rng.randrange(2)), name
        if kind == 8:
            xo, name = rng.choice(((0, "cmp"), (32, "cmpl")))
            return E.cmp_reg(xo, rng.randrange(8), a, b), name
        if kind == 9:
            which = rng.randrange(3)
            if which == 0:
                return E.mcrxr(rng.randrange(8)), "mcrxr"
            if which == 1:
                return E.mfcr(d), "mfcr"
            return E.mtcrf(d, rng.choice((0, 0xFF, 0x80, 0x01, rng.randrange(256)))), "mtcrf"
        if kind == 10:
            return E.mcrf(rng.randrange(8), rng.randrange(8)), "mcrf"
        xo, name = rng.choice(((257, "crand"), (129, "crandc"), (289, "creqv"), (225, "crnand"),
                               (33, "crnor"), (449, "cror"), (417, "crorc"), (193, "crxor")))
        return E.cr_logical(xo, rng.randrange(32), rng.randrange(32), rng.randrange(32)), name

    def shape_operands(self, word, st):
        """Bias operands for arithmetic edges the random pools rarely produce."""
        rng = self.rng
        primary = word >> 26
        if primary == 31:
            xo9 = (word >> 1) & 0x1FF
            a, b = (word >> 16) & 31, (word >> 11) & 31
            if xo9 in (491, 459) and rng.random() < 0.3:      # division edges
                choice = rng.randrange(3)
                if choice == 0:
                    st["gpr"][b] = 0
                elif choice == 1:
                    st["gpr"][a], st["gpr"][b] = 0x80000000, 0xFFFFFFFF
                else:
                    st["gpr"][b] = rng.choice((1, 0xFFFFFFFF, 2))
            if xo9 in (24, 536, 792) and rng.random() < 0.5:   # shift amounts 31/32/63/64
                st["gpr"][b] = rng.choice((31, 32, 33, 63, 64, 0xFFFFFFE0, 0x3F, 0x20))

    def fam_int1(self, count):
        for _ in range(count):
            word, name = self.int_word()
            row = self.row_pic([word, E.BLR], f"int1_{name}")
            for _ in range(self.rng.choice((1, 2, 3))):
                st = self.base_state()
                self.shape_operands(word, st)
                self.add("int1", [row], row, st, f"{word:08X}", mnemonic=name, expect_mnemonic=name)

    # ================================================================================
    # spr1
    # ================================================================================
    def fam_spr1(self, count):
        sprs = (1, 8, 9, 18, 19, 26, 27, 272, 273, 274, 275, 287, 1009)
        for _ in range(count):
            number = self.rng.choice(sprs)
            r = self.reg()
            if self.rng.random() < 0.5 and number not in (287, 1009):
                word, name = E.mfspr(r, number), "mfspr"
            else:
                word, name = E.mtspr(r, number), "mtspr"
                if number == 8:
                    continue      # mtlr would lose the return: covered by the branch shapes
            row = self.row_pic([word, E.BLR], f"spr1_{name}_{number}")
            st = self.base_state()
            self.add("spr1", [row], row, st, f"{word:08X}", mnemonic=name, expect_mnemonic=name)

    # ================================================================================
    # clock: mfspr DEC/TBL/TBU and mftb after a straight run, mtspr DEC/TBL/TBU
    # ================================================================================
    def fam_clock(self, count):
        rng = self.rng
        for _ in range(count):
            k = rng.randrange(0, 14)
            words = [E.addi(3, 3, 1)] * k
            names = ["addi"] * k
            for _ in range(rng.randrange(1, 3)):
                kind = rng.randrange(6)
                r = rng.choice((4, 5, 6, 7))
                if kind == 0:
                    words.append(E.mfspr(r, 22)); names.append("mfspr")
                elif kind == 1:
                    words.append(E.mfspr(r, rng.choice((268, 269)))); names.append("mfspr")
                elif kind == 2:
                    tbr = rng.choice((268, 269))
                    words.append(E.x_form(31, r, E.spr_field(tbr) >> 5, E.spr_field(tbr) & 0x1F, 371))
                    names.append("mftb")
                elif kind == 3:
                    words.append(E.mtspr(r, 22)); names.append("mtspr")
                elif kind == 4:
                    words.append(E.mtspr(r, rng.choice((284, 285)))); names.append("mtspr")
                else:
                    words += [E.addi(3, 3, 1)] * rng.randrange(1, 13); names += ["addi"] * (len(words) - len(names))
                words.append(E.addi(3, 3, 1)); names.append("addi")
            st = self.base_state()
            st["dec"] = rng.choice((0, 1, 2, 0x80000000, 0x7FFFFFFF, rng.getrandbits(32)))
            for r in (4, 5, 6, 7):
                st["gpr"][r] = rng.choice((0, 1, 0x7FFFFFFF, 0x80000000, rng.getrandbits(32)))
            row = self.alloc(len(words) + 1)
            self.put_row(row, words + [E.BLR], "clock_row")
            self.add("clock", [row], row, st, " ".join(f"{w:08X}" for w in words), mnemonic=",".join(names))

    # ================================================================================
    # load / store
    # ================================================================================
    D_LS = ((32, "lwz", 4, "r"), (33, "lwzu", 4, "r"), (34, "lbz", 1, "r"), (35, "lbzu", 1, "r"),
            (36, "stw", 4, "w"), (37, "stwu", 4, "w"), (38, "stb", 1, "w"), (39, "stbu", 1, "w"),
            (40, "lhz", 2, "r"), (41, "lhzu", 2, "r"), (42, "lha", 2, "r"), (43, "lhau", 2, "r"),
            (44, "sth", 2, "w"), (45, "sthu", 2, "w"))
    X_LS = ((23, "lwzx", 4, "r", False), (55, "lwzux", 4, "r", True), (87, "lbzx", 1, "r", False),
            (119, "lbzux", 1, "r", True), (151, "stwx", 4, "w", False), (183, "stwux", 4, "w", True),
            (215, "stbx", 1, "w", False), (247, "stbux", 1, "w", True), (279, "lhzx", 2, "r", False),
            (311, "lhzux", 2, "r", True), (343, "lhax", 2, "r", False), (375, "lhaux", 2, "r", True),
            (407, "sthx", 2, "w", False), (439, "sthux", 2, "w", True), (534, "lwbrx", 4, "r", False),
            (662, "stwbrx", 4, "w", False), (790, "lhbrx", 2, "r", False), (918, "sthbrx", 2, "w", False))

    def ea_in_window(self, width: int, align: int = 1) -> int:
        rng = self.rng
        page_edge = DATA_BASE + 0x4000
        choice = rng.random()
        if choice < 0.35:       # straddle or touch the 16 KiB host page boundary
            ea = page_edge - rng.randrange(0, width + 4)
        elif choice < 0.45:
            ea = DATA_BASE + rng.randrange(0, 8)
        else:
            ea = DATA_BASE + 0x40 + rng.randrange(DATA_SIZE - 0x100)
        ea &= ~(align - 1)
        assert DATA_BASE <= ea and ea + width + 64 <= DATA_BASE + DATA_SIZE
        return ea & M32

    def random_bytes(self, n: int) -> bytes:
        return bytes(self.rng.getrandbits(8) for _ in range(n))

    def mem_around(self, ea: int, width: int, before: int = 8, after: int = 8):
        start = max(DATA_BASE, ea - before)
        end = min(DATA_BASE + DATA_SIZE, ea + width + after)
        return [[start, self.random_bytes(end - start).hex()]]

    def ls_word(self, st):
        """One load/store instruction with a valid EA. Returns (word, name, mem, spec, reason)."""
        rng = self.rng
        kind = rng.random()
        if kind < 0.45:
            op, name, width, rw = rng.choice(self.D_LS)
            update = op & 1 == 1
            d = self.reg()
            a = self.reg(avoid=(0,))
            spec, reason = "compare", ""
            if update and d != 0 and rng.random() < 0.25:
                a = d                                   # rS == rA store / rD == rA load
                if rw == "r":
                    spec, reason = "engine-only", "invalid form: update load with rA == rD (PAGE 87)"
            disp = self.imm16()
            ea = self.ea_in_window(width)
            st["gpr"][a] = (ea - disp) & M32
            word = E.d_form(op, d, a, disp)
            return word, name, self.mem_around(ea, width), spec, reason
        if kind < 0.85:
            xo, name, width, rw, update = rng.choice(self.X_LS)
            # rA = 0 is legal for the non-update X-forms (EA = rB); update forms need rA != 0.
            d, a, b = self.reg(), self.reg(avoid=(0,) if update else ()), self.reg()
            spec, reason = "compare", ""
            alias = rng.random()
            if alias < 0.2:
                b = a
            elif alias < 0.35:
                b = d
            elif alias < 0.5 and not (update and d == 0):
                a = d
            if update and a == d and rw == "r":
                spec, reason = "engine-only", "invalid form: update load with rA == rD (PAGE 87)"
            ea = self.ea_in_window(width, 2 if (b == a and a != 0) else 1)
            if a == 0:
                st["gpr"][b] = ea                         # EA = 0 + rB (rB may itself be r0)
            elif b == a:
                st["gpr"][a] = (ea // 2) & M32
            else:
                st["gpr"][a] = self.rng.getrandbits(32)
                st["gpr"][b] = (ea - st["gpr"][a]) & M32
            word = E.x_form(31, d, a, b, xo)
            return word, name, self.mem_around(ea, width), spec, reason
        if kind < 0.92:
            op, name = rng.choice(((46, "lmw"), (47, "stmw")))
            d = rng.choice((31, 30, 29, 27, 24, rng.randrange(1, 32)))
            a = self.reg(avoid=(0,))
            spec, reason = "compare", ""
            if op == 46 and a >= d:
                spec, reason = "engine-only", "invalid form: lmw with rA in rD..r31 (PAGE 447)"
            disp = self.imm16() & ~3
            align = 4 if rng.random() < 0.85 else 1
            ea = self.ea_in_window(4 * (32 - d), align)
            if ea + 4 * (32 - d) + 64 > DATA_BASE + DATA_SIZE:
                ea = DATA_BASE + 0x100
            st["gpr"][a] = (ea - disp) & M32
            word = E.d_form(op, d, a, disp)
            return word, name, self.mem_around(ea, 4 * (32 - d)), spec, reason
        # dcbz (rA = 0 legal: EA = rB)
        a, b = self.reg(), self.reg()
        ea = self.ea_in_window(40)
        if a == 0:
            st["gpr"][b] = ea
        elif b == a:
            ea &= ~1
            st["gpr"][a] = ea // 2
        else:
            st["gpr"][a] = self.rng.getrandbits(32)
            st["gpr"][b] = (ea - st["gpr"][a]) & M32
        word = E.x_form(31, 0, a, b, 1014)
        return word, "dcbz", self.mem_around(ea & ~31, 32, 16, 16), "compare", ""

    def fam_ls1(self, count):
        for _ in range(count):
            st = self.base_state()
            word, name, mem, spec, reason = self.ls_word(st)
            row = self.row_pic([word, E.BLR], f"ls1_{name}")
            self.add("ls1", [row], row, st, f"{word:08X}", mem=mem, spec=spec, reason=reason,
                     mnemonic=name, expect_mnemonic=name)

    # ================================================================================
    # alias: systematic register aliasing
    # ================================================================================
    ALIAS_VALUES = ((0x80000001, 1), (0xFFFFFFFF, 31), (0x80000000, 0xFFFFFFFF), (0x7FFFFFFF, 1),
                    (0xC0000003, 2), (0x00000001, 0xFFFFFFFF), (0xFFFF8001, 33), (0x12345678, 0x0000FFFF))

    def fam_alias(self, repeats):
        rng = self.rng
        xo_forms = ((266, "add", 2), (10, "addc", 2), (138, "adde", 2), (234, "addme", 1), (202, "addze", 1),
                    (40, "subf", 2), (8, "subfc", 2), (136, "subfe", 2), (232, "subfme", 1), (200, "subfze", 1),
                    (104, "neg", 1), (235, "mullw", 2), (75, "mulhw", 2), (11, "mulhwu", 2), (491, "divw", 2),
                    (459, "divwu", 2))
        x_forms = ((28, "and", 2), (60, "andc", 2), (444, "or", 2), (412, "orc", 2), (316, "xor", 2),
                   (476, "nand", 2), (124, "nor", 2), (284, "eqv", 2), (24, "slw", 2), (536, "srw", 2),
                   (792, "sraw", 2), (824, "srawi", 0), (26, "cntlzw", 1), (922, "extsh", 1), (954, "extsb", 1))
        for _ in range(repeats):
            r1 = rng.choice((3, 5, 9, 13, 31, 1))
            r2 = rng.choice([r for r in (4, 6, 12, 30, 0, 2) if r != r1])
            for xo, name, arity in xo_forms:
                patterns = ((r1, r1, r2), (r1, r2, r1), (r1, r2, r2), (r1, r1, r1)) if arity == 2 else \
                           ((r1, r1, 0), (r1, r2, 0))
                oe_ok = xo not in (75, 11)
                for d, a, b in patterns:
                    word = E.xo_form(d, a, b, xo, rng.randrange(2) if oe_ok else 0, rng.randrange(2))
                    self.alias_vectors(word, name, a, b if arity == 2 else None)
            for xo, name, arity in x_forms:
                if arity == 0:
                    patterns = ((r1, r1, rng.choice((1, 2, 31, 0))), (r1, r2, rng.choice((1, 4, 31))))
                elif arity == 1:
                    patterns = ((r1, r1, 0), (r1, r2, 0))
                else:
                    patterns = ((r1, r1, r2), (r1, r2, r1), (r1, r2, r2), (r1, r1, r1))
                for s, a, b in patterns:
                    word = E.x_form(31, s, a, b, xo, rng.randrange(2))
                    self.alias_vectors(word, name, s, b if arity == 2 else None)
            for op, name in ((20, "rlwimi"), (21, "rlwinm"), (23, "rlwnm")):
                sh = r1 if op == 23 else rng.randrange(32)
                word = E.m_form(op, r1, r1, sh, rng.randrange(32), rng.randrange(32), rng.randrange(2))
                self.alias_vectors(word, name, r1, r1 if op == 23 else None)
            for op, name in ((7, "mulli"), (8, "subfic"), (12, "addic"), (13, "addic."), (14, "addi"),
                             (15, "addis"), (24, "ori"), (26, "xori"), (28, "andi.")):
                word = E.d_form(op, r1, r1, self.imm16())
                self.alias_vectors(word, name, r1, None)

    def alias_vectors(self, word, name, first, second):
        row = self.row_pic([word, E.BLR], f"alias_{name}")
        for pair in self.rng.sample(self.ALIAS_VALUES, 2):
            st = self.base_state()
            st["gpr"][first] = pair[0]
            if second is not None and second != first:
                st["gpr"][second] = pair[1]
            st["xer"] = (st["xer"] & ~0xE0000000) | (self.rng.randrange(8) << 29)
            self.add("alias", [row], row, st, f"{word:08X}", mnemonic=name, expect_mnemonic=name)

    def fam_hint(self):
        for xo, name in ((86, "dcbf"), (54, "dcbst"), (470, "dcbi"), (982, "icbi")):
            for _ in range(3):
                st = self.base_state()
                a, b = self.reg(), self.reg()
                word = E.x_form(31, 0, a, b, xo)
                row = self.row_pic([word, E.BLR], f"hint_{name}")
                self.add("hint", [row], row, st, f"{word:08X}", spec="engine-only",
                         reason="spec model has no " + name + " (not in any spec module)", mnemonic=name)

    # ================================================================================
    # sequences (+ prefixes and interior entries)
    # ================================================================================
    def with_prefixes_and_entries(self, family, words, st, mem, spec, reason, mnemonics):
        body = list(words)
        row = self.alloc(len(body) + 1)
        self.put_row(row, body + [E.BLR], f"{family}_row")
        parent = self.add(family, [row], row, st, " ".join(f"{w:08X}" for w in body), mem=mem,
                          spec=spec, reason=reason, mnemonic=",".join(mnemonics))
        for k in range(1, len(body)):
            prow = self.row_pic(body[:k] + [E.BLR], f"prefix_{family}")
            self.add("prefix", [prow], prow, json.loads(json.dumps(st)), f"prefix {k} of {parent['id']}",
                     mem=mem, spec=spec, reason=reason, parent=parent["id"], prefix_k=k,
                     mnemonic=",".join(mnemonics[:k]))
        if len(body) >= 2:
            k = self.rng.randrange(1, len(body))
            self.add("entry", [row], row + 4 * k, json.loads(json.dumps(st)),
                     f"mid-block entry +{4 * k:#x} of {parent['id']}", mem=mem, spec=spec, reason=reason,
                     parent=parent["id"], mnemonic=",".join(mnemonics[k:]))

    def fam_seq(self, count):
        for _ in range(count):
            n = self.rng.randrange(2, 9)
            words, names = [], []
            last_dest = None
            for _ in range(n):
                for _attempt in range(20):
                    word, name = self.int_word()
                    if last_dest is None or self.rng.random() < 0.4:
                        break
                    # def-use chain: make the previous destination an operand of this one
                    fields = (word >> 16) & 31, (word >> 11) & 31
                    if last_dest in fields:
                        break
                words.append(word)
                names.append(name)
                primary = word >> 26
                last_dest = (word >> 21) & 31 if primary in (7, 8, 12, 13, 14, 15) else (word >> 16) & 31
            st = self.base_state()
            for w in words:
                self.shape_operands(w, st)
            self.with_prefixes_and_entries("seq", words, st, [], "compare", "", names)

    def fam_lsseq(self, count):
        rng = self.rng
        for _ in range(count):
            st = self.base_state()
            shape = rng.randrange(4)
            base = rng.choice((6, 7, 8, 12))
            # lwarx/stwcx. need a word-aligned EA (else alignment, PAGE 119): 1 in 8 stays misaligned.
            ea = self.ea_in_window(24, 4 if (shape == 2 and rng.random() < 0.875) else 1)
            st["gpr"][base] = ea
            if shape == 0:      # store then overlapping narrower loads
                s = rng.choice((3, 4, 5, 9))
                words = [E.d_form(36, s, base, 0), E.d_form(34, 10, base, rng.randrange(4)),
                         E.d_form(40, 11, base, rng.randrange(3)), E.d_form(42, 12 if base != 12 else 13, base, 1)]
                names = ["stw", "lbz", "lhz", "lha"]
            elif shape == 1:    # update chain: stwu / lwzu walking the window
                s = rng.choice((3, 4, 5))
                words = [E.d_form(37, s, base, 4), E.d_form(37, base, base, 4), E.d_form(33, 10, base, -4),
                         E.d_form(39, s, base, 1), E.d_form(35, 11, base, -1)]
                names = ["stwu", "stwu", "lwzu", "stbu", "lbzu"]
            elif shape == 2:    # lwarx / stwcx. / stwcx. (second fails: reservation consumed)
                s = rng.choice((3, 4, 5))
                idx = 9
                st["gpr"][idx] = 0
                words = [E.x_form(31, 10, base, idx, 20), E.x_form(31, s, base, idx, 150, 1),
                         E.x_form(31, s, base, idx, 150, 1), E.x_form(31, 11, base, idx, 23)]
                names = ["lwarx", "stwcx.", "stwcx.", "lwzx"]
            else:               # byte-reversed store then plain load, sthbrx then lhz
                s = rng.choice((3, 4, 5))
                idx = 9
                st["gpr"][idx] = 0
                words = [E.x_form(31, s, base, idx, 662), E.d_form(32, 10, base, 0),
                         E.x_form(31, s, base, idx, 918), E.d_form(40, 11, base, 0)]
                names = ["stwbrx", "lwz", "sthbrx", "lhz"]
            mem = self.mem_around(ea, 16, 16, 16)
            self.with_prefixes_and_entries("lsseq", words, st, mem, "compare", "", names)

    # ================================================================================
    # branches
    # ================================================================================
    BO_ALL = list(range(32))

    def bo_bi(self):
        rng = self.rng
        bo = rng.choice(self.BO_ALL + [4, 12, 20, 16, 18, 0, 2, 8, 10] * 2)
        bi = rng.randrange(32)
        return bo, bi

    def bo_valid_for_ctr_loop(self, bo):
        return (bo & 0x04) == 0

    def add_br(self, rows, entry, st, note, spec="compare", reason="", mnemonic=None, mem=None, max_steps=4096):
        return self.add("br", rows, entry, st, note, mem=mem, spec=spec, reason=reason, mnemonic=mnemonic,
                        max_steps=max_steps)

    def fam_br(self, count):
        rng = self.rng
        producers = (
            lambda crf, a, b: (E.cmpw(crf, a, b), "cmp"),
            lambda crf, a, b: (E.cmplw(crf, a, b), "cmpl"),
            lambda crf, a, b: (E.cmp_imm(11, crf, a, self.imm16()), "cmpi"),
            lambda crf, a, b: (E.cmp_imm(10, crf, a, self.imm16()), "cmpli"),
            lambda crf, a, b: (E.xo_form(3, a, b, 266, rng.randrange(2), 1), "add."),
            lambda crf, a, b: (E.x_form(31, a, 3, b, 444, 1), "or."),
            lambda crf, a, b: (E.m_form(21, a, 3, rng.randrange(32), rng.randrange(32), rng.randrange(32), 1), "rlwinm."),
            lambda crf, a, b: (E.d_form(28, a, 3, self.imm16()), "andi."),
            lambda crf, a, b: (E.d_form(13, 3, a, self.imm16()), "addic."),
        )
        for i in range(count):
            shape = i % 13
            st = self.base_state()
            if shape == 0:      # forward bc, all BO/BI, CTR variants
                bo, bi = self.bo_bi()
                A = self.alloc(6)
                words = [E.bc(bo, bi, A, A + 12), E.addi(3, 3, 1), E.addi(3, 3, 2), E.addi(4, 4, 1), E.BLR]
                self.put_row(A, words, "br_fwd")
                st["ctr"] = rng.choice((0, 1, 2, 0xFFFFFFFF, rng.getrandbits(32)))
                self.add_br([A], A, st, f"bc {bo},{bi} fwd", mnemonic="bc")
            elif shape == 1:    # CR fusion adjacency: producer; bc on same / other field
                crf = rng.randrange(8)
                a, b = rng.choice((4, 5, 6)), rng.choice((5, 6, 7))
                word, pname = rng.choice(producers)(crf, a, b)
                field = 0 if pname.endswith(".") else crf
                bit = rng.randrange(4)
                bi = (field * 4 + bit) if rng.random() < 0.75 else rng.randrange(32)
                bo = rng.choice((12, 4, 12, 4, 13, 5, 0, 2, 8, 10, 16, 18))
                A = self.alloc(7)
                words = [word, E.bc(bo, bi, A + 4, A + 16), E.addi(3, 3, 1), E.addi(3, 3, 2),
                         E.addi(8, 8, 1), E.BLR]
                self.put_row(A, words, "br_fuse")
                if rng.random() < 0.5:
                    st["gpr"][b] = st["gpr"][a]                     # EQ
                st["ctr"] = rng.choice((1, 2, rng.getrandbits(32)))
                v = self.add_br([A], A, st, f"{pname} ; bc {bo},{bi}", mnemonic=f"{pname},bc")
                # the same pair reached through the cold path: entry at the bc (no fusion there)
                self.add("entry", [A], A + 4, json.loads(json.dumps(st)), f"cold entry at bc of {v['id']}",
                         parent=v["id"], mnemonic="bc")
            elif shape == 2:    # conditional bclr / bclrl-free return
                bo, bi = self.bo_bi()
                A = self.alloc(4)
                words = [E.bclr(bo, bi), E.addi(3, 3, 1), E.BLR]
                self.put_row(A, words, "br_bclr")
                st["ctr"] = rng.choice((0, 1, 2, rng.getrandbits(32)))
                self.add_br([A], A, st, f"bclr {bo},{bi}", mnemonic="bclr")
            elif shape == 3:    # CTR loop with bdnz / bdz / bdnzt / bdnzf back edge
                bo = rng.choice((16, 18, 0, 2, 8, 10, 17, 25))
                bi = rng.randrange(32)
                A = self.alloc(4)
                words = [E.addi(4, 4, 1), E.bc(bo, bi, A + 4, A), E.BLR]
                self.put_row(A, words, "br_ctrloop")
                st["ctr"] = rng.randrange(1, 40)
                self.add_br([A], A, st, f"loop bc {bo},{bi} ctr={st['ctr']}", mnemonic="bc", max_steps=200)
            elif shape == 4:    # compare loop: addi; cmplw; blt head (fused back edge)
                A = self.alloc(5)
                words = [E.addi(3, 3, 1), E.cmplw(0, 3, 5), E.bc(12, 0, A + 8, A), E.BLR]
                self.put_row(A, words, "br_cmploop")
                st["gpr"][3] = rng.getrandbits(31)
                st["gpr"][5] = st["gpr"][3] + rng.randrange(1, 30)
                self.add_br([A], A, st, "cmp loop", mnemonic="bc", max_steps=200)
            elif shape == 5:    # bl / bcl to a callee row, LR saved in r30
                lk_cond = rng.random() < 0.4
                C = self.alloc(3)
                self.put_row(C, [E.addi(5, 5, 7), E.m_form(21, 5, 6, 3, 0, 31), E.BLR], "br_callee")
                A = self.alloc(5)
                if lk_cond:
                    bo, bi = self.bo_bi()
                    call = E.bc(bo, bi, A + 4, C, lk=1)
                    note = f"bcl {bo},{bi}"
                else:
                    call = E.b(A + 4, C, lk=1)
                    note = "bl"
                words = [E.MFLR(30), call, E.MTLR(30), E.addi(9, 9, 1), E.BLR]
                self.put_row(A, words, "br_call")
                st["ctr"] = rng.choice((0, 1, 2, rng.getrandbits(32)))
                self.add_br([A, C], A, st, note, mnemonic="b" if not lk_cond else "bc")
            elif shape == 6:    # bcctr forward to a label that is also a direct-branch target
                bo = rng.choice((20, 20, 12, 4, 21))
                bi = rng.randrange(32)
                A = self.alloc(10)
                target = A + 28
                words = E.load_address(12, target) + [E.MTCTR(12), E.bcctr(bo, bi), E.addi(3, 3, 1),
                                                       E.b(A + 20, target), E.addi(3, 3, 2),
                                                       E.addi(4, 4, 1), E.BLR]
                self.put_row(A, words, "br_bcctr_fwd")
                self.add_br([A], A, st, f"bcctr {bo},{bi} forward label", mnemonic="bcctr")
            elif shape == 7:    # bcctr backward switch loop (+ a gap-sweep-recovered `b head`)
                A = self.alloc(9)
                head = A + 12
                words = E.load_address(12, head) + [E.MTCTR(12), E.addi(4, 4, 1), E.cmplw(0, 4, 5),
                                                     E.bc(4, 0, A + 20, A + 32), E.bcctr(20, 0),
                                                     E.b(A + 28, head), E.BLR]
                self.put_row(A, words, "br_bcctr_back")
                st["gpr"][4] = rng.getrandbits(31)
                st["gpr"][5] = st["gpr"][4] + rng.randrange(1, 20)
                self.add_br([A], A, st, "bcctr backward loop", mnemonic="bcctr", max_steps=400)
            elif shape == 8:    # bcctrl / bclrl into a callee row
                C = self.alloc(3)
                self.put_row(C, [E.addi(5, 5, 3), E.x_form(31, 5, 7, 5, 316), E.BLR], "br_icallee")
                A = self.alloc(10)
                bo, bi = rng.choice(((20, 0), (20, 0), (12, rng.randrange(32)), (4, rng.randrange(32)),
                                     (16, rng.randrange(32))))
                if rng.random() < 0.5:
                    if bo & 4 == 0:
                        bo |= 4       # bcctrl with CTR decrement is an invalid form: kept valid here
                    words = [E.MFLR(30)] + E.load_address(12, C) + [E.MTCTR(12), E.bcctr(bo, bi, 1),
                                                                    E.MFLR(29), E.MTLR(30), E.addi(9, 9, 1), E.BLR]
                    note = f"bcctrl {bo},{bi}"
                    mn = "bcctr"
                else:
                    words = [E.MFLR(30)] + E.load_address(12, C) + [E.MTLR(12), E.bclr(bo, bi, 1),
                                                                    E.MFLR(29), E.MTLR(30), E.addi(9, 9, 1), E.BLR]
                    note = f"bclrl {bo},{bi}"
                    mn = "bclr"
                    st["ctr"] = rng.choice((0, 1, 2, rng.getrandbits(32)))
                self.put_row(A, words, "br_icall")
                self.add_br([A, C], A, st, note, mnemonic=mn)
            elif shape == 9:    # cross-record tail jump forward and a backward cross-record loop
                B = self.alloc(3)
                A = self.alloc(4)
                if rng.random() < 0.5:
                    self.put_row(B, [E.addi(4, 4, 1), E.BLR], "br_tail_b")
                    self.put_row(A, [E.addi(3, 3, 1), E.b(A + 4, B)], "br_tail_a")
                    self.add_br([B, A], A, st, "b to lower row (tail)", mnemonic="b")
                else:
                    # B: head: addi r4,1 ; cmplw r4,r5 ; bge exit ; b A ; exit: blr   A: addi r3,1 ; b B
                    self.put_row(B, [E.addi(4, 4, 1), E.cmplw(0, 4, 5), E.bc(4, 0, B + 8, B + 16),
                                     E.b(B + 12, A), E.BLR], "br_xloop_b")
                    self.put_row(A, [E.addi(3, 3, 1), E.b(A + 4, B)], "br_xloop_a")
                    st["gpr"][4] = rng.getrandbits(31)
                    st["gpr"][5] = st["gpr"][4] + rng.randrange(1, 20)
                    self.add_br([B, A], B, st, "cross-record loop", mnemonic="b", max_steps=400)
            elif shape == 10:   # CR logical / mcrf feeding a branch, fusion must not apply
                A = self.alloc(6)
                xo = rng.choice((257, 129, 289, 225, 33, 449, 417, 193))
                crbd = rng.randrange(32)
                words = [E.cr_logical(xo, crbd, rng.randrange(32), rng.randrange(32)),
                         E.bc(rng.choice((12, 4)), crbd, A + 4, A + 16), E.addi(3, 3, 1), E.addi(3, 3, 2),
                         E.addi(4, 4, 1), E.BLR]
                self.put_row(A, words, "br_crlogic")
                self.add_br([A], A, st, "crlogical ; bc", mnemonic="bc")
            elif shape == 12:   # a fusable compare; bc pair whose bc is also reached by an earlier branch
                crf = rng.randrange(8)
                word, pname = rng.choice(producers[:4])(crf, 4, 5)
                A = self.alloc(9)
                bo = rng.choice((12, 4))
                bi = crf * 4 + rng.randrange(3)
                words = [E.bc(rng.choice((12, 4)), rng.randrange(32), A, A + 12), E.addi(3, 3, 1), word,
                         E.bc(bo, bi, A + 12, A + 24), E.addi(3, 3, 2), E.addi(3, 3, 4), E.addi(6, 6, 1), E.BLR]
                self.put_row(A, words, "br_fuse_label")
                if rng.random() < 0.5:
                    st["gpr"][5] = st["gpr"][4]
                self.add_br([A], A, st, f"bc ->label ; {pname} ; bc {bo},{bi}", mnemonic=f"bc,{pname},bc")
            else:               # unconditional b forward inside the row, and bcctr with BO[2]=0 (invalid)
                A = self.alloc(6)
                if rng.random() < 0.7:
                    words = [E.b(A, A + 12), E.addi(3, 3, 1), E.addi(3, 3, 2), E.addi(4, 4, 1), E.BLR]
                    self.put_row(A, words, "br_bfwd")
                    self.add_br([A], A, st, "b forward", mnemonic="b")
                else:
                    target = A + 16
                    words = E.load_address(12, target) + [E.MTCTR(12), E.bcctr(rng.choice((0, 2, 8, 16)), 0),
                                                           E.addi(4, 4, 1), E.BLR]
                    self.put_row(A, words, "br_bcctr_invalid")
                    self.add_br([A], A, st, "bcctr BO[2]=0", spec="engine-only",
                                reason="invalid form: bcctr with BO[2]=0 (PAGE 362)", mnemonic="bcctr")

    # ================================================================================
    # invalid forms (engine vs engine only)
    # ================================================================================
    def fam_invalid(self, count):
        rng = self.rng
        for _ in range(count):
            which = rng.randrange(4)
            if which == 0:
                word = E.cmp_reg(rng.choice((0, 32)), rng.randrange(8), self.reg(), self.reg(), l_bit=1)
                why = "cmp/cmpl with L=1 (PAGE 365/367)"
            elif which == 1:
                word = E.cmp_imm(rng.choice((10, 11)), rng.randrange(8), self.reg(), self.imm16(), l_bit=1)
                why = "cmpi/cmpli with L=1 (PAGE 366/368)"
            elif which == 2:
                xo = rng.choice((104, 202, 200, 234, 232))
                word = E.xo_form(self.reg(), self.reg(), rng.randrange(1, 32), xo, rng.randrange(2), rng.randrange(2))
                why = "unary XO-form with nonzero rB field (reserved, PAGE 87)"
            else:
                word = E.x_form(31, self.reg(), self.reg(), rng.randrange(1, 32), rng.choice((26, 922, 954)),
                                rng.randrange(2))
                why = "cntlzw/extsh/extsb with nonzero rB field (reserved, PAGE 87)"
            row = self.row_pic([word, E.BLR], "invalid")
            st = self.base_state()
            self.add("invalid", [row], row, st, f"{word:08X}", spec="engine-only", reason="invalid form: " + why)

    # ================================================================================
    # string load/store: lswi / lswx / stswi / stswx (repair L0r0-03)
    # ================================================================================
    # Manual PAGE 448 (lswi), PAGE 449 (lswx), PAGE 561 (stswi), PAGE 562 (stswx). n bytes move between
    # EA and GPRs first, first+1, ... (mod 32), left to right, CEIL(n/4) registers; a loaded register
    # is cleared when its first byte loads. lswi/stswi: n = NB, 32 when NB = 0, EA = (rA|0).
    # lswx/stswx: n = XER[25-31], EA = (rA|0) + (rB).
    STR_N = (0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 100, 121, 124, 127)

    @staticmethod
    def string_regs(first: int, n: int) -> list:
        return [(first + k) % 32 for k in range((n + 3) // 4)]

    def set_ea_x(self, st, a: int, b: int, ea: int):
        """EA = (rA|0) + (rB); the caller aligned ea to 2 when rA == rB != 0."""
        if a == 0:
            st["gpr"][b] = ea
        elif a == b:
            st["gpr"][a] = (ea // 2) & M32
        else:
            st["gpr"][a] = self.rng.getrandbits(32)
            st["gpr"][b] = (ea - st["gpr"][a]) & M32

    def str_word(self, st, avoid=(), valid_only=False, kind=None):
        """One string instruction with a data-window EA. Registers in `avoid` are never rA/rB and never
        loaded. Returns (word, name, mem, spec, reason); st (gpr, xer) is updated for the EA and count."""
        rng = self.rng
        kind = rng.randrange(4) if kind is None else kind
        want_invalid = kind in (0, 1) and not valid_only and rng.random() < 0.12
        pool = [r for r in range(1, 32) if r not in avoid]
        for _attempt in range(1000):
            if kind in (0, 2):
                nb = rng.choice((0, 1, 2, 3, 4, 5, 7, 8, 12, 16, 31, rng.randrange(32)))
                n = 32 if nb == 0 else nb
            else:
                nb = None
                n = rng.choice(self.STR_N + (rng.randrange(128),))
            first = rng.choice((0, 1, 29, 30, 31, rng.randrange(32)))
            regs = self.string_regs(first, n)
            if kind in (0, 1) and any(r in avoid for r in regs):
                continue
            if kind == 0:                                   # lswi: invalid if rA = 0 or rA in range
                # rA = 0 (EA 0) is not generated: EA 0 has no BAT under the reset MSR[DR]=1 state and
                # would latch a data-access fault; the invalid lane uses rA inside the loaded range.
                a = rng.choice(([r for r in regs if r != 0 and r not in avoid] or [None]) if want_invalid else
                               ([r for r in pool if r not in regs] or [None]))
                if a is None:
                    continue
                b = None
                invalid = a == 0 or a in regs
            elif kind == 1:                                 # lswx: PAGE 449 invalid / undefined forms
                if want_invalid:
                    a = rng.choice([0] + pool + regs)
                    b = rng.choice(pool + regs)
                else:
                    free = [r for r in pool if r not in regs]
                    if not free:
                        continue
                    a = rng.choice(free)
                    b = a if len(free) == 1 or rng.random() < 0.15 else rng.choice(free)
                if a in avoid or b in avoid:
                    continue
                invalid = a == 0 or a in regs or b in regs or first in (a, b)
            elif kind == 2:                                 # stswi: no invalid form; rA = 0 would store at 0
                a = rng.choice(pool)
                b = None
                invalid = False
            else:                                           # stswx: rA = 0 legal (EA = rB)
                a = rng.choice([0] + pool)
                b = rng.choice(pool)
                invalid = False
            if invalid != want_invalid:
                continue
            break
        else:
            raise RuntimeError("str_word: no operand assignment satisfies the constraints")
        name, xo = (("lswi", 597), ("lswx", 533), ("stswi", 725), ("stswx", 661))[kind]
        width = max(n, 1)
        align = 4 if rng.random() < 0.6 else 1
        if b is not None and a == b and a != 0:
            align = max(align, 2)
        ea = self.ea_in_window(width, align)
        if b is None:
            st["gpr"][a] = ea
            mem = self.mem_around(ea, width)
            word = E.x_form(31, first, a, nb, xo)
        else:
            self.set_ea_x(st, a, b, ea)
            st["xer"] = (st["xer"] & 0xE0000000) | n
            mem = self.mem_around(ea, width)
            word = E.x_form(31, first, a, b, xo)
        spec, reason = "compare", ""
        if invalid:
            spec = "engine-only"
            reason = (f"invalid form: {name} with rA=0 or rA/rB in the loaded range or rD=rA/rB "
                      f"(PAGE {448 if kind == 0 else 449})")
        return word, name, mem, spec, reason

    def fam_str1(self, count):
        for i in range(count):
            st = self.base_state()
            word, name, mem, spec, reason = self.str_word(st, kind=i % 4)
            row = self.row_pic([word, E.BLR], f"str1_{name}")
            self.add("str1", [row], row, st, f"{word:08X}", mem=mem, spec=spec, reason=reason,
                     mnemonic=name, expect_mnemonic=name)

    def fam_strseq(self, count):
        rng = self.rng
        for i in range(count):
            st = self.base_state()
            shape = i % 3
            ea = self.ea_in_window(48, 4 if rng.random() < 0.7 else 1)
            if shape == 0:      # stswi rS -> EA, lswi EA -> rD (range never holds r20), lwz observes the EA
                st["gpr"][20] = ea
                s0 = rng.choice((3, 5, 29, 31, rng.randrange(32)))
                d0 = rng.choice((8, 12, 24))
                words = [E.x_form(31, s0, 20, rng.randrange(32), 725), E.x_form(31, d0, 20, rng.randrange(32), 597),
                         E.addi(3, 3, 1), E.d_form(32, 4, 20, 0)]
                names = ["stswi", "lswi", "addi", "lwz"]
            elif shape == 1:    # stswx / lswx sharing XER n, EA = r20 + r21
                n = rng.choice((0, 1, 3, 4, 7, 8, 13, 16, 25, 32, 40))
                st["xer"] = (st["xer"] & 0xE0000000) | n
                off = rng.randrange(0, 16)
                st["gpr"][20] = (ea - off) & M32
                st["gpr"][21] = off
                s0 = rng.choice((0, 3, 5, 26, 31))
                d0 = rng.choice((8, 22))
                words = [E.x_form(31, s0, 20, 21, 661), E.x_form(31, d0, 20, 21, 533), E.addi(3, 3, 1)]
                names = ["stswx", "lswx", "addi"]
            else:               # lswi into r30, r31, r0 (wrap), stswi the same three back 16+ bytes later
                st["gpr"][20] = ea
                st["gpr"][21] = (ea + 16 + rng.randrange(0, 8)) & M32
                words = [E.x_form(31, 30, 20, 12, 597), E.x_form(31, 30, 21, 12, 725), E.d_form(34, 5, 21, 11)]
                names = ["lswi", "stswi", "lbz"]
            mem = self.mem_around(ea, 48, 8, 8)
            self.with_prefixes_and_entries("strseq", words, st, mem, "compare", "", names)

    def fam_strcall(self, count):
        rng = self.rng
        for i in range(count):
            if i % 2 == 0:      # caller: mflr r30 ; bl S ; mtlr r30 ; addi r9,r9,1 ; blr   S: <string> ; blr
                st = self.base_state()
                word, name, mem, spec, reason = self.str_word(st, avoid=(9, 30))
                callee = self.row_pic([word, E.BLR], f"strcall_callee_{name}")
                caller = self.alloc(5)
                self.put_row(caller, [E.MFLR(30), E.b(caller + 4, callee, lk=1), E.MTLR(30), E.addi(9, 9, 1),
                                      E.BLR], "strcall_caller")
                self.add("strcall", [caller, callee], caller, st, f"bl {name} {word:08X}", mem=mem, spec=spec,
                         reason=reason, mnemonic=f"b,{name}")
            else:               # R: cmpwi r3,0 ; beq +12 ; addi r3,r3,1 ; blr ; <string> ; blr   F: mflr ; bl R ; mtlr ; blr
                base = self.base_state()
                word, name, mem, spec, reason = self.str_word(base, avoid=(3, 30))
                rare = self.alloc(6)
                self.put_row(rare, [E.cmp_imm(11, 0, 3, 0), E.bc(12, 2, rare + 4, rare + 16), E.addi(3, 3, 1),
                                    E.BLR, word, E.BLR], "strcall_rare")
                caller = self.alloc(4)
                self.put_row(caller, [E.MFLR(30), E.b(caller + 4, rare, lk=1), E.MTLR(30), E.BLR],
                             "strcall_rare_caller")
                for entry, rows in ((rare, [rare]), (caller, [caller, rare])):
                    for r3 in (0, rng.choice((1, 5, 0x80000000, 0xFFFFFFFF))):
                        st = json.loads(json.dumps(base))
                        st["gpr"][3] = r3
                        path = "rare string path" if r3 == 0 else "common path"
                        self.add("strcall", rows, entry, st, f"{path} {name} {word:08X}", mem=mem,
                                 spec=spec if r3 == 0 else "compare", reason=reason if r3 == 0 else "",
                                 mnemonic=name if r3 == 0 else "cmpi,bc,addi")


# ------------------------------------------------------------------------------------
# outputs
# ------------------------------------------------------------------------------------
def write_dol(path: Path, gen: Gen) -> tuple:
    end = gen.next_slot
    size = end - CODE_BASE
    image = bytearray(0x100 + size)
    struct.pack_into(">I", image, 0x00, 0x100)          # text0 file offset
    struct.pack_into(">I", image, 0x48, CODE_BASE)      # text0 address
    struct.pack_into(">I", image, 0x90, size)           # text0 size
    struct.pack_into(">I", image, 0xE0, CODE_BASE)      # entry
    for address, row in gen.rows.items():
        for i, w in enumerate(row["words"]):
            struct.pack_into(">I", image, 0x100 + address - CODE_BASE + 4 * i, w)
    path.write_bytes(bytes(image))
    return CODE_BASE, size


def spec_mnemonic_check(gen: Gen, spec_dir: Path) -> list:
    """The blind spec decoder must name every spec-compared single-instruction word as intended."""
    sys.path.insert(0, str(spec_dir))
    import int as spec_int          # noqa: E402
    import branch as spec_branch    # noqa: E402
    import loadstore as spec_ls     # noqa: E402
    problems = []
    for v in gen.vectors:
        if v["expect_mnemonic"] is None:
            continue
        word = int(v["note"].split()[0], 16)
        names = [m.mnemonic(word) for m in (spec_int, spec_branch, spec_ls) if m.supports(word)]
        base = v["expect_mnemonic"].rstrip(".")
        base = {"addic": "addic"}.get(base, base)
        ok = any(n is not None and n.rstrip(".") == base for n in names)
        if not ok:
            problems.append(f"{v['id']}: word {word:08X} intended {v['expect_mnemonic']} spec decodes {names}")
    return problems


def write_driver_vectors(path: Path, gen: Gen):
    lines = ["# gekko difftest vectors v1 (generated by tools/difftest/gen_vectors.py)"]
    for address in sorted(gen.rows):
        words = gen.rows[address]["words"]
        lines.append("code %08X %s" % (address, " ".join(f"{w:08X}" for w in words)))
    for v in gen.vectors:
        st = v["state"]
        lines.append(f"vector {v['id']}")
        lines.append("entry %08X" % v["entry"])
        lines.append("gpr " + " ".join(f"{x:08X}" for x in st["gpr"]))
        lines.append("fpr " + " ".join(f"{p[0]:016X} {p[1]:016X}" for p in st["fpr"]))
        for key in ("cr", "xer", "lr", "ctr", "msr", "fpscr", "dec"):
            lines.append("%s %08X" % (key, st[key]))
        lines.append("tb %016X" % st["tb"])
        lines.append("residue %d" % st["residue"])
        for number, value in sorted(st["spr"].items()):
            lines.append("spr %d %08X" % (int(number), value))
        lines.append("reserve %d %08X" % (1 if st["reserve"] else 0, st["reserve_addr"]))
        for ea, hexbytes in v["mem"]:
            lines.append("mem %08X %s" % (ea, hexbytes))
        lines.append("max %d" % v["max_steps"])
        lines.append("end")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_generated(out: Path) -> tuple:
    table = (out / "recomp_table.cpp").read_text(encoding="utf-8")
    functions = set(int(m.group(1), 16) for m in re.finditer(r"\{0x([0-9A-F]+)u, 0x[0-9A-F]+u, &fn_", table))
    ranges = [(int(a, 16), int(b, 16)) for a, b in
              re.findall(r"\{0x([0-9A-F]+)u, 0x([0-9A-F]+)u, &fn_[0-9A-F]+_at\}", table)]
    return functions, ranges


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--generator", type=Path, required=True, help="ppcrecomp executable")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20260914)
    parser.add_argument("--scale", type=float, default=1.0, help="multiplies every family count")
    parser.add_argument("--shards", type=int, default=4)
    parser.add_argument("--spec", type=Path, default=None, help="spec model directory (encoder cross-check)")
    args = parser.parse_args()

    out = args.out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    gen = Gen(args.seed, args.scale)

    def n(base):
        return max(1, int(round(base * args.scale)))

    gen.fam_int1(n(700))
    gen.fam_spr1(n(60))
    gen.fam_clock(n(120))
    gen.fam_ls1(n(450))
    gen.fam_alias(n(3))
    gen.fam_hint()
    gen.fam_seq(n(60))
    gen.fam_lsseq(n(40))
    gen.fam_br(n(300))
    gen.fam_invalid(n(40))
    # String families last: the families above keep their exact vectors (known-candidate fingerprints).
    gen.fam_str1(n(240))
    gen.fam_strseq(n(30))
    gen.fam_strcall(n(40))

    problems = spec_mnemonic_check(gen, args.spec) if args.spec else []
    if problems:
        print("encoder/spec decoder disagreement:\n  " + "\n  ".join(problems[:40]), file=sys.stderr)
        return 3

    dol = out / "diff.dol"
    write_dol(dol, gen)
    with (out / "functions.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("addr_hex", "addr", "name", "size", "ins", "tags", "strings", "callees"))
        for address in sorted(gen.rows):
            row = gen.rows[address]
            writer.writerow((hex(address), address, f"{row['name']}_{address:08X}", len(row["words"]) * 4,
                             len(row["words"]), "", "", ""))

    # Production arguments (CMakeLists.txt SHADOW_RECOMP_ARGS): no --no-gap-sweep, no --no-wide-entries.
    command = [str(args.generator.resolve()), "--dol", str(dol), "--functions", str(out / "functions.csv"),
               "--out", str(out), "--shards", str(args.shards), "--report", str(out / "recomp_report.txt")]
    result = subprocess.run(command, capture_output=True, text=True)
    (out / "ppcrecomp.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
        return result.returncode

    functions, ranges = parse_generated(out)
    rejected_rows = sorted(a for a in gen.rows if a not in functions)
    report = (out / "recomp_report.txt").read_text(encoding="utf-8")
    if rejected_rows:
        print("ppcrecomp rejected rows the harness expects to be translated:", file=sys.stderr)
        for a in rejected_rows[:40]:
            print(f"  {a:08X} {gen.rows[a]['name']} {' '.join(f'{w:08X}' for w in gen.rows[a]['words'])}",
                  file=sys.stderr)
        print(report, file=sys.stderr)
        return 4

    def covered(address):
        return any(lo <= address < hi for lo, hi in ranges)

    not_dispatchable = [v["id"] for v in gen.vectors if v["entry"] not in functions and not covered(v["entry"])]
    if not_dispatchable:
        print("vector entries with no dispatch path: " + ", ".join(not_dispatchable[:20]), file=sys.stderr)
        return 5

    shards = [out / f"recomp_{i:03d}.cpp" for i in range(args.shards)]
    for shard in shards:
        if not shard.exists():
            print(f"missing generated shard {shard}", file=sys.stderr)
            return 6

    with (out / "vectors.jsonl").open("w", encoding="utf-8") as stream:
        for v in gen.vectors:
            row_words = {f"{a:08X}": gen.rows[a]["words"] for a in v["rows"]}
            record = dict(v)
            record["row_words"] = row_words
            record["entry_declared"] = v["entry"] in functions
            stream.write(json.dumps(record, sort_keys=True) + "\n")
    write_driver_vectors(out / "vectors.txt", gen)

    families = {}
    for v in gen.vectors:
        key = (v["family"], v["spec"])
        families[f"{key[0]}/{key[1]}"] = families.get(f"{key[0]}/{key[1]}", 0) + 1
    manifest = {
        "schema": SCHEMA,
        "generator": str(args.generator.resolve()),
        "generator_sha256": hashlib.sha256(args.generator.read_bytes()).hexdigest(),
        "generator_command": command,
        "seed": args.seed, "scale": args.scale, "shards": args.shards,
        "rows": len(gen.rows), "vectors": len(gen.vectors), "families": families,
        "code_bytes": gen.next_slot - CODE_BASE,
        "declared_entries": sum(1 for v in gen.vectors if v["entry"] in functions),
        "report": report,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    print(f"difftest: {len(gen.vectors)} vectors over {len(gen.rows)} rows, {args.shards} shards -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
