"""Turn test_data/platform/unidentified_names.csv into (1) a proposed patch for the dolrecon symbol table and
(2) a re-plan: what the removal planner's per-service view looks like once the proposed services are in the manifest.

    python -B apply_unidentified_names.py --graph LIVE_GRAPH.txt --dolrecon-dir DIR --out OUTDIR

Read-only on every input. Nothing under the dolrecon directory is written; the patch is written to OUTDIR for a human
(or the main session) to apply. The re-plan uses the planner's own Graph class and the same frontier / freed / greedy
order definitions as removal_plan.py, on an existing live graph (no generator run).

Outputs in OUTDIR:
  symbols.patch            unified diff against <dolrecon>/pal-v30/config/symbols.txt (renames of the PROVEN / INFERRED
                           rows whose name is an SDK symbol)
  service_overrides.json   address -> service for every identified row, plus the rows whose service the manifest
                           generator would NOT derive from the new name (needs a LIBRARY rule or an override)
  replan.json / replan.md  before / after: per-service live bodies, frontier, freed-if-owned, greedy removal order
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import difflib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import removal_plan  # noqa: E402
from removal_plan import Graph, NON_PLATFORM  # noqa: E402
from dolrecon import platform_deps  # noqa: E402

RENAME_KINDS = ("sdk-public", "sdk-static")
RENAME_CONF = ("PROVEN", "INFERRED")


def load_csv(path):
    return list(csv.DictReader(open(path, encoding="utf-8")))


def symbols_patch(sym_path, rows):
    want = {int(r["address"], 16): r["proposed_name"] for r in rows
            if r["name_kind"] in RENAME_KINDS and r["confidence"] in RENAME_CONF and r["proposed_name"]}
    old = open(sym_path, encoding="utf-8", newline="").read().split("\n")
    new, used = [], set()
    for line in old:
        m = re.match(r"^(\S+) = \.\w+:0x([0-9A-Fa-f]{8});", line)
        if m and int(m.group(2), 16) in want:
            a = int(m.group(2), 16)
            line = want[a] + line[len(m.group(1)):]
            used.add(a)
        new.append(line)
    absent = sorted(set(want) - used)
    diff = difflib.unified_diff(old, new, "a/config/symbols.txt", "b/config/symbols.txt", lineterm="", n=0)
    return "\n".join(diff) + "\n", len(used), absent


def derived_service(rows, names_by_addr):
    """The service the manifest generator derives from the NEW names (LIBRARY regexes, then link order)."""
    out = {}
    for r in rows:
        a = int(r["address"], 16)
        if r["name_kind"] in RENAME_KINDS and r["confidence"] in RENAME_CONF and r["proposed_name"]:
            out[a] = platform_deps.library_of(r["proposed_name"])
        elif r["name_kind"] == "existing":
            out[a] = platform_deps.library_of(r["proposed_name"])
    return out


def build_services(dolrecon, overrides):
    man = json.load(open(os.path.join(dolrecon, "pal-v30", "platform_manifest.json")))
    svc = {int(e["address"], 16): e["service"] for e in man["entries"]}
    svc.update(overrides)
    sk = sorted((int(r["address"], 16), int(r["size"] or 0), r["layer"])
                for r in csv.DictReader(open(os.path.join(dolrecon, "skeleton", "functions.csv"), encoding="utf-8")))
    starts = [x[0] for x in sk]

    def of(a):
        i = bisect.bisect_right(starts, a) - 1
        if i < 0 or a >= sk[i][0] + max(sk[i][1], 4):
            return "outside-any-function"
        return svc.get(sk[i][0], "layer:" + sk[i][2])
    return of


def plan(g, service_of):
    live = g.live()
    svc = {r: service_of(r) for r in g.rows}
    platform = lambda r: not svc[r].startswith(NON_PLATFORM)
    services = collections.defaultdict(lambda: {"live_bodies": [], "frontier": {}})
    for r in sorted(live):
        if not platform(r) or g.rows[r][1] == "owner":
            continue
        s = services[svc[r]]
        s["live_bodies"].append(r)
        outside = [(src, via) for src, via in g.into.get(r, ()) if isinstance(src, tuple) or
                   (src in live and not platform(src))]
        if outside:
            s["frontier"][r] = outside
    res = {}
    for name, s in sorted(services.items(), key=lambda kv: -len(kv[1]["live_bodies"])):
        fr = frozenset(s["frontier"])
        freed = live - g.live(own=fr)
        res[name] = {"live_bodies": len(s["live_bodies"]), "frontier": len(fr), "freed": len(freed),
                     "freed_outside_service": sum(1 for r in freed if svc[r] != name),
                     "frontier_rows": sorted(fr)}
    left = {n: frozenset(s["frontier"]) for n, s in services.items()}
    order, owned = [], frozenset()
    pl = lambda own: sum(1 for r in g.live(own=own) if platform(r) and g.rows[r][1] != "owner" and r not in own)
    before = pl(owned)
    while left:
        best = None
        for n, fr in left.items():
            after = pl(owned | fr)
            gain = before - after
            score = (gain / max(len(fr), 1), gain, n)
            if best is None or score > best[0]:
                best = (score, n, after)
        (_, gain, _), n, after = best
        owned |= left.pop(n)
        order.append({"service": n, "frontier": len(services[n]["frontier"]), "freed": gain, "platform_bodies_left": after})
        before = after
    return {"services": res, "order": order,
            "platform_live_bodies": sum(v["live_bodies"] for v in res.values()),
            "platform_frontier": len({r for v in res.values() for r in v["frontier_rows"]})}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--csv", default=os.path.join(HERE, "..", "..", "test_data", "platform", "unidentified_names.csv"))
    ap.add_argument("--graph", required=True, help="live_graph.txt written by removal_plan.py")
    ap.add_argument("--dolrecon-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--skip-replan", action="store_true", help="write only the patches (the re-plan takes minutes)")
    args = ap.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    rows = load_csv(args.csv)

    patch, n_renamed, absent = symbols_patch(
        os.path.join(args.dolrecon_dir, "pal-v30", "config", "symbols.txt"), rows)
    open(os.path.join(args.out, "symbols.patch"), "w", encoding="utf-8", newline="\n").write(patch)

    over = {int(r["address"], 16): r["service"] for r in rows
            if r["service"] != "sdk:unidentified" and r["name_kind"] != "interior"}
    derived = derived_service(rows, None)
    mismatch = {f"0x{a:08X}": {"wanted": over[a], "name": next(r["proposed_name"] for r in rows if int(r["address"], 16) == a),
                               "regex_gives": derived.get(a)}
                for a in sorted(derived) if a in over and derived[a] != over[a]}
    json.dump({"service_by_address": {f"0x{a:08X}": s for a, s in sorted(over.items())},
               "regex_would_not_derive": mismatch, "renamed": n_renamed, "rename_targets_not_in_symbols": absent},
              open(os.path.join(args.out, "service_overrides.json"), "w"), indent=1)

    # rules the manifest generator needs so that the new names land in the proposed services
    pd_path = os.path.join(HERE, "..", "dolrecon", "platform_deps.py")
    old = open(pd_path, encoding="utf-8", newline="").read().split(chr(10))
    rules = ["    # sdk-identify: exact-name rules for newly identified SDK statics whose name carries no library prefix",
             "    # (or whose prefix the regexes below misread, e.g. SPEC2_ matches the audio SP rule)"]
    for a, m in sorted(mismatch.items()):
        rules.append(f'    (r"^{re.escape(m["name"])}$", "{m["wanted"]}"),  # {a}')
    i = next(k for k, l in enumerate(old) if l.startswith("LIBRARY = ["))
    new = old[:i + 1] + rules + old[i + 1:]
    udiff = difflib.unified_diff(old, new, "a/tools/dolrecon/platform_deps.py", "b/tools/dolrecon/platform_deps.py",
                                 lineterm="", n=2)
    open(os.path.join(args.out, "platform_deps.patch"), "w", encoding="utf-8", newline="").write(chr(10).join(udiff) + chr(10))
    if args.skip_replan:
        print(f"renames in patch {n_renamed}; absent {len(absent)}; service rules {len(mismatch)}")
        return 0

    g = Graph(args.graph)
    gen_live = {r for r, v in g.rows.items() if v[2] == "live"}
    if g.live() != gen_live:
        raise SystemExit("planner liveness differs from the generator's graph")
    before = plan(g, build_services(args.dolrecon_dir, {}))
    after = plan(g, build_services(args.dolrecon_dir, over))
    json.dump({"before": before, "after": after}, open(os.path.join(args.out, "replan.json"), "w"), indent=1)

    md = ["# Re-plan with the proposed services (generated by apply_unidentified_names.py)", "",
          f"platform bodies {before['platform_live_bodies']} -> {after['platform_live_bodies']}, frontier "
          f"{before['platform_frontier']} -> {after['platform_frontier']}", "",
          "| service | bodies before | bodies after | frontier before | frontier after | freed before | freed after |",
          "|---|---:|---:|---:|---:|---:|---:|"]
    for name in sorted(set(before["services"]) | set(after["services"]),
                       key=lambda n: -after["services"].get(n, {"live_bodies": 0})["live_bodies"]):
        b = before["services"].get(name, {"live_bodies": 0, "frontier": 0, "freed": 0})
        a = after["services"].get(name, {"live_bodies": 0, "frontier": 0, "freed": 0})
        md.append(f"| {name} | {b['live_bodies']} | {a['live_bodies']} | {b['frontier']} | {a['frontier']} | {b['freed']} | {a['freed']} |")
    for title, p in (("before", before), ("after", after)):
        md += ["", f"## Greedy removal order {title}", "", "| step | service | frontier | freed | left |", "|---:|---|---:|---:|---:|"]
        for i, o in enumerate(p["order"], 1):
            md.append(f"| {i} | {o['service']} | {o['frontier']} | {o['freed']} | {o['platform_bodies_left']} |")
    open(os.path.join(args.out, "replan.md"), "w", encoding="utf-8", newline="\n").write("\n".join(md) + "\n")
    print(f"renames in patch {n_renamed}; rename targets absent from symbols.txt {len(absent)}; "
          f"service-by-address {len(over)}; regex would not derive {len(mismatch)}")
    print("\n".join(md[:40]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
