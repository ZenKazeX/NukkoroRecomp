# S1 core report (2026-11-12)

Branch `s1/core` (cut from e6b3a35). Map: `docs/re/core.md`. Claims: `s1-claims/core.txt` (0x80015000-0x80016000 released to the
enemy agent by the coordinator; no work was done there).

## What was analysed

* The shared low .text 0x80009000-0x80050000 (2011 functions) and 0x800C0000-0x800CB400, with callers per subsystem, the leaf
  families (counted handles, Vec3 copies, bit helpers, debug-print stubs), the layout manager, the shape library and its sweeps,
  CharaColli (host-served already). Profile: the coordinator's `profile-base-700f72d.json`: the field is 15.4 % of the game
  thread by self time, flat (largest single function 0.85 %).
* No Tier E owner was attempted for: the counted-handle leaves (~3.4 %; leaf owners do not pay, the cost is lifted-call overhead,
  a generator-inlining item), the swept-sphere world query 0x8001EE0C / 0x800221A8 / 0x80024AC8 (~1.2 %; ~1,500 instructions of
  exact float code with nested allocating guest calls, no mid-function re-entry), CharaColli (opt territory).

## Owner: LayoutScan1 0x800CAC08 (hand-back owner, not pruned)

Files: `runtime/include/gekko/native_s1_core.h`, `runtime/src/gekko/native_s1_core.cpp`, test
`runtime/tests/s1_core_layout_scan_tests.cpp`. Design: scan the 0x800 slots without writing; on a frame with nothing to create,
restore or mark (97.6 % of the calls on the benchmark) write the live count and the stack words the original's prologue and last
SpawnTest call leave, and return; otherwise **decline before any write** and the original body runs the whole frame. The address is
not in `extract/native-owners.txt` (the body stays for the hand-back); **Withdrawn by default** (no measurable gain, below): the hook is installed but declines unless `GEKKO_S1_CORE_SCAN=1`; `GEKKO_S1_CORE_STATS=1`
prints served/declined at exit.

### Gates
* Contract test (`s1_core_layout_scan_tests`, reference interpreter, pinned DOL, 0x800CA970 served by NativeObjectRange as in the
  game): 8,100 random frames (2,500 quiet: all 2,500 served; 2,500 with 1-3 slots that need the factory / restore / mark: 1,093
  served, 1,407 declined; 1,500 random flags: 101 served), compared after each frame over the manager, point, records, all 0x800
  slots, the stack window, r1 r2 r13 r27-r31 LR: all equal. Negative controls caught: wrong count (282), missing prologue word (300),
  missing spawn-test frame word (300), serving a spawning frame (382), clobbered r28 (100). Directed: a null record is declined
  without a write. Command: `.build/tests/Release/s1_core_layout_scan_tests.exe main.dol` under `capped_run -CpuPercent 7`, 101 s.
* Whole run (candidate cb84bb7552e6, build of 65c96e0 + stats print): state.json md5 **3a75268e85cd** and PCM **c64ee7883f980f73**,
  both identical to the base; behaviour gate `behaviour_diff.py`: no field differs in any bit over 6,320 rows, events identical
  (1,521): PASS. Layout scan calls on the tape: 18,862, served 18,405, declined 457 (all spawn / restore frames, none for a null
  record).

### Benchmark
Alternating A (base 700f72d) / B (candidate, scan enabled) rounds of the unpaced headless Westopolis replay, 5 rounds each, under
`slot_bench.ps1` (slot s1-slot-b + benchmark, 7 % cap); the timing script's `bc` was missing so wall times were taken from the
arrival time of each round's result line (rounds 2-5, +-1 s; round 1 and the first A of round 2 are discarded or unmeasured):

| arm | kept rounds (s) | mean | spread |
|---|---|---|---|
| A base | 334, 289, 298 | 307 | 45 |
| B candidate | 256, 323, 265, 316 | 290 | 67 |

