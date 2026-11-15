# recomp-fix (2026-11-12): fixes for docs/RECOMPILER_ISSUES.md

Branch `agent/recomp-fix`, cut at db875b3 (the integration tip that carries the map). Scope asked for: the kernel (native OS
layer) and the CPU core first, then the failing tests at their root cause, then the rest as time allows. Every build, test,
generator run and game run went through `tools/agents/capped_run.ps1 -CpuPercent 10` (5 for the small ones), headless only,
one build at a time, nothing deployed, pushed or merged.

Trees (all in this worktree): `.build/watch` (lifted game tree, SHADOW_SHIPPING=OFF, 48 shards), `.build/tests` (tests
preset). `tools/platform/regen_manifest.py` failed in the fresh worktree, so `.build/dolrecon` is a copy of
wt-opt-integrate's (as instructed). The base exe `wt-gc-removal/.build/perf/verify-sdk/integ3.exe` did not exist, so the base
is my own build of db875b3 (`.build/rf/base/base.exe`): its state.json md5 on the benchmark tape is 3a75268e85cd9037, the
value docs/agents/opt-host2-r3.md records for the same game code.

The map was written before c25cdad (hashed 8192-slot HLE index). Re-checked against the current code:
- RC-tests-6: the slot-table half was already fixed by opt-host2 (the driver uses `Cpu::kHleSlotCount` / `hle_slot_of`); the
  OS-vector half was still broken and is fixed here.
- RC-dispatch-1: unchanged by c25cdad (the entry vector, its sort and the pointers held across a handler are the same); fixed
  here.
