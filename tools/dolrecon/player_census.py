"""The player census (docs/SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md, batch 00): every function of the player family,
graded by evidence, with its static call structure, its indirect call sites and - once a replay has been observed -
its dynamic execution.

    python -B -m dolrecon.player_census <dolrecon root> <callgraph.json> <out dir> [--dynamic <counts.csv>]...

Membership is dolrecon.game_map's player subsystem (one rule set for the whole game, not a second one for the
player). The call graph is an input so the census names the graph it was built on (dolrecon.callgraph output; the
pre-fix pal-v30/callgraph.json dropped 417 edges). Writes, in <out dir>:
  player_census.csv   one row per member and per outside function a member reaches (SHARED_ENGINE_DEPENDENCY)
  player_census.json  totals, the categories, the outside interface, the unresolved indirect sites, input SHA-256s
Dynamic input (--dynamic, repeatable): the call-census counts file the runtime writes (GEKKO_CALL_CENSUS_COUNTS,
runtime/src/gekko/system.cpp): `address,calls,first_tick,last_tick` rows then `edge,callee,caller_site` rows.
"""
from __future__ import annotations

import bisect
import collections
import csv
import hashlib
import json
import pathlib
import re
import sys

from dolrecon import callgraph, dol, game_map

# The directive's categories, by the owning class (first match wins). A function without a class keeps its game_map
# evidence (unit / callers) and is categorised by the classed members that call it, when they agree.
CATEGORIES = [
    ("PLAYER_ANIMATION", r"(Motion|MotionEye|MotionHand|MotionSub)\b|::Shadow::Motion"),
    ("PLAYER_COLLISION", r"CharaColliMove|ColliDetect"),
    ("PLAYER_ABILITIES", r"Behavior::(ChaosControl|ChaosBlast|DarkSpin|Super)\b|Shadow::Super|ChaosControl|ChaosBlast|Gauge"),
    ("PLAYER_COMBAT", r"Weapon|ControlGun|Attack\b|Behavior::(Damage|Dead|AirAttack|GroundAttack|Throw)\b|Barrier"),
    ("PLAYER_INTERACTION", r"Behavior::(PickupObject|PickupWeapon|DriveVehicle|ExternalControl|ElectricCircuit|Hide|Fly|FirstMeet)\b|Electric|TargetSearchList|PlayerTarget|HandleUser|Vehicle"),
    ("PLAYER_MOVEMENT", r"Behavior::(Idle|Ground|Fall|Landing|Jump|JumpDash|Sliding|Ottotto|TurnOver|SpinDash|Grind|LightDash|LineHang|WallHang|WallJump|EdgeHang|SomethingHang|TouchHang|Launch|Stream|HomingAttack|HomingJump)\b"),
    ("PLAYER_ACTION", r"Command\b|Behavior::(Behavior|Factory|InitializeArgs)|ShadowBehavior|Behavior$"),
    ("PLAYER_CORE", r"Player::(Player|PlayerBase|PlayerObject|PlayerShadow|Executer)\b|ShadowExecuter"),
    ("PLAYER_STATE", r"Effect|PostManager|Watch|Option|Sound"),
    ("PLAYER_SUPPORT", r"Control::|Controller|ActingInput|UserInput|Approach|Bukuu|Player"),
]
CATEGORIES = [(c, re.compile(p)) for c, p in CATEGORIES]

# Indirect transfers the census must account for (primary opcode 19, XO 16 / 528)
BCTR, BCTRL, BLRL = 0x4E800420, 0x4E800421, 0x4E800021


def estimate_op(w: int) -> bool:
    """frsqrte / fres / ps_rsqrte / ps_res: Gekko returns a hardware estimate; the interpreter and the recompiler
    both compute the exact 1/sqrt(x) and 1/x (runtime/include/gekko/ppc_ops.h kFpRsqrte, cpu.cpp RefFp::Rsqrte), so
    no oracle run here can prove these instructions' results."""
    op, xo = w >> 26, (w >> 1) & 31
    return (op == 63 and xo == 26) or (op == 59 and xo == 24) or (op == 4 and xo in (24, 26))