All ten runs end with state.json md5 3a75268e85cd. The arms' spreads overlap completely (machine contention at the 7 % cap moves a
run by +-10 %); the expected gain of the owner is ~0.5 % of the game thread (profile: the scan + SpawnTest ~0.9 % self, of which a
native scan keeps a fraction). **No measurable change: the owner is withdrawn** (code and contract test stay in the tree, off by
default, for a later round that measures CPU time per process instead of wall time).

## Shared-list lines changed
* `CMakeLists.txt`: `runtime/src/gekko/native_s1_core.cpp` in the shadow_runtime_common list (block `# s1/core`), the test target
  `s1_core_layout_scan_tests` after `mtn_pose_contract_tests`.
* `runtime/include/gekko/system.h`: `#include "gekko/native_s1_core.h"`; `runtime/src/gekko/system.cpp`: `s1_core::install(cpu_);`.
* `extract/native-owners.txt`: no line (body kept for the hand-back). Liveness lists: the new source adds the literal 0x800CAD8C marked
  `// liveness-roots: value-only`; **not regenerated here** (the generator tooling was not run; the coordinator regenerates both lists at merge).

## Open questions
1. Generator-level inlining of the counted-handle / Vec3 leaves (~4 % of the game thread in call overhead).
2. A Tier E owner of the swept-sphere query (needs the RenderWare world BSP mirrored natively as NativeWorldCollision does).
3. Process note: I killed three processes of my own profile queue (cap.sh / prof.sh of wt-s1-core) by a command-line match before the
   coordinator's rule on process safety; they were all my own jobs.

## Round 4 (2026-11-12, amendment 4; no timing run was made)

Merged s1/integrate (f6acce9) into s1/core first.

### LayoutScan1 0x800CAC08: window-ready
* Switches: run time `GEKKO_S1_CORE_SCAN=1` enables / `=0` disables; build time **CMake option `SHADOW_S1_CORE_SCAN` (default OFF)**
  compiles the owner in as default-on (`SHADOW_S1_CORE_SCAN_DEFAULT`, `CMakeLists.txt`, block `# s1/core`). `GEKKO_S1_CORE_STATS=1` prints
  served / declined at exit. The address is still not in `native-owners.txt` (the body is kept for the hand-back).
* After the merge, on the merged tree (exe f69a580944bb, built with the option OFF, scan enabled by the env switch): contract test
  `s1_core_layout_scan_tests` PASS (8,100 frames, controls caught 282 + 300 + 300 + 382 + 100; the test enables the owner itself);
  state.json **3a75268e85cd** and PCM **c64ee7883f980f73** identical to the base; behaviour gate with the combined layout
  (`layout-combined.txt`) against `base-combined-1`: no field differs in any bit over 6,320 rows, 2,231 events identical, PASS.
* Not measured (no timing in round 4); the round-1 wall times are in the section above and showed no measurable change.

### Swept-sphere world query 0x8001EE0C -> 0x800221A8 / 0x80024AC8: NOT built; why (docs/re/core.md section 7)
* The chain is 1.81 % of the game thread (203 samples), 0x80024AC8 never runs on the tape. 0x800221A8 / 0x80023B28 / 0x80024AC8 are pure
  float functions (no bctrl, out-pointer writes only, ~3,200 instructions); 0x8001EE0C is not (guest filter callback, RenderWare
  collision-plugin accessors, range-object pool allocation, deleting destructor of the previous hit).
* An exact owner is possible but not provable with the means of this round: it needs a fast-path / `fps_*` two-path evaluation of the
  whole pure function (not hand-checkable at 3,200 instructions), a guest-memory fixture of triangle lists, sphere sweeps and (for the walk)
  a RenderWare world BSP with the pool state, and it saves at most ~1.2 % of the game thread, which no measurement before the quiet window
  can resolve. The generator-level alternative (policy-templated emission for pure float leaves, proven with the existing world-collision
  verifier journal) is described in section 7 and left to the generator track. No code was written for it.

## Round 5 (2026-11-12, amendment 5; no timing run was made)

s1/integrate merged first. LayoutScan1 is KEPT (quiet window: median -1.5 %, 5/5 faster).

