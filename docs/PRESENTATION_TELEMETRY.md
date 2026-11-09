# Lifted renderer measurements

The release renderer uses `LiftedRender`, whose frames do not pass through the
legacy native renderer's counters. Earlier `--telemetry` rows and the JSON
`presentation` object therefore reported zero frames while gameplay rendered;
`--perf-log` contained no frame rows. Those fields cannot establish historical
frame performance or identify the recorded CPU temperature spike.

Use `--telemetry runtime.csv --perf-log presentation.csv --json-report session.json`
on a windowed or fullscreen run. These observers do not change the simulation
timestep or frame selection. Extra timing clocks, synchronization and CSV writes
are enabled by these flags, so use separate uninstrumented runs for performance
claims. Headless execution has no presentation rows. This observer does not
measure GPU execution, display scanout, CPU temperature or process CPU time.

## Frame rows, schema 1

Each call that attempts to render a dequeued packet writes one row, including
failed renders. Empty dequeue waits do not invent a frame. The first successful
return has no preceding interval; its interval cell is empty. Rows contain:

| Field | Meaning |
| --- | --- |
| `wall_seconds` | Steady clock at render/present return, relative to execution start |
| `present_index` | Successful returns so far plus one; a failure does not advance it |
| `packet_tick`, `game_ticks` | Packet identity and game clock carried by that packet |
| `success` | Actual renderer result |
| `render_return_interval_ms` | Time since the preceding successful render/present return |
| `packet_wait_ms` | All dequeue waits since the preceding attempted render, including empty waits |
| `capture_ms` | Synchronous screenshot capture wall time before this render |
| `render_present_ms` | CPU-side wall time inside render/present, including any waits it performs |

Packet cleanup, message processing and logging fall outside render/present
duration but remain included in the next return-to-return interval. A shutdown
dequeue wait may have no subsequent frame row; the final JSON includes it.
Capture stalls and other long intervals remain in all distributions.

## Interval rows, schema 1, renderer `lifted`

Rows are sampled on the game thread at intervals of at least one wall second.
`frames` and `attempts` are interval deltas; `fps = frames / interval_s`.
`game_speed` is elapsed game time divided by elapsed wall time.
`produced_per_game_s` counts new packets per game second, independently of the
number presented. `render_present_ms` is average wall duration per attempted
render; `packet_wait_ms` and `capture_ms` are interval totals. If no render was
attempted, the render duration cell is empty. GPU time is always empty.

`packets`, `dropped`, `skipped_draws`, `resource_failures`, `interp_instr` and
`host_stalls` are cumulative. `pending`, `textures_live` and `meshes_live` are
current counts. `draws_last` belongs to the last completed renderer frame.
RAM fields are current process working set and private commit, or empty when
the operating system query fails. `paced` and `unpaced` are interval deltas
from the existing host pacing counters.

Timing counters share one short mutex; queue and resource snapshots use their
own existing locks separately. The snapshot is not a globally simultaneous
sample across the game, render and queue owners. A boundary may divide work
between adjacent rows. Whole-session frame logs provide exact count accounting.

## Final report and verification

`renderer.backend` identifies `lifted`. Top-level `presentation` reports actual
successful returns, failed renders, queue accounting and measured wall times.
Histogram quantiles have explicit 0.1 ms bounds; the overflow bin starts at
100 ms and has no finite upper bound. Unavailable averages/quantiles are null.
`gpu_frame_ms` is null. `host_stats.presentation` is null because that legacy
collector is unused. `render_rate_applied` is false: the lifted renderer
consumes fresh game packets and does not implement the legacy render-rate option.
The 800 FPS target remains unmet.

Validate the entire frame log against its final report and interval rows:

```powershell
python -B tools/analyze_presentation.py --frames presentation.csv --session session.json --telemetry runtime.csv --out analysis.json
```

The analyzer rejects missing/reordered frames, contradictory timestamps,
invented GPU timing, invalid numbers, inconsistent rates and broken queue
accounting. It retains render failures, game-time rewinds and long frames.
It does not certify physics, pixel correctness or low observer overhead.
