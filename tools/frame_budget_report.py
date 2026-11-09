"""Summarize all frames of a GEKKO_GPU_TRACE diagnostic; never certify player FPS.

CPU intervals exclude slot/swap-chain waits before begin_frame and all game-thread
work. GPU timestamps cover the direct list, not copy-queue work or scanout.
No startup, slow frame, or failed sample is silently removed.
"""
import argparse
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import re
import statistics

from graphics_baseline import fingerprint
from stage_benchmark import inspect_run


def distribution(values, budget_ms):
    ordered = sorted(values)
    if not ordered or any(not math.isfinite(x) or x < 0 for x in ordered):
        raise ValueError("empty, negative or nonfinite interval")
    return {"count": len(ordered), "mean_ms": statistics.mean(ordered),
            "min_ms": ordered[0], "max_ms": ordered[-1],
            **{f"p{p}_ms": ordered[math.ceil(len(ordered) * p / 100) - 1] for p in (50, 95, 99)},
            "above_target_budget": sum(x > budget_ms for x in ordered)}


def summarize(rows, expected_frames, target_fps):
    if not math.isfinite(target_fps) or target_fps <= 0:
        raise ValueError("invalid target FPS")
    budget_ms = 1000 / target_fps
    if not math.isfinite(budget_ms) or budget_ms <= 0:
        raise ValueError("invalid target frame budget")
    if len(rows) != expected_frames or len(rows) < 2:
        raise ValueError("trace does not cover every rendered frame, or has fewer than two frames")
    metrics = {name: [] for name in ("cpu_record_submit", "host_submit_present", "gpu_direct", "present_return_interval")}
    passes = defaultdict(list)
    previous = None
    for row in rows:
        ints = {key: int(row[key]) for key in ("frame", "tick", "game_ticks", "cpu_begin_ns", "cpu_submit_ns",
                "cpu_present_ns", "gpu_begin_ns", "gpu_end_ns", "mark_overflow", "upload_overflows", "passes")}
        if row["kind"] != "present" or any(x < 0 for x in ints.values()):
            raise ValueError("non-presentation or invalid trace row")
        if ints["mark_overflow"] or ints["upload_overflows"]:
            raise ValueError("incomplete timestamp or upload workload")
        if not 0 < ints["cpu_begin_ns"] <= ints["cpu_submit_ns"] <= ints["cpu_present_ns"] or not 0 < ints["gpu_begin_ns"] < ints["gpu_end_ns"]:
            raise ValueError("missing or backwards CPU/GPU timestamp")
        gpu_ms = float(row["gpu_ms"])
        if not math.isfinite(gpu_ms) or gpu_ms < 0 or abs(gpu_ms - (ints["gpu_end_ns"] - ints["gpu_begin_ns"]) / 1e6) > 0.000051:
            raise ValueError("GPU duration disagrees with timestamps or is invalid")
        if previous:
            if ints["frame"] != previous["frame"] + 1 or ints["tick"] <= previous["tick"] or ints["game_ticks"] < previous["game_ticks"] or ints["cpu_present_ns"] <= previous["cpu_present_ns"]:
                raise ValueError("missing, repeated or unordered frame")
            metrics["present_return_interval"].append((ints["cpu_present_ns"] - previous["cpu_present_ns"]) / 1e6)
        previous = ints
        metrics["cpu_record_submit"].append((ints["cpu_submit_ns"] - ints["cpu_begin_ns"]) / 1e6)
        metrics["host_submit_present"].append((ints["cpu_present_ns"] - ints["cpu_submit_ns"]) / 1e6)
        metrics["gpu_direct"].append(gpu_ms)
        detail = row["passes_detail"].split("|") if row["passes_detail"] else []
        if len(detail) != ints["passes"]:
            raise ValueError("missing pass timestamps")
        for item in detail:
            name, value = item.rsplit(":", 1)
            if not name:
                raise ValueError("empty pass name")
            passes[name].append(float(value))
    return {"schema": 1, "target_fps": target_fps, "target_frame_ms": budget_ms, "target_achievement": "not_evaluated_from_instrumented_trace",
            "intervals": {key: distribution(values, budget_ms) for key, values in metrics.items()},
            "passes_per_occurrence": {key: distribution(values, budget_ms) for key, values in sorted(passes.items())},
            "limitations": ["Diagnostic instrumentation is enabled; this is not a speed comparison or FPS certification.",
                "All frames, including loading and outliers, are included; nearest-rank percentiles.",
                "CPU record/submit is elapsed host time, not CPU utilization or the complete game-thread critical path.",
                "CPU slot and swap-chain waits before begin_frame are outside record/submit.",
                "Direct GPU duration excludes copy-queue uploads, presentation/scanout and CPU work.",
                "Pass statistics are per occurrence; names can occur multiple times in a frame.",
                "CSV GPU durations are rounded to 0.0001 ms; clocks are calibrated.",
                "A component within budget does not establish the target FPS."]}


