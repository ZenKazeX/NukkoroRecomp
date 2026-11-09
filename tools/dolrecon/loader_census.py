"""The stage-loader census (docs/STAGE_LOADING.md): every function of the stage loading sequence, graded by evidence,
with its static call structure, its native status in this repo and - once a stage load has been observed - whether
and when it ran inside the load window.

    python -B -m dolrecon.loader_census <dolrecon root> <callgraph.json> <seeds.csv> <out dir>
        [--native <repo root>] [--pruned <recomp_pruned.csv>] [--dynamic <counts.csv>]... [--window <first>,<last>]

Membership is not a class rule (most loader code has no class): it starts at the seed functions the reverse
engineering established (<seeds.csv>: address,group,role,files,evidence - one row per entry point of a step of the
sequence) and follows the static call graph (call / tail edges, and addr edges: completion callbacks handed to the
async layer) through game code that dolrecon.game_map puts in a loader subsystem (LOADER_SUBSYSTEMS, by any
evidence) or could not place at all (UNPLACED: unknown, no caller, a library's callback). A function game_map gives
another subsystem (the player, enemies, the Task framework ..., by class, unit or callers), game code shared by
several subsystems (game_map's "shared game code") or a library function stops the walk: it is an interface row
(category SHARED_ENGINE_DEPENDENCY or OTHER_SUBSYSTEM), counted but not a member. A seed is always a member, whatever
its subsystem.
Category = the seed group whose seeds reach the member; reached from several groups -> SHARED_LOADER.
Native status (from the repo, --native): NATIVE_OWNER (extract/native-owners*.txt: the generator emits only a thunk),
OWNER_ENTRY (an address inside an owner's extent that the runtime enters), PRUNED (recomp_pruned.csv: no caller left),
RUNTIME_REFERENCED (an address literal of the runtime sources, extract/liveness-roots.txt: hooked, called or read by
native code), else TRANSLATED (recompiled PowerPC).
Dynamic input (--dynamic, repeatable): the call-census counts file (GEKKO_CALL_CENSUS_COUNTS,
runtime/src/gekko/system.cpp). --window <first>,<last> (game ticks) marks the load window: the boot-stage hand-off to
the first gameplay frame; a function whose first call falls in it ran during the load.
Writes, in <out dir>:
  loader_census.csv   one row per member and per interface function a member reaches
  loader_scope_ranges.txt  the members as a --verify-player scope file (address,size)
  loader_census.json  totals by group / native status / evidence, the load window's dynamic-only functions (executed
                      in the window but outside the static membership: loaders the seeds miss), input SHA-256s
"""
from __future__ import annotations

import collections
import csv
import hashlib
import json
import pathlib
import re
import sys

from dolrecon import dol, game_map

# "missions, stage flow, results" is not walked: it holds the stage flow's seeds (StageManager, the init task, the
# sequence phases) but also every mission, result and stage-action body those reach; its loader functions are seeds.
LOADER_SUBSYSTEMS = {
    "resources, loading, stage data",
    "stage data, object placement (set data)",
    "rendering and stage environment (fog, light, sky, background)",
}
# game_map's subsystems for code it could not place (no class, no agreeing callers)
UNPLACED = ("unknown (", "no caller found", "game callbacks called by a library")
WALK_KINDS = {"call", "tail", "addr"}
BCTR, BCTRL, BLRL = 0x4E800420, 0x4E800421, 0x4E800021


def sha256(path) -> str:
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def read_seeds(path):
    seeds = {}
    for r in csv.DictReader(open(path, encoding="utf-8")):
        if not r["address"] or r["address"].startswith("#"):
            continue
        seeds[int(r["address"], 16)] = r
    return seeds


def read_dynamic(paths):
    calls, first, last = collections.Counter(), {}, {}
    for p in paths:
        for row in csv.reader(open(p, encoding="utf-8")):
            if not row or row[0] in ("address", "edge", "#"):
                continue
            a = int(row[0], 16)
            calls[a] += int(row[1])
            if int(row[1]):
                first[a] = min(first.get(a, 1 << 62), int(row[2]))
                last[a] = max(last.get(a, 0), int(row[3]))
    return calls, first, last