### SweepSphereTriangle 0x800221A8 + its pure call closure (30 functions) - window-ready
* **What**: the game's swept-sphere vs triangle (+ clip-plane list + shape list) test, 1,178,912 calls on the Westopolis tape. One hook on
  0x800221A8 serves the whole closure natively: 0x80023B28 (only reached from it: not a separate owner), 0x80025E2C, 0x800245C8, 0x80020F48,
  0x80035E2C, the vector / pointer helpers, sqrt etc. The original translated bodies stay (hand-back / A-B).
* **How (exact by construction)**: `tools/s1_core/lift_locals.py <build>/generated <out.cpp> fn_800221A8` re-emits the translated bodies of the
  closure with the same statements, the same `ppc::` helpers (fps_*, fp_compare, psq_store / psq_load, cmp ...), the same memory accesses in the
  same order; only `c.gpr[n]` / `c.fpr[n].bits[k]` become C++ locals (spilled around calls only for the callee's read set and at every exit,
  reloaded for its write set), and the per-instruction MSR[FP] checks are done once at entry (hand back when FP is off). A callee that has an
  owner installed (`has_hle`) is served by it as the translated call would; a callee without a translated body is called through the translated
  table (`native_s1_call.cpp`, compiled into both runtime variants; the reference build runs it in the nested interpreter). No float arithmetic
  was rewritten, so there is no "fast path vs fps_* path" to prove: the arithmetic is the translated code's own; the gain is the register
  file in locals, no per-call thunk / exit checks and one hook for 30 functions. Generated file `runtime/src/gekko/native_s1_sweep_lifted.cpp`
  (10k lines, `// liveness-roots: value-only` on every address literal), glue `native_s1_sweep.{h,cpp}`.
* **Switches**: `GEKKO_S1_SWEEP=1/0` (run time), CMake option `SHADOW_S1_SWEEP` (default OFF; ON = default-on, `SHADOW_S1_SWEEP_DEFAULT`),
  `GEKKO_S1_SWEEP_STATS=1` prints `s1 sweep: <calls> calls, <served> served` at exit. Liveness lists regenerated (+0x800221A8 only, both lists).
* **Contract test** `s1_core_sweep_tests` (reference interpreter, pinned DOL; the same vector / RenderWare-vector owners installed on both machines):
  300,000 random sweeps (degenerate triangles, starts on face / edge / vertex / away, motions zero / tiny / long / axis aligned, radii 0 .. 50, NaN /
  inf / denormal inputs, 0 or 1-4 clip planes, 0 or 1-4 shapes in the shape list), 54,658 returning a hit; compared after each call: the 4 KiB arena
  (inputs and all outputs), the 0x940-byte stack window (dead frame words of the whole closure), r1 r2 r3 r13 r14-r31, f14-f31 both halves, CR2-4, LR,
  CTR, FPSCR except FPCC. PASS. Negative controls caught: flipped output bit (300/300), clobbered r28, missing frame word, wrong return value,
  flipped FPSCR bit (100/100 each). FPU off hands back before any write. Original-instruction coverage: 0x800221A8 1551/1598, 0x80023B28 603/650,
  0x80025E2C 284/385 (missed 0x80026148-0x800262D0), 0x800245C8 137/157, 0x80020F48 170/170, 0x80035E2C 56/56, 0x8003B7D4 61/82; the missed
  blocks are rare branches (not reached by the geometry classes), executed by the same generated statements as the covered ones.
* **Whole run** (build 16c4acb725ca, scan + sweep enabled by env): state.json 3a75268e85cd and PCM c64ee7883f980f73 identical to base in the sweep-only
  and the sweep + LayoutScan1 runs; behaviour gate with the combined layout against `base-combined-1`: no field differs in any bit over 6,320 rows,
  2,231 events identical, PASS (both runs). Counters: sweep 1,178,912 calls, all served; LayoutScan1 18,862 calls, 18,405 served.
* **Expected ceiling**: the closure is ~95 of 11,230 samples = ~0.85 % self of the game thread (0x800221A8 .47, 0x80023B28 .10, the helpers .25).
  Removing all of it would gain 0.85 %; the register-locals form removes the register-file traffic and the call plumbing but keeps the arithmetic,
  so the realistic gain is 0.3-0.5 % of the game thread (not measurable before the window; the main session times it).

