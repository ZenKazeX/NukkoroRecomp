# Per-frame telemetry (2026-10-31)

Phase III of docs/DIRECTIVE_2026-10-31_POWER.md. A measurement foundation: every claim about frame delivery, latency,
CPU / GPU work or power must be reproducible from raw records with the tools below. Measurement only - no scheduling,
pacing or power behaviour was changed by this work.

## 1. What is recorded

Everything goes into the existing presentation trace (runtime/lift/render/present_trace.h, GEKKO_PRESENT_TRACE=<csv>,
compiled out of shipping builds; use the diag preset) and the GPU trace (GEKKO_GPU_TRACE=<csv>, also in shipping).
One clock: std::chrono::steady_clock = QueryPerformanceCounter, in nanoseconds.

| event | where | fields |
|---|---|---|
| Push / DropOnPush / DropOnPop / DropOnClose | packet_queue.h | packet tick, game ticks, queue depth |
| PacketWait (new) | lifted_render.cpp present_newest | 1 packet / 0 timed out, wait start |
| Pop | lifted_render.cpp | packet tick, game ticks |
| WaitBegin / WaitEnd | swap_chain.cpp | the frame-latency waitable |
| PresentBegin / PresentEnd | swap_chain.cpp | sync interval, flags; HRESULT, present id (GetLastPresentCount) |
| Stats | swap_chain.cpp | DXGI frame statistics: last displayed PresentCount, its PresentRefreshCount, SyncRefreshCount, SyncQPCTime |
| PresentMode (new) | swap_chain.cpp | DXGI_FRAME_PRESENTATION_MODE via IDXGISwapChainMedia, or unavailable + HRESULT |
| RenderEnd | lifted_render.cpp | packet tick, success |
| PacerSleep (new) | shadow_main.cpp pace() | game thread: sleep start, deadline, wake (row time), game ticks |
| InputLatch (new) | shadow_main.cpp host service | game ticks of the latch, any port changed, port 0 controls |
| GpuCalibration (new) | gpu_timer.cpp | every GetClockCalibration pair (GPU timestamp, frequency, QPC ns) |
| ThreadCpu (new) | thread_sampler.cpp, 1 Hz | per thread of the process: user + kernel time, context switches (the OS's count of switch-ins = wakeups), cycles (QueryThreadCycleTime); thread names from GetThreadDescription |

GPU trace rows (per renderer frame): frame number, packet tick, game ticks, CPU recording begin, CPU submit (stamped
after ExecuteCommandLists returns), CPU present, GPU begin / end (timestamp queries mapped through the calibration),
per-pass GPU times.

Separate process: tools/gpu_clock_sampler.py (NVML; NVIDIA only): graphics / memory clock, P-state, board power,
utilization, throttle reasons, every 10 ms, stamped with the same QPC clock.

Not recorded, said so in every report: CPU package power (no energy meter exposed on this machine - no energy-meter
device, no power counter set; i9-13900K, Windows 11), CPU C-state residency, and the presentation mode (see 3).

## 2. Identities (never assumed one-to-one)

simulation frame (packet game_ticks / 675,000) -> packet (tick; pushed once, then popped or dropped) -> GPU frame
(gpu.csv frame, joined by packet tick) -> present (present id) -> displayed frame (a present id DXGI reports at a
refresh count). tools/frame_telemetry_report.py writes one row per packet with every identity it reached
(frames.csv) and counts each population separately.

## 3. Validation built into the report

- vblank grid: (SyncRefreshCount, SyncQPCTime) pairs fitted to a period; max residual reported (0.07 ms typical).
- GPU clock mapping: drift between consecutive calibrations (~9 us per second measured) - the uncertainty of every
  GPU timestamp in steady_clock.
- causality per frame: GPU begin not before the CPU began recording it; displayed vblank not before its GPU end;
  displayed vblank not before Present returned. Any violation is counted and reported.
- displayed frames come only from DXGI's statistics, never from Present or GPU completion. DXGI reports the LAST
  displayed present at each read; when two are shown between reads, one is not reported. Displayed fps is therefore
  reported as a lower bound, and unresolved pairs are counted.
- presentation mode: IDXGISwapChainMedia::GetFrameStatisticsMedia returns E_NOINTERFACE for this swap chain, so the
  mode is NOT identified by the trace. The measured presentation delay (whole refreshes between GPU end and the
  displayed vblank, from on-time frames) is reported as a measurement, not as the mode. Identifying the mode needs the
  kernel's present events (ETW, as PresentMon reads them) - not installed here.

## 4. Display truth: repeats and stale refreshes

- repeat: two consecutive displayed presents further apart in refreshes than their game-time step wants.
- stale refresh (added after checking the 3840x1200 run): a refresh that kept an older image although a newer packet
  had been pushed before that vblank. A dropped packet makes the next shown frame a two-frame step, which the repeat
  definition calls on time; the stale count does not depend on the step. Both are reported.

## 5. Tools

```
python -B tools/frame_telemetry_report.py --present present.csv --gpu gpu.csv [--clocks gpu_clocks.csv] --out report
python -B tools/telemetry_baseline_table.py <runs dir> [--prefix wst-] [--overhead]
python -B tools/repeat_attribution.py --report present.json --gpu gpu.csv --trace present.csv --out repeats.csv
```

Runner: .build/validation/telemetry-2026-10-31/bench.ps1 (one run; keeps run.json with the exact command, exe hash,
start time) and batch.ps1 (the baseline set). Raw traces stay in each run folder.
