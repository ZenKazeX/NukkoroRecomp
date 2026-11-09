# Timing architecture of the running game (2026-10-31)

Phase 2 of docs/DIRECTIVE_2026-10-31_POWER.md: what decides when each thing happens, from the game clock to the photons.
Facts with file:line (shipping lifted build, SHADOW_RENDER_LIFTED=ON); the thread / wait inventory was gathered by a
read-only sweep and its top items were re-checked in the code. Nothing here is changed yet.

## 1. The dependency chain

```
game clock (40.5 MHz ticks, gekko/game_clock.h)
  frame k of a 60 Hz game starts at k * 675,000 ticks (FRAME_PACING.md "One time base")
  NativeFrameLimiter (native_frame_limiter.cpp:316-375): advances GAME time only, never waits on wall time
game thread (shadow_main.cpp:4383; THREAD_PRIORITY_ABOVE_NORMAL, EcoQoS off, shadow_main.cpp:190-219)
  translated game code runs ahead of wall time; GameEvent::Frame fires at every 1/300 s pacing tick
  (system.h:268 kPacingHz = 300) and the host service every 5 ms of game time (input latch, stop / savestate points)
  pace() -> FixedSimulationClock::observe (host_timing.cpp:92-258): deadline on the absolute 1/300 s grid;
    sleeps when the deadline is > now + 0.2 ms (host_timing.cpp:232)
  RecoveredClockSleeper::sleep_until (shadow_main.cpp:482-506): high-resolution waitable timer, re-armed on an early
    wake; no spin, no timeBeginPeriod anywhere
  end of a game frame -> FramePacket pushed (packet_queue.h push: never blocks; notify_one)
audio thread "audio-wasapi" (audio_runtime.cpp:1824; MMCSS Pro Audio): event-driven WASAPI (device period ~10 ms or
  the IAudioClient3 minimum); the game's AX passes are every 5 ms of game time, so audio follows game time, and game
  time follows wall time through the pacer
UI / present thread (main thread, shadow_main.cpp:4634-4754)
  PeekMessage pump (never blocks), settings, resize check, pad sample (XInput 0 every iteration)
  present_newest(8 ms) -> PacketQueue::pop_newest: condition wait, 8 ms timeout (packet_queue.h:45)
  Renderer::render (renderer.cpp:3219):
    swap chain waitable (WaitForSingleObjectEx, max frame latency 1; swap_chain.cpp:55, 100)
    frame-slot fence (2 frames in flight, frame_loop.cpp:54 / gpu_device.cpp:156-160)
    record + ExecuteCommandLists; Present(1, 0) with vsync (FLIP_DISCARD, 3 buffers, ALLOW_TEARING flag set but unused
    at interval 1)
GPU: executes the frame; DXGI / DWM put it on screen
```

Measured on the 3840x1200 Westopolis ladder run (L-3840x1200, diag build, 60 Hz display, refresh period 16.6665 ms):
- the game pushes packets at phase ~10 ms after a vblank (push phase bins 9-10 ms: 5,313 of 5,360);
- the present thread pops them 7.0 ms later (p50; p95 7.6), i.e. right after the next vblank, when the swap chain's
  waitable lets the previous present go;
- the GPU starts ~0.4-1 ms after that vblank;
- a frame reaches the screen one whole refresh after its GPU work ends (on-time frames: vblank shown minus GPU end
  >= 16.9 ms, p50 26.3 ms) and two refreshes after Present returns (p50 32.7 ms): the frame is composed by the
  desktop compositor, not independently flipped. Why (overlays, the window, the scaling) is not established yet -
  it needs a presentation-mode measurement (PresentMon reports it; not installed here).

So the GPU's budget for a frame is from ~0.5 ms after vblank V(k-1) to V(k-1) + one refresh - about 15.5-16 ms - and
the input-to-display path holds a packet ~7 ms in the queue plus one composition refresh.

## 2. The 142 repeated frames, one by one

tools/repeat_attribution.py joins present_trace_report.py's repeats with the same packets' GPU timestamps and measures
the presentation delay from the run's own on-time frames (1 refresh here). Result (L-3840x1200/repeats.csv):