### Not done, with reasons
* 0x8001EE0C stays guest (as asked); the generator would need `bctrl` support for it.
* 0x80021678 (line pick, served by NativeWorldCollision), 0x80021480 (calls 0x8001EE0C), 0x800201D4 (allocates result objects through the range
  pools and vtables): not pure; no contract fixture built. 0x80021934's 168-slot loop (0x8003AA58 .34 %) would be the next candidate for the
  generator (needs a character-table fixture and 0x8001EE0C as a guest callee).
* 0x800CA8D8 / 0x800CA970 / 0x800CAE24: ceiling ~0: LayoutScan1 serves 97.6 % of the scans, so the guest versions run only on the 457
  spawn / restore frames of the tape (0x800CAE24 is not in the profile top list).

## Round 6 (2026-11-13, amendment 6; no timing run was made)

* **Swept sphere (76741fb): withdrawn by window 2 (+2.7 % median, slower 5/5). REVERTED in b9270aa**: the sources (`native_s1_sweep*.cpp`,
  `native_s1_call.cpp`, the generated 10k-line lifted file), the generator `tools/s1_core/lift_locals.py`, its test and the CMake option are
  removed; the liveness lists regenerated. Round-5 text above stays as history (git history has the code). Likely cause (not verified): the
  lifted bodies spill ~60 registers around every nested call and keep every helper a separate out-of-line function, which costs more than
  the translated code's own register file.
