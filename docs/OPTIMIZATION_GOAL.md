# Native x64 optimization objective

## CURRENT GOAL, UNIVERSAL EXACT ARCHITECTURE (user, 2026-10-30 late night): one specification, every vendor

STOP NVIDIA-specific work; keep the RTX 4080 research as evidence (GPU_FILL.md 19-23). (1) Reconstruct the observable
rasterization, interpolation, texture sampling, MSAA coverage, blending and quantization needed for the reference
output as a vendor-independent deterministic numerical specification with a portable C++ reference; prefer
controlled integer / fixed-point arithmetic; never assume IEEE float results match across GPUs. (2) Shared rendering
core for NVIDIA, AMD, Intel and future Vulkan Linux / Android; D3D12 stays the Windows backend, Vulkan prepared;
shared maths and shader logic, API specifics isolated; no mandatory vendor extensions; vendor optimisations only with
identical validated output and a portable fallback. (3) Priority: the 4x MSAA smoke - a portable tile-local compute
particle renderer, the reconstructed UNORM16 blend, specified interpolation; exact coverage, interpolation, texture
sampling, order, quantization; no approximation or tolerance. (4) Automated validation: CPU reference vs D3D12 vs
Vulkan vs real NVIDIA / AMD / Intel output, per MSAA sample; never claim an untested GPU family; where the old
renderer is nondeterministic, a canonical reference from the intended semantics, differences from historical
captures documented. (5) Phase III targets stay (3840x1200 p99 <= 1 ms; 5120x1600 median <= 1 ms), whole-frame
speedups measured per architecture. (6) Order: Specification -> CPU reference -> portable GPU prototype -> bit-exact
validation -> performance -> cross-vendor testing -> integration; the current renderer stays until replaced.

## CURRENT GOAL, EXACT-PIXEL ARCHITECTURE (user, 2026-10-30 late): redesign for exactness AND speed

No slower hybrid, no accepted pixel differences. Dissect the whole sample pipeline (coverage, sample positions, depth
order, fixed-function blend precision, sample storage / compression / quantization, reads / writes / resolves, draw
scheduling dependencies); decide which operations must stay ordered. Prototype genuinely different architectures
(programmable sample-aware blending, ROVs, explicit per-sample accumulation with deterministic quantization, shading
separated from ordered framebuffer updates, tile-local processing preserving global draw order, fewer framebuffer
reads / writes, other layouts and passes); prove both speed and bit-identity, never assume them. Exactness first:
the untiled renderer is the reference; compare every RGBA channel of every MSAA sample; repeat reference captures;
characterize any nondeterminism of the reference and never claim reproduction of mutually inconsistent outputs;
find the first differing operation and fix its cause; no ignored pixels or tolerances. Benchmark the whole chain at
32:10, 4x MSAA, same tapes, locked clocks and driver clocks (frame, per pass, bandwidth, VRAM, sample correctness,
final image); recover or beat 5K 2.66 -> 1.60 ms. Complete only when: bit-identical to the reproducible reference,
measurably faster full frame, all gameplay / audio / rendering regressions pass, no quality loss, documented and
reproducible. Dissect -> Model -> Redesign -> Implement -> Prove exactness -> Benchmark. Live record:
docs/GPU_FILL.md sections 19+.

## CURRENT GOAL, PHASE III (user, 2026-10-30 night): targeted native optimization

Targets: GPU p99 <= 1 ms at 3840x1200, GPU median <= 1 ms at 5120x1600, CPU game thread progressively <= 1 ms; 32:10,
4x MSAA, uncompromised quality. Priorities: (1) the 4x tiled-scene difference examined at the individual sample
level, its exact cause found and removed, then the measured ~40 % unlocked; (2) smoke / overdraw: the whole particle
pipeline, removing redundant fragments, blending and traffic without changing the image; (3) CPU game thread from
2.1 ms median toward 1 ms by native C++20 replacements of PowerPC-derived chains; (4) RenderWare loaders fully
native, stage-start frame <= 3 ms; (5) GPU memory, locality, render-target reuse, pacing, and the 585 MHz driver clock
behaviour explained without forced clocks. Validation: Westopolis and Final Haunt at 3840x1200, 5120x1600 and higher
32:10; CPU / GPU p50 / p90 / p99 / max, RAM / VRAM, presentation timing; locked vs driver clocks separate; identical
gameplay / audio / rendering where required; full suite; reproducible A/B; commits isolated from other sessions'
work. No cheating, no fake results, no quality reductions; document a limit with measurements rather than claim it.

## CURRENT GOAL, PHASE II (user, 2026-10-30 evening): fundamentally less CPU and GPU work for the same result