| class | count | GPU ms min / median / max | late by (median / max) | when |
|---|---|---|---|---|
| gpu_work: the frame's own GPU work did not fit | 95 | 16.1 / 17.0 / 20.7 | 1.39 / 5.12 ms | 25-89 s |
| gpu_queued: began late, its work alone would have fitted | 36 | 14.0 / 15.8 / 16.3 | 0.38 / 2.61 ms | 20-90 s |
| after_gpu: finished at the deadline edge (within 0.04 ms) | 8 | 15.1 / 15.7 / 15.8 | -0.04 / 0.00 ms | 20-90 s |
| submit_late: submitted after the deadline (boot) | 3 | 0.1 | 0.36 / 0.43 ms | 0.2-0.4 s |

None is packet_late or render_late: the game and the present thread were always in time. None is intentional game
behaviour (the 30 Hz movie / menu steps are counted as wanted, not repeats). Pattern in the timeline: a frame whose
GPU work runs past the next vblank pushes the following frame a refresh later; one packet is then dropped as stale
(e.g. ticks 1168-1176). That run had no clock sampler, so whether these frames ran at low clocks is NOT known for it;
the Lava Shelter replays (GPU_FILL.md 27) show the same frame-cost growth in P5 (810 MHz memory).

## 3. Findings for phase 4 (none changed yet)

1. Game thread: up to 5 sleeps per 60 Hz frame. Each 1/300 s pacing tick whose deadline is > 0.2 ms away arms a
   timer and sleeps (host_timing.cpp:232, shadow_main.cpp:3437). The 1/300 s grid exists so every frame boundary
   is a pacing point (FRAME_PACING.md 2026-10-29); the extra slices inside a frame keep game time close to wall time
   for the 5 ms audio passes. Whether fewer slices change audio timing must be measured before anything changes.
2. Present thread: an 8 ms timed packet wait (packet_queue.h:45, shadow_main.cpp:4732). With no packet (loading,
   pause) the loop runs ~125 times a second doing pump / settings / resize / pad work for nothing.
3. Present thread order: the packet is taken first, the swap chain's waitable second; a packet then waits ~7 ms in
   the render call for the vblank. Taking the newest packet after the waitable signals would not change work but would
   cut input-to-display latency (and the 425 packets dropped on pop show packets are made that are never shown).
4. Resource worker: submit() wakes the worker and the submitter then runs the same job itself (resource_worker.cpp:67,
   107); finished_.notify_all() after every job (96). Wakeups that may do no work.
5. Audio: a 1 kHz timer only while the stream primes (audio_runtime.cpp:1648-1651), short-lived.
6. Correctness note: Queue::wait shares one auto-reset event per queue (gpu_device.cpp:126, 159); two threads waiting
   on the same queue could take each other's signal.
7. Composition: frames are composed, costing one refresh of latency and tightening the GPU deadline; whether an
   independent flip is reachable (and what blocks it) is to be measured.

## 4. Telemetry today and what is missing (phase 1)

| quantity | today | gap |
|---|---|---|
| GPU time per frame / pass | GEKKO_GPU_TRACE (timestamp queries; shipping too) | - |
| displayed frames, repeats, vblank | GEKKO_PRESENT_TRACE + DXGI frame statistics; present_trace_report.py, repeat_attribution.py | compiled out of shipping builds |
| GPU clocks, power, utilization, P-state | tools/gpu_clock_sampler.py (NVML, separate process; NVIDIA only) | AMD / Intel: none |
| CPU package power | none | not available on this machine: no energy-meter device, no power counter set (i9-13900K, Windows 11); reported as unavailable, never estimated |
| per-thread CPU time / wakeups | GEKKO_SELF_SAMPLE, GEKKO_TASK_TIMES (diag) | no per-thread CPU time or context-switch counts in one report |
| game-thread work vs sleep | pacer counters, GEKKO_FRAME_WORK | not joined to the present trace |
| queue wait, submission overhead | present trace WAIT/PRESENT events, gpu.csv cpu_begin / submit | no per-frame report joining them |

## 5. Next (in the directive's order)

1. Telemetry: one per-frame record joining game-thread work / sleep, packet push / pop, swap-chain wait, submission,
   GPU begin / end, Present, displayed refresh; per-thread CPU time (QueryThreadCycleTime / GetThreadTimes) and
   wake counts; the NVML sampler joined when present; unavailable fields stay empty. Usable in a measurement build of
   the user's configuration, not only the diag tree.
2. Baseline: Westopolis and Final Haunt at 1920x1080, 2560x1440, 3840x2160 fullscreen, driver clocks, several cold and
   warm runs, raw logs kept.
