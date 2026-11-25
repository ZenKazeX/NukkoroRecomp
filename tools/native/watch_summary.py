#!/usr/bin/env python3
"""Summary of a guest watch report (--watch-guest / --watch-report): who touched which watched range, how often,
per frame.

The runtime's CSV has one row per (range label, read / write, accessor): the accessor is the innermost TRANSLATED
row on the faulting host stack, or - when no translated frame is on the stack - the native function (by symbol).

How to read "translated row" here: the watch attributes an access to the innermost translated frame, so native
code CALLED FROM a row (an owner or hook served inside it, an interrupt delivered while the row waits) is counted
under that row. The report cannot tell a row's own generated loads and stores from those of native code it called;
`--native-under` names rows known to host native work (e.g. the row the audio interrupt is delivered in) so the
tables say so instead of calling it translated.

Accesses made while the run writes its exit reports (the state report reads every watched block) are listed apart
and left out of the per-frame figures.

  python watch_summary.py watch.csv [--frames N] [--graph live_graph.txt] [--native-under HEX=text ...]
                          [--json out.json]

--frames: number of frames (audio frames, video frames - whatever the window is counted in) the watch covered;
without it only totals are given. Exit 0; 2 for unreadable input.
"""

from __future__ import annotations

import argparse
import collections
import csv
import json
import re
import sys
from pathlib import Path

EXIT_REPORT = re.compile(r"state_report_json|save_state|savestate|behaviour", re.IGNORECASE)
TOTALS = re.compile(r"^# faults (\d+), outside the ranges \(same page\) (\d+), unattributed (\d+)")


def clean_symbol(symbol: str) -> str:
    symbol = re.sub(r"\+0x[0-9A-Fa-f]+$", "", symbol.strip())
    return symbol.replace("`anonymous namespace'::", "").replace("``", "`") or "(no symbol)"


def read_report(path: Path) -> dict:
    rows, totals = [], {}
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    for line in lines:
        match = TOTALS.match(line)
        if match:
            totals = {"faults": int(match.group(1)), "outside_ranges": int(match.group(2)), "unattributed": int(match.group(3))}
    body = [line for line in lines if not line.startswith("#")]
    for record in csv.DictReader(body):
        row = int(record["row"], 16)
        symbol = clean_symbol(record.get("native_symbol") or "")
        rows.append({
            "label": record["label"], "kind": record["kind"], "row": row,
            "symbol": "" if row else symbol, "count": int(record["count"]),
            "first": int(record["first_guest_address"], 16),
            "exit_report": row == 0 and bool(EXIT_REPORT.search(symbol)),
        })
    if not rows and not totals:
        raise ValueError(f"{path}: not a guest watch report")
    return {"rows": rows, "totals": totals}


def accessor(row: dict, names: dict[int, str], native_under: dict[int, str]) -> str:
    if row["row"] == 0:
        return f"native: {row['symbol']}"
    name = names.get(row["row"], "")
    text = f"0x{row['row']:08X}" + (f" {name}" if name and name.lower() != f"fn_{row['row']:08x}" else "")
    if row["row"] in native_under:
        return f"{text} [hosts native work: {native_under[row['row']]}]"
    return text


def summarise(report: dict, frames: int, names: dict[int, str], native_under: dict[int, str]) -> dict:
    live = [row for row in report["rows"] if not row["exit_report"]]
    labels: dict[str, dict] = collections.OrderedDict()
    for row in live:
        entry = labels.setdefault(row["label"], {"read": 0, "write": 0, "accessors": []})
        entry[row["kind"]] += row["count"]
        entry["accessors"].append({
            "who": accessor(row, names, native_under), "kind": row["kind"], "count": row["count"],
            "per_frame": row["count"] / frames if frames else None, "first": row["first"],
            "class": "native" if row["row"] == 0 else ("native under a row" if row["row"] in native_under else "translated row"),
        })
    for entry in labels.values():
        entry["accessors"].sort(key=lambda item: -item["count"])
    untouched = sorted({row["label"] for row in report["rows"]} - set(labels))
    return {"frames": frames, "totals": report["totals"], "labels": labels, "untouched_except_exit_report": untouched,
            "exit_report_accesses": sum(row["count"] for row in report["rows"] if row["exit_report"])}


def render(summary: dict, source: str) -> str:
    frames = summary["frames"]
    out = [f"# Guest watch summary: {source}", ""]
    totals = summary["totals"]
    if totals:
        out.append(f"{totals['faults']:,} watched-page faults; {totals['outside_ranges']:,} on a watched page outside every "
                   f"range (ignored); {totals['unattributed']:,} unattributed.")
    out.append(f"{summary['exit_report_accesses']:,} accesses by the run's own exit reports are left out below."
               + (f" Per-frame figures over {frames:,} frames." if frames else " No frame count given: totals only."))
    out += ["", "| Range | Reads | Writes |" + (" Reads / frame | Writes / frame |" if frames else ""),
            "|---|---:|---:|" + ("---:|---:|" if frames else "")]
    for label, entry in summary["labels"].items():
        per = f" {entry['read'] / frames:,.1f} | {entry['write'] / frames:,.1f} |" if frames else ""
        out.append(f"| {label} | {entry['read']:,} | {entry['write']:,} |{per}")
    for label in summary["untouched_except_exit_report"]:
        out.append(f"| {label} | 0 | 0 |" + (" 0 | 0 |" if frames else ""))
    for label, entry in summary["labels"].items():
        out += ["", f"## {label}", "", "| Accessor | Kind | Count |" + (" Per frame |" if frames else "") + " First address |",
                "|---|---|---:|" + ("---:|" if frames else "") + "---|"]
        for item in entry["accessors"]:
            per = f" {item['per_frame']:,.2f} |" if frames else ""
            out.append(f"| {item['who']} | {item['kind']} | {item['count']:,} |{per} 0x{item['first']:08X} |")
    out += ["", "An accessor shown as a translated row is the innermost translated frame on the stack: native code called "
            "from that row (an owner served inside it) is counted under it.", ""]
    return "\n".join(out)


def load_names(graph: str | None) -> dict[int, str]:
    names = {}
    if graph:
        with open(graph, encoding="utf-8") as handle:
            for line in handle:
                parts = line.split()
                if parts and parts[0] == "row" and len(parts) > 5:
                    names[int(parts[1], 16)] = parts[5]
    return names


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("csv", type=Path)
    parser.add_argument("--frames", type=int, default=0)
    parser.add_argument("--graph")
    parser.add_argument("--native-under", nargs="*", default=[], help="HEX=text: a row that hosts native work")
    parser.add_argument("--json", dest="json_out", type=Path)
    arguments = parser.parse_args(argv)
    try:
        report = read_report(arguments.csv)
        native_under = {int(spec.split("=", 1)[0], 16): spec.split("=", 1)[1] for spec in arguments.native_under}
        summary = summarise(report, arguments.frames, load_names(arguments.graph), native_under)
    except (OSError, ValueError, KeyError, IndexError) as error:
        print(f"watch_summary: {error}", file=sys.stderr)
        return 2
    sys.stdout.write(render(summary, str(arguments.csv)) + "\n")
    if arguments.json_out:
        arguments.json_out.write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