Summary of the user's "Extreme 32:10 Native Engine Optimization, Phase II" (it extends the goal below):
- Targets (genuine 32:10, 4x MSAA, full quality): 3840x1200 GPU p99 <= 1 ms and less CPU; 5120x1600 GPU median then
  p99 <= 1 ms; 7680x2400 minimal bandwidth and pass cost; 10240x3200 stable 60 FPS in demanding play; 16384x5120
  better stability, memory and worst frames. CPU, GPU and presentation measured separately with their critical path;
  a 1 ms GPU result is not a 1 ms complete frame.
- GPU: (A) smoke / particle overdraw - reverse the whole particle path, remove work without changing the image, no
  low-resolution approximation; (B) the 4x MSAA tiled scene - find the cause of its deterministic differences
  (depth compression, coverage, storage, transitions, rasterization) with controlled experiments before enabling it;
  (C) bandwidth - redundant reads / writes, lifetimes, aliasing, intermediates, transitions, locality, sync.
- CPU (game thread p50 ~2.1, p99 ~3.5, max ~7.3 ms): remaining guest dispatch, register / FPSCR sync, object and
  collision loops, animation / bones, transforms, RenderWare, memory ops and smart pointers - C++20, SIMD where it
  pays, original floating point and memory side effects kept; eliminate computation where safe.
- Native RenderWare loading for the stage-start frame: < 10 ms first, then toward 3 ms; no hiding stalls by deferral.
- GPU clocks: explain the ~585 MHz driver behaviour (workload, pacing, utilization, power policy); no forced clocks as
  a shipping workaround; locked clocks only for controlled comparisons, never reported as player performance.
- Correctness: no cheating; every optimization compared with the original path; deterministic differences
  investigated and any accepted tolerance documented; 131/131 kept and coverage extended.
- Discipline: profile -> reverse -> find redundancy -> design native -> implement -> validate -> benchmark (alternating
  A/B, Westopolis and Final Haunt, driver and locked clocks separately) -> keep or revert. Coordinate with other
  sessions. Update the docs. Commit verified changes as separate milestones once the suite and gates pass. Keep the
  rendering logic portable for a future Vulkan backend (Linux, Android).

## CURRENT GOAL (user, 2026-10-30): extreme 32:10 resolution and a 1 ms complete GPU frame

"Shadow Recomp - Extreme Resolution & Performance Optimization" (verbatim summary of the user's directive; it
supersedes the 10 ms and 3 ms goals given earlier the same day and the 5K-fullscreen benchmark rule):
1. All primary benchmarks at a 32:10 ultrawide aspect (not 16:9), with the exact internal render dimensions reported.
2. Maximum resolution: the highest genuine internal 32:10 render size the engine and GPU SUSTAIN - no crash, device
   removal, memory exhaustion, corruption or instability; tested progressively above 5K. Launching is not stable;
   one good frame is not proof.
3. 1.00 ms or less per complete GPU frame in real gameplay (aspirational); if unattainable at a resolution, prove the
   limiting factor with measurements and keep removing genuine overhead.
4. Optimize passes, shaders, MSAA, overdraw, bandwidth, render-target allocation, synchronization, transitions,
   geometry, CPU submission - with exact maths, efficient algorithms, culling, modern DX12.
5. Zero quality compromises: full internal resolution, 4x MSAA, every effect, all visible geometry; no hidden or
   dynamic resolution scaling; presentation may only scale what the display cannot show.
6. Real measurements of the whole CPU-GPU pipeline: GPU p50/p90/p99/max, CPU frame times, RAM, VRAM, pacing,
   stability; rendering separated from presentation overhead. Headless-only, partial-frame or isolated-pass numbers
   are not success.
7. Exactness and stability: gameplay state, PCM, captures, resource correctness against the original; EVERY
   deterministic mismatch investigated (stage starts, heavy effects, smoke, glare, shadows, extended play). The
   original rendering path stays available for A/B.
8. Final report: maximum stable 32:10 resolution, complete GPU/CPU frame-time statistics, total memory, visual
   equivalence, and the exact bottlenecks preventing further progress.

Live work log and handoff: docs/GPU_FILL.md (section 13 onwards).

Requested on 2026-10-23. `NORTH_STAR.md` governs the architecture.

## Outcome

Produce a measurably smaller executable and faster, less resource-demanding gameplay by replacing proven GameCube-specific implementations with lightweight native x64 PC code. Improve the tools needed to choose, implement and verify those replacements. Preserve game behavior, intended visuals, audio, input, required timing and determinism.

## End-state clarification

