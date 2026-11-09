"""Validate lifted render-return observations against their complete session.

CPU-side wall time includes waits. These logs do not measure GPU execution,
display scanout, temperature or process CPU usage. No slow frame is filtered.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def number(row, key):
    value = float(row[key])
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"invalid {key}: {row[key]}")
    return value


def integer(row, key):
    value = int(row[key])
    if value < 0 or str(value) != str(row[key]):
        raise ValueError(f"invalid integer {key}: {row[key]}")
    return value


def require(condition, message):
    if not condition:
        raise ValueError(message)


def distribution(values):
    if not values:
        return None
    ordered = sorted(values)
    return {"count": len(values), "sum_ms": math.fsum(values),
            "mean_ms": statistics.mean(values), "max_ms": ordered[-1],
            **{f"p{p}_ms": ordered[math.ceil(len(values) * p / 100) - 1] for p in (50, 95, 99)}}


def analyze(rows, session, interval_rows=None):
    report = session["presentation"]
    require(session["renderer"]["backend"] == "lifted", "not the lifted renderer")
    require(report["timing_enabled"] is True, "timing observer was disabled")
    require(report["gpu_frame_ms"] is None, "GPU time falsely reported by a CPU-side observer")
    require(report["render_rate_applied"] is False, "unexpected renderer pacing policy")
    presents = failures = 0
    last_wall = last_success = last_ticks = None
    rewinds = 0
    values = {key: [] for key in ("render_return_interval_ms", "packet_wait_ms", "capture_ms", "render_present_ms")}
    for row in rows:
        require(row.get("schema") == "1", "unknown frame log schema")
        require(integer(row, "present_index") == presents + 1, "missing, duplicated or reordered frame")
        ticks = integer(row, "game_ticks")
        integer(row, "packet_tick")
        rewinds += int(last_ticks is not None and ticks < last_ticks)
        last_ticks = ticks
        wall = number(row, "wall_seconds")
        require(last_wall is None or wall >= last_wall, "wall clock went backwards")
        last_wall = wall
        success = integer(row, "success")
        require(success in (0, 1), "invalid success flag")
        if last_success is None:
            require(row["render_return_interval_ms"] == "", "first return has no measured interval")
        else:
            interval = number(row, "render_return_interval_ms")
            require(abs(interval - (wall - last_success) * 1000) <= 0.002,
                    "frame interval disagrees with timestamps")
            if success:
                values["render_return_interval_ms"].append(interval)
        for key in values.keys() - {"render_return_interval_ms"}:
            values[key].append(number(row, key))
        if success:
            presents += 1
            last_success = wall
        else:
            failures += 1

    require(presents == report["rendered_frames"] and failures == report["render_failures"],
            "log does not cover every successful and failed render attempt")
    intervals = values["render_return_interval_ms"]
    require(len(intervals) == report["intervals"], "session interval count mismatch")
    require(report["available"] is bool(presents), "incorrect presentation availability")
    total = math.fsum(intervals)
    require(abs(total - report["interval_ms"]["sum"]) <= max(1e-5, len(rows) * 1e-6),
            "interval sum mismatch")
    if intervals:
        require(abs(max(intervals) - report["interval_ms"]["max"]) <= 1e-5, "maximum interval mismatch")
        if total > 0:
            require(abs(report["average_fps"] - len(intervals) * 1000 / total) <= 1e-4, "average FPS mismatch")
        else:
            require(report["average_fps"] is None, "zero elapsed time cannot produce an FPS estimate")
        ordered = sorted(intervals)
        for p in (50, 95, 99):
            value = ordered[math.ceil(len(ordered) * p / 100) - 1]
            bounds = report["interval_ms"][f"p{p}"]
            require(value >= bounds["lower_inclusive"] - 1e-6 and
                    (bounds["upper_exclusive"] is None or value < bounds["upper_exclusive"] + 1e-6),
                    "histogram bounds disagree with the complete frame log")
    else:
        require(report["average_fps"] is None, "no interval exists for an FPS estimate")
        require(all(report["interval_ms"][f"p{p}"] is None for p in (50, 95, 99)), "empty histogram has quantiles")
    cpu_wall = report["cpu_wall_ms"]
    for key in ("capture", "render_present"):
        require(abs(math.fsum(values[key + "_ms"]) - cpu_wall[key]) <= max(1e-5, len(rows) * 1e-6),
                f"{key} total mismatch")
    # The final empty dequeue may have no subsequent frame row.
    require(math.fsum(values["packet_wait_ms"]) <= cpu_wall["packet_wait"] + max(1e-5, len(rows) * 1e-6),
            "packet wait exceeds session total")
    queue = report["queue"]
    require(queue["pushed"] == sum(queue[k] for k in ("popped", "dropped", "shutdown_discards", "pending")),
            "packet queue accounting mismatch")
    require(queue["popped"] >= len(rows), "more render attempts than consumed packets")
    telemetry_count = 0
    previous = {"packets": 0, "dropped": 0, "skipped_draws": 0, "resource_failures": 0}
    telemetry_frames = 0
    for row in interval_rows or []:
        require(row["schema"] == "1" and row["renderer"] == "lifted", "wrong interval log schema/renderer")
        require(row["gpu_frame_ms"] == "", "unmeasured GPU time must be empty")
        seconds = number(row, "interval_s")
        require(seconds > 0, "empty wall interval")
        frames, attempts = integer(row, "frames"), integer(row, "attempts")
        require(attempts >= frames, "more successful returns than attempts")
        require(abs(number(row, "fps") - frames / seconds) <= 0.0002, "interval FPS disagrees with count/time")
        game_s = number(row, "game_ms") / 1000
        require(abs(number(row, "game_speed") - game_s / seconds) <= 0.0002, "game speed disagrees with elapsed time")
        if attempts:
            number(row, "render_present_ms")
        else:
            require(row["render_present_ms"] == "", "unmeasured render duration must be empty")
        packets = integer(row, "packets")
        if game_s:
            require(abs(number(row, "produced_per_game_s") - (packets - previous["packets"]) / game_s) <= 0.001,
                    "packet production rate mismatch")
        else:
            require(row["produced_per_game_s"] == "", "no game time for production rate")
        for key in previous:
            value = integer(row, key)
            require(value >= previous[key], "cumulative counter went backwards")
            previous[key] = value
        for key in ("packet_wait_ms", "capture_ms"):
            number(row, key)
        telemetry_frames += frames
        telemetry_count += 1
    require(telemetry_frames <= presents, "interval log invents presentations")
    return {"schema": 1, "scope": __doc__.strip(), "complete_frame_accounting": True,
            "render_attempts": len(rows), "successful_returns": presents, "failed_attempts": failures,
            "game_time_rewinds": rewinds, "interval_rows": telemetry_count,
            "interval_logged_returns": telemetry_frames,
            "durations": {key: distribution(v) for key, v in values.items()},
            "queue": queue, "gpu_time_available": False,
            "readiness": "passed" if not failures else "render failures retained; not a passing performance result"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", type=Path, required=True)
    parser.add_argument("--session", type=Path, required=True)
    parser.add_argument("--telemetry", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    def read_csv(path):
        with path.open(newline="") as stream:
            return list(csv.DictReader(stream))
    result = analyze(read_csv(args.frames), json.loads(args.session.read_text()),
                     read_csv(args.telemetry) if args.telemetry else None)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "durations"}))


if __name__ == "__main__":
    main()
