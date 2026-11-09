# axfx-effects: the sound start-up's three unused AXFX effects owned natively (2026-11-07)

Branch `agent/axfx-effects`, cut from `gc-removal` 857399c. Nothing pushed or merged. Every build, test, generator and
game run went through `tools/agents/capped_run.ps1` (cap 10 %; measured CPU shares below). Everything here is headless;
no windowed run, so the `game-window` lock was never needed.

## What was analysed

The sound start-up `0x8043F9A0` (one caller, `0x8043F978`) initializes four AXFX effect objects inside the sound system
object (r13+0x5FAC, +0x10000 + offset); the shutdown `0x8043F674` releases them in the same order.

| object (GCAX+0x10000+) | init / shutdown | name | evidence | confidence |
|---|---|---|---|---|
| -0x1FC (0x808282E4 in the Westopolis run) | 0x80388FF8 / 0x80389064 | AXFXReverbStdInit / Shutdown | init clears byte +0x13C, loads five floats +0x140..+0x150 (coloration 0..1, mix 0..1, time 0.01..10, damping 0..1, pre-delay 0..0.1) and calls `__ReverbStdCreate` 0x803886A4: memset 0x13C bytes, 3 x (2 + 2) comb/all-pass lines sized from a table at 0x80566D80, 3 pre-delay lines of `32000 x pre-delay` samples; free = 0x80388ED4. Its callback wrapper 0x803890B0 (`if byte +0x13C == 0 call 0x80388B20`) is selector mode 1 | roles PROVEN; library name INFERRED from the SDK's published layout |
| -0xA8 | 0x803884E4 / 0x80388554 | ReverbHi | already owned (step 2) | - |
| +0x138 (0x80828618) | 0x80389464 / 0x803896AC | AXFXChorusInit / Shutdown | validates u32 +0x90 base delay 5..15 ms, +0x94 variation <= 5, +0x98 period 500..10000 ms; allocates 0x1680 = 3 x 0x780 bytes (three 480-sample channel buffers = 15 ms at 32 kHz); sets the LFO fields in 0x803896FC; callback 0x8038980C (mode 3) with two LFO helpers 0x803890E0 / 0x80389278 | roles PROVEN; name INFERRED |
| +0x1D4 (0x808286B4) | 0x80389FF4 / 0x8038A03C | AXFXDelayInit / Shutdown | zeroes line pointers +0x30/+0x34/+0x38, calls 0x80389D2C: validates u32 delay[3] +0x3C..+0x44 10..5000 ms, feedback[3] +0x48..+0x50 and output[3] +0x54..+0x5C 0..100 %, converts to blocks (`(ms-5)*32+159)/160`), allocates three lines of blocks x 0x280 bytes; 0x8038A03C frees them; callback 0x80389B5C (mode 4) | roles PROVEN; name INFERRED |

(`docs/agents/sdk-identify*.csv` in the main tree reaches the same names independently; I did not use it as evidence.)
The game stores the parameters itself (live: ReverbStd 0.5 / 0.5 / 3.0 / 0.5 / 0.1; Chorus 15 / 0 / 500; Delay 500 ms x3,
50 % x3, 100 % x3: peeked from the Westopolis run); all are accepted.

### Nothing uses them after init (static)

* The effect selector `0x8043F304` could register any of the four callbacks (modes 1-4 -> 0x803890B0, 0x803885A0,
  0x8038980C, 0x80389B5C) on aux A/B. Its only callers are `0x80440290` (`li r4, 2`, ReverbHi) and `0x8044024C`
  (`li r4, 0`, unregister); the contract test checks both instruction words.
* `AXRegisterAuxACallback / B` (0x80385050 / 0x8038505C) are called only from the selector (`doldis --xref`).
* No data word of main.dol and no `lis/addi` pair outside the selector holds the selector, the callbacks, the
  initializers or the enable/disable functions (`tools/dolxref.py xrefs`, plus a scan of all data sections).
