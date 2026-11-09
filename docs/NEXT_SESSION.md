# Next session

## START HERE FIRST: native player reconstruction (handoff 2026-11-03, late)

Session: "Shadow player system reconstruction". Nothing committed or deployed; all work is uncommitted in this
worktree plus `.build/` outputs. Run game instances one at a time; never stop a game the user is playing.

**Continue here (in order):**
1. **Ask the user for the pending decisions** (they are theirs; nothing waits silently):
   - batch 01, the native movement core: approve start? Its frC readiness is in the audit doc, 12.
   - the stage loading / streaming work (linked session, below): make the switches default? Start Phase 5 (the
     rwcore RenderWare readers)? Commit the uncommitted streaming work?
2. **Player routes still missing:**
   - Ottotto: the user plays the test stage, walks slowly to a block edge, lets go of the stick and waits.
   - WallHang's last direction half: a hang approached from another angle, or another wall orientation.
   - The WallHang timer drop: a 2 s+ hang, neutral, from a fresh hang (the cut-based `wh_timer` did not reach it).
   - EdgeHang's climb timeout, and the WallJump wall-ahead path: a second wall within 80 units.
   - For each, launch the stage for the user as in `.build/player/userstage/play-*`: PowerShell `Start-Process`,
     their pad and display config copies, an isolated card, `--input-tape-record`, the census env vars. Then replay
     with `run_route.sh` (census) and `STRICT=1 run_verify.sh` (strict), and save the tape to `test_data/player/routes/`.
3. **Per-state coverage after any new run:** `python -B .build/player/targeted/coverage_by_state.py`. Put the newly
   covered branches back into the specs in `docs/player/states/`.
4. **Then batch 01** (once approved): the spec-first movement core (80094CFC, 80097658, 80098ABC, 80097DB4, the
   pools, the Ground chain), verified with targeted strict over its ranges on Westopolis, Final Haunt, the flat stage
   and the user's stage, plus the frC probe.

**Latest fixes and findings this session:**
- **The wall-jump stick bit 0x1B lasts one frame.** It is set by the executer's command receiver 0x800A7CE4 and
  cleared at the end of every executer step 0x800A8160. A route must hold the stick in an earlier frame than the jump
  press, or the jump takes the "no stick" branch (`WallHang_WallJump.md` addendum).
- **The census counts state-object creations, not entries.** The creator runs before CanEnter, so a creation can be
  rejected: EdgeHang 10× in session 1 was mostly rejected tests.
- **The free camera is in place.** The test stage's `stg0100_cam.dat` has 0 records. Stick input is camera-relative,
  so routes recorded under the scripted cameras no longer replay the same.
- **The 2P boot is fixed** in the linked session (below).

**Docs to read, in this order:**
1. `docs/SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md`: the plan; decisions in section 0; verifier modes in 6.1; status in 7;
   the state specs and targeted routes in 9.
2. `docs/SHADOW_PLAYER_COVERAGE_AUDIT.md`: the real inventory (2,541 functions, 82,045 instructions, all translated
   PPC, 0 native verified); frC resolution in 12.
3. `docs/SHADOW_PLAYER_TEST_STAGE.md`: the flat, boxed and user test stages (the user's `Test_stage` OBJ in the game's
   own formats); free camera; the user's recorded tapes.
4. `docs/player/states/EdgeHang.md`, `WallHang_WallJump.md` (+ addendum), `SpinDash_Dead.md`: binary-only specs, every
   claim PROVEN, INFERRED or UNKNOWN.

**Tools:**
- **Verifier.** `--verify-player` defaults to fast sampling; `--verify-player-strict` is the oracle. A scope file
  takes `address,size[,r4dead][,f1dead]` lines.
  - `tools/player/run_verify.sh` (STRICT=1, KNOWN=, CPU=, GAME=, TAPE=)
  - `tools/player/run_route.sh` (census of the 35 state creators)
  - `tools/player/route_tape.py` (cut a tape and append a script)
- **Stage.** `tools/player/make_test_stage.py ../game .build/player/teststage/game --obj ../Test_stage/Stage.obj
  --obj-offset 0 -77.8693 0 --obj-texture-png ../Test_stage/Diffuse.png`. That is the current overlay, with no
  objects and no scripted cameras.
- **Audit.** `tools/dolrecon/player_audit.py`, `player_frc.py` (+ `test_player_frc.py`), `player_r4dead.py`,
  `player_state_triggers.py`.
- **Runtime diagnostics.** The frC probe (`Cpu::note_single_multiply_frc`, `<verify.csv>.frc`) and
  `GEKKO_WORLD_COLLISION_TRACE=<n>`. Capture frames with `--headless --capture-at-tick T:file.png` (windowed runs
  never arm it).

**State:**
- **Strict coverage of player code:** about 44.4k of 82,045 instructions, 0 mismatches anywhere.
- **States with routes (of the 19 never reached by recordings):** EdgeHang and WallJump (the user's tapes in
  `test_data/player/routes/`), SpinDash (hold X), Dead (Westopolis fall route). Ottotto still needs a route.
- **frC:** batch 01 has 0 AFFECTED sites. Force25Bit is not implemented: there is no independent reference rule.
- **Open:**
  - WallHang's last direction half (0x800915D0..618) needs a hang approached differently;
  - the WallHang timer and probe-miss drops, the WallJump wall-ahead path, EdgeHang's climb timeout;
  - native CharaColli owner contract findings: r4 at 0x800C615C (proven dead), f1 at 0x800C5D20 (unproven).
- **Next:** batch 01 (movement core) awaits approval. Its readiness and the frC evidence are in the audit doc, 12.

**Linked session:** "Stage loading sequence: reverse engineer and native C++" (`docs/STAGE_LOADING.md`; 9c the cause,
9f the threads, 9g 2P / Last Story).
- **Discovery done:** the whole load is mapped and measured.
  - Long loads came from the RtGCNFS read yielding twice per 0x2000-byte chunk, each yield costing one
    loading-screen frame.
  - During loading, a background read gets the CPU back only at the audio threads' per-refresh interrupt.
- **Native background read** (user-approved): built in `.build/loadfix`, **off by default**
  (`GEKKO_NATIVE_BG_READ=1`).
  - Loading screens drop from 0.5–2.6 s to about 0.3–0.5 s; The Last Way's own load from 1.42 to 0.29 s.
  - Oracle CTest: 207 calls, 0 differences, 3 controls killed. In-game strict over the loader on Westopolis:
    10.4 M segments, 0 mismatches.
  - A/B at equal game frames: all 38 Select Mode stages identical except the player object's heap address on 20 of
    them; the 2P arenas identical, heap included.
  - Tapes recorded before the change start their input 0.4–1.6 s late relative to gameplay.
- **Native streaming replacement (2026-11-03/04; all uncommitted, every switch off by default).** Plan:
  `docs/NATIVE_STREAMING_REPLACEMENT_PLAN.md`; proof: `docs/STREAMING_DIFFERENTIAL_VALIDATION.md` §5–16; contracts:
  `docs/STREAMING_BEHAVIORAL_CONTRACTS.md`; the ONE archive layer: `docs/ONE_LAYER.md`.
  - **C++20:** every project target builds as C++20. The C++17 and C++20 builds are whole-memory identical on 91 run
    pairs (§8).
  - **`GEKKO_NATIVE_EXPAND_JOBS`** (savestate v29 keeps queued jobs and reads):
    - `=1` W1: no OSThread for TExpandBG;
    - `=2` Phase 1: host decoder;
    - `=3` Phase 2: no TExpandBG, the extract 0x8004C770 queues a host job;
    - `=4` Phase 3: decode-ahead on a native `std::jthread` worker;
    - `=5` W2 (needs `GEKKO_NATIVE_BG_READ=1`): no TBGRead, TBGReadManager::update in C++. Bytes and order equal,
      reads complete earlier (contract C2: PlayAction 0.02–0.22 s sooner). **The user accepted this timing
      2026-11-04.**
  - **`GEKKO_NATIVE_ONE=1`** (Phase 4a–4e, done): all 13 ONE archive functions, the sync Open (reads through
    `NativeRtGcnFs::read_sync`), OpenAsync / OnRead, the whole-file read 0x800450C4 and the six TOneFileAsync loader
    updates are C++ owners (`runtime/src/gekko/native_one.cpp`). Whole memory equals the original on 86 run pairs per
    batch; `native_one_tests` covers all 1,244 disc archives (584,957 checks, 0 failures).
  - **Strict oracle (§16):** 0 mismatched on Westopolis, Final Haunt and a cold stg0402 load (22.4 M segments); the 8
    replayable ONE owners owner-verified. Not replayable by it: the I/O owners (replay reaches the retired DVDOpen) and
    the loader updates (interrupt timing inside the RW readers); they rest on CTests and the sweeps.
  - **Rules learned (apply to every native owner):**
    - the game copies stack leftovers into heap records (fn_8006BEF0): write the original's frame and make guest calls
      from it, with the original return site and registers;
    - never call yielding game code synchronously: drive the yielding owner's state machine (`read_sync`, only while no
      same-priority thread is ready), or enter a callback via `leave_to_dispatcher` with an HLE at the return word;
    - leave r3 / r4 / f1 as the original does, restore pc after guest calls, and pass the current r4..r7 to a call whose
      original loads only r3.
  - Runs: `.build/stage_loading/` (`w2`, `frames`, `one`, `open`, `async`, `whole`, `loaders`, `strict-phase4`).
