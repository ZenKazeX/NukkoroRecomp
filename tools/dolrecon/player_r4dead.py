"""Which function entries leave r4 dead at every caller (the --verify-player owner contract, cpu.cpp
register_differences): r4 is a return register only for a 64-bit result, so an owner whose callers all overwrite r4
before reading it is not compared on r4.

    python -B -m dolrecon.player_r4dead <dolrecon root> <callgraph.json> <scope in> <scope out>

Proof, per entry, from main.dol only:
  - no caller other than direct `bl` sites: no tail branch, address, data, vtable or resolved virtual reference to it
    in the call graph (an unknown caller could read r4);
  - at every `bl` site, scanning forward in straight-line code, r4 is written before it is read, and before any
    branch (a branch ends the proof for that site: not followed, so not proven).
Unknown instruction forms count as reading r4 when any register field is 4. Lines of <scope in> are copied to
<scope out>; proven entries' "address,size" lines get ",r4dead" and / or ",f1dead" (f1, the float return register:
the same proof over FPR reads and writes, following one direct call into its callee's entry path).
"""
from __future__ import annotations

import csv
import json
import pathlib
import sys

from dolrecon import dol, game_map

ARITH_D = {266, 40, 104, 235, 491, 459, 10, 138, 8, 136, 75, 11, 202, 200, 234, 23, 87, 279, 343, 55, 119, 311, 375,
           534, 790}                  # rD written; rA, rB read (adds, multiplies, divides, indexed loads)
LOGIC_A = {444, 28, 316, 124, 60, 412, 24, 536, 792, 26, 954, 922, 476, 284}  # rA written; rS, rB read
STORE_X = {151, 215, 407, 183, 247, 439, 662, 918, 150}                          # rS, rA, rB read


def r4_effect(w: int) -> str:
    """'read', 'write', 'branch' or 'none' for r4."""
    op = w >> 26
    d, a, b = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if op == 18 and (w & 1) and not (w & 2):
        return "call"
    if op in (16, 18, 19) or w == 0x44000002:
        return "branch"
    if op in (14, 15):  # addi / addis (rA = 0: li / lis)
        return "read" if a == 4 else ("write" if d == 4 else "none")
    if op in (32, 33, 34, 35, 40, 41, 42, 43):  # lwz lwzu lbz lbzu lhz lhzu lha lhau
        return "read" if a == 4 else ("write" if d == 4 else "none")
    if op in (48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 60, 61):  # FP loads / stores, psq: rA read only
        return "read" if a == 4 else "none"
    if op in (36, 37, 38, 39, 44, 45):  # stw stwu stb stbu sth sthu
        return "read" if 4 in (a, d) else "none"
    if op == 46:  # lmw rD..r31
        return "read" if a == 4 else ("write" if d <= 4 else "none")
    if op == 47:  # stmw rS..r31
        return "read" if a == 4 or d <= 4 else "none"
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29):  # rlwimi(reads rA) rlwinm rlwnm ori oris xori xoris andi. andis.
        if d == 4 or (op in (20, 23) and (a == 4 or (op == 23 and b == 4))):
            return "read"
        return "write" if a == 4 else "none"
    if op in (10, 11, 3):  # cmpli cmpi twi
        return "read" if a == 4 else "none"
    if op == 7 or op == 8 or op == 12 or op == 13:  # mulli subfic addic addic.
        return "read" if a == 4 else ("write" if d == 4 else "none")
    if op == 31:
        x = (w >> 1) & 1023
        if x in ARITH_D:
            return "read" if 4 in (a, b) else ("write" if d == 4 else "none")
        if x in LOGIC_A:
            srcs = (d,) if x in (26, 954, 922, 824) else (d, b)
            return "read" if 4 in srcs else ("write" if a == 4 else "none")
        if x == 824:  # srawi
            return "read" if d == 4 else ("write" if a == 4 else "none")
        if x in STORE_X or x in (0, 32):  # stores, cmp, cmpl
            return "read" if 4 in (a, b, d) else "none"
        if x in (339, 371):  # mfspr / mftb
            return "write" if d == 4 else "none"
        if x == 467:  # mtspr
            return "read" if d == 4 else "none"
    if op in (59, 63, 4):
        return "none"  # FPU: no GPR
    return "read" if 4 in (a, b, d) else "none"


