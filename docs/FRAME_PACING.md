# Frame pacing contract

The PC runtime has no positive-interval half-rate waiting policy. A normal
frame waits for one game-clock grid boundary, regardless of the console
interval request. Zero remains the reset/disc-error no-wait mode. Explicit
loading bypasses still advance the next event if no game time has passed,
so asynchronous loading cannot spin forever without progressing.

## Original-code evidence

PAL main.dol SHA-256:
fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
The original limiter is 0x801E1F2C (740 bytes). Its interval load at
0x801E2118 belongs to the multi-period console wait. EventManager construction
sets the interval to two at 0x80173F84; destruction restores the saved value
at 0x80173E64. Reset clears it at 0x8032D2F4. These are not evidence that
object count dynamically chooses a rate in the present native renderer.

The original limiter body is already excluded by extract/native-owners.txt.
The preceding build already reduced positive requests to one period. The
2026-10-24 cleanup removes the leftover interval multiplication, generic
multi-period deadline expression, unreachable long-wait error and standalone
clamp helper. It does not newly turn a measured 30 FPS run into a 60 FPS run.
This remains a wait-point adapter to the translated game's timing fields;
it is not a completed native game-clock migration.

## One time base (B2, 2026-10-27)

The frame limiter, the video retrace and the audio passes share one clock: the
game's time base (40.5 MHz), with frame k of an hz game beginning at tick
k * 40,500,000 / hz - exactly 675,000 ticks (1/60 s) or 810,000 (1/50 s).

- NativeFrameLimiter waits for the start of the next frame on that grid. The
  game still reads its own period P (16,666 us at 60 Hz): elapsed is P times
  the frames begun since its last sample, so dt, the setter's step count and
  the cap are unchanged. The console's microsecond grid (multiples of P) was
  0.04% short of 1/60 s.
- NativeVideo makes the 60 Hz field exactly one frame (675,000 ticks, its 525
  half-lines evenly spread) instead of the console's 675,675 (59.94 Hz), and a
  frame-locked field begins on the same grid. The 59.94 Hz retrace beat against
  the 60 Hz frames once every 1,001 frames: for ~200 frames every third frame
  waited a whole extra field.
