"""Attribute repeated display refreshes from a GEKKO_PRESENT_TRACE capture.

The trace (runtime/lift/render/present_trace.h) records, in steady_clock nanoseconds, every packet the game hands over,
every drop, every pop, the frame-latency wait, each Present call with its present id, DXGI frame statistics read right
after it (which present reached the screen at which vblank) and the end of each packet's rendering.

This tool joins those rows per present and answers, for every refresh on which the display showed an old frame again
(a "repeat": the next displayed present landed more than one refresh after the previous one), which side was late:

  packet_late   the game pushed the packet after the vblank it needed (producer phase/work)
  render_late   the packet was there before the vblank, but Present was called after it (render thread)
  gpu_late      Present was called before the vblank and the frame still missed it (GPU / compositor)

It also reports the measured display refresh period against the 60 Hz pacing quantum, the phase of every push
relative to the vblank grid, push-to-push intervals and the drops. Measurement only: it changes nothing.

    python -B tools/present_trace_report.py trace.csv --out report.json
"""

import argparse
import bisect
import csv
import json
import math
import sys

PUSH, DROP_ON_PUSH, DROP_ON_POP, POP, WAIT_BEGIN, WAIT_END, PRESENT_BEGIN, PRESENT_END, STATS, RENDER_END, \
    DROP_ON_CLOSE = range(1, 12)
FAILED_STATS = (1 << 64) - 1
PACING_PERIOD_NS = 1e9 / 60.0
GAME_CLOCK_HZ = 40_500_000  # gekko/game_clock.h kGameTicksPerSecond


def read_trace(path):
    rows = []
    with open(path, newline="") as handle:
        lines = (line for line in handle if not line.startswith("#"))
        for row in csv.DictReader(lines):
            rows.append((int(row["ns"]), int(row["ev"]), int(row["a"]), int(row["b"]), int(row["c"]), int(row["d"])))
    return rows


def _fit(points):
    """Least-squares line ns = intercept + slope * refresh over (refresh, ns) samples."""
    n = len(points)
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    sxx = sum((p[0] - mx) ** 2 for p in points)
    if sxx == 0:
        raise ValueError("vblank samples cover a single refresh; cannot measure the refresh period")
    slope = sum((p[0] - mx) * (p[1] - my) for p in points) / sxx
    return my - slope * mx, slope


class VblankGrid:
    """Vblank times from DXGI's (SyncRefreshCount, SyncQPCTime) samples: the nearest sample, stepped by the period."""

    def __init__(self, samples):
        unique = sorted(dict(samples).items())
        if len(unique) < 2:
            raise ValueError("fewer than two distinct vblank samples in the trace")
        self.intercept, self.period = _fit(unique)
        self.counts = [c for c, _ in unique]
        self.times = [t for _, t in unique]
        residuals = [t - (self.intercept + self.period * c) for c, t in unique]
        self.max_residual_ns = max(abs(r) for r in residuals)

    def time(self, refresh):
        i = bisect.bisect_left(self.counts, refresh)
        candidates = [j for j in (i - 1, i) if 0 <= j < len(self.counts)]
        j = min(candidates, key=lambda k: abs(self.counts[k] - refresh))
        return self.times[j] + (refresh - self.counts[j]) * self.period

    def phase_ms(self, ns):
        """Time since the most recent vblank, in ms (0 .. period)."""
        offset = (ns - self.intercept) % self.period
        return offset / 1e6


def _quantiles(values):
    if not values:
        return None
    ordered = sorted(values)

    def q(p):
        return ordered[min(len(ordered) - 1, int(math.ceil(p * len(ordered))) - 1)]

    return {"count": len(ordered), "min": ordered[0], "p50": q(0.50), "p95": q(0.95), "p99": q(0.99),
            "max": ordered[-1], "mean": sum(ordered) / len(ordered)}


