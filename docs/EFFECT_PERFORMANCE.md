# Effect performance audit (2026-11-02, in progress)

User directive 2026-11-02: audit every visual effect's CPU / GPU cost against the work its image needs, motion blur
first; exact output preserved (no disabling, no sample-count cuts, no approximations); diagnostic disable switches only
for cost isolation; measured at 1920x1080, 2560x1440, 3840x2160 with 4x MSAA on the Westopolis and Final Haunt tapes.

## 1. Motion blur: what it is (original)

docs/effects/screen.md section 2 (P = proven from the disassembly). A recursive feedback trail:
- **SaveScreen** (EffectSaveScreenTask exec 0x801D2764 -> 0x8042E838, PostEffect level, once per frame, full screen):
  copy the frame, draw it ONE/ZERO into the top-left W/2 x H/2 (stomp), copy that corner as the history (a W/2 x H/2
  RGB888 camera-texture raster, B+0x74), draw the full copy back (restore). History valid flag B+0x78.
- **DrawBlur** (EffectDrawBlurTask exec 0x801D27FC -> 0x8042E254, PostGlare level, per viewport, BEFORE SaveScreen in
  the frame): when enable (B+0x00), alpha (B+0x01) and history-valid: one quad = the viewport scaled by zoom (B+0x04)
  about its centre, sampling the previous frame's history, SRCALPHA / INVSRCALPHA, vertex alpha = alpha, NEAREST,
  depth test / write and fog off.
- Maths: `scene_n = lerp(scene_n, zoom(history_{n-1}), a/255)`, `history_n = downsample(scene_n)`. Driven by Chaos
  Control's slow motion (alpha ramps to 64, zoom 1.05) and data-driven EffectGlobalParam type 1 from effect files.
- Native port (deliberate deviation, recorded in screen.md): full view resolution R16G16B16A16_UNORM history,
  LINEAR sampling; recursion, weight, zoom and order exact.

## 2. Motion blur: measured cost (existing traces, before any change)

Source: the 2026-10-31 telemetry baseline (source/.build/validation/telemetry-2026-10-31/<route>-<res>-warm1/gpu.csv,
diag exe 942dae5f, fullscreen, 4x MSAA, driver clocks, 90 s tapes, first 300 frames skipped). Pass "history resolve" =
the history save; it is recorded only on 1.4-1.5 % of frames (contiguous stretches: game frames 189.. and ..3110 on
Westopolis, 1940..3995 on Final Haunt - the stretches where the blur is on).

| run | frames with a history resolve | its mean / p99 ms when present | its cost averaged over the run | share of GPU |
|---|---|---|---|---|
| Westopolis 1920x1080 | 76 (1.5 %) | 0.238 / 1.733 | 0.004 ms/frame | 0.1 % |
| Westopolis 2560x1440 | 70 (1.4 %) | 0.657 / 3.086 | 0.009 | 0.2 % |
| Westopolis 3840x2160 | 76 (1.5 %) | 0.366 / 0.692 | 0.005 | 0.1 % |
| Final Haunt 1920x1080 | 76 (1.5 %) | 0.295 / 1.874 | 0.004 | 0.1 % |
| Final Haunt 2560x1440 | 69 (1.4 %) | 1.197 / 3.243 | 0.016 | 0.3 % |
| Final Haunt 3840x2160 | 75 (1.5 %) | 0.650 / 2.056 | 0.010 | 0.2 % |

Findings so far:
- The native port does NOT pay for motion blur when it is off: no per-frame history save outside the blur stretches.
  On these tapes motion blur is not a disproportionate cost overall.
- While the blur IS on, its history save is a second full-resolution MSAA resolve of the scene, about as expensive as
  the frame's main "resolve" pass (Westopolis 1440p: history 0.657 vs resolve 0.943 ms mean) - the candidate for an
  exact saving (reuse the main resolve's output when nothing is drawn into the scene between the two points: being
  traced). The DrawBlur draw itself has no pass of its own (inside another pass): its cost is not isolated yet.
