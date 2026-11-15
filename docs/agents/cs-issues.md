# cs-issues (CPU swarm field 5, 2026-11-12)

Branch `cs/issues` (base d526569), worktree `wt-cs-issues`. Contract: docs/agents/CS_2026-11-12.md. Every build / test / generator run
went through `capped_run.ps1 -CpuPercent 7 -Lock cs-slot-a`; nothing was timed; no game-tree build, no game run. The tests tree was
configured once (`.build/tests`); only `gekko_core_tests` and `os_threads_contract_tests` were built, plus `ppcrecomp` (item 3/4 patches).

## Summary

| item | map row | result | commit |
|---|---|---|---|
| 1 | RC-kernel-13 gaps 4, 5, 7 | two new tests, pass on the unchanged runtime (gaps closed, no bug found); five local mutations each fail the intended case | d8fc9a0 |
| 2 | RC-kernel-6 before-run | recorded: new case FAILS against a runtime that refuses HostWork, the pre-66372e9 test PASSES against it | 2d1ab5c |
| 3 | RC-gen-2 reference tree | 0x803796FC fixed by a list move (lifted output byte-identical); 0x80376128 needs a generator exemption (patch written, validated) | d18f49a (+ patch below) |
| 4 | RC-gen-13 | no instance in this DOL (scanner, three trees of evidence); generator change written as a patch, validated, not applied | see below |

No runtime fix was needed: every new test passed on the existing code. No cpu.h, flat_memory, recomp.h or main.cpp edit is committed (the
generator patches are files under docs/agents/cs-issues-patches/).

## 1. RC-kernel-13 gaps 4, 5, 7 (gekko_core_tests.cpp)

`test_single_thread_delivery_round_trip_restores_fprs_and_balances_reschedule` - a System, one delivery taken by the interpreter from
interrupted code whose MSR[FP] is clear; the Vi handler calls a guest function (`fneg f1,f1; fcmpu cr0,f1,f2; blr`) that clobbers f1
and FPSCR, then finishes. Asserts: step 0 sees MSR[FP] and the exception context's SRR1[FP] forced and that context current (gap 7:
`os_set_current_context`, single-thread branch); Reschedule is 4 inside, 3 again after (balanced; the start value was 3);
`stats.reschedules == 0` (no SelectThread), `native_returns == 1`, `guest_calls == 1`; f1/f2/f5 and FPSCR are the interrupted code's
again (the host snapshot, gap 4); r1/r3/r4/pc are restored; the OS current-context word is the interrupted thread's again; and
MSR[FP] plus the interrupted context's SRR1[FP] are set on resume.