The user clarified on 2026-10-23: keep eliminating the GameCube burden until only game logic and necessary boot code remain. Intermediate size/CPU gains do not complete that request. Console register/memory machinery, translated SDK/platform services and transitional marshalling must be retired as native PC ownership replaces them. Preserve game initialization and the native rendering, audio, input, storage and scheduling services the game actually requires. Original console code may remain as independent development/reference evidence, not an executable gameplay dependency.

## Behavioral acceptance

The user explicitly approved smaller native x64 implementations that perform the same necessary math. Executable bytes and obsolete console register state need not be identical. Preserve observable gameplay, numerical contracts, side effects and required timing; prove which state is dead before eliminating it. Exact comparisons remain useful diagnostics, but do not constrain a recovered native algorithm to the console implementation.

## Current priority: presentation correctness

The user requested graphics, texture and sound fixes on 2026-10-23. On 2026-10-24 the user confirmed that the old Bugs.txt reports (Lost Impact edge culling and the homing-launcher target layer) were fixed long ago and requested deletion; they are closed by that correction. game/Bugs.txt was deleted. Validate newly observed issues with isolated persistence, exact build/input provenance, captures and audio evidence. The missing auxiliary audio return and the separately reproduced three-pixel Final Haunt capture variability remain current findings; clean counters alone do not certify intended visuals or sound.

## Latest steering: resource use and code size

The user additionally requested better CPU, RAM and VRAM use, and smaller code
with identical necessary math and faster methods. Continue correctness work as
an acceptance gate. Measure process memory, D3D12 allocation bytes versus heap
capacity, and exact executable size separately. First investigate redundant
checks in native matrix composition; require original-word edge/cancellation
oracles, equal-work kernel measurements and actual gameplay comparisons.
The reproduced checkpoint audio-history discontinuity is fixed for v20 files;
older v19 files cannot recover history they never saved. See the tenth increment
in OPTIMIZATION_RESULTS.md. This correctness work does not meet the FPS target.

## Performance target: at least 800 FPS at native 1440p

The user corrected the target on 2026-10-23: at least 800 FPS at native
2560x1440, a sustained 1.25 ms rendered-frame budget, with original gameplay
speed, full intended graphics, textures, sound and input. This supersedes the
previous 1000 FPS target and the proposed 1280x720 performance route. This is
a target, not a measured result or a guarantee on arbitrary hardware. Pin
CPU/GPU/driver, native render and output resolution, quality, stage, route and
build. Report sustained fresh rendered-frame throughput and frame-time
percentiles, CPU critical-path cost, GPU completion time, memory and loading
separately. Reduced internal resolution, repeated presents, frame generation/
cross-fades, accelerated game time, headless packet throughput and inverse
kernel timings do not meet the target. Keep fixed-step gameplay semantics
where required; reaching the presentation target requires native render-state
ownership independent of legacy packet production, rather than merely
changing a rate setting. First establish the actual present/simulation/GPU
bottlenecks and finish the current exact-math, code-size and allocation gates.
Earlier 720p resource comparisons remain explicitly scoped to that resolution
and are not evidence for the 1440p target. No 800 FPS achievement is claimed.

## Fixed timestep is mandatory

The user explicitly required a fixed timestep to avoid physics bugs. Preserve
the established gameplay/physics tick contract and deterministic update order;
rendering must run independently. Do not raise physics speed to manufacture
FPS, use variable frame duration in physics, skip simulation steps, or hide
catch-up debt. Validate collisions, movement, timers, input and audio timing
across render rates and real host hitches. The current host pacing quantum is
60 Hz; this alone is not proof that every recovered game subsystem has the
same tick contract. Recover each contract before migrating its owner.

## Current audio migration gate

The original-word reverb contract is now tested, including initialized pre-delay
behavior, allocation failure and the auxiliary slot protocol. Source inspection
confirmed that the mixer still omits the auxiliary A send/return connection.
Restore the full intended native audio path before claiming an optimization;
removing an unconnected callback does not qualify. Game parameter writers,
shutdown ownership, checkpoint continuation and device delivery remain required.
The pure native effect now passes full-state original-word comparisons,
including overflow, tiny tails and restores. Its gameplay integration and
legacy removal remain pending. Native surround and two-block matrix output with v22 checkpoint history now
pass original-word mode transitions and real-game continuation checks. The
auxiliary effect return remains absent. The fourteenth increment records the
512-byte correctness cost, unchanged tested PCM and unmet FPS target. Repeated
game controls and destructor order are covered with explicit boundary fixtures;
the inconsistent-flag counterexample prevents claiming universal lifetime proof.
The fifteenth increment adds an owned native EffectStage for pending signal and
control timing. It passes 360 original-code dynamic blocks and 30 restores; 80
game bank-switch cases preserve effect history. It remains unconnected to the
voice producer and absent from the production link. Native voice/resource
ownership and direct game control integration must precede legacy removal.