* Dynamic census (Westopolis replay, 90 s from tick 1000, `--watch-guest` over the SDK's aux callback slots r13+0x5CF0
  (A callback), +0x5CF4 (B callback), +0x5CF8 / +0x5CFC (contexts), `.build/ab-jobs/w3.csv`): the aux A callback slot is
  written 17 times, all by `AXRegisterAuxACallback` (`fn_80385050`) plus one clear by AXInit (`fn_80384BC8`); the aux B
  slot is written once, the AXInit clear, and never by `AXRegisterAuxBCallback`. Final state: A = 0x803885A0 (ReverbHi),
  context 0x80828438 (ReverbHi's object), B = 0. The values of the 17 writes are not sampled; with the selector's
  constant modes (2 enable / 0 disable) they are 0x803885A0 and 0. So modes 1, 3 and 4 (0x803890B0 / 0x8038980C /
  0x80389B5C) are not reached in the replay, and statically cannot be.
* All six call sites discard r3: the two ReverbStd and two Chorus sites are followed by `lwz r3, 0x5FAC(r13)`; Delay
  shutdown is followed by `lwz r0` and a later `li r3, 0`; Delay init is the last call of 0x8043F9A0, whose caller
  (0x8043F97C..0x8043F984) loads r4 and overwrites r3 with `li r3, 0`.

## What was replaced

`runtime/src/gekko/native_axfx_effects.cpp` / `runtime/include/gekko/native_axfx_effects.h` (namespace
`gekko::axfx_effects`, installed by `axfx_effects::install(cpu_)` right after `install_reverb_owner()` in
`System::start`). The three inits return the original's verdict on the parameters in the object (the exact compare
chains: ReverbStd ordered `fcmpu` + `blt/bgt`, so NaN passes like in the original; Chorus / Delay unsigned compares) and
write nothing; the three shutdowns return 1. No delay line, descriptor or pre-delay is allocated or built.
Known, intended differences (the same kind as step 2's):

* The original returns 0 when the sound heap cannot serve a request; the owner has no allocation to fail. The contract
  test proves the original's failure path (every allocation-failure point returns 0 without a leak) and states the
  difference. The game's sound heap never fails in the replay.
* A Chorus shutdown on a never-initialized / refused object: the original frees pointer 0 (a store through 0xFFFFFFE0);
  the owner returns 1. The game never shuts down a refused chorus.
* `OSDisableInterrupts / OSRestoreInterrupts` around the original setups restore the entry level; not reproduced.
* ReverbStd with a NaN or tiny pre-delay: the original requests zero-byte blocks (the game's heap serves them) and
  returns 1, and its shutdown then leaks them; the owner returns 1. (ReverbHi's NaN path differed; Std's does not.)

## Gates (docs/AGENT_COORDINATION.md section 4)

| gate | evidence |
|---|---|
| 1 what_if | `what_if.py --name axfx --own` all six, `--release-dir .build/watch --dolrecon-dir ../source/.build/dolrecon` (lock `generator`, 63.6 s CPU, 3.0 %): **14 rows freed, 0 revived, no refusal**: 0x803886A4 `__ReverbStdCreate`, its loop rows 0x803887C8 / 0x80388908 / 0x80388A7C, 0x80388ED4 `__ReverbStdFree` with 0x80388F24 / 0x80388F6C / 0x80388FC0, 0x803894DC, 0x803896DC, 0x803896FC chorus settings, 0x80389D2C delay settings, 0x80389EE4, 0x803B2F50 (powf; its only caller). All `sdk:unidentified`. Pruned rows 751 -> 765. Four stale listed sites (wait-points lines 264-267, the ReverbStd loops) -> removed from `extract/wait-points.txt` |
| 2 contract | `axfx_effects_contract_tests` (new; CTest, label native-contract): original init/shutdown from the pinned DOL (hash-checked) vs the owners on the same object: ReverbStd 71, Chorus 28, Delay 82 parameter sets (every bound, one step outside, 0, mid, +-0, +-inf, NaN, tiny, 12 other values), 33.6 M original instructions. r3 equal in all, the whole object + heap region byte-unchanged by the owners, shutdown r3 = 1, original shutdown frees everything (except the Std zero-block quirk above, counted: 4 sets). **Negative control**: an owner that accepts one value beyond a bound is detected in all three sweeps. Also: every allocation-failure point of each original, the discarded-r3 words and the selector's modes. Passes alone and under ctest |
| 3 guest watch | headless cold-boot Westopolis replay, `--watch-guest` over the three objects and 0x8082C980..0x80872CC0 (all lines) armed at ticks 22,700,000 / 22,882,500 / 40,500,000: **no access except the sound heap's own alloc / free (0x8043BDE4 / 0x8043BD38) reading block headers** and the final state report. Init happens in (22,680,000, 22,690,000]; armed at 22,680,000 the same window shows the originals' init (0x80388FF8, 0x803886A4, 0x80389464, 0x803896FC, 0x80389D2C, 0x80389FF4, 0x8038A03C), the game's parameter stores (0x8043FA14), memset and one bulk stream copy 0x80481D9C / 0x8049A8DC that reads the old contents of the memory before the heap is carved (the same earlier-reuse traffic step 2 found); the watch cannot order events inside that one traversal span, so "no reader after init" rests on the zero accesses from 22.69 M on plus gate 4's equality |
| 4 behaviour | Westopolis 90 s, baseline exe (sha256 59b1f650...) vs candidate (e11764e0...), alternating A1 B1 A2 B2: PCM sha 015445f2... in all four, 3,645,000,000 ticks, 2,697 VI / 5,360 render frames in all four; A/A and B/B memory block hashes identical (0 differing 64 KB blocks); A/B 14 blocks differ (image 88, 90, 96; legacy-os-heap 32-39, 238, 242, 249), identical in both pairs. **All 81,708 differing words classified** (rules below) |
| 5 performance | alternating pairs x4, first discarded, headless 90 s game each, capped-runner job CPU: base 11.4 / 11.5 / 11.3 s, cand 11.5 / 11.3 / 11.3 s (wall 11.7 / 11.7 / 11.7 vs 11.8 / 11.6 / 11.6 s): **no measurable change** (spread overlaps). No FPS claims |

### Classification of the 81,708 differing words (A = baseline, B = candidate)

`delta` = 0x46340, the total size of the retired lines (B's heap shrank by exactly that; all later sound-heap blocks
sit delta lower). Pre-init bytes `P` = the same memory peeked at tick 22,680,000 (`--stop-at-tick`).

| words | class | check |
|---|---|---|
| 77 | the three objects' descriptors (line pointers / sizes / gains / chorus fields), built by the originals | B = 0 in every word (the game zero-fills the object; its own parameters are equal in A and B and not in the diff) |
| 54,843 | the freed lines 0x8082C980..0x80872CC0 | A = 0 (the originals' zero-fill loops), B = P (untouched) |
| 10 | sound-heap block headers of the effect lines (`MB` magic + next pointer) | B = P: B never allocated those blocks |
| 10,076 | later sound-heap allocations moved down into the freed lines (10,003 identical words, 73 with pointers into the shifted region rebased) | B[x] = A[x + delta] (rebased) |
| 16,473 | the tail the moved allocations vacated | B[x - delta] = A[x] (identical or rebased), B[x] = P |
| 214 | pointers to moved blocks (GCAX object fields, image data, free lists) | A = B + delta |
| 13 | 96000.0 (47BB8000) in A vs 0 in B at 0x805876FC, 805AD004, 805AF19C, 805AF2BC, 8060BF74, 814F1A7C/1ABC, 8153307C/30BC, 815ABB3C/BB7C, 815AC7BC/C7FC | 96000.0 = 32000 x 3.0 = `fmuls f28` in `__ReverbStdCreate` (time 3.0): INFERRED stale FPR / FPU-save / dead-stack copies (the same 13 words step 2 saw with 32000.0 vs 96000.0: Hi's create used to overwrite them). Not replicated |
| 1 | 0x8082C964: header next pointer; A = first Std line's header, B = the relocated header of the following block (0x8082E540 = A 0x80874880 - delta) | |
| 1 | 0x80860A20: legacy-OS-heap cell link, written by OSAllocFromHeap / OSFreeToHeap (0x80371F04 / 0x80371FB0) in both runs; in A then zeroed by the delay-line zero fill (`fn_80389D2C` is among its writers in A, absent in B); zero accesses from 22.69 M on in either run | |

## Removed dependencies / measured counts

* 6 owner rows + 14 freed rows = **20 platform-SDK bodies** fewer: shipping (`.build/watch`, `removal_plan.py
  --control-own 0x80370E68`) **1,024** (the ratchet default is 1,044); tests tree **1,030** (default 1,050). The
  ratchet defaults are NOT changed.
* No new runtime coupling to SDK internals (the owners only read the object).

## Tests run (tests tree `.build/tests`, Release, capped)

Passed: axfx_effects_contract_tests, axfx_reverb_contract_tests, audio_output_contract_tests,
sound_table_contract_tests, native_file_api_reference_tests, guest_watch_tests, platform_gc_removal_plan,
platform_retired_readers_osinit / _reverb, platform_rtfs_async_proof, native_jump_tables, native_wait_points_classifier,
native_host_isa_consistency. Failed, all on the snapshot's known-failure list and unchanged in kind: native_file_api_tests
(66 CRI owner rows "listed, not installed", none of mine), ax_pb_tests (segfault), lift_purity, native_liveness_roots
(stale file: 456 unlisted runtime literals, none of mine), platform_runtime_coupling (3 unclassified native_cri.cpp
uses), native_wait_points (stale before and after: the snapshot lacks Hi's six and CRI's four sites too),
native_host_isa_gate (guest_watch TLS initializer, options_audio_mix_tests). Not run: the rest of the suite.

## Open problems

* `extract/liveness-roots.txt` is stale at the snapshot (456 literals); I added only my six lines, not a regeneration.
* Gate 3 cannot order the originals' init against the bulk stream copy inside the 10 k-tick span; gate 4 (retired lines
  and descriptors differ only as classified, PCM / ticks / frames equal) is the evidence for it.
* The 13 FPR words are classified by value and mechanism, not each traced to its spill.
* Name confidence: roles proven, library names inferred. Another session (`gc-axfx-effects`) works the same task in
  `wt-gc-removal` with the Std / Chorus / Delay labels permuted; the evidence above says its labels are wrong.

## Shared list lines changed

* `CMakeLists.txt`: `runtime/src/gekko/native_axfx_effects.cpp` after `native_reverb_owner.cpp` (source list); the
  `axfx_effects_contract_tests` target + test after `axfx_reverb_contract_tests`.
* `extract/native-owners.txt`: appended a commented block with the six rows.
* `extract/wait-points.txt`: removed `0x80388828`, `0x80388884`, `0x80388964`, `0x803889C0` (4 lines).
* `extract/liveness-roots.txt`: inserted `0x80388FF8, 0x80389064, 0x80389464, 0x803896AC, 0x80389FF4, 0x8038A03C`.
* `runtime/src/gekko/system.cpp`: include + one `axfx_effects::install(cpu_);` call after `install_reverb_owner();`.
* `runtime/tests/native_file_api_tests.cpp`: include + `add_owner_addresses<axfx_effects::Owners>`.
* `SHADOW_GC_REMOVAL_RATCHET` untouched.

---

# Step B: os.memory + os.time (2026-11-07, on gc-removal 99fe8c3)

Assigned by the main session: the small OS services left in the removal plan (os.memory, os.time, os.alarms,
os.reset, os.debug). This step owns the five frontier rows of os.memory and os.time; os.alarms and os.reset have no
frontier row (below); os.debug is Step C.

## What was analysed

`tools/platform/removal_plan.py` regenerated at 99fe8c3 (`.build/platform/removal_plan/REMOVAL_PLAN.md`): live rows
38,462, platform-SDK bodies compiled 963 (shipping), frontier rows 261.

| row | name | what it is | entered from outside the SDK by |
|---|---|---|---|
| `0x80372214` | OSGetArenaHi | `lwz r3, 0x5A70(r13)` | game `0x80050C98`; `0x803A3994`, `0x803A3A4C` (C runtime start-up) |
| `0x8037221C` | OSGetArenaLo | `lwz r3, 0x26E8(r13)` | same three |
| `0x8037222C` | OSSetArenaLo | `stw r3, 0x26E8(r13)` | same three |
| `0x80379628` | OSGetTime | `mftbu / mftb / mftbu / cmpw / bne` retry loop, r3:r4 = time base | game `0x8043F4E4`, `0x80051414`, `0x80051460` |
| `0x80379640` | OSGetTick | `mftb r3` | game `0x8040BC18` |

All five are leaf routines with no frame. Names: the manifest's (`OSGetArenaHi` ... `OSGetTick`); the bodies are
loads/stores of small-data cells and the two time-base reads, which is what those SDK functions are (PROVEN by
behaviour; the whole instruction streams were read with `tools/doldis.py`).

Rows with no frontier row, so nothing to own from outside, and why they still compile:

* os.memory interior: `OSSetArenaHi` `0x80372224` and `__OSInitMemoryProtection` `0x80376B60` are called only by OSInit
  (`0x80370E68`, os.core: native-boot's).
* os.time interior: `__OSGetSystemTime` `0x80379648` is called by OSInit, `InsertAlarm` `0x80371810` (os.other) and the
  decrementer callback.
* os.alarms: `DecrementerExceptionCallback` `0x80371BE4` (640 bytes, live) is named by `OSInitAlarm` `0x803717A8`
  (os.core) and by `0x80371E5C`; `OSSetAlarm` and `fn_80371AC8` are already pruned.
* os.reset: `OSRegisterResetFunction` `0x80376D6C` is called by `OSInitAlarm`, `__OSInitMemoryProtection` and `GXInit`
  `0x80393A88` (graphics.gx); `OSResetSystem`, `OSGetResetCode` and `__OSResetSWInterruptHandler` are already owners and
  `__OSReboot*` is pruned.

So the remaining five bodies of these services (OSSetArenaHi, __OSInitMemoryProtection, __OSGetSystemTime,
DecrementerExceptionCallback, OSRegisterResetFunction) leave with OSInit / OSInitAlarm (native-boot, os.core) and
GXInit. No owner can remove them from outside. (Source: the live graph's edges, `.build/platform/removal_plan/live_graph.txt`.)

## What was replaced

`runtime/include/gekko/native_os_arena_clock.h` / `runtime/src/gekko/native_os_arena_clock.cpp`
(`gekko::os_arena_clock::install`, called after the console-settings owners in `System::start`):

* OSGetArenaHi / OSGetArenaLo / OSSetArenaLo read and write the same small-data cells (`r13 + 0x5A70`, `r13 + 0x26E8`)
  with `mem_read32` / `mem_write32`. The cells stay guest memory because OSInit (still compiled) writes them and the
  game's own allocators read them.
* OSGetTime returns `Cpu::time_base()` (the native game clock, 40.5 MHz, `game_clock.h`) in r3:r4; OSGetTick its low
  word. The game clock moves only at wait points and guest code runs in zero game time, so the original's retry loop
  never retries and no hardware timebase is emulated.

Differences from the originals, not observable under the calling convention: OSGetTime leaves r5 and cr0 field 0
untouched where the original sets them to the upper word and "equal" (both volatile; no caller of the five reads them).

## Gates

| gate | evidence |
|---|---|
| 1 what_if (`--own` the five rows, on the pre-change lists, lock `generator`) | `os_arena_clock: frees 0 rows, revives 0, 1 listed sites become work items` (the stale wait point `0x80379638`, removed). The compiled-body count falls by the five owned rows (below). |
| 2 contract test `os_arena_clock_contract_tests` | the pinned DOL's five bodies against the owners on identical machines; r0..r31, cr, xer, ctr, lr, all 32 FPRs, 1.2 MiB of small-data memory and 64 KiB of scratch compared after every call: 144 cases (4 values of r13 times 10 cell values for each of the three arena entries, 12 time bases for OSGetTime and OSGetTick), 350 original instructions; the only differences are the documented r5 / cr0 of OSGetTime (12 each, asserted present). 5 negative controls (hi reads the lo cell, lo stores into the hi cell, a fixed r13, tick off by one, tick reads the high word) are each detected. Also checks the five owners are installed at their addresses. |
| 3 no surviving reader of retired state | nothing is retired: the owners read and write the very cells and the time base the originals did. Dynamic census (guest watch, `--watch-guest-from-tick 0`, full 90 s replay, cells `0x805F1F70` hi and `0x805EEBE8` lo): baseline accesses lo 5 reads / 4 writes, hi 4 / 2 (one read each is the state report); candidate lo 5 / 4, hi 4 / 2, the same counts, now attributed to the callers `0x80050C98` and `OSInit` (an owner call runs in its caller's row). In the replay only `0x80050C98`, `OSInit` and `OSSetArenaHi` touch the cells; `0x803A3994` / `0x803A3A4C` do not run. |
| 4 game behaviour, alternating A/B x2, 90 s, headless | base `28726df1...` (gc-removal 99fe8c3 build), candidate `cbcf9015...`: PCM `015445f2` in all four runs, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames; `state.txt` (64 KiB memory block hashes of every region) is byte-identical across all four runs: 0 differing guest blocks A/B, A/A and B/B. No difference to classify. |
| 5 performance (4 alternating runs each, first discarded, job CPU) | base 14.0 / 14.5 / 15.2 s, candidate 14.8 / 15.0 / 15.0 s: the spreads overlap, no measurable change. |

## Removed dependencies / measured counts

Platform-SDK bodies compiled (`removal_plan.py --control-own 0x80370E68`, the CMake ratchet command):

* shipping (`.build/watch`, lifted renderer): 963 -> **958** (-5); the control (OSInit owned) prediction equals the
  generator (38 rows).
* tests tree (`.build/tests`): 969 -> **964** (-5); `platform_gc_removal_plan` passes.

os.memory: 5 -> 2 bodies; os.time: 3 -> 1. The ratchet default in `CMakeLists.txt` is NOT changed (the main session lowers it).

## Tests run (tests tree, Release, capped, after this step)

Passed: axfx_effects_contract_tests, axfx_reverb_contract_tests, os_arena_clock_contract_tests (new),
os_settings_contract_tests, save_store_contract_tests, audio_output_contract_tests, sound_table_contract_tests,
native_file_api_reference_tests, guest_watch_tests, platform_gc_removal_plan, platform_runtime_coupling,
platform_retired_readers_osinit / _reverb, platform_rtfs_async_proof, native_jump_tables,
native_wait_points_classifier, native_host_isa_consistency.

Failed, all on the baseline list and unchanged in kind: native_file_api_tests (the same 66 CRI rows "listed, not
installed", none of the five), native_liveness_roots (stale before; the five lines are in the file),
native_wait_points (stale before and after). The rest of the suite was not run.

## Shared list lines changed (Step B)

* `CMakeLists.txt`: `runtime/src/gekko/native_os_arena_clock.cpp` after `native_axfx_effects.cpp`; the
  `os_arena_clock_contract_tests` target + test after `axfx_effects_contract_tests`.
* `extract/native-owners.txt`: appended a commented block, five rows (`0x80372214 0x8037221C 0x8037222C 0x80379628 0x80379640`).
* `extract/wait-points.txt`: removed `0x80379638 0x80379628 clock` (1 line).
* `extract/liveness-roots.txt`: inserted the same five addresses in sorted position.
* `runtime/src/gekko/system.cpp`: include + `os_arena_clock::install(cpu_);` after the `os_settings::install` line.
* `runtime/tests/native_file_api_tests.cpp`: include + `add_owner_addresses<os_arena_clock::Owners>`.
* `SHADOW_GC_REMOVAL_RATCHET` untouched.


---

# Step C: os.debug, InitializeUART (2026-11-07, on Step B)

The removal plan's os.debug service (after the sdk-identify names) has two frontier rows: `OSReport` `0x803731CC` and
`InitializeUART` `0x803B57D8` (sdk-identify PROVEN, `test_data/platform/unidentified_names.csv`; in the shared manifest
it is still `sdk:unidentified`). This step owns InitializeUART. OSReport is not owned: it is a design question (below).

## What was analysed

`InitializeUART` (112 bytes, one caller: the console write routine `0x803AF5A8`, MSL's `__write_console` as the game
links it, which calls it while the first-use flag `r13 + 0x5EA0` is clear and sets the flag when it returns 0):

```
if (*(u32*)(r13 + 0x5EE4) == 0xA5FF005A) return 0;                       // already brought up, nothing written
type = BI2 pointer (r13 + 0x5A20) != 0 ? BI2->consoleType (+0x2C) : 0;  // OSGetConsoleType 0x80370E04
if (type == 0) type = 0x10000002;
if (!(type & 0x10000000)) { *(r13 + 0x5EE0) = 0; return 2; }             // retail console: no UART
*(r13 + 0x5EE0) = 0xA5FF005A; *(r13 + 0x5ED8) = 0; *(r13 + 0x5EDC) = 1; return 0;
```

The runtime's default console type is `0x10000006` (development hardware, `system.h`), so in the replay it takes the last
branch once. `r13 + 0x5EE0` is the enable word the native IPL module's WriteUARTN owner checks (`native_ipl.h`). That
header still says InitializeUART "stays game code"; it now does not.

## What was replaced

`runtime/include/gekko/native_os_debug.h` / `runtime/src/gekko/native_os_debug.cpp` (`gekko::os_debug::install`): the
same reads and writes of the same cells, by `mem_read32` / `mem_write32`. Differences from the original, not
observable: it saves its link register into the caller's frame word at `4(r1)` and pushes an 8-byte frame (back chain at
`-8(r1)`), r0 and cr0 are scratch. The owner writes none of them.

## Gates

| gate | evidence |
|---|---|
| 1 what_if (`--own 0x803B57D8`, lock `generator`) | `os_debug: frees 0 rows, revives 0, 0 listed sites become work items` |
| 2 contract test `os_debug_contract_tests` | the pinned DOL's body against the owner on identical machines; all of r0..r31 (r0 excepted: scratch of the original, asserted to be the reloaded link register), cr (cr0 excepted), xer, ctr, lr, FPRs, 192 KiB of small-data memory, 64 KiB of scratch and a stack window compared after every call; 234 inputs (6 values of the "brought up" word, 3 previous contents of the cells, no BI2 block or a block with each of 12 console types incl. 0, bit 0x10000000 alone, all-ones), 8,093 original instructions; result 0 in 144 inputs, 2 in 90; the original's two frame words are asserted to be exactly the back chain and the link register in every case and the owner to write neither. 5 negative controls (retail returns 0, wrong console-type bit, `0x5ED8` not cleared, a write on the already-up path, type 0 not defaulted) are each detected. Also checks the owner is installed at `0x803B57D8`. |
| 3 no surviving reader of retired state | nothing is retired: the cells are the ones the originals wrote. Dynamic census (guest watch from tick 0, full 90 s replay; cells `0x805F23A0` first-use flag, `0x805F23D8..0x805F23E7`): baseline flag reads 1+28, writes 2; UART cells reads 2+1+28, writes 7; candidate the same totals, flag reads 1+28 / writes 2, cells reads 2+29 / writes 4+3, the three owner writes attributed to the console write routine `0x803AF5A8` instead of native code. The 28 reads of `0x5EE0` are the WriteUARTN owner's enable check. |
| 4 game behaviour, alternating A/B x2, 90 s, headless | base `cbcf9015...` (Step B build), candidate `f1621eb1...`: PCM `015445f2` in all four runs, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames, `state.txt` (64 KiB memory block hashes) byte-identical across all four runs: 0 differing guest blocks. (The original's two dead frame words are overwritten by later calls before the report; no block differs.) |
| 5 performance (4 alternating runs each, first discarded, job CPU) | base 14.0 / 13.4 / 12.5 s, candidate 13.3 / 12.6 / 12.8 s: overlapping, no measurable change. |

## Removed dependencies / measured counts

Platform-SDK bodies compiled (`removal_plan.py --control-own 0x80370E68`): shipping 958 -> **957**, tests tree 964 -> **963**
(`platform_gc_removal_plan` passes, control prediction = generator, 38 rows). With Step B: shipping 963 -> 957, tests 969 -> 963.
`InitializeUART`'s only SDK callee `OSGetConsoleType` is still reached by other SDK code.

## Tests run (tests tree, Release, capped, after this step)

Passed: os_debug_contract_tests (new), os_arena_clock_contract_tests, axfx_effects_contract_tests,
axfx_reverb_contract_tests, os_settings_contract_tests, save_store_contract_tests, audio_output_contract_tests,
sound_table_contract_tests, native_file_api_reference_tests, guest_watch_tests, platform_gc_removal_plan,
platform_runtime_coupling, platform_retired_readers_osinit / _reverb, platform_rtfs_async_proof, native_jump_tables,
native_wait_points_classifier, native_host_isa_consistency. Failed, baseline and unchanged in kind:
native_file_api_tests (the same 66 CRI rows "listed, not installed"), native_liveness_roots (stale before),
native_wait_points (stale before and after). The rest of the suite was not run.

## Shared list lines changed (Step C)

* `CMakeLists.txt`: `runtime/src/gekko/native_os_debug.cpp` after `native_os_arena_clock.cpp`; the
  `os_debug_contract_tests` target + test after `os_arena_clock_contract_tests`.
* `extract/native-owners.txt`: appended a commented block with `0x803B57D8 InitializeUART`.
* `extract/liveness-roots.txt`: inserted `0x803B57D8`.
* `runtime/src/gekko/system.cpp`: include + `os_debug::install(cpu_);` after `os_arena_clock::install(cpu_);`.
* `runtime/tests/native_file_api_tests.cpp`: include + `add_owner_addresses<os_debug::Owners>`.
* `SHADOW_GC_REMOVAL_RATCHET` untouched.

## OSReport (`0x803731CC`): not owned, and why (a decision for the main session)

OSReport is a 128-byte varargs wrapper: it spills r3..r10 and f1..f8 into a register-save area, builds a `va_list`
(`{gpr = 1, fpr = 0, overflow, save area}`) and calls MSL's `vprintf` (`0x803AA414`), which locks the critical region
(`0x803A906C`), runs `__pformatter` (`0x803AA738`) against the stdout `FILE` (`0x80568F20`), whose writer ends in
`__write_console` `0x803AF5A8` -> `InitializeUART` + the WriteUARTN owner (the line is counted, `\n` rewritten to `\r`
in the formatted buffer) and, on development hardware, the debugger write `0x803A05A0`.

Facts measured on the 90 s replay (guest watch of the stdout FILE and the UART cells, baseline build):

* 28 calls of the console write routine in the 90 s replay, so 28 OSReport / OSPanic lines. Guest watch of the three
  stdio FILE objects (`0x80568ED0`, 0xA0 bytes, candidate build, rows named): every access in the replay comes from five
  MSL guest rows, `0x803A8C7C` (392 reads / 168 writes), `0x803A8D40` (6 / 4), `0x803A9098` (1,046 / 269), `0x803AF520`
  fwide (192 / 1) and `0x803AF5A8` the console write (28 reads), plus the state report's own read; no game row.
* `0x803AA414` (vprintf) has exactly two direct callers: OSReport `0x803731CC` and `0x8037324C` (the formatter of
  OSPanic / the exception dump). So in this replay the stdout FILE, the UART cells and the formatted buffers are private
  state of the SDK's debug console: nothing outside it reads them. Not proved statically (the compiler shares one
  `lis 0x8057` page between many data objects, so a pair scan for the FILE address is noise); the evidence is the
  dynamic census above. The only host-visible product is the line statistic in the session report
  (`ipl.uart_lines`, printed at `system.cpp:5450` and `shadow_main.cpp:7442`, asserted once in `gekko_core_tests.cpp:4489`
  for the WriteUARTN owner).
* The call sites: 6 in game code (`0x80050C1C, 0x80050C6C, 0x80050E64, 0x80050EC4, 0x80050F24, 0x80050F80`, start-up
  diagnostics), the rest inside the SDK (OSInit's banner, exception dumps, `OSRegisterVersion`, `VIConfigure`, ...).

Why not owned here: (1) The runtime has no native-to-guest call (no `call_guest`; only `native_interrupts` runs guest handlers), so an owner cannot hand the formatting to the guest
`vprintf` as the original does; (2) a native formatter for the guest `va_list` is a printf implementation (floats, `%s`
from guest memory) whose only observable product is a statistic; (3) an owner that simply discards the text is
game-equivalent (nothing reads the FILE or the cells), but it changes the A/B memory report (the FILE words, the
vprintf frames) so it needs gate 4's word classification, and it removes the `uart_lines` statistic's source.
The open choice for the main session: discard (and let WriteUARTN's statistic go with it), or count lines without
formatting, or keep OSReport until native-boot. The cost of keeping it is one SDK body plus the MSL formatter.


---

# Step D: OSReport owned with a native formatter (2026-11-07, on gc-removal 976d0d6)

Assigned by the main session: own `OSReport` `0x803731CC` with a native formatter that reads the format string and the
arguments from the guest per the PowerPC EABI, formats with a host printf equivalent, writes the line to a host debug
console (counted and stored by default, stderr behind a switch) and touches no guest memory; then retire the stdout
FILE, the UART cells and the formatted buffers it used to fill.

## What was replaced

`runtime/include/gekko/native_os_report.h` / `runtime/src/gekko/native_os_report.cpp` (`gekko::os_report`):

* `ArgCursor`: EABI argument access over the live registers: words from r4 on (r3 is the format), 64-bit values in an
  even register pair, doubles from f1..f8, then the overflow area at the caller's `r1 + 8` (8-aligned for 64-bit
  values). The original spills r3..r10 and, when cr1.eq says floats were passed, f1..f8, into its frame and hands MSL a
  `va_list` over them; reading the live registers is the same data.
* `format()`: flags `- + space # 0`, width and precision (also `*`), lengths `hh h l ll j z t`, conversions
  `d i u o x X c s p f F e E g G %`. Anything else (`%n`, wide strings, an unknown letter) stops the run with a message
  instead of guessing. Integers are formatted by hand; floating-point conversions use the host `snprintf` with MSL's
  measured deviations applied (below).
* `DebugConsole` (a `System` member, `System::debug_console()`): per call it counts lines and bytes, keeps the last 256
  lines, and echoes them to stderr when `SHADOW_DEBUG_CONSOLE=1`. Like the original it prints nothing on a retail
  console (console type without bit `0x10000000`, via `os_debug::console_type`, which reads the same cells as
  OSGetConsoleType): such calls are only counted as suppressed. The session report prints
  `OSReport console: N lines (B bytes in C calls, S calls suppressed on a retail console)`.
* The owner writes no guest memory: it only reads the registers, the format string, the `%s` strings and the overflow
  words.

### MSL behaviour found by the contract test (not what the host printf does)

* a null `%s` prints nothing (no `(null)`);
* `%p` prints `0x` and at least 8 hex digits, also for null;
* `%f` keeps 32 significant digits and writes zeros after them, rounding the 32nd digit by the 33rd (1e300, 1.7e40);
* `%.0f` of exactly 0.5 prints `1` (the host rounds the tie to even); other ties (2.5, 0.125 at `%.2f`, ...) agree;
* infinity and NaN print `inf` / `nan` (`INF` / `NAN` for `F E G`), take no `+` or space sign (a negative infinity
  keeps `-`), and the `0` flag pads them with zeros.

## Gates

| gate | evidence |
|---|---|
| 1 what_if (`--own 0x803731CC`, lock `generator`) | `os_report: frees 0 rows, revives 0, 0 listed sites become work items`: only the owner row leaves the compiled bodies; MSL's `vprintf` and the whole console path stay reachable (below) |
| 2 contract test `os_report_contract_tests` | the pinned DOL's OSReport (vprintf, `__pformatter`, the stdout FILE) runs in the reference interpreter with the console write routine `0x803AF5A8` replaced by a capture of the bytes it is asked to write; the owner's `format()` reads the same registers and guest memory. Census: every printf-style string of the DOL's data sections (333; 17 are not formats: binary words that contain `%` and one scanf pattern) plus 16 float formats with flags, 32 argument sets per format chosen per conversion (32-bit and 64-bit integer edge values, strings of 0 / 3 / 300 / 5,000 bytes, a null pointer, doubles incl. +-0, ties, 1e300, +-inf, nan), placed in registers and the overflow area by an independent model of the EABI caller; a format without a newline gets one (the original line-buffers stdout). **10,752 cases, 93.6 M original instructions: the text is equal in every case**, and the formatter writes no guest byte (a 1.5 MiB window is compared before and after). 6 negative controls (a perturbed r4, f1, string byte, r6 of a 64-bit pair, overflow word and format byte) are each detected. |
| 3 no surviving reader of retired state | **Not met as phrased; the cause is found.** Guest watch (full 90 s replay, from tick 0) of the three stdio FILE objects (`0x80568ED0`, 0xA0 bytes), the stdout buffer head (`0x805A6438`), the UART first-use flag (`0x805F23A0`) and cells (`0x805F23D8`, 16 bytes): baseline FILE reads 1,684 / writes 442; candidate reads 946 / writes 254. OSReport's share is gone, a large part remains. The remaining accessors are MSL stdio rows used by the game's own `fprintf` (`0x803AA4AC`, **504 call sites**, e.g. the caller at `0x8000A2B4` passes `stderr`, `0x80568ED0 + 0xA0`) and the `0x803AA58C` family (142 sites): they share the FILE and the UART write path. In the replay 18 of the 28 console writes (WriteUARTN calls) are theirs; OSReport produced 12 lines / 395 bytes. The FILE and UART state cannot retire until `fprintf` / `vfprintf` for the console FILEs are native too (their format reading is this formatter's; the return value is the character count). |
| 4 game behaviour, alternating A/B x2, 90 s, headless | baseline = the merged tree built without the owner (`33950e39...`), candidate `fd5f74a9...`: PCM `015445f2` in all four runs, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames. `state.txt`: 2 of the 64 KiB blocks differ A/B (A/A and B/B equal), **4 guest words**, all in the stdio FILE and its buffer: `0x80568F38` and `0x80568F54` (stdout FILE byte counters) 906 -> 511, a difference of 395 = exactly the bytes the native console counted for OSReport; `0x805A6474` / `0x805A6478` (the stdout buffer's tail, "301)" of the last OSReport line) -> 0. Classified retired state of the console path; nothing in the game reads them (only MSL's stdio rows, whose output goes to the sink). |
| 5 performance (4 alternating runs each, first discarded, job CPU, other agents were running) | base 19.5 / 21.7 / 22.2 s, candidate 20.4 / 21.5 / 21.3 s: overlapping, no measurable change. |

Note on order: the first A/B used a baseline built before the gc-removal merge (the VI work changes ~36 words of its
own); that comparison was discarded and the baseline was rebuilt from the merged tree.

The A/B candidate was built before the last three formatter edits (inf / nan spelling and zero padding, the 0.5 tie): they
only change branches the replay's 12 lines do not reach; the next gated build (Step E) carries them.

## Removed dependencies / measured counts

Platform-SDK bodies compiled (`removal_plan.py --control-own 0x80370E68` at the merged tree): shipping **905**, tests
tree **928** (the OSReport body is now an owner row, so -1 against the same tree without the owner; the absolute counts
include everything merged from gc-removal and cannot be compared with Step C's 957 / 963). The control prediction equals
the generator (42 rows). `platform_gc_removal_plan` passes.

## Tests run (tests tree, Release, capped)

Passed: os_report_contract_tests (new), os_debug_contract_tests, os_arena_clock_contract_tests,
axfx_effects_contract_tests, axfx_reverb_contract_tests, os_settings_contract_tests, save_store_contract_tests,
audio_output_contract_tests, sound_table_contract_tests, native_file_api_reference_tests, guest_watch_tests,
platform_gc_removal_plan, platform_runtime_coupling, platform_retired_readers_osinit / _reverb, platform_rtfs_async_proof,
native_jump_tables, native_wait_points_classifier, native_host_isa_consistency. Failed, baseline and unchanged in kind:
native_file_api_tests (the same 66 CRI rows "listed, not installed", none mine), native_liveness_roots (stale before),
native_wait_points (stale before and after). The rest of the suite was not run.

## Shared list lines changed (Step D)

* `CMakeLists.txt`: `runtime/src/gekko/native_os_report.cpp` after `native_os_debug.cpp`; the `os_report_contract_tests`
  target + test after `os_debug_contract_tests`.
* `extract/native-owners.txt` (CRLF): appended a commented block with `0x803731CC OSReport`.
* `extract/liveness-roots.txt` (CRLF): inserted `0x803731CC`.
* `runtime/include/gekko/system.h`: include, `debug_console()` accessor and member; `runtime/src/gekko/system.cpp`:
  include, `os_report::install(cpu_, debug_console_)`, one report line; `runtime/src/shadow_main.cpp`: one report line;
  `runtime/tests/native_file_api_tests.cpp`: include + `add_owner_addresses<os_report::Owners>`.
* `runtime/include/gekko/native_os_debug.h` / `.cpp` (mine): `console_type()` exposed.
* `SHADOW_GC_REMOVAL_RATCHET` untouched.

## Open problems / next

* Gate 3 as phrased needs `fprintf` (`0x803AA4AC`) and the `0x803AA58C` variant for the console FILEs owned with this
  formatter (return value = characters written, -1 when the UART is disabled), after which `vprintf`, `fwide`, the
  line flush, `__write_console`, `InitializeUART` and `WriteUARTN` lose their callers (OSPanic `0x8037324C` also calls
  `vprintf`: os-other's).
* Not run: a retail-console-type replay (the owner suppresses the text there, as the original's UART path does).


---

# Step E: MSL printf and fprintf on the console files (2026-11-07, on Step D)

Step D left gate 3 open: the stdout/stderr FILE objects, the UART cells and the stdout buffer were still read and
written, by the game's own `fprintf` (504 call sites) and `printf` (142). This step owns those two MSL entries for the
console files with the same formatter, so that the console path has no user left on the game side.

## What was replaced

`runtime/src/gekko/native_os_report.cpp` (same module, `os_report::install`): `0x803AA4AC` `fprintf(FILE*, format, ...)`
and `0x803AA58C` `printf(format, ...)` (identified by their bodies: both call `fwide` `0x803AF520`, take the critical
region and run `__pformatter` against the stdout FILE, `0x803AA6E0` the FILE writer). Both read r3 (the FILE for
fprintf), the format and the arguments exactly as OSReport does (the argument index starts after the FILE for fprintf)
and return the character count. `fprintf` to a FILE other than stdout (`0x80568F20`) or stderr (`0x80568F70`) stops
the run (the game's file I/O does not use MSL FILEs: no `fopen` caller reaches them in the census).

On a console without a UART (type without bit `0x10000000`) the model of MSL's error latch is: the first print to a
FILE fails and latches the error; later prints to that FILE return -1 and write nothing. The failing call itself
returns the length when the failure comes at the end (stdout is line buffered with a 256-byte buffer: a text that
fits and has its only newline last is flushed after the formatter finished) and -1 when the formatter meets it (a
longer text, a newline before the end, or for the unbuffered stderr a second piece of text). All of this was measured
against the original (below) and is the retail configuration only; the replay and the default console type are
development hardware.

## Gates

| gate | evidence |
|---|---|
| 1 what_if (`--own 0x803AA4AC,0x803AA58C` on the lists without them, lock `generator`) | `os_printf: frees 0 rows, revives 0, 0 listed sites become work items`. The console path is not freed: `vprintf` is still called by OSPanic (`0x8037324C`), `__pformatter` by `sprintf`, and the FILE objects name the write routine `0x803AF5A8` in a data word (`0x80568F60`, `0x80568FB0`), which keeps `__write_console`, `InitializeUART` and `WriteUARTN` rooted. Both owned rows are MSL, not SDK, so the SDK body count does not move. |
| 2 contract test `os_report_contract_tests` (extended) | OSReport: 10,784 formatter cases against the original vprintf (above). New: 7,524 entry-point cases: `printf`, `fprintf(stdout)`, `fprintf(stderr)` x 118 formats x 11 argument sets (with strings of 254..258 bytes around stdout's 256-byte buffer) x console write succeeding / failing, comparing the owner's whole behaviour with the original's: the console text, the return value r3, a second print after a failure (-1, nothing written), and no guest byte written by the owner. **All equal** (0 differences; 144 M original instructions in the file). 6 negative controls detected. |
| 3 no surviving reader of retired state | **Met.** Guest watch of the three stdio FILE objects (`0x80568ED0`, 0xA0), the stdout and stderr buffers (`0x805A6438`, `0x805A6338`, 0x100 each), the UART first-use flag (`0x805F23A0`), the UART cells (`0x805F23D8`, 16 bytes) and MSL's pending-flush pointer (`0x805F2388`) over the full 90 s replay, symbols on, candidate build: the only accesses are the C start-up's `.bss` clear (row `0x8000543C`, writes) and the state report's own reads. No guest row and no SDK / MSL row reads or writes any of them; baseline: 1,684 FILE reads / 442 writes by MSL rows `0x803A8C7C`, `0x803A8D40`, `0x803A9098`, `0x803AF520`, `0x803AF5A8`. WriteUARTN is never called (0 debug-UART lines, was 28). |
| 4 game behaviour, alternating A/B x2, 90 s, headless | baseline = Step D's build (`fd5f74a9...`), candidate (Step E): PCM `015445f2` in all four runs, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames. The console counted **30 lines, 906 bytes** in the candidate; the stdout FILE byte counter of the tree without any of these owners ended at 906 (Step D's A/B, `0x80568F38`), so every byte the game printed is accounted for, and the 18 UART writes of the baseline (`WriteUARTN` x18) are gone. `state.txt`: 6 of the 64 KiB blocks differ A/B (A/A and B/B equal): 161 guest words, classified below. |
| 5 performance (4 + 4 alternating runs, the second set in reversed order, first of each discarded, job CPU, other agents were building) | set 1 base 16.1 / 15.6 / 16.3 s, candidate 16.6 / 17.0 / 17.0 s; set 2 (reversed) candidate 18.6 / 18.3 / 18.2 s, base 17.5 / 18.6 / 16.8 s. The first set showed the candidate ~0.7 s slower, the reversed set does not: an ordering drift from other load; spreads overlap overall: no measurable change. |

### Classification of the 161 differing words (A = baseline, B = candidate)

| words | where | A -> B | why |
|---|---|---|---|
| 4 | stdio FILE objects `0x80568F24`, `0x80568F28`, `0x80568F38`, `0x80568F54` | `0x12900000 -> 0x12800000`, `0x20000000 -> 0`, `0x1FF -> 0` | the stdout FILE's state flags, orientation and byte counters: retired (nothing reads the FILE any more) |
| 15 | stdout buffer `0x805A6438..0x805A6470` | the last printed text (Shift-JIS bytes) -> 0 | the formatted tail left in the FILE buffer: retired |
| 4 | `0x805F2388` (MSL's pending-flush pointer, `0x803A54EC` -> 0) and the UART cells `0x805F23A0`, `0x805F23DC`, `0x805F23E0` | UART first-use flag / bring-up cells set by `__write_console` and `InitializeUART` -> never set | retired: the UART path is not entered |
| 70 | heap / stack frames, `0x809E4660..0x809E4B04`, `0x80EDF838..` (stride 0x2C) | `0x02000000 -> 0` (or the older value) | `0x02000000` is the va_list header `lis r28, 0x200 / stw r28, 0x74(r1)` that the original `fprintf` writes into its own frame (gpr index 2): the dead callee frame of the removed call; the candidate leaves the older content |
| 64 | the same frames, pointer words (`0x8060C3xx`, `0x805F08E4`) | pointer -> older pointer / small integer | the original `fprintf` / `printf` spill r3..r10 (`stw r5..r10, 0x10..0x24(r1)`) into the same dead frame: spilled argument registers |
| 4 | `0x80EDA0B4`, `0x80EDA0E0`, `0x80EDA10C`, `0x80EDA138` | `0 -> 0x59` | the same dead frame (a spilled zero argument) over an older word |

The stack and heap rows are classified by mechanism and by value pattern (the frames the removed originals wrote), not
each word traced to its store; no replica of stack garbage was made to match (docs/AGENT_COORDINATION.md gate 4).

## Removed dependencies / measured counts

Platform-SDK bodies compiled (`removal_plan.py --control-own 0x80370E68`): shipping **905**, unchanged from Step D
(`printf` / `fprintf` are MSL, not SDK); control prediction equals the generator. The ratchet default is not changed.

## Tests run (tests tree, Release, capped)

Passed: os_report_contract_tests (extended; 80 s), os_debug_contract_tests, os_arena_clock_contract_tests,
axfx_effects_contract_tests, axfx_reverb_contract_tests, os_settings_contract_tests, save_store_contract_tests,
audio_output_contract_tests, sound_table_contract_tests, native_file_api_reference_tests, guest_watch_tests,
platform_gc_removal_plan (tests tree 928, unchanged), platform_runtime_coupling, platform_retired_readers_osinit /
_reverb, platform_rtfs_async_proof, native_jump_tables, native_wait_points_classifier, native_host_isa_consistency.
Failed, baseline and unchanged in kind: native_file_api_tests (the same 66 CRI rows "listed, not installed", none
mine), native_liveness_roots (stale before), native_wait_points (stale before and after). The rest of the suite was
not run.

## Shared list lines changed (Step E)

* `extract/native-owners.txt` (CRLF): appended a commented block with `0x803AA4AC fprintf`, `0x803AA58C printf`.
* `extract/liveness-roots.txt` (CRLF): inserted `0x803AA4AC`, `0x803AA58C`.
* `runtime/tests/os_report_contract_tests.cpp`, `runtime/include/gekko/native_os_report.h`, `.cpp` (mine).
* `SHADOW_GC_REMOVAL_RATCHET` untouched.

## Open problems

* The stdout/stderr FILE write-proc data words (`0x80568F60`, `0x80568FB0`) still root `__write_console`,
  `InitializeUART` and `WriteUARTN`: nothing calls them any more, but they are not pruned until the FILE objects'
  consumers (`vprintf` for OSPanic `0x8037324C`, the MSL flush routines) are owned or retired. OSPanic is os-other's.
* The retail-console behaviour (error latch, return values) is covered by the contract test against the original but
  was never run in the game.

---

# Step G: debug.trk, MetroTRK's remaining entries (2026-11-08, on gc-removal cd88ab4)

Assigned by the main session (moved from native-boot): make nothing enter the debugger library. The PC build has no
debugger consumer; the goal is that no compiled body of MetroTRK is reachable.

## What was analysed

The plan regenerated on the merged tree (`removal_plan.py`, shipping `.build/watch`): debug.trk had **11 compiled bodies,
4 frontier rows**. The native boot (docs/agents/native-boot.md) had already removed `__start`'s calls: `InitMetroTRK
0x8039F8E0` / `InitMetroTRK_BBA 0x8039F978` are stop owners (native_ipl.h; they appear as owner rows, not bodies), and the
boot information names no monitor (BI2 kind 0). What remained:

| frontier row | entered by | what it is |
|---|---|---|
| `0x80370BF0` DBInit | runtime literals in `native_pc_boot_contract.h` (the opt-in original-boot capture tool) and `native_pc_bootstrap_image_tests.cpp` | three stores, no inputs: `r13+0x5A18 = 0x80000040`, `*(0x80000048) = 0x370C60`, `r13+0x5A1C = 1`. The address constant it builds, `0x80370C60`, is the debugger exception destination stub (`mfmsr / ori 0x30 / mtmsr / b DBExceptionDestination 0x80370C18`), which is what keeps `DBExceptionDestination` and its callees live. |
| `0x803A051C`, `0x803A065C` | data words of the three stdio FILE objects (`0x80568F0C`, `F14`, `F5C`, `F64`, `FAC`, `FB4`): the console file's read and close procedures | MetroTRK's console file procedures |
| `0x803A05A0` | the console write routine `0x803AF5A8` (the FILE's write proc, no longer called since the printf family is native) | the console write procedure |

The three console procedures talk to the debugger through the TRK message layer, but each begins with two predicates:
the console flag byte `0x805A54A8` (`0x803A0500`; write and read only) and the nub state word `0x805A4F08`
(`0x8039D04C`); when either is clear they return 1 (`__io_error`) having touched nothing. Their callees `0x8039F550`,
`0x8039F560`, `0x8039D04C`, `0x803A0500` have no other caller.

## What was replaced

`runtime/include/gekko/native_trk.h` / `runtime/src/gekko/native_trk.cpp` (`gekko::trk::install`):

* `DBInit`: exactly the three stores.
* the three console procedures: the original's early-return path (r3 = 1, nothing written); with the nub up (`0x805A4F08 != 0`
  and, for write / read, the flag byte non-zero) they stop the run with a message. Nothing in this program starts the
  nub: `InitMetroTRK` stops, and the guest watch below shows no instruction of the replay touches either cell.

Differences from the originals that are not observable: r0, r4 (DBInit) / the saved-and-restored r30 / r31 (frames), ctr,
cr0, and the stack frames the console procedures push (their words are not written by the owners).

## Gates

| gate | evidence |
|---|---|
| 1 what_if (`--own` the four rows, lock `generator`) | `trk: frees 7 rows, revives 0, 0 listed sites become work items` (7 debug.trk rows: DBExceptionDestination, the helpers, ...) |
| 2 contract test `trk_contract_tests` | the pinned DOL's bodies against the owners on identical machines; r1 / r2 / r13..r31, cr (cr0 excepted), xer, lr, FPRs, the low-memory block, 192 KiB of small-data memory, both TRK cells, 64 KiB of scratch and the stack window compared. 18 cases against the original (DBInit with four different low-memory contents; each console procedure with the flag byte 0 / 1 / 0x80 / 0xFF and the nub down; write and read with the flag clear and the nub up), 9 nub-up cases (three procedures x three nub values) assert the owner stops, 439 original instructions. 4 negative controls (verbosity flag not stored, hook word not stored, close returning 0, write setting the flag) are each detected. |
| 3 no surviving reader of retired state | nothing is retired but code. Guest watch of both TRK cells (`0x805A4F08`, `0x805A54A8`) over the full 90 s replay on the baseline: **no guest instruction reads or writes either** (the only access is the state report's own read), so the original procedures never got past their predicates; and in the candidate no TRK owner is hit (`out.log` hit table). DBInit has no caller in the build. |
| 4 game behaviour, alternating A/B x2, 90 s, headless | baseline = the merged tree built without the owners, candidate with them: PCM `015445f2` in all four runs, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames, **`state.txt` byte-identical across the four runs**: 0 differing guest blocks. |
| 5 performance (4 alternating runs, first discarded, job CPU) | base 11.0 / 11.0 / 10.9 s, candidate 10.9 / 11.0 / 11.0 s: no measurable change |

## Removed dependencies / measured counts

Platform-SDK bodies compiled (`removal_plan.py` on `.build/watch`, `../source/.build/dolrecon` manifest, lifted renderer,
same tree before and after): **421 -> 410** (-11: the 4 owned rows and the 7 freed ones). debug.trk is down to the two
`InitMetroTRK*` stop-owner rows. These absolute numbers use the shared manifest; the main session's `b1456bc` measure
(520 / 770) uses its own manifest and tree; deltas are comparable, absolutes are not. (My Step D / E figures, 905 / 928, were
taken on the tree before the gc-removal merge `2b0e093`, i.e. a different tree.)

cpu.cache: the plan's five bodies are already `DCInvalidateRange`, `DCFlushRange`, `DCStoreRange`, `DCFlushRangeNoSync`
(owner rows) and pruned `DCEnable`, `ICInvalidateRange`, ...: no compiled body is left, nothing to do.

## Shared list lines changed (Step G)

* `CMakeLists.txt`: `runtime/src/gekko/native_trk.cpp` after `native_rw_support.cpp`; the `trk_contract_tests` target + test.
* `extract/native-owners.txt` (LF): appended a commented block with the four rows.
* `extract/liveness-roots.txt` (LF): inserted the four addresses.
* `runtime/src/gekko/system.cpp` (LF): include + `trk::install(cpu_);`; `runtime/tests/native_file_api_tests.cpp`: include + owner list.
* `SHADOW_GC_REMOVAL_RATCHET` untouched.

## Tests run (tests tree, Release, capped; tests tree 667 bodies)

Passed: trk_contract_tests (new), rw_support / os_report / os_debug / os_arena_clock / axfx_effects / axfx_reverb /
os_settings / save_store / audio_output / sound_table contract tests, native_file_api_reference_tests, guest_watch_tests,
platform_gc_removal_plan, platform_retired_readers_reverb, platform_rtfs_async_proof, native_jump_tables,
native_wait_points, native_wait_points_classifier, native_host_isa_consistency. Failed: native_file_api_tests (the same
66 CRI rows "listed, not installed", none mine), native_liveness_roots (stale before), platform_runtime_coupling (2
unclassified shipping uses, `0x80491448` and `0x8049CA80` at `runtime/src/gekko/native_rw_world.cpp:233`: not my file,
reported). The rest of the suite was not run.

## Open problems

* `native_pc_boot_contract.h` and the bootstrap-image test still name `0x80370BF0` (DBInit) as a literal: owning it keeps
  the tool and the test working (the owner does what the original did), so the literal costs nothing now.
* The nub-up paths are stops, not emulation: the debugger cannot be started in this program.


---

# Step H: GX display-copy and FIFO-setup leaves (2026-11-08, on gc-removal 1858197)

Assigned by the main session (for `vi`, which needs these GX leaves gone before it can own the display-swap unit in
graphics.vi). Ten rows that are callees of the RW driver's device handler 0x80494EDC and ShowRaster 0x80495CBC and of
GXInit's interior (callers 0x80393B0C, 0x803948xx). None is a frontier row of the plan yet: their callers are compiled
bodies, so they are interior rows; owning them now removes the dependency before the callers go.

## What each row is (from the original words, `tools/doldis.py`)

| row | what | product |
|---|---|---|
| 0x80394AF4 GXInitFifoBase(fifo, base, size) | FIFO object setup, GXInitFifoLimits and GXInitFifoPtrs inlined | **guest memory**: `[0]=base [4]=base+size-4 [8]=size [0xC]=size-0x4000 [0x10]=(size>>1)&~0x1F [0x14]=[0x18]=base [0x1C]=0`; interrupts disabled around the pointer update and restored |
| 0x80396098 GXFlush | dirty-state flush (gx+0x5AC bits -> setter calls), eight zero pipe words, `sc` | __GXData shadow + dead pipe stores |
| 0x80396BD4 GXSetDispCopySrc | BP 0x49 / 0x4A words | shadow gx+0x1E0, +0x1E4 |
| 0x80396CCC GXSetDispCopyDst | BP 0x4D word | shadow gx+0x1E8 |
| 0x80396E54 display copy clamp | bits of the copy words | shadow gx+0x1EC, +0x1FC |
| 0x80397228 GXSetCopyFilter | BP 0x01..0x04 and 0x53 words from the sample patterns | shadow gx+0 (dirty half-word) |
| 0x80397430 display copy gamma | 2 bits of the copy word | shadow gx+0x1EC |
| 0x80399E58 pixel format setter | BP 0x41 word | shadow gx+0, +0x1D4, +0x1DC, +0x204, +0x5AC |
| 0x80399FCC BP mask setter | BP 0x4F word + 0x61 write | shadow gx+0x7C, +0 |
| 0x80397444 GXCopyDisp | BP 0x52 = the `copy_trigger` site | one copy trigger, shadow reads |

## What was replaced

`runtime/include/gekko/native_gx_display.h` / `runtime/src/gekko/native_gx_display.cpp` (`gekko::gx_display`):

* eight rows common to both trees (`install`, system.cpp): `GXInitFifoBase` reproduced exactly (the FIFO object is memory a
  caller can read); the other seven served and silent, the same retirement as the state setters (gx_state, card-saves).
* `GXCopyDisp` only in the lifted build (`install_lifted`, `LiftedRender::install_owners`, `extract/native-owners-r2.txt`):
  every `copy_trigger` is a dead site there (extract/gx-fail-sites-lifted.txt lists 0x8039755C) and its callers' call
  site 0x80495DFC is lowered to `render_submit_frame`; a call that reached it would have stopped at that site, so the owner
  refuses with its own message. The legacy tree keeps the original body (its copy trigger drives the legacy capture).

Not observable: r0, r3..r12, ctr, cr, lr-slot and stack frames the original bodies pushed.

## Gates

| gate | evidence |
|---|---|
| 1 what_if (`--own` the ten rows, lists reverted inside the generator lock) | `gxdisplay: frees 10 rows, revives 0, 75 listed sites become work items`; the 75 listed sites are `extract/gx-sites.txt` rows inside the owned bodies (they stay valid for the legacy tree; the shipping build is unaffected) |
| 2 contract test `gx_display_contract_tests` (`reference;native-contract`) | the originals' words in the reference interpreter: 39 calls over 8 silent rows (every write inside __GXData or the stack frame; no live pipe site, no unlisted pipe store, no write to a vertex-format word; the owner writes nothing and returns with r1 intact), printed retired map (offsets below); GXInitFifoBase 7 cases (interrupts enabled / disabled on entry, bases in main RAM and the image, sizes 0x20 to 0x7FFFFFE0) comparing the FIFO object, callee-saved registers, r1, the MSR and every write; GXCopyDisp: the original emits exactly one copy trigger, the lifted owner stops naming GXCopyDisp, the legacy tree runs the original. Negative controls: GXInitFifoBase variants (count word kept, end pointer without the -4) are told apart in all 7 cases, a write outside __GXData and a vertex-format write are detected. |
| 3 no surviving reader of retired state | guest watch of the 14 words (`--watch-guest`, 90 s replay, candidate): the surviving guest accesses are `0x803970E4` (reads gx+0x1E4 and +0x1EC, writes +0x1EC and gx+0; 2 calls from the game unit 0x80306888), `0x803970E4`'s sibling `0x803971B0` (writes gx+0) and `0x803992C8` (writes gx+0, 702 times), plus the state report. Those are GX library functions of the same family that rewrite the words and send dead pipe stores (the same argument as the state setters); none feeds guest state outside __GXData. The remaining display-copy readers `0x80396EAC` / `0x803970E4` / `0x803971B0` are the next rows of this family (frontier rows of graphics.gx from 0x80306888). |
| 4 game behaviour, alternating A/B x2, 90 s, headless | PCM `015445f2` in all runs, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames. The memory report differs in 5 words: 3 shadow words at 0x805A3140..0x805A3148 (gx+0x1E0 / 0x1E4 / 0x1E8: the BP words GXSetDispCopySrc / Dst kept; the candidate leaves 0) and 2 dead stack words at 0x8060C490..0x8060C49C (the saved back chain and the link-register slot 0x80495E44, the return address after the `bl GXFlush` at 0x80495E40, that GXFlush's frame left). Classified by value pattern and mechanism, as agreed. |
| 5 performance | 4 runs, job CPU: base 17 / 14 s wall, candidate 17 / 15 s: no measurable change. |

Hit counts in the replay (candidate): GXInitFifoBase x2, GXFlush x10,721, GXSetDispCopySrc x2, GXSetDispCopyDst x2,
GXSetCopyFilter x2, copy gamma x2, pixel format setter x2, BP mask setter x2; the **copy clamp (0x80396E54) and GXCopyDisp
(0x80397444) were never reached**: the refusal costs nothing in the replay, but the Westopolis replay does not exercise the
whole game, so a call to GXCopyDisp from an unlowered caller (0x8032DB10 / 0x8032DE58, 0x80495EF8, 0x80496050) would stop
the run with its message instead of silently producing no frame.

## Retired shadow map (offsets in __GXData, from the contract test)

BP mask setter +0 +7C; GXFlush +0 +5A8 +5AC; GXSetCopyFilter +0; GXSetDispCopyDst +1E8; GXSetDispCopySrc +1E0 +1E4;
copy clamp +1EC +1FC; copy gamma +1EC; pixel format setter +0 +1D4 +1DC +204 +5AC.

## Measured counts

Platform-SDK bodies compiled (`removal_plan.py`, `.build/watch`, own manifest, same tree before / after): **410 -> 390**
(-20: the ten owned rows and ten bodies only they reached).

## Shared list lines changed (Step H)

* `extract/native-owners.txt`: 9 rows; `extract/native-owners-r2.txt`: GXCopyDisp.
* `extract/liveness-roots.txt` / `liveness-roots-lifted.txt`: regenerated with the check commands' exact arguments
  (additions only). Note: my revert of Step F had dropped four addresses that other sources name (0x8049BCE0, 0x8049C190,
  0x8049C97C, 0x8049C9F4); the regeneration restores them, and adds 0x80000048 / 0x805A4F08 / 0x805A54A8 (Step G's
  constants).
* `CMakeLists.txt`: `native_gx_display.cpp` after `native_gx_state.cpp`; `gx_display_contract_tests` target + test.
* `runtime/src/gekko/system.cpp`, `lifted_render.cpp`, `runtime/tests/native_file_api_tests.cpp`: includes and install lines.
* `SHADOW_GC_REMOVAL_RATCHET` untouched.

## Tests run

CTest subset (tests tree, rebuilt after the final lists; capped runner, 4.3 % CPU at most): 24 of 27 pass:
gx_display_contract_tests, gx_state_contract_tests, native_gx_sites_reference_tests, native_gx_sync_reference_tests,
save_store_contract_tests, the earlier steps' contract tests (axfx, os arena/clock, os debug, os report, trk, os
settings, sound tables, audio output), native_file_api_reference_tests, guest_watch_tests, native_liveness_roots
(after regenerating the lists), native_jump_tables, native_wait_points(+classifier), native_host_isa_consistency,
platform_gc_removal_plan, platform_retired_readers_reverb, platform_rtfs_async_proof. `platform_runtime_coupling` passes
once the lifted owner table is written one row per line (the classifier's owner-table form); that edit is formatting only
and came after the final builds. Failing, not caused by this step: `native_file_api_tests` (66 CRI rows listed, not
installed), `lift_sites_reference_tests` (fails identically in wt-gc-removal). Not run: the retail-configuration replay,
the pixel gate (no render output changes: GXCopyDisp is unreached and the other rows are shadow-only), the rest of CTest.

## Open problems

* Baseline failures unchanged by this step: `native_file_api_tests` (66 CRI rows listed, not installed),
  `lift_sites_reference_tests` (fails identically in wt-gc-removal: "the four stream-ring sites are generated").

# Step I - two more GX BP setters (0x803971B0, 0x803992C8), on fc64567

Added to `gx_display` (served and silent, both trees): 0x803971B0 (BP 0x4F/0x50/0x51 from a 4-byte colour and a value) and
0x803992C8 (BP with the state register at gx+0x124). Each ends by clearing the halfword gx+2; every other store is a dead pipe
site (`extract/gx-sites.txt`).

| Gate | Result |
| --- | --- |
| Contract test vs the original DOL | `gx_display_contract_tests` PASS: 47 calls over 10 functions (+8 cases for the two rows incl. gx+0x124 values), originals write only __GXData words and stack, owners write nothing, negative controls detected |
| what_if | frees 0 rows, 8 listed sites become work items (interior rows: the callers keep them alive) |
| Guest watch (Westopolis, gx display words) | after the step the only guest accessor left is 0x80494F18 (inside the RW device handler 0x80494EDC, vi's unit; reads gx+1E4/+1EC, writes gx+1EC/+0) plus the state report |
| Replay A/B 2+2 alternating, jobs=0 (base = fc64567 exe, cand = + Step I) | PCM 015445f2, 3,645,000,000 ticks, VI 5,395 in all four; shadow window (0x805A2F60..+0x600) 0 differing words; wall 11-15 s both |
| Pixel gate (5 captures, ticks 5e6/2e8/9e8/2e9/3.5e9) | byte-identical base vs cand (22fdf629, 770a968f, 1c3765ed, 1cb7b963, 8e35fb7e) |
| Hits in the replay | 0x803971B0 x2, 0x803992C8 x702 |

Lists: `native-owners.txt` +2; liveness roots regenerated (+2 each).

# Step J - the GX texture-object functions and the lifted texture loads (graphics.gx interior, last 22 bodies)

`gekko/native_gx_texobj.{h,cpp}`. AC88 (the RW texture bind, 568 insns, 7 guest stores) was NOT owned: its 22 GX callees were
owned directly instead, which frees them regardless of their callers (the planner's "frontier 0" only says nothing outside the
SDK enters them). AC88 stays a compiled RW row.

* Both trees, EXACT (the driver reads these objects back; `extract/gx-sites.txt` lists no pipe site in any of them):
  0x80397E34 GXInitTexObj (+ the six interior jump-table labels 0x80397FB4..0x80398018), 0x80398080 GXInitTexObjCI, 0x803980C8
  GXInitTexObjLOD (float clamps, single-precision multiplies, fctiwz), the getters 0x8039824C / 0x8039828C / 0x80398298 /
  0x803982AC, 0x80398490 (texture-cache region / TLUT initialiser), 0x8039A1E4 GXGetProjectionv (reads the retired, never
  rewritten projection shadow, so it stays exact).
* Lifted tree only (native-owners-r2.txt), served silent: 0x803982C0 (pre-loaded load, + interior 0x803983CC), 0x8039843C
  GXLoadTexObj (+ 0x80398468), 0x803984C8 GXLoadTlut (+ 0x803984F4), 0x803987B8 GXInvalidateTexAll. Every pipe site is dead; the
  guest words they write (printed by the contract test) are the register-id top bytes of the texture / TLUT object, the region
  record the region callback returns, and __GXData shadow words - read only by GX bodies that are owned silent or by these
  functions themselves (the getters read other bits, GXInitTexObj rewrites the object). The region callbacks (gx+0x4C8/0x4CC,
  default bodies 0x80393634/0x80393730) are NOT called; with the loads dropped nothing in the lifted tree calls them, so they
  can stay pruned. The legacy tree keeps the original bodies (it draws through the guest pipelines).

| Gate | Result |
| --- | --- |
| Contract test vs the original DOL (`gx_texobj_contract_tests`) | PASS: 8,945 fuzzed cases of the exact owners (all 16 formats + out-of-range, sizes 0..0x12345, mip on/off, aligned and unaligned objects, TLUT stack argument, LOD floats incl. NaN, +-inf, -0, denormals, 2^31 and out-of-range, 1e30) comparing the arena bytes, r3 / f1, preserved registers and the written word set; 64 projection cases; the five lifted loads (original with the default callbacks installed writes only object / region / __GXData / stack words, owner writes nothing); 3 negative controls detected |
| what_if | frees 9 rows (23 listed sites become work items) |
| Planner (current generator, own regenerated manifest) | platform-SDK bodies compiled 328 -> 306 (graphics.gx 22 -> 0 on this tree; the 328 was measured before this step on the same lists) |
| Replay A/B 2+2 alternating, jobs=0, Westopolis (base = Step I exe) | PCM 015445f2, 3,645,000,000 ticks, VI 5,395 in all four; shadow window 0 differing words; wall 15-19 s both. NONE of the new owners is hit in this replay (AC88 is not reached by W / FH / LH - guest watch of the bind table: 0 writes by AC88), so the A/B proves no regression only; the contract test carries the proof |
| Pixel gate (5 captures) | byte-identical (22fdf629, 770a968f, 1c3765ed, 1cb7b963, 8e35fb7e) |

### Step J follow-ups: recorded corpus, reader proof, reachability

- **Recorded corpus (separate from the fuzz).** The legacy tree was run on Westopolis, Final Haunt and Lethal Highway with an observer on the nine exact entries (and the loads): 34,795 real calls, every exact entry hit (GXGetProjectionv 36 times). `test_data/gx-texobj-corpus/{westopolis,final-haunt,lethal-highway}.txt` keep 250 distinct calls per entry per recording (6,036 lines, entry, argument registers, f1..f3 bits, stack word, the object words before the call); `gx_texobj_contract_tests` takes the directory as its third argument (wired in CTest) and replays them against the original DOL bodies.
- **Reader proof for the silent loads (legacy-tree guest watch, `--watch-guest-from-tick 0`, Westopolis + Final Haunt complete; Lethal Highway was stopped, its recorded loads are the same call sites).** Watched: every texture object (16 B), TLUT name, region record the recorded loads touch and the `__GXData` words the loads write. Native rows attributed with the link map (`ShadowTheHedgehog.map`). Accesses outside the removed bodies (0x803982C0, 0x803984F4 = GXLoadTlut, 0x803988A4 = invalidate) are: (a) other GX bodies on `__GXData` words (0x80395520 / 0x80395578 / 0x80395758 / 0x803959BC / 0x80396934 ...), which touch the words the load does not write or are themselves dead frontier rows; (b) the same heap addresses later reused by unrelated code (string compare 0x803AD8FC..., allocators, memcpy, the many tlut-name addresses are ordinary heap words), which read other data once the texture object is gone; (c) RenderWare class-A pipeline code (0x80463964, 0x8046D124, 0x80464AAC, 0x8045D7C0, 0x80414588) - unreachable in the lifted build (below); (d) one live path, 0x80497A70 / 0x8049A8DC (native texture read, raster set-mip-levels), which reads halfword +0x0E of the image-address word, below the register-id top byte the loads write. No reader consumes the register-id bytes. The getters read bits 8..21 of w0 (not the top byte).
- **Reachability of the texture bind (0x8049AC88).** 11 of its 13 callers are RenderWare pipeline node callbacks entered only through RxPipelineExecute 0x8049C6C4, whose five callers are all lowered / owned in the lifted build; the other path is the Im2D line/triangle and raster-render device-table words (0x80498854 chain), 0 hits in the frontier replay. Details: shared notes `axfx-effects-ac88-reachability.md`.

### Step J on the merged tree (gc-removal round 16, 823cc91)

| gate | result |
|---|---|
| Contract test + recorded corpus | PASS (8,945 fuzz + 15,050 cases incl. 6,036 recorded lines x replays, 3 negative controls); CTest subset gx_display / gx_texobj / rw_raster / gx_state / native_gx_* PASS; `platform_runtime_coupling`, `platform_gc_removal_plan`, `native_liveness_roots`, wait points, jump tables, host-ISA PASS. Baseline failures unchanged: `native_file_api_tests`, `lift_sites_reference_tests` ("the four stream-ring sites are generated") |
| Lifted census (`GEKKO_CALL_CENSUS`, 38 bootable stages x 15 s, `--boot-stage`) | **0 hits on all 35 entries**: the bind 0x8049AC88 and its callers (0x80498854, 0x8049148C, Im2D 0x80491D0C / 0x80491B08, raster wrappers 0x80499A9C / AC4 / AEC, 0x80499924, 0x80499394), the RW node callbacks (0x80449D54, 0x8044EEC4, 0x80452C1C, 0x80463964, 0x804A62D0, 0x804A0B68, matfx chain), RxPipelineExecute 0x8049C6C4, the nine exact texobj entries, the four silent loads. Control in the same build: ShowRaster 0x80495CBC 860 hits in one stage. Title / attract were not run |
| Replay, jobs=0, Westopolis, two runs | PCM 46a70a1051347069 both, 3,645,000,000 ticks (the canonical numbers of round 11+); no separate base exe was built for this merge (the comparison is against the canonical values) |
| Pixel gate, 5 captures | 22fdf629, d22a1750, b6511d3d, 37299f60, ae967db4 = the canonical baselines |

## Step K: RxPipelineExecute as a stop owner, and what the static closure does and does not prove (lifted tree)

Main ruled census evidence ("0 hits in 38 stages") out for stop owners and asked for the static closure of every entry. Tools (committed): `tools/platform/closure_proof.py` (every bl / plain branch / bc / materialisation (constant-folded lis+addi, ori, add) / aligned data word of an address, each classified by the function containing it as OWNED, LOWERED, PRUNED in the lifted build's `recomp_pruned.csv`, or LIVE) and `tools/platform/indirect_calls.py` (every bctr / bctrl / blrl with the load that produced its target, a linear backward slice; a lower bound only). Results on the lifted build with the Step K owners:

| entry | closure | status |
|---|---|---|
| `RxPipelineExecute` 0x8049C6C4 | 5 `bl` sites: 0x80456F3C and 0x80460BC8 LOWERED (rw_pipeline_execute_atomic / _sector), 0x8049B930 / 0x8049BAFC / 0x8049BC90 in the RwIm3D owners (OWNED); 0 plain branches, 0 materialisations, 0 data words. Closed | **stays a stop owner** (`native_rw_node_stops.cpp`) |
| Node-body dispatch | the DOL has exactly ONE call of a node definition's `nodeBody` (def+4) through the idiom `lwz rD,0(node); lwz r12,4(rD); mtctr r12; bctrl`: 0x8049C73C inside RxPipelineExecute (scan of every text word). No node body (the all-in-one definition at 0x8056E98C: nodeBody 0x80464AAC, init 0x80464D30; SubmitNoLightNode's definition 0x8056FD84, ...) runs unless that function does | supports the above; limit: a dispatch through another instruction shape would not be seen |
| Bind 0x8049AC88 and its 16 callers | all 16 `bl` sites are in the render callbacks, the matfx chain and 0x80498854, i.e. in bodies that are OWNED/PRUNED only if the callbacks below are; 0 materialisations, 0 data words, 0 branches | closed **given** the next rows; not a stop owner itself |
| Render callbacks 0x80449D54 (pTank) / 0x8044EEC4 (skin) / 0x80452C1C / 0x80463964 (default) / 0x804A62D0 (MatFX) | 0 `bl`; materialised only in node-definition inits 0x8044960C, 0x80452DB0, 0x80446C88, 0x80464228, 0x804642E8, 0x80464D30, 0x804A64E4 (LIVE: they run at boot), which pass them to 0x804643AC / 0x80464D78 (pipeline creators that store them into the private data of the all-in-one node: 4 setters 0x80463C40..64) or store them (0x80464D30, nodeInit). They are called by node body 0x80464AAC (bctrl at 0x80464B7C, 0x80464C3C, 0x80464C68, 0x80464CA8, 0x80464CD4, 0x80464D00) | **NOT stop owners** (closure not proven): the node body that calls them is unreachable by the row above, but I did not prove that no other function reads those private-data words and calls them; the slice finds none, but it is a lower bound (the node body itself reads them through a register held across the function, which the slice does not follow) |
| SubmitNoLightNode 0x804A0B68 | no `bl`, no materialisation; one data word (0x8056FD84, its node definition's nodeBody), consumer 0x804A17B0 (LIVE definition init) | **NOT a stop owner** (same reason: relies on the nodeBody row) |
| Raster-render wrappers 0x80499A9C / 0x80499AC4 / 0x80499AEC | 0 `bl`; data words 0x8051D494 / 0x8051D4A4 / 0x8051D49C in the device standard-function table (id 17 / 19 / 18), copied into the engine by the device system call 0x804954F8 | **NOT stop owners**; since gc-removal round 18 their core 0x80499924 is a native owner (os-other), so they no longer reach the bind |

Result: Step K shrinks to the one entry whose closure is complete. What I could not cover: (1) every reader of the pipeline nodes' private-data callback words (needs data-flow through registers across whole functions; the node body is the only reader I found, by reading it); (2) the engine's standard-function slots (offsets of ids 17..19 in the engine record), i.e. which API stubs call them (the slice finds 284 sites on slot 0x10C alone, so the slot numbering needs the engine record layout, which I did not recover); (3) address arithmetic that is not a constant fold. A next step that closes (1) is to own the node body 0x80464AAC itself (its only entry is the dispatch at 0x8049C73C) and then the callbacks become unreachable through it.

Gates for the Step K state of 8419a0b (gc-removal a19fe6c merged; the earlier 879b043 gate run failed on gc-removal's own round-18 boot bug, fixed by a19fe6c): 38 of 38 stage boots exit 0 with 0 hits on the watched entries; replay jobs=0 x2 PCM 46a70a1051347069, 3,645,000,000 ticks; 5 pixel captures 22fdf629 / d22a1750 / b6511d3d / 37299f60 / ae967db4; CTest subset passes except `lift_sites_reference_tests` (baseline, "the four stream-ring sites are generated") and `platform_runtime_coupling` ("9 native -> SDK calls, more than the ratchet's 6": native_rw_display.cpp x4, native_guest_scheduler.cpp x2, native_rtgcnfs.cpp, native_gx_sync.cpp, native_video.cpp - none in files of this branch). `platform_closure_proof_tool` (8 tests) passes. The closure tool's first version never reported plain `b` / `bc` branches into a function entry (its own unit test found it); re-run: no checked entry has one.

Node bodies (follow-up commit): `slot_readers.py` lists the functions that load a callback word through a node's private-data pointer (node+0x14, slots 0/4/8/0xC) and call it: only 0x80464AAC (the all-in-one node body, definition 0x8056E98C, nodeBody word 0x8056E990) and 0x80464FF4 (the world-sector node body, descriptor 0x8056E9F4, word 0x8056E9F8) in the RenderWare code, plus 0x800CA8D8 and 0x802462B4 (unrelated game objects). Closure of both bodies: 0 bl, 0 branches, 0 materialisations, one data word each; the definition address 0x8056E98C is materialised once (the accessor 0x80464D6C, one caller 0x804643AC); the only nodeBody call in the DOL is the dispatch in RxPipelineExecute. what_if owning both: 9 interior rows freed, 0 stale sites. The callbacks themselves still have live materialisers (the boot inits), so they stay.

Open: AC88's callers (RW pipeline callbacks) are reached through indirect calls, so "never executed" is a census, not a static
proof. No recording that exercises the texture bind in the lifted build was found (title / attract / the 38 stage boots were
not run); a texture load in the lifted build is served silent either way.

## Step K3: SubmitNoLightNode 0x804A0B68 becomes the fourth node-body stop owner (2026-11-09)

Closure (`closure_proof.py 0x804A0B68`): 0 `bl`, 0 branches, 0 materialisations, one data word, 0x8056FD84 - the nodeBody slot (def+4) of the node definition at 0x8056FD80, which 0x804A17B0 returns (same layout as the all-in-one definition 0x8056E98C / 0x8056E990). The single nodeBody dispatch of the DOL is inside the stop-owned RxPipelineExecute, so the body has no other entry. Same stated limit as the other two bodies: a read of the method slot with another instruction shape is not excluded.

Planner (`what_if --own 0x804A0B68`, `.build/watch` before the merge): frees 1 row, 0x80498854 (the Im bind, graphics.rw-gamecube-driver), revives 0, no generator refusal. The 384 GX-site entries inside 0x804A0B68 stay in extract/gx-sites.txt / gx-fail-sites-lifted.txt, as for every other owned body (the file is the inventory the native_gx_sites test compares).

Gates on the merged tree (gc-removal 3d85e87 + lift_sites fix 27588d7b): 38/38 stage boots exit 0 with 0 hits; replay x2 jobs=0 PCM 46a70a1051347069; five captures 22fdf629 d22a1750 b6511d3d 37299f60 ae967db4; CTest subset (native_rw_world_reference_tests, native_liveness_roots, platform_runtime_coupling, platform_gc_removal_plan, gx_texobj_contract_tests, native_wait_points, native_jump_tables, lift_sites_reference_tests, native_file_api_tests, platform_closure_proof_tool) 10/10 passed. The liveness-roots files were regenerated on the merged tree (the lifted file also gained the merge's new test literals).

Still open in my share: TextureBind 0x8049AC88 (callers are the render callbacks, whose closure over private-data readers is not proven).
