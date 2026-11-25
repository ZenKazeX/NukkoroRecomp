#!/usr/bin/env python3
"""Guest/native boundary report for one run (or the difference between two runs).

Inputs are what every headless run already writes: its stdout log and, beside it, session.json. Nothing is rebuilt
and no runtime option is needed.

What the existing counters give
  guest -> native   per hooked guest address: the "HLE hooks installed: N; hits this run:" block lists address, hits and
                    the hook's name for every native owner / hook that served at least one call. session.json has the
                    totals (guest.owner_hits, guest.owner_entry_attempts, guest.owner_hooks).
  dispatcher        entries into translated code from the dispatcher (guest.native_entries), dispatcher round trips,
                    published transfers, "no native code" dispatches.
  native -> guest   only aggregates: interrupt guest calls, SVM callbacks, SJ guest callbacks, and the hits of the
                    "guest call return" hooks. There is NO per-target count of gekko::guest_call / recomp::call_indirect
                    made from native owners. The report says so instead of guessing (see "not measured").

Rates are per presented frame (guest.limiter_frames) when the run recorded frames, else per VI interrupt.

  python boundary_report.py RUN_DIR [--top 40] [--json out.json]
  python boundary_report.py RUN_DIR --against BASE_DIR        # what changed between two runs of the same tape
  python boundary_report.py --stdout a.log --session a.json

RUN_DIR is a directory holding stdout.log and session.json. Exit code 0; 2 for unreadable or incomplete input
(a log without the hook block is an error, not an empty report).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

HOOK_HEADER = re.compile(r"^HLE hooks installed: (\d+); hits this run:\s*$")
HOOK_ROW = re.compile(r"^\s+0x([0-9A-Fa-f]{8})\s+x(\d+)\s{2}(.*)$")

# Aggregate lines of the end-of-run report. Each entry: key, regular expression with one group, description.
AGGREGATES = (
    ("dispatcher_entries", r"^Native execution: (\d+) entries", "dispatcher entries into translated code"),
    ("dispatcher_round_trips", r"^Native execution: \d+ entries, (\d+) dispatcher round trips",
     "dispatcher round trips (sc/rfi/yield/trap)"),
    ("published_transfers", r"^Native transfers: (\d+) published", "transfers published to the waiting frame"),
    ("transfers_left_to_dispatcher", r"^Native transfers: .*?, (\d+) left to the dispatcher",
     "transfers left to the dispatcher"),
    ("normal_returns", r"^Counter identity: .*normal returns (\d+)", "translated call trees that returned normally"),
    ("non_native_ppc_executions", r"^TOTAL_NON_NATIVE_PPC_EXECUTIONS: (\d+)", "interpreted PowerPC instructions"),
    ("dispatch_no_native_code", r"^Dispatch blocked: .*no native code (\d+)", "dispatches with no native code"),
    ("dispatch_blocked_interrupt", r"^Dispatch blocked: interrupt due (\d+)", "dispatches blocked by a due interrupt"),
    ("interrupt_guest_calls", r"^Native interrupts: .*?; (\d+) guest calls",
     "native -> guest: interrupt handlers called in the guest"),
    ("vi_interrupts", r"^Native interrupts: delivered VI (\d+)", "VI interrupts delivered"),
    ("svm_guest_callbacks", r"^native SVM .*?, callbacks (\d+)", "native -> guest: SVM server callbacks"),
    ("sj_guest_callbacks", r"^native SJ .*guest callbacks (\d+)", "native -> guest: SJ callbacks"),
)

NATIVE_TO_GUEST_KEYS = ("interrupt_guest_calls", "svm_guest_callbacks", "sj_guest_callbacks")


class InputError(Exception):
    pass


def group_of(name: str) -> str:
    """The subsystem label of a hook name: the text before the first ':' ("native boot: __start" -> "native boot"),
    else the whole name ("S1 shptr (native)")."""
    head = name.split(":", 1)[0].strip()
    return head if head else "(unnamed)"


def parse_stdout(text: str) -> dict:
    lines = text.splitlines()
    hooks = []
    installed = None
    for index, line in enumerate(lines):
        match = HOOK_HEADER.match(line)
        if not match:
            continue
        installed = int(match.group(1))
        for row in lines[index + 1:]:
            hit = HOOK_ROW.match(row)
            if not hit:
                break
            hooks.append({"address": int(hit.group(1), 16), "hits": int(hit.group(2)), "name": hit.group(3).strip()})
        break
    if installed is None:
        raise InputError("no 'HLE hooks installed: N; hits this run:' block: not the stdout log of a finished run")
    aggregates = {}
    for key, pattern, _ in AGGREGATES:
        expression = re.compile(pattern)
        for line in lines:
            match = expression.match(line)
            if match:
                aggregates[key] = int(match.group(1))
                break
    return {"hooks_installed": installed, "hooks": hooks, "aggregates": aggregates}


def load_run(stdout_path: Path, session_path: Path | None) -> dict:
    try:
        run = parse_stdout(stdout_path.read_text(encoding="utf-8", errors="replace"))
    except OSError as error:
        raise InputError(f"cannot read {stdout_path}: {error}") from error
    run["stdout"] = str(stdout_path)
    guest = {}
    if session_path is not None:
        try:
            guest = json.loads(session_path.read_text(encoding="utf-8")).get("guest", {})
        except (OSError, ValueError) as error:
            raise InputError(f"cannot read {session_path}: {error}") from error
        run["session"] = str(session_path)
    run["guest"] = {key: guest[key] for key in (
        "limiter_frames", "game_seconds", "native_entries", "owner_hits", "owner_hooks", "owner_entry_attempts",
        "dispatch_no_native_code", "interpreted_instructions") if key in guest}
    frames = guest.get("limiter_frames") or 0
    if frames:
        run["rate_unit"], run["rate_base"] = "presented frame", frames
    elif run["aggregates"].get("vi_interrupts"):
        run["rate_unit"], run["rate_base"] = "VI interrupt", run["aggregates"]["vi_interrupts"]
    else:
        run["rate_unit"], run["rate_base"] = None, 0
    run["hook_hits_total"] = sum(hook["hits"] for hook in run["hooks"])
    return run


def consistency(run: dict) -> list[str]:
    """Cross-checks between the log and session.json. A mismatch means the two files are not from the same run or the
    report format changed; either way the numbers below must not be trusted silently."""
    problems = []
    guest = run["guest"]
    if "owner_hits" in guest and guest["owner_hits"] != run["hook_hits_total"]:
        problems.append(f"session owner_hits {guest['owner_hits']} != sum of listed hook hits {run['hook_hits_total']}")
    if "owner_hooks" in guest and guest["owner_hooks"] != run["hooks_installed"]:
        problems.append(f"session owner_hooks {guest['owner_hooks']} != hooks installed in the log {run['hooks_installed']}")
    entries = run["aggregates"].get("dispatcher_entries")
    if "native_entries" in guest and entries is not None and guest["native_entries"] != entries:
        problems.append(f"session native_entries {guest['native_entries']} != log dispatcher entries {entries}")
    return problems


def by_group(hooks: list[dict]) -> list[dict]:
    groups: dict[str, dict] = {}
    for hook in hooks:
        entry = groups.setdefault(group_of(hook["name"]), {"group": group_of(hook["name"]), "hooks": 0, "hits": 0})
        entry["hooks"] += 1
        entry["hits"] += hook["hits"]
    return sorted(groups.values(), key=lambda entry: (-entry["hits"], entry["group"]))


def rate(run: dict, count: int) -> str:
    return f"{count / run['rate_base']:.1f}" if run["rate_base"] else "-"


def share(part: int, whole: int) -> str:
    return f"{100.0 * part / whole:.2f} %" if whole else "-"


def render(run: dict, top: int) -> str:
    out = []
    total = run["hook_hits_total"]
    unit = run["rate_unit"] or "frame (not recorded)"
    out.append(f"# Guest/native boundary: {run['stdout']}")
    out.append("")
    if run["rate_base"]:
        out.append(f"Rates are per {unit} ({run['rate_base']:,} in this run).")
    else:
        out.append("No frame or VI count in the inputs: rates are not given.")
    for problem in consistency(run):
        out.append(f"INCONSISTENT INPUT: {problem}")
    out.append("")
    out.append("## Totals")
    out.append("")
    out.append("| Crossing | Count | Per " + unit + " |")
    out.append("|---|---:|---:|")
    served = len(run["hooks"])
    out.append(f"| guest -> native: hook / owner calls served ({served} of {run['hooks_installed']} hooks hit) | "
               f"{total:,} | {rate(run, total)} |")
    attempts = run["guest"].get("owner_entry_attempts")
    if attempts is not None:
        out.append(f"| guest -> native: slow-path entry attempts (served + handed back + filter collisions) | "
                   f"{attempts:,} | {rate(run, attempts)} |")
    descriptions = {key: text for key, _, text in AGGREGATES}
    for key, _, _ in AGGREGATES:
        if key in run["aggregates"] and key != "vi_interrupts":
            value = run["aggregates"][key]
            out.append(f"| {descriptions[key]} | {value:,} | {rate(run, value)} |")
    out.append("")
    out.append("Not measured by the existing counters: native -> guest calls made from native owners through "
               "`gekko::guest_call` / `recomp::call_indirect`, per target address. Only the aggregates above "
               "(interrupt guest calls, SVM and SJ callbacks) exist.")
    out.append("")
    out.append("## Guest -> native by subsystem (hook name before ':')")
    out.append("")
    out.append("| Subsystem | Hooks hit | Calls served | Share | Per " + unit + " |")
    out.append("|---|---:|---:|---:|---:|")
    for entry in by_group(run["hooks"]):
        out.append(f"| {entry['group']} | {entry['hooks']} | {entry['hits']:,} | {share(entry['hits'], total)} | "
                   f"{rate(run, entry['hits'])} |")
    out.append("")
    ranked = sorted(run["hooks"], key=lambda hook: (-hook["hits"], hook["address"]))
    shown = ranked if top <= 0 else ranked[:top]
    out.append(f"## Guest -> native by address (top {len(shown)} of {len(ranked)})")
    out.append("")
    out.append("| Address | Calls served | Share | Cumulative | Per " + unit + " | Hook |")
    out.append("|---|---:|---:|---:|---:|---|")
    running = 0
    for hook in shown:
        running += hook["hits"]
        out.append(f"| 0x{hook['address']:08X} | {hook['hits']:,} | {share(hook['hits'], total)} | "
                   f"{share(running, total)} | {rate(run, hook['hits'])} | {hook['name']} |")
    out.append("")
    return "\n".join(out)


def compare(run: dict, base: dict, top: int) -> str:
    out = [f"# Boundary difference: {run['stdout']} against {base['stdout']}", ""]
    for label, item in (("run", run), ("base", base)):
        for problem in consistency(item):
            out.append(f"INCONSISTENT INPUT ({label}): {problem}")
    out.append("| Counter | Base | Run | Change |")
    out.append("|---|---:|---:|---:|")
    out.append(f"| guest -> native calls served | {base['hook_hits_total']:,} | {run['hook_hits_total']:,} | "
               f"{run['hook_hits_total'] - base['hook_hits_total']:+,} |")
    out.append(f"| hooks installed | {base['hooks_installed']:,} | {run['hooks_installed']:,} | "
               f"{run['hooks_installed'] - base['hooks_installed']:+,} |")
    keys = [key for key, _, _ in AGGREGATES if key in run["aggregates"] or key in base["aggregates"]]
    descriptions = {key: text for key, _, text in AGGREGATES}
    for key in keys:
        old, new = base["aggregates"].get(key), run["aggregates"].get(key)
        if old is None or new is None:
            out.append(f"| {descriptions[key]} | {'-' if old is None else f'{old:,}'} | "
                       f"{'-' if new is None else f'{new:,}'} | n/a |")
        else:
            out.append(f"| {descriptions[key]} | {old:,} | {new:,} | {new - old:+,} |")
    out.append("")
    before = {hook["address"]: hook for hook in base["hooks"]}
    after = {hook["address"]: hook for hook in run["hooks"]}
    changes = []
    for address in sorted(set(before) | set(after)):
        old = before.get(address, {}).get("hits", 0)
        new = after.get(address, {}).get("hits", 0)
        if old != new:
            name = (after.get(address) or before.get(address))["name"]
            changes.append((address, old, new, name))
    changes.sort(key=lambda change: (-abs(change[2] - change[1]), change[0]))
    out.append(f"## Addresses whose served calls changed: {len(changes)}")
    out.append("")
    if changes:
        out.append("| Address | Base | Run | Change | Hook |")
        out.append("|---|---:|---:|---:|---|")
        for address, old, new, name in (changes if top <= 0 else changes[:top]):
            out.append(f"| 0x{address:08X} | {old:,} | {new:,} | {new - old:+,} | {name} |")
        if 0 < top < len(changes):
            out.append(f"| ... | | | | {len(changes) - top} more |")
    out.append("")
    return "\n".join(out)


def resolve(run_dir: str | None, stdout: str | None, session: str | None) -> tuple[Path, Path | None]:
    if run_dir:
        directory = Path(run_dir)
        session_path = directory / "session.json"
        return directory / "stdout.log", session_path if session_path.exists() else None
    if not stdout:
        raise InputError("give RUN_DIR or --stdout")
    return Path(stdout), Path(session) if session else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("run_dir", nargs="?", help="directory with stdout.log and session.json")
    parser.add_argument("--stdout", help="stdout log of the run (instead of RUN_DIR)")
    parser.add_argument("--session", help="session.json of the run (instead of RUN_DIR)")
    parser.add_argument("--against", help="base RUN_DIR: report the difference instead")
    parser.add_argument("--top", type=int, default=40, help="rows in the per-address table; 0 = all (default 40)")
    parser.add_argument("--json", dest="json_out", help="also write the parsed run as JSON to this file")
    arguments = parser.parse_args(argv)
    try:
        run = load_run(*resolve(arguments.run_dir, arguments.stdout, arguments.session))
        if arguments.against:
            text = compare(run, load_run(*resolve(arguments.against, None, None)), arguments.top)
        else:
            text = render(run, arguments.top)
    except InputError as error:
        print(f"boundary_report: {error}", file=sys.stderr)
        return 2
    sys.stdout.write(text + "\n")
    if arguments.json_out:
        payload = dict(run)
        payload["groups"] = by_group(run["hooks"])
        payload["inconsistencies"] = consistency(run)
        Path(arguments.json_out).write_text(json.dumps(payload, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
