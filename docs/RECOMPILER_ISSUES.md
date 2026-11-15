# Recompiler and runtime issue map (2026-11-12)

Author: agent recomp-map, branch `agent/recomp-map` at ce59357 (integration tip: optimization rounds 1-3 merged, 0
GameCube SDK bodies in the shipping build). Scope: the generator (`tools/ppcrecomp`), generated-code support
(`runtime/include/gekko/ppc_ops.h`, `recomp.h`, `cpu.h`), the CPU core and dispatch (`runtime/src/gekko/cpu.cpp`,
`flat_memory.cpp`), the native OS layer ("kernel": `native_os_*.cpp`, `native_interrupts.cpp`,
`native_vi_wait.cpp`, `gameplay_scheduler.cpp`, `os_exception_entry.cpp`, `system.cpp` wait/advance code), and the
failing ctests. This file is the work list for the fixer; it changes no code.

How this was made:
- Read the code. Every test run used `tools/agents/capped_run.ps1 -CpuPercent 6`, from the tests tree
  `wt-opt-integrate/.build/tests` (Release, built at ce59357; that worktree's HEAD has since moved to ea0ba98, a
  docs/tools-only merge). Nothing was rebuilt, deployed or timed under the `benchmark` lock.
- Three helper agents did the kernel, generator and failing-test parts; their findings are merged here with their
  evidence. The CPU / dispatch / memory / FP part is this agent's own.
- Generated shards were read in `wt-opt-integrate/.build/watch/generated` (lifted, 48 shards) and
  `.build/tests/generated` (reference, 64 shards). Probe scripts are in the session scratchpad (`probe_gen.py`,
  `probe_owner.py`, `census.py`, `dolpeek.py`, `sampler.py`); they are not part of the project.
- "Not verified" marks anything that was not run. No in-game run was made: every in-game consequence below is not
  verified.

