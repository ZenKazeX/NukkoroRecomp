#!/usr/bin/env python3
"""Dependency closure of a set of guest functions, from the generator's own liveness graph.

For a subsystem S given as guest addresses, ranges or a name pattern, this answers what a migration step needs to
know before it is written:

  members    the rows of S: live translated bodies, native owners, already pruned
  pinned by  everything that enters S from outside: translated callers (with the address they enter, which the graph
             records per edge; it is the member's entry or an address inside it), liveness
             roots (addresses the runtime / census names), data words that hold an S address, the DOL entry.
             S can leave the image only when each of these is gone or served natively.
  reaches    the rows outside S that S calls: native owners (already native) and translated bodies (guest code S
             still depends on; a native replacement of S must call them, reimplement them or take them along)
  frees      the translated bodies that become unreachable once S is native (S's rows no longer call into the guest):
             computed by re-running the generator's reachability with S's outgoing edges removed, not estimated.

Input: a `ppcrecomp-live-graph-v1` file (ppcrecomp --live-graph; tools/platform/removal_plan.py writes one as
<out>/live_graph.txt). The graph is the generator's pruning pass: seeds (entry, roots, data words) and row-to-row
edges; an owner row has no outgoing edges. The tool first checks that plain reachability over the file reproduces
the file's own live / pruned marks and refuses to report when it does not (exit 2): a report from a model that
disagrees with the generator would be a guess.

Limits, stated in the report: the graph has no native -> guest edges (see guest_callsites.py: a native owner that
calls a guest function shows here only as a liveness root), and it is as old as the file - give the graph of the
build you are planning against.

  python closure_report.py --graph live_graph.txt (--addr HEX ... | --range LO-HI ... | --name REGEX | --list FILE)
                           [--label TEXT] [--top 40] [--json out.json]

Exit 0; 2 for bad input, an empty selection or a graph that does not reproduce its own liveness.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import json
import re
import sys
from pathlib import Path


class InputError(Exception):
    pass


class Graph:
    def __init__(self, lines):
        self.rows: dict[int, dict] = {}
        self.edges: dict[int, set[int]] = collections.defaultdict(set)      # from row -> to rows
        self.calls: dict[tuple[int, int], int] = {}                         # (from row, to row) -> address entered
        self.seeds: dict[int, list[tuple[str, int]]] = collections.defaultdict(list)   # row -> [(kind, via)]
        iterator = iter(lines)
        if next(iterator, "").strip() != "ppcrecomp-live-graph-v1":
            raise InputError("not a ppcrecomp-live-graph-v1 file")
        for number, line in enumerate(iterator, start=2):
            parts = line.split()
            if not parts:
                continue
            try:
                if parts[0] == "row":
                    self.rows[int(parts[1], 16)] = {
                        "address": int(parts[1], 16), "size": int(parts[2]), "kind": parts[3],
                        "live": parts[4] == "live", "name": parts[5] if len(parts) > 5 else ""}
                elif parts[0] == "edge":
                    source, via, target = int(parts[1], 16), int(parts[2], 16), int(parts[3], 16)
                    self.edges[source].add(target)
                    self.calls.setdefault((source, target), via)
                elif parts[0] == "seed":
                    self.seeds[int(parts[3], 16)].append((parts[1], int(parts[2], 16)))
                else:
                    raise ValueError(f"unknown record '{parts[0]}'")
            except (ValueError, IndexError) as error:
                raise InputError(f"line {number}: {error}") from error
        self.starts = sorted(self.rows)

    def row_containing(self, address: int) -> int | None:
        index = bisect.bisect_right(self.starts, address) - 1
        if index >= 0:
            start = self.starts[index]
            if start <= address < start + max(self.rows[start]["size"], 1):
                return start
        return None

    def reachable(self, cut: frozenset[int] = frozenset()) -> set[int]:
        """Rows reached from the seeds; rows in `cut` are reached but call nothing (as native owners do)."""
        seen = set(self.seeds)
        work = list(seen)
        while work:
            row = work.pop()
            if row in cut or self.rows.get(row, {}).get("kind") == "owner":
                continue
            for target in self.edges.get(row, ()):
                if target not in seen:
                    seen.add(target)
                    work.append(target)
        return seen

    def self_check(self) -> list[str]:
        reached = self.reachable()
        live = {start for start, row in self.rows.items() if row["live"]}
        problems = []
        if reached - live:
            problems.append(f"{len(reached - live)} rows are reachable in the file but marked pruned "
                            f"(first 0x{min(reached - live):08X})")
        if live - reached:
            problems.append(f"{len(live - reached)} rows are marked live but not reachable from the file's seeds "
                            f"(first 0x{min(live - reached):08X})")
        return problems


def select(graph: Graph, addresses, ranges, pattern, listed) -> tuple[set[int], list[str]]:
    chosen: set[int] = set()
    notes = []
    for address in list(addresses) + list(listed):
        row = graph.row_containing(address)
        if row is None:
            notes.append(f"0x{address:08X} is not inside any row of the graph")
        else:
            chosen.add(row)
    for low, high in ranges:
        chosen.update(start for start in graph.starts[bisect.bisect_left(graph.starts, low):] if start < high)
    if pattern:
        expression = re.compile(pattern)
        chosen.update(start for start, row in graph.rows.items() if expression.search(row["name"]))
    return chosen, notes


def analyse(graph: Graph, members: set[int]) -> dict:
    rows = graph.rows
    live_bodies = {m for m in members if rows[m]["live"] and rows[m]["kind"] == "body"}
    callers = collections.defaultdict(list)   # outside row -> [(member, via)]
    for source, targets in graph.edges.items():
        if source in members or not rows.get(source, {}).get("live"):
            continue
        for target in targets & members:
            callers[source].append((target, graph.calls[(source, target)]))
    seeds = {"root": [], "data": [], "entry": []}
    for member in sorted(members):
        for kind, via in graph.seeds.get(member, ()):
            seeds.setdefault(kind, []).append((member, via))
    reaches = collections.defaultdict(list)   # outside row -> [member]
    for member in live_bodies:
        for target in graph.edges.get(member, ()):
            if target not in members:
                reaches[target].append(member)
    before = graph.reachable()
    after = graph.reachable(frozenset(members))
    freed = {row for row in before - after if row not in members and rows[row]["kind"] == "body"}
    return {
        "members": members, "live_bodies": live_bodies, "callers": callers, "seeds": seeds, "reaches": reaches,
        "freed": freed,
        "bytes_members": sum(rows[m]["size"] for m in live_bodies),
        "bytes_freed": sum(rows[r]["size"] for r in freed),
        "bytes_live_bodies_total": sum(r["size"] for r in rows.values() if r["live"] and r["kind"] == "body"),
        "live_bodies_total": sum(1 for r in rows.values() if r["live"] and r["kind"] == "body"),
    }


def label(graph: Graph, row: int) -> str:
    name = graph.rows[row]["name"]
    return f"0x{row:08X} {name}" if name and name.lower() != f"fn_{row:08x}" else f"0x{row:08X}"


def render(graph: Graph, result: dict, title: str, source: str, notes: list[str], top: int) -> str:
    rows = graph.rows
    members = result["members"]
    owners = sum(1 for m in members if rows[m]["kind"] == "owner")
    pruned = sum(1 for m in members if not rows[m]["live"])
    limit = (lambda items: items) if top <= 0 else (lambda items: items[:top])
    out = [f"# Dependency closure: {title}", "", f"Graph: {source} ({len(rows):,} rows, {result['live_bodies_total']:,} live "
           f"translated bodies, {result['bytes_live_bodies_total']:,} guest bytes).", ""]
    out += [f"NOTE: {note}" for note in notes]
    out += ["## Members", "",
            f"{len(members)} rows: {len(result['live_bodies'])} live translated bodies ({result['bytes_members']:,} guest "
            f"bytes), {owners} native owners, {pruned} already pruned.", ""]
    callers, seeds = result["callers"], result["seeds"]
    out += ["## Pinned by", "",
            f"{len(callers)} translated callers outside the set, {len(seeds['root'])} liveness roots, "
            f"{len(seeds['data'])} data words, {len(seeds['entry'])} DOL entry.", ""]
    if callers:
        out += ["| Caller (outside) | Calls into | Address entered |", "|---|---|---|"]
        ordered = sorted(callers.items(), key=lambda item: (-len(item[1]), item[0]))
        for source_row, hits in limit(ordered):
            targets = ", ".join(label(graph, member) for member, _ in sorted(hits)[:4]) + (" ..." if len(hits) > 4 else "")
            sites = ", ".join(f"0x{via:08X}" for _, via in sorted(hits)[:4]) + (" ..." if len(hits) > 4 else "")
            out.append(f"| {label(graph, source_row)} | {targets} | {sites} |")
        if 0 < top < len(ordered):
            out.append(f"| ... {len(ordered) - top} more | | |")
        out.append("")
    for kind, heading in (("root", "Liveness roots (an address the runtime or the census names)"),
                          ("data", "Data words holding a member address (vtables, callback tables)"),
                          ("entry", "DOL entry")):
        if seeds[kind]:
            out += [f"### {heading}: {len(seeds[kind])}", ""]
            shown = limit(seeds[kind])
            out.append(", ".join(f"{label(graph, member)}" + ("" if via == member else f" (at 0x{via:08X})")
                                 for member, via in shown) + (f", ... {len(seeds[kind]) - len(shown)} more" if len(shown) < len(seeds[kind]) else ""))
            out.append("")
    reaches = result["reaches"]
    to_owner = sorted(r for r in reaches if rows[r]["kind"] == "owner")
    to_body = sorted(r for r in reaches if rows[r]["kind"] != "owner")
    out += ["## Reaches (outside the set)", "",
            f"{len(to_owner)} native owners (already native) and {len(to_body)} translated bodies "
            f"({sum(rows[r]['size'] for r in to_body):,} guest bytes, direct callees only).", ""]
    if to_body:
        out += ["| Translated callee | Bytes | Called by members |", "|---|---:|---:|"]
        ordered = sorted(to_body, key=lambda r: (-len(reaches[r]), r))
        for row in limit(ordered):
            out.append(f"| {label(graph, row)} | {rows[row]['size']:,} | {len(reaches[row])} |")
        if 0 < top < len(ordered):
            out.append(f"| ... {len(ordered) - top} more | | |")
        out.append("")
    if to_owner:
        shown = limit(to_owner)
        out += ["Native owners called: " + ", ".join(label(graph, r) for r in shown) +
                (f", ... {len(to_owner) - len(shown)} more" if len(shown) < len(to_owner) else ""), ""]
    freed = sorted(result["freed"])
    total = result["bytes_live_bodies_total"]
    gone = result["bytes_members"] + result["bytes_freed"]
    out += ["## Frees", "",
            f"With every member native (no member calls into the guest), {len(freed)} further translated bodies "
            f"({result['bytes_freed']:,} guest bytes) become unreachable. Together with the members' own bodies: "
            f"{len(freed) + len(result['live_bodies'])} bodies, {gone:,} guest bytes = "
            f"{(100.0 * gone / total if total else 0.0):.3f} % of the live translated code.", ""]
    if freed:
        shown = limit(sorted(freed, key=lambda r: (-rows[r]["size"], r)))
        out += ["| Freed body | Bytes |", "|---|---:|"]
        out += [f"| {label(graph, row)} | {rows[row]['size']:,} |" for row in shown]
        if len(shown) < len(freed):
            out.append(f"| ... {len(freed) - len(shown)} more | |")
        out.append("")
    out += ["Not in this graph: native -> guest calls from runtime code (guest_callsites.py). A freed body that a "
            "native owner still calls is held by its liveness root and is therefore NOT counted as freed here; a "
            "member that a native owner calls shows above as a liveness root.", ""]
    return "\n".join(out)


def parse_range(text: str) -> tuple[int, int]:
    low, _, high = text.partition("-")
    return int(low, 16), int(high, 16)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--graph", required=True, help="ppcrecomp-live-graph-v1 file")
    parser.add_argument("--addr", nargs="*", default=[], help="guest addresses (hex); the row containing each is taken")
    parser.add_argument("--range", nargs="*", default=[], dest="ranges", help="LO-HI (hex, HI exclusive): rows starting inside")
    parser.add_argument("--name", help="regular expression on the row name")
    parser.add_argument("--list", dest="list_file", help="file with one hex address per line ('#' comments)")
    parser.add_argument("--label", help="title of the report")
    parser.add_argument("--top", type=int, default=40, help="rows per table; 0 = all (default 40)")
    parser.add_argument("--json", dest="json_out", help="also write the result as JSON")
    arguments = parser.parse_args(argv)
    try:
        with open(arguments.graph, encoding="utf-8") as handle:
            graph = Graph(handle)
        problems = graph.self_check()
        if problems:
            raise InputError("the graph does not reproduce its own liveness: " + "; ".join(problems))
        listed = []
        if arguments.list_file:
            for line in Path(arguments.list_file).read_text(encoding="utf-8").splitlines():
                token = line.split("#", 1)[0].strip().split(",")[0].strip()
                if token:
                    listed.append(int(token, 16))
        members, notes = select(graph, [int(a, 16) for a in arguments.addr], [parse_range(r) for r in arguments.ranges],
                                arguments.name, listed)
        if not members:
            raise InputError("the selection matches no row" + ("; " + "; ".join(notes) if notes else ""))
    except (OSError, ValueError, InputError) as error:
        print(f"closure_report: {error}", file=sys.stderr)
        return 2
    result = analyse(graph, members)
    title = arguments.label or (arguments.name or f"{len(members)} rows")
    sys.stdout.write(render(graph, result, title, arguments.graph, notes, arguments.top) + "\n")
    if arguments.json_out:
        payload = {
            "graph": arguments.graph, "label": title, "notes": notes,
            "members": sorted(members), "live_bodies": sorted(result["live_bodies"]),
            "callers": {f"{k:08X}": [[m, v] for m, v in sorted(hits)] for k, hits in result["callers"].items()},
            "seeds": {k: [[m, v] for m, v in v_list] for k, v_list in result["seeds"].items()},
            "reaches": {f"{k:08X}": sorted(v) for k, v in result["reaches"].items()},
            "freed": sorted(result["freed"]), "bytes_members": result["bytes_members"],
            "bytes_freed": result["bytes_freed"], "bytes_live_bodies_total": result["bytes_live_bodies_total"]}
        Path(arguments.json_out).write_text(json.dumps(payload, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