def render_size_evidence(stdout, required=None):
    """Use the renderer's chosen-size announcement, never capture/game/window requests."""
    selected = sorted({tuple(map(int, match)) for match in re.findall(
        r"^\[lifted-render\] render size ([0-9]+)x([0-9]+) \(asked ", stdout, re.MULTILINE)})
    if any(w <= 0 or h <= 0 for w, h in selected):
        raise ValueError("invalid announced render dimensions")
    if required is not None:
        match = re.fullmatch(r"([1-9][0-9]*)x([1-9][0-9]*)", required)
        if match is None:
            raise ValueError("required render size must be WIDTHxHEIGHT")
        expected = tuple(map(int, match.groups()))
        if selected != [expected]:
            raise ValueError("live render size unconfirmed or changed; capture/game/window dimensions are not proof")
    return {"mode": "explicit_scene_size" if selected else "window_size_unverified",
            "announced_dimensions": [list(size) for size in selected],
            "required_dimensions": required,
            "scope": "Renderer chosen-size announcements, not physical display scanout or per-frame resource readback. "
                     "The renderer may downsample this scene into its presentation window."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("trace", "session", "stdout", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--target-fps", type=float, required=True)
    parser.add_argument("--require-render-size", help="reject unconfirmed or changing live render dimensions, e.g. 2560x1440")
    args = parser.parse_args()
    try:
        paths = {key: getattr(args, key).resolve() for key in ("trace", "session", "stdout")}
        pins = {key: fingerprint(path) for key, path in paths.items()}
        report = json.loads(paths["session"].read_text(encoding="utf-8-sig"))
        stdout = paths["stdout"].read_text(errors="replace")
        dimensions = render_size_evidence(stdout, args.require_render_size)
        errors, workload, _ = inspect_run(report, stdout, report["run"]["budget_ms"], False, True)
        if errors:
            raise ValueError("run failed validation: " + "; ".join(errors))
        with paths["trace"].open(encoding="utf-8-sig", newline="") as stream:
            header = stream.readline().strip()
            match = re.fullmatch(r"# shadow gpu trace 1, times in steady_clock ns, gpu timestamp frequency ([1-9][0-9]*) Hz", header)
            if match is None:
                raise ValueError("unrecognized trace or missing timestamp frequency")
            rows = list(csv.DictReader(stream))
        result = summarize(rows, workload["rendered_frames"], args.target_fps)
        result.update(inputs=pins, workload=workload, gpu_timestamp_frequency_hz=int(match[1]))
        result["render_size"] = dimensions
        if pins != {key: fingerprint(path) for key, path in paths.items()}:
            raise ValueError("input changed during analysis")
        with args.out.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2)
            stream.write("\n")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"frame budget report failed: {error}\n")
    print(args.out)


if __name__ == "__main__":
    main()