3. Work: findings 1-4 above, each A/B'd with audio PCM, gameplay state and captures identical.
4. Controller (disabled by default) only after 1-3 can measure its effect.

## 6. Finding 3 measured and changed: credit first, then the newest packet (2026-10-31, session 2)

Measured (telemetry baseline, diag exe 942dae5f..., Westopolis tape, 1920x1080 fullscreen, driver clocks), event times
relative to the vblank at which the frame was displayed, p50 (p1 / p99):

| event | wst-1920x1080-warm1 |
|---|---|
| input latch | -37.68 (-41.30 / -36.88) ms |
| packet pushed | -34.05 (-34.64 / -33.46) |
| packet popped = swap-chain wait begins | -32.64 |
| waitable signalled (vblank before display) | -16.60 |
| GPU submit / begin / end | -16.18 / -15.55 / -10.18 |

The render thread popped packet k right after presenting k-1, then held it ~16 ms waiting for the frame-latency
waitable; packet k+1 was pushed 0.7 ms BEFORE that waitable signalled and sat in the queue while k was recorded.
The hold depends on where the game's push falls relative to vblank, which is set by the boot (game clock and display
are independent): push -> recording begins = 17.4 ms (warm1), 11.9 ms (cold1), 15.2 ms (smoke2). In any phase where
another push lands between the pop and the waitable, the old order shows a frame one refresh older than it could.

Change (runtime/src/gekko/lifted_render.cpp present_newest, render/packet_queue.h wait_for_packet,
render/renderer.cpp acquire_frame): wait until a packet exists (not taking it), take the swap chain's credit
(Renderer::acquire_frame, held until the next render), then pop the newest packet. GPU start time and GPU work are
the same (the credit decides the start in both orders); the recorded packet is never older than before.
SHADOW_R2_CREDIT_FIRST=0 restores the old order for A/B (both shipping and diag). Expected: push -> recording begins
bounded by one refresh instead of up to ~1 refresh + 0.7 ms; on warm1's phase about -16.7 ms latch -> display.
Built and A/B'd 2026-10-31 10:41-11:19 - see 6.2.
Report: tools/frame_telemetry_report.py attaches a WaitBegin / WaitEnd that precedes a Pop to that Pop, and the
latency stages are now order-independent: "held before recording (push -> later of pop and waitable)" and "render
thread CPU (recording begins -> GPU submit)" (re-run on warm1's old trace: same totals, 17.46 ms held p50).

Next on this line (not started): the hold that remains is the push phase relative to vblank (0-16.7 ms, random per
boot). Bringing it to a small fixed margin needs the game clock's phase steered to the display's vblank - a game-time
change that must be A/B'd for audio (AX passes follow game time) and gameplay (tape identity), so it is a separate step.

### 6.1 1080p repeats and clocks (baseline, before the change)

wst-1920x1080-cold1 / warm1: 4 / 5 repeats in 90 s. All but one are boot frames (ticks < 30, GPU < 0.2 ms, the
packet pushed after its deadline while the stage loads; one is the 588 ms load). The one real one (warm1 tick 1185,
gpu_queued, late 0.33 ms): GPU 15.97 ms with the same draws (409) / particles (2026) as its neighbours; only the
`scene` pass swings 7.2 <-> 14.6 ms from frame to frame. NVML: the card spends 8354 of ~8890 samples in P8 (core
210-315 MHz, memory 405 MHz), 290 in P5, 243 in P0; board power ~37 W. The driver runs this load at its lowest state
that (nearly always) fits; the frame-to-frame swing is that clock's ramp (NVML's 100 ms sample cannot resolve it per
frame). Normal driver power management, the engine's work unchanged - only less work (GPU_FILL smoke track) moves it.

### 6.2 A/B result (2026-10-31 10:41-11:19): accepted as the default

Runner .build/validation/telemetry-2026-10-31/ab_credit.ps1 (bench_ab.ps1): one diag exe (cc519c95 rebuild of the
same source; first build 8b341a69), both tapes (Westopolis, Final Haunt), 1920x1080 / 2560x1440 / 3840x2160
fullscreen, driver clocks, arms alternated new, old, new, old; 12 runs per arm, all ended at the 90 s budget
(stop reason 0). An earlier partial A/B was stopped at the user's request and is kept in aborted-ab-1 (not used).