Ownership marks (coordinate before editing):
- **[opt-runtime]**: session opt-runtime (s3/*) owns `tools/ppcrecomp` emission, the guest memory access path
  (`cpu.h` `fm_*` accessors, `flat_memory.cpp`, a pending volatile-store change) and `recomp::call_indirect`'s table
  path.
- **[opt-host2]**: agent opt-host2 owns the HLE dispatch internals: `try_hle*`, `find_hle`, `serve_native_owner`,
  the slot filter (`rebuild_hle_slots`, `hle_hot_`).

## Summary table

Ranked by impact x confidence. Confidence: P = proven, L = likely, S = suspected.

| Rank | ID | Area | Issue | Impact | Conf. | Owner |
|---:|---|---|---|---|:-:|---|
| 1 | RC-tests-3 | tests | compiler_body_tests loops forever: decrementer delivery off since a7da0b9, test arms DEC from C++ | hang (25 min CPU per run), wait-point contract untested | P | |
| 2 | RC-tests-1 | tests/render | unorm calibration target (NOT_ZEROED heap) drawn without Clear/Discard: render_core_tests red | correctness (GPU UB), 2 tests red | P | |
| 3 | RC-kernel-1 | kernel | a stop inside native OSSleepThread / VIWaitForRetrace leaves a non-resumable pc, r1, r3 | savestate / resume corruption | L | |
| 4 | RC-gen-2 | generator | owner-interior rule enforced only for cross-row branches; tests tree runs owner words natively | correctness (owner bypassed) | P | |
| 5 | RC-gen-1 | generator | liveness roots read as given; staleness only caught by a ctest, never by the build | correctness (unsound pruning) | L | |
| 6 | RC-kernel-2 | kernel | wait point inside a drive_sync callback livelocks (advance_game_time refuses while wait_active) | hang | L | |
| 7 | RC-dispatch-1 | dispatch | handler pointer / HleEntry pointer used after the handler ran; install_hle during a handler moves the running std::function | UB (use after move/free) | L | opt-host2 |
| 8 | RC-gen-5 | generator | no gate that every live listed wait point was emitted with its wait | latent hang | P | |
| 9 | RC-tests-6 | tests | f1c canary driver stale: 1024-slot HLE table, zero OS vector words | test red, guard lost | P | |
| 10 | RC-kernel-5 | kernel | FPR-shadow policy set only in shadow_main; tests' Systems run without it; docs stale | test validity | P | |
| 11 | RC-tests-2 | tests | render_determinism red, same cause as RC-tests-1 | test red | P | |
| 12 | RC-memory-1 | memory | 4 GiB windows have no guard: an access wrapping past 0xFFFFFFFF touches host memory after the window | silent host corruption or crash | L | opt-runtime |
| 13 | RC-kernel-3 | kernel | drive_sync does not preserve GQR1-7 / FPSCR of the waiting thread | correctness | S | |
| 14 | RC-gen-3 | generator | jump tables feed liveness only; 871 of 2,479 live case words (35 %) leave the dispatching row | perf | P | (adjacent opt-runtime) |
| 15 | RC-tests-5 | tests | difftest_f1b spec flushes NI input denormals of ps_res / ps_rsqrte; engines follow hwtests | test red | P | |
| 16 | RC-tests-4 | tests | dolrecon_lift_tests: 34,045 functions in one test, ~13,600 CPU-s vs 1800 s timeout | test never finishes | L | |
| 17 | RC-gen-6 | generator | backward computed transfers (bctr/blr to a lower address, other row) have no recognition point | hang (stop/interrupt starved) | S | opt-runtime |
| 18 | RC-memory-2 | memory | process-wide VEH services a guest-window fault on any thread, mutating Cpu state off the game thread | race (latent) | S | opt-runtime |
| 19 | RC-fp-2 | fp | host MXCSR (rounding, FTZ/DAZ) never set or checked on the game thread | determinism (latent) | S | |
| 20 | RC-kernel-7 | kernel | no progress detector for native waits that never end; dead kNoDeadline branch | diagnosability, latent hang | P | |
| 21 | RC-dispatch-2 | dispatch | dispatcher find_hle per iteration + shared slots: 0.5-0.6 % of the game thread | perf | P | opt-host2 |
| 22 | RC-tests-7 | tests | lift_purity: 18 GameCube terms in runtime/lift comments (never passed in this history) | test red | P | |
| 23 | RC-tests-8 | tests | lift_scaffold_inventory: 6 rw_driver files miss the marker's `: why` part | test red | P | |
| 24 | RC-fp-1 | fp | fctiw/fctiwz of a negative value converting to 0 must give high word 0xFFF80001 (hwtests); both engines give 0xFFF80000 | fidelity | P | |
| 25 | RC-kernel-6 | kernel | run_pending_event HostWork contract, its test and run_upcall's comment disagree with the code | test gap | P | |
| 26 | RC-gen-4 | generator | shard layout depends on the build dir's previous recomp_layout.csv | perf A/B confounded | P | |
| 27 | RC-gen-10 | cpu+generator | mcrfs does not clear copied bits; mtfsf/mtfsfi/mtfsb do not recompute FX/FEX/VX; traps lack SRR1 bit 14 | correctness (low) | P | |
| 28 | RC-fp-3 | fp | FPSCR[RN] ignored and arithmetic FPSCR status unmodelled; no guard if the guest sets RN | latent correctness | P | |
| 29 | RC-gen-9 | generator | bclrl lowered as a return (unwinds) instead of a call (10 sites, tests tree) | perf (reference) | closed: by design (host stack) | opt-runtime |
| 30 | RC-gen-7 | generator | imprecise FallbackReason / missing site in call_indirect, jump_indirect, fp_unavailable; dead exit_at | diagnostics | P | opt-runtime |
| 31 | RC-gen-8 | generator | cold `_at` with an unknown entry silently starts at the function's first instruction | correctness (defensive) | P | |
| 32 | RC-kernel-4 | kernel | drive_sync zeroes FPRs; its comment contradicts __OSLoadFPUContext; deliver() does not | fidelity, misleading | P | |
| 33 | RC-kernel-8 | kernel | thread-context upcall bridge unreachable in lifted build; its FPR restore wrong in reference mode | maintainability | L | |
| 34 | RC-gen-12 | generator | malformed list lines accepted silently (bare strtoul) | robustness | P | |
| 35 | RC-gen-11 | generator | `--hot-set` emits accessors that no longer exist | maintainability | P | opt-runtime |
| 36 | RC-kernel-9 | kernel | external line and native_interrupt_first are production-dead; cpu.h says otherwise | maintainability | P | |
| 37 | RC-kernel-10 | kernel | timeline.cpp dead, with a latent race | maintainability | P | |
| 38 | RC-kernel-11 | kernel | SHADOW_NATIVE_JOBS=1 mode cannot run in the single-thread configuration | maintainability | L | |
| 39 | RC-kernel-12 | kernel | UB in native OSReport width/precision parse | UB (unreachable in practice) | P | |
| 40 | RC-gen-13 | generator | liveness address formation misses register copies / chained lows | correctness (low) | S | |
| 41 | RC-gen-14 | generator | generator-level coverage gaps (list) | test gap | P | |
| 42 | RC-kernel-13 | kernel | kernel test gaps (list) | test gap | P | |

Counts by area: tests 8, kernel 13, generator 14, dispatch 2, memory 2, fp 3 (RC-gen-10 counts under generator) = 42.
Counts by confidence: proven 29, likely 8, suspected 5.

---

## Issues in full

Fields: where; what is wrong; evidence; impact; confidence; fix direction; verify; fix risk.

### RC-tests-3: compiler_body_tests never finishes (no output, ctest timeout)

- **Where:** runtime/tests/compiler_body_tests.cpp:248-300 (`wait_point_contract`); native_boot.cpp:139
  (`cpu.set_decrementer_delivery(false)` in `NativeBoot::install`, called from `System::start`, system.cpp:3306);
  cpu.cpp:1631-1648 (`latch_decrementer` drops the edge when delivery is off); CMakeLists.txt:3448 (no TIMEOUT).
- **What is wrong:** commit a7da0b9 (2026-11-07, "Native boot step 3: the decrementer exception is not delivered")
  turned delivery off at boot; only a guest `mtdec` turns it back on (ppc_ops.h:1202-1205). The test arms the edge
  with C++ `c.set_decrementer(kIdleEdge)`, so delivery stays off. The generated SelectThread idle loop (0x803788FC)
  then waits forever: every `idle_wait` moves the clock to the next event, the decrementer re-arms 2^32 ticks later,
  `needs_dispatch()` never becomes true. The PASS lines go to fully buffered `std::cout`, lost when ctest kills it.
- **Evidence:** capped ctest run, no output; stopped after 373 s wall / 367 CPU-s (~1 core). 150 stack samples: all in
  the generated function for 0x803787B0-0x80378954, at the loop reading r13+0x5AE0, calling the idle wait, testing
  two flags, jumping back (~60 % in ntdll+0x16a450 called from it, 37 % on the polled read). Another agent's copy in
  wt-agent-opt-host2 (PID 57856, not ours, left running) had >2,000 CPU-s at the same point.
  docs/agents/audio-ax.md:183 (06820f9) records a pass before a7da0b9.
- **Impact:** burns a core for 25 min in every full ctest run; the wait-point contract has had no coverage since
  2026-11-07.
- **Confidence:** proven (mechanism and location); the a7da0b9 link is by code + the earlier pass, not bisected.
- **Fix direction:** in the test's `prepare()`, `c.set_decrementer_delivery(true)` (the test models a guest that
  armed DEC) or drive the wait by another deliverable event; add a bounded watchdog that fails instead of hanging;
  unbuffered stdout or flush per line; `TIMEOUT 180` at CMakeLists.txt:3448. Do not flip the Cpu default.
- **Verify:** the test passes in seconds and prints both `PASS SelectThread idle loop waits ...` lines.
- **Risk:** low (test only).

### RC-tests-1: render_core_tests red: calibration target drawn uninitialised

- **Where:** runtime/lift/render/unorm_calibration.cpp:147-165 (`copy_through`); gpu_device.cpp:311
  (`ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED`); renderer.cpp:326 (`direct_resolve_mode()` returns -1 when
  `SHADOW_R2_DIRECT_RESOLVE` is unset) and :1296 (`ensure_unorm_calibration` on the first frame).
- **What is wrong:** `shadow.unorm-calibration.target` is created with `HEAP_FLAG_CREATE_NOT_ZEROED` and drawn into
  (`DrawInstanced`, :165) with no Clear/Discard. The D3D12 debug layer errors on every renderer test. Came in with
  snapshot f5ec9b3 (2026-11-07).
- **Evidence:** `ctest -C Release -R ^render_core_tests$ --output-on-failure -V`: 2.58 s, `28 failure(s)`, all
  `ID3D12CommandQueue1::ExecuteCommandLists ... D3D12_HEAP_FLAG_CREATE_NOT_ZEROED ... must be initialized with a
  Discard/Clear/Copy ... 'shadow.unorm-calibration.target' ... DrawInstanced`. With `SHADOW_R2_DIRECT_RESOLVE=0`:
  passed in 0.57 s.
- **Impact:** two tests red (with RC-tests-2). On GPUs with render-target compression metadata the draw into an
  uninitialised target is undefined, which can corrupt the measured thresholds and so direct-resolve output bytes.
- **Confidence:** proven.
- **Fix direction:** `list->DiscardResource(target.resource.Get(), nullptr)` (or a ClearRenderTargetView) after
  `OMSetRenderTargets` (:155), before :165.
- **Verify:** render_core_tests and render_determinism pass with the variable unset.
- **Risk:** very low.

### RC-kernel-1: a stop inside the native OSSleepThread / VIWaitForRetrace wait leaves an unusable resume point

- **Where:** native_os_threads.cpp:81-93 (`serve_sleep_thread`); native_vi_wait.cpp:28-29, 33, 46; cpu.cpp:834;
  native_interrupts.cpp:662 (`drive_sync` writes `c.gpr[3] = result`); system.cpp:2064-2068;
  shadow_main.cpp:4666-4670, 5482-5487.
- **What is wrong:** the wait loop ends on `c.stop_requested()` while the thread is not woken and still returns
  true. The host service inside `c.idle_wait()` can request that stop (savestate, development link, snapshot, budget;
  `request_stop_between_traversals` stops at once whenever the gameplay scheduler is idle: boot, menus, loading,
  movies). `serve_native_owner` then skips `pc = lr` because `stopped()` (cpu.cpp:834), and nothing calls
  `leave_to_dispatcher`, so pc is the last return site published in the caller chain. r3 (the queue) is clobbered
  by `drive_sync`. Through VIWaitForRetrace, `after_disable` returns on `done(c)` with its 16-byte frame still
  pushed (r1 = sp0 - 16); re-entered at `kAfterDisable` it re-reads `r31 = r3` (garbage) as the saved EE flag.
  shadow_main.cpp:5482 assumes "cpu.pc is a real resume point". A host-thread stop (`request_stop_from_host`) is
  the inverse: `stopped()` is still false, `pc = lr`, so OSSleepThread "returns" unwoken (terminal stop, low impact).
- **Contrast:** native_frame_limiter.cpp:286-293, 322-337 handles the same case (`kZeroTimeWaitStopped`,
  `leave_to_dispatcher(kWaitResume)`).
- **Impact:** a savestate taken there, or a resume after `clear_stop`, continues at a wrong pc with shifted r1 and
  wrong r3/r31: corrupt state, wrong MSR[EE], or a hang.
- **Confidence:** likely (path proven by reading; not reproduced).
- **Fix direction:** in `serve_sleep_thread`, on a stop without a wake: restore `r3 = queue` and
  `leave_to_dispatcher(entry, FallbackReason::Yield, entry)` so re-running re-enters the wait. In vi_wait, on
  `done(c)`: leave to a continuation row at the loop head that keeps r30/r31 (guest_call restores r14-r31), or leave
  at `kAfterDisable` with `r3 = r31`.
- **Verify:** new cases in os_threads_contract_tests and the vi_wait case of rw_raster_contract_tests: the idle handler
  calls `request_stop` on its first call; assert pc/r1/r3/r31 form a resumable entry; `clear_stop()`; run on; compare
  with an uninterrupted run (vi_wait: against the original 14 words).
- **Risk:** low to medium (one continuation row; the owner lists may need it).

### RC-gen-2: "no code enters an owner except at its start" is enforced only for cross-row branches

- **Where:** tools/ppcrecomp/main.cpp:5175-5216 (the check); :3955 (D1 merge runs before owners are loaded at :4580);
  :4595-4599 (owner rows matched by row start); :5215 (owner-entry remap); cpu.cpp:2889 (`RecompiledCode::install`
  keeps the last of duplicate addresses silently).
- **What is wrong:** the check skips targets in the branching row's own reachable set and ignores fall-through, so an
  overlapping or D1-widened non-owner row can run an owner's original instructions natively. A native-owners line
  meant as an "entry" becomes a whole owner row when its address starts an accepted row. The remap at :5215 can
  collide with another row's entry, and a duplicate kFunctions address is accepted.
- **Evidence (probe_owner.py):** watch tree: 601 owner rows, 0 owner words covered by live rows. Tests tree: 10 owner
  words covered: `fn_803796CC_body` contains `goto L_80379700;`, inside owner 0x803796FC's extent
  [0x803796FC, 0x80379720); live row `fn_80376128` has range `{0x80376128u, 0x80376130u}` inside owner
  OSDisableInterrupts (0x8037611C, size 0x14). Duplicate kFunctions addresses today: 0 in both trees.
- **Impact:** correctness: an owner can be bypassed (its original body runs instead of, or interleaved with, the
  native owner). Present in the reference tree; latent in shipping.
- **Confidence:** proven (tests tree).
- **Fix direction:** refuse (or drop from reachable sets and ranges) any live non-owner row that intersects owned
  instructions unless the address is declared; run the D1 merge after owners are known and never widen into an
  owner extent; refuse duplicate kFunctions addresses (generator and `RecompiledCode::install`); make an "entry" line
  that coincides with a row start an error.
- **Verify:** a probe_owner-style assertion in `ppcrecomp_output_structure`; generate the tests tree.
- **Risk:** medium: the tests tree will refuse to generate until 0x803796FC and fn_80376128 are sorted out in the
  lists.

### RC-gen-1: liveness roots can be stale at build time

- **Where:** CMakeLists.txt:1076-1100 (generator custom command), :3621-3641 (`native_liveness_roots*` ctests);
  tools/ppcrecomp/liveness_roots.py:35 (docstring), :67 (literal-only match). Same pattern for jump-tables,
  wait-points, gx-sites lists.
- **What is wrong:** the generator reads `--prune-roots` as given. The build depends on the roots file, not on the
  runtime sources it is derived from, and `liveness_roots.py --check` never runs in the build. A runtime edit that
  adds a guest-entry literal builds fine and the newly entered row can be pruned; entering it then stops with
  NotRecompiled. Computed entries (`base + off`) root nothing, so "under-listing is impossible" is not strictly true
  (today the computed cases seen are LR values `+4u`/`+8u` passed to guest_call).
- **Evidence:** docs/agents/opt-intrinsics.md:144-146: liveness-roots-lifted.txt stale at an integration base (16
  unlisted addresses incl. 0x80414B80, 0x80416338, 0x80421358) and the build accepted it. docs/agents/opt-host2.md:
  the plain list made the tests-tree generator fail ("0x80416338 / 0x804213A0 branches into the interior of a native
  owner").
- **Impact:** correctness (unsound pruning -> runtime stop).
- **Confidence:** likely (staleness proven; a runtime failure not observed).
- **Fix direction:** run the `--check` scripts as part of (or as a PRE step of) the generator command, or generate the
  roots file in the build from a glob of runtime sources.
- **Verify:** add a literal in a runtime .cpp; the build must fail.
- **Risk:** low (seconds of build time).

### RC-kernel-2: a wait point reached inside a native-wait event livelocks; a host stop there abandons a callback

- **Where:** system.cpp:5364 (`advance_game_time` returns false while `cpu_.native_interrupt() &&
  interrupts_->wait_active()`), :5354-5358 (WaitScope resets the wall watchdog on every call);
  native_interrupts.cpp:619-664 (`drive_sync`); native_cri.cpp:163-165, 366; savestate.cpp:341.
- **What is wrong:** `wait_active_` stays true while `drive_sync` runs guest callbacks (`run_pending_event`,
  `run_upcall`, the CRI Idle pass) with EE cleared. A wait point or poll fixed point inside such a callback livelocks
  once an unmasked source is latched (by an event the wait delivered, or by the callback itself, e.g.
  `raise(HostWork)` at CRI lock leave, native_cri.cpp:366, or a card post): every `idle_wait` returns at once and the
  clock never moves; the watchdog cannot see it. NativeCri already counts waits inside the Idle upcall
  (`waits_in_idle_upcall`), so they are anticipated. Separately, a host stop raised inside `drive_sync` makes
  `guest_call` fail, the callback is abandoned, `sync_` is cleared, and the savestate.cpp:341 refusal no longer
  applies: a state with a half-run callback can be saved.
- **Impact:** hang (silent livelock); savestate corruption.
- **Confidence:** likely (mechanism by reading); reachability from those callbacks suspected; no test.
- **Fix direction:** `wait_active() && !sync_event_active()` at system.cpp:5364, or clear `wait_active_` around
  `drive_sync`; defer host stops while `sync_event_active()`, or record an "event abandoned" flag savestate refuses on.
- **Verify:** a test where a native-wait VI callback polls the time base (or the idle handler raises a second source):
  the clock must advance; a savestate test: a stop inside the sync event is refused.
- **Risk:** low.

### RC-dispatch-1: HLE handler storage is not stable while a handler runs  [opt-host2]

- **Where:** cpu.cpp:619-632 (`install_hle`: `push_back` + `std::sort` of `hle_`, then `rebuild_hle_slots`);
  :736-751 (`try_hle_slow`: `handler` points into `hle_[i].handler`; after the call `entry` is dereferenced for
  `hits`); :821-834 (`serve_native_owner`: `hot.handler`); :3109-3116 (reference dispatcher: `hook->handler(*this)`,
  then `const_cast<HleEntry*>(hook)->hits`).
- **What is wrong:** `HleEntry` (with its `std::function`) lives in a `std::vector` that `install_hle` reallocates
  and sorts. If a handler (or anything it calls) installs or replaces a hook, the `std::function` that is executing
  is moved or freed under it (its captures are then read from moved-from or freed storage), and the `entry`/`hook`
  pointer used after the call dangles (a write of `hits` into freed memory). The code anticipates the case: comments
  at :740 and :823 say "before the handler runs: it may install hooks (rebuild_hle_slots)", and only the index was
  hoisted; `hot_entry` is also stale after a sort (hits credited to the wrong entry). `handler_depth_` exists only in
  the reference build, so nothing can refuse an install from inside a handler.
- **Evidence:** code as quoted. The install sites checked (system.cpp:2304-2414, native_cri.cpp:445, native_gx_state,
  native_gx_display) run at install time, not inside a handler; whether any of the 347 `install_hle(` sites in
  runtime/src runs inside a served call was not proven either way.
- **Impact:** UB (use after move/free) the moment it happens; silent wrong `hits` otherwise.
- **Confidence:** likely (the defect is proven by reading; reachability suspected).
- **Fix direction:** count handler depth in all builds and make `install_hle` while depth > 0 a stop (cheapest, keeps
  the hot path); or keep handlers in stable storage (`std::deque` / `unique_ptr<HleEntry>` with the sort over an
  index) and defer `rebuild_hle_slots` to the end of the outermost handler.
- **Verify:** gekko_core_tests case: a handler that installs a second hook, run under ASan or with a canary
  `std::function` capture, must stop (or work) cleanly; gekko_core_tests, native_fallback_tests, ppc_recomp_tests.
- **Risk:** low.

### RC-gen-5: no generator gate for wait points

- **Where:** tools/ppcrecomp/main.cpp:5287, 5818-5823 (`g_wait_points_emitted` only reported); claim of a gate in
  wait_points.py:51-56 and CMakeLists.txt:970.
- **What is wrong:** a listed back edge in a surviving row can lose its wait (e.g. the leave path at main.cpp:1046
  when the head is not recompiled) and nothing fails; the guest then spins without advancing the game clock.
- **Evidence (probe_gen.py, watch tree):** 401 listed; 220 emitted with their wait; 0 emitted without; the 181 not
  emitted all lie outside every emitted range. Nothing is broken today.
- **Impact:** latent hang.
- **Confidence:** proven (the gate is missing).
- **Fix direction:** as the platform-site check does: a site held by a live, non-owner, non-absorbed row must be in
  `g_wait_points_emitted`, else fail the generation.
- **Verify:** ppcrecomp_output_structure; drop one emission in a scratch run and expect the failure.
- **Risk:** low.

### RC-tests-6: f1c_canary_longrun red: stale driver fixture (two breaks)

- **Where:** runtime/tests/difftest/f1c_canary_driver.cpp:238-247 (`expected_hle_slots`: 1,024 slots, mask 1023) vs
  cpu.h:1484 (`kHleSlotCount = 4096` since snapshot f5ec9b3); :391-398 (`install_os_entry` writes only
  `li r3,<index>` at vector+0x68) vs os_exception_entry.cpp's constant-template check (`os_exception_page_word`,
  67eb159, extended in 9545590).
- **What is wrong:** `HLE slot table pattern found 0 times in the Cpu object`; the first `sc` stops: `exception 0x0C00
  taken at pc 0x800300C4, but the guest has not installed the OS's vector for it (__OSExceptionInit)`; every later
  B1 check fails as a consequence.
- **Evidence:** `ctest -R ^f1c_canary_longrun$`: failed in 0.82 s, 12 failures in
  Testing/difftest-f1c-results/canary-longrun.json; phase A passes (234 runs, watermarks stable, 0 guard violations).
- **Impact:** test red; the R12 host-stack guard it carries is not exercised.
- **Confidence:** proven (both causes); B1 after the fix not verified.
- **Fix direction:** size the expected table from the real constant (expose `kHleSlotCount`/mask to the test, or
  search 4,096 u32); write the full vector words (`os_exception_page_word(word, index)` per word, PPCSync head for
  0x0C00).
- **Verify:** canary report `passed: true`; B1 services = 2*outer*INNER_A.
- **Risk:** low (test only).

### RC-kernel-5: the FPR-shadow policy lives only in shadow_main

- **Where:** shadow_main.cpp:2726, 1466-1469, 9166-9168; cpu.h:422-436; native_interrupts.cpp:207-212;
  gameplay_scheduler.cpp:1006, 1066-1068.
- **What is wrong:** the single-thread FP policy (MSR[FP] forced in `os_set_current_context`) needs
  `set_interrupt_fpr_shadow(true)`, set only by shadow_main. Every other System user runs the forced-FP policy without
  the restore, so callbacks can clobber the interrupted thread's FPRs: gekko_core_tests (shadow set only in unit
  cases), lift_sites_reference_tests, compiler_body_tests, ax_pb_tests, camera_tests, native_file_api_tests,
  native_sound_store_tests, numeric_probe_tests. `--no-fpr-shadow` is silently ignored by default. cpu.h:428-434 and
  `--help` still say "OFF by default ... 0 of 1,141,282"; FpProbe's "--fpr-shadow is on" warning fires on every
  default run.
- **Impact:** test validity (tests run a configuration the game never runs); misleading docs.
- **Confidence:** proven.
- **Fix direction:** set the shadow in `System::start` or `NativeInterrupts::install` from
  `single_thread_configuration()`; refuse `--no-fpr-shadow` there; update cpu.h, the help text and the FpProbe text.
- **Verify:** gekko_core test on a System built without shadow_main: a delivery's handler clobbers f1; f1 restored.
- **Risk:** low.

### RC-tests-2: render_determinism red

- **Where:** tools/lift/render_determinism.py:44 (fails on any `debug layer:` line of stage_preview).
- **What is wrong / evidence:** same cause as RC-tests-1. Unset: 6.62 s, two NOT_ZEROED debug-layer lines, `FAIL:
  debug-layer messages`, draws complete (64 meshes, 1392 draws, 0 without texture). With
  `SHADOW_R2_DIRECT_RESOLVE=0`: passed in 1.85 s, `two WARP renders of stg0100 are byte-identical (70406 bytes)`.
  Side note: the script leaves a `render-determinism-*` folder in %TEMP% each run.
- **Confidence:** proven. Fix/verify/risk: RC-tests-1 (plus delete the temp folder).

### RC-memory-1: the flat windows have no guard; a wrapping access reaches host memory  [opt-runtime]

- **Where:** guest_memory.h:174 (`kFmWindowBytes = 0x100000000`); guest_memory.cpp:154-160 (both windows reserved at
  exactly 4 GiB); cpu.h:755-768 (`fm_base_ + effective` with the access width added by the host instruction);
  flat_memory.cpp:304-312 (refuses an access whose end passes the window).
- **What is wrong:** `effective` is a u32 but the host access is `width` bytes at `fm_base_ + effective`. A guest
  access that wraps past 0xFFFFFFFF (`lwz r3,-2(r4)` / `lfd f1,-4(r4)` with r4 = 0, a misaligned 8-byte access at
  0xFFFFFFF9+) touches up to 7 bytes after the window. On the PowerPC the address wraps to 0. Here the bytes after
  the reservation belong to whatever the process mapped next: if committed, the access silently reads or writes host
  memory; if not, the VEH sees an end past the window and refuses ("refusing to guess"), so the process crashes
  instead of stopping with a guest diagnostic. Same for the null window.
- **Evidence:** by reading (no guard reservation; the decoder's bound check at flat_memory.cpp:304-305). Not run.
- **Impact:** silent host memory corruption (layout-dependent) or a crash without a guest stop, on a guest null-pointer
  bug at a negative offset.
- **Confidence:** likely.
- **Fix direction:** reserve `kFmWindowBytes + 64 KiB` (PAGE_NOACCESS tail) for both windows; in `fm_fault`, accept
  an operand that ends inside the guard and serve it as the guest does (stop as unmapped, or split and wrap).
- **Verify:** flat_memory_tests: a 4-byte read at 0xFFFFFFFE and an 8-byte store at 0xFFFFFFFC must reach the slow
  path (unmapped stop), never host memory.
- **Risk:** low (64 KiB of address space per window).

### RC-kernel-3: drive_sync does not preserve GQR1-7 or FPSCR for the waiting thread

- **Where:** native_interrupts.cpp:605-673 (saves/restores only r1, MSR, FPR 0-31 and words 0xC0/0xD4/0xD8); compare
  `deliver()` (:493, :935 save/restore GQR1-7; FPSCR through the shadow).
- **What is wrong:** the original restored the sleeping thread's GQRs and FPSCR on `OSLoadContext` and the lazy FPU
  switch. GQR writers exist in CRI Sofdec code the Idle pass may run inside `run_upcall`: generated
  `write_spr(c, 914..919u, ...)` in fn_803D411C, fn_803E09F8, fn_804036D0, fn_8040A204, fn_8040A7CC, fn_80445ECC,
  fn_80449D54, fn_8044E5A4, fn_8044E738, fn_804A7FDC. FPSCR sticky bits change on every FP compare (ppc_ops.h:410-456).
- **Impact:** wrong psq_l/psq_st scaling in the main thread if a GQR leaks; FPSCR (state.json-visible through
  OSContext) differs.
- **Confidence:** suspected (whether these run inside a native wait is not verified).
- **Fix direction:** save/restore `spr[SPR_GQR0+1..7]` and `fpscr` next to `saved_fpr` in `drive_sync`.
- **Verify:** os_threads_contract_tests `run_waits`: callback writes GQR2 and FPSCR; both unchanged after
  `serve_sleep_thread`.
- **Risk:** low.

### RC-gen-3: jump tables feed liveness only; a third of live switch cases leave the dispatching body

- **Where:** tools/ppcrecomp/main.cpp:3800-3815 (a row starts after every bctr), :1072-1130 (only in-extent labels
  become gotos).
- **What is wrong:** when a function's size ends at its `bctr`, the case arms become separate rows and each case goes
  through `recomp::jump_indirect` (lookup, `transfer_to`, `finish_transfers`) instead of a goto.
- **Evidence (probe_gen.py, watch):** 2,479 live case words: 1,608 labels of the dispatching row, 871 starts of other
  rows, 0 without native code; 69 dispatch sites, largest 0x803A71B0 (73 words), 0x803ABDAC (33), 0x800519E8 (29).
  Example: fn_800106B4, functions.csv size 284 ending exactly at bctr+4, next row fn_800107D0.
- **Impact:** perf (each case pays an indirect lookup + published transfer); not measured in wall time.
- **Confidence:** proven.
- **Fix direction:** widen the dispatcher row over its listed table targets (D1-style re-analysis with the targets as
  labels), or seed labels from jump-tables.txt.
- **Verify:** probe_gen.py: 0 case words outside the dispatching row; behaviour gate + benchmark.
- **Risk:** larger rows; containment collapse absorbs the arm rows. Emission sits next to `jump_indirect`
  (coordinate with opt-runtime).

### RC-tests-5: difftest_f1b red: stale NI operand rule in the spec

- **Where:** test_data/core-audit/spec/fp_A.py:633-653 (`_ni_operand`, :643 `zero = True if single_instr`);
  tools/difftest/known_candidates_f1b.json.
- **What is wrong:** with FPSCR[NI] = 1 the spec zeroes a single denormal input operand of every single instruction.
  The engines switched to hardware table estimates in f5ec9b3 (runtime/include/gekko/gekko_estimate.h), following
  hwtests, where NI flushes outputs only, for fres/frsqrte/ps_res/ps_rsqrte.
- **Evidence:** `ctest -R ^difftest_f1b$`: failed in 103.2 s; `0 engine-divergence ... 0 host-fact, 0 harness errors`;
  229 known groups KNOWN; 3 NEW: `2 x ["common-mode","ps_res",[],["fpr[].ps0{value}"]] e.g. ps-00186`, `1 x [...
  "ps_res" ... "fpr[].ps1{value}"] e.g. ps-00190`, `1 x [... "ps_rsqrte" ... "fpr[].ps0{nan-vs-number}"] e.g.
  ps-00230`; all NI = 1 with single-denormal operands (8.42e-39, -3.74e-39, -2.04e-39). Memory note
  project-hw-estimates-2026-11-07 records the same 4 findings.
- **Impact:** test red only; both engines agree (common-mode), RL oracle 0 mismatches.
- **Confidence:** proven.
- **Fix direction:** limit `_ni_operand`'s input flush to non-estimate instructions (cite hwtests), or ledger the 3
  groups with that reason. Changing `_ni_operand` for all FP ops re-fingerprints many known groups.
- **Verify:** `ctest -R ^difftest_f1b$`: 0 NEW, 0 stale_known.
- **Risk:** low if scoped to the estimate instructions. **User decision:** spec change vs ledger.

### RC-tests-4: dolrecon_lift_tests cannot finish within its timeout

- **Where:** CMakeLists.txt:2377-2378 (all 34,045 lifted functions, up to 256 random cases each, one process,
  `TIMEOUT 1800`, 16 threads, output printed at the end); flat_memory.cpp:306-311 (stderr flood).
- **What is wrong:** the work does not fit the timeout (under the 6 % cap also a cap artifact). The harness catches
  faults (dolrecon_lift_tests.cpp:342) and counts the case as invalid input, but the VEH first prints `[fm] access
  violation ... refusing to guess` for each one (~90 MB of stderr per full run).
- **Evidence (`dolrecon_lift_tests.exe main.dol --range a b`, capped, one thread):** 0-300: 146.4 s wall, 139.3 CPU-s,
  227 verified / 73 unverified, ~4,900 `[fm]` lines; 17000-17200: 98.1 s, 143/57; 33800-34045: 37.9 s, 93/152. No
  function FAILED in the 745 sampled. Estimate ~13,600 CPU-s (5,000-15,800) total: ~7,100 s under the cap; ideal
  uncapped at 16 threads ~850 s; recorded real runs 2,712 s (docs/GC_REMOVAL_PLAN.md:166-170) and >4,157 s
  (docs/NEXT_SESSION.md:218, 323).
- **Impact:** the test never completes in a normal run.
- **Confidence:** likely (extrapolated; the full run was not done: NEXT_SESSION.md:218 says "only when the user
  allows").
- **Fix direction:** shard into 8-16 CTest entries by `--range` with per-shard timeouts (or drive it through
  tools/dolrecon/lift_verify.py under a long label kept out of default runs); pass the `--skip` list; silence the
  `[fm]` line when the caller handles the fault (e.g. a thread-local "expect faults" flag). Do not lower case counts.
- **Verify:** every shard finishes capped; merged summary 0 FAILED.
- **Risk:** low. **User decision:** permission for the full run.

### RC-gen-6: backward computed transfers have no recognition point  [opt-runtime]

- **Where:** recomp.h:98-126 (`jump_indirect` / `resume_elsewhere` publish via `transfer_to`); cpu.cpp:2937-2951
  (`finish_transfers` tests nothing between entries); main.cpp:1118-1120 (the poll covers local labels only).
- **What is wrong:** a guest cycle closed only by a bctr/blr to a lower address in another row never polls
  `needs_dispatch()`; a stop, interrupt or budget then waits for the loop to end on its own.
- **Evidence:** switch tables: 0 backward cross-row targets in watch. Function-pointer and longjmp cycles not checked.
- **Impact:** hang (stop or interrupt starved).
- **Confidence:** suspected.
- **Fix direction:** in `emit_indirect_jump`'s default arm: `if (tmp <= site && c.needs_dispatch()) { leave(Yield) }`
  before `jump_indirect`.
- **Verify:** native_fallback fixture with such a cycle and a pending interrupt.
- **Risk:** low (one compare per computed jump).

### RC-memory-2: the flat-map VEH services faults from any thread  [opt-runtime]

- **Where:** flat_memory.cpp:278-382 (`Cpu::fm_fault`), :243-256 (process-wide `AddVectoredExceptionHandler`).
- **What is wrong:** the handler resolves any access violation inside a registered window, on whatever thread faulted,
  and calls `owner->mem_*_slow`, which can latch the data fault, write DAR/DSISR, `request_stop`, rebuild the HLE
  slots and set `cold_store_return_`: all game-thread state, with no synchronisation. A host thread that dereferences
  a window pointer (a job, the audio or render thread holding `fm_window()` + offset, or `GuestMemory` raw pointers
  into an uncommitted page) would mutate the Cpu concurrently. The kernel audit found no host thread doing so today
  (expand-jobs worker uses a host snapshot; audio copies synchronously on the game thread).
- **Impact:** data race / corrupted stop state (latent).
- **Confidence:** suspected.
- **Fix direction:** record the guest thread id at `Cpu::run` entry; in `fm_fault`, if the faulting thread is not the
  owner's guest thread, print a diagnostic naming the address and return `EXCEPTION_CONTINUE_SEARCH`.
- **Verify:** flat_memory_tests: a second thread reading an uncommitted window page must not touch Cpu state.
- **Risk:** low (one compare on a cold path).

### RC-fp-2: host MXCSR is never set or checked on the game thread

- **Where:** ppc_ops.h:588-590 (`to_single` = host `cvtsd2ss`), :477-478 (`std::nearbyint`), :569-574 (FMA),
  :823 (`std::sqrt`); grep: no `_mm_setcsr` / `_controlfp` in runtime/src (only in three tests:
  native_hermite_tests.cpp:61-65/164, native_matrix_compose_tests.cpp:65-68/245, native_scalar_math_tests.cpp:169).
- **What is wrong:** every fast path depends on MXCSR = round-to-nearest with FTZ and DAZ off. MXCSR is per thread and
  inherited by whatever runs on the game thread (driver callbacks, a DLL that sets FTZ/DAZ, a native owner that
  forgets to restore it). Nothing asserts it at `Cpu::run` entry or after host services, so a change would silently
  change guest arithmetic (denormals, ties, fctiw rounding) with no failing test.
- **Impact:** determinism (latent); the behaviour gate would catch a desync but not name the cause.
- **Confidence:** suspected (no instance found).
- **Fix direction:** set MXCSR to 0x1F80 (masked exceptions, RN, no FTZ/DAZ) at `Cpu::run` entry; in non-shipping
  builds check it after each host-service callback and stop naming the service that changed it.
- **Verify:** gekko_core test: set FTZ in a handler, expect the stop (non-shipping); behaviour gate unchanged.
- **Risk:** none for the game (it already assumes this state).

### RC-kernel-7: no progress detector for native waits that never end; dead kNoDeadline branch

- **Where:** system.cpp:5368-5373, 5398-5401; system.h:320-324.
- **What is wrong:** the header says "The idle wait passes kNoDeadline", but `idle_wait` passes `when`
  (`advance_to_next_event`); `when > target` with `target == kNoDeadline` is never true, so the "nothing is scheduled"
  stop is dead; with `kNoDeadline` and an empty schedule `move_clock_to(~0ull)` would loop forever. `GameEvent::Frame`
  is always scheduled, so a native wait whose waker never comes (OSSleepThread on a queue nobody wakes) runs forever in
  game time with no diagnostic; the wall watchdog is reset on every wait (5354-5358).
- **Impact:** diagnosability (silent infinite waits); latent hang.
- **Confidence:** proven (by reading).
- **Fix direction:** remove/fix the branch and the comment; add a game-time bound to native waits (stop after N game
  seconds in `serve_sleep_thread`, naming the queue; N generous for loading).
- **Verify:** gekko_core test with only Frame scheduled and a never-woken sleep: the stop fires.
- **Risk:** false positive on a legitimately long wait if N is too small.

### RC-dispatch-2: dispatcher and shared-slot HLE lookups cost 0.5-0.6 % of the game thread  [opt-host2]

- **Where:** cpu.cpp:3029 (`find_hle(pc)` on every dispatcher iteration), :3226; :634-662 (`find_hle`: shared-slot
  scan, two dependent loads into `HleEntry`); :676-760 (`try_hle_slow`); cpu.h:1484 (4096 direct-mapped slots, "~7"
  shared).
- **What is wrong:** profile cost with no functional need: a hooked address is known at generation time for owners,
  and a collision-free slot function would remove the shared-slot path entirely.
- **Evidence:** docs/agents/opt-runtime.md table: `try_hle_slow` 0.5 %, `find_hle` 0.6 % ("the hot find_hle samples
  are the dispatcher's own calls, and the shared-slot loop (cpu.cpp:643-644)"); opt-host2 item 1 (packed `HleHot`)
  measured no change (32.63 vs 32.71 s).
- **Impact:** perf, <= ~1 % of the game thread.
- **Confidence:** proven (cost); the gain of a fix is not measured.
- **Fix direction:** at `rebuild_hle_slots`, pick a multiplicative hash (seed search) that leaves no shared slot for
  the installed set (fallback: current scheme); skip the dispatcher's `find_hle` when the pc came from a native
  return (most iterations) or cache "pc unhooked" per dispatch target.
- **Verify:** gekko_core_tests, native_fallback_tests, ppc_recomp_tests; benchmark (3 alternating rounds).
- **Risk:** low.

### RC-tests-7: lift_purity red

- **Where:** tools/lift/check_purity.py; runtime/lift: geometry_import.h:133 (`GameCube`), math/msl.cpp:712
  (`Gekko`) since 873d9fc; effects/watch_camera_sheet.h:24, 26, 29; render/frame.h:81, 82, 94;
  render/frame_math.h:72, 317; render/shaders/mesh.hlsl:13, 196 (addresses, `GameCube`, `TEV`) since f5ec9b3.
- **Evidence:** `ctest -R ^lift_purity$`: failed in 0.35 s, `lift purity: 18 violation(s) in 146 lifted source
  file(s)`; the checker on `git archive` of 873d9fc and 35dfa30: 2 violations; f5ec9b3: 18. Never passed in this
  history. All in comments (frame_math.h:317 an address in an inline comment).
- **Confidence:** proven. **Fix:** move the console explanations into docs (docs/effects/..., R_DESIGN), neutral
  wording in code. **Verify:** `no GameCube concept`. **Risk:** none.

### RC-tests-8: lift_scaffold_inventory red

- **Where:** tools/lift/check_scaffold.py:25 (marker pattern requires `: <why>`); runtime/scaffold/rw_driver
  `im2d_prims.cpp/.h/_install.cpp` (deb3a26, 2026-11-08) and `raster_render.cpp/.h/_install.cpp` (ed4e9fe).
- **Evidence:** `ctest -R ^lift_scaffold_inventory$`: failed in 0.10 s, 6 problems `no "SCAFFOLDING (step): what -
  deleted by step: why" marker`; checker on deb3a26^ passes (16 files, 8,382 lines), deb3a26 3 problems, ed4e9fe 6.
- **Confidence:** proven. **Fix:** add `: <which lift removes its callers>` to line 1 (line 2 in raster_render.h).
  **Verify:** the R4/R5/R6 debt table prints. **Risk:** none.

### RC-fp-1: fctiw / fctiwz high word for a negative operand that converts to zero

- **Where:** ppc_ops.h:465-481 (`ppc::fctiw`, returns `0xfff8000000000000ull | static_cast<u32>(converted)`);
  cpu.cpp:4705 (interpreter, same expression).
- **What is wrong:** the hardware sets bit 31 of the doubleword (high word 0xFFF80001) when the operand is negative
  and the converted word is 0 (-0.0, and e.g. -0.3 under fctiwz). Both engines give 0xFFF80000.
- **Evidence:** research/sources/repos/hwtests/cputest/fctiwz.cpp: `{0x8000000000000000, 0xfff8000100000000},  // -0
  (!)`; hwtests/cputest/fctiw.cpp `fctiw_expected` ("matches Broadway's behavior of setting bit 31 to 1 for negative
  zeroes", `frac = ~(u32)frac + 1` for any negative result); Dolphin Interpreter_FloatingPoint.cpp
  `ConvertToInteger`: `if (value == 0 && std::signbit(b)) result |= 0x100000000ull;`. Not run on hardware here (the
  hwtests source is the evidence).
- **Impact:** fidelity, low: visible only if the guest stores the whole double (stfd) and reads the high word (MWCC
  reads +4), or compares stored doubles.
- **Confidence:** proven (deviation from the hardware test table).
- **Fix direction:** in both engines, `if (converted == 0 && sign(operand)) result |= 0x100000000ull` (NaN excluded).
  This changes the reference oracle too: decide as a Tier E fidelity fix.
- **Verify:** ppc_recomp_tests and difftest_f1a/f1b vectors for -0.0, -0.3, -0.7 (fctiw and fctiwz); state.json
  identity on the benchmark tape (expected identical unless the high word is stored).
- **Risk:** low. **User decision:** adopt hwtests behaviour in the oracle.

### RC-kernel-6: HostWork contract, its test and the run_upcall comment disagree with the code

- **Where:** native_interrupts.h:188-190, 196; native_interrupts.cpp:565-603; os_threads_contract_tests.cpp:344-350;
  native_cri.cpp:293-295.
- **What is wrong:** the header says HostWork "cannot drive ... is an ExecutionFault"; the code drives HostWork through
  its handler (System installs NativeCri's `host_work`; `run_idle_in_wait` raises it on purpose). The test "a latched
  HostWork must stop the wait, naming it" passes only because no handler is installed (it tests the generic "no
  handler" stop). `run_upcall` "returns true when the machine stopped" but always returns true.
- **Confidence:** proven. **Fix:** fix both comments; test: HostWork with a handler is driven, the upcall request
  inside a wait still stops. **Verify:** os_threads_contract_tests. **Risk:** none.

### RC-gen-4: shard layout depends on the build dir's history

- **Where:** tools/ppcrecomp/main.cpp:3167-3238 (`plan_shards` reads the existing `out/recomp_layout.csv`); :5694
  (kRanges sorted by begin only).
- **Evidence:** vs a fresh `index % shards` layout: 5,011 of 37,743 rows differ (watch), 5,290 of 38,074 (tests).
- **Impact:** identical inputs give different translation units and link order per tree, confounding cross-worktree
  performance A/B; tie order of `std::sort` is implementation-defined.
- **Confidence:** proven (by design). **Fix:** `--fresh-layout` or a checked-in layout for benchmark trees; print a
  layout hash in the report; `stable_sort` with a symbol tie-break. **Risk:** more recompilation after layout changes.

### RC-gen-10: shared FPSCR / trap deviations (generator and interpreter agree)

- **Where:** mcrfs: main.cpp:2694-2698, cpu.cpp:4721 (does not clear the copied exception bits); mtfsf/mtfsfi/mtfsb1:
  main.cpp:2682-2735, cpu.cpp:4709-4760 (FX/FEX/VX written directly, not recomputed via `fpscr_raise` rules); tw/twi:
  main.cpp:1651, 2202, cpu.cpp:3510, 3988 (Program exception with SRR1 bits 0; the architecture sets SRR1[14] =
  0x00020000 for a trap).
- **Evidence (census.py):** DOL: mcrfs 0, mtfsfi 0, mtfsb 1 (mtfsb1 NI at 0x80370BE8), mtfsf 4, twi 4
  (0x8039F550..68); emitted: 0 in watch, 4 mtfsf in tests.
- **Impact:** correctness, low (no shipping site; OSContext/diagnostics only).
- **Confidence:** proven. **Fix:** both engines together. **Verify:** native_fallback_tests, ppc_recomp_tests (add
  cases). **Risk:** low.

### RC-fp-3: FPSCR[RN] ignored; arithmetic FPSCR status unmodelled; no guard

- **Where:** ppc_ops.h:524-526 ("Unchanged and still unmodelled: the FPSCR status and exception bits ... FPSCR[RN]"),
  :477-478 (`std::nearbyint` = host mode); interpreter equivalent cpu.cpp:4690-4704.
- **What is wrong:** a deliberate policy (FPCC/VXVC from compares are modelled, FPRF/FI/FR/XX/OX/UX/ZX are not), but
  nothing enforces its premise that the guest never selects a non-nearest rounding mode or enables FP exceptions: a
  `mtfsf`/`mtfsfi`/`mtfsb1` that sets RN or VE/OE/UE/ZE/XE is accepted and silently ignored. FPSCR content saved into
  OSContext is therefore a partial mix (state.json-visible), which is why native owners must reproduce FPCC of their
  last compare (the "FPSCR policy" in the opt reports; MetroTRK accessors excluded, docs/agents/opt-host2.md item 3).
- **Impact:** latent correctness if a path ever sets RN or an enable.
- **Confidence:** proven (policy by reading; no write of RN found: census shows 4 mtfsf in the tests tree only).
- **Fix direction:** in both engines, stop (fail closed) on an FPSCR write that sets RN != 0 or any enable bit, as
  `write_hid2` refuses clearing PSE/LSQE.
- **Verify:** gekko_core test with a refused mtfsfi; benchmark tape unchanged.
- **Risk:** low.

### RC-gen-9: bclrl lowered as a return instead of a call  [opt-runtime]

- **Where:** tools/ppcrecomp/main.cpp:2048-2062 (`c.lr = next; c.pc = tmp; return;`).
- **Evidence (census.py):** 153 blrl in the DOL; 0 emitted in watch; 10 in tests (0x80371DD0, 0x803788E4,
  0x803796F8, ...). Each unwinds the host frames; callee and later returns go through `resume_elsewhere`.
- **Impact:** perf (reference tree only). **Confidence:** proven. **Fix:** lower unconditional bclrl like bcctrl
  (`call_indirect(c, tmp, next)`, exit check, pc check). **Risk:** low.
- **Status (2026-11-13): closed, by design.** s3/call2 tried the fix (e2f3937) and reverted it (f36baeb): lowering
  bclrl as a call grows the host stack from 144 to 272 B per lap on the bclrl shapes and exhausts the 16 MiB reserve
  after about 116k laps (f1c_host_stack_integrity and f1c_host_stack_integrity_notail fail). The unwinding return is
  what keeps the host stack bounded. Report: docs/agents/s3-call.md.

### RC-gen-7: imprecise FallbackReason and lost sites  [opt-runtime]

- **Where:** recomp.h:83, :107 (`leave_to_dispatcher(target)` = NotRecompiled with site 0, cpu.h:936-938);
  recomp.h:171, :183 (`fp_unavailable`, `illegal_instruction` report `Trap`); recomp.h:128-130 (`exit_at` unused; its
  comment "the interpreter raises the exception itself" is false: production has no interpreter, and traps are
  emitted inline at main.cpp:1645-1655, 2198-2205).
- **Impact:** the fallback work list loses the site and mislabels faults. **Confidence:** proven.
- **Fix:** pass the site (link-4, bctr address) into `call_indirect`/`jump_indirect`; add FpUnavailable / Program
  reasons; delete `exit_at`. **Risk:** low.

### RC-gen-8: cold `_at` with an unknown entry starts at the function's first instruction

- **Where:** tools/ppcrecomp/main.cpp:3524-3538 (`default: break;` then `body(c, dispatch_base)`; misaligned entries
  too); warned against at :5669-5671.
- **Impact:** correctness, defensive (unreachable today by construction). **Confidence:** proven by reading.
- **Fix:** default `leave_to_dispatcher(entry, NotRecompiled, entry)` or `request_stop`. **Risk:** none (cold path).

### RC-kernel-4: drive_sync zeroes FPRs; its comment contradicts the DOL

- **Where:** native_interrupts.cpp:613-618.
- **What is wrong:** the comment says the lazy switch "loaded its cleared FPRs". DOL: `__OSLoadFPUContext` 0x8037292C:
  `A0A401A2 lhz r5,0x1A2(r4)`, `54A507FF clrlwi. r5,r5,31`, `41820118 beq return`; `OSSwitchFPUContext` 0x80373100
  calls it at 0x8037313C. A context cleared by `OSClearContext` (FPSAVED clear) loads nothing: callbacks see the
  previous owner's live FPRs, as `deliver()` leaves them; `drive_sync` zeroes them instead.
- **Impact:** fidelity of dead-stack / heap-leftover words; misleading comment.
- **Confidence:** proven (comment wrong); visible effect suspected.
- **Fix:** drop the zeroing (keep save/restore) or document it as a deliberate divergence, decided by a whole-memory
  A/B. **Risk:** low. **User decision:** divergence or not.

### RC-kernel-8: thread-context upcall bridge unreachable in the lifted build

- **Where:** native_interrupts.cpp:755-759, 826-903; native_cri.cpp:222-224, 235.
- **What is wrong:** `request_upcall` from a delivery needs `idle_may_run` (`*0x800000E4 == 0`); in the single-thread
  configuration the main thread is always current, so `begin_upcall`, `end_upcall`, non-sync `upcall_call` and
  `upcall_return` are dead in shipping. In reference mode B with `--fpr-shadow`, `end_upcall` calls
  `enter_native_interrupt()` with resume = its own pc, so the final rfi's SRR0 never matches and
  `restore_interrupt_fpr` skips (cpu.cpp:1506): the interrupted thread resumes with the upcall's FPRs.
- **Confidence:** likely. **Fix:** compile only in the reference build (as native_guest_scheduler.cpp) or delete with
  the CRI bridge; if kept, set `interrupt_fpr_resume_` to the interrupted SRR0 in `end_upcall`. **Risk:** low.

### RC-gen-12: malformed list lines accepted silently

- **Where:** tools/ppcrecomp/main.cpp: native-owners :4149, :4592; prune-roots :4666-4673 (a typo in `census` becomes
  a plain root); jump-tables :4724-4726; wait-points :4238-4239 (duplicates overwrite at :4251); `--only` :3715;
  options as last argument become "" and disable the feature (:3066-3068); `--shards 0` becomes 1. The D1-widened
  analysis is not re-validated by pass 1 (fails late, closed).
- **Confidence:** proven by reading. **Fix:** `parse_hex32` and exact field counts, as the platform/gx/lift parsers
  do. **Risk:** low.

### RC-gen-11: `--hot-set` emits accessors that no longer exist  [opt-runtime]

- **Where:** tools/ppcrecomp/main.cpp:3245-3263, 3414-3419 (`mem_*_hot` are gone, CMakeLists.txt:1058);
  test_output_structure.py:720-770 checks text only. Also dead: `mem_r` helper (:802-810), an empty `if` in mtspr
  (:2412-2413).
- **Confidence:** proven. **Fix:** delete or retarget (conflicts with the pending volatile-store change).

### RC-kernel-9: external line and native_interrupt_first are production-dead

- **Where:** cpu.h:528-541, 563-570; cpu.cpp:1672-1693; native_os_threads.cpp:84-86.
- **Evidence:** `set_native_interrupt_first` and `set_external_interrupt(` are called only in tests (difftest drivers,
  gekko_core, native_execution, native_fallback, os_interrupts, os_threads tests); cpu.h says "NativeInterrupts
  installs the comparison".
- **Confidence:** proven. **Fix:** correct the comment; gate the dead paths or move them to a test shim.

### RC-kernel-10: timeline.cpp is dead code with a latent race

- **Where:** runtime/src/gekko/timeline.cpp; CMakeLists.txt:564. No includer of `gekko/timeline.h`, no caller of
  `timeline::enable/emit/finish`. If revived, `finish()`/`stats()` read `ThreadBuffer::records` / `used_in_last`
  under the registry mutex while `emit_slow` writes without it.
- **Confidence:** proven. **Fix:** delete (or publish under the lock/atomics). **Risk:** none.

### RC-kernel-11: SHADOW_NATIVE_JOBS=1 cannot run in the single-thread configuration

- **Where:** native_jobs.cpp:107-108; native_expand_jobs.cpp:221-224; native_os_threads.cpp:159-163, 205-211. With
  the variable NativeExpandJobs refuses to install (lifted build exits 2); `single_thread_configuration()` ignores
  it, so the reference build stops at the first loader thread. The bridge is effectively dead.
- **Confidence:** likely (not run). **Fix:** delete the bridge or honour the variable in
  `single_thread_configuration()`.

### RC-kernel-12: UB in native OSReport width/precision parsing

- **Where:** native_os_report.cpp:166-182: `spec.width * 10 + digit` (and precision) overflow `int` on long digit
  runs; `-w` with `w == INT_MIN` from `*`; huge widths allocate ~2 GiB in `pad()` (bad_alloc inside an owner).
  Format strings are guest data.
- **Confidence:** proven (unreachable with the game's constant formats). **Fix:** saturate at e.g. 4096 and refuse
  beyond with `why`. **Verify:** os_report_contract_tests with a long width.

### RC-gen-13: liveness address formation misses some forms

- **Where:** tools/ppcrecomp/main.cpp:4784-4895: H5/H6 do not follow register copies (`lis r3; mr r4,r3; addi
  r5,r4,lo`) or `li` + `addis rX,rX,hi`; H6 drops chained lows; H6 `block_starts` omits code addresses held in data
  words. (`gpr_writes`, :1237-1297, checked correct.)
- **Confidence:** suspected (no instance found). **Fix:** add data-word code values to `block_starts`; treat
  `or rA,rS,rS` as a copy.

### RC-gen-14: generator-level test coverage gaps

runtime/tests/ppc_recomp_tests.cpp compares the interpreter with hand-written lambdas, not with generator output;
only the native_fallback fixtures and compiler_body_tests compile and run generated code differentially. Not covered:
CR fusion and FP/HID2 hoisting across labels and entries; the cold `_at` trampoline and interior entries; the D1
merge; containment collapse + forwarding; the kRanges clip; lmw/stmw (rA in range); lswi/lswx/stswi/stswx; update
forms; lha/lhau; dcbz/dcbz_l alignment; stfiwx; eciwx/ecowx; mtfsf/mtfsfi/mtfsb/mcrfs; mtmsr's two-sided R4 check;
bclr/bclrl/bcctr BO variants; fsel; ps_merge00/11; ps_muls/madds; psq update forms; owner overlap (RC-gen-2);
wait-point completeness (RC-gen-5). Fix: a generated-fixture suite (make_fallback_fixture.py style) per family.

### RC-kernel-13: kernel test gaps

None of these exist: (1) a stop inside `serve_sleep_thread` / vi_wait, then resume (RC-kernel-1); (2) a wait or poll
point inside a `drive_sync` callback (RC-kernel-2); (3) GQR/FPSCR preservation across a native wait (RC-kernel-3);
(4) an end-to-end delivery (deliver, call_guest, callback_return, finish, resume) in the single-thread configuration
asserting the shadow restores FPRs and Reschedule is balanced (native_video_reference_tests installs NativeInterrupts
but does not check these); (5) the wait-point watchdog (`set_wait_watchdog`); (6) `advance_game_time` with only Frame
scheduled (RC-kernel-7); (7) the `os_set_current_context` single-thread branch (MSR[FP] / SRR1[FP] forced); (8) an
`install_hle` from inside a handler (RC-dispatch-1).

---

## Checked and sound (do not re-audit)

Tests run green (capped, tests tree): gekko_core_tests 5.38 s, ppc_recomp_tests 1.11 s, native_fallback_tests 0.83 s,
flat_memory_tests 0.71 s (`ctest -C Release -R ^<name>$`, each "100% tests passed"); os_threads_contract_tests,
os_interrupts_contract_tests, guest_call_tests, native_threads_tests, host_timing_tests, os_heap_contract_tests
("100% tests passed, 0 tests failed out of 6"); python tools/ppcrecomp test_wait_points.py (9 OK), test_gx_sites.py
(25 OK).

CPU core and instruction semantics (ppc_ops.h; interpreter agrees, ppc_recomp_tests):
- Carry/overflow family, ppc_ops.h:139-260 (addc/adde/addze/addme/subf*/neg/subfic/addic): one 33-bit sum each;
  OV masks correct for y = 0 and y = -1.
