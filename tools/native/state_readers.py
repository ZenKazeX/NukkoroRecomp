#!/usr/bin/env python3
"""Side-effect inventory of a set of guest functions: the fixed-address state their ORIGINAL bodies write, and
every reader of that state that is still in the image.

For an owner that is a candidate for retirement, or a call chain a native replacement must stand in for. The
question is which of the original writes anything still depends on.

  writers   the rows given (--addr / --list), live, owned or pruned alike: every store of their original
            instructions whose address is proven (absolute or small-data; the must-analysis of
            tools/dolrecon/vcalls.py, the same scan as tools/platform/retired_readers.py), grouped as guest RAM
            or memory-mapped hardware register (0xCC00....)
  readers   translated: every load at a proven address in a live translated body outside the set that overlaps
            a write
            native:     runtime sources that name an overlapping absolute address (the liveness-root literal
            scan), and - as a hint only - runtime source lines that mention the small-data offset of a written
            word (the runtime usually spells such a word "sda + 0x5D58")

What it cannot place is counted, not dropped: stores of the set through unproven pointers (object fields, indexed
forms, the stack) and loads of surviving rows through unproven pointers. A word with no proven reader may still be
read through a pointer; the dynamic watch (--watch-guest) covers that half.

tools/platform/retired_readers.py answers the same question for an ownership step and only for rows that are live
before it; this takes any rows, including bodies the generator already prunes behind a native owner.

  python state_readers.py --graph live_graph.txt --dolrecon-dir DIR (--addr HEX ... | --list FILE) [--label TEXT]
                          [--json out.json]

Exit 0; 2 for bad input.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "platform"))
sys.path.insert(0, str(HERE.parent))
from closure_report import Graph, InputError  # noqa: E402

MMIO = (0xCC000000, 0xCC010000)
SHIPPING = ("runtime/src/", "runtime/include/", "runtime/lift/", "runtime/backends/", "runtime/game/", "runtime/scaffold/")
SOURCE_SUFFIXES = {".cpp", ".h", ".inc", ".hpp"}


def merge_spans(writes: list[tuple[int, int]]) -> list[tuple[int, int]]:
    spans = sorted({(ea, ea + width) for ea, width in writes})
    return spans


def overlap_finder(spans: list[tuple[int, int]]):
    starts = [start for start, _ in spans]

    def find(ea: int, width: int):
        index = bisect.bisect_right(starts, ea + width - 1) - 1
        while index >= 0 and spans[index][1] > ea - 64:
            if spans[index][0] < ea + width and ea < spans[index][1]:
                return spans[index]
            index -= 1
        return None
    return find


def sda_offset_hints(root: Path, offsets: dict[int, int]) -> dict[int, list[str]]:
    """{written address: ["file:line: text"]} for runtime lines that mention its small-data offset as 0xNNNN next to
    the word 'sda' or 'r13' - a hint for a reader of that word, never proof."""
    hints: dict[int, list[str]] = collections.defaultdict(list)
    if not offsets:
        return hints
    patterns = {ea: re.compile(rf"\b0x0*{offset:X}u?\b", re.IGNORECASE) for ea, offset in offsets.items()}
    for path in sorted(root.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        relative = path.relative_to(root.parent).as_posix()
        if not relative.startswith(SHIPPING):
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            low = line.lower()
            if "sda" not in low and "r13" not in low:
                continue
            for ea, pattern in patterns.items():
                if pattern.search(line):
                    hints[ea].append(f"{relative}:{number}: {line.strip()[:150]}")
    return hints


def analyse(graph: Graph, members: set[int], dolrecon: Path, runtime_root: Path) -> dict:
    import retired_readers  # tools/platform: the instruction scan
    import what_if
    from dolrecon import dol as dolmod, spine, strrefs

    project = dolrecon / "pal-v30"
    image = dolmod.load_file(str(project / "main.dol"))
    _, objects = spine.load_symbols(str(project / "config" / "symbols.txt"))
    text = next(section for section in image.sections if section.kind == "text")
    r2, r13 = strrefs.sda_bases(image, text.address, text.address + text.size)
    sda = {register: value for register, value in ((2, r2), (13, r13)) if value is not None}
    named = sorted((address, name, size) for name, (address, size, *_) in objects.items())
    named_starts = [entry[0] for entry in named]

    def name_of(ea: int):
        index = bisect.bisect_right(named_starts, ea) - 1
        if index >= 0 and ea < named[index][0] + max(named[index][2], 1):
            return f"{named[index][1]}+0x{ea - named[index][0]:X}"
        return None

    writes = collections.defaultdict(list)          # (ea, width) -> [(row, pc)]
    unproven_stores = collections.Counter()
    for row in sorted(members):
        proven, unproven = retired_readers.accesses(image, sda, row, graph.rows[row]["size"])
        for pc, kind, ea, width in proven:
            if kind == "store":
                writes[(ea, width)].append((row, pc))
        unproven_stores[row] += sum(1 for _, kind in unproven if kind == "store")
    find = overlap_finder(merge_spans(list(writes)))

    readers = []
    unproven_loads = 0
    surviving = [start for start, row in graph.rows.items()
                 if row["live"] and row["kind"] == "body" and start not in members]
    for row in sorted(surviving):
        proven, unproven = retired_readers.accesses(image, sda, row, graph.rows[row]["size"])
        unproven_loads += sum(1 for _, kind in unproven if kind == "load")
        for pc, kind, ea, width in proven:
            if kind == "load" and find(ea, width):
                readers.append({"row": row, "name": graph.rows[row]["name"], "pc": pc, "ea": ea, "width": width})

    native = []
    for address, files in sorted(what_if.roots_by_file().items()):
        shipping = sorted(f for f in files if not f.startswith("runtime/tests/"))
        if shipping and find(address, 1):
            native.append({"address": address, "files": shipping})
    offsets = {ea: ea - r13 for (ea, _width) in writes if r13 is not None and 0 <= ea - r13 + 0x8000 < 0x10000
               and not MMIO[0] <= ea < MMIO[1]}
    hints = sda_offset_hints(runtime_root, {ea: off & 0xFFFF if off >= 0 else off for ea, off in offsets.items() if off >= 0})

    rows = []
    for (ea, width), sites in sorted(writes.items()):
        rows.append({
            "ea": ea, "width": width, "object": name_of(ea),
            "kind": "hardware register" if MMIO[0] <= ea < MMIO[1] else "guest RAM",
            "sda_offset": offsets.get(ea),
            "writers": sorted({row for row, _ in sites}), "sites": sorted(pc for _, pc in sites),
            "translated_readers": [r for r in readers if r["ea"] < ea + width and ea < r["ea"] + r["width"]],
            "native_literal": [n for n in native if ea <= n["address"] < ea + width],
            "native_offset_hints": hints.get(ea, []),
        })
    return {
        "members": sorted(members), "surviving_rows": len(surviving), "writes": rows,
        "unproven_stores": {row: count for row, count in sorted(unproven_stores.items()) if count},
        "surviving_unproven_loads": unproven_loads,
        "sda": {"r2": r2, "r13": r13},
    }


def label(graph: Graph, row: int) -> str:
    name = graph.rows[row]["name"]
    state = "owner" if graph.rows[row]["kind"] == "owner" else ("live" if graph.rows[row]["live"] else "pruned")
    shown = f" {name}" if name and name.lower() != f"fn_{row:08x}" else ""
    return f"0x{row:08X}{shown} ({state})"


def render(graph: Graph, result: dict, title: str, source: str) -> str:
    writes = result["writes"]
    ram = [w for w in writes if w["kind"] == "guest RAM"]
    mmio = [w for w in writes if w["kind"] != "guest RAM"]
    read = [w for w in writes if w["translated_readers"] or w["native_literal"]]
    hinted = [w for w in writes if w["native_offset_hints"] and w not in read]
    out = [f"# State written by {title}, and who still reads it", "",
           f"Graph: {source}. {len(result['members'])} rows scanned as writers; {result['surviving_rows']:,} live "
           f"translated bodies scanned as readers.", "",
           f"{len(writes)} fixed-address locations written: {len(ram)} in guest RAM, {len(mmio)} hardware registers. "
           f"{len(read)} have a proven reader (translated load or a runtime literal); {len(hinted)} more are mentioned "
           f"by small-data offset in runtime sources (hint). "
           f"Not placed: {sum(result['unproven_stores'].values())} stores of the set through unproven pointers, "
           f"{result['surviving_unproven_loads']:,} loads of surviving code through unproven pointers.", ""]
    out += ["| Address | Bytes | Object | sda+ | Written by | Translated readers | Native (literal) | Native (offset hint) |",
            "|---|---:|---|---|---|---|---|---|"]
    for w in writes:
        writers = ", ".join(f"0x{row:08X}" for row in w["writers"][:3]) + (" ..." if len(w["writers"]) > 3 else "")
        readers = ", ".join(sorted({f"0x{r['row']:08X}" for r in w["translated_readers"]})[:4]) or "-"
        literal = ", ".join(sorted({f.rsplit('/', 1)[-1] for n in w["native_literal"] for f in n["files"]})) or "-"
        hint = ", ".join(sorted({h.split(":", 1)[0].rsplit("/", 1)[-1] for h in w["native_offset_hints"]})[:4]) or "-"
        offset = f"0x{w['sda_offset']:X}" if w["sda_offset"] is not None and w["sda_offset"] >= 0 else ""
        kind = w["object"] or ("(hardware register)" if w["kind"] != "guest RAM" else "")
        out.append(f"| 0x{w['ea']:08X} | {w['width']} | {kind} | {offset} | {writers} | {readers} | {literal} | {hint} |")
    out += ["", "## Writers", ""]
    by_row = collections.defaultdict(int)
    for w in writes:
        for row in w["writers"]:
            by_row[row] += 1
    for row in result["members"]:
        unproven = result["unproven_stores"].get(row, 0)
        out.append(f"- {label(graph, row)}: {by_row.get(row, 0)} fixed-address locations, {unproven} stores through "
                   f"unproven pointers")
    detail = [w for w in writes if w["translated_readers"] or w["native_offset_hints"] or w["native_literal"]]
    if detail:
        out += ["", "## Readers in detail", ""]
        for w in detail:
            out.append(f"0x{w['ea']:08X} ({w['object'] or w['kind']}):")
            for reader in w["translated_readers"][:12]:
                out.append(f"  - translated 0x{reader['row']:08X} {reader['name']} loads {reader['width']} bytes at pc 0x{reader['pc']:08X}")
            for entry in w["native_literal"]:
                out.append(f"  - runtime literal 0x{entry['address']:08X} in {', '.join(entry['files'])}")
            for hint in w["native_offset_hints"][:8]:
                out.append(f"  - offset hint {hint}")
    out.append("")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--graph", required=True)
    parser.add_argument("--dolrecon-dir", required=True, type=Path)
    parser.add_argument("--addr", nargs="*", default=[])
    parser.add_argument("--list", dest="list_file")
    parser.add_argument("--label")
    parser.add_argument("--runtime", type=Path, default=SRC / "runtime")
    parser.add_argument("--json", dest="json_out", type=Path)
    arguments = parser.parse_args(argv)
    try:
        with open(arguments.graph, encoding="utf-8") as handle:
            graph = Graph(handle)
        wanted = [int(a, 16) for a in arguments.addr]
        if arguments.list_file:
            for token in Path(arguments.list_file).read_text(encoding="utf-8").replace(",", " ").split():
                if not token.startswith("#"):
                    wanted.append(int(token, 16))
        members = set()
        for address in wanted:
            row = graph.row_containing(address)
            if row is None:
                raise InputError(f"0x{address:08X} is in no row of the graph")
            members.add(row)
        if not members:
            raise InputError("no rows given")
        result = analyse(graph, members, arguments.dolrecon_dir, arguments.runtime)
    except (OSError, ValueError, KeyError, StopIteration, InputError) as error:
        print(f"state_readers: {error}", file=sys.stderr)
        return 2
    sys.stdout.write(render(graph, result, arguments.label or f"{len(members)} rows", arguments.graph) + "\n")
    if arguments.json_out:
        arguments.json_out.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