* **Counted-handle family: map in docs/re/core.md section 8; owner `s1_shptr` (hand-written native C++20; `native_s1_shptr.{h,cpp}`).**
  - Entries: 10 shared members (0x8000945C release, 0x80009408 shared_count dtor, 0x800097E0 copy, 0x80009E4C assign, 0x80017454 weak_count dtor,
    0x80017930 / 0x8001AEE8 weak_count copy, 0x80016914 add_ref_lock, 0x80016838 shared_count-from-weak, 0x80028DB0 weak_ptr::lock) and the hot
    per-type clones found by exact instruction match over the DOL (16 of 132 shared_ptr dtor wrappers, 12 of 234 copy constructors, 3 of 118
    assignments). Weak count lives at +8 of the same control block (0x80017454 is a weak_count dtor, not an intrusive pointer: corrected).
  - Every served call reproduces the translated writes: the counts and the dead stack frames of every level in the translated order
    (back chains, LR words with the nested return addresses, saved r28..r31), so state.json stays byte-identical.
  - Hand-back before any write: last strong release (dispose virtual), weak count reaching zero (destroy virtual), operator delete (flag > 0),
    add_ref_lock of an expired block / null block (throw), and any chain member covered by a foreign hook (`--watch`, `--context-hook`, any other
    install_hle; names without the "S1 shptr" prefix), asked once per entry. A watch on an entry's own address replaces the owner's hook.
  - **What is hooked**: the chain entries only (shared_count dtor, assign, from-weak, lock, the dtor / copy / assign wrapper clones; 36 hooks).
    The leaves (shared_count / weak_count copy: 22.6 M calls of 5-9 instructions), the weak_count dtor (24.4 M single-level calls), release (always
    the last release when entered directly: 100,829 hand-backs, 0 served) and add_ref_lock (0 calls) are served by `serve()` and covered by the contract
    test, but NOT hooked: their guest bodies are as short as the dispatch. `GEKKO_S1_SHPTR_ALL=1` hooks every entry (to time the difference).
  - **Switches**: `GEKKO_S1_SHPTR=1/0`, CMake option `SHADOW_S1_SHPTR` (default OFF; ON = default-on, `SHADOW_S1_SHPTR_DEFAULT`),
    `GEKKO_S1_SHPTR_STATS=1` (served / handed back per kind at exit), `GEKKO_S1_SHPTR_ALL=1`. Liveness lists regenerated (the new addresses).
  - **Contract test `s1_core_shptr_tests`** (reference interpreter, pinned DOL): 53,600 calls: 400 rounds of every one of the 41 entries on fresh
    fixtures plus 3,000 random 12-call sequences over the same blocks (copy / assign / release / lock interleavings, state carried between calls); control
    block counts from {0, 1, 2, 3, 5, 0xFFFFFFFF}, empty and non-empty handles, flags <= 0 (s16). Compared after every call: the 4 KiB arena, the 0x340-byte
    stack window, r1 r2 r3 r13 r14-r31, CR2-4, LR. PASS. Directed: operator delete and a foreign hook on a chain member are handed back with no write.
    Negative controls, one per mandatory case, all caught: last strong release served anyway (200/200), weak count reaching zero served anyway
    (167/200), lock of an expired block incrementing (200/200), a count touched by the copy of an empty pointer (200/200), a lost increment in a
    copy / release interleaving (200/200), a missing dead-stack word of the shared_count dtor frame (200/200).
  - **Whole run** (build 59cd348d2c85): state.json 3a75268e85cd and PCM c64ee7883f980f73 identical to base with the default hook set, with it plus
    LayoutScan1, and with `GEKKO_S1_SHPTR_ALL=1`; behaviour gate (combined layout) vs base-combined-1 (and -2): no field differs in any bit over 6,320
    rows, 2,231 events identical, PASS in the runs above.
  - **Counts on the tape** (one state run): hooked chains: dtor_wrap 26.39 M served / 21,093 handed back, copy_wrap 10.42 M / 0, lock 9.999 M / 0,
    assign_wrap 5.60 M / 4,686, sc_from_weak 4.20 M / 0, sc_dtor 0.50 M / 95,971, assign 0.205 M / 4,858: 57.3 M hooked calls per tape. Unhooked (ALL=1):
    weak_copy 21.8 M, weak_dtor 24.4 M, sc_copy 0.79 M, release 0.10 M (all hand-backs).
  - **Expected ceiling**: the guest self time of the hooked chains in the base profile is about 3.0 % of the game thread (wrappers .7, 0x80009408 .62,
    0x8000945C .53, lock chain .45, from-weak .37, copies .25, assignments .1); the leaves excluded add ~1.0 %. The honest range is -1.6 % to +1.4 %:
    57.3 M hooked calls cost one dispatch each, i.e. 0.57 s per 10 ns of dispatch over a ~35 s run (1.6 %), so the owner pays only if the dispatch is
    well below ~10 ns, and the measured family guest cost per call is already ~12 ns (4.3 % self over ~120 M calls). A dispatch-free route (the
    generator emitting these chains inline into their callers, with the hook / watch check kept) is the real fix; this owner is the exact baseline for
    that comparison. The window decides.

## Round 7 (2026-11-13, amendment 7; no timing run was made)

Merged agent/opt-integrate 0f54a54 (and s1/integrate) first. Ranking: docs/re/core.md section 9 (profile of the kept build with the stack
sampler, 10,855 samples; top 15 with address, inclusive %, net %, identity and verdict; tools `tools/s1_core/rank_trees.py`, `rank_closure.py`).

### Candidate 1: CharSweep 0x80021934 (`s1_chars`, hand-written; `native_s1_chars.{h,cpp}`, `native_s1_call.cpp`)
* **Why**: rank 1 of the exact, ownable trees: 5.52 % inclusive / 3.18 % net of the game thread; the contact code 0x80098ABC (6.17 %) calls it every
  frame. Its own guest cost is the 168-slot loop: two frame-less guest calls per slot (0x8003AA58 .52 % self, 0x8003A568) plus the loop body (.22 %).
* **What it does**: the whole function natively: prologue / epilogue frame words, the leaf 0x80021C80 and both slot leaves inlined (they write only
  two halfwords at [r1 + 0x10] / [r1 + 0x14]), and every other callee (0x80045AC0, 0x80021480, 0x80021C04, 0x80021BC4, 0x80021B08, 0x800C51C8,
  0x80021B74) called through the translated table with the registers, stack-argument words and LR the translated code sets, written back to the Cpu
  before each call so a callee that leaves to the dispatcher (the sweep 0x8001EE0C yields at its loop back edges) resumes the translated function with
  exact state. Hand-back before any write: MSR[FP] clear with mode bit 1; a hook of any kind on an inlined leaf.