`test_wait_watchdog_stops_a_guest_that_never_waits` (gap 5) - a recompiled spin entry run by `System::run()`:
(a) limit 120 ms, spin forever: stopped, ExecutionFault, detail names "wait-point watchdog" and "limit 120 ms";
(b) limit 300 ms, the spin calls `advance_game_time` every 10 ms for 900 ms: not stopped by the watchdog (ends by itself);
(c) limit 0: the watchdog is off (only the test's own guard stops it). A guard thread (5 s / 400 ms) keeps a broken watchdog from
hanging the test.

Run (clean sources): gekko_core_tests Passed (5.7-10 s). Mutations, each applied locally by `.build/mut.py`, built, run, reverted
(sources restored from backups; `git status` clean afterwards; the final clean run Passed again):

| mutation | file | result |
|---|---|---|
| `fp_not_forced`: single-thread branch of os_set_current_context stops setting MSR[FP]/SRR1[FP] | native_interrupts.cpp | FAIL: "exception 0x0800 taken at pc 0x80100B00 ... FP-unavailable" (the callee's first FP instruction) |
| `reschedule_unbalanced`: finish() no longer decrements Reschedule | native_interrupts.cpp | FAIL "Reschedule must be balanced by the delivery's end" |
| `no_fpr_shadow`: install() no longer turns the FPR snapshot on | native_interrupts.cpp | FAIL "the FPR snapshot must give the interrupted code its registers back" (and the RC-kernel-5 test) |
| `watchdog_silent`: the watchdog never calls request_stop_from_host | system.cpp | FAIL "must be stopped by the watchdog, naming it" (the test guard fired) |
| `watchdog_ignores_waits`: advance_game_time no longer restarts the guest-execution clock | system.cpp | FAIL "must not be stopped by the watchdog (got: ... 307 ms ... limit 300 ms)" |

The wall-clock cases use generous margins (limit 120/300 ms, 50 ms poll); under the 7 % cap they were stable in every run above (5 runs).

## 2. RC-kernel-6 before-run (os_threads_contract_tests.cpp, comment only)

recomp-fix's new case (HostWork driven through a handler) had no before-run. Reproduced against a mutated runtime: in
`NativeInterrupts::run_pending_event` HostWork is refused ("latched with no handler", what the old header comment said) even when a
handler is installed.

| test | clean runtime | runtime refusing HostWork |
|---|---|---|
| os_threads_contract_tests at HEAD (new case) | Passed | FAIL: "HostWork with a handler must be driven by the wait" |
| os_threads_contract_tests of 66372e9^ (old case: no handler installed) | Passed | Passed (the old case tests only the generic "no handler" stop) |

So the old test could not tell the two runtimes apart and the new one can. The mutation is not committed; the committed change is a
comment in the test recording this.

## 3. RC-gen-2, reference-tree part

Which list lines create the overlaps (base generator, tests configuration, `--strict-owner-extents`, `--why-live`):

- **0x803796FC.** `extract/native-owners.txt` had `0x803796FC entry: native boot: a constructor returned ...`. functions.csv has a row
  `fn_803796fc` (size 4) at that address, so the line became a whole OWNER ROW (RC-gen-2: an "entry" line that coincides with a row
  start), extent [0x803796FC, 0x80379720). The tests tree keeps the original `__init_cpp` (fn_803796CC, live through the runtime literal
  root 0x803796F4: the reference runner's `--original-boot` runs it), which contains those 9 words (`goto L_80379700`). In the lifted
  tree `native-owners-r2.txt` owns `__init_cpp` itself, so there the two never coexist. **Fix (committed): the line moves to
  native-owners-r2.txt, next to `0x803796CC __init_cpp`, with a comment in both files.** The generator unions both files into a
  `std::set` and ignores names, so the lifted input is identical by construction; measured: lifted configuration, old lists vs new
  lists, `--fresh-layout`, md5 of all 52 generated files identical, strict passes in both. `native_file_api_tests` compares the runtime
  owner addresses minus the r2 lines with the base list: 0x803796FC is installed by NativeBoot and now only in r2, so both sides lose it
  (reasoned, not run: the target links the full runtime; the coordinator should include it in the next batch).
  Tests-tree consequence to check in the next full tests-tree build: the 0x803796FC hook is now served through the hooked-range path
  (hook once, then the row's range; cpu.cpp ~3075-3100), not through an owner thunk. Tests that boot through the constructors in the
  reference build (native boot and --original-boot) exercise exactly that.
- **fn_80376128.** The row [0x80376128, 0x80376130) is the resume point after the `mtmsr` of OSDisableInterrupts (owner 0x8037611C,
  [0x8037611C, 0x80376130)). why-live: kept by OSLoadContext (0x80372C68, a liveness root of the tests tree) forming the address 0x80376128
  as its `__RAS_OSDisableInterrupts_end` comparison constant. No list can sort this out: functions.csv/native-owners.txt are shared with
  the lifted tree (changing the owner's size changes its table row and the lifted output), the roots file is derived from runtime
  literals, and an owner entry line would turn the row into an owner row that the runtime must install (a runtime change, and not a bug
  fix). The row is the design the generator already documents (main.cpp ~4228: "the resume point after an mtmsr in a function the
  runtime now owns - stays a row of its own"), and its original tail words (`rlwinm r3,r3,17,31,31; blr`) are exact. **Patch for routing
  (not applied): docs/agents/cs-issues-patches/rc-gen-2-owner-resume-point.patch**: in the `--strict-owner-extents` loop a row whose
  first owned word is its own start, preceded by an owned `mtmsr`, is printed as a note and not refused.

Validation of the patch (ppcrecomp built from the patched main.cpp, then main.cpp restored; all runs `--fresh-layout`):

| run | result |
|---|---|
| patched exe, tests config, OLD lists, strict | refused: only 0x803796CC (and the 0x80376128 note) |
| patched exe, tests config, NEW lists, strict | **passes** (exit 0, the 0x80376128 note) |
| base exe, tests config, NEW lists, not strict | notes 0x80376128 only |
| patched strict vs base not strict, tests config | all generated files identical |
| base exe, lifted config, old vs new lists | byte-identical, strict passes |
| patched exe, lifted config, new lists | byte-identical to the base exe's |

## 4. RC-gen-13 (address formation)

Scanner (committed): `tools/ppcrecomp/scan_address_formation.py` - reads extract/functions.jsonl (every function's disassembly) and a
recomp_pruned.csv, runs exact straight-line constant propagation (reset at labels, calls, unconditional branches; `--loose` keeps values
across them, the over-approximation H5's any-pair rule makes), and reports every formed word-aligned value that lands inside a function
extent, classified: `direct` (hi half into low half: H5/H6 see it), `copy_hi` (a copy of the high half before the low half: `mr`,
`addi rD,rA,0`), `li-addis`, `chained` (two non-zero low halves). Rows of owners are skipped (`--owners`), since an owner's body is not
emitted.

Results (d526569; pruned lists of the tests tree, 1,282 rows, and of the lifted tree, 1,613 rows):

- A value formed by a live, non-owner function that lands in a PRUNED row's extent: **0**, in every shape, tight and `--loose`, both trees.
  So no row is currently lost to a shape H5/H6 miss.
- Shapes outside `direct` that form any code address at all (`--all-starts`, no filter): 0 `copy_hi`, 0 `li-addis`; `chained`: 1 tight hit
  (site 0x803A0024 in fn_803a0000: data-address arithmetic landing 0x80370B20, the interior of the 20-byte PPCHalt owner), no code use;
  the `--loose` extras are `lis`/`addi` bases walked with +4 strides over tables in a text-section pool, again data.
- The one literal copy shape in the DOL: EXIInit (an owner) does `lis r4,hi; addi r28,r4,lo; addi r4,r28,0` and passes r4 on. The
  copy is made AFTER the formation, and H6 forms the value at the first `addi` (Exact), so it is caught anyway.
- Code addresses held in data words: 24,012 aligned data words point into code, **all are function starts**, none interior (`--dol`), so
  the `block_starts` omission has no reachable instance: H6 can only stop at a block start that is already a row start.

Verdict: suspected, not an instance in this DOL (three searches above); the missed shapes are real gaps of the algorithm, not of the data.
**Generator change for routing (not applied): docs/agents/cs-issues-patches/rc-gen-13-liveness-copies.patch** - H6's walk-back follows
`mr` / `addi rD,rA,imm` writes of the traced register (copy and chained addi, summing the immediates; ori chains and `li`+`addis` are
left conservative) and `block_starts` gains every code address a data word holds. Validated: it compiles with MSVC (the patched
ppcrecomp above), clang -fsyntax-only is clean, and it changes NO generated file of either tree (tests: patched == unpatched, lifted:
patched == unpatched; the combined patched exe is the one in the table above). It is a robustness fix, not a behaviour change.

## Open / routing

- Route `rc-gen-2-owner-resume-point.patch` (and, optionally, `rc-gen-13-liveness-copies.patch`) to session opt-runtime (main.cpp owner).
  They apply independently with `git apply` (checked against d526569); with the RC-gen-2 patch the CMake `--strict-owner-extents` flag
  can be passed for the tests tree as well (a decision: it makes the reference build refuse an overlap it only notes today).
- Include `native_file_api_tests` and the reference-tree boot tests in the next full tests-tree run for the 0x803796FC list move.
- `.build/mut.py` (mutation harness) and the generator scripts live in the ignored `.build` of the worktree, not committed.
