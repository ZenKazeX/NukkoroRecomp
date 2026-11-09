"""Independent checks of the RenderWare names (rw_names.json): evidence no rwalign rule uses.

    python -B -m dolrecon.rwcheck <named project> <spine dir> <rw_names.json>        (run from source/tools)

F  field offsets   both builds are 32-bit, so a RenderWare struct has the same layout on GameCube and PC: a correct
                   pair addresses the same fields. Per pair: the memory displacements of the guest function (PowerPC
                   D-form loads / stores, base not r1 / r2 / r13 / r0) and of the PC function (x86 memory operands,
                   base not esp, no index), 0 < d < 0x10000; score = Jaccard of the two sets, also tried with the
                   guest set shifted by its most common difference (a struct whose PC layout has a block of another
                   size in front: Rt2dGlobals fields sit 960 bytes further on PC). Reported against a negative
                   control: the same guest functions paired with random PC functions.
L  link order      CodeWarrior links each library contiguously: a name whose library differs from that of both
                   neighbouring names (address order), the neighbours sharing one library, is an island.
A pair is listed when both sides have >= 3 displacements and the score is < 0.2, or when it is an island.
Exit status 1 while any listed pair is not in REVIEWED (read by hand, with the reason): a gate, not a report.
Nothing is renamed here.
"""
from __future__ import annotations

import argparse
import collections
import json
import random
import statistics
import struct
import sys

import capstone

from . import match as mmatch
from . import rwalign


# pairs read by hand after rwcheck listed them: address -> why the name stands
REVIEWED = {
    0x804742B0: "_rt2dFontOpen: the same two 'fonts\\\\' strings, the same callees and font free-list globals; its "
                "fields go through Rt2dGlobals at other offsets (2026-11-01)",
}


def pc_offsets(md, fn) -> set[int]:
    out = set()
    for ins in md.disasm(fn.code, 0):
        for op in ins.operands:
            if (op.type == capstone.x86.X86_OP_MEM and op.mem.base != capstone.x86.X86_REG_ESP and op.mem.index == 0
                    and 0 < op.mem.disp < 0x10000):
                out.add(op.mem.disp)
    return out


def guest_offsets(proj, addr: int, size: int) -> set[int]:
    out = set()
    for a in range(addr, addr + size, 4):
        raw = proj.read(a, 4)
        if raw is None:
            break
        w = struct.unpack(">I", raw)[0]
        op, ra, d = w >> 26, (w >> 16) & 31, w & 0xFFFF
        if 32 <= op <= 55 and ra not in (0, 1, 2, 13) and 0 < d < 0x8000:
            out.add(d)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("spine")
    ap.add_argument("rw_names")
    args = ap.parse_args(argv)
    pc, _ = rwalign.load_pc()
    proj = mmatch.Project(args.project)
    with open(args.rw_names, encoding="utf-8") as f:
        names = json.load(f)["names"]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    pairs = sorted((int(a, 16), v["name"], v["rule"], v["guest_size"]) for a, v in names.items())

    def score(g, p):
        if not g and not p:
            return None
        best = len(g & p) / len(g | p)
        diffs = collections.Counter(q - x for x in g for q in p)
        if diffs:
            shift = diffs.most_common(1)[0][0]
            gs = {x + shift for x in g}
            best = max(best, len(gs & p) / len(gs | p))
        return best
    rows, control = [], []
    rnd = random.Random(1)
    pool = [n for n in pc if pc[n].size >= 24]
    for a, p, rule, size in pairs:
        g = guest_offsets(proj, a, size)
        rows.append((a, p, rule, g, pc_offsets(md, pc[p]), score(g, pc_offsets(md, pc[p]))))
        control.append(score(g, pc_offsets(md, pc[rnd.choice(pool)])))
    named = [s for *_, s in rows if s is not None]
    rand = [s for s in control if s is not None]
    flagged = [r for r in rows if len(r[3]) >= 3 and len(r[4]) >= 3 and r[5] < 0.2]
    islands = []
    for i, (a, p, rule, _) in enumerate(pairs):
        lib = pc[p].module.split("/")[0]
        near = {pc[pairs[j][1]].module.split("/")[0] for j in (i - 1, i + 1) if 0 <= j < len(pairs)}
        if 0 < i < len(pairs) - 1 and len(near) == 1 and lib not in near:
            islands.append((a, p, rule, sorted(near)))
    report = {
        "pairs": len(pairs),
        "fields": {"named_median": round(statistics.median(named), 2), "named_mean": round(statistics.mean(named), 2),
                   "control_median": round(statistics.median(rand), 2),
                   "control_mean": round(statistics.mean(rand), 2),
                   "flagged": [f"0x{a:08X} {p} ({r}) score {s:.2f} guest {sorted(g)[:6]} pc {sorted(q)[:6]}"
                               for a, p, r, g, q, s in flagged]},
        "link_islands": [f"0x{a:08X} {p} ({r}) between {n}" for a, p, r, n in islands],
    }
    open_items = [x for x in flagged if x[0] not in REVIEWED] + [x for x in islands if x[0] not in REVIEWED]
    report["unreviewed"] = len(open_items)
    print(json.dumps(report, indent=1))
    return 1 if open_items else 0


if __name__ == "__main__":
    sys.exit(main())