def sha256(path) -> str:
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def category_of(cls: str) -> str | None:
    for c, p in CATEGORIES:
        if p.search(cls):
            return c
    return None


def read_dynamic(paths):
    calls, edges = collections.Counter(), collections.Counter()
    for p in paths:
        for row in csv.reader(open(p, encoding="utf-8")):
            if not row or row[0] in ("address", "#"):
                continue
            if row[0] == "edge":
                edges[(int(row[1], 16), int(row[2], 16))] += int(row[3]) if len(row) > 3 else 1
            else:
                calls[int(row[0], 16)] += int(row[1])
    return calls, edges


def build(root: pathlib.Path, cg_path: str, dynamic_paths=()):
    fn = game_map.load_functions(root)
    owners = game_map.load_owners(root / "gameplay_scope/owners.json")
    cg = json.load(open(cg_path, encoding="utf-8"))
    sub, how = game_map.classify(fn, game_map.load_bases(root), owners, cg["edges"])
    image = dol.load_file(str(root / "pal-v30/main.dol"))
    starts = sorted(fn)
    n2a = {r["name"]: a for a, r in fn.items()}

    def owner_of(pc):
        i = bisect.bisect_right(starts, pc) - 1
        return starts[i] if i >= 0 and pc < starts[i] + int(fn[starts[i]]["size"]) else None

    members = {a for a, s in sub.items() if s == game_map.PLAYER}
    cls = {a: owners.get(a) or (fn[a]["class"].split(" | ")[0] if fn[a]["class"] else "") for a in fn}

    callers, callees = collections.defaultdict(set), collections.defaultdict(set)
    kinds = collections.defaultdict(collections.Counter)  # member -> incoming edge kinds
    for e in cg["edges"]:
        s, t = n2a.get(e["from"]), n2a.get(e["to"])
        if s is None or t is None:
            continue
        callers[t].add(s)
        callees[s].add(t)
        if t in members:
            kinds[t][e["kind"]] += 1

    # Category: by class; then, for unclassed members, the category every categorised member caller agrees on.
    cat = {a: category_of(cls[a]) for a in members if cls[a]}
    changed = True
    while changed:
        changed = False
        for a in members:
            if cat.get(a):
                continue
            cs = {cat.get(c) for c in callers[a] if c in members}
            if len(cs) == 1 and None not in cs:
                cat[a] = cs.pop()
                changed = True
    for a in members:
        cat[a] = cat.get(a) or "UNRESOLVED"

    # Indirect sites in member code.
    indirect = {}
    for a in members:
        size = int(fn[a]["size"])
        words = [image.word(a + i) for i in range(0, size, 4)]
        indirect[a] = (words.count(BCTRL) + words.count(BLRL), words.count(BCTR), sum(map(estimate_op, words)))
    virtual_resolved = collections.Counter(n2a.get(e["from"]) for e in cg["edges"] if e["kind"] == "virtual")

    # Roots: every vtable slot of a class whose methods are members (the dispatch the game reaches by data).
    slots, _ = callgraph.class_tables(str(root / "skeleton"))
    member_classes = {cls[a] for a in members if cls[a]}
    roots = {n2a[n] for c in member_classes for n in slots.get(c, {}).values() if n in n2a}
    reach, frontier = set(roots), list(roots)
    while frontier:
        a = frontier.pop()
        for t in callees[a]:
            if t in members and t not in reach:
                reach.add(t)
                frontier.append(t)

    shared = {t for a in members for t in callees[a] if t not in members}
    interface = {a for a in members if any(c not in members for c in callers[a])}
    calls, dyn_edges = read_dynamic(dynamic_paths)
    dyn_callers = collections.defaultdict(set)
    for (callee, site), n in dyn_edges.items():
        o = owner_of(site - 4)  # LR: the instruction after the call
        if o is not None:
            dyn_callers[callee].add(o)

    rows = []
    for a in sorted(members | shared):
        r = fn[a]
        is_member = a in members
        ins = sum(1 for c in callers[a] if c in members)
        rows.append({
            "address": f"0x{a:08X}", "size": int(r["size"]), "instructions": int(r["size"]) // 4, "name": r["name"],
            "class": cls[a], "layer": r["layer"], "subsystem": sub[a], "evidence": how[a],
            "category": cat[a] if is_member else "SHARED_ENGINE_DEPENDENCY",
            "callers_member": ins, "callers_outside": len(callers[a]) - ins,
            "incoming_kinds": " ".join(f"{k}:{v}" for k, v in sorted(kinds[a].items())) if is_member else "",
            "indirect_calls": indirect[a][0] if is_member else "", "indirect_jumps": indirect[a][1] if is_member else "",
            "estimate_ops": sum(estimate_op(image.word(a + i)) for i in range(0, int(r["size"]), 4)),
            "virtual_calls_resolved": virtual_resolved.get(r["name"], 0) if is_member else "",
            "reached_from_player_vtables": (a in reach) if is_member else "",
            "dynamic_calls": calls.get(a, 0) if dynamic_paths else "",
            "dynamic_callers_outside_static": len(dyn_callers[a] - callers[a]) if dynamic_paths else "",
        })

    def total(sel):
        return {"functions": len(sel), "bytes": sum(int(fn[a]["size"]) for a in sel)}

    by_cat = collections.defaultdict(set)
    for a in members:
        by_cat[cat[a]].add(a)
    by_ev = collections.defaultdict(set)
    for a in members:
        by_ev[how[a]].add(a)
    summary = {
        "inputs": {"functions.csv": sha256(root / "skeleton/functions.csv"),
                   "owners.json": sha256(root / "gameplay_scope/owners.json"),
                   "callgraph": {"path": str(cg_path), "sha256": sha256(cg_path), "kinds": cg["kinds"]},
                   "main.dol": image.sha256,
                   "dynamic": {str(p): sha256(p) for p in dynamic_paths}},
        "members": total(members),
        "by_evidence": {k: total(v) for k, v in sorted(by_ev.items())},
        "by_category": {k: total(v) for k, v in sorted(by_cat.items())},
        "classes": len(member_classes),
        "vtable_roots": len(roots),
        "members_not_reached_from_player_vtables": total(members - reach),
        "outside_interface": total(interface),
        "shared_engine_dependencies": dict(total(shared), by_subsystem=dict(collections.Counter(sub[a] for a in shared).most_common())),
        "indirect_call_sites": sum(v[0] for v in indirect.values()),
        "indirect_jump_sites": sum(v[1] for v in indirect.values()),
        "functions_with_indirect_calls": sum(1 for v in indirect.values() if v[0]),
        "functions_with_estimate_ops": total([a for a, v in indirect.items() if v[2]]),
        "estimate_op_sites": sum(v[2] for v in indirect.values()),
        "shared_with_estimate_ops": total([a for a in shared if any(estimate_op(image.word(a + i))
                                                                    for i in range(0, int(fn[a]["size"]), 4))]),
    }
    if dynamic_paths:
        executed = {a for a in members if calls.get(a)}
        summary["dynamic"] = {
            "members_executed": total(executed),
            "members_not_executed": total(members - executed),
            "outside_functions_executed_from_member_sites": len({c for (c, s), _ in dyn_edges.items()
                                                                 if owner_of(s - 4) in members and c not in members}),
            "member_callees_with_callers_static_graph_lacks": len([a for a in members if dyn_callers[a] - callers[a]]),
        }
    return rows, summary


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    dynamic = []
    while "--dynamic" in argv:
        i = argv.index("--dynamic")
        dynamic.append(argv[i + 1])
        del argv[i:i + 2]
    root, cg_path, out = pathlib.Path(argv[0]), argv[1], pathlib.Path(argv[2])
    out.mkdir(parents=True, exist_ok=True)
    rows, summary = build(root, cg_path, dynamic)
    with open(out / "player_census.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    with open(out / "player_census.json", "w", newline="\n", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
        f.write("\n")
    print(json.dumps({k: summary[k] for k in summary if k != "inputs"}, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