- These runs were at low driver clocks (P8 / P5 at 1080p, docs/TIMING_ARCHITECTURE.md 6.1): per-pass times swing
  with the clock; costs are compared within the same run, and A/B pairs must run back to back.

## 2b. Motion blur: code trace of the native path (read-only, 2026-11-02; renderer.cpp lines in wt-gc-removal)

- The history save has no renderer-side condition: a "history resolve" pass is recorded exactly when the frame's
  packet carries a SaveHistory pass, emitted only by the guest's SaveScreen copy (scaffold/rw_driver/screens.cpp:92-103
  -> frame.cpp:162-165). The 1.5 % rate is how often the GAME saves; the port saves nothing on its own. Frame n's
  DrawBlur reads frame n-1's save (history_valid, renderer.cpp:1686 / 2824 / 3058-3066) - the original's B+0x78 order.
- The history is NOT the main resolve's image: it is the scene at the SaveScreen point, before DrawScreenColor, the
  fade and the HUD / sprites, which draw later into the MSAA colour; the main resolve includes those. So the history
  resolve is required work - "reuse the main resolve" would change the image (exact only in frames with no post-save
  draw, which is rare). Same format (R16G16B16A16_UNORM) and the same PSResolve sample mean as the main resolve.
- Implementation (add_resolve, 1743-1796): a full-screen-triangle pixel-shader resolve of the 4x MSAA colour into a
  persistent per-view single-sample history (recreated only on a size change, Load::Discard - no clear), plus the
  MSAA RT->SRV and history ShaderRead<->RT transitions. No CPU wait, no per-frame allocation.
- DrawBlur: a 4-vertex quad (zoomed about the view centre, scissored) inside the view's "scene" pass, into the 4x
  MSAA colour, SRCALPHA / INVSRCALPHA, LINEAR clamp sampler (the documented deviation from the original's NEAREST on a
  half-size history); no port-side gate - the guest only issues it with enable, alpha and a valid history.
- Considered and rejected: reuse of the main resolve (not the same image); skipping saves the next frame will not
  read (guest-state prediction, risky, little to gain); ping-pong histories (saves one barrier, not the resolve); a
  region resolve (the zoomed quad samples the whole history).

Verdict so far: motion blur is already proportionate - zero cost while off, and while on one necessary resolve plus
one full-screen blend. The blend quad's own cost (inside "scene") is still to be isolated by per-draw timing on a
blur-heavy segment before the verdict is final.

## 2c. Motion blur: the blend quad isolated (per-draw GPU timing, 2026-11-02)

Run: .build/watch exe 29cdb23a..., Westopolis tape, windowed, render 2560x1440, 4x MSAA, 12 s budget,
GEKKO_GPU_TRACE + GEKKO_GPU_TRACE_DRAWS (.build/blur-draws/). The DrawBlur quad = the immediate draw whose image
source is History (draw note c bits 12-15 = 2): present in game frames 147-159 (10 frames), one per frame.

CAVEAT (found after this run): the per-draw trace keeps only frames whose GPU time is >= GEKKO_GPU_TRACE_DRAWS_MIN_MS,
default 8 ms (gpu_timer.h:142). All 10 blur frames here were >= 8 ms, so they were kept, but the sample is selected
by slowness; the figures below are superseded by the MIN_MS=0 runs at three resolutions (section 2d).

| quantity | value |
|---|---|
| blur quad GPU time | mean 1.42 ms, p50 1.44, max 1.55 (frames >= 8 ms only) |
| frame GPU time, frames with the blur | 8.29 ms mean |
| frame GPU time, other frames (after frame 60) | 5.77 ms mean |
| added while the blur is on | about 2.5 ms / frame (+43 %): the quad + the history resolve (about 1 ms) |

