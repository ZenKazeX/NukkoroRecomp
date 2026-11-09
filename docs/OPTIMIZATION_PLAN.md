# CPU / RAM / GPU optimization plan (high level)

Written 2026-10-26 after increments 24 (snapshot regions) and 25 (sound phase 1). It orders the work; each item still
goes through the directive's loop: measure the owner, record the reference behaviour, change one thing, prove
equivalence (byte-exact where the output is exact; captures, PCM, state attribution for gameplay; the Dolphin
reference when the question is "what does the original do"), measure paired and alternating with all runs kept, keep
it only on evidence. North star applies throughout: the gains come from replacing console mechanisms with native
code, never from wrapping them. Details and measured numbers: OPTIMIZATION_RESULTS.md; handoff: NEXT_SESSION.md.

## Goals

| Resource | What the player should get | How it is judged |
| --- | --- | --- |
| Smoothness (GPU + present) | No repeated or dropped frames in normal play at 1440p render | present trace: repeats/drops per minute by cause (not CPU-side present intervals) |
| CPU | Headroom on the game thread in the heaviest frames; fewer, cheaper threads | per-quantum guest work (W), per-thread cycles, inclusive attribution |
| RAM | Lower private commit and peak at stage loads, without disk I/O stalls | private commit / peak, memory_map_sampler attribution |
| VRAM | Lower footprint (it is also process commit on this driver) | D3D12 usage per named allocation (GEKKO_GPU_MEMORY) |
| Power | Lower energy for the same frames, measured where the machine allows | last; only measured quantities |

## Where we stand (measured)

- GPU (1440p, 4x RGBA16F, RTX 4080): frame p50 ~4.4 ms, p99 ~10.7 ms after increment 24 (was 14.7-14.9). Remaining
  over-budget frames are motion-trail moments: scene 7 ms + a second scene pass 3.4 ms + view resolve 3.2 ms + history
  resolve 1.8 ms. Full-view resolves run far below the GPU's bandwidth; GPU clock state under a light 60 Hz load is the
  suspect. Display: 7 repeats and 2-5 drops per 90 s on the Westopolis replay (was 18-20 / 42-51).
- CPU: the game thread does 2-15 ms of guest work per 60 Hz quantum and keeps up (2 behind boundaries in 12,985).
  Generated game code is ~60% of its samples with no function above 3%; paired-single/compare/indirect-call helpers
  ~5%; AX ~0.5% of a core. Process CPU varies +-15% run to run, so claims stay at timer/cycle level.
- RAM: private ~498 MiB steady at 1440p, of which ~360 MiB mirrors D3D12 default heaps; guest memory ~25 MB; 16 MiB
  card image; 16 MiB sample memory (ARAM stand-in, 6-10 MiB written); +55 MiB unattributed peak at stage load.
- VRAM (1440p): view colour 116 MiB, view depth 60, glare sources 60 (recreated when glare pauses > 3 frames),
  history 28.8, output 15, view 30, assets + meshes ~21.

## Phase A - done 2026-10-26 (OPTIMIZATION_RESULTS.md, "Phase A of the optimization plan")

What it settled, and what that changes below:
- A1: the GPU runs in P5/P8 at 570-780 MHz (p50) under this ~28% load; every over-budget frame landed at 300-690 MHz.
  B1 (trail resolves) and B2 (how load reaches the driver) are confirmed as the spike work; clock state is half the
  story, so B2 is now measured, not hypothetical.
- A2: per-thread cycles are in every session report; the game thread is the only busy thread headless (0.5% spread);
  seven threads never run (C4).
- A3: game-thread ranking by class: Shadow's executor 10%, the Sonic NPC 10%, effects ~11%, collision 4-7%, a shared
  enemy/object base update ~7%, 42% not yet named (RE work). C1's next native subsystem candidates come from this.
- A4: per-pass table on three routes; Final Haunt's glare composite (1.1 ms/frame) is a new B item. A one-off skipped
  draw (lh-1) needs per-reason skip counters (new B item, correctness).
- A5: the "+55 MiB stage-load peak" is glare-target churn (57.5 MiB stays committed ~46 s after a recreation), so B3
  now covers RAM too and D2 is dropped.

### Phase A - original list (kept for reference)

1. GPU clock/power state during a run (NVML or PDH if available without a driver; else report unavailable). Decides
   whether the remaining spikes are work or clocks.
