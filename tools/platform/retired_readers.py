"""Gate 2 of docs/GC_REMOVAL_PLAN.md, static half: does any surviving code read the state a removal step retires?

    python -B retired_readers.py --what-if <dir of what_if.json> --graph <live_graph.txt>
                                 [--dolrecon-dir DIR] [--out DIR] [--require-no-readers]

A removal step (tools/platform/what_if.py) owns rows and so frees others. The RETIRED rows are both: their original
bodies no longer run. This tool scans every instruction of the retired rows and of every row that stays live after
the step (the generator's liveness graph with the step's owners applied, tools/platform/removal_plan.py) with the
must-analysis of tools/dolrecon/vcalls.py (r2 / r13 small-data bases known at entry, constants through the block
graph), and reports:

  retired writes      every store of a retired row whose effective address is proven (an absolute or small-data
                      address): the retired state that has a fixed address;
  surviving readers   every load of a surviving row whose proven address overlaps a retired write: the owner must
                      write those bytes as the original did (shown by its contract test), or the step is wrong;
  native readers      runtime sources naming an address inside a retired write (they may read it);
  unproven accesses   retired stores and surviving loads through a pointer the analysis cannot prove (heap and
                      object fields, indexed forms): NOT excluded by this tool - the dynamic watch (gate 2's other
                      half) or a type-level argument must cover them, and the report lists them per row.

Sound for what it reports as a reader; silent on nothing: every access it cannot place is counted as unproven.
Writes <out>/retired_readers.json. With --require-no-readers, exits 1 when a surviving or native reader exists.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(SRC, "tools"))
import removal_plan  # noqa: E402
import what_if  # noqa: E402
from dolrecon import dol as dolmod, ppc, spine, strrefs, vcalls  # noqa: E402

STORES = {36: 4, 37: 4, 38: 1, 39: 1, 44: 2, 45: 2, 52: 4, 53: 4, 54: 8, 55: 8, 60: 8, 61: 8}
LOADS = {32: 4, 33: 4, 34: 1, 35: 1, 40: 2, 41: 2, 42: 2, 43: 2, 48: 4, 49: 4, 50: 8, 51: 8, 56: 8, 57: 8}
STMW, LMW = 47, 46
# X-form (opcode 31) memory accesses by extended opcode: indexed, never proven here
X_LOADS = {23, 55, 87, 119, 279, 311, 343, 375, 535, 567, 599, 631, 790, 534, 20, 6}
X_STORES = {151, 183, 215, 247, 407, 439, 663, 695, 727, 759, 918, 662, 150, 983}


def accesses(image, sda, start, size):
    """(proven, unproven) accesses of one row: proven = [(pc, kind, ea, width)], unproven = [(pc, kind)]."""
    fn = vcalls.Function(image, start, size, ())
    proven, unproven = [], []

    def visit(pc, w, regs, consts):
        op = w >> 26
        _, d, ra, _, imm = ppc.fields(w)
        if op in STORES or op in LOADS or op in (STMW, LMW):
            kind = "store" if op in STORES or op == STMW else "load"
            width = STORES.get(op) or LOADS.get(op) or (32 - d) * 4
            base = 0 if ra == 0 else consts.get(ra)
            if isinstance(base, int):
                disp = ppc.simm(imm) if op not in (56, 57, 60, 61) else (((imm & 0xFFF) ^ 0x800) - 0x800)
                proven.append((pc, kind, (base + disp) & 0xFFFFFFFF, width))
            else:
                unproven.append((pc, kind))
        elif op == 31:
            xo = (w >> 1) & 0x3FF
            if xo in X_LOADS:
                unproven.append((pc, "load"))
            elif xo in X_STORES:
                unproven.append((pc, "store"))

    fn.scan((), visit, dict(sda))
    return proven, unproven


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--what-if", help="directory holding a what_if.py result (what_if.json)")
    ap.add_argument("--own", action="append", default=[],
                    help="run what_if.py for this ownership step first (into --out), instead of --what-if")
    ap.add_argument("--release-dir", default=None, help="the build tree what_if.py reads (with --own)")
    ap.add_argument("--expect", choices=("readers", "no-proven-readers"), default=None,
                    help="a control: fail unless the step has surviving readers / has none at a proven address "
                         "while still reporting its unproven accesses")
    ap.add_argument("--graph", required=True, help="live_graph.txt of the same generator inputs (removal_plan.py)")
    ap.add_argument("--dolrecon-dir", default=what_if.DOLRECON)
    ap.add_argument("--out", default=None)
    ap.add_argument("--require-no-readers", action="store_true")
    args = ap.parse_args(argv)
    if args.own:
        if not args.out:
            raise SystemExit("--own needs --out")
        args.out = os.path.abspath(args.out)
        step = ["--name", "step", "--out", args.out, "--dolrecon-dir", args.dolrecon_dir] + \
               (["--release-dir", args.release_dir] if args.release_dir else []) + \
               [x for a in args.own for x in ("--own", a)]
        if what_if.main(step) != 0:
            raise SystemExit("the step's generator run failed")
        args.what_if = os.path.join(args.out, "step")
    elif not args.what_if:
        raise SystemExit("give --what-if or --own")

    wi = json.load(open(os.path.join(args.what_if, "what_if.json")))
    own = {int(x, 16) for x in wi["step"]["own"]}
    freed = {int(x.split()[0], 16) for v in wi["freed_by_service"].values() for x in v["rows"]}
    if wi["step"]["add_roots"] or wi["step"]["drop_roots"]:
        raise SystemExit("only ownership steps are supported (roots change what survives in ways the graph cannot replay)")
    g = removal_plan.Graph(args.graph)
    live_after = g.live(own=frozenset(own))
    live_before = g.live()
    if live_before - live_after != freed:
        raise SystemExit(f"the graph does not reproduce the step: it frees {len(live_before - live_after)} rows, "
                         f"what_if.py's generator run freed {len(freed)}")
    retired = sorted(freed | (own & live_before))
    surviving = sorted(r for r in live_after if g.rows[r][1] == "body" and r not in own)

    project = os.path.join(args.dolrecon_dir, "pal-v30")
    image = dolmod.load_file(os.path.join(project, "main.dol"))
    _, objects = spine.load_symbols(os.path.join(project, "config", "symbols.txt"))
    text = next(x for x in image.sections if x.kind == "text")
    r2, r13 = strrefs.sda_bases(image, text.address, text.address + text.size)
    sda = {r: v for r, v in ((2, r2), (13, r13)) if v is not None}
    obj = sorted((a, n, size) for n, (a, size, *_) in objects.items())
    obj_starts = [o[0] for o in obj]

    def name_of(ea):
        i = bisect.bisect_right(obj_starts, ea) - 1
        if i >= 0 and ea < obj[i][0] + max(obj[i][2], 1):
            return f"{obj[i][1]}+0x{ea - obj[i][0]:X}"
        return None

    writes = []                     # (ea, width, row, pc)
    retired_unproven = collections.Counter()
    for r in retired:
        p, u = accesses(image, sda, r, g.rows[r][0])
        writes += [(ea, w, r, pc) for pc, k, ea, w in p if k == "store"]
        retired_unproven[r] += sum(1 for _, k in u if k == "store")
    spans = sorted({(ea, ea + w) for ea, w, _, _ in writes})
    span_starts = [s for s, _ in spans]

    def overlaps(ea, width):
        i = bisect.bisect_right(span_starts, ea + width - 1) - 1
        while i >= 0 and spans[i][1] > ea - 64:
            if spans[i][0] < ea + width and ea < spans[i][1]:
                return spans[i]
            i -= 1
        return None

    readers = []
    surviving_unproven = 0
    for r in surviving:
        p, u = accesses(image, sda, r, g.rows[r][0])
        surviving_unproven += sum(1 for _, k in u if k == "load")
        for pc, k, ea, w in p:
            if k == "load" and overlaps(ea, w):
                readers.append({"row": f"0x{r:08X} {g.rows[r][3]}", "pc": f"0x{pc:08X}", "ea": f"0x{ea:08X}",
                                "width": w, "object": name_of(ea)})

    native = []
    lits = what_if.roots_by_file()
    for a, files in sorted(lits.items()):
        ship = sorted(f for f in files if not f.startswith("runtime/tests/"))
        if ship and overlaps(a, 1):
            native.append({"address": f"0x{a:08X}", "object": name_of(a), "files": ship})

    by_ea = collections.defaultdict(list)
    for ea, w, r, pc in writes:
        by_ea[(ea, w)].append(f"0x{pc:08X}")
    result = {
        "step": wi["step"], "retired_rows": len(retired), "surviving_rows": len(surviving),
        "retired_writes": [{"ea": f"0x{ea:08X}", "width": w, "object": name_of(ea), "sites": sites}
                           for (ea, w), sites in sorted(by_ea.items())],
        "surviving_readers": readers, "native_readers": native,
        "retired_unproven_stores": {f"0x{r:08X} {g.rows[r][3]}": n for r, n in sorted(retired_unproven.items()) if n},
        "surviving_unproven_loads": surviving_unproven,
    }
    out = args.out or args.what_if
    os.makedirs(out, exist_ok=True)
    json.dump(result, open(os.path.join(out, "retired_readers.json"), "w"), indent=1)
    print(f"retired rows {len(retired)}, surviving rows {len(surviving)}")
    print(f"retired writes with a proven address: {len(by_ea)} locations; through unproven pointers: "
          f"{sum(retired_unproven.values())} stores in {len(result['retired_unproven_stores'])} rows")
    print(f"surviving readers of retired state: {len(readers)} loads in "
          f"{len({x['row'] for x in readers})} rows; native readers: {len(native)} addresses")
    for x in readers[:15]:
        print(f"  {x['row']} at {x['pc']} reads {x['ea']} ({x['object']})")
    for x in native[:10]:
        print(f"  native: {x['address']} ({x['object']}) in {', '.join(x['files'])}")
    if args.expect == "readers" and not (readers or native):
        print("CONTROL FAILED: the step was expected to have surviving readers and none was found")
        return 1
    if args.expect == "no-proven-readers" and (readers or native or not
                                               (result["retired_unproven_stores"] or result["retired_writes"])):
        print("CONTROL FAILED: expected retired state with no reader at a proven address")
        return 1
    return 1 if args.require_no_readers and (readers or native) else 0


if __name__ == "__main__":
    sys.exit(main())
