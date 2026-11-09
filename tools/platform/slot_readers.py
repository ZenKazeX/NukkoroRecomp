#!/usr/bin/env python
"""Which functions load a word from a structure pointer that was itself loaded from `holder+PTR_OFF`, and call it.

    python -B tools/platform/slot_readers.py [--pruned CSV] --ptr 0x14 --slots 0,4,8,0xC [--live-only]

A forward, straight-line taint inside each function (stack spills of r1+off are followed; a call kills the volatile registers):
  P  = a register holding `lwz rX, PTR_OFF(any)`              (e.g. a pipeline node's private data, node+0x14)
  C  = a register holding `lwz rY, SLOT(P)` for SLOT in --slots (a stored callback)
A function is reported when a C register reaches `mtctr` / `mtlr` (an indirect call). Lower bound: values passed to callees,
kept in memory other than the stack, or computed with arithmetic are not followed. Used by Step K (docs/agents/axfx-effects.md) to
list the readers of the callback words the RenderWare pipeline creators store into node private data.
"""
import argparse
import csv
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import dolxref  # noqa: E402
from closure_proof import owner_set  # noqa: E402


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


def analyse(words, base, ptr_off, slots):
    """Yield (address, kind) for indirect calls whose target is a callback loaded through a private-data pointer."""
    P, C, stack = set(), set(), {}   # P/C: registers; stack: offset -> "P"|"C"
    out = []
    for k, w in enumerate(words):
        addr = base + 4 * k
        op = w >> 26
        rt, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 18 and (w & 1):                      # bl: volatile registers die
            for r in [0] + list(range(3, 13)):
                P.discard(r); C.discard(r)
            continue
        if op == 19 and ((w >> 1) & 0x3FF) == 528:    # bctr / bctrl
            continue
        if op == 32:                                  # lwz rt, d(ra)
            d = sx(imm)
            newP = newC = False
            if ra == 1 and d in stack:
                newP, newC = stack[d] == "P", stack[d] == "C"
            elif ra in P and d in slots:
                newC = True
            elif d == ptr_off:
                newP = True
            P.discard(rt); C.discard(rt)
            if newP: P.add(rt)
            if newC: C.add(rt)
            continue
        if op == 36:                                  # stw rt, d(ra)
            d = sx(imm)
            if ra == 1:
                if rt in P: stack[d] = "P"
                elif rt in C: stack[d] = "C"
                else: stack.pop(d, None)
            continue
        if op == 31:
            xo = (w >> 1) & 0x3FF
            rb = (w >> 11) & 31
            if xo == 444:                             # or ra, rs, rb  (mr)
                rs = rt
                P.discard(ra); C.discard(ra)
                if rs == rb:
                    if rs in P: P.add(ra)
                    if rs in C: C.add(ra)
                continue
            if xo == 467:                             # mtspr
                spr = (rb << 5) | ra
                if spr in (8, 9) and rt in C:
                    out.append(addr)
                continue
        if op in (14, 15, 24, 25, 28, 29, 21, 20, 34, 40, 42, 33, 41, 43):
            tgt = ra if op in (24, 25, 28, 29, 21, 20) else rt
            if op == 14 and imm == 0 and False:
                pass
            P.discard(tgt); C.discard(tgt)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pruned")
    ap.add_argument("--ptr", default="0x14")
    ap.add_argument("--slots", default="0,4,8,0xC")
    ap.add_argument("--live-only", action="store_true")
    a = ap.parse_args()
    ptr = int(a.ptr, 0)
    slots = {int(x, 0) for x in a.slots.split(",")}
    img = dolxref.Image(dolxref.DOL)
    funcs = dolxref.load_functions()
    owned = owner_set("extract/native-owners.txt", "extract/native-owners-r2.txt")
    pruned = set()
    if a.pruned:
        for r in csv.DictReader(open(a.pruned)):
            pruned.add(int(r["address"], 16))
    n = 0
    for base, size, off in img.text:
        allw = struct.unpack_from(">%dI" % (size // 4), img.raw, off)
        for fa, fs, name in funcs:
            if not (base <= fa < base + size):
                continue
            i0 = (fa - base) // 4
            words = allw[i0:i0 + fs // 4]
            hits = analyse(words, fa, ptr, slots)
            if not hits:
                continue
            state = "OWNED" if fa in owned else "PRUNED" if fa in pruned else "LIVE"
            if a.live_only and state != "LIVE":
                continue
            n += 1
            print("0x%08X %-6s %-22s indirect calls of a loaded callback at %s" % (fa, state, name, ", ".join("0x%X" % h for h in hits)))
    print("# %d functions" % n)


if __name__ == "__main__":
    main()
