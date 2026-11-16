# What ShadowRecomp does, and what it means for us (2026-11-16)

ShadowRecomp (FoxWorkshop/ShadowRecomp) is a static recompilation of the **Xbox** Shadow the Hedgehog: x86 code to C++
with their XboxRecomp2, and a D3D12/Vulkan renderer derived from UnleashedRecomp's. It is the same game and engine as
ours. Their code is GPL-3.0: we take ideas and reasoning, never code. Their addresses are the Xbox build's and are
useless to us.

This page collects what a read of their repository found (three research passes over the code: GPU, CPU/recompiler,
pacing/loading/audio) and checks each idea against our port. The work it led to is listed at the end, with what to
verify once a machine with the disc is available.

## 1. Their measured numbers (the only ones in their repository)

- **Recompiler options, all on: +12.5 % ± 1.3 %.** Stage 0201, 20 s idle, 8 runs (XboxRecomp2
  `doc/docs/optimizations/index.md`). Largest single options: x87 stack analysis +3.8 %, dead-flag elimination
  +3.0 %, compare+branch fusion +1.2 %, `rep movs` as memmove +1.1 %. Most single options are within noise; the
  author says so.
- **Specialized fixed-function shaders instead of one ubershader: 60 → 300 FPS** on one full-screen torch overlay in
  Cryptic Castle. A branchy pixel shader under transparent overdraw. 338 observed pixel states collapse to 75
  programs (XShaderRecomp `fixed_function/README.md`).
- **A 3-float vector length: "~7 % of the game thread in 2P".** The native version went 313 → 298 fps the wrong way
  with f32 and was left commented out (`patches/vector_patches.cpp`).
- The README's ">4x performance" credit has **no numbers** behind it in the repository.

## 2. Each idea against our port

| idea (their file) | ours | verdict |
|---|---|---|
| Absolute-grid frame deadlines, no per-frame rounding drift (`fps_patches.cpp` sub_56FC0) | one time base, frame k at k × ticks / hz (FRAME_PACING.md) | **have it** |
| High-resolution waitable timer, sleep then yield-spin (`frame_pacing/`) | `RecoveredClockSleeper`, spin when V-Sync is off (shadow_main.cpp ~480-530) | **have it** |
| Raise timer resolution (`NtSetTimerResolution`; Linux `PR_SET_TIMERSLACK` 50 µs) | not done ("no timeBeginPeriod anywhere", TIMING_ARCHITECTURE.md) | **cheap, try**: measure wake overshoot before / after |
| Game thread / render thread split, bulk command queue | packets to a present thread (packet_queue.h) | **have it** |
| Adaptive present wait (game blocks on the render thread only when GPU-bound or V-Sync) | burst-ahead / phase lock (burst_ahead.h, phase_lock.h) | have an equivalent |
| Recorded pipeline cache, replayed at start; canonical keys; last-PSO shortcut (`pipeline.cpp`, `video.cpp` 962-1233) | was: made at first draw | **done** (docs/PIPELINE_CACHE.md) |
| Specialized shaders per material state (XShaderRecomp) | `mesh.hlsl` branches on per-draw flags (uniform over a draw, so cheap) | **only if** a GPU capture shows the mesh pixel shader is the cost at 1440p / 2160p |
| Barrier batching, redundant ones dropped (`video.cpp` 704-790) | frame graph: tracked states, one batch per pass | **have it** |
| Upload ring per frame, 256-byte constant blocks (`video.cpp` 519, 6015) | `frames_.ring()`, one 256-byte `DrawConstants` per draw | **have it** |
| Constant dirty tracking (`vertexshader.cpp` 724-790) | per-submesh rebinds of VB / IB / lights; render thread 0.35-0.45 ms | **not worth it** (a few hundredths of a ms) |
| Geometry hashed, converted and uploaded once (`vertexbuffer.cpp`) | each model / sector made once, handle kept on the game's object (rw_driver/pipelines.cpp 435, 1838) | **have it** |
| Bindless textures (`shared_constants.h`) | bindless heaps, one root signature | **have it** |
| Non-volatile guest memory (XboxRecomp2 `nonvolatile_memory`) | every access volatile (cpu.h 1289) | **not viable**: our flat memory map's fault handler decodes only the instruction forms volatile accesses compile to (flat_memory.cpp 25-40); the device window, journal and poll watch rely on every access happening |
| Dead-flag elimination (`eliminate_dead_flags`) | compare+branch fusion done (CPU-18); dead CR0 / XER[CA] updates still written | **blocked**: `--verify-recomp` compares the whole CR / XER (cpu.cpp 2448); needs a decision to mask ABI-volatile fields, and the payoff on PowerPC should be measured first (record forms are few, and usually read at once) |
| Clang `-mllvm` analysis limits raised for huge generated functions (`ShadowRecompLib/CMakeLists.txt` 12-40) | MSVC | only if the shards ever move to clang-cl |
| x87 stack in locals, `rep movs`, `idiv` after `cdq` | — | x86-only |
| O(1) allocator (o1heap) replacing the game's malloc (`kernel/heap.cpp`) | the game's own heaps (MEMORY_ALLOCATORS.md) | candidate; they give no number |
| Lock elision for single-thread heap locks (`single_thread_locks_from`) | — | for us: `lwarx` / `stwcx.` loops; check the profile first |
| 512-frame WASAPI buffer "for weapon SFX timing" (`audio/driver/sdl3_driver.cpp`) | event-driven WASAPI, ~10 ms period | probably fine already |
| Sound-effect slots 32 → 59 (`audio_patches.cpp` 643-740) | the GameCube SE manager (SE_MANAGER_MAP.md) | **check** if effects drop in busy scenes; their reason is the Xbox's 64 channels |
| Bank loop-point fixes (`audio_patches.cpp` 351-600) | — | Xbox data bugs; they copied the fix from the GameCube data, which we already have |
| Quick boot to the title, `--l` / `--e` to boot into a stage or event (`start_patches.cpp`) | `--boot-stage` | have most of it |
| Executable preload with `VirtualLock` (`preload_executable.cpp`) | — | startup page faults only; low value |
| File I/O | — | theirs is plain `fstream`, no cache: ours is ahead |
| High frame rate: ~100 per-object fixes (`fps_patches.cpp`) | — | **foundation done**; the fixes are a checklist (docs/HIGH_FRAME_RATE.md), each described in docs/HIGH_FRAME_RATE_FIXES.md |