- **Resource / loading / memory / streaming audit (2026-11-04, the user's directive; uncommitted):**
  `docs/RESOURCE_AUDIT_2026-11-04.md`.
  - **Fixed, default path:** RtGCNFS's read / open / close / sync / abort owners are now frame-exact. Read keeps its
    state in r23..r31 as the original does, and no longer uses the 'RTFS' frame tag. This changes default-mode thread
    stack bytes only; the new bytes are the original's. Also fixed: a zero-byte motion upload/fetch stop; the motion
    store's journal/trace bypass; savestate region restore (committed size, order); texture eviction never re-parsing;
    the case of `.one`; failed decode-ahead entries.
  - **Fixed, switches:**
    - the BG_READ loop is frame-exact through read_sync;
    - expand jobs: mode 1/2 states load in mode 3+; failed publications requeue; worker exceptions; queue cleared on
      load; shutdown;
    - CRI / jobs bridges refuse cross-setting states;
    - PRS retries only when a larger hint helps.
  - **Savestate version 30** (the expand jobs' creation stamp).
  - **New C++ owners under `GEKKO_NATIVE_ONE=1`:** the byte-swap leaves 0x80043D0C / 0x80043D34 / 0x80043C80.
  - **Proof:** the oracles have 0 failures (stack windows included), and the A/B is identical apart from thread-stack
    words.
  - **Open:** a savestate round trip in-game; F7 (texture cache bypasses the asset system); the native allocator (M6).
  - **The game target does not build right now:** `shadow_main.cpp:3477` `peripheral::combine`, edited 10:33 by
    another session.
- **2P fixed (user confirmed).** 2P depends on the stage record's flag (stg0800–0802 only). The old
  `--boot-story-stage` parked P1 outside the world and never placed P2, hence the darkness and the T-pose.
  - New uncommitted test options: `--boot-2p <stage>`, `--boot-2p-three`, `--boot-2p-chars a,b`,
    `--boot-last-story`, and `--boot-expert <0..22>` (experimental, untested).
- **Pending user decisions:** make the native switches (BG_READ, EXPAND_JOBS=5, ONE) the default; Phase 5 (the rwcore
  RenderWare readers); the expand waits / later publication; the read on a pool thread (not built: reads cost only
  24–70 µs); committing.
- **Not reached yet:** Devil Doom (stg0710) needs The Last Way finished. Ask the user to play it while recording.
- **Off limits until the Sonnet workspace's fields R and S report:** the set-object range and draw-distance code. The
  workspace is "Shadow Hedgehog world rendering reconstruction" (`H:\ShadowTheHedgehogDiskGC_function_copies`,
  `src/world_distance/`).

### Cross-reference: what the Sonnet workspace adds (read 2026-11-03, read-only; nothing copied)
Root `H:\ShadowTheHedgehogDiskGC_function_copies\world_distance`. Its own label is "NOTHING here is approved". "PROVEN" in
the gameplay docs means read from the disassembly, not executed. Check each value with our oracle before use.
- **Player constants with consumer PCs** (`from_main_repo_2026-11-03/gameplay/PLAYER_GAMEPLAY_RECOVERY.md` 5.4): homing
  search range 150 (0x805F0CE0, used at 0x80206C98), cone 45° when |stick|² > 0.09 else 90° (0x805F79D0/D8/CC); jump table
  0x804B1628 (100 speed, 270 thrust, 0.75 s hold at 0x805F3834); JumpDash 180/430/0.4 s; HomingAttack 10 s timeout
  (0x805F37BC); SpinDash 150 + 850·min(charge, 1 s). None of these addresses were in our docs. Use them to design homing,
  JumpDash and SpinDash routes (a target must sit inside 150 units and the cone).
- **Command ids 0x101–0x137 all named** (`GAMEPLAY_COMMANDS.md`); our audit already has 0x11C / 0x12A / 0x133 paths, so the
  gap there is still execution.
- **Weapons** (`WEAPONS_AND_TARGETING.md`): VacuumPod 24 and VacuumEgg 61/62 have no bullet (ThrowObject power 6.0,
  0x805F91C8 → 0x8027DA84); ControlVacuum 0x8023DB54; SatelliteLaser damage untraced.
- **2P / stage table:** 59 × 0x50 records at 0x804C5AE8, set by fn_80203BC4; flag bit 2 = two viewers (only 800–802),
  byte 0x8057E812 (`docs/fragments/R_runtime_graph.md`). Player id slots: 0x805EF630 (P1) and 0x805E5A28 (P2), one writer
  fn_800AD6D0.
- **Loader pieces not in STAGE_LOADING.md** (`docs/fragments/O_stage_models.md`, `J_*`, `D_*`): TDestroyManager deferred
  destroy (vtable 0x8051E144, fn_8004384C, drained by fn_80043724 on unload); TOneFileAsync<RpClump> twin 0x8004BDA0; layer
  pick by entry-name token in fn_8004652C; 9 RG1 entries no loader reaches; BDT boxes for 63 stages in `out/O_bdt_boxes.csv`.
- **Caution:** its interpreter `tools/E_ppc.py` takes Force25 and a frsqrte model from Dolphin, so it is not an independent
  frC reference. Our frC decision stands.

## START HERE (handoff 2026-11-03, gc-removal worktree)

**2026-11-07: GC removal step 2 done** (docs/GC_REMOVAL_PLAN.md "Step 2 done"): ReverbHiInit / ReverbHiShutdown are
native owners, 9 SDK rows pruned, ratchet 1,044 shipping / 1,050 tests; gates 1-4 with evidence there. New diagnostic
`--watch-guest-from-tick <ticks>` (headless) arms the guest watch after a point of a cold-boot replay. Same session:
fres/frsqrte/ps_res/ps_rsqrte return the hardware table estimates (runtime/include/gekko/gekko_estimate.h, tested
against hwtests' definition) and the native sqrt (sqrt_guest) takes the host root for positive finite singles
(exhaustively equal). Nothing committed. Next candidates: the three unused GCAX effects' init (same pattern), then
the plan's step 3 / 4.

Where: H:/ShadowTheHedgehogDiskGC_Extract/wt-gc-removal, branch gc-removal at 35dfa30 + a large UNCOMMITTED working
set (28 modified, 25 new files - `git -c core.autocrlf=false status`). Nothing committed (commit only when the user
asks). Release tree .build/watch (exe .build/watch/bin/Release/ShadowTheHedgehog.exe); tests tree builds the CTests.

Directive in force (user, 2026-11-02): Discover -> temporary translation bridge -> fully reconstruct the original
behaviour -> replace with lightweight native C++20 x64 -> test, debug, optimize. Bridges count for nothing; no fake
parity, no stack-garbage replication, no hidden emulation; CPU claims only from repeated alternating benchmarks.

### State (all verified this session)

| item | state | evidence |
|---|---|---|
| CRI thread bridge (native_cri.cpp, SHADOW_NATIVE_CRI=1) | works, OFF by default pending acceptance | stall root-caused (servers in interrupt context; ADX output spins at 0x803CF0D4 for an ARQ completion) and fixed with thread-context upcalls (NativeInterrupts::request_upcall / upcall_call, continuation 0x80376224); Westopolis 90 s: stage loads, PCM 015445f2..., 5,360 frames, 0 underruns |
| Erratum E1 (user-approved fix) | ON by default, SHADOW_ERRATA=0 for A/B only | EnemyBaseAI +0x34 lock byte never initialised; original: 17 of 30 AIs skip their initial None state; fix zeroes it at constructor entry 0x8019F1D4; docs/GAME_ERRATA.md, CTest game_errata_tests |
| Native SVM (CRI "SVM/GC Ver.1.58", 18 functions) | DONE, owners in extract/native-owners.txt | docs/CRI_SERVERS.md 1a (seven-item proof): oracle native_svm_tests (48 calls, 75 events), A/B PCM + AI census identical bridge on/off, owners 144 -> 162, -454 guest instructions, exe -29,696 B, CPU 3x alternating: no measurable change (no claim) |
| Savestates | v27 (saves native SVM state; older states import S) | regression fixed: states saved since v25 were unloadable (audio_state.cpp capped at 24); v27 round trip identical, v26 import identical PCM |
| NativeJobs bridge (SHADOW_NATIVE_JOBS=1) | FAILED its A/B, OFF | one loader job never ran (loading screen waits instead of yielding); to be replaced by native async TBGRead / TExpandBG (docs/NATIVE_THREADS.md) |
| Native coverage audit | DONE, no engine change | docs/NATIVE_COVERAGE_AUDIT.md, tools/audit/native_coverage.py; of 1,214,564 originally live instructions: 96.72 % translated, 0.70 % native owner bodies (0.55 % implementations + 0.15 % GameCube-contract shims), 0.02 % stop-only, 2.56 % freed; runtime (game thread, busy): 68 % translated / 18 % native / 14 % machinery. The old dashboard (0.64 / 2.98 / 96.37 %) double-counted overlapping pruned rows and missed native-owners-r2.txt |
| Unclassified code (dolrecon spine layers) | 1,795 functions -> 86 (2026-11-03) | source/tools/dolrecon/spine.py rules A5 (proven N8 / N10 / M0 names), S3 (gaps between game-side anchors -> game-unit), G1 position guard (no "only X calls it" layer between two library anchors of other layers; 27 library functions no longer claim game) and S1b (library spans across std instances); 109 tests in test_dolrecon_evidence.py; spine + skeleton regenerated; details source/docs/GAME_SPINE.md top two sections |
| Native SJ (CRI stream joints, step 2a) | DONE 2026-11-03, owners in extract/native-owners.txt | runtime/src/gekko/native_sj.cpp, 38 functions (SJMEM, SJRBF, SJ_SplitChunk, SJCRS counts); oracle native_sj_tests 3,094 calls 0 differences, 4 negative controls killed; A/B 4 runs PCM 015445f2..., 5,360 frames, end state differs only in dead CRI thread-stack words; native counts = the original's census; CPU 3x alternating 18.70 -> 17.73 s mean (non-overlapping); static translated 96.56 %. docs/CRI_SERVERS.md 4b |
| Native ADX output (CRI rna, step 2) | DONE 2026-11-03, owners in extract/native-owners.txt | runtime/src/gekko/native_adx_output.cpp (5 functions over native SJ; ARQ post + spin + DSP DMA interrupt replaced by a synchronous copy + completion); oracle native_adx_output_tests 0 differences, 5 controls killed; A/B PCM 015445f2..., native counts = census; CPU unchanged (no claim); translated 96.52 %, CRI 4.95 % native. docs/CRI_SERVERS.md 4c |
| "Continuous audio buzzing" | NOT reproduced | 0 underruns in every replay run; needs the user's repro (stage, settings, when it starts) |

### Next steps, in order

1. (DONE 2026-11-03) CRI SJ stream joints native - docs/CRI_SERVERS.md 4b.
2. (DONE 2026-11-03) rna output native (docs/CRI_SERVERS.md 4c): runtime/src/gekko/native_adx_output.cpp, 5
   functions; each transfer is a synchronous copy into the stream ring, its completion run at once; oracle
   native_adx_output_tests 2,988 calls 0 differences, 5 controls killed; A/B PCM identical, counts = census, end state
   differs only in dead stack / dead volatile registers of the vsync thread's saved context; CPU no measurable change.
2b. (DONE 2026-11-07, branch agent/cri-threads, docs/agents/cri-threads.md; bridge still OFF by default) revisit the CRI thread bridge (section 5) now that the vsync server never waits for an interrupt - its
   upcall may become a plain native call at the retrace point; then step 3 (ADX exec).
3. ADX exec (step 3), stream servers, Sofdec.
4. CRI bridge acceptance: repeat the A/B (PCM, AI census, frames) + an alternating 3x benchmark, then promote it to
   default through the normal gates.
5. Then eliminate the guest thread stacks, PPC context and scheduler machinery (stages A-D in docs/NATIVE_THREADS.md;
   loaders become native async jobs).
6. Re-run tools/audit/native_coverage.py after each milestone (command at the top of docs/NATIVE_COVERAGE_AUDIT.md;
   --reuse only when the generator inputs did not change) and report the corrected numbers, never the old dashboard's.

Owed: Westopolis A/B of the RtGCNFS async retirement; dolrecon_lift_tests (only when the user allows: > 4,157 s,
stderr flood); the effect-tagging "Sky" defect.

### How to run / measure

- A/B harness: .build/ab-jobs/run.sh (EXE= and EXTRA= overrides); pre-SVM baseline exe .build/ab-jobs/base-presvm.exe;
  run dirs full0/full1 (bridge off/on), o0/e0/e1 (original / E1 / E1 + bridge), s0/s1 (native SVM), bb1-3 / bn1-3
  (benchmarks), audit (coverage-audit replay with samples samp-audit.csv).
- AI census: GEKKO_CALL_CENSUS_ADDRS=8019F528,801A0000 GEKKO_CALL_CENSUS_R3_WORD=C.
- Savestates: --save-state-at-tick works only with --headless; load with --state-file X --load-state.
  --report-peek needs 0x-prefixed hex.
- Sampling: GEKKO_SELF_SAMPLE + tools/self_sample_report.py with the exe map.

Constraints: never touch the user's saves / cards / SRAM (copies only); a running ShadowTheHedgehog* process is the
user playing (don't stop it or run heavy checks beside it); limit parallel jobs (CPU heat); git with
`-c core.autocrlf=false`; never ShadowRecomp / BigSnek9.

## Session log (newest first)

**2026-11-02 evening: native coverage audit** - see the table above and docs/NATIVE_COVERAGE_AUDIT.md.

**2026-11-02 (gc-removal worktree, H:/ShadowTheHedgehogDiskGC_Extract/wt-gc-removal, branch gc-removal at 35dfa30 =
source/ snapshot 12:45 + UNCOMMITTED work), GameCube-layer removal, STATE (read first for that work):**
- Directive (user, 2026-11-02): native execution, NOT scheduler replication - derive the guarantees the game's real
  thread users need and implement them with native primitives; report per milestone (functions eliminated, reachable
  lifted functions, compiled SDK bodies, guest-context dependencies, validation, CPU change).
- Done + CTest-verified (tests tree): RtGCNFS asynchronous branch retired (proof tools/platform/rtfs_async_proof.py with
  7 refused mutations; owner refuses async files); native -> SDK sites 11 -> 6 (ratchet 6, 5 fails); liveness roots
  -7. SDK bodies unchanged (1,063 tests / 1,057 shipping). A Westopolis A/B replay of that step is still owed.
- Gate 2's dynamic half BUILT: runtime/src/gekko/guest_watch.cpp (--watch-guest <ranges> --watch-report <csv>; page
  protection on both host views, exact row attribution by unwinding to the generator's fn_XXXXXXXX(_body) frames;
  needs -DSHADOW_WATCH_SYMBOLS=ON for leaf rows; tree .build/watch). guest_watch_tests passes (leaf row, helper,
  native, outside-range, write-through).
- Thread users established (docs/NATIVE_THREADS.md): the game's only threads are two one-step background loaders
  (TBGRead / TExpandBG on SonicteamUSA::System::Thread) + CRI's four frame-driven server threads; first watch run:
  no game / CRI / RenderWare row touches RunQueue, the default thread or CRI's OSThreads. Direction: no scheduler at
  all (loaders = native jobs at the main thread's yield / wait points; CRI servers = frame-loop calls at the retrace
  event; main waits = advance to next event), stages A-D in docs/NATIVE_THREADS.md. Nothing implemented yet.
- Also fixed: tools/dolrecon/callgraph.py (+993 call edges: _text_N files, name_ADDR, labels), platform_deps.py /
  lift_cpp.py file filters; shadow_main.cpp included present_trace.h / phase_lock.h only when lifted (non-lifted builds
  failed) - source/ has the same defect.
- Later 2026-11-02: NativeJobs bridge (runtime/src/gekko/native_jobs.cpp) built and A/B'd - it FAILED (one job never
  ran: the loading screen waits instead of yielding; the user saw the loading screen blinking) and is OFF by default
  (SHADOW_NATIVE_JOBS=1 to diagnose). G0 passes: two identical runs are byte-identical (state + PCM); the default build
  is byte-identical to the baseline. User directive: NativeJobs is a bridge only - rebuild TBGRead / TExpandBG as
  native async loading (design + spec in docs/NATIVE_THREADS.md; PRS decode is already a native owner). Launched for
  the user: game/ShadowTheHedgehog.gc-removal-2026-11-02.exe (29cdb23a, lifted, bridge off), card
  save-data/8P-GUPP-SHADOWTHEHEDGEHOG-full-gc-removal-2026-11-02.pc.raw.
- Loading work measured (docs/NATIVE_THREADS.md "Measured"): file reads 6.7 ms / 90 s (RAM-resident), PRS expands
  mostly served by the existing decode-ahead workers (601, 0.75 ms), 3-11 game-thread decodes (cache misses, max 1.4 ms).
  The loader's work is already native; what remains GameCube is the thread / scheduler CONTROL, which needs the main
  thread's waits (stage C) and CRI's servers (stage B) native first. Exit lines report all of it ("loading work on the
  game thread", "native jobs").
- CRI (user directive: reconstruct CRI's server ALGORITHMS natively; bridges count for nothing): the thread bridge
  runtime/src/gekko/native_cri.cpp (servers at the retrace / idle point, lock as a count, HostWork interrupt source,
  idle hook in Cpu::idle_wait, retrace tick phase in native_video.cpp) FAILED its A/B (stage load stalled; the game
  thread reaches an idle point only ~11 times in 30 s) and is OFF by default (SHADOW_NATIVE_CRI=1). Default build
  byte-identical to the baseline. Server chain map + reconstruction order: docs/CRI_SERVERS.md. NEXT ACTIONABLE STEP:
  native SVM (table, dispatch, registration, lock, test-and-set; ~400 insns) with a reference-interpreter oracle test,
  then the ADXGC rna output.
- Effect audit (docs/EFFECT_PERFORMANCE.md): motion blur costs nothing while off (history saved only when the game's
  SaveScreen runs, ~1.5 % of frames); while on it adds ~2.5 ms / frame at 2560x1440 4x MSAA (blend quad 1.42 ms + the
  history resolve ~1 ms) - bandwidth of a full-screen blend into the 16-bit MSAA scene; the history resolve is
  required (it is not the main resolve's image). Resolution scaling with NVML clocks: running.
- LATER 2026-11-02 (15:00-16:00): the CRI bridge's stage-load stall is ROOT-CAUSED AND FIXED (docs/CRI_SERVERS.md
  section 5): (1) no preemption -> SelectThread(1) at the tick's delivery end; (2) the servers ran in interrupt
  context, so the ADX output's wait at 0x803CF0D4 for its ARQ transfer's DSP DMA completion could never end -> new
  thread-context upcalls (NativeInterrupts::request_upcall / upcall_call, continuation 0x80376224 in
  extract/native-owners.txt; EE on, own OSContext, nested deliveries normal, SelectThread refuses switches while one
  runs) on CRI's own thread stacks (create_thread records r6). Westopolis 90 s with SHADOW_NATIVE_CRI=1 vs off
  (.build/ab-jobs/full0, full1; exe .build/watch): stage loads, PCM byte-identical (sha 015445f2...), render frames
  5,360 = 5,360, 0 audio underruns both, Vsync ticks 5,367, Idle calls 5,268, process CPU 17.58 s vs 18.48 s (1 sample).
  ONE gameplay difference, traced to an ORIGINAL GAME BUG, now FIXED BY USER DECISION as erratum E1
  (docs/GAME_ERRATA.md; runtime/src/gekko/game_errata.cpp; CTest game_errata_tests passes): EnemyBaseAI's
  state-machine lock byte (+0x34) is never initialised; with garbage there StateMachine::Start (0x8019F528) skips the
  initial state (None) - it only clears the byte. Original game, 90 s tape: 17 of 30 AIs skip it. Fix: byte = 0 at
  the constructor's entry (declining hook 0x8019F1D4); SHADOW_ERRATA=0 restores the original for A/Bs only. Runs
  o0 / e0 / e1 (original / E1 / E1 + CRI bridge): PCM all 015445f2..., 5,360 frames, AI censuses e0 = e1 (all 30
  enter None). The census tool: GEKKO_CALL_CENSUS_ADDRS=8019F528,801A0000 GEKKO_CALL_CENSUS_R3_WORD=C.
  Not reproducing stack garbage (user: no stack-garbage replication). "Continuous audio buzzing": not reproduced on
  the replay (0 underruns in every run) - needs the user's repro. CPU: o0 18.13 s, e0 19.91 s, e1 17.84 s process CPU
  (single samples, noisy: NO CPU claim until repeated benchmarks). CRI bridge still OFF by default pending its
  acceptance (repeat A/B + benchmarks). Tests tree rebuilt: native_threads, guest_watch, cri_file_device, game_memory
  owner, native_video_reference, game_errata pass. Uncommitted.
- DONE 2026-11-02 16:20: native SVM (reconstruction step 1, docs/CRI_SERVERS.md 1a). runtime/src/gekko/native_svm.cpp
  owns all 18 functions of CRI's "SVM/GC Ver.1.58" (0x803CBFB8..0x803CCD50; bodies pruned to thunks, exe -29,696 B);
  S (0x805C0E20) is native state (savestate v27, older states import S). Guest calls only through continuation
  0x803CC0D0 with a frame on the guest stack. Oracle CTest native_svm_tests: 48 scripted calls, 75 traced events,
  results + full state equal to the original words. A/B 90 s Westopolis vs the pre-SVM exe
  (.build/ab-jobs/base-presvm.exe; runs e0/s0 bridge off, e1/s1 bridge on): PCM 015445f2... in all four, AI censuses
  identical, 5,360 frames, 0 underruns; counts: 21,550 server calls (types 2/4/5/6: 5,367/5,367/5,548/5,268), 32,429
  callbacks, 92,665 lock hooks (= svm_lock 71,281 + test-and-set 21,377 + 7 setter locks), 6 registrations, no
  error reports. CPU: single samples contradictory -> alternating 3x benchmark (bb1-3 base, bn1-3 native) running.
  Movie trace counters for the two servers now read NativeSvm stats (they would have replaced the owners).
- SVM proof complete (docs/CRI_SERVERS.md 1a, seven items): generator owners 144 -> 162, pruned rows +15, guest
  instructions -454; CPU benchmark 3x alternating: no measurable change (no claim). REGRESSION FOUND + FIXED:
  savestates saved since version 25 (this session's native-jobs bump) could not be loaded - audio_state.cpp refused
  versions > 24; the bound now follows save_state_version(). Verified: v27 save at 20 s -> resume to 30 s equals the
  uninterrupted run (all memory blocks, PCM tail); a v26 state from the pre-SVM exe imports S and resumes with
  identical PCM. Note: --save-state-at-tick is served only by the --headless loop.
- NEXT: reconstruction step 2 needs CRI's SJ stream joints first (docs/CRI_SERVERS.md 4a: the rna output pulls PCM
  through SJ vtables +0x18 / +0x1C and splits chunks at 0x803CBF34). Map SJ (classes, vtables, users), reconstruct it
  natively with an oracle, then the rna output (the ARQ post + spin at 0x803CF0D0 / 0x803CF0D4 becomes a synchronous
  native copy), then ADX exec (step 3). CRI bridge acceptance: repeat A/B + benchmark, then promote (still OFF).
- Owed / next: A/B replay of the RtGCNFS step; stage A (loader jobs) with an A/B replay deciding whether the exact
  completion point must be kept; watch the loader OSThreads (heap) and more stages; dolrecon_lift_tests runs > 4,157 s
  (killed at the user's request; [fm] stderr flood suspected - 611k lines, 100 MB - see the 2026-11-02 log).

**2026-10-30: fresh whole-project map docs/PROJECT_MAP.md. Rule: every validated major success is merged into
H:\NukkoroGit (the public copy). Disk housekeeping done (H: .build 548 -> ~20 GB; C temp -110 GB).**

**2026-11-02 (latest), GameCube SDK removal planned and gated: docs/GC_REMOVAL_PLAN.md (user: "plan the next step to
actually rip out the obsolete GameCube part").** New tools: ppcrecomp `--live-graph` (every reach event of the pruning
pass; emits nothing different), tools/platform/what_if.py (what an ownership / root change frees, by a real generator
run on scratch copies), tools/platform/removal_plan.py (frontier per service, cumulative removal order, work list;
its liveness equals the generator's, its prediction for owning OSInit equals the generator's run: 30 = 30 rows).
Measured: 1,057 platform-SDK bodies compiled, 292 frontier rows, 0 left if all were owned. CTest
platform_gc_removal_plan (ratchet SHADOW_GC_REMOVAL_RATCHET = 1057). Fixed: native_liveness_roots was failing (the
generated lift's 171,849 literals; lift_cpp.py now marks its output reference-interpreter-only, regenerated from
pal-v3 byte-identical apart from that line; +32 real roots, pruned set unchanged at 711). Also: the host directory
scan moved out of core asset_namespace.cpp into the host-directory backend (namespace_from_host_directory; the
contract's name rule enforced in the backends and the core binding); 10 affected ctests pass. Next: plan step 2
(ReverbHiInit / Shutdown owned; needs the guest load watch of gate 2), step 1's classification of the 53 runtime-held
frontier rows. Nothing committed.

**2026-11-02 (later), baresamp DONE and palquant ported (rwcore/src/resample.cpp, rwcore/src/palquant.cpp,
rw/core/palquant.h; details: source/rwcore/README.md module rows "baresamp" / "palquant", "Native contracts" ->
Resampling / Palette quantisation, and their proof paragraphs).** baresamp: 34 controls, 32 killed, 2 proven
equivalent (exhaustive float check in .build/palquant_check/fuse_check.cpp). palquant: 21 GameCube functions
0x8048E8AC..0x8049148C named from the PC palquant.obj symbols (release + debug, .build/rw_pc_oracle/objs/l7.cmd);
its oracle (240 quantisers, 2.18 G original instructions, ~265 s; rwcore_oracle.h's call() now takes a step limit) and
native test pass; the oracle found one native bug (maxcols <= 0 wrote palette[0]), fixed. Controls:
.build/dolrecon/palquant_mutations.json (51) - results in palquant_mut_native.log / palquant_mut_oracle.log, then the
README's palquant proof paragraph gets its control sentence; ASan of the new tests and the full
`ctest -R "rwcore_|rpworld_"` (32 tests) still to run. Next: batextur's default read path (read: PalettizeImage
0x8048C7C4, PalettizeMipmaps, TextureImageReadAndSize, TextureDefaultNormalRead; notes in
.build/dolrecon/palquant_re/tex_*.s). The mutation watchdog (.build/dolrecon/mut_watchdog.ps1) stops a control that
hangs; mutate.py has no time limit of its own.

**2026-11-02, native RenderWare resources arena (baresour / resmem) proven (rwcore/src/resources.cpp,
rw/core/resources.h; details: source/rwcore/README.md module row "baresour / resmem", "Native contracts" -> Resources
arena, the rwcore_resources_tests paragraph).** 10 functions of PAL main.dol: 0x8048158C..0x80481CBC (open, close,
FreeResEntry, Purge, AllocateResEntry, EmptyArena) and the heap 0x804839D4..0x80483C10, identified as resmem's
_rwResHeapInit / Close / Free / Alloc (labelled _rwResourcesOpen__sub2 etc.): the same code as the PC libraries'
resmem.obj (extracted and disassembled into .build/rw_pc_oracle/objs). E_RW_INSUFFICIENTRESOURCES = 0x0C from the
debug library's error table. The module is registered after texture (pipeline / chunk group / immediate mode still
missing between them) and RwCameraShowRaster purges the arena before the device shows the frame. Oracle 116,501
operations and native test 95,949 checks pass, no native / original difference found; controls 49, 46 killed, 3
equivalent (a dead branch of the original's split; the firstFree start). 64-bit: the heap's block header stays 32
bytes; native RwResEntry is 48 bytes, so entries take up to 32 bytes more arena than on GameCube. Next: baresamp
(0x8048B1C8 = ImageResampleGetSpan, matched to the PC's static; 0x8048B55C its rectangle, 0x8048B7E0 called by the
texture reader 0x8048CA80 and RpMatFX, 0x8048B9BC by TextureRasterDefaultBuildMipmaps - which is RwImageResample /
RwImageCreateResample is still to be shown), then batextur's remainder (0x8048C6FC..0x8048E310, 10 functions).

**2026-11-01, native RenderWare images (baimage / baimras) proven (rwcore/src/image.cpp, rw/core/image.h; details:
source/rwcore/README.md module row "baimage / baimras", "Native contracts" -> Images, the rwcore_image_tests
paragraph).** 24 functions of PAL main.dol 0x80488964..0x8048AB90 (+ _rwpathisabsolute 0x80484494), the 13 unnamed
ones identified from their code, callers and the PC libraries' link order: RwImageDestroy 0x80488E00, FreePixels
0x80488FA0, MakeMask 0x80488FF8, ApplyMask 0x804890D8, ImagePathForAllFullNames 0x8048971C, ImageAttempRead
0x80489A54, RwImageFindFileType 0x80489BA8, RwImageReadMaskedImage 0x80489BE4 (RwImageRead inlined), RwRGBASetFromPixel
0x8048A168 (mislabelled rtpitexdCreateImageFromRaster__sub0), ImageConvertDepth 0x8048A1AC, RwImageCopy 0x8048A408,
RwRasterGetOffset 0x8048AB7C (baraster); 0x80488924 is RwFrameRegisterPlugin (baframe, already ported). The image
module is registered in its slot (after camera, 0x220 bytes there, id 0x406) and RwEngineStart sets the device's gamma
as the original. New in the MSL port: pow (__ieee754_pow 0x803B0444, MSL's variant), proven by native_msl_tests in both
NI modes. Native structures only; the host device's image / raster exchange says "not supported" (the PC driver brings
it). Oracle rwcore_image_tests (19,591 operations, 100 sessions) and rwcore_image_native_tests (2,892 checks) pass; no
native / original difference was found (the harness needed fixing twice: start-up requests, free-list block sizes).
The game links no RwImageRegisterImageFormat: its format list stays empty, every image read finds nothing. Controls:
54 in .build/dolrecon/image_mutations.json (52 killed, 2 explained in the README), pow_mutations.json for pow. fp
digest now 0xACC0995AB18D8A3A (gamma tables added). Next: the four unnamed functions after baraster
(0x8048B1C8..0x8048BE30: resampling / mipmap building candidates), resources arena.

**2026-11-01, native world reader proven (rpworld/src/world.cpp; details: source/rwcore/README.md module row
"baworld / basector / babinwor / bamesh", "Native contracts" -> Worlds, the rpworld_world_tests paragraph).** New
oracle `rpworld_world_tests` (the original RpWorldStreamRead 0x80456744 and its parts in the reference interpreter
against the native on 5,999 random operations: converted-shape and malformed world streams, world lights and ties,
walks, look-ups, destroys; the guest world module set up without the GameCube pipelines, each manual step with its
evidence address in the test header) and `rpworld_world_native_tests` (348,981 checks: exact read-back, every native
contract, every truncation / allocation failure, a 10,000-level BSP, ties against their definition). Three native
differences found and fixed: the material list's array is allocated at once (space = count), RpWorldDestroy's order
(meshes, materials, sectors, world), a failed tree read's unwinding order. Controls: 43 in
.build/dolrecon/world_mutations.json, 42 killed, 1 equivalent (mutate.py now knows world_native_tests and builds
with /m:4). Not covered: RpWorldCreate and the mesh rebuild pass (not ported), GameCube-native worlds (refused), the
0x510 sector data and rights-to-render chunks (not registered natively).

**2026-10-30, 5K memory side (second session): live handoff docs/RENDER_TARGET_ALIASING_5K.md (top block).**
Render-target aliasing in TexturePool DONE (13:40): -115 MiB default heap / -109 MiB private commit at 5K
fullscreen, pixels proven (permutation test; 4x depth and 8-bit 1x targets stay committed because placing them
changes 4x output), GPU time unchanged; delivered to H:\NukkoroGit. CTest 130/131: f1c_host_stack_integrity_notail
fails independently of it (open, main session). Next there: smoke trimming.

**2026-11-02 (later), user: "dont stop until first gameplay clip". balight DONE (new library source/rpworld,
rw/world/light.h; 13 functions + RpWorldPluginAttach with the light module); oracle rpworld_light_tests, native
rpworld_light_native_tests, 29 controls (3 equivalent, explained in rwcore/README.md). Milestone defined: a video of
Westopolis drawn entirely by the native path (offline-converted world in native rpworld structures, the stage's own
light set, native RwCamera, the D3D12 renderer) along the Westopolis replay's gameplay camera path, at 5K. Found on the
way (stage lighting, game side): StageManager vf1 -> sub52 (0x80177798): 0x80047F5C fills 17 light sets (defaults),
0x80048738 reads "stg%04d/stg%04d_light.bin" (16 records x 0x34 bytes, little-endian on the disc, swapped by
0x80043C80 with descriptor "xxxxxxxxxxxii": ambient RGBA, directional RGBA, a vector, two int angles in degrees),
0x80048574 makes two stage-light wrappers (0x80047D88: ambient + directional, 0x40 bytes) and selects set 4 (+0x455);
0x80047870 applies a record: RpLightCreate(type via table r13-0x7B88 = {2 ambient, 1 directional, 0x80 point, 0x81
spot}), colour 0x80047808, frame rotated about X (1,0,0) by angle 1 then about Y (0,1,0) by angle 2, radius
0x8045B7D4, cone 0x8045BA94. World read path (for the oracle of the native world reader): RpWorldStreamRead =
0x80456744 (called once by the game, 0x8004B598), plane sectors 0x804562FC, atomic sectors 0x80455D34 (the GENERIC
platform-independent reader is present: 0x2C struct, vertices / normals / UVs / prelit / triangles), material list
read 0x8045C974, material read 0x8045C398 -> texture read 0x80484D78. Texture module open / close are 0x8048E52C /
0x8048E350 (RwEngineInit slot after raster; names.json mislabels them as plugin callbacks). Next: batextur (CPU side)
-> bamateri / bamatlst -> bamesh -> baworld / basector (native reader + offline converter writing RW's
platform-independent world chunks from the GameCube's prepared sectors via runtime/lift/assets importers) -> backend.

**2026-11-02, bacamera DONE (user: "Native RenderWare Camera Reconstruction"); details: source/rwcore/README.md
(modules bacamera / babincam / babbox, "Native contracts" -> Cameras, the camera tests, rwcore_camera_bench).** 25
functions ported from main.dol (0x80485318..0x80486C1C, 0x804844EC..0x804845E0), names from the RW link order and
call sites; the camera validation module is compiled out of the game (empty stubs) and not ported; RwCameraClone /
StreamWrite / GetPluginOffset are not linked. Camera module registered in RwEngineInit (slot after stream); the host
device's begin / end update succeed. Native contracts: refused begin restores curCamera, destroying the current camera
clears it, zero-filled creation, constructor failure / no module fail creation, stream struct capped at 32 bytes,
plugin-read failure destroys (no leak). Found: RwRasterShowRaster returns the RASTER, so a camera without a frame
buffer fails RwCameraShowRaster (native fixed to match). Math has no aspect assumption: Hor+ windows give square
pixels at 16:9 / 16:10 / 21:9 / 32:9 / 32:10 / 5K (tested). Kept as the original: world-space float corners, so boxes
narrower than far x 2^-23 lose their side planes (659 of 2,000 extreme test boxes; no ordinary one). Tests:
rwcore_camera_tests (oracle, 30,000 ops, 141,870 frusta bit-identical; NaN rule: generated NaN is 0x7FC00000 on Gekko,
0xFFC00000 on x86 - any two NaNs equal, closest bytes of NaN normals skipped, counted), rwcore_camera_native_tests
(1.18 M checks from the camera's definition, not its code), fp digest now 0xD7A9AC3F7B638783 (MSVC, MSVC AVX2, ASan,
clang-cl AVX2 contract off; NaN sign of closest bytes is compiler-dependent, excluded). 49 negative controls: 48 fail
a test, 1 equivalent (redundant version check). 16 rwcore tests pass in Release (44 s) and under ASan (456 s). Bench baseline:
~65 ns per camera sync, ~6.5 ns per sphere test; nothing optimized (negligible, resolution-independent). Next in order:
balight + world-light sync (RpLightCreate 0x8045BD84, _rpLightOpen 0x8045BEE8, RpWorldPluginAttach 0x8046185C,
WorldOpen 0x80460AA0, WorldLightSync 0x804622F8, RpWorldAddLight 0x804635C8) - map its dependency graph first. Backend
fact from the Godfather D3D9 driver: show raster on PC = a textured quad of the camera raster's sub-rectangle onto the
back buffer.

**2026-11-02, baframe DONE (user: "Native RenderWare Frame System Reconstruction"); details: source/rwcore/README.md
(modules baframe / basync / batypehf, "Native contracts" -> Frames, the frame tests and rwcore_frame_bench).** All 31
frame functions of the game ported from main.dol and named from evidence (0x80487CAC is _rwFrameCloneAndLinkClones,
not RwFrameCloneHierarchy: its sources keep root -> copy links for RpClumpClone 0x80458118, which purges via
0x80487CF4 = _rwFramePurgeClone; RwFrameCloneHierarchy / Count / OrthoNormalize / ForAllObjects / GetPluginOffset are
not linked). Frame module registered in RwEngineInit (slot after matrix). Iterative walks (any depth; 100,000-deep
chain tested), original defects fixed (stale roots after RwFrameDestroy, dangling sub-hierarchy, objects left on freed
frames, cycles, destructors on unconstructed clone copies, sources left pointing at freed copies, ignored constructor
failure). Free lists got an O(log n) block index with identical results: 200,000 frames create+destroy 5,000 ms ->
18 ms. Tests: rwcore_frame_tests (oracle, 22,000 ops, hierarchies 26 deep, 1.8 M LTMs), rwcore_frame_native_tests,
lifecycle / memory / fp_digest extended (digest now covers frame LTMs: 0x676237286060222D; MSVC, MSVC AVX2, MSVC ASan,
clang-cl AVX2 -ffp-contract=off reproduce it, clang contraction does not). 14 rwcore tests pass in Release and ASan.
Negative controls: every non-equivalent mutant fails (one equivalent mutant documented; three test gaps found by
surviving controls and closed). Next for world loading, in order: bacamera, balight + the rpworld plugin (WorldOpen,
RpWorldAddLight, WorldLightSync), materials / material lists / textures (RwTextureRead, texture dictionaries), babinwor
world stream read (GameCube-native sectors -> the offline converter), raster / image CPU side. User reference for the PC
renderer: H:\The Godfather The Game\_re\out (decompiled RenderWare 3.7 Direct3D 9 driver of The Godfather PC).

**2026-11-01, rwcore REVIEW (user: review the whole native core before proceeding) - DONE; details in
source/rwcore/README.md "Native contracts" and "Review 2026-11-01".** Fixed (each with a test whose negative controls
fail): RwStreamSkip / RwFtell were 32-bit on files (2 GiB+ skips went backwards; now RwInt64, _fseeki64 / _ftelli64);
RwSList / plugin-offset RwInt32 wrap-arounds; free-list parameter checks; the _rwVectorOpen leak; errors before the
error module opens (were written over curCamera; now held and carried into the module); the open-engine counter
(rwNumEngineInstances, decremented by RwEngineClose - it was misdescribed as "started"); RwMatrixInvert / Multiply
safe for any overlap (the original's in-place invert is WRONG; RenderWare forbids it; the game never does it, 9 call
sites checked); rwcore lacked -ffp-contract=off for GCC / Clang (measured: clang-cl /arch:AVX2 default contraction
changes results). Added: RwStreamWriteInt32 / Real / Int16, batkbin (8 functions), the engine's life (RwEngineInit /
Open / Start / Stop / Close / Term, RwEngineRegisterPlugin, _rwDeviceSystemRequest, colour + string modules, a host
device in place of the GameCube driver). Tests: 12 rwcore tests (8 interpreter oracles + rwcore_lifecycle_tests /
math_tests / limits_tests / fp_digest_tests), all pass in Release and under AddressSanitizer (.build/asan; runtime
DLL on PATH from the MSVC toolset bin/Hostx64/x64; configure from Git Bash with MSYS_NO_PATHCONV=1). Not covered:
big-endian paths, memory streams past 4 GiB (arithmetic only), threading (none). Forward work resumes at baframe.

**2026-11-01 (later), rwcore progress toward the stage-load goal; state: source/rwcore/README.md.** User rule given
mid-work: "No fake DVD code. Lightning-fast native x64 code with converted assets." So: streams use host files only
(RwFopen family over C files, rwFileInterface; the GameCube's disc file functions 0x8047B1C8..0x8047B810 appear only as
oracle stubs), and stage data (sector draw lists, geometry, textures) gets an OFFLINE converter to native PC formats
that the runtime loads directly - no GameCube-format decoding at load time. Ported and oracle-proven this session
(each with negative controls): balist (12), bamatrix _rwMatrixOpen / _rwMatrixClose (in rwcore_memory_tests),
batkreg (7 + rwDestroyEntry; plugin slices 8-aligned natively), bavector RwV3dNormalize / RwV3dLength (README had
wrongly listed them as not linked), bastream (7: memory / file / name / custom streams), babinary (13 read-side;
writers Int32 / Int16 / Real still open). All 7 rwcore oracles pass (ctest -R ^rwcore_, 21 s). Next, in order:
batkbin (plugin data chunks: _rwPluginRegistryReadDataChunks 0x804826FC ...), baframe, the texture / raster CPU side,
rpworld (material, matlist, geometry, world / sectors, balight), then the offline stage converter and the native
load of stg0100 with its lights (find RpLightCreate 0x8045BD84's callers for how the game makes stage lights).
Disposable negative-control runner: .build/dolrecon/mutate.py <test> <mutations.json> (spec files are removed after
each run).

**2026-11-01, GOAL (user): "continue until we can load a stage with lighting" with the native rwcore.** Scope found:
the stage worlds keep their sector geometry as GameCube native data (rpworld/baworobj readWorldSectorNative 0x80462D14,
readGeometryNative 0x80462C8C: per-attribute arrays + indexed strips, as runtime/lift/assets/geometry_import.h reads
them), so the PC needs a native-data reader into RenderWare's generic sector arrays; the rest is portable RenderWare.
Module map in the game (PC link order differs): balist 0x8047ECE0..0x8047F138, bamatrix 0x8047F150..0x80480A74,
bamemory 0x80480A78..0x80481580 (0x80480A78 _rwFreeListEnable, 0x80480A80 FreeListCreate(entrySize, perBlock,
align, preallocBlocks, inPlace, hint), 0x80480C28 RwFreeListCreate, 0x80480C74 = RwFreeListDestroy's twin used on
create failure), baresour 0x8048158C.., bastream 0x80481CBC..0x804823B8 (0x804823B8 = RwStreamOpen), batkbin
0x80482640.., batkreg ..0x80483194, babinary 0x8047DD48..0x8047EB60 (chunk header / FindChunk / Read Int16 Real
Int32 at 0x8047E6D4 / 0x8047E7D4 / 0x8047E8B8), baerr 0x8047EB88... RwFreeList (GC 0x24): {entrySize aligned,
entriesPerBlock, heapSize = bitmap bytes, alignment (0 -> 32), blockList, flags (2 = free empty blocks, 1 = in place),
link in the list at 0x805F2618}; block = {link, bitmap, slack, entries}, entry base = align_up(block + 8 +
heapSize), free indexes from the unaligned block + 8 + heapSize. babinwor (world stream read) has no names yet.

**2026-11-01, native RenderWare core + world layer programme started, STATE: source/rwcore/README.md.** The user
asked for RenderWare's platform-independent core and world layer in native 64-bit, able to use a modern machine's
memory. Done: the 64-bit model (types.h, engine.h: RwGlobals with native pointers, RwModuleInfo offsets, host
RwMemoryFunctions with size_t sizes), modules bavector (8 functions incl. the _rwSqrt / _rwInvSqrt tables) and baerr
(5), the shared oracle harness runtime/tests/rwcore_oracle.h (r2 / r13, guest engine block, rwmalloc / rwfree stubs
over a guest heap). rwcore_vector_tests: 332,198 checks bit-identical, 4 negative controls fail; rwcore_matrix_tests
still passes. Then bamatrix (17 functions, rwcore_matrix_tests 20,000 cases x 19 functions bit-identical, 5
negative controls fail; MSL math split into library shadow_msl), bamemory (10 functions, rwcore_memory_tests 40,000
operations structurally identical, 4 controls fail). Next, in order: balist (then _rwMatrixOpen / Close), batkreg,
babinary / bastream, baframe, bacamera,
baraster / batextur, streams, then rpworld (README "Scope and plan": 938 PC functions in rwcore + rpworld, the game
links a subset). Naming feedback: 0x8047EB88 / 0x8047EBC0 are _rwErrorOpen / _rwErrorClose (names.py N9 gave them
plugin-registry callback names) - rwcore-proven names should become name seeds.

**2026-11-01, Astra review 6 (max effort): .build/dolrecon/astra/question6.md / answer6.md; its defects FIXED in pal-v30 (docs/DOLRECON.md "Astra review 6: defects in the new rules fixed": 17,910 named, 94 tests), its enhancements open.** The defects it reported:
Defects it reports in today's rules (read-only deductions, not measured misnames): N15 does not prove a guard or
one-time construction (its own positive fixture is unguarded), store_index misses lis/ori writers, stwx, r2;
N14's RUNTIME_WORDS check is an ordered subsequence (in_order), not word for word, and builds_address is not tied to
the loop pointer; N7 accepts "flag != 0" (EQ clear) where -1 is a real flag; the N2/N6 fallback can name a function
the first pass rejected as plain_delete; returns_this / singleton ignore conditional returns (beqlr); skeleton
renders the flattened base list as direct public bases; an N12 name is truncated to boost__check (names.py:209).
Then: per-function analysis service, DOL-keyed fact store, explicit rule dependencies, body-family recogniser
(get_deleter first), dispatcher-backed record tables, typed shared_ptr / singleton dispatch, crosschecks, held-out
oracle, mutation tests.

**2026-11-01, Boost shared_ptr by body, STATE: docs/DOLRECON.md "Boost shared_ptr / shared_count by body".** Named
project .build/dolrecon/pal-v29 (17,920 named, byte-exact; decomp 4,635 / 4,636). dolrecon/boostsp.py pins Boost 1.32's release /
shared_count / shared_ptr bodies (the classroles copy / assign calls); N16 names 488 (234 copies, 132 dtors, 118
assignments, 4 primitives); spine moves them to boost and their neighbours lose S1 claims (game 26,952). 80 tests.
Next: command slots (CharCommand / PlayerCommand), Gadget 4, Boost slot 3 / other Boost bodies (function, bind).

**2026-11-01, static storage naming, STATE: docs/DOLRECON.md "Static storage: static initializers, exit-time
destructors, singleton getters".** Named project pal-v28, superseded by pal-v29 (17,577 named, byte-exact; decomp 4,635 / 4,636). N14: 281
.ctors initializers __sinit_<addr>, 79 exit-time destructors G__static_dtor (CodeWarrior runtime pinned word for
word); N15: 152 singleton getters C__instance (one registered static, returned directly or through a cache written
only with it). 75 tests. Next: shared_ptr copy / assign summaries, command slots, Gadget 4, Boost slot 3.

**2026-11-01, RTTI conflicts / secondary vtables / thunks, STATE: docs/DOLRECON.md "RTTI conflicts, secondary
vtables and thunks in ownership".** Named project pal-v26, superseded by pal-v28 (16,746 named, byte-exact; decomp 4,635 / 4,636). rtti_classes
abstains on conflicting duplicates (none in PAL), finds 563 secondary vtables inside data objects and 9 interfaces
with a leading pure slot; corrections.slot_holders / body_classes give ownership over every table (thunk != target;
a virtual-base thunk's target makes no class claim); N13 names 1,039 thunks. 64 tests. Next from Astra's list:
DOL-keyed facts, audit / rwalign / rwheaders tables, binary slices; then naming expansion.

**2026-11-01, N6 / N7 / sizes on the CFG engine, STATE: docs/DOLRECON.md "N6 / N7 / allocation sizes on the CFG
engine".** Named project pal-v25, superseded by pal-v26 (15,110 named, byte-exact; decomp 4,635 / 4,636). vcalls.object_code: delete under a proven
flag test (cr0 edge facts), constructor class = the vtable in each slot at every return, allocation sizes from any
constructor call on the new object (934 classes). 53 tests. Next from Astra's list: RTTI conflicts, DOL-keyed facts,
binary slices; then naming expansion.

**2026-11-01, Astra review 5 applied, STATE: docs/DOLRECON.md "Astra review 5: shared decoder, CFG-proven
evidence".** Named project pal-v24, superseded by pal-v25 (15,113 named, byte-exact; decomp 4,635 / 4,636). dolrecon/ppc.py
is the one instruction decoder; vcalls (jump tables, interior entries, def-use call proof, per-object stores),
rwheaders and strrefs run on its must-analysis; classroles proves returned-object provenance; spine reads structured
classes (layers moved: docs/GAME_SPINE.md "pal-v24"). 46 tests. Next: Astra's remaining items (N6 / N7 / sizes on the
CFG engine with a deletion predicate, RTTI conflicts, DOL-keyed facts, binary slices), then naming expansion.

**2026-11-01, N2 / N7 fixed, STATE: docs/DOLRECON.md "N2 / N7 naming defects fixed".** Named project
pal-v22, superseded by pal-v24 (15,104 named, byte-exact; decomp 4,635 / 4,636). N2 no longer names stack-temporary builders
(__dynamic_cast!); 288 virtual-base constructors are N6 C__ctor; 431 derived deleting destructors carry their own
class. names.json class_uses feeds spine A3. Astra question 5 asked (tool restructuring; answer5.md).

**2026-11-01, class roles of three more bases, STATE: docs/DOLRECON.md "Class roles of three more bases".**
Named project pal-v21, superseded by pal-v22 (byte-exact; decomp 4,635 / 4,636, 0 file errors). classroles.py contracts with instruction
checks + constructor type proofs: boost sp_counted_base 1 dispose / 2 destruct (release 0x8000945C), TEnemyAIState
1 setOwner / 2 enter / 3 execute / 4 exit (EnemyBaseAI state change 0x8019D8C0), Player::Behavior 1 canEnter /
2 enter / 3 exit / 4 onCommand / 5 update (Executer change 0x800A82FC, step 0x800A8160, command handler 0x800721A8).
N12 385 -> 1,125; C__vfN placeholders 3,233 -> 2,493. New finder dolrecon/vcalls.py (typed calls on this / this->F).
Next: Gadget 3 / 4, CharCommand / PlayerCommand, Weapon / Vehicle bases; N2 construct and N7 derived-dtor naming
defects (DOLRECON.md); then m2c pilot; CRI aligner.

**2026-11-01, class roles (Task / Gadget), STATE: docs/DOLRECON.md "Class roles from Task / Gadget dispatch".**
Named project .build/dolrecon/pal-v20 (15,134 named, byte-exact; decomp 4,635 / 4,636). RTTI fixes: duplicate class
records (73 base classes incl. Task / Gadget had no vtable) and pure-virtual 0 slots (7,875 slots); pal-v4
regenerated. dolrecon/classroles.py + names.py N12: Task slot 1 update (task-tree walk 0x8004ECAC), Gadget slot 2
exec / 5 checkRemove (Gadget update 0x8015AD28): 385 names. names.symbol(): RW leading underscores and `__`
separators were lost (97 RW names, all N11). Next: Gadget slots 3 / 4, contracts of the other base classes; then
m2c pilot; CRI aligner.

**2026-11-01, Astra review 4 + type registrations, STATE: docs/DOLRECON.md "Astra review 4" and "Object-type
registrations".** Named project .build/dolrecon/pal-v18 (N11: 640 callbacks of the type table 0x8052C1A0 by
dispatched role). Evidence-gate defects fixed (strrefs mr / stfs, rwheaders bctrl, X1 fail-open, full relocation
sets, data extents, rwdriver getter, rwcheck gate); tests tools/test_dolrecon_evidence.py (CTest
dolrecon_evidence_tests). CRI reference H:\Tools\refs\3s-decomp (AGPL: names / order only). Next (Astra's order):
class / virtual-role analysis from Task / Gadget dispatch; m2c pilot (200 functions); CRI aligner with explicit
API / internal identities; open review items in DOLRECON.md.

**2026-11-01, string pools ("Keep naming"), STATE: docs/DOLRECON.md "Names from string pools and CRI error texts".**
Current named project .build/dolrecon/pal-v16 (13,733 named, byte-exact). dolrecon/strrefs.py resolves pooled
string references (CodeWarrior pool + offset); N5 self-names 36 -> 70 (CRI adxt / adxf / cvFs / mwPly API by
elimination over inlined copies), N3 +151. Next naming: CRI API wrappers (ADXT_* around adxt_* inside the SVM lock),
SJ functions known only by error code, game debug texts now visible through pools.

**2026-11-01, RW names round 4 ("Keep naming"), STATE: docs/DOLRECON.md "RenderWare names, fourth round".** Current
named project .build/dolrecon/pal-v15 (13,605 named, byte-exact). GameCube RW driver named by role from its
RwStandardFunc list + RwDevice block + node definitions (dolrecon.rwdriver, rule N10: 45 Gcn* functions); rwalign
O2 (module order with stripped functions): 358 RW functions + 56 globals, holdout 0 wrong. Next naming: the rest of
the driver (helpers of the Gcn* functions are only __sub names), more of rwcore by O2 as names accumulate.

**2026-11-01, RW names round 3 (the user: "Keep naming"), STATE: docs/DOLRECON.md "RenderWare names, third round".**
Current named project .build/dolrecon/pal-v14 (13,514 functions named, byte-exact; pal-v12 superseded). 310
RenderWare functions + 50 globals; >= 12 wrong pal-v12 names found from the code and removed (_rwerror was called
RwErrorSet; six GX driver functions had PC names); coff.py had truncated every PC call list (fixed). New rules T1
(data tables), C4, E1/E2 + consistency checks; holdout 0 wrong; independent check dolrecon.rwcheck (field offsets,
link order). 235 callbacks (201 named by role). decomp_names.py keeps source/decomp/auto on current names.
Next naming: _rt2dFontOpen review; extend T1 to tables reached through code (lis/addi of a table address); then
back to native rwcore modules (bamatrix rest, balist, baframe) on these names.

**2026-10-31 night, round 2 (the user: "Name more RW first" + "use RW37SDK to find names and connections"), STATE:
docs/DOLRECON.md "RenderWare names and connections from the RW 3.7 SDK".** Current named project
.build/dolrecon/pal-v12 (13,229 functions named, byte-exact; pal-v10 superseded). 206 RenderWare functions + 24
globals named with measured 0-wrong holdout; 57 game callbacks named from SDK callback types; 175 exact SDK
prototypes in names.json "signatures" and in the skeleton. Next: native rwcore modules (bamatrix rest, balist,
baframe) using these names; feed more RW names back through rwheaders for more callbacks.

**2026-10-31 night (the user: "bring back the Engine Core for RenderWare ... x64 native PC code with ZERO GAMECUBE
CODE ... everything named, clean, well structured"), STATE: source/rwcore/README.md + docs/DOLRECON.md "Mass
matching, RenderWare naming, native RenderWare core".** source/rwcore (CMake target rwcore, C++20, no runtime
dependency): matrix module bit-identical to the original (rwcore_matrix_tests). RenderWare names: rwalign.py, 127
evidence names (not yet applied to names.py: next = rule N8 from rw_names.json). Matching: 4,636 functions
byte-identical in source/decomp (autodecomp.py). Next: verifier hardening (Astra's list), m2c adapter, the rest of
bamatrix / bavector / balist / baframe natively with oracles. Astra runs visibly via .build/dolrecon/astra/astra_live.ps1.

**2026-10-31 22:00 (the user: "detach the game from the dead hardware, identify and name every possible part, a
source I can later compile for PC"), STATE: source/decomp/README.md, docs/DOLRECON.md "Names, second round",
docs/GAME_SPINE.md (update block).** Current named project .build/dolrecon/pal-v10 (byte-exact; supersedes v3/v5).
Matched source started: source/decomp (9 functions byte-identical incl. operator new/delete and CameraModeMtnViewer
vf0/vf2/vf3; verify with `python -B -m dolrecon.decomp_verify ../.build/dolrecon/pal-v10 --objdiff`). New names:
1,157 constructors, 1,367 destructors, 36 self-named library functions, 761 proven class sizes; virtual inheritance
exists (BossCtrlUnit). Skeleton: .build/dolrecon/skeleton (tools/dolrecon/skeleton.py). Open: the camera destructor
(stmw r30 vs two stw, not found in any flag), unlisted vtables (derived classes without RTTI records), SDK internals
unnamed (dtk names only), function signatures and member layouts not recovered.

**2026-10-31 20:30 (game spine + matching decompilation, the user's directives "find the spine of the game with only
the game logic" and "turn the game back to its source as close as possible"), STATE: docs/GAME_SPINE.md and
docs/DOLRECON.md "Matching decompilation".** tools/dolrecon/spine.py classifies all 34,037 functions by evidence:
27,389 game-side (3.54 MB), of which only 4 touch GameCube hardware (3 bus-clock reads, 1 GX FIFO write) and 121
call the SDK directly; 26,772 (3.30 MB) are pure logic. RTTI model corrected (record = {name, base list}; vtable =
{record, offset, methods}): 1,620 classes / 6,988 slots; named project now .build/dolrecon/pal-v5 (12,719 functions
named, byte-exact; pal-v4 = corrected). CodeWarrior GC compilers 1.0-3.0a5.2 + objdiff 3.6.1 on H:\Tools\dolrecon;
tools/dolrecon/match.py accepts a function only when byte-identical (M1-M4 + objdiff cross-check). First real game
function matched (CameraModeMtnViewer::vf2): compiler is 1.3-2.7 at -O4 (1.0-1.2.5 and all 3.0 excluded); narrow it
next with more functions. Known name defect: 803A1380 = operator new, 803A1334 = operator delete (N2 misfire).

**2026-10-31 19:15 (C++ reconstruction of ALL functions, the user's directive), STATE: docs/DOLRECON.md "State
2026-10-31 19:15".** 34,045 / 34,121 entries lifted to C++ (integer, scalar FP, paired singles; 76 OS-kernel refused),
proven per function against the original instructions in the reference interpreter. Pre-FP result: 16,302 verified,
0 FAILED. Names: tools/dolrecon/names.py -> .build/dolrecon/pal-v3 (9,074 functions named, all with evidence; the
generated C++ is lifted from pal-v3). Full 3-pass verification of the new set was running at handoff (.build/dolrecon/lv3-*): read its merged
summary first. Generated C++: runtime/tests/generated/ (regenerate with tools/dolrecon/lift_cpp.py from
.build/dolrecon/pal-v2), test: CTest dolrecon_lift_tests. Nothing committed.

**2026-10-31 (DOL reconstruction system, the user's spec), STATE: docs/DOLRECON.md.** Milestone 1 done: PAL, USA
and JP main.dol rebuild byte for byte from dtk 1.8.4 assembly + GNU as/ld 2.42 (tools on H:\Tools\dolrecon - the
user's rule: nothing on C:). 34,037 functions, 99.989 % of bytes assembled, ~12 s per rebuild. Accuracy pass done
(audit.py / linker_tables.py / corrections.py; .build/dolrecon/<region>-v2 = corrected projects, byte-exact; 511/511
extab boundaries, 4,997/4,997 vtable slots on function starts, 0 unlabelled body calls / branches, 2,920 RTTI names,
48 missed relocations added; the signature-name list was rejected with evidence). Next: Milestone 2
(durable jobs, 10k-job benchmark), Milestone 3 (similarity). Grok was stopped at the user's request; its collision
work is in H:\Unresolved Shadow functions\findings-20261031-GROK (oracle self-check 192/192, not game-verified).
Power line (below): the 20-minute rate-trim re-run (ab_lock2) is still owed.

**2026-10-31 (power / frame-pacing directive, docs/DIRECTIVE_2026-10-31_POWER.md), STATE (read first):**
Timing chain: docs/TIMING_ARCHITECTURE.md (sections 1-6). Telemetry: docs/FRAME_TELEMETRY.md. Baseline batch
.build/validation/telemetry-2026-10-31 (batch_resume.ps1; diag exe 942dae5f, 1080p/1440p/4K x Westopolis/Final Haunt,
cold + 2 warm, then the overhead set; the 4 instrumented overhead runs were closed early by a window close at 10:26-27 -
redo). LANDED (in source, uncommitted, diag built): "credit first" present order (TIMING_ARCHITECTURE 6): the render
thread takes the swap chain's latency credit before choosing the packet. A/B 24 runs (6.2): hold bounded by one
refresh (p99 worst 30.7 -> 16.2 ms), latch->display p50 -6.7 ms / p99 -13.3 ms (means), PCM + state byte-identical,
repeats / GPU / power unchanged. SHADOW_R2_CREDIT_FIRST=0 = old order. OPEN (6.3): since 10:29 every run is composed
(+1 refresh), code ruled out; check after a sign-out / restart, then redo the baseline. Next work: steer the game
clock's phase to the vblank (removes the remaining 0-16.7 ms boot-dependent hold), then findings 1-2 (game-thread
sleeps, present-thread idle wakeups).

**2026-11-02, native OS research (read first for GameCube-OS removal):** docs/native_os_research/00_MASTER_INTEGRATION_PLAN.md
integrates six read-only research documents (01 scheduler semantics, 02 interrupts / contexts, 03 SDK atlas, 04 native
execution architecture, 05 validation gates, 06 roadmap). Verified against code: all 5 OSCreateThread calls are
detached (attr 1); the decrementer / alarm path still enters the guest exception handler (cpu.cpp:1359) - a second
asynchronous path the scheduler work must own; the idle spin 0x803788FC is already a wait point. Nothing removed by
this research; implementation lives in the wt-gc-removal worktree (RtGCNFS step and native_threads_tests not yet
built / verified there).

**2026-10-31 (Phase IX part 2b), world collision complete to the result-object boundary, STATE (read first):**
native_world_collision.cpp (renamed from native_line_pick.cpp) owns every world query below RenderWare's dispatcher:
line picks and sphere queries (fn_80443260 incl. RenderWare's sphere-triangle test fn_8047C770), both callbacks'
query-side work and all 10 filters, on mirrored geometry with host float + FMA. 1,149,567 calls verified identical on
15 routes (0 differed, 0 declined); gates identical; savestate A/B dead stack only; game thread -1.66 % (WST) /
-2.41 % (FH) vs part 1, about -2.6 / -3.8 % vs fully translated. Fixed on the way: the guest-filter return address
(0x80020E7C, was 0x80020E70) - proven by a real guest filter on stg0601 (0x803350F0, now native). Deliberately left:
the game's pooled result objects (fn_80021678 tail, fn_800201D4, fn_80021E24 / FE4), consumed by 45+ translated
gameplay sites - they go native with their consumers. Env: GEKKO_NATIVE_LINE_PICK / GEKKO_NATIVE_SPHERE_QUERY,
GEKKO_VERIFY_WORLD_COLLISION, GEKKO_WORLD_COLLISION_STATS. Next: player update fn_800A8160 -> fn_80083A38 (the main
consumer), then enemies fn_801A7280. Details: docs/engine/WORLD_COLLISION.md.

**2026-10-31 (Phase IX part 2), native engine directive, STATE (read first):** the user's rule changed: the GameCube
exe is a behavioural reference, not an architecture - native data structures, host maths, no PowerPC/FPSCR emulation,
no translated callbacks where avoidable; the guest comparison stays as the regression detector. Landed (uncommitted,
diag tree): native_line_pick.cpp owns the game's whole world line pick below fn_80021678 (dispatcher + query +
callback 0x80020DC0 + all 9 filters) with mirrored collision geometry (dropped by hooks on the collision plugin's
sector ctor / reader / dtor) and host float + FMA maths. 762,500 picks verified identical on 15 stages (3 tapes incl.
Lethal Highway, 12 stages idle); gates identical; savestate A/B dead stack only; -0.65 % / -0.86 % (clean), ~-1.7 /
-2.0 % after caching (noisy). docs/engine/WORLD_COLLISION.md has the full chain and the open items (result objects,
fn_80021E24 / FE4, sphere query). run.ps1 now has routes lh (Lethal Highway tape) and idle -Stage stgNNNN.
Next: finish collision (result objects, sphere query), then player fn_800A8160 -> fn_80083A38, enemies fn_801A7280.

**2026-10-31 (Phase IX), gameplay engine RE, STATE (read first):** first gameplay-engine reconstruction landed
(uncommitted, diag tree): RenderWare's world line query fn_804421D8 (the largest translated self-cost, 2.0 %) is native
- docs/engine/WORLD_COLLISION.md. Every call of both tapes verified against the guest body (425,600 calls, 0
differed, comparing all bytes outside the dead stack); audio + frame 3000 byte-identical; savestate A/B 0 unexplained
bytes; game thread -0.9 % Westopolis / -1.4 % Final Haunt (non-overlapping ranges). The game runs with FPSCR NI = 1:
native owners must serve NI (the core's fps_* helpers implement it). tools/state_delta.py now accepts savestate v24.
Coverage map (tools/coverage_map.py on base8): guest 66 %, native 22 %, runtime 11 %; top tasks: player update
fn_800A8160 (10.9 / 13.2 %), shared enemy update fn_801A7280 (9.2 / 8.0 %), effects, render traversal. Next: the
line-query callbacks and their callers, player fn_80083A38, enemy fn_801A7280, motion unknowns (MOTION_PLAYER.md).

**2026-10-30 (Phase VIII), engine RE, STATE (read first):** clean baseline binary + map frozen in
.build/validation/perf-2026-10-30/base8 (fp_compare inline removed; gates byte-identical to the previous binary on
both tapes). Motion player documented in docs/engine/MOTION_PLAYER.md: 11.7 % (Westopolis) / 12.9 % (Final Haunt) of
the game thread - the largest subsystem; sequencer stages, the streamed half-float key sampler 0x804162AC and its
track layout, the Hermite interpolator 0x8041669C, the channel setter 0x80417E18. New non-shipping probe
GEKKO_CALL_CENSUS (+ _ADDRS; logs and declines; state byte-identical): ~14 % of motion updates repeat an object in the
same frame, all with different inputs - not redundant (disproven). Not started this phase: heat haze 0x8043531C,
F1-F14 runtime proofs, MT19937 consumption trace, psq / call_indirect specialisation.

**2026-10-30 night (latest), performance session, STATE (read first):**
- Game-engine RE / CPU (Phase VII): net cost maps in docs/effects/particles.md section 7 and docs/CPU_FINAL_HAUNT.md
  (last section). Landed in source, NOT committed: HLE slot index (find_hle one load; Final Haunt -0.7 %, gates
  byte-identical). Reverted: fp_compare force-inline (not demonstrably faster) - rebuild .build/diag before measuring
  again. FPSCR: native owners stay exact through the ppc_ops helpers (no open decision any more).
- Exact universal renderer (separate experimental line, tools/exact/, docs/EXACT_RENDER_SPEC.md v0 + v1, GPU_FILL.md
  23-26): exact on real frames on CPU / RTX 4080 / WARP; GPU mask binning exact; dense 5K frame 35.3 ms vs the game's
  hardware particles 11.1 ms - not integrated.
- Earlier tonight, in source and not committed: exact opaque write + exact alpha (renderer.cpp, mesh.hlsl; GPU_FILL 20),
  diagnostics (SHADOW_R2_IDENTITY_BLEND_OFF, PARTICLE_PROBE 32 / 64, OPAQUE_CHECK, EXACT_OPAQUE, EXACT_ALPHA,
  DUMP_PARTICLES), PrsAhead / glyph worker / NativeLinkGroups / kSkipClear (GPU_FILL 1-10).
- Particle dumps: .build/particle_dump/wst|fh (317 MB, regenerable). Tooling: tools/sample_subtree.py,
  tools/exact/particle_dump.py, tools/gpu_repro/*.

**2026-10-30 late night, performance session, UNIVERSAL EXACT ARCHITECTURE (docs/OPTIMIZATION_GOAL.md top;
docs/EXACT_RENDER_SPEC.md v0; docs/GPU_FILL.md sections 23-24).** NVIDIA interpolator RE stopped by directive (kept as
evidence, section 23). Specification v0 for particles + portable C++ reference + SM 6.0 integer compute kernel
(tools/exact/): CPU = RTX 4080 = WARP on every sample (640x400; RTX also 3840x1200 and 5120x1600). Kernel 2.86 ms vs
hardware path 1.33 ms at 3840x1200 (from 13.05 ms): performance gate not met yet; next optimisations in section 24.
AMD / Intel / Vulkan not validated (no hardware here).

**2026-10-30 night, performance session, EXACT-PIXEL ARCHITECTURE (docs/OPTIMIZATION_GOAL.md top; docs/GPU_FILL.md
sections 19-22).** Proven: the 4x mismatch is NVIDIA's blend path coalescing same-pixel fragments in flight (mean of
samples, timing-dependent, any blend state; blend off and float32 immune). Blend unit arithmetic modelled exactly
(0 / 5.9 M). Landed in source (not committed): exact opaque write + exact alpha (first differing operations fixed;
cost-neutral at 5K; CTest 131/131). Particles = 90 % of heavy-frame GPU; ROV exact but 4-11x slower; a compute
particle renderer needs the hardware interpolator reproduced (fit failed with IEEE formulas). Decision pending.

**2026-10-30 18:45, performance session, Phase III item 1: the tiled 4x mismatch is ROOT-CAUSED (docs/GPU_FILL.md
section 18).** On the RTX 4080 an ENABLED blend unit (even One/Zero) on 4x MSAA edge pixels stores a value that is
not the mathematical blend and depends on scheduling (scissor partition, even identical resubmission); blend unit
off is exact. Proven in a standalone repro (tools/gpu_repro/msaa_partition.cpp) and in the game: blend unit off +
no additive extra passes -> tiled == untiled, 0 differing pixel-frames at 16-bit. Next: blend unit off for
provably-identity draws, then a user decision on genuinely blended draws before tiling (-40% GPU at 5K) is enabled.

**2026-10-30: performance session - CURRENT GOAL: extreme 32:10 resolution, 1 ms complete GPU frame, zero quality
compromises (docs/OPTIMIZATION_GOAL.md "CURRENT GOAL").** Live handoff: docs/GPU_FILL.md (sections 1-13). Landed in
source (not committed; delivered to H:\NukkoroGit up to section 10): kSkipClear, glyph worker, NativeLinkGroups, PRS
members decoded ahead (+ ahead-of-read queuing, 4 workers). Diagnostics: GEKKO_FRAME_WORK, GEKKO_TASK_TIMES,
GEKKO_SELF_SAMPLE, GEKKO_GPU_TRACE_DRAWS, SHADOW_R2_STABLE_POWER, SHADOW_R2_SKIP_CLEAR, SHADOW_R2_PARTICLE_PROBE,
SHADOW_R2_DUMP_TEXTURE, SHADOW_R2_SCENE_TILES (diagnostic, 4x mismatch open), --tape-output-free. Next: section 13.

**2026-10-29: exact native math (the user: "replace systems with identical behavior lightweight x64 PC math").**
- Method: docs/NATIVE_MATH_METHOD.md (in the north star). Its section 4 is the worklist.
- Item 1, the game's MSL library, is done: `runtime/lift/math/msl`, docs/NATIVE_MSL_MATH.md.
  - It includes the large-angle reduction neither earlier copy had.
  - `native_msl_tests`: 1.8 million original calls, 0 differences, both NI modes.
  - Pose math now uses it: 105 clips byte-identical.
- Open:
  - Moving the sound and CharaColli copy onto it needs the user's decision to compare FPSCR's control byte instead
    of its status bits. The census shows no game code reads them.
  - Then lower the translated callers' MSL call sites to the library and prune the old bodies.
- Nothing committed or deployed.

**START HERE (user, 2026-10-26, "Save this for next session"): docs/DIRECTIVE_2026-10-26_NATIVE_ENGINE.md** - the
complete GameCube-architecture elimination directive (verbatim). The original code is an offline mathematical and
behavioural reference only; the production runtime must end with zero GameCube hardware dependencies (no PPC
interpretation, no GX/EFB/XFB emulation, no console register/MMIO/timing models, no 640x480 rendering base), replacing
each subsystem by a validated, measured native implementation and removing the old one; keep an explicit dependency
elimination report. The resolution work of the same day (docs/RESOLUTION_AUDIT.md, increment 28, not deployed) is
the rendering-resolution part of it and is done: exact WxH rasterization at every aspect, DXGI detection, launcher
presets, no bars (window takes the picture's shape; fullscreen offers only the screen's shape), full suite 118/118.
Its listed limitations (no exclusive fullscreen, primary display only, movies not checked per aspect) remain.
Same-day presentation follow-ups (RESOLUTION_AUDIT.md 9d, not deployed): 60 Hz-switch frame blanked, edge art at the
screen sides, backgrounds covered instead of stretched, the results-screen pupil flicker fixed (double-precision matrix
composition + skins posed about their own origin), real-time story cutscenes boxed 4:3 (2P intros excluded). Open: a
story cutscene has not been captured (no boot route reaches one).

Written 2026-10-26 at the end of the twenty-third increment; updated the same day after the twenty-fourth
(snapshot regions; see "Session 2026-10-26b" below). Read first: AGENTS.md, docs/NORTH_STAR_COMPACT.md,
README.md ("Current validation and optimization"), then this file. The user's standing rules for this work are the
north star and the "Native Execution Modernization Directive" of 2026-10-26: preserve observable behaviour, not the
console's method; measure first; runtime CPU-feature dispatch with fallbacks; architecture-independent compute
interfaces (future ARM64/NEON); cache-aware data; power/thermal as measured quantities; no fabricated or mocked
results, no weakened validation, failed experiments reported.

## State at handoff

| Item | Value |
| --- | --- |
| Current release candidate | 105,507,328 bytes, SHA-256 2db5170d000fa9ad7cffacc5e3763375cc68b81c876e9fac2e954919b413553b (.build/release; increment 25, sound phase 1, WARPHOLE silent as a deliberate deviation; = the measured 2f561e4a except 4 timestamp bytes). Increment 24 (1a0fd1ae) kept as .build/validation/sfx/control-release.exe; increment 23 (1ef49e04) as .build/validation/snapshot-regions/control-release.exe |
| Deployed game/ShadowTheHedgehog.exe | unchanged, fa1d49a6... (never replaced in increments 21-24) |
| Test executable in game/ | ShadowTheHedgehog.reverb-2026-10-26.exe (= the increment-23 candidate 1ef49e04). Added at the user's request to listen to the reverb; AGENTS.md keeps only the deployed and reference executables there, so remove it when the user says so. |
| Test executable in game/ (2) | ShadowTheHedgehog.snapshot-regions-2026-10-26.exe (= increment-24 candidate 1a0fd1ae). Launched for the user 2026-10-26 (--fullscreen --audio, the -full card, render at the window's 1920x1080; logs .build/play-session-regions). Remove when the user says so. |
| Test executable in game/ (3) | ShadowTheHedgehog.sound-tables-2026-10-26.exe (= increment-25 candidate 2f561e4a, WARPHOLE silent; it first held the withdrawn program-90 build ccd38863). Launched for the user in Lethal Highway (--boot-stage stg0202, fullscreen, audio, 2560x1440 render, -full card; logs .build/play-session-sound). Remove when the user says so. |
| Diagnostic build | .build/diag (lifted, SHADOW_SHIPPING=OFF: GEKKO_PRESENT_TRACE, GEKKO_GPU_TRACE, SHADOW_R2_WHOLE_SNAPSHOTS work). Incremental rebuild ~30 s; a fresh tree ~9 min. |
| Savestate version | 23 (room effect history). 19-22 load with warnings. |
| Full suite | 116/118 at increment 27; the two failures (rw_geometry_convert_tests, gx_texture_decode_tests) were the missing B45.mem1 fixture, restored 2026-10-26 as test_data/renderer-re/final-haunt-b45.mem1 (the original run's image, SHA-256 065e797a..., found at H:/RenderWare_RE/RENDERER_RE/_work/geometry/runs; both now pass, no expectation changed). |

Increments 21-23 (details in OPTIMIZATION_RESULTS.md):
- 21: render-target memory. Frame-graph lifetime sharing + own allocation for textures >= 1 MiB. 1440p Westopolis
  D3D12 usage 427.6 -> 357.4 MiB, steady private commit ~570 -> ~498 MiB. On this RTX 4080 driver every default-heap
  byte is also process private commit (standalone probe).
- 22: AX voice selection (unchanged-chain fast path) and ADPCM run decoder, exact against verbatim old code; AX mix
  time -23% (Westopolis) / -31% (Final Haunt); PCM byte-identical.
- 23: the room reverb is heard. Mixer owns EffectStage; ReverbHi callback is a native owner; translated callback,
  crossfeed and HandleReverb pruned (-82,944 bytes). Captures and CPU state identical; every changed guest byte
  attributed.

User play session on the candidate (2026-10-26, .build/play-session-reverb/session.json): 216 s, 12,456 frames, 0
render failures, average 57.6 FPS, interval p99 33.6 ms, max 592 ms; one 1.25 ms audio underrun at audio time
24.8 s; 6 clipped samples; peak private 505 MiB, final 350 MiB; 0.20 logical cores. Not attributed yet.

## Session 2026-10-26b: what was found and done (details: OPTIMIZATION_RESULTS.md, twenty-fourth increment)

- The "29% quanta > 17 ms" was a misread metric (boundary-to-boundary wall time, sleep included); the game thread
  kept up (2 behind boundaries in 12,985). Stutter lives in delivery to the display.
- The session JSON / stdout "present intervals > 20 ms" are CPU-side render/present returns, NOT displayed frames
  (a run with 258 such intervals had 7 displayed repeats). Judge stutter with GEKKO_PRESENT_TRACE +
  tools/present_trace_report.py on the diag build (repeats by cause: packet_late / render_late / gpu_late).
- Test route the user asked for (2026-10-26: "Use the westopolis gameplay replay for testing new code"):
  --boot-stage stg0100 + .build/recordings/westopolis-cpu-spike/input.tape, isolated persistence copies from its
  initial/. Not the cold-boot tape replay: that one diverged into movies (movie timing follows the live audio clock).
- Owner found: GPU-late repeats came in clusters from distortion snapshots, each a full 1440p 4x resolve (~1 ms, up
  to 2.5 ms). Fixed: a snapshot resolves only the texels its distortion draws can read (snapshot_reads.h). Exact
  (no-MSAA captures identical in 100/100 frames; 4x within the GPU's own 1-LSB run variance). Display A/B: repeats
  18, 20 -> 7, 7; drops 42, 51 -> 2, 5; GPU frames > 16.6 ms 21, 35 -> 0, 2.
- Display measured 60.00057 Hz vs the 60.000 Hz pacing ladder: ~10 ppm, one refresh of phase drift per ~29 min; the
  push phase relative to vblank is random per run. Pushes landed 6-8 ms after vblank in the measured runs.
- Startup stretch: still not reproduced. Headless game frames are correctly framed; a 15 s fullscreen screen grab
  of startup showed no stretch (the PAL 60 Hz answer comes later on the tape). Next: grab across the user's own
  boot (they answer the prompt by hand) or record when it happens.

Next, in order:
1. Remaining GPU-late frames: motion-trail moments (ticks 3045-3065 on the replay): scene 7 ms + second scene pass
   3.4 ms + view resolve 3.2 ms + history resolve 1.8 ms. The full-view resolves run far below bandwidth; check GPU
   clock behaviour under the light 60 Hz load (NVAPI/PDH clocks if available without a driver) before optimizing.
2. The remaining packet_late / render_late repeats (2-3 per 90 s) and the three stage-start repeats (ticks 16-26).
3. Then the plan below (CPU attribution, glare churn, RAM).

## Increment 32 (2026-10-27): object draw distance (native far-object proxies) + stage restart tool

- Pop-in = the layout objects' authored range (entry +0x23, x100 world units). NativeObjectRange owns the four range
  functions and keeps them bit-exact: scaling the range changed gameplay (replays diverged at x2/x4/x6) and at x8 ran
  the game's heap out in Final Haunt (bad_alloc -> abort at ~101 s). The user's rule: no cap, no heap growth.
- Draw distance is now render-only: `scaffold/rw_driver/far_objects.*` draws every unspawned spawn candidate's model
  from the game's own data - ART GADGET via the native resource-manager walk to ARTISTGADGET.BIN -> parts -> DFF
  RpClumps - wherever it covers a pixel, with a handover at spawn. `--draw-distance` (default 8; 1 = original), launcher
  slider. Westopolis and the full Final Haunt tape: 100% resolved, no heap use, frames identical except far props
  (docs/DRAW_DISTANCE.md). Also SPRING, DASHPANEL, JUMP_BOARD, CONTAINER, WEAPON_CONTAINER
  (kRules; each measured error 0 at handovers on both tapes). Open: HRD_CONTAINER sample, RING (per-slot collected
  bitset), other containers; the fade-in (appear_fade.h) stays off (effects).
- `tools/stage_control.py` + dev-link `restart_stage`: the pause menu's Restart route (docs/tools/STAGE_CONTROL.md).
- Not deployed, not committed.

## Increment 31 (2026-10-27): plan B2 - one time base

- The periodic slowdown was the console's 59.94 Hz retrace beating against 60 Hz frames, not the GPU clock. Frames,
  retrace and audio passes now sit on exact 1/60 s ticks; the loading screen's console-field draw throttle (li
  r0,0x412B in fn_802BE9D4) is a new platform value site answered by NativeVideo (FRAME_PACING.md "One time base",
  OPTIMIZATION_RESULTS.md increment 31). Not deployed. NEXT: draw distance / pop-in (user request), then B4.

## Increments 29-30 (2026-10-26): presentation follow-ups, B1, B3a

- 29: 4:3 boxed story cutscenes (EventManager byte r13+0x3E5D, 2P excluded), backgrounds covered, pupils fixed by
  double-precision matrices (RESOLUTION_AUDIT.md 9d).
- 30: B1 history saves resolve straight into the history; B3a glare composite folded into the resolve when only 2D
  follows (Final Haunt composite + resolve 3.80 -> 2.49 ms; ~11 px/frame 1 LSB where the GPU collapses edge samples,
  accepted). renderer.cpp now takes its math from render/frame_math.h (shared with the Vulkan backend in
  runtime/lift/render_vk, started by a side session; docs/VULKAN_RENDERER.md). NEXT: B2 (GPU clock state), B4.

## Increment 27 (2026-10-26): plan B3b + B3 done

Skipped draws are counted by reason (the one-off Lethal Highway skip did not recur in 4 more runs; the counters will
name it). Pooled render targets are kept 60 s instead of 3 frames: pixel-exact over 100 frames across the glare pause,
peak commit 566.8 -> 516.9 MiB, the 5.4 ms target recreation gone (OPTIMIZATION_RESULTS.md, twenty-seventh
increment). Not deployed. NEXT: B1 (motion-trail resolves), B2 (GPU clock state), B3a (Final Haunt glare composite),
B4. Rendered runs: ask the user; they chose windowed for these checks.

## Optimization plan Phase A done (2026-10-26)

All five measurements ran (OPTIMIZATION_RESULTS.md, "Phase A of the optimization plan"; OPTIMIZATION_PLAN.md Phase A
summary). Headlines: the GPU sits in P5/P8 at 570-780 MHz and every over-budget frame landed on a low clock; the
"+55 MiB stage-load peak" is glare-target churn (57.5 MiB committed ~46 s after one recreation); game-thread time by
class is ranked (Shadow 10%, Sonic NPC 10%, effects ~11%, collision 4-7%). New tools: gpu_clock_sampler.py,
native_sample.py --stacks, guest_classes.py, stack_report.py (CTest profiling_tools); the session report lists
per-thread cycles. NEXT: B3b skipped-draw counters (one unexplained skipped draw in one Lethal Highway run), then B3
glare keep-alive, then B1/B2/B3a/B4. Rendered measurement runs are fullscreen on the user's monitor - ask first.

## High-level plan

docs/OPTIMIZATION_PLAN.md (written 2026-10-26 at the user's request) is the current CPU / RAM / GPU plan and its
order; the "Plan, in order" section below is the earlier, more detailed version it summarizes and reorders.

## Sound effects (2026-10-26): stray menu sound fixed; native sound path phases 1-2 done (increments 25-26)

Root cause (docs/GCAX_SOUND_MAP.md, top section): AudioDataStageCity.bin row 90 `0x7160 SE_Z1090_WARPHOLE` is
unfilled in the shipped PAL data (bank 0 / program 0), so every Black Arms tank warp-in played B0 program 0 =
SE_SY00_WINOPEN, the menu window-open sound. A Dolphin reference trace of the retail disc proved the ORIGINAL does the
same (an original-game bug). The user decided to keep the warp-ins silent as a deliberate, documented deviation: the
row is disabled. Reference tooling for any future "does the original do this?" question: tools/dolphin_dump.py and
the private Dolphin user folder .build/validation/sfx/dolphin-user (the user's Dolphin: G:\Dolphin\Dolphin-x64;
retail ISO F:\ISO!\SHADOW_THE_HEDGEHOG_PAL.iso; never use the Dolphin under H:\SONIC_COLORS_WII_DISK_..., another
agent's workspace). User decisions: replace the GCAX sound path natively in phases, each checked against the original
code. Phase 1 (native SE table lookup 0x801D88CC + the erratum) is done and validated (OPTIMIZATION_RESULTS.md,
twenty-fifth increment). Phase 2, the native SE manager, is done (increment 26: GCAX push, request wrappers, gates
and FrameSE native; the manager internals and positional math pruned; push streams and PCM identical to the
translated manager on both replays). NEXT for sound: phases 3-4 (native voices, native bank store; docs/GCAX_SOUND_MAP.md).
The user's order was sound phase 2 first, then the optimization plan (docs/OPTIMIZATION_PLAN.md, Phase A). Test routes:
the Lethal Highway recording (.build/recordings/lethal-highway-sfx: --boot-stage stg0202 + its input.tape and
initial/) and the Westopolis gameplay replay; tools: GEKKO_AUDIO_AUDIT + `--watch` in the diag build.

## Open hazards and known gaps

- extract/liveness-roots.txt is generated from EVERY 0x8xxxxxxx literal in runtime/ code, tests included. Regenerate
  (tools/ppcrecomp/liveness_roots.py --out to a new file, then copy) after any such literal changes, and run the
  native_liveness_roots CTest. It was stale after increment 22 until increment 23.
- extract/native-owners.txt must equal the union of every owner's addresses() list (native_file_api_tests).
- PowerShell Copy-Item keeps the source mtime: MSBuild then skips recompiling. Touch copied sources.
- Idle MSBuild node-reuse workers trip stage_benchmark's concurrency guard: set MSBUILDDISABLENODEREUSE=1.
- Bash heredocs collapse backslashes: write edit scripts with the Write tool and run them with python.
- ax_pb_tests --adpcm-loop-oracle (opt-in, not in CTest) fails identically on the pre-increment-22 source.
- ReverbHiInit/Shutdown still run as translated SDK code and allocate ~80 KB of delay lines the native effect no
  longer uses (SCAFFOLDING until the sound system's effect setup is lifted).
- The wall-time IP sampler (tools/native_sample.py) overstates code that runs while the thread is often preempted
  (AX was ~3x too high). Rank with timers or cycle counts; use the sampler to locate, not to size.
- Process CPU seconds vary +-15% between identical runs (paced and headless). No CPU claim has been possible at
  process level; every claimed CPU change so far is a timer- or kernel-level measurement.

## Measured baseline (Westopolis input tape, 1440p, RTX 4080, 32 logical processors)

- CPU (40 s paced): game thread 6.08 s, main-ui 0.42 s, audio 0.19 s, render-resource-worker 0.14 s; game thread
  waits ~80% of wall time. In-image game-thread samples: generated game code ~60% with no function above 3%;
  paired-single/compare/indirect-call helpers ~5%; AX (after 22/23) about 0.5% of a core plus the effect at ~25 us
  per 5 ms block.
- RAM: private ~498 MiB steady at 1440p, of which ~360 MiB mirrors D3D12 default heaps; guest memory ~25 MB; 16 MiB
  card image; 16 MiB sample memory; 1,356 MiB of asset files mapped (shared, not private). Peak private is ~55 MiB
  above steady during stage load (not attributed).
- VRAM (1440p): view colour 4x RGBA16 116 MiB, view depth 4x D32 60 MiB, glare sources 4x RGBA8 60 MiB (released and
  recreated when glare pauses > 3 frames), view 30, history 28.8, output 15; assets + meshes ~21 MiB.
- GPU time: per-pass GPU timestamps exist (GEKKO_GPU_TRACE, render/gpu_timer.h) but no baseline table was taken.

## Priority set by the user (2026-10-26): occasional slowdowns, not temperature

The user reports a consistent CPU temperature but slowdowns "here and there sometimes". Their play session
(.build/play-session-reverb/session.json) shows where: the 60 Hz simulation quantum exceeded 17 ms in 3,807 of
12,984 quanta (29%); quantum p95 19.9 ms, p99 23.9 ms, max 60.2 ms; the pacer caught up without skipping
(maximum debt 27.3 ms), so game speed held but motion stuttered. The presentation queue dropped 86 packets and ~1%
of present intervals were two refreshes (p99 33.6 ms); one 592 ms interval (probably a load). 99.6% of game-thread
placement samples were on performance cores with throttling disabled: the cost is the game thread's own work, not
scheduling. Workstream 0 below therefore starts with attributing over-budget quanta; power/thermal moves last.

FINDING (2026-10-26, next session, from the same session's stdout frame-tail): the "29% of quanta > 17 ms" figure is
NOT over-budget work. A quantum is the wall time between two observed boundaries, sleep included; the guest normally
crosses boundary n right after waking, so a quantum is 16.67 ms + (work before crossing n) - (work before crossing
n-1). The 612 quanta >= 20 ms average guest 6.5 + sleep 16.0 ms, and each is followed by a short one (mean 14.0 ms,
430 of 612 below 16 ms): heavy guest frames of 12-15 ms that still fit the budget. Only 2 of 12,985 boundaries were
behind (no sleep), maximum debt 27 ms; the one 60 ms quantum is the eggmonitor.one load (43 ms native execution).
Game speed held. The visible stutter is therefore in delivery to the display: 86 dropped packets, 480 present
intervals > 20 ms. Hypothesis under test: the pacer's 60.000 Hz steady-clock ladder is not phase-locked to the
display's vblank (display reports 60/1 Hz), so the push time (wake + 2..15 ms of guest work) straddles the vblank the
render thread needs; a late push repeats the previous frame, and the next vblank sees two packets (one dropped).
Tooling: tools/present_trace_report.py (+ test, in CTest presentation_analysis) attributes every repeated refresh
to packet_late / render_late / gpu_late from a GEKKO_PRESENT_TRACE capture, which needs a non-shipping lifted build
(.build/diag: cmake --preset release -B .build/diag -DSHADOW_SHIPPING=OFF).

Second user observation (2026-10-26): when the game starts there is a momentary stretched picture while the game
sets up its resolution. Not reproduced or attributed yet. The session log says the output is 1920x1080 from game
tick 0 and the 2560x1440 render is aspect-locked "to the game's output when it is set", the window is borderless
fullscreen, and renderer Start() took 188 ms. Leads to check, not conclusions: frames presented before the
fullscreen window or swap chain reaches its final size, or before the game's own aspect/output setting is applied.
Reproduce with a startup frame capture (window capture of the first seconds, per-frame present sizes), find the
first frame whose framing differs, and fix the owner of that state rather than hiding frames.
Checked 2026-10-26: headless captures (release exe, cold boot, Westopolis tape, 960x540) of frames 1-600 (the PAL
"display in 60Hz mode?" prompt) and 1000-1899 (answer, "Screen display has been set to 60Hz mode.", autosave notice,
SEGA logo) are all correctly framed (.build/validation/startup-stretch/). So the game's frames are not stretched;
the stretch is on the presentation side (window / swap chain / render-size scale pass) and needs an on-screen grab.

## Plan, in order

Every workstream follows the directive's loop: locate the cost with a reproducible measurement, record the
reference behaviour, change one thing, prove equivalence (exact where the output is exact; captures, PCM, state
attribution for gameplay), measure paired and alternating with all samples kept, keep it only on evidence.

### 0. Measurement foundation (do first; everything else depends on it)

0. Slowdown attribution (first): log every simulation quantum's duration with its game time, the stage and what
   the game thread did in it (guest execution, AX, asset reads/decompression, packet production, waits), so each
   over-budget quantum can be tied to a gameplay moment and an owner. Reproduce on the recorded Westopolis tape and
   on a stage-load route; sample call stacks (item 2) only inside over-budget quanta. Fixed-step semantics stay:
   the fix is less work per quantum, never a longer or skipped step.
1. Per-thread CPU cycles: add QueryThreadCycleTime per named thread to the session report (no admin rights needed;
   cycles are less sensitive than wall CPU time to frequency and scheduling). Check identical-binary spread
   before using it for any claim.
2. Inclusive CPU attribution: extend tools/native_sample.py from exclusive IP to call stacks (RtlVirtualUnwind on
   the suspended thread's context using the image's unwind data, which the sampler already reads). The flat
   exclusive profile cannot show which game subsystem (by caller) owns the 60% of generated code.
3. GPU: a 1440p per-pass GPU-time table from GEKKO_GPU_TRACE, plus present/scanout pacing; repeat to get spread.
4. Frame-time attribution: the play session's p99 33.6 ms / 592 ms max and the 1.25 ms underrun. Correlate
   frame-interval spikes with stage loads, pool texture creation, PSO creation, upload waits and asset reads
   (the session JSON/CSV fields in docs/PRESENTATION_TELEMETRY.md).
5. Power/thermal (lowest priority: the user observes a stable temperature): find what this machine can measure
   without a kernel driver (Windows Energy Meter Interface, PDH processor counters, frequency). Report what is
   unavailable rather than inventing proxies.

Gate: identical-binary calibration for each new metric, all samples retained, tools unit-tested.

### 1. GPU and VRAM

- Glare chain churn: 60 MiB 4x MSAA + 15 MiB targets created and released whenever glare pauses for more than three
  frames (TexturePool::end_frame keep_frames = 3). Measure creation cost and hitch correlation before choosing a
  policy (longer keep-alive, or aliasing with other transients via placed resources); VRAM and private commit move
  together on this driver.
- Per-pass costs at 1440p: 4x RGBA16 scene, resolves, glare. Optimize only passes that dominate the measured table;
  visual output must stay byte-identical (capture gates) or the change is a renderer-quality decision for the user.
- Upload stream (16 MiB ring) and per-frame upload volume: check ring waits and redundant re-uploads.

### 2. CPU (game thread first)

- With inclusive attribution, rank game subsystems by cycles; pick the top bounded subsystem whose contract can be
  recovered from the DOL and rewrite it natively (north star), with original-code oracles as in increments 22-23.
- Hot helpers (psq_load/psq_store, fp_compare, call_indirect): inspect generated call sites; measure inlining or
  specialization by quantization type at kernel level, then in gameplay.
- Runtime ISA dispatch (directive section 3): the image currently requires AVX2 at startup (host_isa_gate). Design a
  dispatch layer for the few measured kernels (mixer gains, reverb, matrix math) with SSE2 fallbacks and an
  architecture-neutral interface; measure before multiplying code paths.
- Thread wakeups: the host job pool reports 9 workers with zero groups/items in the paced route; verify they sleep
  without periodic wakeups (count context switches per thread) and size the pool to the work.

### 3. RAM

- Attribute the ~55 MiB stage-load peak (tools/memory_map_sampler.py with a short walk gap during the load).
- Card image (16 MiB private) and sample memory (16 MiB): measure which parts are touched; consider file-backed or
  demand-committed storage only if saves/persistence stay byte-identical (never modify the user's cards).
- Unused reverb delay-line allocation in guest memory: removable only together with lifting the effect setup.

## Commands and tools

```powershell
$env:MSBUILDDISABLENODEREUSE = "1"
cmake --preset release; cmake --build --preset release
cmake --preset tests;   cmake --build --preset tests; ctest --preset tests -j 2
```

- tools/memory_map_sampler.py: process commit by region kind/allocation base over time (add
  --env GEKKO_GPU_MEMORY=gpu.csv for named D3D12 allocations).
- tools/native_sample.py: wall-time IP samples with module attribution outside the image.
- tools/stage_benchmark.py: isolated paired runs (headless or --rendered) and --verification (state + capture).
- tools/state_delta.py: byte-exact snapshot comparison (v19-v23; trailing length differences counted).
- Headless gameplay gate used in increments 22-23: --headless --audio-dump audio.pcm --state-report state.json
  --native-screenshot frame.png --native-screenshot-frame N --save-state --save-state-file snapshot.gekkostate,
  alternating control/candidate/candidate/control, with isolated copies of the persistence files from
  .build/recordings/westopolis-cpu-spike/initial/ and its input.tape. The guest-byte attribution script used for the
  reverb is .build/validation/reverb-integration/attribute_delta.py (disposable; promote to tools/ with a test if
  reused).
- Never run builds, benchmarks or CTest while a ShadowTheHedgehog* process is running: that is the user playing.
