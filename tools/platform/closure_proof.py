#!/usr/bin/env python
"""Static closure of every way a guest function can be entered (docs/agents/axfx-effects.md, Step K).

    python -B tools/platform/closure_proof.py [--pruned CSV] [--freed ADDR,...] ADDR [ADDR ...]

For each target address T it lists every place T appears, then classifies the function that contains the place:

  bl / b / bc sites   a direct call or branch to T;
  materialisations    lis+addi / ori / add (constant-propagated, dolxref.scan) that build T in a register: a function pointer;
  data words          an aligned word equal to T anywhere in the DOL (data tables, vtables, node definitions), with the table
                      it sits in and the code that materialises that table's base (the consumers).

Each containing function is OWNED (its entry is in extract/native-owners*.txt: the guest body is replaced), LOWERED (the call
site address is in a lift-sites list: the call instruction is replaced), PRUNED (the generator does not emit it; --pruned is a
what_if recomp_pruned.csv, --freed the rows the step itself frees, which are then also pruned) or LIVE. Only LIVE places can
execute; they are what the manual slot analysis (stores of the pointer, indirect calls that read the slot) must cover.
Nothing here proves the absence of address arithmetic that is not a constant fold; that stays a stated limit.
"""
import argparse
import csv
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dolxref  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))


def owner_set(*files):
    out = set()
    for f in files:
        for line in open(os.path.join(ROOT, f), encoding="utf-8", errors="replace"):
            m = re.match(r"\s*0x([0-9A-Fa-f]{8})\b", line)
            if m:
                out.add(int(m.group(1), 16))
    return out


def lowered_sites():
    out = set()
    for f in ("extract/lift-sites.txt", "extract/lift-sites-r2.txt", "extract/gx-sites.txt", "extract/gx-fail-sites-lifted.txt"):
        p = os.path.join(ROOT, f)
        if not os.path.exists(p):
            continue
        for line in open(p, encoding="utf-8", errors="replace"):
            m = re.match(r"\s*site\s+0x([0-9A-Fa-f]{8})\b", line) or re.match(r"\s*0x([0-9A-Fa-f]{8})\b", line)
            if m:
                out.add(int(m.group(1), 16))
    return out


class Closure:
    """Everything that can enter a function: built once per DOL, queried per target."""

    def __init__(self, pruned=None):
        self.img = dolxref.Image(dolxref.DOL)
        self.funcs = dolxref.load_functions()
        self.starts = [f[0] for f in self.funcs]
        self.owned = owner_set("extract/native-owners.txt", "extract/native-owners-r2.txt")
        self.lowered = lowered_sites()
        self.pruned = set(pruned or ())
        self.words = {}
        for base, size, off in self.img.all:
            for k in range(0, size - 3, 4):
                self.words.setdefault(struct.unpack_from(">I", self.img.raw, off + k)[0], []).append(base + k)

    def status(self, site):
        o = dolxref.owner(self.funcs, self.starts, site)
        if o is None:
            return "NOFUNC", None
        fa = o[0]
        if site in self.lowered:
            return "LOWERED", fa
        if fa in self.owned:
            return "OWNED", fa
        if fa in self.pruned:
            return "PRUNED", fa
        return "LIVE", fa

    def branches(self, T):
        out = []
        for base, size, off in self.img.text:
            for k in range(0, size, 4):
                w = struct.unpack_from(">I", self.img.raw, off + k)[0]
                addr = base + k
                op = w >> 26
                if op == 18 and not (w & 1):
                    d = w & 0x03FFFFFC
                    if d & 0x02000000:
                        d -= 0x04000000
                    dest = d if (w & 2) else (addr + d) & 0xFFFFFFFF
                elif op == 16:
                    d = w & 0xFFFC
                    if d & 0x8000:
                        d -= 0x10000
                    dest = d if (w & 2) else (addr + d) & 0xFFFFFFFF
                else:
                    continue
                if dest == T:
                    so = dolxref.owner(self.funcs, self.starts, addr)
                    if so and so[0] == T:      # a loop back to the entry inside the function itself
                        continue
                    out.append(addr)
        return out

    def table_consumers(self, addr, window=0x40):
        """Functions that materialise an address within `window` bytes before a data word (the table's base)."""
        cons = {}
        for back in range(0, window + 4, 4):
            _, mats = dolxref.scan(self.img, self.funcs, self.starts, addr - back)
            for ma, _k in mats:
                st, fa = self.status(ma)
                cons.setdefault((fa, st), []).append((ma, back))
        return cons

    def query(self, T):
        """{'bl': [(site, status, fn)], 'branch': [...], 'mat': [...], 'data': [(addr, consumers)]}"""
        calls, mats = dolxref.scan(self.img, self.funcs, self.starts, T)
        res = {"bl": [], "branch": [], "mat": [], "data": []}
        for key, sites in (("bl", calls), ("branch", self.branches(T)), ("mat", [m[0] for m in mats])):
            for site in sites:
                st, fa = self.status(site)
                res[key].append((site, st, fa))
        for addr in self.words.get(T, []):
            res["data"].append((addr, self.table_consumers(addr)))
        return res

    def live_places(self, res):
        """The places the proof still has to cover: LIVE call/branch/materialisation sites, and LIVE table consumers."""
        out = [(k, s) for k in ("bl", "branch", "mat") for s in res[k] if s[1] == "LIVE"]
        for addr, cons in res["data"]:
            live = [c for c in cons if c[1] == "LIVE"]
            out.append(("data", (addr, live if live else "no table-base materialiser within 0x40: unproven")))
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pruned")
    ap.add_argument("--freed", default="")
    ap.add_argument("targets", nargs="+")
    a = ap.parse_args()
    pruned = set()
    if a.pruned:
        for r in csv.DictReader(open(a.pruned)):
            pruned.add(int(r["address"], 16))
    for t in a.freed.split(","):
        if t:
            pruned.add(int(t, 16))
    c = Closure(pruned)
    for t in a.targets:
        T = int(t, 16)
        res = c.query(T)
        o = dolxref.owner(c.funcs, c.starts, T)
        print("=" * 100)
        print("TARGET 0x%08X  %s  (%s)" % (T, o[2] if o else "?", "OWNED" if T in c.owned else "PRUNED" if T in c.pruned else "LIVE"))
        for key, label in (("bl", "bl"), ("branch", "b/bc"), ("mat", "materialised")):
            print("  %s: %d site(s)" % (label, len(res[key])))
            for site, st, fa in res[key]:
                of = dolxref.owner(c.funcs, c.starts, site)
                print("    0x%08X %-9s in %s %s" % (site, st, ("0x%08X" % of[0]) if of else "-", of[2] if of else ""))
        print("  data words: %d" % len(res["data"]))
        for addr, cons in res["data"]:
            print("    word at 0x%08X; table-base materialisers within 0x40 before it:" % addr)
            for (fa, st), v in sorted(cons.items(), key=lambda kv: (kv[0][0] is None, kv[0][0] or 0)):
                of = dolxref.owner(c.funcs, c.starts, fa) if fa else None
                print("      %-8s %s %s  %s" % (st, ("0x%08X" % fa) if fa else "-", of[2] if of else "",
                                                ",".join("0x%X(-0x%X)" % x for x in v[:3])))
            if not cons:
                print("      (none within 0x40; consumers must come from a table base further up)")
        live = c.live_places(res)
        print("  LIVE places the proof must still cover: %d" % len(live))


if __name__ == "__main__":
    main()
