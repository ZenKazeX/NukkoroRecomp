#!/usr/bin/env python3
"""Who still calls into each native owner file? Translated callers of the guest entries a runtime file owns.

A native owner serves a guest entry address: translated code that calls that address crosses into native code.
A runtime file whose entries no translated code calls any more (and no data word or root names) exists only for
callers that are gone - or for native callers - and is the cheapest to retire. This ranks the files.

  owned entries   from the owner lists (extract/native-owners*.txt): a comment line that starts with
                  "# runtime/<path>" opens a section, the address lines after it belong to that file
  callers         from a `ppcrecomp-live-graph-v1` file: live translated bodies with an edge into an owned row,
                  grouped by the layer / platform service of the caller when --dolrecon-dir is given
                  (pal-v30/platform_manifest.json + skeleton/functions.csv, as tools/platform/what_if.py reads them)
  other pins      data words holding an owned address, liveness roots on it

Not covered, and said in the report: hooks that can decline (install_hle with a guest body kept) are not in the owner
lists; native -> native calls (serve_owner) and native -> guest calls are not edges of the graph.

  python owner_callers.py --graph live_graph.txt --owners extract/native-owners.txt [--owners ...]
                          [--dolrecon-dir DIR] [--files GLOB ...] [--top 12] [--json out.json]

--files keeps only runtime files whose name matches one of the globs (e.g. native_trk.cpp "native_os_*.cpp").
Exit 0; 2 for bad input or a graph that does not reproduce its own liveness.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import csv
import fnmatch
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from closure_report import Graph, InputError  # noqa: E402

SECTION = re.compile(r"^#\s*(runtime/[\w./-]+\.(?:cpp|h|inc))\b")
ADDRESS = re.compile(r"^\s*0x([0-9A-Fa-f]{8})\b\s*(.*)$")
UNNAMED = "(before any runtime file is named)"


def read_owner_lists(paths: list[Path]) -> dict[str, list[tuple[int, str]]]:
    """{runtime file: [(address, text)]}, in file order; an address listed twice is kept once per file."""
    owned: dict[str, list[tuple[int, str]]] = collections.OrderedDict()
    for path in paths:
        current = UNNAMED
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            section = SECTION.match(line)
            if section:
                current = section.group(1)
                continue
            address = ADDRESS.match(line)
            if address:
                entry = (int(address.group(1), 16), address.group(2).strip())
                bucket = owned.setdefault(current, [])
                if all(existing[0] != entry[0] for existing in bucket):
                    bucket.append(entry)
    return owned


class Layers:
    """Layer / platform service of a guest address (None when no dolrecon directory was given)."""

    def __init__(self, dolrecon: Path | None):
        self.enabled = dolrecon is not None
        self.service: dict[int, str] = {}
        self.functions: list[tuple[int, int, str]] = []
        if dolrecon is None:
            return
        manifest = json.loads((dolrecon / "pal-v30" / "platform_manifest.json").read_text(encoding="utf-8"))
        self.service = {int(entry["address"], 16): entry["service"] for entry in manifest["entries"]}
        with (dolrecon / "skeleton" / "functions.csv").open(newline="", encoding="utf-8") as handle:
            self.functions = sorted((int(row["address"], 16), int(row["size"] or 0), row["layer"])
                                    for row in csv.DictReader(handle))
        self.starts = [function[0] for function in self.functions]

    def of(self, address: int) -> str:
        if not self.enabled:
            return "translated"
        index = bisect.bisect_right(self.starts, address) - 1
        if index < 0 or address >= self.functions[index][0] + max(self.functions[index][1], 4):
            return "outside-any-function"
        start = self.functions[index][0]
        return self.service.get(start, "layer:" + self.functions[index][2])


def analyse(graph: Graph, owned: dict[str, list[tuple[int, str]]], layers: Layers) -> list[dict]:
    incoming: dict[int, set[int]] = collections.defaultdict(set)   # row -> live translated caller rows
    for source, targets in graph.edges.items():
        row = graph.rows.get(source)
        if row is None or not row["live"] or row["kind"] != "body":
            continue
        for target in targets:
            incoming[target].add(source)
    results = []
    for name, entries in owned.items():
        rows: set[int] = set()
        missing, not_owner = [], []
        for address, _ in entries:
            row = graph.row_containing(address)
            if row is None:
                missing.append(address)
                continue
            rows.add(row)
            if graph.rows[row]["kind"] != "owner":
                not_owner.append(address)
        callers: dict[int, set[int]] = collections.defaultdict(set)   # caller row -> owned rows it calls
        for row in rows:
            for caller in incoming.get(row, ()):
                if caller not in rows:
                    callers[caller].add(row)
        by_layer = collections.Counter(layers.of(caller) for caller in callers)
        seeds = collections.Counter(kind for row in rows for kind, _ in graph.seeds.get(row, ()))
        called = {row for targets in callers.values() for row in targets}
        results.append({
            "file": name, "entries": len(entries), "rows": sorted(rows), "missing": missing, "not_owner": not_owner,
            "callers": {caller: sorted(targets) for caller, targets in callers.items()},
            "by_layer": dict(by_layer), "data_words": seeds.get("data", 0), "roots": seeds.get("root", 0),
            "entries_called": len(called), "entries_uncalled": sorted(rows - called),
        })
    results.sort(key=lambda item: (len(item["callers"]), item["data_words"], item["file"]))
    return results


def label(graph: Graph, row: int) -> str:
    name = graph.rows[row]["name"]
    return f"0x{row:08X} {name}" if name and name.lower() != f"fn_{row:08x}" else f"0x{row:08X}"


def render(graph: Graph, results: list[dict], layers: Layers, source: str, top: int) -> str:
    out = ["# Translated callers of native owner files", "",
           f"Graph: {source}. Sorted by the number of distinct live translated functions that call into the file's "
           f"owned entries (fewest first).", "",
           "Roots: liveness roots on the owned rows. The runtime names every address it installs, so each owned entry "
           "normally has one; the column tells nothing about remaining guest callers. Data words are guest data "
           "(vtables, callback tables) that still hold an owned address.", ""]
    out += ["| Runtime file | Owned entries | Entries still called | Translated callers | Data words | Roots |"
            + (" Callers by layer / service |" if layers.enabled else ""),
            "|---|---:|---:|---:|---:|---:|" + ("---|" if layers.enabled else "")]
    for item in results:
        split = ""
        if layers.enabled:
            split = " " + ", ".join(f"{name} {count}" for name, count in
                                    sorted(item["by_layer"].items(), key=lambda pair: (-pair[1], pair[0]))) + " |"
        out.append(f"| {item['file']} | {item['entries']} | {item['entries_called']} | {len(item['callers'])} | "
                   f"{item['data_words']} | {item['roots']} |{split}")
    out.append("")
    for item in results:
        out += [f"## {item['file']}", ""]
        notes = []
        if item["missing"]:
            notes.append(f"{len(item['missing'])} listed addresses are in no row of the graph: "
                         + ", ".join(f"0x{a:08X}" for a in item["missing"][:6]))
        if item["not_owner"]:
            notes.append(f"{len(item['not_owner'])} listed addresses lie in a row the graph does not mark as owner: "
                         + ", ".join(f"0x{a:08X}" for a in item["not_owner"][:6]))
        out += [f"NOTE: {note}" for note in notes]
        if not item["callers"]:
            out += ["No live translated function calls any entry of this file" +
                    (f" ({item['data_words']} data words and {item['roots']} roots still name them)."
                     if item["data_words"] or item["roots"] else "; no data word or root names one either."), ""]
            continue
        ranked = sorted(item["callers"].items(), key=lambda pair: (-len(pair[1]), pair[0]))
        shown = ranked if top <= 0 else ranked[:top]
        out += ["| Translated caller | " + ("Layer / service | " if layers.enabled else "") + "Owned entries it calls |",
                "|---|" + ("---|" if layers.enabled else "") + "---|"]
        for caller, targets in shown:
            names = ", ".join(label(graph, target) for target in targets[:5]) + (" ..." if len(targets) > 5 else "")
            out.append(f"| {label(graph, caller)} | " + (f"{layers.of(caller)} | " if layers.enabled else "") + f"{names} |")
        if len(shown) < len(ranked):
            out.append(f"| ... {len(ranked) - len(shown)} more | " + ("| " if layers.enabled else "") + "|")
        if item["entries_uncalled"]:
            uncalled = item["entries_uncalled"]
            out += ["", f"Entries no translated function calls ({len(uncalled)}): "
                    + ", ".join(label(graph, row) for row in uncalled[:12]) + (" ..." if len(uncalled) > 12 else "")]
        out.append("")
    out += ["Not covered: hooks that can decline (their guest body stays and they are not in the owner lists), "
            "native -> native calls (`serve_owner`) and native -> guest calls. An entry with no translated caller may "
            "still be entered through a data word, a root, or by native code.", ""]
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--graph", required=True)
    parser.add_argument("--owners", action="append", required=True, type=Path, help="owner list (repeatable)")
    parser.add_argument("--dolrecon-dir", type=Path, help="tools/dolrecon outputs: name the callers' layer / service")
    parser.add_argument("--files", nargs="*", default=[], help="keep runtime files whose base name matches a glob")
    parser.add_argument("--top", type=int, default=12, help="callers listed per file; 0 = all (default 12)")
    parser.add_argument("--json", dest="json_out")
    arguments = parser.parse_args(argv)
    try:
        with open(arguments.graph, encoding="utf-8") as handle:
            graph = Graph(handle)
        problems = graph.self_check()
        if problems:
            raise InputError("the graph does not reproduce its own liveness: " + "; ".join(problems))
        owned = read_owner_lists(arguments.owners)
        if arguments.files:
            owned = {name: entries for name, entries in owned.items()
                     if any(fnmatch.fnmatchcase(name.rsplit("/", 1)[-1], glob) for glob in arguments.files)}
        if not owned:
            raise InputError("no owner section selected")
        layers = Layers(arguments.dolrecon_dir)
    except (OSError, ValueError, KeyError, InputError) as error:
        print(f"owner_callers: {error}", file=sys.stderr)
        return 2
    results = analyse(graph, owned, layers)
    sys.stdout.write(render(graph, results, layers, arguments.graph, arguments.top) + "\n")
    if arguments.json_out:
        payload = [{**item, "callers": {f"{caller:08X}": targets for caller, targets in item["callers"].items()}}
                   for item in results]
        Path(arguments.json_out).write_text(json.dumps(payload, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