- mulhw/mulhwu/mullw/mulli (s64 products, no signed overflow UB); divw/divwu edge cases (/0, 0x80000000 / -1 give
  0xFFFFFFFF or 0 by dividend sign; divwu /0 gives 0) match Dolphin's hardware-derived rule.
- rotl32, mask_from (wrap when me < mb), sign_extend (C++20 arithmetic shift), slw/srw/sraw (rB & 63, >= 32),
  srawi carry, cntlzw (LZCNT only under AVX2, BSR + zero test otherwise), trap_taken.
- CR/XER helpers, cmp/cmpl SO copy, cr_bit/set_cr_bit, field_mask8; fp_compare: FPCC at <<12, CR field, VXSNAN/VXVC
  rules, FPSCR[C] untouched; fpscr_raise FX/VX/FEX summaries.
- FP fast paths ppc_ops.h:785-940: tiny-key bounds (2^-125 / 2^-1021 / sum bounds 2^-70 / 2^-966), NaN routing,
  single-midpoint test of fused singles (`(r_bits & 0x1fffffff) == 0x10000000`), negation after rounding for
  fnmadd/fnmsub, default QNaN 0x7FF8...; fp_arith_slow NI operand/result flush and exact residual sign.
- Force25Bit (frC rounding of single multiplies): deliberately not modelled; docs/SHADOW_PLAYER_COVERAGE_AUDIT.md
  section 12: 7,609 of 8,001 single-multiply sites statically proven single, 0 wide executions in 45 M strict
  segments. Not an issue.