- NativeAudio starts a DMA's first pass so that it ends on a whole multiple of
  the pass length (5 ms, 202,500 ticks, for the game's 640-byte 32 kHz buffer);
  reloads follow without a gap. A pass end meets a frame boundary every 50 ms.
- The loading screen's draw throttle (fn_802BE9D4) skipped while fewer than
  16,683 us (`li r0,0x412B`, the console's field) had passed since its last
  draw. That constant is a platform value site (extract/platform-sites.txt,
  `display_field_us`): NativeVideo answers the current field in whole
  microseconds, the same floor of the same field - 16,666 us at 60 Hz.

Measured on the Westopolis replay: gameplay frames are all 675,000 ticks; with
the console constant the stage loading screen drew every fourth audio pass
(20 ms) instead of every frame.

## Pacing on the frame grid (2026-10-29)

**Defect.** With vsync off, frames reached the screen in a repeating 33 / 2 / 15 ms rhythm. That averages 60 fps,
but a third of all intervals were over 20 ms, and the user saw it as "slow as hell". Vsync hid it: the swap chain
spread the frames over even refreshes.

**Measurement.** Westopolis tape, vsync off, GEKKO_PRESENT_TRACE + tools/present_trace_report.py:
- the game thread itself handed packets over unevenly (push interval p50 14.4 ms, p95 33.4 ms, min 0.004 ms);
- 841 of the 843 repeated refreshes were `packet_late`;
- every packet was exactly 675,000 game ticks after the previous one, and stamped exactly on a frame boundary.

**Cause.** Real-time pacing ran only inside the host service, a game-clock event every 5 ms (202,500 ticks). The
pacer's 60 Hz boundary was therefore seen 0, 1.67 or 3.33 ms of game time after it, in a cycle that repeats every
50 ms. The frame begins at the boundary itself (the retrace on the same tick), so it was built either before the
sleep (its packet a whole frame early in wall time) or after it, three frames per 50 ms.

**Fix.**
- GameEvent::Frame is also delivered at every pacing tick, `System::kPacingTicks` = 1/300 s (135,000 ticks). That
  divides the 1/60 s and the 1/50 s frame, so every frame boundary of either game rate is one.
- A tick that is not a host-service tick runs only the pacing hook. The 5 ms host service is unchanged: its ticks,
  input latching, stop and savestate boundaries. So input tapes and savestates are unchanged; no event was added.
- The pacer runs at 300 Hz (`FixedSimulationClock::Config::pacing_hz`).
- Its reset anchors to the absolute grid, so a re-anchor (startup, load, pause) cannot move the boundaries off the
  frames.
- On a frame boundary the clock delivers the service (event index 0) before the retrace, so every frame is built
  after its boundary's sleep.
- The pacer's telemetry (quantum histogram, frame tail, audit lane) stays per 1/60 s.

**Result.** Same tape, vsync off:

| | push interval p50 / p95 / p99 | intervals > 20 ms | repeated refreshes |
|---|---|---|---|
| before | 14.4 / 33.4 / 33.5 ms | 1,184 | 843 |
| after | 16.66 / 17.1 / 17.3 ms | 14 | 18 |

With vsync on: p50 16.7, p95 17.1 ms.

**The remaining long intervals** (fixed build, 60 s of the tape):
- 12 are the boot's 50 Hz frames (20 ms of game time each: correct).
- The 81 ms and 610 ms intervals are the stage load. Their game time is the same (80 / 607 ms): the game draws
  nothing then, and wall time tracks game time exactly.
- One is a real host stall at the stage start: game time 1.27 s, a 38 ms interval for 16.7 ms of game time.
  `[host-stall]`: 23-27 ms of native execution for 5 ms of game time, last file `character/doomseye.one`, 0.85 ms of
  it file I/O.
- None after it in 60 s.

**Attribution.** tools/native_sample.py on the game thread, four runs of the release exe, 63 samples in the stall
stretches:
- 46% `PrsEngine::run` (game_memory.cpp, the native PRS expand 0x80043FE0), ~11 ms. It is a register-level
  transliteration: per byte it models r0-r10, CR0, CA, a cursor lookup and a dispatch poll;
- the rest is spread over the game's own stage-setup code (no function above 5%).

Native code advances no game time, so the expansion's whole host cost lands inside one 5 ms window. The fix is a
lean, byte-exact PRS decoder (docs/NATIVE_MATH_METHOD.md):
- pending interrupt lines are game-thread state, and game time is frozen during the call, so mid-call only a host
  stop request can change needs_dispatch();
- an interrupt pending at entry keeps the original's first hand-back.

**The PRS decoder (2026-10-29).** `PrsEngine` (game_memory.cpp) keeps the original's state machine and register file.
- As locals, not object members.
- It polls for a hand-back only where one can be due: an interrupt or stop at entry, after a cold accessor access,
  and a stop request every 4,096 tokens.
- Whole tokens run on raw host pointers (`prs_raw`) while the stream and the output stay inside their committed
  regions:
  - a match is one memcpy between disjoint runs, or a forward byte loop when it overlaps itself;
  - it hands the exact register state back to the token loop at a region edge or at a match source outside the
    output's region.

Evidence:
- **game_memory_owner_tests: PASS.**
  - Register-exact against the original words in the interpreter: 138 synthetic runs, 46 of them with an interrupt
    held up (1,108,400 hand-backs and resumes), and 513 disc members (8.4 MB, 68 with hand-backs).
  - All 15,109 PRS members of the disc's 1,244 ONE archives (330,178,916 bytes) equal the host codec.
  - The owner's calls take 0.83-0.91 s against 1.86-1.91 s for the state-in-locals version alone, which measured no
    faster than the old engine (whole-test totals 4.3 / 4.4 s against 4.2 / 4.4 s).
- **Journal on the Westopolis tape** (headless, 40 s, SHADOW_GAME_MEMORY_JOURNAL; release 09:22 against diag 11:52):
  - the same 605 expansions, with identical callers, sources, counts and output hashes;
  - the journals are identical up to line 54,948. There the newer build makes 0xC0-byte memcpy calls from 0x803CB254
    (fn_803CB178, sound streaming area) that the older one does not, and three later expansions land 0xC0 away in
    the heap with the same bytes. Attributed to a non-PRS change in that build, pending confirmation from the
    session that owns it.
- **The stage-start frame** (traced, vsync off): 38.3 ms before (pacing fix in), 30.3 / 30.2 / 29.5 ms after. The
  rest of that frame is the game's own stage-setup code. Still over its 16.7 ms slot: open.

**Regression found and fixed on the way.** The pacing-only ticks moved `last_service_end`, which shortened the
`[host-stall]` window, so the 38 ms frame went unreported, and dropped guest time from `execute_ns`. Now they leave
it alone and their sleep is subtracted (`paced_between_ns`). Tests: gekko_core_tests (the pacing ticks, the unchanged service ticks, the 1/60 s
boundary is a pacing tick) and host_timing_tests (the absolute-grid anchor).

## V-Sync off (2026-11-06)

**Report.** With V-Sync off the FPS counter's worst frame time read ~17-18 ms, worse than with V-Sync on.

**What decides a frame with V-Sync off.** The game's own 60 Hz frame (logic is not decoupled from presentation; there
are no extra unique frames to show). The frame is finished before its boundary and pushed right after the pacer's
boundary wake; the render thread takes it at once (push -> pop 0.01 ms), presents with `Present(0,
DXGI_PRESENT_ALLOW_TEARING)` on a FLIP_DISCARD swap chain created with ALLOW_TEARING, and the frame-latency credit
(max latency 1) is already there. So a frame's release time is the pacer's wake. The FPS counter
(effect_layer_overlay.cpp) times consecutive frames the render thread takes: CPU-side, per present, not the display.

**Two defects, both measured** (Westopolis tape `westopolis-cpu-spike`, stg0100, 1920x1080 borderless, 60 Hz display,
75 s, first 15 s left out; .build/vsync-off/, scripts analyze.py, jitter.py, phase.py, cpu.py there):
1. **The phase lock ran with no vblank to lock to.** Its hold (push -> credit) is ~0 with immediate presents, against a
   2 ms target, so the controller slewed the game clock earlier at its 1000 ppm limit for ever: -73 ms in 60 s, the
   game at 60.06 fps. The audio trim (capped at 300 ppm) could not follow: the device ring grew from 2,379 to 3,404
   source frames (+32 ms of audio latency per minute, unbounded), and the present phase swept through the refresh
   every 16.7 s (a rolling tear).
2. **The boundary wake's timer lateness was the frame's release jitter.** The high-resolution waitable timer wakes
   0.29 / 0.70 / 1.26 ms (p50 / p99 / max) after its deadline, by a different amount each time; push jitter (sd 0.236
   ms) is that lateness (sd of its change 0.227 ms; the build after the wake is 0.05 ms). One interval is 16.667 ms +
   this wake's lateness - the previous one's: the counter's 17-18 ms. With V-Sync on the swap chain's credit re-times
   the frame to the vblank, which hides it.

**Fix.**
- `phase_lock::display_paced()` (gekko/phase_lock.h), set by LiftedRender::start / set_vsync. The render thread samples
  holds and the game thread steers only while V-Sync is on. A V-Sync change clears the published hold and target and
  resets the filters (no stale samples).
- `WakeGuard` (gekko/host_timing.h) and `RecoveredClockSleeper::sleep_until_precise` (shadow_main.cpp): with V-Sync off,
  the frame-boundary sleep arms the timer `guard` early (the worst of the last 64 wakes' lateness + 0.1 ms, 0.25-2 ms)
  and yield-spins to the deadline. Other pacing ticks and every V-Sync-on sleep are unchanged. Cost: the game thread
  15.2 -> 18.8 % of a core with V-Sync off; V-Sync on 15.0 -> 14.6 % (no change).
- Nothing caps, smooths or hides frames; the FPS counter is unchanged.

**Result** (same tape; overlay = render-thread frame time, the FPS counter's measure; latch -> screen = input latch to
max(Present returned, GPU end), the tearing flip; latch -> vblank = to the refresh DXGI reports, for V-Sync on):

| | fps | frame p50 / p99 / max ms | sd ms | 1 % low fps | overlay worst / s p50 / max ms | > 17 ms | latch -> screen p50 / p99 | latch -> vblank p50 / p99 | clock drift | audio ring |
|---|---|---|---|---|---|---|---|---|---|---|
| on, before | 60.00 | 16.67 / 16.82 / 17.06 | 0.059 | 59.25 | 16.81 / 17.06 | 1 | 6.6 / 10.7 | 22.1 / 25.6 | -9 ppm | 923 -> 937 |
| on, after | 60.00 | 16.67 / 16.82 / 16.99 | 0.059 | 59.34 | 16.82 / 16.99 | 0 | 6.6 / 10.6 | 22.1 / 25.6 | -9 ppm | 964 -> 977 |
| off, before | 60.06 | 16.67 / 17.17 / 17.70 | 0.240 | 57.95 | 17.15 / 17.70 | 228 | 4.3 / 8.0 | - | -1001 ppm | 2,379 -> 3,404 |
| off, after | 60.00 | 16.67 / 16.79 / 17.39 | 0.048 | 59.29 | 16.78 / 17.39 | 4 | 3.9 / 7.6 | - | 0 (lock idle) | 1,463 -> 1,465 |

0 audio underruns in all four; the same 2,381,920 source frames consumed (the tape has no checkpoints: version 2).

**What V-Sync off cannot do on a 60 Hz display.** Show more unique frames than the game's 60: that would need rendering
interpolated game state, which this renderer does not have (the plume path's frame_interpolation is a cross-fade, not
used by the lifted renderer). Its gain is ~18 ms less latch -> screen latency, at the cost of a tear.

**Open (measured, not changed).** With the drift gone the present phase stays where the boot put it (one run: 15.6 ms
after the vblank, moving ~11 ppm - the display against QPC). DXGI's Stats then count 321 "repeated refreshes" in that
run (21 before): presents within ~1 ms of a vblank attributed to the neighbouring refresh. With an independent flip that
is a tear in the top or bottom rows; under DWM composition it would be a real one-refresh hold. The presentation mode
could not be read (IDXGISwapChainMedia::GetFrameStatisticsMedia unavailable; PresentMon not installed), so which one it is
was not verified. Placing the flip inside the vertical blank (~0.67 ms at 1080p60) is not reliable with GPU-end jitter
of several ms, so no phase target was added.

## Timing that remains

The existing period calculation, elapsed/raw dt, authored
speed override and cap, movie wait, event delivery, FPU continuation, setter
arguments and counter tail call retain their semantics. Host wall-time pacing
still uses fixed 60 Hz quanta and never changes its rate based on scene load.
This does not certify fixed timesteps in every translated physics subsystem.
The game still consumes its existing elapsed-time fields.

The lifted renderer presents each newest completed game packet; it does not
select a two-refresh swap interval (VSync is one refresh or immediate).
Real CPU/GPU delays and missed display refreshes can still cause longer frame
intervals. Removing a rate policy cannot remove that work. Fresh rendering is
still tied to game packet production, so 800 FPS at 1440p remains unmet.

## Validation

The installed NativeFrameLimiter owner is exercised for 9,600 frames across
50/60 Hz game modes, interval requests 1/2/60/UINT32_MAX and uninterrupted or
interrupted waits. Tests check actual deadlines, one-period dt and setter
arguments, counter handoff and caller-stack restoration. Separate cases cover
zero mode, explicit loading bypass and reset cancelling a pending wait.
Existing host_timing_tests check fixed pacing independently of presentation.
Current build/gameplay evidence lives in .build/validation/frame-pacing/.

The user-controlled Westopolis recording is retained in
.build/recordings/westopolis-cpu-spike/. It contains 315.385 game seconds,
972 suppressed multi-period requests, 18,800 completed presentations, a
16.7 ms median interval and two intervals of at least 34 ms (maximum 603.8 ms).
Those values describe the recording build, not this cleanup's performance.
The one-second CSV renderer fields are invalid for that lifted build; the
presentation observations above come from its actual renderer stdout report.
No cooler-temperature marker was supplied, so neither long interval has been
attributed to temperature. No claim of a thermal fix is made.
