"""Plan the removal of the GameCube SDK from the build, from the generator's own liveness graph.

    python -B removal_plan.py [--release-dir DIR] [--out DIR] [--check-what-if NAME ...]

1. Runs ppcrecomp with the release build's exact arguments and --live-graph (every reach event of its pruning pass:
   seeds - the DOL entry, liveness roots, data words - and row-to-row edges; an owner row has no outgoing edges).
2. Classifies every row by the platform manifest: a platform-SDK row (graphics.vi, os.threads, saves.card, ...),
   or not (game, RenderWare, CRI, MSL, compiler runtime).
3. The FRONTIER of a service: its live body rows entered from outside the platform SDK - by a game / RW / CRI row,
   a data word, a runtime literal (with the runtime source files naming it), a census root or the DOL entry. Owning
   every frontier row natively (through the runtime contract) leaves no way into the service's interior: the
   planner computes, by the same reachability the generator uses, how many body rows that frees.
4. --check-what-if NAME compares the planner's prediction with a real generator run made by what_if.py (same step):
   the freed sets must be equal, or the planner is wrong and says so (exit 1).

Writes <out>/removal_plan.json and <out>/REMOVAL_PLAN.md (the work list: per service, every frontier row with its
callers and their locations). Nothing under extract/ is written.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import what_if  # noqa: E402

NON_PLATFORM = ("engine.", "middleware.", "host.", "layer:", "outside-any-function")


class Graph:
    def __init__(self, path):
        self.rows = {}          # start -> (size, kind, state, name)
        self.edges = collections.defaultdict(set)   # from row -> {to rows}
        self.into = collections.defaultdict(set)    # to row -> {(from row | seed tuple, via)}
        self.seeds = collections.defaultdict(set)   # (kind, via) -> {rows}
        with open(path, encoding="utf-8") as f:
            if f.readline().strip() != "ppcrecomp-live-graph-v1":
                raise SystemExit(f"{path}: not a ppcrecomp-live-graph-v1 file")
            for line in f:
                t = line.split()
                if t[0] == "row":
                    self.rows[int(t[1], 16)] = (int(t[2]), t[3], t[4], t[5] if len(t) > 5 else "")
                elif t[0] == "edge":
                    a, via, b = int(t[1], 16), int(t[2], 16), int(t[3], 16)
                    self.edges[a].add(b)
                    self.into[b].add((a, via))
                elif t[0] == "seed":
                    via, b = int(t[2], 16), int(t[3], 16)
                    self.seeds[(t[1], via)].add(b)
                    self.into[b].add(((t[1],), via))

    def live(self, own=frozenset(), drop_seeds=frozenset()):
        """Rows the generator would keep: reachable from the seeds; an owner (existing or added) has no out-edges."""
        seen = set()
        work = []
        for (kind, via), rows in self.seeds.items():
            if (kind, via) in drop_seeds:
                continue
            for r in rows:
                if r not in seen:
                    seen.add(r)
                    work.append(r)
        while work:
            r = work.pop()
            if r in own or self.rows[r][1] == "owner":
                continue
            for b in self.edges.get(r, ()):
                if b not in seen:
                    seen.add(b)
                    work.append(b)
        return seen


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--release-dir", default=os.path.join(SRC, ".build", "release"))
    ap.add_argument("--out", default=os.path.join(SRC, ".build", "platform", "removal_plan"))
    ap.add_argument("--check-what-if", action="append", default=[])
    ap.add_argument("--dolrecon-dir", default=what_if.DOLRECON, help="tools/dolrecon outputs (manifest, skeleton)")
    ap.add_argument("--reuse-graph", action="store_true", help="use <out>/live_graph.txt if it exists")
    ap.add_argument("--control-own", action="append", default=[],
                    help="positive control: run what_if.py owning these rows (a real generator run) and require the "
                         "planner's prediction to equal it and to free at least one row")
    ap.add_argument("--max-platform-bodies", type=int, default=None,
                    help="coupling ratchet: fail if more platform-SDK bodies are compiled than this (lower it as "
                         "services are removed; a new hook or literal that pins SDK code raises the count)")
    args = ap.parse_args(argv)
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)

    base_argv = what_if.release_argv(args.release_dir)
    graph_path = os.path.join(args.out, "live_graph.txt")
    if not (args.reuse_graph and os.path.exists(graph_path)):
        gen = os.path.join(args.out, "generated")
        os.makedirs(gen, exist_ok=True)
        a = list(base_argv)
        a[a.index("--out") + 1] = gen
        a[a.index("--report") + 1] = os.path.join(gen, "recomp_report.txt")
        p = subprocess.run(a + ["--live-graph", graph_path], capture_output=True, text=True, errors="replace")
        open(os.path.join(args.out, "stderr.log"), "w").write(p.stderr)
        if p.returncode != 0:
            raise SystemExit(f"ppcrecomp failed ({p.returncode}); see {args.out}/stderr.log")
        for f in os.listdir(gen):
            if f.startswith("recomp_") and f.endswith(".cpp"):
                os.remove(os.path.join(gen, f))
    g = Graph(graph_path)

    # the planner's liveness must equal the generator's before it predicts anything
    live = g.live()
    gen_live = {r for r, v in g.rows.items() if v[2] == "live"}
    if live != gen_live:
        raise SystemExit(f"planner liveness differs from the generator's: {len(live - gen_live)} extra, "
                         f"{len(gen_live - live)} missing")

    service_of = what_if.services(args.dolrecon_dir)
    svc = {r: service_of(r) for r in g.rows}
    platform = lambda r: not svc[r].startswith(NON_PLATFORM)
    literal_files = what_if.roots_by_file()
    roots_path = base_argv[base_argv.index("--prune-roots") + 1]
    census = {int(l.split()[0], 16) for l in open(roots_path, encoding="utf-8")
              if l.strip() and not l.startswith("#") and "census" in l}

    def describe(src, via):
        if isinstance(src, tuple):
            kind = src[0]
            if kind == "root":
                if via in literal_files:
                    return "runtime-literal", f"0x{via:08X} in " + ", ".join(sorted(literal_files[via]))
                return "census-root", f"0x{via:08X}" + (" (census)" if via in census else "")
            if kind == "data":
                return "data-word", f"word at 0x{via:08X}"
            return "dol-entry", f"0x{via:08X}"
        return "code:" + svc[src].replace("layer:", ""), f"0x{src:08X} {g.rows[src][3]} (via 0x{via:08X})"

    services = collections.defaultdict(lambda: {"live_bodies": [], "frontier": {}})
    for r in sorted(live):
        if not platform(r) or g.rows[r][1] == "owner":
            continue
        s = services[svc[r]]
        s["live_bodies"].append(r)
        outside = [(src, via) for src, via in g.into.get(r, ()) if isinstance(src, tuple) or
                   (src in live and not platform(src))]
        if outside:
            s["frontier"][r] = sorted({describe(src, via) for src, via in outside})

    # what owning a service's whole frontier frees (by itself), and all services together
    all_frontier = {r for s in services.values() for r in s["frontier"]}
    after_all = g.live(own=frozenset(all_frontier))
    result = {"graph": graph_path, "rows": len(g.rows), "live": len(live),
              "platform_live_bodies": sum(len(s["live_bodies"]) for s in services.values()),
              "platform_frontier": len(all_frontier),
              "platform_bodies_left_if_all_frontier_owned":
                  sum(1 for r in after_all if platform(r) and g.rows[r][1] != "owner" and r not in all_frontier),
              "services": {}}
    for name, s in sorted(services.items(), key=lambda kv: -len(kv[1]["live_bodies"])):
        fr = frozenset(s["frontier"])
        after = g.live(own=fr)
        freed = sorted(r for r in live - after)
        kinds = collections.Counter(d[0] for v in s["frontier"].values() for d in v)
        result["services"][name] = {
            "live_bodies": len(s["live_bodies"]), "frontier": len(fr),
            "freed_if_frontier_owned": len(freed),
            "freed_outside_service": sum(1 for r in freed if svc[r] != name),
            "frontier_entered_by": dict(kinds),
            "frontier_rows": {f"0x{r:08X} {g.rows[r][3]}": [f"{k}: {d}" for k, d in v]
                              for r, v in sorted(s["frontier"].items())}}

    # A removal order: greedily the service whose frontier, owned on top of the ones before it, frees the most SDK
    # bodies per frontier row (services reach each other's interiors, so one alone often frees little).
    order, owned_so_far, left = [], frozenset(), dict((n, frozenset(s["frontier"])) for n, s in services.items())
    platform_left = lambda own: sum(1 for r in g.live(own=own) if platform(r) and g.rows[r][1] != "owner" and r not in own)
    before = platform_left(owned_so_far)
    while left:
        best = None
        for n, fr in left.items():
            after_n = platform_left(owned_so_far | fr)
            gain = before - after_n
            score = (gain / max(len(fr), 1), gain, n)
            if best is None or score > best[0]:
                best = (score, n, after_n)
        (_, gain, _), n, after_n = best
        owned_so_far |= left.pop(n)
        order.append({"service": n, "frontier": len(services[n]["frontier"]), "freed": gain,
                      "platform_bodies_left": after_n})
        before = after_n
    result["removal_order"] = order

    checks = []
    if args.control_own:
        control = os.path.join(args.out, "control")
        if what_if.main(["--name", "control", "--out", control, "--release-dir", args.release_dir,
                         "--dolrecon-dir", args.dolrecon_dir] +
                        [x for a in args.control_own for x in ("--own", a)]) != 0:
            raise SystemExit("the control's generator run failed")
        args.check_what_if.append(os.path.join(control, "control"))
    for name in args.check_what_if:
        path = name if os.path.isabs(name) else os.path.join(SRC, ".build", "platform", "what_if", name)
        wi = json.load(open(os.path.join(path, "what_if.json")))
        own = frozenset(int(x, 16) for x in wi["step"]["own"])
        drop = {int(x, 16) for x in wi["step"]["drop_roots"]}
        add = [int(x, 16) for x in wi["step"]["add_roots"]]
        if add:
            checks.append({"what_if": name, "skipped": "adds roots (the planner cannot place a new seed's edges)"})
            continue
        pred = live - g.live(own=own, drop_seeds=frozenset(("root", v) for v in drop))
        real = {int(x.split()[0], 16) for v in wi["freed_by_service"].values() for x in v["rows"]}
        checks.append({"what_if": name, "predicted": len(pred), "generator": len(real), "equal": pred == real,
                       "only_predicted": [f"0x{r:08X}" for r in sorted(pred - real)][:20],
                       "only_generator": [f"0x{r:08X}" for r in sorted(real - pred)][:20]})
    result["what_if_checks"] = checks
    json.dump(result, open(os.path.join(args.out, "removal_plan.json"), "w"), indent=1)

    md = ["# GameCube SDK removal work list (generated by tools/platform/removal_plan.py; do not edit)", "",
          f"Live rows {result['live']} of {result['rows']}; platform-SDK body rows still compiled: "
          f"{result['platform_live_bodies']}; frontier rows (entered from outside the SDK): "
          f"{result['platform_frontier']}; SDK bodies left if every frontier row were owned: "
          f"{result['platform_bodies_left_if_all_frontier_owned']}.", "",
          "| service | compiled bodies | frontier | freed if its frontier is owned | entered by |", "|---|---:|---:|---:|---|"]
    for name, s in result["services"].items():
        md.append(f"| {name} | {s['live_bodies']} | {s['frontier']} | {s['freed_if_frontier_owned']} | "
                  + ", ".join(f"{k} {v}" for k, v in sorted(s["frontier_entered_by"].items(), key=lambda kv: -kv[1])) + " |")
    md += ["", "## Removal order (cumulative: each step owns that service's frontier on top of the steps before)", "",
           "| step | service | frontier rows to own | SDK bodies freed | SDK bodies left |", "|---:|---|---:|---:|---:|"]
    for i, o in enumerate(result["removal_order"], 1):
        md.append(f"| {i} | {o['service']} | {o['frontier']} | {o['freed']} | {o['platform_bodies_left']} |")
    for name, s in result["services"].items():
        md += ["", f"## {name}", ""]
        for row, why in s["frontier_rows"].items():
            md.append(f"- `{row}`")
            md += [f"  - {w}" for w in why[:12]] + ([f"  - ... {len(why) - 12} more"] if len(why) > 12 else [])
    open(os.path.join(args.out, "REMOVAL_PLAN.md"), "w", encoding="utf-8", newline="\n").write("\n".join(md) + "\n")

    print(f"platform bodies compiled {result['platform_live_bodies']}, frontier {result['platform_frontier']}, "
          f"left if all frontier owned {result['platform_bodies_left_if_all_frontier_owned']}")
    for name, s in result["services"].items():
        print(f"  {name:30} bodies {s['live_bodies']:4}  frontier {s['frontier']:4}  frees {s['freed_if_frontier_owned']:4}  "
              + ", ".join(f"{k} {v}" for k, v in sorted(s["frontier_entered_by"].items(), key=lambda kv: -kv[1])))
    bad = [c for c in checks if c.get("equal") is False]
    # a positive control that frees nothing proves nothing
    control_name = os.path.join(args.out, "control", "control") if args.control_own else None
    bad += [c for c in checks if c["what_if"] == control_name and c.get("generator", 0) == 0]
    for c in checks:
        print("  what-if check", c)
    ratchet = args.max_platform_bodies is not None and result["platform_live_bodies"] > args.max_platform_bodies
    if ratchet:
        print(f"COUPLING: {result['platform_live_bodies']} platform-SDK bodies compiled, more than the ratchet's "
              f"{args.max_platform_bodies}; see {args.out}/REMOVAL_PLAN.md for what enters them")
    return 1 if bad or ratchet else 0


if __name__ == "__main__":
    sys.exit(main())