- psq_l/psq_st: GQR field extraction (store type bits 0-2 / scale 8-13, load 16-18 / 24-29), element strides
  (+1 for u8/s8, +2 for u16/s16), NaN-safe saturation before integer casts, type-0 bit-exact widening and
  denormal-flush store (types.h:221-287); the `psq_*_k` specialisations repeat the generic arithmetic.
- read_spr/write_spr: XER/LR/CTR, TBL/TBU read (268/269) and write (284/285) as views of the game clock, DEC write
  re-arms and enables delivery, HID2 refuses clearing LSQE/PSE.
- Exception entry: SRR1 = MSR & 0x87C0FFFF, MSR clear set, ILE->LE; rfi mask 0x87C0FF73 and POW clear (cpu.cpp:1437-
  1443, 1553-1584, recomp.h:187-192) match the architecture / Dolphin; interrupt-FPR depth bookkeeping and the
  context-switch skip (cpu.cpp:1451-1527).
- Time base / decrementer (cpu.cpp:1618-1648, cpu.h:371-374): edge at now + value + 1, re-arm at deadline + 2^32, u64
  arithmetic, modular u32 views. Interrupt priority and level/edge semantics in take_pending_interrupt.
- request_stop / request_stop_from_host / serve_host_stop (cpu.cpp:843-879, 2992): CAS + acquire/release, served on
  the guest thread; `stop_requested_` is the only cross-thread flag.