- RC-dispatch-2: its first half (a collision-free slot function) IS c25cdad: 48 of 138 M calls take the shared-slot path now.
  The second half (the dispatcher's `find_hle` per iteration) is one slot load for an unhooked pc since 2026-11-04; nothing
  left worth a change. Not touched.

## Status table

Status: fixed / partly / not done / user decision / routed (to opt-runtime). "Before" = the new test run against the unfixed
code; "after" = against the fixed code. Commits are on `agent/recomp-fix`, one per issue (hunks of shared files were split per
issue with a script; an intermediate commit is not guaranteed to build on its own, the tip is what was built and tested).

| issue | status | commit | test and evidence |
|---|---|---|---|
| RC-kernel-1 | fixed | 0e2625f | rw_raster_contract_tests, 9 new stop/resume cases against the original 14 words; before (base tree): "unimplemented primary instruction 0 at 0x8037ECFC"; after: pass. os_threads_contract_tests: 3 new cases pass (not run against the unfixed sleep on their own). Tape unaffected (stop path only). |
| RC-kernel-2 | fixed (a host THREAD's stop inside a callback: not done) | c1d1f85 | gekko_core "wait point inside a native event sees time pass", "boundary stop inside a native event waits for its end" (incl. the savestate refusal); before (source hunks dropped): both FAIL; after: pass |
| RC-dispatch-1 | fixed | aa5c675 | gekko_core "install_hle inside a running handler"; before (base tree): FAIL, the running handler was destroyed; after: pass |
| RC-kernel-3 | fixed | 826622e | os_threads_contract_tests; before (base tree): FAIL "the sleeping thread's GQRs must come back after the event"; after: pass. state.json on the tape unchanged |
| RC-kernel-5 | fixed | aeeafba | gekko_core "single-thread FP policy is set by the System"; before: FAIL; after: pass. The `--no-fpr-shadow` refusal itself: not run |
| RC-kernel-7 | fixed | b9eeac6 | gekko_core "native sleep without a waker is bounded", "advance_game_time with nothing scheduled stops"; before: both FAIL; after: pass |
| RC-gen-2 | partly | c8d6771 | ppcrecomp_output_structure (overlap noted / refused under the strict flag); before (base generator): FAIL; after: pass. Real lists: lifted tree 0 overlaps. Duplicate-row refusal and the install abort: no failing input could be built from lists, not exercised |
| RC-gen-1 | fixed for the roots file | ada7bfb | the tests-tree build stopped on a stale root ("unlisted addresses 0x80082004", a literal of a new test) and built once it was gone |
| RC-gen-5 | fixed | bc9fef9 | ppcrecomp_output_structure; before: generates; after: refused. Real lists pass in both trees |
| RC-gen-8 | fixed | 0c64214 | ppcrecomp_output_structure asserts the emitted tail; the run-time path relies on the dispatcher's existing no-progress tests, no new run-time test |
| RC-gen-10 | not done (user decision, see below) | | |
| RC-fp-2 | fixed (restore and report, no stop) | 52b50f2 | gekko_core "host FP environment is restored"; before: FAIL; after: pass; 0 reports on the replay |
| RC-fp-3 | fixed (guard only) | e4a1af0 | gekko_core "FPSCR policy guard refuses unmodelled modes" + emitted text in ppcrecomp_output_structure; before: FAIL; after: pass |
| RC-memory-2 (handed over by the coordinator) | fixed | 684d071 | flat_memory_tests case I (a second thread faults during a bound run); before (check dropped): 3 FAIL; after: pass. Windowed play: not verified |
| RC-gen-7, runtime half (handed over) | done; the emitter half is opt-runtime's | c8689a9 | native_execution_tests: each reason with its site, a sited-bctr loop is progress, a bctr to itself at the entry is a stall; pass (no "before" run) |
| RC-kernel-6 | fixed | 66372e9 | os_threads_contract_tests (HostWork driven through a handler); a test-gap fix, it passes before and after by construction |
| RC-kernel-12 | fixed | 9cae6b0 | os_report_contract_tests; before (base tree): FAIL "an oversized field width / precision was not refused"; after: pass |
| RC-tests-3 | fixed | 5f07781 | compiler_body_tests passes in 16 s (was: no output until killed) |
| RC-tests-1, RC-tests-2 | fixed | 7d9113d | render_core_tests, render_determinism pass with SHADOW_R2_DIRECT_RESOLVE unset |
| RC-tests-6 | fixed | 5e6e468 | f1c_canary_longrun passes |
| RC-tests-7 | fixed | d9c760d | lift_purity: "146 lifted source file(s), no GameCube concept" |
| RC-tests-8 | fixed | c38c756 | lift_scaffold_inventory prints the debt table |
| RC-gen-4 | partly (`--fresh-layout` + fingerprint) | 88cc492 | ppcrecomp_output_structure |
| RC-gen-12 | fixed except the D1 re-validation | 748988c | ppcrecomp_output_structure (16 malformed inputs refused; blank / CR / comment lines accepted) |
| RC-gen-14, RC-kernel-13 | partly | with the fixes | kernel gaps 1, 2, 3, 6, 8 and the owner-overlap / wait-point gaps now have tests; the rest (generated-fixture suites per instruction family, the end-to-end delivery, the wait watchdog, the os_set_current_context branch) not done |
| RC-dispatch-2 | nothing left to do | | c25cdad did it (see above) |
| RC-gen-13 | not done | | |
| RC-fp-1, RC-tests-4, RC-tests-5, RC-kernel-4, -8, -9, -10, -11, RC-gen-11 | user decision | | untouched |
| RC-memory-1, RC-gen-3, -6, -9, RC-gen-7 emitter half | routed to opt-runtime | | designs below |

## What each fix does (and what it does not)

### RC-kernel-1: a stop inside the native OSSleepThread / VIWaitForRetrace wait
`os_threads::serve_sleep_thread` (runtime/src/gekko/native_os_threads.cpp): when the wait ends without a wake (the host
service inside `idle_wait` asked for a stop: savestate boundary, development link, snapshot, budget; or a host thread's stop),
it restores `r3 = queue` (an event's callback result had replaced it) and hands back AT ITS ENTRY
(`leave_to_dispatcher(entry, Yield, entry)`), so a run that continues after `clear_stop()` enters the wait again. When the
wake and the stop arrive together the call is complete, so it hands back at the return site (the owner thunk does not write
`pc = lr` once the machine is stopped).
`vi_wait` (native_vi_wait.cpp): the loop after the sleep is now its own row, 0x8037ECFC "VIWaitForRetrace after OSSleepThread"
(a third resume row next to 0x8037ECEC / 0x8037ED10). The sleep is entered with LR = 0x8037ECFC as before; when a stop ends
it the frame, r30 (the retrace count read at 0x8037ECEC) and r31 (the restore argument) are exactly the original's at that
point, and the resumed run goes sleep entry -> 0x8037ECFC -> the rest. Shared list line added:
`extract/native-owners-r2.txt`: `0x8037ECFC VIWaitForRetrace after OSSleepThread` (with a 2-line comment above the block).
The generator gives it a thunk and a table row (kNativeOwnerCount 634 -> 635, kFunctionCount +1; nothing else changes).
Not covered: a stop inside one of the EVENT'S CALLBACKS (see RC-kernel-2).

### RC-kernel-2: a wait point inside a native event; a host stop inside it
- `System::advance_game_time` no longer returns at once for "a latched source in a native wait" while that wait's own event is
  running its callbacks (`&& !interrupts_->sync_event_active()`): nothing processes the latched source until the callback
  returns, so a wait point reached there must see time pass. Between events the rule is unchanged.
- `System::request_stop_between_traversals` inside a native event (and no traversal running) no longer stops at once: the
  stop is kept (the existing `boundary_stop_*` fields) and raised by `advance_game_time` at the first wait after the event.
- `NativeInterrupts::event_abandoned()`: set when a stop does cut a callback short (an ExecutionFault inside it, a host
  thread's stop); `save_state` refuses such a machine, as it refuses one whose event is running. Cleared by `reset()` and
  `load_state()`.
Not done: a host THREAD's stop (`request_stop_from_host`, the window's close) inside a callback still ends as
"guest_call: a nested guest call did not return" (terminal stop; the abandoned flag covers the savestate side).

### RC-dispatch-1: handler storage while a handler runs
`Cpu::hle_` is now `std::vector<std::unique_ptr<HleEntry>>` (sorted pointers), a replaced entry moves to `hle_retired_` and
lives until the Cpu is destroyed. `install_hle` therefore never moves or destroys a `std::function` that may be executing,
and the entry pointer the dispatch paths keep across the call (`try_hle_slow`, `serve_native_owner`, the reference
dispatcher's hook path, `serve_hle_for_verify`) stays valid. No handler-depth counting was added to the shipping hot path;
the price is one more dependent load per owner call (`hle_[i]` is a pointer now). `hle_entries()` returns a small view with
the same use (`for (const auto& e : cpu.hle_entries())`, `.size()`, `[i]`, `.front()`): no call site changed.
Limit, stated: the call that replaces its own hook is counted (`hits`) on the retired entry, not on its replacement.

### RC-kernel-3: GQR1-7 and FPSCR across a native event
`NativeInterrupts::drive_sync` saves and restores `spr[GQR1..7]` and `fpscr` next to the FPRs it already restored.
The FPR zeroing itself (RC-kernel-4) is untouched: user decision.

### RC-kernel-5: the FPR-shadow policy
`NativeInterrupts::install` sets `set_interrupt_fpr_shadow(true)` in the single-thread configuration, so every System runs
the policy, not only shadow_main's. shadow_main refuses `--no-fpr-shadow` in that configuration (exit 2 with the reason)
instead of ignoring it; `--help`, the cpu.h comment and FpProbe's line say what is true now.
Effect on tests: every System-based test now runs with the snapshot on (the configuration the game runs).

### RC-kernel-7: waits that never end
- `advance_game_time(kNoDeadline)` with nothing scheduled stops ("nothing is scheduled") instead of moving the clock to ~0
  and spinning; the dead branch and the stale system.h comment are gone.
- The native OSSleepThread is bounded in game time: past `bound_ticks` without a wake it is an ExecutionFault naming the queue
  and the time waited. Default 600 s of game time; `GEKKO_NATIVE_WAIT_BOUND_S=<seconds>` changes it, `0` switches it off.
  Risk as the map says: a legitimate single sleep longer than 10 game minutes would stop; none is known (the game's sleeps end
  at the next retrace or a device completion).

### RC-gen-2: owner extents
Generator: after the cross-row branch check, every surviving non-owner, non-absorbed row is checked against every owned
instruction. It prints one line per overlapping row; with `--strict-owner-extents` (CMake passes it for the lifted tree) the
tree is refused. A function table with two rows for one address is refused by the generator, and
`RecompiledCode::install` aborts on a second, different function for an address (as it does for a slot that does not fit).
Measured: lifted tree 0 overlaps (strict passes); the tests tree's 10 owned words in 2 rows (0x803796CC/0x80379700...,
0x80376128) are printed as notes.
Partly: the D1 merge still runs before owners are known (the gate catches a widened row afterwards), and an "entry" line that
coincides with a row start still becomes an owner row (the tests tree depends on that today: sorting its two rows out of
the lists is a reference-tree design question, left for the coordinator).

### RC-gen-1: stale liveness roots
The check the `native_liveness_roots*` tests run is now a build step (`shadow_liveness_roots_check`, a dependency of the
generator target): it reruns when any runtime source, the roots file or the script changes and fails the build on a stale
file; it does not rerun the generator. The jump-table / wait-point / gx-site lists keep their ctest-only checks (not done).
Computed entries (`base + off`) still root nothing (unchanged).

### RC-gen-5, RC-gen-8, RC-gen-12, RC-gen-4 (generator)
- RC-gen-5: a listed wait point whose back edge a surviving row translates must have been emitted with its wait, else the
  generation fails naming site, head and row. Real lists: passes in both trees.
- RC-gen-8: the cold `_at` trampoline hands back at an unknown entry (`leave_to_dispatcher(entry, NotRecompiled, entry)`)
  instead of running the function from its first instruction; the dispatcher's no-progress rule then stops (production) or
  interprets (reference). This is the only change to every function's generated text (32,913 trampolines + 1,816
  one-instruction functions in the lifted tree; hot bodies untouched).
- RC-gen-12: address lists (native owners, liveness roots, `--only`), jump tables and wait points are parsed strictly
  (file:line named; duplicate wait point refused; `census` must be spelt; blank / comment / CR lines stay legal); an option
  without its value and a non-number `--shards` / `--limit` are refused. Not done: the D1-widened analysis is still not
  re-validated by pass 1.
- RC-gen-4: `--fresh-layout` ignores the directory's `recomp_layout.csv`; every run prints the layout's fingerprint and
  whether it was carried over. Not done: a checked-in layout; kRanges' sort tie-break (ranges are disjoint after the clip).

### RC-fp-2 and RC-fp-3 (guards; no semantics changed)
- RC-fp-2: `Cpu::note_host_fp_control` restores the SSE control word to 0x1F80 when it differs, counts it and prints one line
  per value found; called at the start of every `run()` and after every host service / pacing callback. It restores and
  reports instead of stopping (the map suggested a stop in non-shipping builds): a driver that flips FTZ/DAZ on the game
  thread should not end the user's game, and the restore removes the hazard either way.
- RC-fp-3: `Cpu::fpscr_written()` after mtfsf / mtfsfi / mtfsb1 in both engines; with the guard on (`System::start`) a value
  with RN != 0 or an enable bit stops the machine naming it. Off for a bare Cpu (the instruction tests and the differential
  drivers write every FPSCR value). The lifted tree emits no FPSCR-writing instruction, so its generated text has no guard
  call; native owners that write `c.fpscr` directly are not covered.

### RC-kernel-6, RC-kernel-12
- RC-kernel-6: the two comments say what the code does; the os_threads test names what it really tests (a latched source
  with NO handler) and a new case drives HostWork through a handler.
- RC-kernel-12: widths and precisions are parsed in 64 bits and refused beyond 4096 with a reason
  ("field width too large" / "precision too large").

### Tests (root causes)
- RC-tests-3: `compiler_body_tests` turns decrementer delivery on in `prepare()` (it arms DEC from C++, standing in for a guest
  mtdec), has a game-time watchdog on the waits, unbuffered stdout and `TIMEOUT 600`. The Cpu default is unchanged.
- RC-tests-1/2: `unorm_calibration.cpp` discards the calibration target before the draw that covers it;
  `render_determinism.py` deletes its temp folder.
- RC-tests-6: the canary fixture writes the whole vector page of each exception it takes (`os_exception_page_word`).
- RC-tests-7: the 18 comment lines are neutral; the addresses and console terms moved to
  docs/effects/lighting_materials.md "Address notes for the lifted sources".
- RC-tests-8: the six markers carry their `: why` ("RenderWare's lift removes the translated callers", the reason
  rw_driver.cpp gives for the same scaffold).

## Not decided here (the user's decisions)

| issue | decision needed |
|---|---|
| RC-fp-1 | adopt the hwtests fctiw / fctiwz high word 0xFFF80001 for a negative operand converting to 0, in both engines (changes the reference oracle) |
| RC-tests-5 | difftest_f1b: limit the spec's NI input flush to the non-estimate instructions, or ledger the 3 groups |
| RC-tests-4 | whether the full dolrecon_lift_tests may run, and sharding it into CTest entries (not run here; excluded from the full ctest below) |
| RC-kernel-4 | keep `drive_sync`'s FPR zeroing as a documented divergence, or remove it (untouched here) |
| RC-kernel-8, -9, -10, -11, RC-gen-11 | deleting the upcall bridge, the production-dead external line / native_interrupt_first, timeline.cpp, the NativeJobs bridge, `--hot-set` |
| RC-gen-10 | NOT done, and it needs a decision of the same kind as RC-fp-1: making mcrfs clear the copied bits and mtfsf / mtfsfi / mtfsb recompute FEX / VX (and SRR1[14] on traps) changes the reference oracle, and the difftest spec (test_data/core-audit/spec/fp_A.py) already models these moves, so the fix must come with a ledger update of difftest_f1a / f1b (known groups become stale). No shipping site executes any of them (census: 0 emitted in the lifted tree). |

## Routed to opt-runtime (their files; designs, nothing edited)

| issue | design |
|---|---|
| RC-memory-1 | reserve `kFmWindowBytes + 64 KiB` (PAGE_NOACCESS tail) for both windows in guest_memory.cpp; in `Cpu::fm_fault` accept an operand that ENDS inside the tail and send it to `mem_*_slow` (the unmapped stop) instead of "refusing to guess". Test in flat_memory_tests: a 4-byte read at 0xFFFFFFFE and an 8-byte store at 0xFFFFFFFC reach the slow path. |
| RC-memory-2 | record the guest thread id at `Cpu::run` entry; `fm_fault` on any other thread prints the address and returns EXCEPTION_CONTINUE_SEARCH without touching the Cpu. Test: a second thread reading an uncommitted window page. |
| RC-gen-3 | seed a dispatcher row's labels from jump-tables.txt (or widen it over its listed targets, D1 style) so case arms are gotos; verify with the probe: 0 case words outside the dispatching row, then behaviour gate + benchmark. |
| RC-gen-6 | in `emit_indirect_jump`'s default arm: `if (tmp <= site && c.needs_dispatch()) { c.leave_to_dispatcher(tmp, Yield, site); return; }` before `recomp::jump_indirect`. |
| RC-gen-7 | pass the site into `call_indirect` / `jump_indirect` (link - 4, the bctr address); FpUnavailable / Program reasons for `fp_unavailable` / `illegal_instruction`; delete `exit_at`. |
| RC-gen-9 | lower unconditional `bclrl` like `bcctrl` (`call_indirect(c, tmp, next)`, exit check, pc check); tests tree only (10 sites). |
| RC-gen-11 | delete or retarget `--hot-set` with the volatile-store change (also the user's call as dead code). |

Notes for opt-runtime about what this branch touched in tools/ppcrecomp/main.cpp, so the merge is mechanical: three one-line
`c.fpscr_written();` emissions in group 63 (mtfsb1 / mtfsfi / mtfsf, emitted only when the write can set RN or an enable);
the cold `_at` trampoline's tail (`unknown_entry`); checks and parsers (owner extents, table rows, wait points, list lines,
options); `plan_shards(..., fresh)`. Nothing in the hot-body emission, the memory accessors, `call_indirect`,
`jump_indirect` or b / bl / blr.

## Generated code: what changed (old generator + base lists vs new generator + this branch's lists, lifted configuration)

Both run into scratch directories from the same starting layout. 51 of 52 output files differ, and after normalising the
RC-gen-8 trampoline tail the only remaining differences are the new row: the thunk
`fn_8037ECD4_e8037ECFC` (recomp_047.cpp), its declaration, its `kFunctions` row, `kNativeOwners` (635) and the report's entry
count. Pruning is identical (1,613 rows pruned, 37,743 kept, 601 owner rows); 2 more extra roots (0x80601000, 0x80602000:
data addresses a new test names; `extract/liveness-roots*.txt` regenerated, +2 lines each).

## Coordinator items that arrived during the work

- RC-memory-2: done here from my own reading (the notes folder with the mem agent's diff could not be found). The binding
  lasts for a run, not for ever, so set-up and reports on other threads keep working between runs.
- RC-gen-7: one runtime change, commit c8689a9. `IndirectTarget` is MY name for the call agent's entry (their diff was not
  available): rename on rebase if theirs differs. The stall test's premise versus recording the site: recording the site is
  right. The rule is "gave up at the instruction it was dispatched to"; a bctr's site is the bctr, a later instruction than
  the head, so the loop is progress with or without a site. No assertion was loosened and two cases were added. The two
  assertions that counted a missing computed target as NotRecompiled now expect IndirectTarget (the more precise reason).
- Folding `stopped()` into `exit_pending_`: declined (no proof that every `stop_` writer sets the flag, no A/B).
- The RC-gen-7 item cost a second full rebuild of both trees: the first cpu.h rebuild had finished when it arrived.

## Replay gates (Westopolis tape, 316000 ms, headless; candidate = the branch tip's game exe)

| gate | base (db875b3) | candidate |
|---|---|---|
| state.json md5 (behaviour run, `--no-audio`) | 3a75268e85cd9037 | 3a75268e85cd9037, byte-identical |
| state.json md5 (PCM run) | 3a75268e85cd9037 | 3a75268e85cd9037, byte-identical |
| PCM (`--audio-dump`, 40,375,680 bytes) sha256 | 19fe683a6e87bbff | 19fe683a6e87bbff, byte-identical |
| behaviour gate (`tools/behaviour_diff.py`, 8,553 lines per trace) | | VERDICT: PASS, no field differs |

The candidate printed no host-FP report and no foreign-thread fault line. Performance was not measured (no benchmark run):
RC-dispatch-1 adds one dependent load per owner call; its cost is not verified either way.

## Full ctest (tests tree, Release, `-j 2`, 2,433 s)

264 tests; `dolrecon_lift_tests` excluded (RC-tests-4: the user's decision; not run). The full run had 6 failures. Two were
mine and are fixed in the tip (`os_threads_contract_tests`: a new case did not clear the hand-back exit word as the
dispatcher does; `platform_runtime_coupling`: a reformatted install line was no longer classified). Both pass in the rerun,
together with gekko_core, native_execution, flat_memory, guest_call, os_interrupts, both liveness-roots tests and
platform_gc_removal_plan (10 of 10). The whole set was NOT rerun after those two fixes.

Remaining failures, all in the baseline lists:

| test | cause |
|---|---|
| difftest_f1b | RC-tests-5 (the spec's NI input flush for ps_res / ps_rsqrte): user decision |
| options_settings_tests, options_config_tests | baseline (AGENT_COORDINATION.md); menu tests, not looked at here |
| shipping_platform_selection | needs a reference exe path the tree does not have (as in opt-host2's report) |

Against the baseline lists: render_core_tests, render_determinism, f1c_canary_longrun, lift_purity,
lift_scaffold_inventory and compiler_body_tests pass because of this branch. native_liveness_roots, native_wait_points,
platform_runtime_coupling, ax_pb_tests, lift_sites_reference_tests, native_file_api_tests and native_host_isa_gate also
pass; they were not run on the base here, so whether they already passed at db875b3 is not verified
(native_liveness_roots did: checked by hand).

## Not verified

- No windowed run, and no savestate taken in the real game inside a wait: RC-kernel-1 / -2 rest on the targeted tests.
- The `--no-fpr-shadow` refusal, the GEKKO_NATIVE_WAIT_BOUND_S parser, the duplicate-table-row refusals: written, not exercised.
- The individual commits do not each build; only the tip was built and tested.