* **Switches**: `GEKKO_S1_CHARS=1/0`, CMake option `SHADOW_S1_CHARS` (default OFF), `GEKKO_S1_CHARS_STATS=1`
  (calls, served, hand-backs, slots tested, slots swept).
* **Contract test `s1_core_chars_tests`** (reference interpreter, pinned DOL; both machines hook the seven callees with logging stubs that record every
  argument register, the stack words and f1, in order, and return values derived from the arguments, so the r10 -> r31 chaining is observable; the three
  inlined leaves run as original code on the original machine): 6,500 calls over every mode-flag combination, random slot occupancy in both banks, null
  characters / bodies, a null manager, the stack flag byte 0 / 1; 163,025 slot sweeps. Compared: the whole 16 KiB arena with the log, the 0x240-byte
  stack window, r1 r2 r3 r13 r14-r31, f1, LR. PASS. Controls caught (100/100 each): a missing frame word, a wrong return value, a corrupted callee
  argument, a clobbered non-volatile register, a wrong stack-argument word. FPU off hands back with no write.
* **Whole run** (build f5dfb0e4d943, scan + shptr + Mtn defaults on, chars on): state.json 3a75268e85cd and PCM c64ee7883f980f73 identical to base;
  behaviour gate against base-combined-1 and -2: PASS (no field differs in any bit). Counters: 86,862 calls, 73,864 completed natively, the other 12,998 left to
  the dispatcher inside a callee and resumed in the translated function (exact by the resume contract), 14.59 M slots tested, 323,478 swept.
* **Expected ceiling**: ~0.85 % of the game thread (0x8003AA58 .52 + the function's own .22 + 0x8003A568 + the 29 M saved guest leaf calls' plumbing);
  the native loop costs ~0.1 % (14.6 M iterations of 3 loads). The 3.18 % net of the whole tree is mostly the sweep below it (next assignment).

### Candidate 2: weak_count holder destructor wrappers (`SHPTR_WEAKWRAP`, in `s1_shptr`)
* The ranking found a 2-level chain the round-6 owner left guest: 0x8001FC28 (+ clones) -> 0x80017454: 0.77 % inclusive, 24.2 M calls per tape. New kind
  `WeakDtorWrap` (37 clones found by instruction match; hooked: 0x8001FC28, 0x80051CFC, 0x80021B74, 0x800741BC, 0x8005FF1C, 0x8006E52C), same contract
  machinery as the other wrappers: frame words of both levels in order, the weak count at +8 of the block, hand-back for weak -> 0 (destroy virtual) and
  operator delete, foreign-hook check on 0x80017454.
* **Switches**: `GEKKO_S1_SHPTR_WEAKWRAP=1/0` (run time), CMake option `SHADOW_S1_SHPTR_WEAKWRAP` (default OFF); everything else of s1_shptr unchanged
  (the kept default hook set does not include it). Stats via `GEKKO_S1_SHPTR_STATS=1` (weak_dtor_wrap line).
* **Contract**: s1_core_shptr_tests now 56,200 calls (the new kind driven singly and in sequences) PASS, plus a negative control (serving the wrapper's last
  weak release anyway: caught 200/200 among the existing six).
* **Whole run** (build c530bc5b2cb2): state 3a75268e85cd + PCM c64ee7883f980f73 identical, behaviour gate PASS vs base-combined-1 and -2, with the wrapper
  hooks alone and together with chars. 24,243,329 served, 146 handed back.
* **Expected ceiling**: ~0.7-1.0 % guest (wrapper .30 leaf + 0x80017454 .45 under it) against 24.2 M dispatches (about 10 ns each = 0.7 % of a 35 s run):
  an honest range of about -0.3 % to +0.8 %. The round-6 lesson applies (the chain replacement paid for dtor_wrap, the single-level leaf did not): the
  wrappers replace a two-call chain by one hook and leave the unhooked weak_dtor leaf guest as before.