- Dispatcher (cpu.cpp:2956-3266): R1 recognition before native entry, `native_entry_exit_state` re-arm, R12
  `finish_transfers` bounds host frames, the no-progress stop in production, hooked-range entry runs the hook once.
- RecompiledCode::install/lookup (cpu.cpp:2857-2925, cpu.h:289-305): 32-bit anchor distances checked to round-trip;
  computed targets are always `& ~3` before lookup.
- rebuild_hle_slots / find_hle exactness (cpu.cpp:565-662): a slot rejects only addresses it proves unhooked; shared
  lists exact; verify/fault fill routes everything to the exact path.

Memory (flat map):
- fm_decode (flat_memory.cpp:115-207): 0x66/REX order, MOVBE (F2-prefixed CRC32 not accepted), MOVZX/MOVSX widths,
  SIB with REX.X index 12, base 101 + mod 00 = disp32, RIP-relative refused, MOV imm sizes; register write-back
  widths incl. AH-BH without REX. Split-page faults are decided by `*_slow` (x86 commits nothing of a faulting
  store).
- Fault-path cost: the coordinator's benchmark logs (wt-gc-removal/.build/perf) report `[fm] flat-map handler
  entries: 3672 (null window 3672, device window 0, page split 0, unmapped 0)` per 316 game-second run (596 runs):
  the poll-watch null window only; negligible.
- No MXCSR or rounding-mode writes anywhere in runtime/src (grep); bits_to_double is a bit cast (types.h:187).

Generator (helper agent; main.cpp lines):
- rlwimi/rlwinm/rlwnm masks (408-413, 1778-1798); CR-logical (2133-2151), mtcrf (2282-2291), mcrxr (2423-2427), mcrf
  (2044-2047); branch conditions (850-884, 1708-1747, 2048-2125): CTR decremented for bc/bclr not bcctr, LR written
  taken or not, bctr target CTR & ~3; the DOL has no `bcl $+4` and no CTR-decrementing bclr.
- CR fusion: next instruction only, hot body only, never at a label, reset on gaps (779-786, 865-873, 1576-1581,
  3374-3379); every way into the hot body lands on a label (3472-3483, 3527-3531, 1101-1160).
- FP/HID2 hoist reset at labels, calls, bcctrl, lift sites, mtmsr, mtspr 920, gaps (1606-1642, 936-937, 2120-2123,
  2309-2310, 2415-2417); no temporary is live across instructions.
- Loads/stores vs interpreter (cpu.cpp:3745-3770, 4569-4590, 5068-5074): update forms latch EA, write rA last, rA = 0
  handling; lmw/stmw latch EA; lha sign extension; byte-reversed and stfiwx; dcbz/dcbz_l EA & ~31; string ops (NB 0
  = 32, register wrap, 2525-2584); psq fields (1851-1853, 2754-2755), update skipped when rA = 0;
  ps_sum/ps_sel/ps_merge/set_pair aliasing-safe (2775-2903); fsel.
- All 51 EndOfFunction tails in watch are dead code; kRanges clip keeps ranges inside reachable sets (5672-5707);
  containment collapse requires reachable-set inclusion and a non-owner absorber (4161-4180); pruning never drops a
  row whose instructions a live row shares (5051); jump-table verification (4917-4947) and data-word roots
  (5021-5033); cross-row owner-interior check (5186-5206); emission iterates ordered containers only.

Kernel (helper agent):
- interrupt_gate (native_interrupts.cpp:107-128), restore_interrupted_context SRR0 clamp (924-948), OSContext layout
  (484-494: GPR 0, CR 0x80, LR 0x84, CTR 0x88, XER 0x8C, SRR0 0x198, SRR1 0x19C, state 0x1A2, GQR 0x1A4+4g),
  Reschedule balance (505/752), best_pending ranking (238-262), mask groups (282-313, 404-452), yield_is_identity
  (771-787).
- native_os_threads.cpp:15-26, 122-134 (scheduler counts, OSIsThreadTerminated, base priority 0x2D4); native_os_heap.cpp
  (SDK OSAlloc incl. MINOBJSIZE 64 unsigned compare); guest_call register preservation (native_guest_call.cpp:12-94);
  savestate load order (savestate.cpp:598, 623); game_clock equal-deadline order (game_clock.h:57-68); clock never
  moves backwards, past events delivered at now (system.cpp:5239-5245); pacing-tick skip (5393-5420);
  PollRegisters has no padding (cpu.h:1550-1554), so poll_point's memcmp is well-defined.
- Host threads: every `NativeInterrupts::raise` site runs on the game thread; audio pass copies guest bytes on the
  game thread (audio_runtime.cpp:2026-2047); expand-jobs worker uses a host snapshot (native_expand_jobs.cpp:642-687);
  input thread uses `pad_mutex`; UI thread uses only `request_stop_from_host`.
- os_exception_entry.cpp:74-154 stores and SRR handling; vi_wait normal path (rw_raster_contract_tests 2093-2191).

## Decisions for the user

1. RC-tests-4: whether the full dolrecon_lift_tests run is allowed, and sharding it into CTest entries.
2. RC-tests-5: ps_res/ps_rsqrte NI inputs: change the spec (hwtests rule) or ledger the 3 groups.
3. RC-fp-1: adopt the hwtests fctiw/fctiwz -0 high word in both engines (changes the oracle).
4. RC-kernel-4: keep drive_sync's FPR zeroing as a documented divergence, or remove it.
5. Deleting dead bridges: RC-kernel-8 (upcall bridge), RC-kernel-11 (NativeJobs), RC-kernel-10 (timeline.cpp),
   RC-gen-11 (`--hot-set`).