def _histogram(values, width, upper):
    bins = [0] * int(math.ceil(upper / width))
    for v in values:
        bins[min(len(bins) - 1, max(0, int(v // width)))] += 1
    return {"bin_ms": width, "counts": bins}


def analyze(rows, game_clock_hz=GAME_CLOCK_HZ):
    pushes, pops, render_end, drops = {}, {}, {}, []
    presents = {}      # present id -> fields
    displayed = {}     # present id -> refresh count it was displayed at
    sync_samples = []  # (SyncRefreshCount, ns)
    failed_stats = 0
    current = None     # the packet being rendered: tick, pop ns, wait ns, present begin ns
    for ns, ev, a, b, c, d in rows:
        if ev == PUSH:
            pushes[a] = ns
        elif ev in (DROP_ON_PUSH, DROP_ON_POP):
            drops.append({"tick": a, "ns": ns, "kind": "push" if ev == DROP_ON_PUSH else "pop"})
        elif ev == POP:
            pops[a] = ns
            current = {"tick": a, "pop": ns, "game_ticks": b}
        elif ev == WAIT_BEGIN and current is not None:
            current["wait_begin"] = ns
        elif ev == WAIT_END and current is not None:
            current["wait_end"] = ns
        elif ev == PRESENT_BEGIN and current is not None:
            current["present_begin"] = ns
        elif ev == PRESENT_END and current is not None:
            current["present_end"] = ns
            current["hresult"] = a
            presents[b] = dict(current)
        elif ev == STATS:
            if a == FAILED_STATS:
                failed_stats += 1
                continue
            if a != 0 or b != 0:
                displayed.setdefault(a, b)
            if c != 0 and d != 0:
                sync_samples.append((c, d))
        elif ev == RENDER_END:
            render_end[a] = ns
            current = None

    grid = VblankGrid(sync_samples)
    period_ms = grid.period / 1e6

    # Consecutive displayed presents. A packet's game step says how many refreshes the previous frame should stay on
    # screen (one for a 60 Hz step, two for the 30 Hz movie/menu steps); staying longer is a repeat, shorter is early.
    ids = sorted(i for i in displayed if i in presents)
    repeats, unresolved, fresh, early = [], 0, 0, 0
    causes = {"packet_late": 0, "render_late": 0, "gpu_late": 0}
    for prev, cur in zip(ids, ids[1:]):
        gap_ids = cur - prev
        gap_refresh = displayed[cur] - displayed[prev]
        if gap_ids != 1:
            unresolved += 1
            continue
        step_ns = (presents[cur]["game_ticks"] - presents[prev]["game_ticks"]) * 1e9 / game_clock_hz
        wanted = max(1, round(step_ns / grid.period))
        if gap_refresh < wanted:
            early += 1
            continue
        if gap_refresh == wanted:
            fresh += 1
            continue
        p = presents[cur]
        expected = displayed[prev] + wanted
        deadline = grid.time(expected)
        push = pushes.get(p["tick"])
        if push is not None and push >= deadline:
            cause = "packet_late"
        elif p.get("present_begin", deadline) >= deadline:
            cause = "render_late"
        else:
            cause = "gpu_late"
        causes[cause] += 1
        repeats.append({
            "present": cur, "tick": p["tick"], "refreshes_shown_previous": gap_refresh, "refreshes_wanted": wanted,
            "cause": cause,
            "deadline_ns": deadline,
            "push_minus_deadline_ms": None if push is None else (push - deadline) / 1e6,
            "present_begin_minus_deadline_ms": (p["present_begin"] - deadline) / 1e6 if "present_begin" in p else None,
            "push_phase_ms": None if push is None else grid.phase_ms(push),
        })

    ordered_pushes = sorted(pushes.values())
    push_intervals = [(b - a) / 1e6 for a, b in zip(ordered_pushes, ordered_pushes[1:])]
    push_phases = [grid.phase_ms(t) for t in ordered_pushes]
    render_ms = [(render_end[t] - pops[t]) / 1e6 for t in pops if t in render_end]
    pop_after_push = [(pops[t] - pushes[t]) / 1e6 for t in pops if t in pushes]

    beat = None
    drift = grid.period - PACING_PERIOD_NS
    if drift != 0:
        # The pacer's 60 Hz ladder slides one whole refresh against the display every |period^2 / drift|.
        beat = abs(grid.period * PACING_PERIOD_NS / drift) / 1e9

    return {
        "display": {
            "refresh_period_ms": period_ms,
            "refresh_hz": 1e3 / period_ms,
            "pacing_period_ms": PACING_PERIOD_NS / 1e6,
            "pacing_minus_refresh_ppm": (PACING_PERIOD_NS - grid.period) / grid.period * 1e6,
            "phase_beat_seconds": beat,
            "vblank_fit_max_residual_ms": grid.max_residual_ns / 1e6,
            "vblank_samples": len(sync_samples),
            "failed_statistics": failed_stats,
        },
        "presents": {
            "recorded": len(presents),
            "displayed_known": len(ids),
            "consecutive_pairs_fresh": fresh,
            "consecutive_pairs_repeat": len(repeats),
            "consecutive_pairs_early": early,
            "pairs_unresolved": unresolved,
            "repeat_causes": causes,
        },
        "packets": {
            "pushed": len(pushes),
            "dropped": len(drops),
            "dropped_on_push": sum(1 for d in drops if d["kind"] == "push"),
            "dropped_on_pop": sum(1 for d in drops if d["kind"] == "pop"),
            "push_interval_ms": _quantiles(push_intervals),
            "push_phase_ms": _histogram(push_phases, 1.0, period_ms),
            "repeat_push_phase_ms": _histogram([r["push_phase_ms"] for r in repeats if r["push_phase_ms"] is not None],
                                               1.0, period_ms),
            "drop_phase_ms": _histogram([grid.phase_ms(d["ns"]) for d in drops], 1.0, period_ms),
            "pop_after_push_ms": _quantiles(pop_after_push),
            "pop_to_render_end_ms": _quantiles(render_ms),
        },
        "repeats": repeats,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace")
    parser.add_argument("--out")
    args = parser.parse_args(argv)
    report = analyze(read_trace(args.trace))
    text = json.dumps(report, indent=1)
    if args.out:
        with open(args.out, "w", newline="\n") as handle:
            handle.write(text + "\n")
    summary = {k: report[k] for k in ("display", "presents")}
    summary["packets"] = {k: report["packets"][k] for k in ("pushed", "dropped", "push_interval_ms")}
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
