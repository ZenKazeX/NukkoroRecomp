#!/usr/bin/env python3
"""Replay a recording at chosen display settings and report framerate, pacing and latency (docs/tools/RECORDER.md).

    python -B tools/replay_report.py <recording dir> [--resolution 2560x1440] [--mode windowed|borderless|exclusive]
        [--aspect auto|16:9|...] [--render-scale 100] [--render 2560x1440] [--msaa 8] [--anisotropy 16]
        [--boot-stage stg0602] [--seconds N] [--out DIR] [--exe PATH] [--no-clocks]

The recording directory holds initial/ (card.raw, SRAM.pc.raw, pad.cfg) and input.tape (or the older run/input.tape).
The replay runs from fresh copies of initial/; the game logic takes the tape's recorded aspect, so the picture may be any
resolution, aspect, render scale, MSAA or filtering. Traces: GEKKO_PRESENT_TRACE, GEKKO_GPU_TRACE, GEKKO_GPU_MEMORY and
(unless --no-clocks) the NVML clock sampler. Output: <out>/report.md and report.json.
"""
import argparse
import csv
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_EXE = os.path.join(REPO, ".build", "watch", "bin", "Release", "ShadowTheHedgehog.exe")
GAME = "H:/ShadowTheHedgehogDiskGC_Extract/game"
TICKS_PER_SECOND = 40_500_000


def tape_info(path):
    """start tick, the newest record's tick, meta (a version 3 tape) - read without the game."""
    start, last, meta = 0, 0, {}
    with open(path, encoding="ascii", errors="replace") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "start":
                start = int(parts[1])
            elif parts[0] == "meta" and len(parts) >= 2:
                meta[parts[1]] = " ".join(parts[2:])
            elif parts[0] in ("sample", "check", "logic", "output", "setting") and len(parts) >= 2:
                last = max(last, int(parts[1]))
    return start, last, meta


def quantile(values, q):
    v = sorted(values)
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))] if v else None


