"""Every wait point retired from the shared list must belong to a loop the REFERENCE build can no longer run.

extract/wait-points-retired.txt removes a back edge from extract/wait-points.txt for every build. That is only sound when the
loop's function is gone from every build: a native owner in extract/native-owners.txt (the generator emits a thunk, not the body),
pruned from the reference build's liveness set (recomp_pruned.csv of a SHADOW_RENDER_LIFTED=OFF tree), or unreachable by static
closure (every bl / branch / materialisation / data word of it sits in a function that is itself owned, pruned or unreachable).
A guest loop that can still run without its wait point never advances the game clock: strict mode B hung that way at
0x804954CC (2026-11-09) after a lifted-only owner retired the shared rows. Rows for lifted-only owners belong in
extract/wait-points-retired-lifted.txt.

Limits (closure_proof.py's): pointers held in registers, std-function slots and non-constant address arithmetic are not seen;
a function without any caller, materialisation or data word counts as unreachable (an entry point would be listed as a root).

  retired_wait_reach.py --pruned <reference recomp_pruned.csv> [--retired extract/wait-points-retired.txt]
"""
import argparse
import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import closure_proof  # noqa: E402


def read_sites(path):
    out = []
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*0x([0-9A-Fa-f]{8})\b", line)
        if m:
            out.append(int(m.group(1), 16))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pruned", required=True)
    ap.add_argument("--retired", default=os.path.join(closure_proof.ROOT, "extract", "wait-points-retired.txt"))
    a = ap.parse_args()
    pruned = {int(r["address"], 16) for r in csv.DictReader(open(a.pruned, encoding="utf-8"))}
    cl = closure_proof.Closure(pruned)
    cl.owned = closure_proof.owner_set("extract/native-owners.txt")  # the lifted-only rows do not own the reference build's bodies
    cl.lowered = set()
    roots = set(read_sites(os.path.join(closure_proof.ROOT, "extract", "liveness-roots.txt")))  # the reference build's seeds
    memo = {}

    def can_run(fa, stack=()):
        """(True, why) when something live can enter function fa."""
        if fa in memo:
            return memo[fa]
        if fa in cl.owned or fa in pruned:
            memo[fa] = (False, "owned/pruned")
            return memo[fa]
        if fa in stack:
            return (False, "cycle")
        if fa in roots:
            memo[fa] = (True, "liveness root")
            return memo[fa]
        res = cl.query(fa)
        for key in ("bl", "branch", "mat"):
            for site, st, caller in res[key]:
                if st in ("OWNED", "PRUNED"):
                    continue
                if caller is None or caller == fa:
                    continue
                ok, why = can_run(caller, stack + (fa,))
                if ok:
                    memo[fa] = (True, "%s 0x%08X in 0x%08X" % (key, site, caller))
                    return memo[fa]
        for addr, cons in res["data"]:
            live = [c for c in cons if c[1] == "LIVE" and can_run(c[0][0] if isinstance(c[0], tuple) else c[0], stack + (fa,))[0]]
            if live:
                memo[fa] = (True, "data word 0x%08X" % addr)
                return memo[fa]
        memo[fa] = (False, "no live entry")
        return memo[fa]

    bad = 0
    for site in read_sites(a.retired):
        o = closure_proof.dolxref.owner(cl.funcs, cl.starts, site)
        fa = o[0] if o else None
        if fa is None:
            print("0x%08X: no function" % site)
            bad += 1
            continue
        ok, why = can_run(fa)
        print("0x%08X in 0x%08X: %s" % (site, fa, "CAN RUN (%s)" % why if ok else "cannot run (%s)" % memo[fa][1]))
        bad += ok
    if bad:
        print("%d retired wait point(s) in functions the reference build can still run: move them to wait-points-retired-lifted.txt "
              "(lifted-only owners) or restore them" % bad)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
