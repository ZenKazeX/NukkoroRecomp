"""Sample the NVIDIA GPU's clocks, P-state, power and throttle reasons while a command runs (OPTIMIZATION_PLAN A1).

MEASUREMENT ONLY: the sampler is a separate process reading NVML (nvml.dll, shipped with the NVIDIA driver; no admin
rights, nothing is set on the GPU). Each row is stamped with time.perf_counter_ns(), which on Windows is
QueryPerformanceCounter in nanoseconds - the clock the GEKKO_GPU_TRACE and GEKKO_PRESENT_TRACE rows use
(steady_clock) - so samples can be joined with a GPU trace's frames.

  gpu_clock_sampler.py run --out <dir> [--interval-ms 10] -- <command ...>
  gpu_clock_sampler.py join --samples <dir>/gpu_clocks.csv --gpu-trace <trace.csv> [--budget-ms 16.67] --out <json>

What NVML reports and its limits: clocks are the current graphics/SM/memory clocks (MHz) as the driver last updated
them (its own update period, typically tens of ms); power is the board power in mW as NVML's own averaging window
reports it (about 1 s on recent boards), not an instantaneous value; utilization is the percentage of the last
sample period the GPU was busy; throttle reasons are NVML's bitmask (THROTTLE below: 0x1 = GPU idle, 0x2 =
applications clocks, 0x4 = SW power cap, 0x8 = HW slowdown, 0x20 = SW thermal, 0x40 = HW thermal, 0x80 = HW power
brake, 0x100 = display clock).
A machine without NVML reports "unavailable" and exits 2; nothing is estimated in its place.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import ctypes as ct
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

FIELDS = ["ns", "graphics_mhz", "sm_mhz", "memory_mhz", "pstate", "power_mw", "util_gpu", "util_memory",
          "throttle", "temperature_c"]
THROTTLE = {0x1: "gpu_idle", 0x2: "applications_clocks", 0x4: "sw_power_cap", 0x8: "hw_slowdown",
            0x10: "sync_boost", 0x20: "sw_thermal", 0x40: "hw_thermal", 0x80: "hw_power_brake",
            0x100: "display_clock"}


class Nvml:
    def __init__(self):
        try:
            self.lib = ct.WinDLL("nvml.dll")
        except OSError as error:
            raise RuntimeError(f"NVML unavailable: {error}") from error
        if self.lib.nvmlInit_v2() != 0:
            raise RuntimeError("NVML unavailable: nvmlInit_v2 failed")
        self.device = ct.c_void_p()
        if self.lib.nvmlDeviceGetHandleByIndex_v2(0, ct.byref(self.device)) != 0:
            raise RuntimeError("NVML: no device 0")
        name = ct.create_string_buffer(96)
        self.lib.nvmlDeviceGetName(self.device, name, 96)
        self.name = name.value.decode(errors="replace")

        class Utilization(ct.Structure):
            _fields_ = [("gpu", ct.c_uint), ("memory", ct.c_uint)]
        self.Utilization = Utilization

    def sample(self):
        u, c, p, s, t, temp = ct.c_uint(), ct.c_uint(), ct.c_uint(), ct.c_int(), ct.c_ulonglong(), ct.c_uint()
        clocks = []
        for kind in (0, 1, 2):  # graphics, SM, memory
            clocks.append(c.value if self.lib.nvmlDeviceGetClockInfo(self.device, kind, ct.byref(c)) == 0 else -1)
        power = p.value if self.lib.nvmlDeviceGetPowerUsage(self.device, ct.byref(p)) == 0 else -1
        pstate = s.value if self.lib.nvmlDeviceGetPerformanceState(self.device, ct.byref(s)) == 0 else -1
        util = self.Utilization()
        if self.lib.nvmlDeviceGetUtilizationRates(self.device, ct.byref(util)) != 0:
            util.gpu = util.memory = 2**32 - 1
        throttle = t.value if self.lib.nvmlDeviceGetCurrentClocksThrottleReasons(self.device, ct.byref(t)) == 0 else -1
        temperature = temp.value if self.lib.nvmlDeviceGetTemperature(self.device, 0, ct.byref(temp)) == 0 else -1
        return [*clocks, pstate, power, util.gpu, util.memory, throttle, temperature]

    def close(self):
        self.lib.nvmlShutdown()


def percentiles(values):
    ordered = sorted(values)
    if not ordered:
        return {}
    pick = lambda p: ordered[max(0, math.ceil(len(ordered) * p / 100) - 1)]
    return {"count": len(ordered), "min": ordered[0], "p5": pick(5), "p50": pick(50), "p95": pick(95),
            "max": ordered[-1], "mean": statistics.mean(ordered)}


def summarize(rows):
    pstates, reasons = {}, {}
    for r in rows:
        pstates[f"P{r['pstate']}"] = pstates.get(f"P{r['pstate']}", 0) + 1
        for bit, name in THROTTLE.items():
            if r["throttle"] >= 0 and r["throttle"] & bit:
                reasons[name] = reasons.get(name, 0) + 1
    return {"samples": len(rows), "pstate_samples": pstates, "throttle_reason_samples": reasons,
            "graphics_mhz": percentiles([r["graphics_mhz"] for r in rows if r["graphics_mhz"] >= 0]),
            "memory_mhz": percentiles([r["memory_mhz"] for r in rows if r["memory_mhz"] >= 0]),
            "power_w": percentiles([r["power_mw"] / 1000 for r in rows if r["power_mw"] >= 0]),
            "util_gpu": percentiles([r["util_gpu"] for r in rows if r["util_gpu"] <= 100])}


def read_samples(path):
    with open(path, newline="") as f:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]


def run(args):
    out = Path(args.out)
    if out.exists():
        raise SystemExit("--out must be a new directory")
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        raise SystemExit("no command")
    try:
        nvml = Nvml()
    except RuntimeError as error:
        print(f"unavailable: {error}")
        return 2
    out.mkdir(parents=True)
    rows = []
    interval = args.interval_ms / 1000
    with open(out / "stdout.log", "wb") as so, open(out / "stderr.log", "wb") as se:
        process = subprocess.Popen(command, cwd=args.cwd, stdout=so, stderr=se, stdin=subprocess.DEVNULL)
        started = time.perf_counter_ns()
        while True:
            ns = time.perf_counter_ns()
            rows.append([ns, *nvml.sample()])
            if process.poll() is not None:
                break
            time.sleep(max(0.0, interval - (time.perf_counter_ns() - ns) / 1e9))
        # A tail after the process ends: the idle state for comparison.
        for _ in range(int(1.0 / interval)):
            rows.append([time.perf_counter_ns(), *nvml.sample()])
            time.sleep(interval)
    nvml.close()
    with open(out / "gpu_clocks.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(FIELDS)
        writer.writerows(rows)
    samples = [dict(zip(FIELDS, r)) for r in rows]
    summary = {"device": nvml.name, "command": command, "exit_code": process.returncode,
               "started_ns": started, "interval_ms": args.interval_ms, "while_running": summarize(
                   [s for s in samples if s["ns"] <= rows[-int(1.0 / interval) - 1][0]])}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary["while_running"], indent=1))
    return 0 if process.returncode == 0 else 1


def join(args):
    samples = read_samples(args.samples)
    times = [s["ns"] for s in samples]
    frames = []
    with open(args.gpu_trace, newline="") as f:
        lines = [line for line in f if not line.startswith("#")]
    for row in csv.DictReader(lines):
        begin, end = int(row["gpu_begin_ns"]), int(row["gpu_end_ns"])
        if begin <= 0 or end <= begin:
            continue
        # The samples inside the frame's GPU interval, else the nearest one before its end.
        lo, hi = bisect.bisect_left(times, begin), bisect.bisect_right(times, end)
        inside = samples[lo:hi] or ([samples[hi - 1]] if hi > 0 else [])
        if not inside:
            continue
        frames.append({"frame": int(row["frame"]), "gpu_ms": float(row["gpu_ms"]),
                       "graphics_mhz": min(s["graphics_mhz"] for s in inside),
                       "pstate": max(s["pstate"] for s in inside),
                       "sample_gap_ms": (end - inside[-1]["ns"]) / 1e6 if not samples[lo:hi] else 0.0})
    if not frames:
        raise SystemExit("no frame overlaps the samples (different clock or run?)")
    slow = [f for f in frames if f["gpu_ms"] > args.budget_ms]
    normal = [f for f in frames if f["gpu_ms"] <= args.budget_ms]
    by_clock = {}
    for f in frames:
        bucket = f"{(f['graphics_mhz'] // 250) * 250}-{(f['graphics_mhz'] // 250) * 250 + 249} MHz"
        by_clock.setdefault(bucket, []).append(f["gpu_ms"])
    result = {
        "frames_joined": len(frames), "budget_ms": args.budget_ms,
        "over_budget": {"count": len(slow), "graphics_mhz": percentiles([f["graphics_mhz"] for f in slow]),
                        "pstate": {f"P{p}": sum(1 for f in slow if f["pstate"] == p) for p in sorted({f["pstate"] for f in slow})},
                        "frames": [{k: f[k] for k in ("frame", "gpu_ms", "graphics_mhz", "pstate")} for f in
                                   sorted(slow, key=lambda f: -f["gpu_ms"])[:40]]},
        "within_budget": {"count": len(normal), "graphics_mhz": percentiles([f["graphics_mhz"] for f in normal]),
                          "gpu_ms": percentiles([f["gpu_ms"] for f in normal])},
        "gpu_ms_by_graphics_clock": {k: percentiles(v) for k, v in sorted(by_clock.items(),
                                                                           key=lambda kv: int(kv[0].split('-')[0]))},
        "limits": ["A frame's clock is the lowest graphics clock sampled during its GPU interval (or the last sample "
                   "before it when none falls inside); NVML updates clocks on its own period.",
                   "Correlation only: a low clock during a slow frame does not by itself say which caused which."],
    }
    Path(args.out).write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: result[k] for k in ("frames_joined", "over_budget")}, indent=1)[:3000])
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    r = sub.add_parser("run")
    r.add_argument("--out", required=True)
    r.add_argument("--cwd", default=None)
    r.add_argument("--interval-ms", type=float, default=10.0)
    r.add_argument("command", nargs=argparse.REMAINDER)
    j = sub.add_parser("join")
    j.add_argument("--samples", required=True)
    j.add_argument("--gpu-trace", required=True)
    j.add_argument("--budget-ms", type=float, default=1000 / 60)
    j.add_argument("--out", required=True)
    args = parser.parse_args()
    if args.mode == "run" and not 1 <= args.interval_ms <= 1000:
        parser.error("--interval-ms must be 1..1000")
    return run(args) if args.mode == "run" else join(args)


if __name__ == "__main__":
    sys.exit(main())