## 3. Where our time goes (docs/PERF_60FPS_2026-11-04.md)

- **Game thread:** about 2.7 ms per frame at p90. Translated game code 2.11 ms, other runtime 0.52, audio mix 0.44,
  dispatch 0.24.
- **Render thread:** 0.35-0.45 ms p50.
- **GPU:** 1.5 ms at 480p up to 5.7 ms p50 at 2160p. The scene pass dominates from 1080p up, and 2160p shows about 13
  repeated frames in 75 s from 12-22 ms GPU frames.

So the remaining big levers are the scene pass at high resolution and the hottest translated functions. Neither of the
ideas that fits ShadowRecomp's design (non-volatile memory, dead flags) fits ours as it stands.

## 4. Done in this pass (branch `claude/hud-alignment-review-47wszw`)

- `f32f002`: opt-in high frame rate, 120 / 180 / 240 Hz (`--frame-rate`, display.cfg `frame_rate`).
  docs/HIGH_FRAME_RATE.md.
- `b27cb07`: pipeline cache. docs/PIPELINE_CACHE.md.
- Runtime tests build on Linux again (voice archive types). `gekko_core_tests` takes name filters.

## 5. To do at home (Windows, the disc)

1. **Build.** `Build.cmd`. Nothing in this pass was compiled with MSVC; the D3D12 code was compile-checked with the
   Windows headers under MinGW only.
2. **Tests.** `ctest` in the tests tree. Expect `pipeline_cache_tests` and the six new frame-rate tests in
   `gekko_core_tests` to pass.
3. **Pipeline cache.**
   - Start twice. The second log should show `[render] pipeline cache: N of M pipelines made in X ms` and a
     `pipeline.cache` file beside display.cfg.
   - The first explosion or menu of a stage should no longer hitch.
   - A/B with `SHADOW_R2_PIPELINE_CACHE=0`.
4. **High frame rate.** `Build.cmd -Run --boot-stage stg0100 --frame-rate 120`, then the checks in
   docs/HIGH_FRAME_RATE.md "To verify in the game":
   - game speed against a stopwatch;
   - anything counting retraces;
   - ADX and Sofdec;
   - events dropping to 60;
   - the per-object list.
5. **Before any more performance work, measure:**
   - a GPU capture (PIX) of the 2160p scene pass on a heavy stage, to see whether the mesh pixel shader or the fill is
     the cost;
   - a sampling profile of the game thread on the Westopolis tape (`tools/native_sample.py`), to rank the hottest
     translated functions.
6. **Decisions only you can make:**
   - Whether `--verify-recomp` may ignore the ABI-volatile CR fields (CR0, CR1, CR5-7) and XER[CA] at a function's
     return. Without that, dead CR elimination is not possible.
   - If yes, a report-only mode in `ppcrecomp` can first count the dead updates on the DOL.