Why: one full-screen alpha blend into the 4x MSAA R16G16B16A16_UNORM scene - per pixel 4 samples x 8 bytes read
(blend) and written, plus the history texel read: bandwidth-bound, at 2560x1440 several hundred MB per draw. Driver
clocks were not sampled in this run (light frames run in low P-states, which inflates bandwidth-bound passes - see
docs/TIMING_ARCHITECTURE.md 6.1); the next run records NVML clocks and repeats at 1920x1080 and 3840x2160.
Exact-optimisation candidates examined:
- blend after the resolve (the resolve is a linear mean, so mathematically equal): NOT bit-exact - per-sample 16-bit
  rounding differs, and DrawScreenColor / fade / HUD still draw into the MSAA target after it. Rejected unless a
  rounding proof shows identical bits.
- lower precision / fewer samples / half-resolution history: forbidden (quality).
- still open: whether the quad's interior (all 4 dst samples equal) is already served by the GPU's colour
  compression, i.e. whether the cost scales with edge pixels or with all pixels - measured by the 1080p / 4K ratio.

## 2d. Motion blur: unbiased cost at three resolutions (2026-11-02)

Runs: exe 29cdb23a..., Westopolis tape, windowed, 12 s, GEKKO_GPU_TRACE_DRAWS_MIN_MS=0 (every frame), NVML sampler
beside the game (.build/blur-draws/scale2.sh -> scale-<res>/). 37 blur frames per run (game frames ~146-183).

| render size | blur quad mean / p50 / max | history resolve mean | quad ns per pixel | clocks at the blur frames |
|---|---|---|---|---|
| 1920x1080 | 0.492 / 0.483 / 0.584 ms | 0.138 ms | 0.237 | P5, core 330-360 MHz, memory 810 MHz |
| 2560x1440 | 0.259 / 0.255 / 0.268 ms | 0.198 ms | 0.070 | P3, core 375-420 MHz, memory 5001 MHz |
| 3840x2160 | 0.526 / 0.525 / 0.562 ms | 0.374 ms | 0.063 | P3, core 450-480 MHz, memory 5001 MHz |

- At a normal memory clock the blend costs ~0.065 ns per pixel and scales linearly with the pixel count: the cost of
  one full-screen blend into the 4x MSAA R16G16B16A16 scene. Active-blur total ~0.46 ms at 2560x1440, ~0.90 ms at
  3840x2160 (quad + history resolve).
- 1080p is 3.4x more expensive per pixel only because the driver had the memory at 810 MHz (P5) - the driver's power
  management on a light load (docs/TIMING_ARCHITECTURE.md 6.1, GPU_FILL.md 27), not work the effect wastes.
- The blur frames are not slower than the others in these runs (light stage-intro frames): the effect is within the
  frame budget everywhere measured.

**Verdict: motion blur is proportionate; no rewrite.** Zero cost while off; while on, one necessary resolve and one
full-screen blend at the bandwidth they need. The earlier 1.42 ms (section 2c) was the 8 ms trace filter's selection
of slow, low-clock frames. Remaining fidelity question (not cost): LINEAR sampling of a full-resolution history versus
the original's NEAREST on a half-resolution history (docs/effects/screen.md 2, a recorded deliberate deviation).
Tooling defects found on the way: (1) GEKKO_GPU_TRACE_DRAWS silently keeps only frames >= 8 ms unless
GEKKO_GPU_TRACE_DRAWS_MIN_MS is set - a default that biases any per-draw cost; (2) under gpu_clock_sampler.py's
wrapper the game's per-draw file came out truncated or empty (run the sampler beside the game instead).

## 3. Whole-frame GPU pass costs (same runs, mean ms per frame where the pass ran)