| per arm (12 runs) | old order (CREDIT_FIRST=0) | credit first (default) |
|---|---|---|
| held before recording p50, mean over runs | 16.47 ms | 9.75 ms |
| held before recording p99, mean / worst run | 25.17 / 30.72 ms | 10.36 / 16.17 ms |
| latch -> displayed vblank p50, mean | 53.33 ms | 46.60 ms |
| latch -> displayed vblank p99, mean / worst run | 63.53 / 67.90 ms | 50.25 / 56.03 ms |
| latch -> displayed vblank max, mean / worst | 77.75 / 101.20 ms | 59.88 / 72.27 ms |
| presents / repeats / stale refreshes / dropped on pop | 64,221 / 68 / 132 / 93 | 64,222 / 67 / 129 / 86 |
| render CPU p50, GPU ms p50, board power | 0.27 ms, 5.67 ms, 44.56 W | 0.30 ms, 5.69 ms, 44.25 W |

- Identity: audio.pcm and the --state-report are byte-identical in all 12 runs of each route, both arms
  (Westopolis 015445f2 / a186ece3, Final Haunt 231a3aa5 / 56ce185b, SHA-256 prefixes). Presentation-only, as designed.
- The hold is now bounded by one refresh in every run (worst p99 16.17 ms; old order up to 30.72 ms). The p50 still
  depends on the boot's phase (0.9-15.6 ms new, 7.6-27.4 ms old): that remainder is the next step (phase steering).
- Repeats and stale refreshes are unchanged (display truth). The displayed-fps LOWER BOUND reads 57.50 vs 58.21
  only because more consecutive displays went unresolved in DXGI's statistics (2,121 vs 1,353 of ~64k): the stats are
  read once per present and the new order reads them at a different point of the refresh - a trace limitation, not a
  difference on screen (presents and repeats are equal).
- GPU time and board power unchanged within noise (no work changed).

### 6.3 Machine-state change during the session: every run after 10:29 is composed (+1 refresh)