def f1_effect(w: int) -> str:
    """'read', 'write', 'branch', 'call' or 'none' for FPR f1 (the float return register)."""
    op = w >> 26
    d, a, b, c = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    if op == 18 and (w & 1) and not (w & 2):
        return "call"
    if op in (16, 18, 19) or w == 0x44000002:
        return "branch"
    if op in (48, 49, 50, 51, 56, 57):
        return "write" if d == 1 else "none"
    if op in (52, 53, 54, 55, 60, 61):
        return "read" if d == 1 else "none"
    if op == 31:
        x = (w >> 1) & 1023
        if x in (535, 567, 599, 631):
            return "write" if d == 1 else "none"
        if x in (663, 695, 727, 759, 983):
            return "read" if d == 1 else "none"
        return "none"
    if op == 59:
        return "read" if 1 in (a, b, c) else ("write" if d == 1 else "none")
    if op == 63:
        x = (w >> 1) & 1023
        if x in (0, 32):
            return "read" if 1 in (a, b) else "none"
        if x in (72, 40, 264, 136, 12, 14, 15, 711):
            return "read" if b == 1 else ("write" if d == 1 and x != 711 else "none")
        if x == 583:
            return "write" if d == 1 else "none"
        if x in (38, 70, 134, 64):
            return "none"
        return "read" if 1 in (a, b, c) else ("write" if d == 1 else "none")
    if op == 4:
        x10, x6 = (w >> 1) & 1023, (w >> 1) & 63
        if x6 in (6, 38):
            return "write" if d == 1 else "none"
        if x6 in (7, 39):
            return "read" if d == 1 else "none"
        if x10 in (0, 32, 64, 96):
            return "read" if 1 in (a, b) else "none"
        return "read" if 1 in (a, b, c) else ("write" if d == 1 else "none")
    return "none"


def dead_after(image, site: int, effect, depth: int = 3, budget: int = 4000) -> bool:
    """Every path from the instruction after the call at `site` writes the register `effect` describes before it
    reads it. Followed: fall-through, in-range conditional and unconditional relative branches (both ways), direct
    calls into their callees and back at blr (`depth` levels). Not proven (False): an indirect branch or call, a return
    out of the calling function (the value would be its caller's), sc / rfi, or more than `budget` steps."""
    steps = 0
    visited = set()
    work = [(site + 4, ())]  # (pc, return stack)
    while work:
        pc, stack = work.pop()
        while True:
            if (pc, stack) in visited:
                break  # a loop or a joined path: what follows was checked already
            visited.add((pc, stack))
            steps += 1
            if steps > budget:
                return False
            w = image.word(pc)
            op = w >> 26
            e = effect(w)
            if e == "write":
                break
            if e == "read":
                return False
            if op == 18:  # b / bl (relative)
                if w & 2:
                    return False
                tgt = pc + (((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000)
                if w & 1:  # call
                    if len(stack) >= depth:
                        return False
                    stack = stack + (pc + 4,)
                pc = tgt
                continue
            if op == 16:  # bc
                if (w & 1) or (w & 2):
                    return False
                tgt = pc + (((w & 0xFFFC) ^ 0x8000) - 0x8000)
                if ((w >> 21) & 0x14) == 0x14:
                    pc = tgt
                    continue
                work.append((tgt, stack))
                pc += 4
                continue
            if op == 19:
                x = (w >> 1) & 1023
                if x == 16 and not (w & 1):  # bclr
                    if ((w >> 21) & 0x14) != 0x14:
                        return False  # conditional return: not modelled
                    if not stack:
                        return False  # returns out of the calling function
                    pc, stack = stack[-1], stack[:-1]
                    continue
                if x in (16, 528, 50):  # bclrl, bcctr(l), rfi
                    return False
            if w == 0x44000002:
                return False
            pc += 4
    return True


def proven(image, fn, edges_to, entry: int, sites: list[int], effect=r4_effect) -> bool:
    if any(k != "call" for k in edges_to.get(entry, ())) or not sites:
        return False
    return all(dead_after(image, site, effect) for site in sites)


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    root, cg_path, scope_in, scope_out = pathlib.Path(argv[0]), argv[1], argv[2], argv[3]
    image = dol.load_file(str(root / "pal-v30/main.dol"))
    fn = game_map.load_functions(root)
    n2a = {r["name"]: a for a, r in fn.items()}
    edges_to = {}
    for e in json.load(open(cg_path, encoding="utf-8"))["edges"]:
        t = n2a.get(e["to"])
        if t is not None:
            edges_to.setdefault(t, set()).add(e["kind"])
    lines = open(scope_in, encoding="utf-8").read().split()
    entries = {int(l.split(",")[0], 16) for l in lines}
    sites = {a: [] for a in entries}
    for a in sorted(fn):
        for k in range(0, int(fn[a]["size"]), 4):
            w = image.word(a + k)
            if (w >> 26) == 18 and (w & 1) and not (w & 2):
                t = a + k + (((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000)
                if t in sites:
                    sites[t].append(a + k)
    out, n, n_f1 = [], 0, 0
    for l in lines:
        a = int(l.split(",")[0], 16)
        if "," in l and proven(image, fn, edges_to, a, sites[a]):
            l += ",r4dead"
            n += 1
        if "," in l and proven(image, fn, edges_to, a, sites[a], f1_effect):
            l += ",f1dead"
            n_f1 += 1
        out.append(l)
    open(scope_out, "w", newline="\n", encoding="utf-8").write("\n".join(out) + "\n")
    print(f"{n} of {len(lines)} entries: r4 dead at every caller; {n_f1}: f1 dead at every caller")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
