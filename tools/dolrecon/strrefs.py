"""String references through CodeWarrior string pools.

CodeWarrior collects a unit's literals into one data object (dtk names it lbl_<address>; the CRI error messages of a
file, for instance) and reaches each literal as the pool's address plus an offset held in a register:
    lis r3, pool@ha ; addi r31, r3, pool@l ; ... ; addi r3, r31, 0x24 ; bl OSReport
A relocation only names the pool, so a symbol-based scan sees no string at all (or, reading at the pool's address,
always its first string). This module follows the register values instead.

pools(image, syms)            string start address -> text, for every data object made of NUL-separated printable
                              strings (>= 90 % of its bytes; strings of >= 4 characters)
pooled_refs(image, funcs, pool_text, sda)
                              function -> [(site, string address)]: the values lis / addis / addi / ori / mr compute
                              from constants, r2 / r13 (the small-data bases) and each other, by dolrecon.vcalls'
                              must-analysis over the function's blocks (a value holds at an instruction only when it
                              does on every path; every GPR write decoded by dolrecon.ppc). A value that is a string
                              start counts only when USED as one there: passed to a call (r3..r10 at a linked branch,
                              or a tail call), stored (a GPR store of it), or returned (r3 at a return); site = that
                              use. Used as a base (of a further offset, or of a load / store) it is not: the pool's own
                              address, used to reach other strings or data, is no use of its first string. Until
                              2026-11-01 this was a linear scan whose branch targets restored the fall-through path's
                              values (Astra answer 5) and lost in-place `ori r3,r3,lo` and `mr r3,r3`.
sda_bases(image, text_start, text_end)  (r2, r13) from the startup code's lis / ori pairs.
"""
from __future__ import annotations

import bisect
import os
import re

PRINTABLE = set(range(0x20, 0x7F)) | {0x09, 0x0A, 0x0D}
SIZE = re.compile(r"size:0x([0-9A-Fa-f]+)")


SYM = re.compile(r"^(\S+) = ([^:]+):0x([0-9A-Fa-f]+);(.*)$")


def project_refs(project_dir: str):
    """(pool texts, {function: [(site, string address)]}, pool objects holding several strings) of a dtk project."""
    from . import dol as dolmod
    image = dolmod.load_file(os.path.join(project_dir, "main.dol"))
    syms = []
    with open(os.path.join(project_dir, "config", "symbols.txt"), encoding="utf-8") as f:
        for line in f:
            m = SYM.match(line.rstrip("\r\n"))
            syms.append([m.group(1), m.group(2), int(m.group(3), 16), m.group(4)] if m else None)
    funcs = [(s[2], int(SIZE.search(s[3]).group(1), 16)) for s in syms
             if s and "type:function" in s[3] and SIZE.search(s[3])]
    text = pools(image, syms)
    starts = sorted(text)
    multi = set()
    for s in syms:
        if s and "type:function" not in s[3] and SIZE.search(s[3]):
            size = int(SIZE.search(s[3]).group(1), 16)
            i = bisect.bisect_left(starts, s[2])
            if i + 1 < len(starts) and starts[i + 1] < s[2] + size:
                multi.add(s[0])
    sec = next(x for x in image.sections if x.kind == "text")
    return text, pooled_refs(image, funcs, text, sda_bases(image, sec.address, sec.address + sec.size)), multi


def pools(image, syms) -> dict[int, str]:
    out = {}
    for s in syms:
        if not s or "type:function" in s[3] or s[1] not in (".rodata", ".data", ".sdata", ".sdata2"):
            continue
        m = SIZE.search(s[3])
        if not m:
            continue
        size = int(m.group(1), 16)
        try:
            blob = image.read(s[2], size)
        except ValueError:
            continue
        runs, covered, i = [], 0, 0
        while i < size:
            j = blob.find(b"\0", i)
            j = size if j < 0 else j
            run = blob[i:j]
            if run and all(b in PRINTABLE for b in run):
                covered += len(run)
                if len(run) >= 4:
                    runs.append((s[2] + i, run.decode("ascii")))
            covered += 1 if j < size else 0
            i = j + 1
        if runs and covered >= 0.9 * size:
            out.update(runs)
    return out


def sda_bases(image, start: int, end: int) -> tuple[int | None, int | None]:
    found = {}
    a = start
    while a + 4 < end and len(found) < 2:
        w, n = image.word(a), image.word(a + 4)
        if w >> 26 == 15 and (w >> 16) & 31 == 0 and n >> 26 == 24:
            rd, rs, ra = (w >> 21) & 31, (n >> 21) & 31, (n >> 16) & 31
            if rd in (2, 13) and rs == rd and ra == rd and rd not in found:
                found[rd] = ((w & 0xFFFF) << 16) | (n & 0xFFFF)
        a += 4
    return found.get(2), found.get(13)


GPR_STORES = set(range(36, 40)) | {44, 45}                  # stw stwu stb stbu sth sthu (D-form)
X_GPR_STORES = {151, 183, 215, 247, 407, 439, 662, 918}       # stwx stwux stbx stbux sthx sthux stwbrx sthbrx


def pooled_refs(image, funcs: list[tuple[int, int]], pool_text: dict[int, str], sda) -> dict[int, list]:
    from . import ppc, vcalls
    entry = {r: v for r, v in ((2, sda[0]), (13, sda[1])) if v is not None}
    vfuncs = [(a, "", size, ".text") for a, size in funcs]
    entries = vcalls.interior_entries(image, vfuncs)
    out = {}
    for start, size in funcs:
        if size < 4 or image.section_at_address(start) is None:
            continue
        fn = vcalls.Function(image, start, size, entries.get(start, ()))
        used = {}

        def use(pc, v):
            if isinstance(v, int) and v in pool_text and v not in used:
                used[v] = pc

        def visit(pc, w, regs, consts, lo=start, hi=start + size):
            op, rs, _, _, _ = ppc.fields(w)
            br = ppc.branch(w, pc)
            if br is not None:
                if br.link or br.kind == "b" and not lo <= br.target < hi:     # a call or a tail call
                    for r in range(3, 11):
                        use(pc, consts.get(r))
                elif br.kind == "bclr":                                         # a return: r3 is its value
                    use(pc, consts.get(3))
            elif op in GPR_STORES or op == 31 and (w >> 1) & 0x3FF in X_GPR_STORES:
                use(pc, consts.get(rs))
        fn.scan_values(entry, visit)
        if used:
            out[start] = sorted((pc, v) for v, pc in used.items())
    return out