All runs before 10:28 (baseline batch, both routes, all resolutions) show presentation delay 0 refreshes (GPU end ->
displayed vblank p50 ~10-12 ms); every run from 10:41 on shows delay 1 (~25-28 ms), BOTH A/B arms, and also an exe
rebuilt with the change reverted (probe-revert, 10:5x). So it is the machine, not the code. Same window mode
(borderless-fullscreen 1920x1080 window, 60 Hz) in both. Ruled out by probes: the respawned NVIDIA Overlay's visible
full-screen layered window (hidden with ShowWindow: still delay 1), the NVIDIA Overlay processes (suspended: still 1),
cua-driver (suspended: still 1), NVENC capture (0 sessions), third-party DLLs in the game (none; only NVIDIA's
nvspcap64 / driver). Between 10:27 and 10:41: the user's app cleanup (10:28) and LogonUI.exe started at 10:29:28 and
is still running (lock screen). IDENTIFIED 11:5x: the session is LOCKED (WTSQuerySessionInformation WTSSessionInfoEx: SessionFlags = 0 =
WTS_SESSIONSTATE_LOCK; the user locked the PC when leaving, LogonUI 10:29:28). Under the lock screen DWM composes every
frame (+1 refresh). Not a player-facing state; runs made while locked are labelled locked and are valid only as
same-state A/B. Check before every benchmark: scratchpad tool lockstate.ps1 logic (now in bench_ab.ps1's run.json). Absolute latencies after
10:29 are therefore one refresh higher than the morning baseline; the A/B compares arms under the same state.

## 7. Clock drift: game, display, audio device (2026-10-31)

New telemetry: AudioClock rows (audio_runtime.cpp render_endpoint, diag builds, ~1 Hz: IAudioClock position +
frequency + QPC time, and the game -> device ring fill) and tools/clock_drift_report.py (least-squares rates against
QPC). 20-minute run .build/validation/telemetry-2026-10-31/drift-wst-20min-1 (Westopolis tape then idle in the stage,
1920x1080, session locked = composed; the rates do not depend on that):

| clock | rate vs QPC | note |
|---|---|---|
| game clock (pushes) | -0.00 ppm | 70,199 pushes / 1,170 s - the pacer locks game time to QPC, as designed |
| display (DXGI vblanks) | +8.93 ppm | 60.000536 Hz, residual rms 18.5 us |
| audio device (IAudioClock) | -1.06 ppm | 1,159 samples, position jitter rms 3.1 ms (WASAPI updates per period) |
| ring fill | +0.3 ms per hour | 760-1,080 frames the whole run; 0 underruns, 0 drops |

A 30 s probe had read the device at +11.5 ppm - noise from the 3 ms position jitter over a short span; the 20-minute
fit refutes it. Today's audio path does not drift: a game clock on QPC and a device within ~1 ppm of QPC.

Consequences for latency:
- The game's frame phase against the vblank slides by 8.93 ppm: a whole refresh every ~31 minutes. Where it falls
  decides the hold before recording (0-16.7 ms, section 6). A one-time alignment at boot would only help for minutes.
- Removing the hold for good needs the game clock to follow the display (+8.93 ppm, phase-locked to the swap chain's
  credit) AND the device side to absorb the ~10 ppm surplus: the ring would otherwise grow ~36 ms per hour. The
  resampler (rational phase step up_/down_ = 441/320 at 32 -> 44.1 kHz, interpolated kernel rows) can take a fine
  ratio trim: scaling up_/down_ by 2^20 is arithmetically identical (exact doubles below 2^53) and gives a
  ~0.003 ppm step. Game-side PCM (--audio-dump) would stay byte-identical; only the device-side resampling moves by
  ~10 ppm (0.02 cents of pitch). Both parts behind switches, default off, until A/B'd.

## 8. Phase lock + audio rate trim (2026-10-31, in source, default OFF)

- SHADOW_PHASE_LOCK=1 (runtime/include/gekko/phase_lock.h; packet stamp in render/packet_queue.h FramePacket::pushed_ns;
  hold measured in lifted_render.cpp present_newest after the credit; pacer step in shadow_main.cpp pace() via
  FixedSimulationClock::shift_anchor): median of the last 31 holds (push -> recording begins), anchor moved by
  0.001 x (hold - 2 ms) per 1/300 s pacing tick, at most 1000 ppm of wall time. Game time and its steps are untouched.
- SHADOW_AUDIO_RATE_TRIM=1 (audio_runtime.cpp RateTrim / AudioResampler fine ratio): resampler ratio trimmed
  0.5 ppm per frame of (smoothed fill - target), +-300 ppm. Off: the resampler is configured as before.
- Trace rows AudioTrim (19) / PhaseLock (20); tools/clock_drift_report.py summarises both.

A/B 12:18-12:51 (1920x1080, credit-first both arms, session locked = composed in both arms; ab_lock.ps1):

| run | hold p50 / p99 | latch -> vblank p50 / p99 | repeats / stale |
|---|---|---|---|
| wst off-1 / off-2 | 1.40 / 2.02, 8.53 / 9.13 | 38.22 / 41.88, 45.40 / 49.04 | 3/5, 3/5 |
| wst on-1 / on-2 | 2.01 / 6.11, 1.95 / 2.34 | 38.82 / 44.41, 38.74 / 42.20 | 4/6, 4/5 |
| fh off-1 / off-2 | 7.19 / 7.82, 14.02 / 14.65 | 44.07 / 47.69, 50.90 / 54.55 | 3/5, 5/6 |
| fh on-1 / on-2 | 2.05 / 13.89, 2.03 / 9.37 | 38.84 / 52.24, 38.81 / 47.74 | 5/6, 3/5 |

- PCM (--audio-dump) and --state-report byte-identical on vs off, both routes (015445f2 / a186ece3, 231a3aa5 /
  56ce185b), and identical to the earlier credit-first A/B: gameplay and game-side audio do not see the lock.
- The hold settles at ~2.0 ms in every lock-on run (filtered hold after 30 s: 1.76-2.13 ms); latency p50 38.8 ms in
  every lock-on run instead of 38.2-50.9 ms depending on the boot. The lock-on p99s include the first seconds while
  it converges (up to 14.7 ms of shift at <= 1 ms/s).
- 20 minutes lock-on: game clock +8.96 ppm = display +8.94 ppm (locked); 0 underruns / drops.
- DEFECT found and fixed: the trim learned its fill target over the first 5 s after the start, when the ring still
  holds the prime (target 1,348 frames vs ~1,000 settled), and so held the ring ~11 ms fuller for the whole run (a
  latency change the design forbids). Now: 20 s settle, then 10 s averaged. Re-run (ab_lock2.ps1) pending; the first
  20-minute run is kept in superseded-trim1/.
