#!/usr/bin/env python
"""Every indirect call/branch (bctr, bctrl, blrl) of the DOL with the memory slot its target was loaded from.

    python -B tools/platform/indirect_calls.py [--pruned CSV] [--freed ADDR,...] [--live-only] [--slot OFF[,OFF...]] [--via OFF]

A linear backward slice inside the function: `mtctr rX` / `mtlr rX` (for blrl) <- the nearest preceding write of rX; when it is an
`lwz rX, off(rB)` the site's slot is (off, how rB was obtained: another `lwz rB, via(rC)` -> "via"). Sites whose target
register has another origin are reported as `other`. --slot keeps the sites loading at those offsets, --via those whose base
was itself loaded from `via(...)`. Used by docs/agents/axfx-effects.md, Step K, to cover "every indirect call that can read a slot".
"""
import argparse
import bisect
import collections
import csv
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dolxref  # noqa: E402

sys.path.insert(0, HERE)
from closure_proof import owner_set  # noqa: E402


def decode(w):
    return w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pruned")
    ap.add_argument("--freed", default="")
    ap.add_argument("--live-only", action="store_true")
    ap.add_argument("--slot", default="")
    ap.add_argument("--via", default="")
    a = ap.parse_args()
    img = dolxref.Image(dolxref.DOL)
    funcs = dolxref.load_functions()
    starts = [f[0] for f in funcs]
    owned = owner_set("extract/native-owners.txt", "extract/native-owners-r2.txt")
    pruned = set()
    if a.pruned:
        for r in csv.DictReader(open(a.pruned)):
            pruned.add(int(r["address"], 16))
    for t in a.freed.split(","):
        if t:
            pruned.add(int(t, 16))
    slots = {int(x, 0) for x in a.slot.split(",") if x}
    via = int(a.via, 0) if a.via else None
    rows = []
    for base, size, off in img.text:
        words = struct.unpack_from(">%dI" % (size // 4), img.raw, off)
        for k, w in enumerate(words):
            op = w >> 26
            if op != 19:
                continue
            xo = (w >> 1) & 0x3FF
            bo = (w >> 21) & 31
            if xo == 528 and bo == 20:       # bctr / bctrl
                reg_kind = "ctr"
            elif xo == 16 and bo == 20 and (w & 1):  # blrl
                reg_kind = "lr"
            else:
                continue
            addr = base + 4 * k
            o = dolxref.owner(funcs, starts, addr)
            fa = o[0] if o else None
            state = "OWNED" if fa in owned else "PRUNED" if fa in pruned else "LIVE"
            if a.live_only and state != "LIVE":
                continue
            # find mtctr / mtlr
            reg = None
            j = k - 1
            while j >= 0 and k - j < 40:
                op2, rt, ra, imm = decode(words[j])
                if op2 == 31 and ((words[j] >> 1) & 0x3FF) == 467:   # mtspr
                    spr = (((words[j] >> 11) & 31) << 5) | ((words[j] >> 16) & 31)
                    if (spr == 9 and reg_kind == "ctr") or (spr == 8 and reg_kind == "lr"):
                        reg = rt
                        break
                j -= 1
            slot = base_via = None
            if reg is not None:
                m = j - 1
                while m >= 0 and j - m < 40:
                    op2, rt, ra, imm = decode(words[m])
                    # an instruction writing reg
                    writes = (op2 in (32, 33, 34, 35, 40, 41, 14, 15, 24, 25, 28, 29, 21, 20) and (
                        rt == reg if op2 in (32, 33, 34, 35, 40, 41, 14, 15) else ra == reg)) or (
                        op2 == 31 and ((words[m] >> 1) & 0x3FF) in (444, 266, 40, 23, 87, 279) and (
                            ((words[m] >> 16) & 31) == reg if ((words[m] >> 1) & 0x3FF) == 444 else rt == reg))
                    if writes:
                        if op2 == 32:   # lwz
                            slot = imm - 0x10000 if imm & 0x8000 else imm
                            rb = ra
                            # how was rb obtained
                            n = m - 1
                            while n >= 0 and m - n < 40:
                                op3, rt3, ra3, imm3 = decode(words[n])
                                if op3 in (32, 14, 15, 24, 25) and rt3 == rb:
                                    if op3 == 32:
                                        base_via = imm3 - 0x10000 if imm3 & 0x8000 else imm3
                                    break
                                if op3 == 31 and ((words[n] >> 1) & 0x3FF) == 444 and ((words[n] >> 16) & 31) == rb:
                                    break
                                n -= 1
                        break
                    m -= 1
            rows.append((addr, fa, state, slot, base_via))
    c = collections.Counter()
    shown = 0
    for addr, fa, state, slot, bv in rows:
        if slots and slot not in slots:
            continue
        if via is not None and bv != via:
            continue
        c[(state, slot, bv)] += 1
        if slots or via is not None:
            o = dolxref.owner(funcs, starts, addr)
            print("0x%08X %-6s fn 0x%08X %s slot=%s via=%s" % (addr, state, fa or 0, o[2] if o else "", slot, bv))
            shown += 1
    print("# %d indirect sites matched; by (state, slot, base loaded via):" % sum(c.values()))
    for k, v in sorted(c.items(), key=lambda kv: (-kv[1])):
        print("  %5d  %s" % (v, k))


if __name__ == "__main__":
    main()