2. Per-thread cycles (QueryThreadCycleTime) in the session report; identical-binary spread before any CPU claim.
3. Inclusive CPU attribution: stack sampling of the game thread (RtlVirtualUnwind with the image's unwind data) so the
   flat 60% of generated code is ranked by owning game subsystem.
4. Per-pass GPU table on three routes (Westopolis, Lethal Highway, Final Haunt) from the GPU trace, with spread.
5. Stage-load RAM peak attribution (memory_map_sampler during a load).

## Phase B - GPU and presentation (largest visible effect)

1. DONE (increment 30): history saves resolve straight into the history (0.090 -> 0.049 ms per save). Was: trail-moment resolves: resolve the view once and copy into the history only when needed (or render the history
   pass from the already-resolved view); check the history save's extra CopyResource. Exact output required.
2. DONE (increment 31): the periodic slowdown was not the GPU clock but the console's 59.94 Hz retrace beating against
   the game's 60 Hz frames (~200 judder frames every 1,001). One time base: frames, retrace and audio passes on
   exactly 1/60 s ticks, and the loading screen's console-field draw throttle answered natively (FRAME_PACING.md).
   Was: if Phase A shows clock ramp-down, schedule work so the GPU is not idle-throttled in bursts.
3. DONE (increment 27): pooled targets kept 60 s instead of 3 frames - pixel-exact, peak commit 566.8 -> 516.9 MiB,
   the 5.4 ms recreation at the glare's return gone. Was: glare chain churn. Phase A: a pause of 11 frames on
   Westopolis releases and recreates the 60 MiB target and leaves 57.5 MiB of process commit for ~46 s.
3a. DONE (increment 30): the glare composite is folded into the view's resolve when only 2D follows it (4x+): composite +
   resolve 3.80 -> 2.49 ms on Final Haunt; ~11 px/frame differ by 1 LSB where the GPU's own MSAA blend collapses edge
   samples (accepted by the user).
3b. DONE (increment 27, counters; the one-off skip did not recur in 4 more runs). Skipped draws: count each of the renderer's eight skip reasons in the session report and find the one-off skip
   seen in one Lethal Highway run (correctness first).
4. Load-time hitches: pipeline-state and texture creation off the render thread / ahead of use; upload ring waits.
5. Presentation: the render loop pops a packet, then waits for the swap chain; review order and frame latency for
   the 2-3 remaining packet/render-late repeats. Optional later: VRR/tearing-allowed presentation for capable displays
   (user decision: latency vs smoothness).
6. Quality-for-cost options, user decisions only: scene format RGBA16F -> R11G11B10F, MSAA 4x -> 2x, hardware resolve
   (not bit-identical across processes).

## Phase C - CPU (game thread first)

1. Native rewrites of the top subsystems from Phase A's ranking, each with original-code oracles (as increments
   22-25). Planned already: the sound path phases 2-4 (docs/GCAX_SOUND_MAP.md) - native SE manager, native voices,
   native bank store - which remove GCAX's per-frame MIDI drain, voice updates and AX parameter-block traffic.
2. Hot helpers: psq_load/psq_store specialised by quantisation type, fp_compare and call_indirect at their generated
   call sites; kernel-level measurement, then gameplay.
3. Heaviest guest frames (12-15 ms of work): attribute what makes them heavy (Phase A) and cut that owner; this is
   the headroom that protects frame pacing.
4. Threads: job-pool workers that never get work, the render thread's 8 ms polling wait, audio thread wakes - count
   context switches, size pools to the work.
5. Runtime ISA dispatch for the few measured kernels (mixer gains, reverb, matrix math): AVX2 with SSE2 fallback behind
   an architecture-neutral interface (future ARM64/NEON). The image currently requires AVX2 at startup.

## Phase D - RAM and VRAM

1. VRAM first (it is also private commit here): every render target justified by the per-pass table; transients
   aliased by lifetime (increment 21 did the first round); glare and history sizes by need.
2. (Dropped after Phase A: the "+55 MiB stage-load peak" is the glare churn of B3, not a stage load.)
3. ARAM stand-in (16 MiB): retired by sound phase 4 - banks held natively at their real size, no fixed 8 MiB
   GCAX block and slot sizes.
4. Card image (16 MiB): demand-committed or file-backed only if saves stay byte-identical (never touch the user's
   cards).
5. Guest heap (legacy-os-heap, 18 MiB committed): shrinks as subsystems move to native ownership (memory programme).

## Phase E - power and thermal (last)

Measured only: package/GPU power where the machine exposes it, frequency, per-thread cycles. Report what cannot be
measured instead of using proxies.

## Order and decision points

A (measured, done) -> B3b, B3 (done, increment 27) -> B1, B2, B3a, B4 (GPU smoothness) -> C1 (sound phase 2, then the top-ranked subsystem) -> D1-D3 -> C2-C5 -> E.
User decisions expected: quality trade-offs in B6, VRR/latency in B5, any deviation from original behaviour (as with
the WARPHOLE fix), and every deployment of a candidate to game/ShadowTheHedgehog.exe.