| pass | WST 1080p | WST 1440p | WST 2160p | FH 1080p | FH 1440p | FH 2160p |
|---|---|---|---|---|---|---|
| scene (opaque, transparent, particles, effects) | 3.939 | 3.523 | 3.473 | 2.376 | 2.817 | 2.798 |
| resolve (scene MSAA -> 1x) | 0.373 | 0.943 | 0.702 | 0.312 | 1.297 | 0.762 |
| compose | 0.843 | 0.263 | 0.363 | 0.594 | 0.358 | 0.360 |
| glare composite | 0.162 | 0.147 | 0.425 | 0.145 | 0.250 | 0.350 |
| present scale | - | 0.390 | 0.114 | - | 0.428 | 0.122 |
| whole frame p50 / p99 / max | 5.43 / 10.95 / 15.97 | 4.65 / 12.38 / 19.01 | 5.50 / 10.31 / 15.75 | 3.78 / 7.30 / 11.32 | 4.29 / 11.97 / 19.05 | 5.42 / 7.73 / 9.26 |

The scene pass dominates everywhere (60-80 % of the frame); inside it, particles / smoke were already shown to be up
to 90 % of heavy frames' GPU time (docs/GPU_FILL.md). Lower clocks at lower resolutions make 1080p look as slow as 4K.

## 3b. Full 90 s tapes, current build, with clocks (2026-11-02)

Runs: exe 29cdb23a... + per-draw layer tag in the trace note, windowed, render size as listed, 4x MSAA, every draw
timed, NVML sampled beside the game (.build/effects-audit-untagged/; first 120 frames skipped). These runs did NOT
tag draws by effect (the driver stamps layers only while an effect switch is off) - the per-effect split comes from
the SHADOW_R2_TAG_LAYERS=1 runs (section 3c).

| run | frame GPU p50 / p90 / p99 / max ms | resolve | compose | present scale | glare composite | glare sources | NVML P-state samples |
|---|---|---|---|---|---|---|---|
| WST 1920x1080 | 6.45 / 7.10 / 8.35 / 13.39 | 0.240 | 0.123 | 0.573 | 0.167 | 0.083 | P8 9710, P5 341, P0 243 |
| WST 2560x1440 | 5.14 / 9.71 / 11.81 / 18.13 | 0.844 | 0.230 | 0.294 | 0.132 | 0.062 | P5 6413, P8 3351, P3 340, P0 194 |
| WST 3840x2160 | 5.81 / 7.10 / 9.56 / 25.41 | 0.598 | 0.312 | 0.191 | 0.356 | 0.134 | P3 7825, P8 1265, P5 923, P0 292 |
| FH 1920x1080 | 5.94 / 6.47 / 7.63 / 9.21 | 0.284 | 0.124 | 0.566 | 0.147 | 0.229 | P8 9722, P5 292, P0 243, P3 48 |
| FH 2560x1440 | 4.51 / 9.08 / 11.31 / 22.05 | 0.936 | 0.253 | 0.283 | 0.224 | 0.178 | P5 6116, P8 3163, P0 584, P3 437 |
| FH 3840x2160 | 5.85 / 6.38 / 7.22 / 8.28 | 0.617 | 0.290 | 0.220 | 0.301 | 0.402 | P3 8596, P8 1266, P0 244, P5 194 |

(mean ms per frame over the run for the passes; history resolve 0.003-0.013 ms/frame everywhere.)
Observations: (1) the frame time is dominated by the driver's P-state, not resolution - 1080p runs in P8, 1440p
mostly P5 (810 MHz memory: p90 jumps to ~9-10 ms), 4K mostly P3; the same frame costs very different GPU time per
state, so an effect's "cost" must always be read with its clock. (2) "present scale" (the render size scaled into the
window) is 0.57 ms at 1080p windowed - a windowed-run artefact to re-check fullscreen at native size before calling it
an inefficiency. (3) The world scene's mesh draws are 85-91 % of draw time on these tapes; effects are a small share
on average - the per-effect split (3c) shows which effect owns the heavy frames' p99 / max.