def present_stats(present_csv, skip_s):
    """Displayed frames from DXGI's own counts (Stats events): fps, 1-s windows, frame times, repeated refreshes.

    A Stats read can skip presents (two or more shown between reads), so the time between two reads is not one frame.
    A gap of n presents over r refreshes is n frames: n - 1 of one refresh and one of r - n + 1. The extra refreshes are
    put on one frame, the worst case for the lows."""
    rows = [r for r in csv.reader(l for l in open(present_csv) if not l.startswith("#"))][1:]
    if not rows:
        return {}
    t0 = int(rows[0][0]) + skip_s * 1e9
    seen = {}
    for r in rows:
        if r[1] == "9" and int(r[0]) >= t0 and int(r[2]) != (1 << 64) - 1 and int(r[2]) not in seen:
            seen[int(r[2])] = (int(r[3]), int(r[5]))  # present count -> (refresh it showed at, its vblank ns)
    counts = sorted(seen)
    if len(counts) < 2:
        return {}
    first, last = seen[counts[0]], seen[counts[-1]]
    refreshes = last[0] - first[0]
    if refreshes <= 0:
        return {}
    period_ms = (last[1] - first[1]) / 1e6 / refreshes
    held, shown_at, repeats, skipped_reads = [], [], 0, 0
    for a, b in zip(counts, counts[1:]):
        n, r = b - a, seen[b][0] - seen[a][0]
        if r < n:  # a counter reset (mode change): not a frame
            continue
        held += [1] * (n - 1) + [r - n + 1]
        repeats += r - n
        skipped_reads += n - 1
        # Each frame's vblank (reads can be a second or more apart): the first n - 1 one refresh apart, the last at b.
        shown_at += [seen[a][1] + (k + 1) * period_ms * 1e6 for k in range(n - 1)] + [seen[b][1]]
    frame_ms = [h * period_ms for h in held]
    seconds = (last[1] - first[1]) / 1e9
    windows, t, i = [], first[1], 0
    while t + 1e9 <= last[1]:
        count = 0
        while i < len(shown_at) and shown_at[i] < t + 1e9:
            count += shown_at[i] >= t
            i += 1
        windows.append(count)
        t += 1e9
    worst = sorted(frame_ms)[-max(1, len(frame_ms) // 100):]
    return {
        "seconds": round(seconds, 1),
        "frames": len(frame_ms),
        "refresh_hz": round(1000.0 / period_ms, 3),
        "fps_avg": round(len(frame_ms) / seconds, 2) if seconds else None,
        "fps_1s_min": min(windows) if windows else None,
        "fps_1s_max": max(windows) if windows else None,
        "fps_1pct_low": round(1000.0 / statistics.mean(worst), 1) if worst else None,
        "frame_ms_p50": round(quantile(frame_ms, 0.5), 2),
        "frame_ms_p99": round(quantile(frame_ms, 0.99), 2),
        "frame_ms_max": round(max(frame_ms), 2),
        "repeated_refreshes": repeats,
        "frames_held": {str(k): held.count(k) for k in sorted(set(held))},
        "presents_between_stats_reads": skipped_reads,
    }


def gpu_stats(gpu_csv, skip_s):
    rows = [r for r in csv.DictReader(l for l in open(gpu_csv) if not l.startswith("#"))]
    if not rows:
        return {}
    t0 = int(rows[0]["cpu_begin_ns"]) + skip_s * 1e9
    gpu = [float(r["gpu_ms"]) for r in rows if int(r["cpu_begin_ns"]) >= t0]
    cpu = [(int(r["cpu_submit_ns"]) - int(r["cpu_begin_ns"])) / 1e6 for r in rows if int(r["cpu_begin_ns"]) >= t0]
    return {"gpu_ms_p50": round(quantile(gpu, 0.5), 2), "gpu_ms_p95": round(quantile(gpu, 0.95), 2),
            "gpu_ms_p99": round(quantile(gpu, 0.99), 2), "gpu_ms_max": round(max(gpu), 2),
            "record_ms_p50": round(quantile(cpu, 0.5), 2), "record_ms_p99": round(quantile(cpu, 0.99), 2)}


def vram_peak(memory_csv):
    try:
        peak = 0
        for r in csv.DictReader(l for l in open(memory_csv) if not l.startswith("#")):
            peak = max(peak, int(r.get("local_usage", 0) or 0))
        return round(peak / 2**20)
    except OSError:
        return None


def _picture(resolution, aspect):
    """The game picture inside the output, as runtime/include/gekko/display_geometry.h display_geometry() makes it."""
    ow, oh = (int(v) for v in resolution.lower().split("x"))
    text = aspect.strip().lower()
    if text in ("auto", "0"):
        return ow, oh
    if text == "native":
        text = "4:3"
    try:
        if ":" in text or "x" in text:
            n, d = text.replace("x", ":").split(":")
            ratio = float(n) / float(d)
        else:
            ratio = float(text)
    except ValueError:
        sys.exit("--render: cannot work out the picture for --aspect %s" % aspect)
    if not 0.25 <= ratio <= 8.0:
        sys.exit("--render: --aspect %s is outside 0.25..8" % aspect)
    if abs(ratio / (ow / oh) - 1.0) <= 0.03:  # kRatioSnap: the picture fills the output
        return ow, oh
    if ow / oh > ratio:
        return max(1, min(ow, round(oh * ratio))), oh
    return ow, max(1, min(oh, round(ow / ratio)))


def exact_render_scale(render, resolution, aspect):
    """display.cfg no longer accepts render_width / render_height (display.cpp ignores them with a warning; the render
    size is picture x render_scale, display_geometry.h). An exact WxH is therefore the render_scale that rounds to it
    on both sides, or an error: never a silent fall back to the window's size."""
    try:
        rw, rh = (int(v) for v in render.lower().split("x"))
    except ValueError:
        sys.exit("--render must be WxH, e.g. 854x480")
    pw, ph = _picture(resolution, aspect)
    scale = 100.0 * rw / pw
    text = float("%.9f" % scale)
    if not 25.0 <= text <= 200.0:
        sys.exit("--render %s is %.1f %% of the %dx%d picture; the build accepts a render scale of 25..200 %% only "
                 "(display.cfg render_width/render_height are ignored). Change --resolution/--aspect so the picture "
                 "is 0.5x..4x the render size." % (render, text, pw, ph))
    got = (int(math.floor(pw * text / 100.0 + 0.5)), int(math.floor(ph * text / 100.0 + 0.5)))
    if got != (rw, rh):
        sys.exit("--render %s is not reachable from the %dx%d picture by one scale (render_scale=%.6f gives %dx%d; "
                 "the build keeps the picture's shape). Use a --resolution/--aspect whose picture has the render "
                 "size's shape." % (render, pw, ph, text, got[0], got[1]))
    print("--render %s -> render_scale=%.6f of the %dx%d picture" % (render, text, pw, ph))
    return text


def main():
    ap =argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording")
    ap.add_argument("--resolution", default="1920x1080")
    ap.add_argument("--mode", choices=("windowed", "borderless", "exclusive"), default="windowed")
    ap.add_argument("--aspect", default="auto")
    ap.add_argument("--render-scale", type=float, default=100.0)
    ap.add_argument("--render", default="", help="an exact 3D render size WxH, converted to the equivalent render_scale of the picture "
                    "(wins over --render-scale; an error when no scale of 25..200 %% gives exactly WxH)")
    ap.add_argument("--msaa", type=int, default=4)
    ap.add_argument("--anisotropy", type=int, default=16)
    ap.add_argument("--boot-stage", default="")
    ap.add_argument("--seconds", type=float, default=0.0, help="game seconds to run (default: the tape's length + 2)")
    ap.add_argument("--skip", type=float, default=15.0, help="seconds left out of the statistics (loading)")
    ap.add_argument("--out", default="")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--no-clocks", action="store_true")
    ap.add_argument("--vsync", choices=("default", "on", "off"), default="default",
                    help="V-Sync for the run (--vsync / --no-vsync); default: the build's own default")
    ap.add_argument("--report-only", action="store_true",
                    help="rebuild report.md / report.json from an earlier run's --out folder; the game is not run")
    a = ap.parse_args()
    if a.report_only and not (a.out and os.path.isdir(a.out)):
        sys.exit("--report-only needs --out <an earlier run's folder>")

    rec = os.path.abspath(a.recording)
    tape = os.path.join(rec, "input.tape")
    if not os.path.exists(tape):
        tape = os.path.join(rec, "run", "input.tape")
    start, last, meta = tape_info(tape)
    stage = a.boot_stage or (("stg%04d" % int(meta["boot_stage"])) if "boot_stage" in meta else "")
    if not stage:
        sys.exit("no start route: the tape has no meta boot_stage (recorded before version 3) - pass --boot-stage")
    seconds = a.seconds or (last - start) / TICKS_PER_SECOND + 2.0
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = os.path.abspath(a.out or os.path.join(rec, "reports", stamp))
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ)
    env["GEKKO_PRESENT_TRACE"] = os.path.join(out, "present.csv")
    env["GEKKO_GPU_TRACE"] = os.path.join(out, "gpu.csv")
    env["GEKKO_GPU_MEMORY"] = os.path.join(out, "gpu_memory.csv")
    if a.report_only:
        try:
            rc = json.load(open(os.path.join(out, "report.json"))).get("exit_code", -1)
        except (OSError, ValueError):
            rc = -1
    else:
        for name in ("card.raw", "SRAM.pc.raw", "pad.cfg"):
            src = os.path.join(rec, "initial", name)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(out, name))
        w, h = a.resolution.lower().split("x")
        if a.render:
            a.render_scale = exact_render_scale(a.render, a.resolution, a.aspect)
        cfg = ["width=" + w, "height=" + h, "fullscreen=" + ("false" if a.mode == "windowed" else "true"),
               "exclusive=" + ("true" if a.mode == "exclusive" else "false"), "aspect=" + a.aspect,
               "render_scale=%s" % ("%.9f" % a.render_scale).rstrip("0").rstrip("."), "msaa=%d" % a.msaa,
               "anisotropy=%d" % a.anisotropy, "fps_counter=false"]
        with open(os.path.join(out, "display.cfg"), "w", newline="\n") as f:
            f.write("\n".join(cfg) + "\n")
        game = [a.exe, "--game-data", GAME, "--audio", "--memory-card", "card.raw", "--display-cfg", "display.cfg",
                "--pad-cfg", "pad.cfg", "--state-file", "unused.gekkostate", "--snapshot-key", "none", "--no-xinput",
                "--boot-stage", stage, "--budget-ms", str(int(seconds * 1000)), "--input-tape-play", tape,
                "--json-report", "session.json", "--perf-log", "perf.csv"]
        if a.vsync != "default":
            game.append("--vsync" if a.vsync == "on" else "--no-vsync")
        cmd = game if a.no_clocks else [sys.executable, "-B", os.path.join(HERE, "gpu_clock_sampler.py"), "run",
                                        "--out", os.path.join(out, "clocks"), "--interval-ms", "10", "--"] + game
        print("replaying %s (%s, %.0f s) at %s %s, render %s, MSAA %dx, AF %dx -> %s" % (
            tape, stage, seconds, a.resolution, a.mode, a.render or ("%g%%" % a.render_scale), a.msaa, a.anisotropy,
            out))
        with open(os.path.join(out, "stdout.log"), "w") as so, open(os.path.join(out, "stderr.log"), "w") as se:
            rc = subprocess.call(cmd, cwd=out, env=env, stdout=so, stderr=se)

    subprocess.call([sys.executable, "-B", os.path.join(HERE, "frame_telemetry_report.py"), "--present",
                     env["GEKKO_PRESENT_TRACE"], "--gpu", env["GEKKO_GPU_TRACE"], "--out", os.path.join(out, "telemetry")]
                    + (["--clocks", os.path.join(out, "clocks", "gpu_clocks.csv")]
                       if os.path.exists(os.path.join(out, "clocks", "gpu_clocks.csv")) else []),
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    report = {"recording": rec, "tape": tape, "stage": stage, "exit_code": rc,
              "settings": {"resolution": a.resolution, "mode": a.mode, "aspect": a.aspect,
                           "render": a.render or ("%g%%" % a.render_scale), "msaa": a.msaa, "anisotropy": a.anisotropy,
                           "vsync": a.vsync}}
    report["frames"] = present_stats(env["GEKKO_PRESENT_TRACE"], a.skip)
    report["gpu"] = gpu_stats(env["GEKKO_GPU_TRACE"], a.skip)
    report["vram_peak_mib"] = vram_peak(env["GEKKO_GPU_MEMORY"])
    try:
        s = json.load(open(os.path.join(out, "telemetry", "summary.json")))
        report["latency_ms"] = {k: {q: round(v[q], 1) for q in ("p50", "p99", "max") if q in v}
                                for k, v in s.get("latency_ms", {}).items() if isinstance(v, dict)}
    except (OSError, ValueError):
        report["latency_ms"] = {}
    try:
        res = json.load(open(os.path.join(out, "session.json"))).get("resolution", {})
        report["geometry"] = res.get("geometry", {})
        report["quality"] = res.get("quality", {})
    except (OSError, ValueError):
        pass
    try:
        clocks = json.load(open(os.path.join(out, "clocks", "summary.json"))).get("while_running", {})
        report["clocks"] = {"core_mhz_p50": clocks.get("graphics_mhz", {}).get("p50"),
                            "memory_mhz_p50": clocks.get("memory_mhz", {}).get("p50"),
                            "power_w_p50": clocks.get("power_w", {}).get("p50")}
    except (OSError, ValueError):
        pass
    try:
        report["repeat_causes"] = json.load(open(os.path.join(out, "telemetry", "summary.json"))).get("repeat_causes", {})
    except (OSError, ValueError):
        report["repeat_causes"] = {}
    # The game's own log: under the clock sampler it is the sampler's child output (clocks/), otherwise the run folder.
    replay_lines = []
    for folder in (os.path.join(out, "clocks"), out):
        for name in ("stdout.log", "stderr.log"):
            try:
                lines = open(os.path.join(folder, name), errors="replace").read().splitlines()
            except OSError:
                continue
            replay_lines += [l.strip() for l in lines if l.startswith("[replay]") or l.startswith("[logic]")
                             or l.lstrip().startswith("consumer:")]
    report["replay"] = replay_lines[:12]
    json.dump(report, open(os.path.join(out, "report.json"), "w"), indent=1)

    f, g, lat = report["frames"], report["gpu"], report["latency_ms"]
    total = next((v for k, v in lat.items() if k.startswith("latch -> displayed vblank")), {})
    md = ["# Replay report", "",
          "- Recording: `%s` (%s), %.0f s, exit %d" % (rec, stage, seconds, rc),
          "- Settings: %s %s, aspect %s, render %s, MSAA %dx, AF %dx" % (
              a.resolution, a.mode, a.aspect, report["settings"]["render"], a.msaa, a.anisotropy),
          "- Picture: output %s, viewport %s, 3D render %s" % (
              report.get("geometry", {}).get("output"), report.get("geometry", {}).get("viewport"),
              report.get("geometry", {}).get("render")),
          "", "## Framerate (after %g s)" % a.skip, "",
          "- Average %s fps; 1-s min %s / max %s; 1%% low %s fps" % (
              f.get("fps_avg"), f.get("fps_1s_min"), f.get("fps_1s_max"), f.get("fps_1pct_low")),
          "- Frame time p50 %s ms, p99 %s ms, worst %s ms; repeated refreshes %s" % (
              f.get("frame_ms_p50"), f.get("frame_ms_p99"), f.get("frame_ms_max"), f.get("repeated_refreshes")),
          "- Frames by refreshes on screen: %s (%s Hz); repeat causes (whole run): %s" % (
              f.get("frames_held"), f.get("refresh_hz"), report["repeat_causes"]),
          "", "## Latency (whole run)", "",
          "- Input latch -> displayed vblank: p50 %s ms, p99 %s ms, max %s ms" % (
              total.get("p50"), total.get("p99"), total.get("max"))]
    md += ["- %s: p50 %s / p99 %s / max %s ms" % (k, v.get("p50"), v.get("p99"), v.get("max"))
           for k, v in lat.items() if not k.startswith("latch -> displayed vblank")]
    md += [
          "", "## GPU / memory", "",
          "- GPU frame p50 %s / p95 %s / p99 %s / max %s ms; render thread p50 %s ms" % (
              g.get("gpu_ms_p50"), g.get("gpu_ms_p95"), g.get("gpu_ms_p99"), g.get("gpu_ms_max"), g.get("record_ms_p50")),
          "- VRAM peak %s MiB; clocks %s" % (report["vram_peak_mib"], report.get("clocks")),
          "", "## Replay", ""] + ["- " + l for l in replay_lines[:12]]
    open(os.path.join(out, "report.md"), "w").write("\n".join(md) + "\n")
    print("\n".join(md))


if __name__ == "__main__":
    main()
