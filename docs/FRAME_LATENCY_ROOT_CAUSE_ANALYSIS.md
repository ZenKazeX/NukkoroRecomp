# Frame latency root-cause analysis (2026-11-04)

Investigation only: no gameplay, rendering or algorithm change. Every number below comes from a run made in this
session; the run directory is under `.build/latency/runs/<name>/` (raw traces `present.csv`, `gpu.csv`, `fw.csv`,
`init.csv`, `samples.csv`, `clk/`; per-frame table `perframe.csv`; `analysis.txt`). Scripts: `tools/latency/`
(copies of the ones used in `.build/latency/`; they carry that worktree's absolute paths). Nothing was committed,
deployed or copied to `../game`.

## 0. Answer in one page

There is no single "17+ ms" cause. Three independent mechanisms produce frames or displayed intervals above 16.67 ms;
only the first matters in steady gameplay.

| # | Mechanism | Where | Worst measured | Deterministic? | Removed by |
|---|---|---|---|---|---|
| 1 | The smoke particle batches (texture 0x010000F1, 84 % of the scene pass) cost up to 10.5 ms of GPU at 5K (7.2 ms at frame 1229) with the GPU at full locked clocks, and the driver does not keep the GPU at full clocks: the same frames cost 14-18 ms at the driver's P3 state and 25-36 ms in 4 of 10 unsampled runs | Westopolis game time 20.1-21 s (frames 1205-1260), 5K; also 3840x1200 | GPU frame 36.19 ms (run `s5k-unl-2`); typical unlocked max 15.6-18.2 ms (13 of 17 runs) | content: yes (same frames every run); amplification by the clock state: intermittent | locked clocks (A/B: 0 frames over 16.67 ms in 86 s at 5K and at 3840x1200, `L5k-lock`, `Lw32x10-lock`) |
| 2 | Render-thread one-time resource creation, proportional to the render size: `AllocateMemory` of render-target heaps (6.7-10.9 ms each) and large texture creation (3.7-5.8 ms each) inside `Renderer::record` | first frame (24.8 ms), first gameplay frame at 2.250 s (13.2-14.4 ms, was 21 ms in the first cold run), `shadow.view_history` at 3.15 s (3.7-4.0 ms); 5K only above 9 ms | record 25.9 ms (first frame), 21.0 ms (`fs5k-a` frame 135) | yes, every run | allocating the pool and the history before the first presented gameplay frame |
| 3 | Game thread at the stage hand-off (frames 76-79, game time 1.267-1.32 s): 21-25 ms of work in a 16.67 ms frame; 40 ms in the first cold run | translated RenderWare / ONE-archive loaders plus native file API plus PRS-ahead bookkeeping | 40.1 ms (`fs5k-a`, cold), 22-25 ms warm | yes (warm 22-25 ms) | not display-visible in any run (buffered by the packet queue) |

Unlocked clocks also add GPU start latency (submit to GPU begin, p99 2.4-3.5 ms and up to 20.9 ms at 480p and 2133x1200,
versus p99 0.22-0.28 ms and max 0.5 ms with locked clocks) and late presentation of frames whose GPU work had already
finished ("after_gpu" repeats: 1-7 per unlocked 86 s run, 0 gameplay repeats in either locked 86 s run). Together they give
displayed intervals of 33-67 ms on frames whose GPU work is 0.35-8 ms (section 6.2).

Steady-state game-thread and render-thread CPU are not the problem: game-thread work p99 4.0-4.8 ms and max 5.0-8.2 ms
in the 86 s runs (one outlier 11.0 ms), render-thread CPU p99 0.55-0.68 ms and max 1.0-3.0 ms outside the three one-time events.

With the GPU clock held (diagnostic `SHADOW_R2_STABLE_POWER=1`), a full 86 s Westopolis replay at 5120x2880
fullscreen has GPU p50 / p90 / p99 / max 2.70 / 3.28 / 6.98 / 11.96 ms, every displayed interval 16.67 ms, no
repeats after the boot screens. So stable 60 FPS at 5K needs the GPU clock policy fixed (section 9); it does not
need the 3 ms or 1 ms goals.

## 1. What "17+ ms" can mean here (task 1)

The number is not tied to one instrument in the repository. The candidates, all produced by existing tools, with
their clock and frame boundary (one clock for all: `std::chrono::steady_clock` = QueryPerformanceCounter, ns):

| Metric | Source | Frame boundary | Overhead | Not the same as |
|---|---|---|---|---|
| GPU frame ms (`gpu_ms`) | `GEKKO_GPU_TRACE` (gpu_timer.cpp): timestamp queries at the start and end of the renderer's command list, mapped through `GetClockCalibration` | one renderer frame (packet) | one query pair per frame; draw detail `GEKKO_GPU_TRACE_DRAWS` adds a pair per draw (scene 11.69 vs 11.02 ms, +6 %) | display time; includes nothing before the GPU starts it |
| Render-thread CPU | gpu.csv `cpu_submit_ns - cpu_begin_ns` (begin = after the swap-chain wait and the frame-slot wait; submit = after `ExecuteCommandLists` returns) | recording + submit of one packet | none beyond the trace | Present call (`present_call_ms`, 0.2 ms median) |
| Game-thread work | `GEKKO_FRAME_WORK` (native_frame_limiter.cpp): host time at every limiter entry (5 per 60 Hz frame, 300 Hz quanta), minus the pacer sleeps (`PacerSleep` rows of the present trace) | limiter quantum, summed per game frame | one vector push per entry | headless wall time (no sleeps) |
| Push interval | present trace `Push` rows | game thread packet to packet | none | work: it is pacing (16.65 ms median by construction) |
| Displayed interval | DXGI frame statistics `SyncQPCTime` of consecutive displayed presents | refresh | one `GetFrameStatistics` per present | GPU time: a frame of 16.1 ms already shows 33.3 ms (section 5) |
| End-to-end latency | frame_telemetry_report: input latch to the vblank that shows the frame | | | 20-38 ms by the boot's push phase (section 4), not a spike |

Which one the "17+" came from could not be established from the repository. The documented sightings are: GPU frame
max 17.31 ms at 5K (docs/RENDER_TARGET_ALIASING_5K.md), presentation p99 17.4 ms in the user's 383 s session at 1080p
(docs/CPU_FINAL_HAUNT.md), the game-thread stage-start frame 17.7-18.5 ms before PRS-ahead (docs/GPU_FILL.md section
9, now 13.8-14.9 ms headless). This session's own measurement that matches best is the GPU frame max in the smoke
window at 5K with driver clocks: **15.6-18.2 ms in 13 of 17 runs** (table 5.2). Treat it as the working definition;
the report keeps all metrics apart.

## 2. Method (task 2)

Replay: Westopolis, `--boot-stage stg0100 --input-tape-play input.tape` (tape and card copies from
`.build/stage_loading/p3g/tape-westopolis/stg0100/4/`, 86 s of game time, 5,116 simulation frames), `--no-audio`,
fresh isolated run directory per run (card, SRAM, tape, pad.cfg copied; originals untouched). Display config
generated per run. Traces always on: `GEKKO_PRESENT_TRACE`, `GEKKO_GPU_TRACE`, `GEKKO_FRAME_WORK`. Environment
`GEKKO_*` / `SHADOW_*` variables cleared before each run, then only the listed ones set. A process list sampled every 2 s
shows no other ShadowTheHedgehog process alive during any run listed here (`others.txt`, max 1).

Machine: i9-13900K (32 logical), RTX 4080, one 1920x1080 60 Hz monitor (DXGI-reported 60.0006 Hz), Windows 11. Fullscreen
= `--fullscreen` borderless on the primary monitor; the render target is `--render-width/height` and is scaled
into the 1080p back buffer (the render size is not the display size). This is the same arrangement as the earlier
"5K fullscreen" rule.

Executables (three builds of `.build/loadfix`, Release, SHADOW_SHIPPING=OFF so the diag switches exist; the first is a
copy of the tree's exe at 08:22 before I touched anything, the others add only the env-gated trace of section 8):

| exe | sha256 prefix | used by |
|---|---|---|
| `.build/latency/exe` | d73a95f1 | `fs*`, `s*`, `u5k-*` |
| `.build/latency/exe2` | b5d5d188 | `i*`, `w32x10-*` (pipeline / texture / buffer trace) |
| `.build/latency/exe3` | ec550268 | `j*`, `h-*`, `r*`, `L*` (adds heap, placed, record, submit scopes) |

Caveat: exe2 / exe3 were linked from the worktree as it stood when I built them, so they include whatever the other
session had edited in the meantime. Nothing in the numbers depends on it (same-source pairs are used for every A/B),
but run-to-run comparisons across exes are not claimed.

Resolution deviations from the brief: "32:10" with this tape renders **2133x1200** unless `--tape-output-free` is
given (the game follows the tape's 1920x1080 output; `fs2133x1200-*` and `s2133x1200-lock` are those runs and are
labelled as such). The real 3840x1200 runs are `w32x10-*` and `Lw32x10-lock` (`--tape-output-free`). A first 480p
attempt in a window (`_anomalous-win480-windowed`) presented 15.7 fps (the window was not being flipped at
60 Hz; cause not investigated) and is excluded; the 480p runs are fullscreen 854x480.

Measurement overhead: the telemetry sampler thread costs 0.63 % of one core and the game thread 16.9 % of one core
(ThreadCpu rows of `fs5k-a`), whole process 18.7 s CPU in 86.2 s (0.22 logical core). The init trace (section 8): `i5k-lock-1`
(on) vs `i5k-lock-off` (off) render-thread CPU p50 0.41 / 0.41, p99 0.76 / 0.71, max 1.22 / 1.18 ms, GPU identical
(p50 2.05, max 11.95 / 11.94). The in-process stack sampler (`GEKKO_SELF_SAMPLE`) lengthens the stage hand-off by
about 3 ms (23.8 vs 20.8 ms, headless). The NVML clock sampler (separate process, 10 ms) is not neutral for the clock
trajectory in a way I could prove (section 6.2).

Valid vs not: all rendered runs are production-like (borderless fullscreen, vsync, DXGI latency waitable) with the
driver's own clock policy, except where `SHADOW_R2_STABLE_POWER=1` is named (diagnostic: `SetStablePowerState`,
graphics 2205 MHz, memory 11201 MHz; not a shipping behaviour). Headless runs are CPU-only measurements (the game
thread never sleeps, nothing is rasterised) and are used only for the stage hand-off attribution.

## 3. Run inventory

| group | runs | what |
|---|---|---|
| 5K full, driver clocks | `fs5k-a`, `fs5k-b`, `fs5k-clk` (NVML), `u5k-clk-1..3` (NVML, 70 s) | 86 s (70 s) |
| 5K 25 s, driver clocks | `s5k-unl-1..5`, `s5k-smp-3..5` (NVML), `i5k-unl-1`, `j5k-unl-1..2` | smoke window |
| 5K locked | `s5k-lock-1..2`, `i5k-lock-1`, `i5k-lock-off`, `L5k-lock` (86 s), `s5k-lock-p1/p2/p4` (probes), `s5k-lock-draws` (per-draw GPU) | |
| 2133x1200 (asked 3840x1200, 16:9 tape output) | `fs2133x1200-a/b` (86 s), `s2133x1200-lock` | |
| 3840x1200 (`--tape-output-free`) | `w32x10-a/b` (86 s), `w32x10-lock`, `Lw32x10-lock` (86 s) | |
| 480p control, fullscreen | `fs480-a` (86 s), `s480-lock`, `i480-lock`, `j480-lock` | |
| 1080p / 1440p locked | `s1080-lock`, `s1440-lock`, `i1080-lock`, `j1080-lock` | |
| stage hand-off, headless | `h-base-1..5`, `h-noprs-1,3,4,5` (`GEKKO_PRS_AHEAD=0`), `h-ss-1..3` (stack sampler) | 4 s |
| stage hand-off, rendered sampler | `r1080-ss-1..2`, `r5k-ss-1..2`, `r5k-base-1..2` | 4 s, locked |
| init trace | `i5k-*`, `i1080-lock`, `i480-lock`, `j5k-unl-1..2`, `j1080-lock`, `j480-lock` | `init.csv` |

Every run directory holds `run.txt` (exact command and environment), `exe.sha256`, `start.txt`, `end.txt`, `others.txt`.
`fs5k-a` was the first run of this session and a cold one (game files not yet in the OS cache; section 6.7).

## 4. Distributions (gameplay frames, game tick >= 300; ms)

Definitions: GPU = `gpu_ms`; game = game-thread work per game frame; rcpu = render-thread CPU (record + submit);
disp = displayed interval; latch = input latch to display. "missed" = displayed interval >= 25 ms.

### 4.1 Full 86 s replays

| run | render | clocks | metric | p50 | p90 | p99 | max | frames > 16.67 | missed vblanks |
|---|---|---|---|---|---|---|---|---|---|
| `fs5k-a` | 5120x2880 | driver | GPU | 5.79 | 6.77 | 10.74 | 17.13 | 1 | 3 |
| | | | game | 2.76 | 3.48 | 4.30 | 6.30 | 0 | |
| | | | rcpu | 0.41 | 0.47 | 0.59 | 1.46 | 0 | |
| | | | latch | 30.73 | 34.03 | 34.36 | 50.67 | | |
| `fs5k-b` | 5120x2880 | driver | GPU | 7.39 | 9.19 | 13.51 | **30.52** | **33** | **51** |
| | | | game | 2.78 | 3.50 | 4.14 | 5.00 | 0 | |
| | | | rcpu | 0.41 | 0.46 | 0.58 | 1.04 | 0 | |
| | | | latch | 20.44 | 23.75 | 36.63 | 56.59 | | |
| `fs5k-clk` | 5120x2880 | driver, NVML | GPU | 5.81 | 6.78 | 10.68 | 16.53 | 0 | 1 |
| | | | game | 2.82 | 3.59 | 4.77 | 7.20 | 0 | |
| `L5k-lock` | 5120x2880 | locked | GPU | 2.70 | 3.28 | 6.98 | 11.96 | 0 | **0** |
| | | | game | 2.73 | 3.43 | 4.00 | 5.65 | 0 | |
| | | | rcpu | 0.41 | 0.46 | 0.55 | 2.17 | 0 | |
| | | | disp | 16.67 | 16.67 | 16.67 | 16.67 | | |
| `w32x10-a` | 3840x1200 | driver | GPU | 5.06 | 8.26 | 12.83 | 17.70 | 1 | 13 |
| `w32x10-b` | 3840x1200 | driver | GPU | 5.26 | 7.89 | 12.63 | 16.74 | 1 | 3 |
| | | | game | 2.75 | 3.48 | 4.59 | 11.00 | 0 | |
| `Lw32x10-lock` | 3840x1200 | locked | GPU | 0.73 | 0.83 | 1.83 | 3.18 | 0 | **0** |
| | | | game | 2.69 | 3.43 | 3.98 | 6.21 | 0 | |
| `fs2133x1200-a` | 2133x1200 | driver | GPU | 6.09 | 7.63 | 11.73 | 22.84 | 13 | 17 |
| `fs2133x1200-b` | 2133x1200 | driver | GPU | 6.03 | 8.20 | 10.73 | 24.21 | 2 | 12 (max 66.7) |
| `fs480-a` | 854x480 | driver | GPU | 1.63 | 3.47 | 5.39 | 10.34 | 0 | 6 (max 66.7) |
| | | | game | 2.78 | 3.62 | 4.50 | 8.18 | 0 | |

The driver-clock GPU distribution moves by a factor of 2-3 between identical runs: GPU p50 5.79 / 7.39 / 5.81 ms at
5K (`fs5k-a/b/clk`) against 2.70 locked. Game-thread and render-thread CPU are the same in every run (p50 2.7-2.8 ms
and 0.41 ms) at any resolution.

### 4.2 25 s smoke-window runs, locked clocks (frames 300-1500)

| render | GPU p50 / p90 / p99 / max | scene pass max in the smoke window | run |
|---|---|---|---|
| 854x480 | 0.15 / 0.21 / 0.46 / 0.50 | 0.40 | `s480-lock` |
| 1920x1080 | 0.24 / 0.42 / 1.56 / 1.73 | 1.61 | `s1080-lock` |
| 2560x1440 | 0.44 / 0.73 / 2.74 / 3.02 | 2.80 | `s1440-lock` |
| 2133x1200 | 0.32 / 0.54 / 1.93 / 2.15 | 1.96 | `s2133x1200-lock` |
| 3840x1200 | 0.52 / 0.82 / 2.83 / 3.19 | 2.93 | `w32x10-lock` |
| 5120x2880 | 2.05 / 3.35 / 10.52 / 11.96 (two runs agree to 0.01 ms) | 11.01 | `s5k-lock-1`, `s5k-lock-2` |

Pixels: 0.41 / 2.07 / 3.69 / 2.56 / 4.61 / 14.7 million. In the smoke window the scene pass is close to linear in pixels:
0.64-0.78 ms per megapixel from 1080p to 5K (480p 0.97 ms per megapixel, fixed costs). 5K has 4.0x the pixels of 1440p and
costs 3.9x (11.01 vs 2.80 ms). So the smoke is a fill cost: it grows with the area, not with anything else in the frame.

## 5. The GPU-bound spike: smoke (mechanism 1)

### 5.1 What the frame is, and the real budget

Timeline of one frame, from `rep/frames.csv`, times relative to the vblank at which the frame is shown (`fs5k-a`,
frames 1214-1217, all on time):

| tick | latch | push | pop = waitable | record begin | submit | GPU begin | GPU end | gpu_ms |
|---|---|---|---|---|---|---|---|---|
| 1214 | -33.94 | -27.35 | -16.64 | -16.64 | -16.16 | -16.13 | -3.30 | 12.84 |
| 1216 | -30.98 | -27.43 | -16.65 | -16.65 | -16.21 | -15.99 | -1.43 | 14.56 |
| 1217 | -34.00 | -27.36 | -16.61 | -16.61 | -16.12 | -15.93 | **-0.40** | 15.53 |
| 1218 | -47.28 | -44.22 | -33.33 | -33.33 | -32.86 | -32.66 | -16.54 | 16.13 |

The renderer takes its swap-chain credit one refresh before the vblank that shows the frame, records for ~0.5 ms and
the GPU starts 0.1-0.2 ms later, so the GPU has **about 16.0-16.1 ms, not 16.67 ms**. Tick 1218 ends 0.13 ms after the vblank at -16.67, is shown one refresh late (33.33 ms
displayed interval), and tick 1227 (16.01 ms) is also late. A frame over ~16.1 ms of GPU time is a missed vblank.

### 5.2 Smoke window GPU max, every unlocked 5K run (frames 1200-1260)

| runs | NVML sampler | GPU max in the window (ms) | frames > 16.67 |
|---|---|---|---|
| `fs5k-a`, `i5k-unl-1`, `j5k-unl-1`, `j5k-unl-2`, `s5k-unl-4`, `s5k-unl-5` | no | 17.1, 18.1, 16.8, 17.3, 16.1, 15.8 | 1, 1, 1, 1, 0, 0 |
| `fs5k-clk`, `u5k-clk-1..3`, `s5k-smp-3..5` | yes | 16.5, 16.0, 16.9, 17.2, 16.2, 15.6, 18.2 | 0, 0, 1, 1, 0, 0, 1 |
| `fs5k-b`, `s5k-unl-1`, `s5k-unl-2`, `s5k-unl-3` ("spike mode") | no | **30.5, 32.4, 36.2, 25.0** | 17, 14, 18, 9 |
| locked: `s5k-lock-1/2`, `L5k-lock`, `i5k-lock-1` | n/a | 11.96, 11.95, 11.96, 11.95 | 0 |

Typical mode (13 of 17 runs): 15.6-18.2 ms, up to two frames over; spike mode (4 of 17, 24 %): 25-36 ms for 9-18
frames. The NVML sampler runs never entered spike mode (0 of 7 against 4 of 10 without it, Fisher one-sided
p = 0.09): not significant, so no claim that the sampler changes the power state.

### 5.3 Which draw: per-draw GPU timestamps

`s5k-lock-draws` (locked clocks, `GEKKO_GPU_TRACE_DRAWS_MIN_MS=0.3`; tick numbers in this paragraph are gpu.csv's packet
tick, 1-2 above the game-frame tick used elsewhere): the sum of draw times equals the scene pass to within 0.04 ms (tick 1229:
scene 8.11, draws 8.07, GPU 8.71). The particle batches of texture **0x010000F1** are 10.46 ms of 11.50 (tick 1205), 7.18 of
8.07 (tick 1229), and **234.5 ms of 278.3 ms of all draw time (84 %, also 84 % of the 280.5 ms scene pass) over ticks
1210-1259**: 6 batches of 60 particles (kind 1, a = 60) per frame. That is the same effect that docs/GPU_FILL.md
section 3b found (a 32x32 smoke puff). Source: particle path of `Renderer::record` (renderer.cpp ~1928-1960 draw loop;
particle pass shaders under `runtime/lift/render/shaders`). The PPC producer of the emitter was not traced in this session
(the draw trace names the texture handle, not the guest function).

### 5.4 Controlled experiments on the smoke (locked clocks, 5K, 25 s, frames 1200-1260)

| experiment (one variable) | scene pass max / p50 (ms) | GPU max (ms) | note |
|---|---|---|---|
| baseline, locked (`s5k-lock-1`, `-2`) | 11.01 / 6.27; 11.17 / 6.27 | 11.96, 11.95 | repeatable to 0.01-0.16 ms |
| particles without texture sample (probe 1, `s5k-lock-p1`) | 12.62 / 7.08 | 13.49 | not cheaper |
| particles without blending (probe 2, `s5k-lock-p2`) | 12.07 / 6.88 | 13.03 | not cheaper |
| particles without depth test (probe 4, `s5k-lock-p4`) | 13.67 / 9.23 | 14.28 | more expensive |
| per-draw timestamps on (`s5k-lock-draws`) | 11.69 / 6.74 | 12.30 | +6 % measurement cost |

The probes change the picture (diagnostic switches, off in production) and none lowers the scene pass: the batch is not
limited by sampling, blending or depth test; it is the area covered (one batch of 60 quads can cover the whole screen,
GPU_FILL.md section 3b), consistent with fill. Resolution A/B (4.2): 480p 0.40 ms, 1080p 1.61 ms, 5K 11.01 ms.

### 5.5 Driver clocks: A/B and sampled trajectory

Same replay, same frames, one variable (`SHADOW_R2_STABLE_POWER=1`):

| | GPU p50 | GPU max (smoke window) | frames > 16.67 | missed vblanks |
|---|---|---|---|---|
| driver clocks, 5K, 17 runs | 5.7-7.4 | 15.6-36.2 | 0-18 per run | 1-51 per 86 s run |
| locked (2205 / 11201 MHz) | 2.05 (25 s), 2.70 (86 s) | 11.96 | 0 | 0 |

NVML samples joined to GPU intervals (`clkjoin.py`; `fs5k-clk`, `u5k-clk-1`, `s5k-smp-5`): over frames 1205-1260 the GPU
ran at 510-900 MHz graphics and **5001 MHz memory (P3, half the memory clock)** for the first part of the window
(frames 1205-1230, GPU 7.4-14.3 ms), then 1650-1800 MHz and (`fs5k-clk`, `s5k-smp-5`) 11201 MHz (P0) after frame ~1236
(GPU 8.4-13.8 ms for the window's frames). Median graphics clock over the 5,114 frames of `fs5k-clk` is 540 MHz (p95 780);
GPU time per frame by clock bin: 250-499 MHz p50 5.90, 500-749 MHz 5.76, 750-999 MHz 5.16,
1750-1999 MHz 10.94 (the heavy frames come late, when the clock has ramped, so the bins mix content and clock; the
clean evidence is the locked/unlocked pair above). **Not captured:** the clock trajectory during a 25-36 ms spike-mode
frame (all sampled runs were typical mode). The second heavy region, ticks 3917-3947 in `fs5k-b`, shows the size of the
effect: the scene is a light 2.62 ms (locked: 1.39 ms scene, GPU 2.57 ms), yet GPU time was 20-23 ms and 16 frames
were late; at the same ticks `fs5k-a`, `fs5k-clk`, `L5k-lock` give 6.05, 6.62, 2.72 ms.

### 5.6 Spike block: the smoke

```
Frame                      1229 (also the cluster 1205-1260)          [run fs5k-a, 5120x2880 fullscreen, driver clocks]
Game time                  20.483 s (cluster 20.1-21.0 s)
Measured latency           GPU 17.13 ms; displayed interval 33.33 ms (ticks 1218, 1227, 1229); latch->display 50.67 ms
CPU game-thread            2.05 ms
CPU render-thread          0.457 ms (record+submit)
GPU execution              17.13 ms: scene 12.45, skin 0.006, glare 0.03-0.05 per pass, resolve/compose/scale ~0.5 (gpu.csv passes_detail)
Synchronization-waiting    swap-chain wait 27.56 ms (the previous frame was shown a refresh late); GPU start delay 0.3 ms
Streaming activity         none: whole run 2,342 file reads, 15.2 MiB, 6.6 ms total, worst read 0.4 ms (out.log "Assets:")
Responsible function(s)    Renderer::record particle draw loop (renderer.cpp ~1928-1960), particle pass shader; 6 batches of 60 particles, texture 0x010000F1
Original PPC address(es)   not traced (the draw trace names the texture handle; the emitter's guest function was not identified)
Root cause                 GPU fill of overlapping full-screen smoke quads at 5120x2880 (6-7 ms at locked clocks, 84 % of the scene pass),
                           run at the driver's P3 state (memory 5001 MHz) for 3x the cost; the budget is ~16.1 ms, not 16.67
Supporting evidence        per-draw trace (5.3); resolution ladder 0.40 / 1.61 / 11.01 ms (4.2); probes do not help (5.4)
Controlled experiment      STABLE_POWER=1: same frames max 11.96 ms, 0 late; 5 runs unlocked same frames max 15.8-36.2 ms
Measured contribution      all gameplay frames > 16.67 ms in the 86 s runs (1 in fs5k-a); 100 % of fs5k-b's 33
Recommended correction     hold the GPU clock for the game (section 9); then reduce the smoke fill (resolution of the particle pass or
                           exact overdraw trimming, GPU_FILL.md 3b)
Confidence                 high for the cause of the content cost and for locked-vs-driver; medium for the exact clock mechanism
```

### 5.7 Spike block: driver spike mode (the 25-36 ms frames)

```
Frame                      1216-1248 (17 frames) and 3917-3947 (16 frames)     [run fs5k-b]
Game time                  20.27-20.8 s and 65.3-65.8 s
Measured latency           GPU 16.7-30.5 ms (smoke), 19.8-23.0 ms (second region); displayed interval 33.33 ms on every one
CPU game-thread            1.6-2.4 ms
CPU render-thread          0.25-0.57 ms
GPU execution              scene 8.8-23.4 ms in the smoke window (locked 6.3-11.0); 5.0-11.9 ms in the second region where locked is 1.4-1.8
Synchronization-waiting    swap-chain wait 17.3 ms on every other frame (pairs of late frames)
Streaming activity         none
Responsible function(s)    none in the game: GPU / driver power state (NVIDIA, "GPU idle" throttle reason in sampled runs)
Original PPC address(es)   n/a
Root cause                 the GPU executed the same work 3-9x slower than at locked clocks; the clock state is the driver's
Supporting evidence        4 of 10 unsampled unlocked 5K runs; none locked (0 of 6 locked 5K runs, max 12.30 ms); same ticks 2.6-2.7 ms locked and 4.9-6.6 ms in other unlocked runs
Controlled experiment      lock the clocks: removes it (5.5)
Measured contribution      up to 51 missed vblanks and 33 frames over 16.67 ms in one 86 s run (fs5k-b)
Recommended correction     as 5.6; add a clock-state field to the shipped telemetry so the next report can name the state
Confidence                 high that it is not game work; medium on the mechanism (trajectory not captured in spike mode)
```

## 6. The other spikes

### 6.1 Render-thread one-time resource creation (mechanism 2), measured by direct timing

`GEKKO_INIT_TRACE` (section 8) times each `CreateGraphicsPipelineState`, texture, placed texture, render-target heap
(`AllocateMemory`) and buffer creation and each `Renderer::record` / submit with the calling thread. For the render-thread
frames over 3 ms (`initattr.py`; 5K = `j5k-unl-1`, `j5k-unl-2`, same numbers in `i5k-*`):

| render-thread frame | record+submit | what it contained (render thread) | same frame at 1080p / 480p |
|---|---|---|---|
| first frame, game time 0.02 s | 24.8-24.9 ms | `shadow.render_target_memory` heap 474 MB **10.9 ms**; `view depth` 5120x2880 texture **5.8 ms**; `output` texture 1.6 ms; a 120 MB heap 3.2 ms; `shadow.asset_texture` 3.1 ms; pipelines 0.9 ms | 8.7 (heap 2.5) / 7.9 (heap 1.1) ms |
| first gameplay frame, 2.250 s | 14.2-14.4 ms (13.0-13.2 locked) | 239 MB heap **6.7-6.9 ms**; 12 mesh / glare pipelines 2.4-2.5 ms; 7 textures 1.25 ms; ~3.5-4 ms of other recording | 7.7 (heap 1.0) / 7.0 (heap 0.7) ms |
| 3.15 s (game tick 189) | 3.9-4.0 ms | `shadow.view_history` texture 14.7 Mpx **3.65 ms** | 1.0-1.4 ms |

Totals over a run at 5K: 28 pipelines 5.1 ms, 356 textures 20.8 ms (at 1080p 8.6 ms, 480p 7.3), 3 heaps 20.8 ms
(1080p 3.5, 480p 1.7), 88 buffers 21-22 ms (boot, independent of resolution). Over the 86 s replay the render thread's
`record` scope averages 0.29 ms and submit 0.08 ms per frame (1,452 frames in a 25 s run: 416.7 and 117.8 ms total), so
these three events are the only render-thread CPU events above 3 ms in the run (all other frames: max 1.0-3.0 ms, listed
in `perframe.csv`). Code: `TexturePool::acquire` allocates a committed heap on first request (frame_graph.cpp 296-335
-> `allocate_target_memory`, resources.cpp:76), `create_texture_2d` (resources.cpp:113), `Renderer::mesh_pipeline`
-> `create_graphics_pipeline` (pipelines.cpp:91), view history (renderer.cpp ~1655-1672).

Visibility: at 5K the first gameplay frame is shown one refresh late (displayed interval 33.33 ms at tick 135) in 22 of
30 5K runs (e.g. `fs5k-a`, `fs5k-clk`, `i5k-lock-1`, `j5k-*`, `u5k-clk-*`, `s5k-smp-*`; the other 8 got a
favourable push phase), and never at 480p, 1080p, 1440p, 2133x1200 or 3840x1200 (record 7.0-9.3 ms).

```
Frame                      135 (first gameplay frame; packet sim frame 135.0)   [run fs5k-a, 5K; typical of 22/30 5K runs]
Game time                  2.250 s
Measured latency           displayed interval 33.33 ms (1 missed vblank); render record+submit 21.02 ms in the cold run, 13.0-14.5 ms warm
CPU game-thread            10.2 ms in the cold run (fs5k-a), 3.0-3.4 ms warm (r5k-base, r1080-ss)
CPU render-thread          21.02 ms (fs5k-a); 14.2-14.4 ms (j5k-unl); 12.7-13.2 locked
GPU execution              6.26 ms (3.2 locked)
Synchronization-waiting    swap-chain wait 6.9 ms (cold run)
Streaming activity         resource worker: 533 jobs, busy 157.6 ms over the run, waits 0, "draws recorded while their resource was made" 0
Responsible function(s)    TexturePool::acquire -> allocate_target_memory -> D3D12MA AllocateMemory (6.7-6.9 ms), create_graphics_pipeline (12 x 0.2 ms), create_texture_2d
Original PPC address(es)   none: host renderer code (the guest only asks for the first view)
Root cause                 lazy creation of render-size-proportional GPU memory on the render thread during the first presented frame
Supporting evidence        init.csv rows inside the frame; same frame 7.7 ms (1080p) / 7.0 ms (480p) with heap 1.0 / 0.7 ms
Controlled experiment      resolution change: heap cost 0.7 / 1.0 / 6.7 ms at 480p / 1080p / 5K
Measured contribution      14.4 - 7.7 = 6.7 ms of the 14.4 ms at 5K is the heap; PSO 2.4 ms is resolution independent
Recommended correction     create the pool memory, depth, output and history at renderer start or on the resource worker during the loading screen
Confidence                 high (direct timing inside the frame)
```

The first frame (tick 1) and tick 189 follow the same pattern. Tick 189: render 3.7-4.8 ms, `shadow.view_history` 3.6-3.7 ms, 5K
only; displayed on time in every run. The first frame is shown at game time 0.02 s before the logos and is not
visible as a stall.

### 6.2 GPU start latency and late presentation on light frames (mechanism 4)

Submit-to-GPU-begin delay (`qdelay.py`, frames >= 300):

| run | clocks | p50 | p99 | max | frames > 2 ms |
|---|---|---|---|---|---|
| `L5k-lock`, `Lw32x10-lock`, `s480-lock`, `s1080-lock`, `s1440-lock`, `s2133x1200-lock` | locked | 0.02 | 0.22-0.28 | 0.4-0.5 | 0 |
| `fs5k-a`, `fs5k-clk`, `u5k-clk-1` | driver | 0.02-0.03 | 0.39-0.50 | 0.8-0.9 | 0 |
| `fs5k-b` | driver | 0.07 | 0.74 | 2.1 | 5 |
| `w32x10-a/b` | driver | 0.33 | 2.4-2.8 | 4.8 | 78 / 117 |
| `fs2133x1200-a/b` | driver | 0.51-0.60 | 3.4 | 5.2 / **20.9** | 243 / 207 |
| `fs480-a` | driver | 0.60 | 3.45 | **16.8** | 292 |

The lighter the GPU load, the later the GPU starts a submitted frame (the 5K runs keep the GPU busy; 480p and 2133x1200
do not), and locking the clocks removes it. Classifying every displayed interval >= 25 ms of the unlocked light runs
(ticks >= 300): `fs2133x1200-a` 14 frames of GPU >= 16.0 ms (the smoke at low clocks), 1 with start delay >= 2 ms; `fs2133x1200-b`
2 with GPU >= 16.0, 4 with start delay >= 2 ms and GPU + delay >= 15 ms, 3 with GPU < 1 ms (ticks 1850-1895, below), 3 other;
`w32x10-a` 10 frames of GPU 15.6-15.9 ms (frames 1338-1362, the second smoke wave; locked the same window is <= 3.2 ms,
so a 5x slowdown at low clocks), 2 >= 16.0; `fs480-a` 6 misses with GPU 1.6-8.0 ms and start delay 0.6-3.0 ms. The last
group is "after_gpu": the GPU finished more than a refresh before the vblank (tick 3563: GPU end -28.3 ms, shown at 0) and the
present was still shown one refresh late (frame_telemetry_report class; 1-7 per unlocked 86 s run, none in a locked run).
Present() itself took up to 9.1 ms once (`fs480-a` tick 1688, 8.7 ms in `w32x10-b`), not tied to a miss. I did not
identify the compositor / driver mechanism behind "after_gpu"; it is outside the game's CPU and GPU work. Frames with
tiny GPU time and a 21 ms start delay:

```
Frame                      1850 (and 1855, 1892, 1895)               [run fs2133x1200-b, driver clocks]
Game time                  30.83 s
Measured latency           displayed interval 49.99 ms (1850), 66.66 ms (1855); GPU 0.385 / 0.369 ms
CPU game-thread            2.8 / 4.4 ms
CPU render-thread          0.46 ms
GPU execution              0.385 ms of work; GPU began executing 21.4 ms after submit (record -49.46, GPU begin -28.54 vs the vblank)
Synchronization-waiting    GPU queue / start delay 21.4 ms; no CPU wait
Streaming activity         none
Responsible function(s)    none in the game; GPU scheduling / power-state wake (driver, WDDM)
Original PPC address(es)   n/a
Root cause                 the GPU was at an idle / low clock state and started the queued command list late (mechanism not identified further)
Supporting evidence        qdelay.py table above; all locked runs have start delay max 0.5 ms
Controlled experiment      STABLE_POWER=1: delay max 0.5 ms, 0 displayed misses at every resolution (L5k-lock, Lw32x10-lock, s480/1080/1440/2133-lock)
Measured contribution      fs2133x1200-b: 3 misses with GPU < 1 ms plus 4 with start delay >= 2 ms (of 12); fs480-a 6 of 6 misses have GPU <= 8 ms
Recommended correction     as 5.6 (clock policy); not fixable in the game's frame work
Confidence                 high that the delay is outside the game's CPU and GPU work; low on the exact mechanism
```

### 6.3 Stage hand-off game thread (mechanism 3)

fw quanta (5 ms of game time each) with the pacer sleeps removed, warm rendered runs (`s5k-lock-1`, `j5k-unl-1`,
`j1080-lock`, `w32x10-a`, `s480-lock`): limiter entries 33 / 34 = 6.7-8.0 / 13.3-15.3 ms; game-thread work for game
frame 79 (sum of the quanta) 21.9-25.4 ms (every resolution: 24.7 ms at 5K and 24.0 ms at 1080p in `r5k-base`/`r1080-ss`). The
first cold run (`fs5k-a`): 21.1 + 15.9 ms, work 40.1 ms; push gap 38.5 ms. The packet queue absorbs it: the displayed
interval stays 16.67 ms at ticks 79-81 in every run. Headless (`h-base-1..5`): 20.2-21.9 ms (mean 20.8) for the two
entries; with `GEKKO_PRS_AHEAD=0` (`h-noprs-*`) 23.1-26.1 ms (mean 24.4): PRS-ahead saves 3.6 ms net.

Attribution, stack samples of those two entries (`GEKKO_SELF_SAMPLE`, effective interval ~0.7 ms, so n is small;
`h-ss-1..3` 88 samples, `r1080-ss-*` / `r5k-ss-*` 168 samples; `tools/self_sample_report.py`, link map of exe3):

| inclusive share | function | what it is |
|---|---|---|
| 33-53 % / 29-47 % | fn_8006B1B8 / fn_8006A3C0 | stage resource loader chain (0x8006A3C0 = ResourceOneFileReq "other members" per docs/STAGE_LOADING.md; 0x8006B1B8 not named in the docs) |
| 21-32 % | fn_8004BB4C / fn_8004BBD4 | TOneFileAsync<RpClump> loader (docs/STAGE_LOADING.md) |
| 19 % (incl. `copy_asset` 17-19 %) | NativeRtGcnFs lambdas -> NativeFileApi::device_read -> copy_asset | host file API copying archive bytes |
| 19-26 % / 14-18 % / 14-16 % | fn_804586E8 / fn_80456F68 / fn_8045AA18 | RenderWare stream deserialisation (geometry / materials) |
| 12 % | fn_800149F4 -> fn_80013C30 | ResourceOneFile |
| 10-15 % (exclusive 7-14 %) | `gekko::PrsAhead::announce` | host: parses the member table and queues decodes on the game thread |
| 9-11 % | fn_8004BF68 / fn_8004BFF0, fn_8004C770 | RwTexDictionary loader; "extract" (PRS plus memcpy) |

```
Frame                      79 (stage hand-off; limiter entries 32-34)    [warm: j5k-unl-1; cold: fs5k-a]
Game time                  1.317-1.32 s (PlayOnce handed over at 1.267 s)
Measured latency           game-thread work 21.9-25.4 ms (warm), 40.1 ms (cold); push gap 38.5 ms (cold); no displayed miss
CPU game-thread            entries 33/34: 6.7-8.0 / 13.3-15.3 ms warm; 21.1 / 15.9 ms cold
CPU render-thread          0.2 ms
GPU execution              0.93 ms
Synchronization-waiting    none (loading runs on the game thread; yields are native)
Streaming activity         file reads inside this window (all assets of the stage), PRS expands: 606 served ahead in the run (1.0 ms total), 6 decoded on the game thread (2.5 ms)
Responsible function(s)    fn_8006B1B8 / fn_8006A3C0 / fn_8004BB4C / fn_804586E8 / fn_80456F68 / fn_8045AA18 / fn_800149F4; host NativeFileApi::copy_asset, PrsAhead::announce
Original PPC address(es)   0x8006B1B8, 0x8006A3C0, 0x8004BB4C, 0x8004BBD4, 0x8004BF68, 0x8004BFF0, 0x8004C770, 0x800149F4, 0x80013C30, 0x804586E8, 0x80456F68, 0x8045AA18, 0x80481D9C
Root cause                 computation: the stage's whole resource set is built in one frame by translated RenderWare loaders
Supporting evidence        stack samples; PRS A/B 20.8 vs 24.4 ms headless (n=5 / n=4)
Controlled experiment      GEKKO_PRS_AHEAD=0: +3.6 ms; sampler on: +3.0 ms
Measured contribution      deterministic ~21 ms (2.5 ms of it PrsAhead::announce bookkeeping), none displayed
Recommended correction     spread the work over the loading screen (STAGE_LOADING.md native loaders); queue PRS-ahead work off the game thread
Confidence                 medium (88-168 samples, mechanism per function clear, shares +-10 points)
```

### 6.4 Boot cadence is not a latency spike (measurement artifact)

Every run shows displayed intervals of 33.3 ms at ticks 6, 15, 22, 28, 83.3 ms at 33 and 616.7 ms at 70. These are game-time
steps, not stalls: the boot packets are 20.0 ms apart in game ticks (1.2 frames; the 50 Hz boot logo content), then 80.0 and
606.7 ms apart (the game draws nothing in between: packets 26-27 of `fs5k-a`), and the display shows each at the next vblank.
Frames below tick 70 are excluded from the gameplay tables and listed in section 7.

### 6.5 Cold-run stalls (inference, one run)

The first run of the session (`fs5k-a`) logged three `[host-stall]` game-thread events during boot (22.9 ms at
guest pc 0x801D84C0, last file `sound/AudioDataPlayerDark.bin`; 33.0 ms at 0x801E2320, `common/CommonEffect.one`; 22.0 ms at
0x801E2320, `csdFiles/InfoWindow.gncp`; each "native execution ... while game time advanced 5.0 ms", io_wait 0.24-0.30 ms).
None of the other runs without the stack sampler (warm) logged any; the four rendered sampler runs (`r1080-ss-*`, `r5k-ss-*`) logged one each (26-30 ms at guest pc 0x803788FC, no file, io_wait 0: the hand-off window lengthened by the sampler). The files are copied from mapped host files by the native file API, so a first
touch of a not-yet-cached file is the likely cause, but I could not flush the OS file cache here (no elevation) and did not
prove it. The same cold run is also the one with 40 ms hand-off work and 21 ms record at 135 (versus 22-25 and 13-14.5 ms
warm). A cold user launch will see these.

### 6.6 Game-thread gameplay outliers (not > 16.67 ms)

Over all 86 s rendered runs the game thread peaks at 5.5-7.9 ms on scattered frames; the repeated ones sit at the same
ticks in every run (3577-3636, 3747-3748: ~5.7-6.3 ms), so they are deterministic game logic; one run has 11.0 ms (tick 4588,
`w32x10-b`) and one 8.2 ms (tick 4356, `fs480-a`), both single frames not repeated in the other runs (intermittent,
unattributed: no sampler was attached to those runs). None is near 16.67 ms or visible.

## 7. Every frame over 16.67 ms

Raw list for the 13 full runs: `.build/latency/frames_over_16.67.csv` (213 rows: any of GPU, game-thread work, render
CPU > 16.667 ms, or displayed interval >= 25 ms). Cause classes, all from sections 5-6:

| region | frames | metric | cause |
|---|---|---|---|
| first frame (tick 1) | 1 per run | render CPU 25.9 ms (5K), 8-12 ms others | 6.1 |
| boot / title, ticks 6-70 | 5-7 per run | displayed interval only | game cadence, 6.4 (not latency) |
| stage hand-off, tick 79 | 1 | game-thread work 22-25 ms (40.1 cold); `fs5k-a` row | 6.3 |
| first gameplay frame, tick 135 | 22 of 30 5K runs | render CPU 13-21 ms, displayed 33.33 ms; GPU 3-11 ms | 6.1 |
| smoke, ticks 1205-1260 | 0-18 per run | GPU 16.1-36.2 ms, displayed 33.33 ms | 5 |
| second heavy region, ticks 3917-3947 | 16 (`fs5k-b` only) | GPU 20-23 ms | 5.7 |
| light-scene GPU start delay, various ticks | 0-12 per run | displayed 33-67 ms, GPU 0.3-14 ms | 6.2 |

`fs5k-a` in full (12 rows, every frame over the line in any metric): tick 1 (render 25.9 ms), ticks 6, 15, 22, 28, 33, 70
(boot cadence), 79 (game 40.1 ms), 135 (render 21.0 ms + display 33.33), 1218 (GPU 16.13, display 33.33), 1227 (16.01, 33.33),
1229 (GPU 17.13, 33.33). Gameplay frames 300-5116 in that run: 1 with GPU > 16.67, 3 displayed late, 0 with CPU > 7 ms.

## 8. Instrumentation added (all uncommitted, env-gated, off by default)

* `runtime/lift/render/init_trace.h` (new): `GEKKO_INIT_TRACE=<csv>` records begin/end ns (steady_clock), thread, kind, bytes,
  label for pipeline, texture, buffer, placed texture, heap, upload-ring wait, `record`, `submit`. One relaxed atomic load per
  site when off; compiled out under `GEKKO_SHIPPING`.
* `runtime/lift/render/pipelines.cpp`, `resources.cpp`, `renderer.cpp`: one `init_trace::Scope` line at each of those
  sites (renderer.cpp: around `record(...)` and `frames_.submit()` in `Renderer::render`, plus the include).
* `tools/latency/*`: the runner (`run3.sh`) and the analysis scripts used here.

Not touched: native_one.cpp, native_expand_jobs.cpp, native_rtgcnfs.cpp and their tests. The build was `cmake --build
.build/loadfix --config Release --target ShadowTheHedgehog --parallel 2` (twice); `.build/loadfix/bin/Release/ShadowTheHedgehog.exe`
now holds exe3. No copy was made to `../game`.

## 9. Path to stable 60 FPS, the 3 ms target and a 1 ms GPU goal

Estimated impact (E = estimate, not measured; M = measured):

| fix | effect on the 5K 86 s run | basis |
|---|---|---|
| hold the GPU at high clocks while the game runs (user's per-program NVIDIA "Prefer maximum performance", or a game-side request that works without developer mode; `SetStablePowerState` needs developer mode and is diagnostic only) | GPU max 36.2 -> 11.96 ms, frames > 16.67 33 -> 0, missed vblanks 51 -> 0; GPU start delay max 20.9 -> 0.5 ms | M (`L5k-lock`, `Lw32x10-lock` vs unlocked) |
| pre-create the pool heaps / depth / output / history before the first presented gameplay frame | first gameplay frame record 14.4 -> ~7.7 ms at 5K (the 1080p value); removes the one missed vblank in 22 of 30 runs; tick 189 4.0 -> ~1.0 ms | E from M resolution scaling of the heap cost |
| move the stage hand-off loading off the visible frame | 22-25 ms -> within 16.67 ms | not display-visible today; E |
| reduce smoke fill by half | smoke GPU max 11.96 -> ~7-8 ms locked | E from M (84 % of draw time is that texture) |

Stable 60 FPS at 5K: needs only the first row. With it the replay had no gameplay frame over 12.0 ms GPU / 7 ms CPU and the
displayed interval never exceeded 16.67 ms (86 s, 4,860 frames, plus 25 s runs at 480p-1440p). Without it, the driver's
power policy decides: 17 unlocked 5K runs gave a GPU max of 15.6-36.2 ms. That figure cannot be fixed by game code alone;
what the game can do is reduce the window in which the GPU is idle-clocked (the lighter the frames, the more likely
the low state) or ask for the performance state.

The GPU budget is ~16.1 ms, not 16.67 ms (5.1): any plan counting GPU time must subtract the ~0.5 ms record plus GPU-start gap.

3 ms intermediate target (locked clocks, 4,860 gameplay frames): 3840x1200 GPU p50 0.73 ms, 0.2 % of frames over 3 ms, max
3.18 ms: **realistic** on the GPU there. 5120x2880: GPU p50 2.70, 22.8 % of frames over 3 ms, p99 6.98, max 11.96 ms (0.7 % over
8 ms): **not realistic** without cutting the smoke fill and the tail of the scene pass (the smoke costs 0.75 ms per megapixel, 4.2). The game thread alone misses 3 ms: work p50 2.69-2.73 ms, 30-32 % of frames over 3 ms, p99
3.98-4.00 ms, max 5.65-6.21 ms. A 3 ms frame is therefore not reachable on the CPU side without the native-player and
RenderWare-loader work the repository already plans (docs/NATIVE_THREADS.md); do not promise it for CPU or for 5K GPU.

1 ms GPU-frame goal: at 3840x1200 locked, p50 0.73, p90 0.83, p99 1.83 (2.5 % of frames over 1 ms, max 3.18; all over-1 ms frames
are the smoke). At 5K it would need roughly 2.7x less work at the median and ~7-12x less in the smoke frames, i.e. a different
rendering approach for the particle and scene passes (lower-resolution particle pass, tiling), none of which this session tested.
Not promised at 5K. Nothing here shows a 1 ms goal on the CPU side: the game thread median is 2.7 ms.

## 10. Prioritised remediation plan

1. GPU clock policy (mechanism 1, all driver-state amplification, GPU start latency): the largest measured effect. Decide with the
   user between the NVIDIA per-program profile and a supported application request; add the clock state (NVML, 1 s) to the
   shipped diagnostics so a user report can say which state a late frame ran in.
2. Pre-allocate render-size-proportional GPU memory before gameplay (mechanism 2): `TexturePool` initial heaps for the views,
   `view depth`, `output`, `shadow.view_history`, and the 12-28 pipeline state objects (pipelines are resolution independent,
   2.4-5.1 ms total). Verify with `GEKKO_INIT_TRACE` that `record` at the first gameplay frame is < 8 ms at 5K.
3. Smoke fill (mechanism 1, content): per GPU_FILL.md 3b options (particle pass at lower resolution or exact trimming); each
   changes pixels or saves little exactly, so it is the user's decision.
4. Stage hand-off (mechanism 3): move PRS-ahead's `announce` and the RenderWare stream builders off the first game frame.
5. Cold-start stalls: warm the asset files that the first seconds read (inference only, 6.5).
6. End-to-end latency: input latch to display is 20-38 ms and varies by boot (the push phase relative to vblank; docs/TIMING_ARCHITECTURE.md
   "next on this line"). Not a spike; listed for completeness.

## 11. Confidence, uncertainties, not measured

* The origin of the user's "17+ ms" is not established (section 1). The match to the smoke-window GPU max is by value.
* Spike-mode clock trajectory not captured; NVML-sampled runs happened to stay in typical mode (0 of 7 vs 4 of 10, p = 0.09).
* Which draw state of texture 0x010000F1 is dominant (area) rests on the earlier GPU_FILL.md analysis and on the probes failing to
  lower the cost; fragment counts were not measured. The PPC emitter was not traced.
* GPU start delay: the mechanism (power-state wake, WDDM scheduling, DWM) was not identified.
* Cold-run stalls and the 40 ms cold hand-off: one cold run; no cache flush available.
* Hand-off stack shares: 88-168 samples at an effective ~0.7 ms interval.
* Game-thread outliers (11.0 ms at tick 4588, 8.2 ms at 4356): no sampler on those runs; cause unattributed.
* No GPU-side per-pass trace of the second heavy region (ticks 3917-3947) with locked clocks beyond the 86 s `L5k-lock`
  (scene 1.39 ms median there) and no draw trace in spike mode.
* CPU package power, C-state residency, presentation mode (the swap chain refuses the media interface) are not recorded.
* Streaming/decompression events per gameplay frame: only run totals exist (2,342 reads, 6.6 ms, resource worker waits 0,
  PRS expands); a per-frame file-read log does not exist in this build.
* Exe provenance: exe2 / exe3 include the other session's edits at link time (section 2).
