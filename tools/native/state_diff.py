#!/usr/bin/env python3
"""Where do two state reports differ? Header fields and memory blocks, by guest address.

A gate compares the md5 of `state.json` (`--state-report`, "shadow-state-report 1"). When two runs hash differently
this says what kind of difference it is before anyone reads a hex dump:

  header   every top-level field except `memory`, compared by value. Fields that describe the run rather than the
           guest (`render_frames`: frames handed to the renderer; a restored run and a straight run differ there
           by construction on executables that count per process) are marked "host-side".
  memory   per region, the 64 KiB blocks whose digest differs, as guest address ranges; regions present in only
           one report, or with a different committed size.

  python state_diff.py A/state.json B/state.json [--max-ranges 40] [--json out.json]

A directory may be given instead of a file (its state.json is read).
Exit 0 when the guest part is equal (header fields other than the host-side ones, and all memory), 1 when it
differs, 2 for unreadable input. The verdict line says which.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HOST_SIDE = {"render_frames"}   # process-lifetime counters: not guest state


class InputError(Exception):
    pass


def load(path: Path) -> dict:
    if path.is_dir():
        path = path / "state.json"
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise InputError(f"cannot read {path}: {error}") from error
    if not isinstance(report, dict) or "memory" not in report or "regions" not in report.get("memory", {}):
        raise InputError(f"{path}: not a state report (no memory.regions)")
    return report


def merge_ranges(indices: list[int]) -> list[tuple[int, int]]:
    """Consecutive block indices as (first, last) pairs."""
    ranges: list[tuple[int, int]] = []
    for index in sorted(indices):
        if ranges and index == ranges[-1][1] + 1:
            ranges[-1] = (ranges[-1][0], index)
        else:
            ranges.append((index, index))
    return ranges


def diff(a: dict, b: dict) -> dict:
    header = []
    for key in sorted((set(a) | set(b)) - {"memory"}):
        if a.get(key) != b.get(key):
            header.append({"field": key, "a": a.get(key), "b": b.get(key), "host_side": key in HOST_SIDE})
    block_a = a["memory"].get("block_bytes", 65536)
    block_b = b["memory"].get("block_bytes", 65536)
    regions = []
    by_name_a = {region["name"]: region for region in a["memory"]["regions"]}
    by_name_b = {region["name"]: region for region in b["memory"]["regions"]}
    total_blocks = 0
    for name in list(by_name_a) + [n for n in by_name_b if n not in by_name_a]:
        ra, rb = by_name_a.get(name), by_name_b.get(name)
        if ra is None or rb is None:
            regions.append({"region": name, "only_in": "a" if rb is None else "b"})
            continue
        entry = {"region": name, "base": ra["base"]}
        if ra["base"] != rb["base"] or ra.get("committed") != rb.get("committed") or block_a != block_b:
            entry["layout"] = {"a": [ra["base"], ra.get("committed")], "b": [rb["base"], rb.get("committed")]}
        blocks_a, blocks_b = ra.get("blocks", []), rb.get("blocks", [])
        total_blocks += max(len(blocks_a), len(blocks_b))
        differing = [i for i in range(max(len(blocks_a), len(blocks_b)))
                     if i >= len(blocks_a) or i >= len(blocks_b) or blocks_a[i] != blocks_b[i]]
        if differing:
            entry["blocks"] = len(differing)
            entry["ranges"] = [[ra["base"] + first * block_a, ra["base"] + (last + 1) * block_a]
                               for first, last in merge_ranges(differing)]
        if len(entry) > 2:
            regions.append(entry)
    guest_header = [item for item in header if not item["host_side"]]
    return {"header": header, "regions": regions, "blocks_compared": total_blocks,
            "guest_equal": not guest_header and not regions,
            "identical": not header and not regions}


def render(result: dict, name_a: str, name_b: str, max_ranges: int) -> str:
    out = [f"state diff: A = {name_a}", f"            B = {name_b}"]
    if result["header"]:
        out.append("header fields that differ:")
        for item in result["header"]:
            mark = "  (host-side: not guest state)" if item["host_side"] else ""
            out.append(f"  {item['field']}: A {json.dumps(item['a'])}  B {json.dumps(item['b'])}{mark}")
    else:
        out.append("header: equal")
    if result["regions"]:
        out.append("memory regions that differ:")
        for entry in result["regions"]:
            if "only_in" in entry:
                out.append(f"  {entry['region']}: only in {entry['only_in'].upper()}")
                continue
            if "layout" in entry:
                out.append(f"  {entry['region']}: layout (base, committed) A {entry['layout']['a']}  B {entry['layout']['b']}")
            if "ranges" in entry:
                out.append(f"  {entry['region']}: {entry['blocks']} blocks in {len(entry['ranges'])} ranges")
                shown = entry["ranges"] if max_ranges <= 0 else entry["ranges"][:max_ranges]
                for low, high in shown:
                    out.append(f"    0x{low:08X}..0x{high:08X}")
                if len(shown) < len(entry["ranges"]):
                    out.append(f"    ... {len(entry['ranges']) - len(shown)} more ranges")
    else:
        out.append(f"memory: equal ({result['blocks_compared']} blocks compared)")
    if result["identical"]:
        out.append("VERDICT: identical")
    elif result["guest_equal"]:
        out.append("VERDICT: guest state equal; only host-side header fields differ ("
                   + ", ".join(item["field"] for item in result["header"]) + ")")
    else:
        out.append("VERDICT: guest state differs")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    parser.add_argument("--max-ranges", type=int, default=40, help="address ranges listed per region; 0 = all")
    parser.add_argument("--json", dest="json_out", type=Path)
    arguments = parser.parse_args(argv)
    try:
        result = diff(load(arguments.a), load(arguments.b))
    except InputError as error:
        print(f"state_diff: {error}", file=sys.stderr)
        return 2
    print(render(result, str(arguments.a), str(arguments.b), arguments.max_ranges))
    if arguments.json_out:
        arguments.json_out.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    return 0 if result["guest_equal"] else 1


if __name__ == "__main__":
    sys.exit(main())