def read_native(repo: pathlib.Path, pruned_path):
    owners, entries = set(), set()
    for name in ("native-owners.txt", "native-owners-r2.txt"):
        p = repo / "extract" / name
        if not p.exists():
            continue
        for line in open(p, encoding="utf-8"):
            m = re.match(r"(0x[0-9A-Fa-f]{8})\s*(.*)", line)
            if m:
                (entries if m.group(2).startswith("entry:") else owners).add(int(m.group(1), 16))
    literals = {int(l.split()[0], 16) for l in open(repo / "extract/liveness-roots.txt", encoding="utf-8")
                if l.startswith("0x")}
    pruned = set()
    if pruned_path:
        pruned = {int(r["address"], 16) for r in csv.DictReader(open(pruned_path, encoding="utf-8"))}
    return owners, entries, literals, pruned


def build(root: pathlib.Path, cg_path: str, seeds_path: str, repo=None, pruned_path=None, dynamic_paths=(),
          window=None):
    fn = game_map.load_functions(root)
    owners_json = game_map.load_owners(root / "gameplay_scope/owners.json")
    cg = json.load(open(cg_path, encoding="utf-8"))
    sub, how = game_map.classify(fn, game_map.load_bases(root), owners_json, cg["edges"])
    image = dol.load_file(str(root / "pal-v30/main.dol"))
    seeds = read_seeds(seeds_path)
    n2a = {r["name"]: a for a, r in fn.items()}
    cls = {a: owners_json.get(a) or (fn[a]["class"].split(" | ")[0] if fn[a]["class"] else "") for a in fn}

    callers, callees = collections.defaultdict(set), collections.defaultdict(set)
    for e in cg["edges"]:
        s, t = n2a.get(e["from"]), n2a.get(e["to"])
        if s is None or t is None or e["kind"] not in WALK_KINDS:
            continue
        callers[t].add(s)
        callees[s].add(t)

    def walkable(a):
        if fn[a]["layer"] not in game_map.GAME_LAYERS:
            return False
        return sub[a] in LOADER_SUBSYSTEMS or sub[a].startswith(UNPLACED)

    groups = collections.defaultdict(set)   # member -> seed groups reaching it
    members = set()
    for s, r in seeds.items():
        frontier = [s]
        seen = {s}
        while frontier:
            a = frontier.pop()
            members.add(a)
            groups[a].add(r["group"])
            for t in callees[a]:
                if t not in seen and walkable(t) and t not in seeds:
                    seen.add(t)
                    frontier.append(t)
    interface = {t for a in members for t in callees[a] if t not in members}

    owners, entries, literals, pruned = read_native(repo, pruned_path) if repo else (set(), set(), set(), set())

    def native_status(a):
        if a in owners:
            return "NATIVE_OWNER"
        if a in entries:
            return "OWNER_ENTRY"
        if a in pruned:
            return "PRUNED"
        if a in literals:
            return "RUNTIME_REFERENCED"
        return "TRANSLATED"

    calls, first, last = read_dynamic(dynamic_paths)
    lo, hi = window if window else (None, None)

    def in_window(a):
        return a in first and lo is not None and lo <= first[a] <= hi

    cat = {}
    for a in members | interface:
        g = sorted(groups[a])
        if a in members:
            cat[a] = g[0] if len(g) == 1 else "SHARED_LOADER"
        elif fn[a]["layer"] in game_map.GAME_LAYERS:
            cat[a] = "OTHER_SUBSYSTEM"
        else:
            cat[a] = "SHARED_ENGINE_DEPENDENCY"

    rows = []
    for a in sorted(members | interface):
        r = fn[a]
        m = a in members
        size = int(r["size"])
        words = [image.word(a + i) for i in range(0, size, 4)] if m else []
        g = sorted(groups[a])
        seed = seeds.get(a, {})
        rows.append({
            "address": f"0x{a:08X}", "size": size, "instructions": size // 4, "name": r["name"], "class": cls[a],
            "layer": r["layer"], "subsystem": sub[a], "evidence": how[a], "category": cat[a],
            "groups": " ".join(g), "seed_role": seed.get("role", ""), "files": seed.get("files", ""),
            "native_status": native_status(a),
            "callers_member": sum(1 for c in callers[a] if c in members),
            "callers_outside": sum(1 for c in callers[a] if c not in members),
            "indirect_calls": (words.count(BCTRL) + words.count(BLRL)) if m else "",
            "indirect_jumps": words.count(BCTR) if m else "",
            "dynamic_calls": calls.get(a, 0) if dynamic_paths else "",
            "dynamic_first_tick": first.get(a, "") if dynamic_paths else "",
            "ran_in_load_window": in_window(a) if dynamic_paths and window else "",
        })

    def total(sel):
        sel = list(sel)
        return {"functions": len(sel), "bytes": sum(int(fn[a]["size"]) for a in sel)}

    def by(key, sel):
        d = collections.defaultdict(list)
        for a in sel:
            d[key(a)].append(a)
        return {k: total(v) for k, v in sorted(d.items())}

    summary = {
        "inputs": {"functions.csv": sha256(root / "skeleton/functions.csv"),
                   "owners.json": sha256(root / "gameplay_scope/owners.json"),
                   "callgraph": {"path": str(cg_path), "sha256": sha256(cg_path), "kinds": cg["kinds"]},
                   "seeds": {"path": str(seeds_path), "sha256": sha256(seeds_path), "rows": len(seeds)},
                   "main.dol": image.sha256,
                   "pruned": {"path": str(pruned_path), "sha256": sha256(pruned_path)} if pruned_path else None,
                   "dynamic": {str(p): sha256(p) for p in dynamic_paths}, "window": window},
        "members": total(members),
        "by_category": by(cat.get, members),
        "by_native_status": by(native_status, members),
        "by_evidence": by(lambda a: how[a], members),
        "interface": dict(total(interface), by_layer=dict(collections.Counter(fn[a]["layer"] for a in interface))),
        "seeds_missing_from_function_table": [f"0x{a:08X}" for a in seeds if a not in fn],
    }
    if dynamic_paths:
        executed = {a for a in members if calls.get(a)}
        summary["dynamic"] = {"members_executed": total(executed), "members_not_executed": total(members - executed)}
        if window:
            win = {a for a in first if lo <= first[a] <= hi and fn.get(a, {}).get("layer") in game_map.GAME_LAYERS}
            summary["dynamic"]["window"] = {
                "game_functions_first_run_in_window": total(win),
                "members_first_run_in_window": total(win & members),
                "outside_membership": dict(total(win - members - interface),
                                           by_subsystem=dict(collections.Counter(sub[a] for a in win - members
                                                                                 - interface).most_common(25))),
            }
    return rows, summary


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)

    def opt(name, many=False):
        vals = []
        while name in argv:
            i = argv.index(name)
            vals.append(argv[i + 1])
            del argv[i:i + 2]
        return vals if many else (vals[-1] if vals else None)

    dynamic = opt("--dynamic", many=True)
    repo, pruned, window = opt("--native"), opt("--pruned"), opt("--window")
    window = tuple(int(x) for x in window.split(",")) if window else None
    root, cg_path, seeds_path, out = pathlib.Path(argv[0]), argv[1], argv[2], pathlib.Path(argv[3])
    out.mkdir(parents=True, exist_ok=True)
    rows, summary = build(root, cg_path, seeds_path, pathlib.Path(repo) if repo else None, pruned, dynamic, window)
    with open(out / "loader_census.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    # The members as a --verify-player scope file ("address,size" per function, hex without 0x): targeted strict
    # verification of the loader (docs/SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md 6.1)
    with open(out / "loader_scope_ranges.txt", "w", newline="\n", encoding="utf-8") as f:
        for r in rows:
            if r["category"] not in ("OTHER_SUBSYSTEM", "SHARED_ENGINE_DEPENDENCY"):
                f.write(f"{r['address'][2:]},{r['size']}\n")
    with open(out / "loader_census.json", "w", newline="\n", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
        f.write("\n")
    print(json.dumps({k: summary[k] for k in summary if k != "inputs"}, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