## 3c. Per-effect GPU cost (SHADOW_R2_TAG_LAYERS=1, 90 s tapes, 2026-11-02)

Runs: as 3b plus SHADOW_R2_TAG_LAYERS=1 (.build/effects-audit/; full table copied to
docs/effects/cost_audit_2026-11-02.txt). Tagging leaves the frame time unchanged within noise (p50 within 0.1 ms of
the untagged runs except FH 1080p, which ran in a different P-state mix). Per layer: mean ms per frame over the run,
p99 / max of that layer's per-frame time, and its mean inside the heaviest 1 % of frames.

| layer (draw kind) | WST 1080p | WST 1440p | WST 2160p | FH 1080p | FH 1440p | FH 2160p |
|---|---|---|---|---|---|---|
| Particles: mean / p99 / max | 0.343 / 3.687 / 8.757 | 0.300 / 3.990 / 10.315 | 0.597 / 5.742 / 11.199 | 0.117 / 0.582 / 2.453 | 0.137 / 1.378 / 2.026 | 0.259 / 1.557 / 3.484 |
| Particles in the heaviest 1 % frames | 4.433 | 2.100 | 5.150 | 0.100 | 0.264 | 0.764 |
| MotionBlur: mean / max | 0.014 / 1.352 | 0.020 / 3.387 | 0.011 / 0.578 | 0.018 / 2.784 | 0.026 / 4.790 | 0.010 / 0.806 |
| Refraction (incl. heat-haze elements): mean / max | 0.021 / 1.326 | 0.013 / 0.175 | 0.018 / 0.493 | 0.012 / 2.422 | 0.021 / 3.410 | 0.020 / 0.815 |
| Trails: mean / max | 0.013 / 0.407 | 0.011 / 0.857 | 0.021 / 1.611 | 0.006 / 0.352 | 0.008 / 1.988 | 0.015 / 1.758 |
| Shadows / ScreenFade / Particles3D / Rays | <= 0.005 mean each | | | | | |

Findings:
- **Particles (the smoke) are the one disproportionate effect**: small on average, but they own the frame-time
  spikes - up to 11.2 ms of GPU in a single frame on Westopolis, 4-5 ms of the heaviest 1 % of frames. This is the
  overdraw already dissected in docs/GPU_FILL.md (one smoke effect drawn over the screen ~26 times; particles ~90 % of
  heavy-frame GPU); the exact-renderer work there is the optimisation track, not a new one.
- Motion blur, refraction / heat haze, trails, shadows, fades: each <= 0.03 ms per frame on average; their maxima are
  frames at the P5 low memory clock (810 MHz).
- Glare is not in these numbers (its work is passes, not scene draws): glare sources + composite 0.2-0.75 ms per frame
  (section 3b).
- TAGGING DEFECT (diagnostic, not rendering): 183 world mesh draws per frame are tagged "Sky" - the world sectors
  should be Landscape / LandscapeAlpha / ...; layer_of_caller / land_layer (scaffold/rw_driver/layers.cpp) misattribute
  them, so the Sky / None / Landscape split is NOT trustworthy until fixed. The effect layers above are distinct draw
  kinds (particle batches, ribbons, the History quad, refraction quads) and are attributed correctly.

## 4. Next (in order)

1. Code trace of the native blur path (history save condition, what draws between SaveScreen and the main resolve,
   formats, barriers, the DrawBlur draw's pass) - running.
2. A blur-heavy measurement: a tape segment with Chaos Control active (blur on for many frames), per-draw GPU timing
   (GEKKO_GPU_TRACE_DRAWS) to isolate the DrawBlur quad, at the three resolutions; the MotionBlur effect-layer switch
   for diagnostic isolation only.
3. Then the same breakdown for glare, heat haze, refraction, particles / smoke, screen colour, at the three
   resolutions, with clocks recorded (NVML sampler).
