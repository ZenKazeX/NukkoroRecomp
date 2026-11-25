#!/usr/bin/env python3
"""Verdict of an interleaved A/B timing (ab_tape.ps1): per metric, each arm's median and range, the paired
differences, and whether the difference is outside the run-to-run spread.

Reads <dir>/r<round>-A/session.json and r<round>-B/session.json. Metrics: the game thread's CPU seconds and cycles
(host), and - when the build has them - the audio engine's timers per audio frame (host_stats.audio_engine):
whole native frame, mixer, native outside the mixer, frame callback, reverb. --metric adds any other number by
dotted path, optionally divided by another ("host.wall_seconds", "host_stats.audio_engine.mix_ns/host_stats.audio_engine.frames").

The verdict per metric is deliberately strict; a difference is called real only when BOTH hold:
  - the ranges are disjoint (every B run is below every A run, or every one above), and
  - B is on the same side of A in every round (the paired sign is unanimous).
Otherwise it is "inside the run-to-run spread": report the work removed, not a speed-up. The table also gives the
difference of medians in percent and each arm's spread, so the reader can see how far inside or outside it is.

Deterministic counters that must not differ between the arms (state md5 of state.json, audio frames, frame
callbacks, mixed voices, owner hits, native entries) are compared and listed when they do.

  python ab_summary.py <ab dir> [--metric PATH[/PATH] ...] [--json out.json]

Exit 0; 2 for unreadable input. The exit code is not the verdict.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import statistics
import sys
from pathlib import Path

DEFAULT_METRICS = [
    ("game thread CPU (s)", "host.game_thread_cpu_s", None, 1.0),
    ("game thread cycles (G)", "host.game_thread_cycles", None, 1e-9),
]
AUDIO_METRICS = [
    ("audio native frame (us/frame)", "host_stats.audio_engine.frame_native_ns", "host_stats.audio_engine.frames", 1e-3),
    ("  mixer (us/frame)", "host_stats.audio_engine.mix_ns", "host_stats.audio_engine.frames", 1e-3),
    ("  native outside the mixer (us/frame)", "host_stats.audio_engine.frame_native_ns-host_stats.audio_engine.mix_ns",
     "host_stats.audio_engine.frames", 1e-3),
    ("frame callback (us/frame)", "host_stats.audio_engine.frame_callback_ns", "host_stats.audio_engine.frames", 1e-3),
    ("reverb (us/frame)", "host_stats.audio_engine.effect_ns", "host_stats.audio_engine.frames", 1e-3),
]
COUNTERS = ["guest.owner_hits", "guest.native_entries", "guest.game_ticks", "host_stats.audio_engine.frames",
            "host_stats.audio_engine.frame_callbacks", "host_stats.audio_engine.mixed_voices",
            "host_stats.audio_engine.frame_steps"]


def lookup(data: dict, path: str):
    value = data
    for key in path.split("."):
        value = value[key]
    return value


def evaluate(data: dict, expression: str) -> float:
    """A dotted path, or 'path-path' (difference)."""
    if "-" in expression:
        left, right = expression.split("-", 1)
        return float(lookup(data, left)) - float(lookup(data, right))
    return float(lookup(data, expression))


def load(directory: Path) -> dict:
    rounds: dict[int, dict] = {}
    for run in sorted(directory.iterdir()):
        match = re.fullmatch(r"r(\d+)-([AB])", run.name)
        if not match or not (run / "session.json").exists():
            continue
        session = json.loads((run / "session.json").read_text(encoding="utf-8"))
        state = run / "state.json"
        session["_state_md5"] = hashlib.md5(state.read_bytes()).hexdigest()[:12] if state.exists() else "(missing)"
        rounds.setdefault(int(match.group(1)), {})[match.group(2)] = session
    complete = {number: arms for number, arms in rounds.items() if set(arms) == {"A", "B"}}
    if not complete:
        raise ValueError(f"{directory}: no complete round (r<n>-A and r<n>-B with session.json)")
    return complete


def metric_row(rounds: dict, name: str, numerator: str, denominator: str | None, scale: float) -> dict | None:
    values = {"A": [], "B": []}
    try:
        for number in sorted(rounds):
            for arm in "AB":
                value = evaluate(rounds[number][arm], numerator)
                if denominator:
                    value /= evaluate(rounds[number][arm], denominator)
                values[arm].append(value * scale)
    except (KeyError, TypeError, ZeroDivisionError):
        return None
    a, b = values["A"], values["B"]
    median_a, median_b = statistics.median(a), statistics.median(b)
    signs = {(vb > va) - (vb < va) for va, vb in zip(a, b)}
    disjoint = max(b) < min(a) or min(b) > max(a)
    unanimous = len(signs) == 1 and 0 not in signs
    return {
        "metric": name, "a": a, "b": b, "median_a": median_a, "median_b": median_b,
        "delta_percent": 100.0 * (median_b / median_a - 1.0) if median_a else None,
        "spread_a_percent": 100.0 * (max(a) - min(a)) / median_a if median_a else None,
        "spread_b_percent": 100.0 * (max(b) - min(b)) / median_b if median_b else None,
        "b_lower_rounds": sum(1 for va, vb in zip(a, b) if vb < va), "rounds": len(a),
        "ranges_disjoint": disjoint, "outside_spread": disjoint and unanimous,
    }


def counter_differences(rounds: dict) -> list[str]:
    problems = []
    for path in COUNTERS + ["_state_md5"]:
        seen = {"A": set(), "B": set()}
        try:
            for arms in rounds.values():
                for arm in "AB":
                    seen[arm].add(lookup(arms[arm], path))
        except (KeyError, TypeError):
            continue
        if len(seen["A"]) > 1 or len(seen["B"]) > 1:
            problems.append(f"{path} varies WITHIN an arm: A {sorted(map(str, seen['A']))} B {sorted(map(str, seen['B']))}")
        elif seen["A"] != seen["B"]:
            problems.append(f"{path} differs between the arms: A {next(iter(seen['A']))} B {next(iter(seen['B']))}")
    return problems


def render(directory: Path, rows: list[dict], problems: list[str], meta: dict) -> str:
    out = [f"# A/B timing: {directory}", ""]
    if meta:
        out += [f"A = {meta.get('exe_a')}", f"B = {meta.get('exe_b')}",
                f"Tape {meta.get('tape')}, {meta.get('rounds')} rounds, interleaved (A first in odd rounds).", ""]
    out += ["| Metric | A median [min .. max] | B median [min .. max] | B vs A | B lower in | Verdict |", "|---|---|---|---:|---:|---|"]
    for row in rows:
        verdict = "OUTSIDE the run-to-run spread" if row["outside_spread"] else "inside the run-to-run spread"
        out.append(f"| {row['metric']} | {row['median_a']:.3f} [{min(row['a']):.3f} .. {max(row['a']):.3f}] | "
                   f"{row['median_b']:.3f} [{min(row['b']):.3f} .. {max(row['b']):.3f}] | {row['delta_percent']:+.2f} % | "
                   f"{row['b_lower_rounds']}/{row['rounds']} | {verdict} |")
    out.append("")
    if problems:
        out += ["Deterministic counters that are NOT equal:"] + [f"- {problem}" for problem in problems]
    else:
        out.append("Deterministic counters (state md5, ticks, owner hits, native entries, audio frames / callbacks / mixed "
                   "voices where present) are equal in every run of both arms.")
    out += ["", "Verdict rule: outside the spread only when the two ranges are disjoint AND B is on the same side of A in "
            "every round. Inside the spread means: no speed claim.", ""]
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("directory", type=Path)
    parser.add_argument("--metric", action="append", default=[], help="PATH or PATH/PATH (numerator/denominator)")
    parser.add_argument("--json", dest="json_out", type=Path)
    arguments = parser.parse_args(argv)
    try:
        rounds = load(arguments.directory)
    except (OSError, ValueError) as error:
        print(f"ab_summary: {error}", file=sys.stderr)
        return 2
    specs = list(DEFAULT_METRICS) + list(AUDIO_METRICS)
    for spec in arguments.metric:
        numerator, _, denominator = spec.partition("/")
        specs.append((spec, numerator, denominator or None, 1.0))
    rows = [row for row in (metric_row(rounds, *spec) for spec in specs) if row is not None]
    problems = counter_differences(rounds)
    meta_path = arguments.directory / "ab.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8-sig")) if meta_path.exists() else {}
    sys.stdout.write(render(arguments.directory, rows, problems, meta) + "\n")
    if arguments.json_out:
        arguments.json_out.write_text(json.dumps({"rows": rows, "counter_differences": problems, "meta": meta}, indent=1) + "\n",
                                      encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