### Shared-list lines
CMakeLists.txt (s1/core blocks: `native_s1_chars.cpp` in the common list, `native_s1_call.cpp` in both runtime libraries, options `SHADOW_S1_CHARS` and
`SHADOW_S1_SHPTR_WEAKWRAP` default OFF, test target `s1_core_chars_tests`), system.h include, system.cpp `s1_chars::install(cpu_)`; liveness lists regenerated twice
(roots: 0x80021934 and its call targets / link addresses, the six wrapper clones).

## Round 8 (2026-11-13, amendment 8; no timing run was made)
Assignment: the hand-written sweep tree under chars (0x80021480 -> 0x8001EE0C -> 0x800221A8), approved by main.

### Candidate: swept-sphere world walk 0x8001EE0C (`s1_sweep2`)
* **Owned**: 0x8001EE0C only, hand transliteration (895 asm lines) in plain float on locals: prologue (frame 0x470, f29-31, r14-31), the swept box, the BSP walk with
  its explicit stacks (+0x2C0 / +0x3E0), the per-sector triangle-range trees, the per-triangle block and the hit block, the epilogue; every frame store in the
  translated order, float steps through the ppc:: helpers. The inlined leaf 0x8001EE04 runs natively. Callees (plugin accessors 0x80453C54 / 0x80453C9C, the
  callback, 0x800221A8 / 0x80024AC8, the hit chain, 0x800169F8, 0x8040DDC4 / 0x8040DF38, _rwInvSqrt, the bad-cast thrower) are called through the translated
  table with the registers, stack words and LR the translated code sets (`call_guest`); the non-volatiles and f29-f31 are written back first so a yielding
  callee resumes the translated function exact. 0x80021480 (tiny, calls three more guest helpers) stays guest. The hook returns false for the translated body
  (owner address not in native-owners.txt).
* **Hand-back (no write)**: MSR[FP] clear; any hook on the inlined leaf 0x8001EE04.
* **Switches**: `GEKKO_S1_SWEEP2=1/0` (run time), CMake `SHADOW_S1_SWEEP2` (default OFF), `GEKKO_S1_SWEEP2_STATS=1`.
* **Contract** (`s1_core_sweep2_tests`, reference interpreter original vs native, logging stubs for 24 callees, whole arena + stack window + r1 r2 r13 r14-31
  + f29-31 + LR compared): 100,800 calls over a random world BSP (inner / leaf nodes, sectors incl. inverted boxes for the octant-mask path, pair trees, hit lists,
  callback, flag byte) PASS; 125,416 leaves, 202,181 candidate triangles, 90,300 hits; **820 / 820 instructions of the original function executed**. Controls
  caught: one-ulp box word 200, wrong hit slot 138, skipped early-out 200, missing frame word 100, clobbered r20 100. FPU off hands back with no write.
* **Whole run** (watch build 6febc7b + stats fix, scan + shptr + Mtn defaults, sweep2 on): state.json 3a75268e85cd and PCM c64ee7883f980f73 identical;
  behaviour gate vs base-combined-1 and -2: PASS (no bit differs). 267,029 calls, all served natively, 0 handed back.
* Combined with chars + weak wrappers on: same state, PCM and behaviour PASS.
* **Not done**: Final Haunt state comparison (no recipe at hand; needs main's), no timing.
* **Expected ceiling**: the guest-leaf cost of 0x8001EE0C (.75 %) plus its ppc:: helper time (~.2-.3 %): realistic 0.5-0.9 % of the game thread; 0x800221A8 and
  0x80023B28 stay guest callees (next stage if wanted).

### Shared-list lines
CMakeLists.txt (`native_s1_sweep2.cpp` common list, option `SHADOW_S1_SWEEP2` default OFF, test target `s1_core_sweep2_tests`), system.h include, system.cpp
`s1_sweep2::install(cpu_)`; liveness lists regenerated (0x8001EE0C, its leaf, the call links).
