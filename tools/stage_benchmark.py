"""Paired, isolated workload/resource checks for the current build.

Headless/no-audio builds frame packets but does not rasterize every frame. It
measures simulation throughput, not player FPS, GPU or audio performance. Verification runs add memory digests and captures and are never
mixed with timing. No input executable or personal persistence is modified.
--rendered instead runs paced windowed gameplay with audio and checks complete
render/audio workload counters. Its wall time is pacing-limited, not an FPS or
uncapped-throughput measurement. Process CPU and memory remain actual metrics.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time

from graphics_baseline import copy_new, fingerprint, write_json
from native_renderer_benchmark import blockers


def clean_environment(source):
    return {key: value for key, value in source.items()
            if not key.upper().startswith(("GEKKO_", "SHADOW_"))}


@contextmanager
def staged_executable(source, directory, expected):
    """Run both arms at one pathname; never overwrite an input or existing file.

    Executable identity can select different host/driver behavior. Copies are
    hashed before launch, outside the measured interval, and removed on exit.
    Callers must join the child before leaving this context.
    """
    source, directory = Path(source).resolve(), Path(directory).resolve()
    target = directory / "ShadowTheHedgehog.exe"
    if source == target:
        raise ValueError("staging must not replace the source executable")
    directory.mkdir(parents=True, exist_ok=True)
    created = False
    try:
        with source.open("rb") as reader, target.open("xb") as writer:
            created = True
            shutil.copyfileobj(reader, writer)
        actual = fingerprint(target)
        if any(actual[key] != expected[key] for key in ("sha256", "size")):
            raise ValueError(f"Input changed while staging executable: {source}")
        yield actual
    finally:
        if created:
            target.unlink(missing_ok=True)


def presentation_intervals(stdout, presentations):
    """Decode the existing 0.1 ms histogram without treating its overflow as a value.

    These are host render-return intervals, not GPU durations or display scanout.
    The producer labels inclusive histogram thresholds as 'longer than'.
    """
    prefix = "[lifted-render] present intervals:"
    lines = [line for line in stdout.splitlines() if line.startswith(prefix)]
    if not lines:
        return {"available": False}, []
    number = r"(\d+\.\d)"
    pattern = (r"\[lifted-render\] present intervals: (\d+), p50 " + number +
               r" p95 " + number + r" p99 " + number + r" max " + number +
               r" ms; longer than 20 ms (\d+), 34 ms (\d+), 50 ms (\d+)")
    match = re.fullmatch(pattern, lines[0])
    if len(lines) != 1 or match is None:
        return {"available": False}, ["malformed/duplicate presentation interval counters"]
    count = int(match[1])
    edges = [float(match[i]) for i in (2, 3, 4)]
    maximum = float(match[5])
    thresholds = [int(match[i]) for i in (6, 7, 8)]
    errors = []
    if count != max(presentations - 1, 0):
        errors.append("presentation interval count does not match presentations")
    if (not math.isfinite(maximum) or edges != sorted(edges) or not all(0 <= value <= 100.1 for value in edges) or
            thresholds != sorted(thresholds, reverse=True) or any(n > count for n in thresholds)):
        errors.append("inconsistent presentation interval counters")
    if count == 0:
        if any(edges) or maximum or any(thresholds):
            errors.append("nonzero presentation measurements without intervals")
    elif any(value < 0.1 for value in edges) or maximum + 0.051 < edges[-1] - 0.1:
        errors.append("presentation percentile contradicts maximum")
    bounds = {}
    for name, edge in zip(("p50", "p95", "p99"), edges):
        if not count:
            bounds[name] = None
            continue
        bounds[name] = {"lower_inclusive_ms": round(max(0.0, edge - 0.1), 1),
                        "upper_exclusive_ms": None if edge == 100.1 else edge,
                        "overflow": edge == 100.1}
    return {"available": True, "count": count, "quantile_bounds": bounds,
            "max_rounded_ms": maximum, "max_rounding_ms": 0.05,
            "at_least_ms": dict(zip(("20", "34", "50"), thresholds)),
            "scope": "Host intervals after successful render/present returns; histogram bins, not GPU frame times or display scanout. "
                     "The last bin is [100 ms, infinity); no exact percentile is inferred there. "
                     "Threshold counts include the threshold itself."}, errors


def inspect_run(report, stdout, duration_ms, verification, rendered=False, *, expected_captures=None):
    # Diagnostic sequences require every requested capture and its GPU work.
    if expected_captures is None:
        expected_captures = int(verification)
    if type(expected_captures) is not int or expected_captures < 0 or bool(expected_captures) != verification:
        raise ValueError("capture count must agree with verification mode")
    errors, workload, metrics = [], {}, {}
    def require(condition, message):
        if not condition:
            errors.append(message)
    require(report.get("stop") == {"reason": 0, "failed": False}, "abnormal termination")
    run = report.get("run", {})
    require(run.get("budget_ms") == duration_ms, "wrong duration")
    require(run.get("window") is rendered and run.get("reference_execution") is False,
            "not the requested production run mode")
    require(run.get("native_renderer") is True and run.get("native_renderer_failed") is False
            and run.get("native_presenter_failed") is False, "renderer failed or unidentified")
    require(run.get("verify_recomp") is False, "instrumented instruction verification")
    guest = report.get("guest", {})
    require(guest.get("interpreted_instructions") == 0, "interpreter execution or absent count")
    require(guest.get("initial_game_ticks") == 0, "not a cold boot")
    ticks = guest.get("game_ticks")
    require(type(ticks) is int and duration_ms * 40500 <= ticks < (duration_ms + 1000) * 40500,
            "game duration not reached or excessive overshoot")
    workload["game_ticks"] = ticks
    audio = report.get("audio", {})
    require(audio.get("active") is rendered, "wrong or unidentified audio mode")
    if rendered:
        require(all(audio.get(key) == 0 for key in (
            "underrun_events", "underrun_frames", "dropped_frames", "dropped_events",
            "open_failures", "render_errors", "device_invalidations")),
            "audio failure, underrun, dropped work or absent counters")
        for key in ("blocks", "produced_samples", "consumed_frames"):
            value = audio.get(key)
            require(type(value) is int and value > 0, f"missing/invalid audio {key}")
            workload["audio." + key] = value
    require(report.get("capture", {}).get("native_screenshot_requested") is verification,
            "wrong capture group")
    assets = report.get("assets", {})
    require(all(assets.get(key) == 0 for key in ("failed", "unmapped", "out_of_range", "map_failures")),
            "asset read/mapping failure or absent counters")
    workload.update({f"assets.{key}": assets.get(key) for key in ("reads", "bytes")})
    queue_match = re.search(r"\[lifted-render\] queue: pushed (\d+) popped (\d+) dropped (\d+) shutdown discards (\d+) pending (\d+)", stdout)
    require("[lifted-render] queue:" not in stdout or queue_match is not None, "malformed queue ownership counters")
    shutdown_discards = int(queue_match[4]) if queue_match else 0
    match = re.search(r"\[lifted-render\] frames (\d+) presented (\d+) headless (\d+) dropped (\d+) captures (\d+) render failures (\d+)", stdout)
    require(match is not None, "missing lifted-renderer counters")
    if match:
        frames, presents, headless, dropped, captures, failures = map(int, match.groups())
        if rendered:
            # present_newest() calls maybe_capture(packet), then presents that
            # same packet. A capture adds a GPU render, not a queue consumer.
            require(frames > 0 and presents > 0 and headless == failures == 0 and
                    frames == presents + dropped + shutdown_discards, "incomplete rendered packet accounting")
            workload["presented"] = presents
            workload["dropped_render_packets"] = dropped
            workload["shutdown_discarded_packets"] = shutdown_discards
        else:
            require(frames > 0 and frames == headless and presents == failures == 0,
                    "incomplete headless packet production")
            workload["unconsumed_queue_packets"] = dropped
        require(captures == expected_captures, "wrong screenshot count")
        workload["frames"] = frames
        if queue_match:
            pushed, popped, backlog, cancelled, pending = map(int, queue_match.groups())
            require(pushed == frames and backlog == dropped and pending == 0 and
                    pushed == popped + backlog + cancelled + pending and
                    popped == (presents if rendered else 0), "incomplete queue ownership accounting")

    patterns = [
        (r"\[gameplay-scheduler\] traversals (\d+).*? visits (\d+) executes (\d+).*? faults (\d+)",
         ("traversals", "visits", "executes", "scheduler_faults")),
        (r"\[lifted-render\] sectors (\d+) drawn meshes (\d+) built (\d+) skipped (\d+) import failures (\d+)",
         ("sectors", "sector_meshes", "sector_builds", "sector_skips", "import_failures")),
        (r"\[lifted-render\] models: draws (\d+) built (\d+) rebuilt (\d+) failed (\d+)",
         ("model_draws", "model_builds", "model_rebuilds", "model_failures")),
        (r"\[lifted-render\] particles: batches (\d+) particles (\d+) unsupported (\d+) skipped (\d+)",
         ("particle_batches", "particles", "unsupported_particles", "skipped_particles")),
        (r"\[lifted-render\] renderer: frames (\d+) last draws (\d+) \(skipped (\d+), all frames (\d+)\)",
         ("rendered_frames", "last_draws", "last_skipped", "all_skipped")),
    ]
    for pattern, names in patterns:
        match = re.search(pattern, stdout)
        require(match is not None, f"missing counters: {names[0]}")
        if match:
            workload.update(zip(names, map(int, match.groups())))
    for key in ("scheduler_faults", "sector_skips", "import_failures", "model_failures",
                "unsupported_particles", "skipped_particles", "last_skipped", "all_skipped"):
        require(workload.get(key) == 0, f"failed or absent {key}")
    if rendered:
        intervals, interval_errors = presentation_intervals(stdout, workload.get("presented", 0))
        metrics["presentation_intervals"] = intervals
        errors.extend(interval_errors)
    gpu_frames = workload.get("rendered_frames")
    require(type(gpu_frames) is int and (gpu_frames > 0 if verification or rendered else gpu_frames == 0),
            "unexpected GPU work for the requested group")
    if rendered:
        require(gpu_frames == workload.get("presented", -1) + expected_captures,
                "GPU frame count does not cover presentations and capture")
    for group, fields in (("host", ("wall_seconds", "process_cpu_seconds")),
                          ("memory", ("peak_working_set_mib", "peak_private_mib"))):
        source = report.get("host", {}) if group == "host" else report.get("host_stats", {}).get("memory", {})
        for field in fields:
            value = source.get(field)
            require(type(value) in (int, float) and math.isfinite(value) and value > 0, f"missing/invalid {field}")
            metrics[field] = value
    return errors, workload, metrics


def summarize(rows, repetitions, verification, rendered=False):
    errors = [f"run {row['sequence']}: {error}" for row in rows for error in row["errors"]]
    expected = {(arm, i) for arm in ("control", "candidate") for i in range(repetitions)}
    actual = [(row["arm"], row["repetition"]) for row in rows]
    if set(actual) != expected or len(actual) != len(expected):
        errors.append("missing/duplicate paired runs")
    if len({json.dumps(row["workload"], sort_keys=True) for row in rows}) != 1:
        errors.append("workload differs across runs")
    if verification:
        for field in ("state", "image"):
            if any(not row.get(field) for row in rows) or len({json.dumps(row.get(field), sort_keys=True) for row in rows}) != 1:
                errors.append(f"{field} missing or differs across runs")
    metrics = {}
    for key in ("wall_seconds", "process_cpu_seconds", "peak_working_set_mib", "peak_private_mib"):
        entry = {}
        for arm in ("control", "candidate"):
            values = [row["metrics"].get(key) for row in rows if row["arm"] == arm]
            if values and all(type(v) in (int, float) for v in values):
                median = statistics.median(values)
                entry[arm] = {"values": values, "median": median,
                              "mad": statistics.median(abs(v - median) for v in values)}
        if "control" in entry and "candidate" in entry:
            entry["median_percent"] = 100 * (entry["candidate"]["median"] / entry["control"]["median"] - 1)
            pairs = []
            for rep in range(repetitions):
                pair = {row["arm"]: row["metrics"].get(key) for row in rows if row["repetition"] == rep}
                if set(pair) == {"control", "candidate"} and all(type(v) in (int, float) for v in pair.values()):
                    pairs.append(pair["candidate"] - pair["control"])
            entry["paired_deltas"] = pairs
        metrics[key] = entry
    hashes = {arm: sorted({row["binary_sha256"] for row in rows if row["arm"] == arm})
              for arm in ("control", "candidate")}
    if any(len(values) != 1 or not re.fullmatch(r"[a-fA-F0-9]{64}", values[0]) for values in hashes.values()):
        errors.append("binary identity changed or missing")
    return {"accepted": not errors, "errors": errors, "metrics": metrics, "binary_hashes": hashes,
            "identical_binary_calibration": hashes["control"] == hashes["candidate"],
            "performance_gain_proven": False,
            "scope": ("Paced windowed gameplay with rendering and audio. Wall time is pacing-limited; not player FPS or uncapped throughput. " if rendered else
                      "Simulation and frame-packet construction only; no full GPU rendering or audio output. Not player FPS. ") +
                     "All pairs included; no calibration subtraction.",
            "rendered": rendered,
            "verification": verification, "runs": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("control", "candidate", "game-data", "card", "sram", "display-cfg", "pad-cfg", "input-tape", "out"):
        parser.add_argument("--" + key, type=Path, required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--duration-ms", type=int, default=22000)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--verification", action="store_true")
    parser.add_argument("--rendered", action="store_true", help="paced windowed rendering and audio; compare resource cost, not FPS")
    parser.add_argument("--raw-state", action="store_true", help="save complete snapshots for byte-level diagnosis; requires --verification")
    parser.add_argument("--capture-frame", type=int, default=600)
    args = parser.parse_args()
    if args.raw_state and not args.verification:
        parser.error("--raw-state requires --verification; snapshots must not contaminate timing")
    if not 1 <= args.runs <= 10 or not 1000 <= args.duration_ms <= 120000:
        parser.error("runs 1..10 and duration 1000..120000 ms required")
    if not re.fullmatch(r"stg[0-9]{4}", args.stage) or args.capture_frame < 1:
        parser.error("explicit stage and positive capture frame required")
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.resolve())
    if args.card.suffix.lower() != ".raw":
        parser.error("pin a raw card, copied into each run")
    pins = {key: fingerprint(getattr(args, key)) for key in
            ("control", "candidate", "card", "sram", "display_cfg", "pad_cfg", "input_tape")}
    # Hash the shared asset tree once, and recheck after the session. This runs
    # outside timings and keeps both arms' filesystem-cache starting point warm.
    def assets():
        digest = hashlib.sha256()
        for folder in ("files", "sys"):
            for path in sorted((args.game_data / folder).rglob("*")):
                if path.is_file():
                    digest.update(path.relative_to(args.game_data).as_posix().encode() + b"\0")
                    digest.update(bytes.fromhex(fingerprint(path)["sha256"]))
        return digest.hexdigest()
    if any(not entry["sha256"] for entry in pins.values()):
        parser.error("a pinned file is missing")
    if blockers():
        parser.error("another game/build/test is active")
    args.out.mkdir(parents=True, exist_ok=False)
    asset_hash = assets()
    environment = clean_environment(os.environ)
    if args.verification:
        environment["SHADOW_R2_DEBUG_LAYER"] = "1"
    write_json(args.out / "provenance.json", {"pins": pins, "asset_tree_sha256": asset_hash,
               "settings": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
               "execution_identity": {"path": str(args.out / "executable" / "ShadowTheHedgehog.exe"),
                                      "policy": "Both binaries staged at the same absolute pathname; original inputs retained."},
               "observers": {k: v for k, v in environment.items() if k.startswith(("SHADOW_", "GEKKO_"))}})
    rows = []
    for rep in range(args.runs):
        for arm in (("control", "candidate") if rep % 2 == 0 else ("candidate", "control")):
            for key, pin in pins.items():
                if fingerprint(getattr(args, key))["sha256"] != pin["sha256"]:
                    raise ValueError(f"Pinned input changed: {key}")
            if blockers():
                raise ValueError("another game/build/test is active")
            directory = args.out / f"run-{len(rows):03d}"
            directory.mkdir()
            for key, name in (("card", "card.raw"), ("sram", "SRAM.pc.raw"),
                              ("display_cfg", "display.cfg"), ("pad_cfg", "pad.cfg"), ("input_tape", "input.tape")):
                copy_new(getattr(args, key), directory / name, pins[key])
            with staged_executable(getattr(args, arm), args.out / "executable", pins[arm]) as execution:
                mode = ["--windowed", "--audio"] if args.rendered else ["--headless", "--no-audio"]
                argv = [execution["path"], "--game-data", str(args.game_data)] + mode + [
                        "--memory-card", "card.raw", "--display-cfg", "display.cfg", "--pad-cfg", "pad.cfg",
                        "--state-file", "unused.gekkostate", "--snapshot-key", "none", "--no-xinput",
                        "--boot-stage", args.stage, "--budget-ms", str(args.duration_ms), "--input-tape-play", "input.tape",
                        "--native-output", "1280x720", "--width", "1280", "--height", "720", "--json-report", "session.json"]
                if args.verification:
                    argv += ["--state-report", "state.json", "--native-screenshot", "frame.png",
                             "--native-screenshot-frame", str(args.capture_frame)]
                if args.raw_state:
                    argv += ["--save-state", "--save-state-file", "snapshot.gekkostate"]
                write_json(directory / "invocation.json", {"argv": argv, "arm": arm, "repetition": rep, "execution": execution,
                                                               "source": pins[arm]})
                with (directory / "stdout.log").open("xb") as out, (directory / "stderr.log").open("xb") as err:
                    process = subprocess.Popen(argv, cwd=directory, env=environment, stdout=out, stderr=err, stdin=subprocess.DEVNULL)
                    deadline = time.monotonic() + 180
                    abort = None
                    while process.poll() is None:
                        collision = [item for item in blockers() if item[0] != process.pid]
                        if collision or time.monotonic() > deadline:
                            abort = f"overlapping workload {collision}" if collision else "timeout"
                            process.kill()  # Only the process this runner created.
                            process.wait()
                            break
                        try:
                            process.wait(timeout=0.2)
                        except subprocess.TimeoutExpired:
                            pass
            try:
                report = json.loads((directory / "session.json").read_text())
            except (OSError, ValueError):
                report = {}
            errors, workload, metrics = inspect_run(report, (directory / "stdout.log").read_text(errors="replace"), args.duration_ms, args.verification, args.rendered)
            if process.returncode != 0 or abort:
                errors.append(f"process exit {process.returncode}: {abort}")
            row = {"sequence": len(rows), "arm": arm, "repetition": rep, "binary_sha256": pins[arm]["sha256"],
                   "errors": errors, "workload": workload, "metrics": metrics}
            if args.verification:
                try:
                    row["state"] = json.loads((directory / "state.json").read_text())
                    row["image"] = fingerprint(directory / "frame.png")["sha256"]
                    if args.raw_state:
                        row["snapshot"] = fingerprint(directory / "snapshot.gekkostate")
                        if not row["snapshot"]["sha256"]:
                            errors.append("missing raw snapshot")
                except (OSError, ValueError):
                    errors.append("missing state/capture")
            write_json(directory / "result.json", row)
            rows.append(row)
            print(f"{arm}/{rep}: {metrics}; errors={errors}", flush=True)
            if errors:
                break
        if rows[-1]["errors"]:
            break
    result = summarize(rows, args.runs, args.verification, args.rendered)
    if assets() != asset_hash or any(fingerprint(getattr(args, k))["sha256"] != p["sha256"] for k, p in pins.items()):
        result["errors"].append("input changed during session")
        result["accepted"] = False
    write_json(args.out / "summary.json", result)
    return 0 if result["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