## Work sequence

1. Establish source- and binary-hashed release baselines. Finish the existing validation, record failures and missing inputs honestly, and use isolated saves and repeatable input routes.
2. Improve attribution tools: exact PE section sizes; linked code ownership and generated-function retention; reachable console dependencies; CPU hot paths; frame-time distributions; allocations, working set and commit; GPU memory where measurable; startup and loading costs. Every report must name its inputs and distinguish exact measurements from estimates.
3. Rank bounded replacement candidates by measured size/runtime cost, expected benefit and evidence confidence. Recover the original contract by disassembling and tracing the real DOL: callers, data, ownership, lifetime, error handling, side effects and timing. Do not select a candidate merely because its name sounds obsolete.
4. Rewrite the owning implementation for native pointers, values, resources and platform services. Preserve behavioral meaning, not console transport. Remove the replaced translated body and dependencies from code generation and linking; verify their absence in the actual production executable.
5. Compare baseline and candidate sequentially, with matching assets, logical state/input, compiler settings, resolution and hardware. Separate diagnostics from repeated uninstrumented timing. Report all runs, medians/spread, paired deltas and relevant regressions; first use identical-binary runs to measure instability.
6. Gate each accepted change on reference/behavior tests, meaningful regression tests, deterministic routes where applicable, gameplay captures, audio checks and GPU validation. Keep concise reproducible evidence and an explicit inventory of unresolved console dependencies.

## Rules against false progress

- No compatibility heaps, permanent guest-address wrappers, hardware facades or renamed emulation presented as a native port.
- No speed claims from disabled features, changed workloads, cherry-picked runs, missing samples or diagnostic-only timing.
- No deleting or weakening failing tests to obtain a green result. Missing fixtures remain visible until genuinely recovered or replaced with equivalent independent evidence.
- A smaller file alone does not prove native replacement or faster gameplay. Report actual code removal and runtime/resource effects separately.
- Do not call the whole game native while translated execution or console scaffolding remains.
- Preserve the deployed executable, comparator reference and all personal data. Test in `.build/` with isolated persistence; deployment needs an explicit request. Keep the clean `source/`, `game/`, `RW37SDK/` layout and remove disposable outputs.

## Initial evidence

The release build after the reverb/memory integration repair is 109,440,512 bytes, SHA-256 `24015edc6eb4f8692ba32611e2d20841967169aebc16d856425e2b3d4d79623f`. It ran 22 seconds each in Westopolis and Final Haunt at 1280x720, with movement input, audio and D3D12 validation requested. Both runs reported no render failures, asset-read failures or audio underruns. These are smoke checks, not full-game certification or performance benchmarks.

The reverb regression compared 3,000 cases successfully. Broader validation exposed an outdated asset path and historical tests requiring a removed `B45.mem1` fixture. Those limitations must be resolved or explicitly carried in every readiness statement. No optimization gain has been claimed.

## Completion evidence

Completion requires genuine native replacements with verified absence of superseded production code, a quantitatively smaller executable, reproducible evidence of faster/lower-resource gameplay, documented working tools and no concealed validation regressions. Record the scope actually achieved and remaining dependencies; a partial rewrite is not complete console removal.


## Latest frame pacing requirement

On 2026-10-24 the user requested eliminating the console 60-to-30 fallback.
The active limiter already suppressed positive multi-period requests. Its
remaining variable-interval wait machinery is now removed and actual wait /
resume behavior is covered for 9,600 frames. Recorded Westopolis replay,
checkpoint, PCM and native-1440p capture comparisons pass. This cleanup saves
1,536 executable bytes, with no claimed new FPS or temperature gain. Preserve
fixed pacing; optimize the actual CPU/GPU work next. Repair the lifted renderer
CSV telemetry before using those fields for bottleneck or thermal attribution.
See docs/FRAME_PACING.md and .build/validation/frame-pacing/summary.json.


## Current measurement prerequisite

The eighteenth increment repairs lifted CSV, per-frame and JSON measurements,
with 14 focused tool cases and byte-identical Westopolis replay state, PCM and
1440p captures. The 30-second diagnostic contains every rendered frame and
attributes its largest hitch to the synchronous screenshot. It cannot identify
the earlier temperature spike or establish a speedup. The candidate is another
3,072 bytes smaller. Measurements now expose the approximately 60 Hz packet
production ceiling; native frame production/ownership and real CPU/GPU work
are the next optimization targets. Preserve fixed game pacing and genuine
rendering. See docs/PRESENTATION_TELEMETRY.md and
.build/validation/presentation-telemetry/summary.json.
