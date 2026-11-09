# Native boot (agent `native-boot`, 2026-11-07)

Branch `agent/native-boot`, cut from `gc-removal` 857399c. Plan step 4 (docs/GC_REMOVAL_PLAN.md "Next steps").

## Result

`__start` (0x80003154) is a native owner (`runtime/src/gekko/native_boot.{h,cpp}`, class `NativeBoot`, installed by
`System`). The generator frees **11 rows** (751 -> 762 pruned; measured, `what_if --name native-boot`). Platform-SDK
bodies compiled (`removal_plan.py`, measured): **shipping tree 1,038 (was 1,044), tests tree 1,044 (was 1,050)**. The
ratchet was NOT changed (still 1,044 / 1,050).

Freed rows: `0x80003140`, `0x8000314C` (flag helpers), `__init_registers 0x800032B0`, `__init_hardware 0x80003400`,
`0x8000340C`, `0x80003488`, `0x80370CDC` / `0x80370CE8` (__OSFPRInit), `0x80371714` (PS init), `0x803725F4` (HID0 ICFI),
`InitMetroTRK 0x803A0000`. NOT freed, and why (what_if with the listed literals dropped frees 18 rows): `DBInit
0x80370BF0`, `__init_data 0x80003340`, `0x80003424` and the constructors' wrapper `0x803796AC` stay live because
runtime/include/native_pc_boot_contract.h (the opt-in original-boot capture tool) and
runtime/tests/native_pc_bootstrap_image_tests.cpp name them as literals (one root list for both trees). I did not
touch them: they are tool/test code, deleting or reshaping them to move a count would be gaming the scanner. Dropping
those literals is a separate, decided step (7 more rows).

## What the original boot did (disassembly; evidence)

`__start`: `__init_registers` (r1 = 0x8060C5F0, r2 = 0x805FA780, r13 = 0x805EC500, rest 0) -> `__init_hardware` (MSR[FP],
`0x80371714`: HID2 |= 0xA0000000, GQR0-7 = 0, `0x803725F4`: HID0 |= 0x800; `__OSFPRInit`: FPRs from BSS words
0x805F1F38/0x805F1F30, both 0; `0x80372838` cache init, repeated by OSInit) -> first frame (`stwu r1,-8`, -1 at
base-8 and base-4) -> `__init_data` -> store 0 at 0x80000044 -> BI2 (0x800000F4): debug monitor kind at BI2+0xC
(2/3 -> InitMetroTRK, 4 -> InitMetroTRK_BBA + flag r13+0x5AF0), argument block at BI2+8 (-> r14/r15 argc/argv,
0x80000034) -> DBInit -> OSInit -> `if (!(u16@0x800030E6 & 0x8000) || (u16@0x800030E6 & 0x7FFF) == 1)` and
`(u16@0x800030E4 & 0xEEF) == 0xEEF` -> OSResetSystem(0,0,0) -> constructors (wrapper 0x803796AC -> walker 0x803796CC)
-> `main(r14, r15)` 0x800510C0 -> tail `b exit` 0x803A3B04.

Facts proven for this image: `__init_data`'s ten copy rows all have source == destination (0x80005544..) and its three
zero ranges (0x8056FE00+0x74700, 0x805EF020+0x375C, 0x805FC540+0xAC) are exactly the gaps between loaded sections, already
zero after `System::load_dol` (also `runtime/src/native_pc_bootstrap_image.cpp`'s verified contract). The disc's BI2
has monitor kind 0 and argument offset 0 (game/sys/bi2.bin). DBInit stores three things: `r13+0x5A18 = 0x80000040`
(__DBInterface), `0x80000048 = 0x00370C60`, `r13+0x5A1C = 1`. The only reader of `r13+0x5A18` in the DOL is
`DBIsExceptionMarked 0x80370C70` (called by OSExceptionInit 0x80371430/0x80371460), which reads the word at
0x80000044. Nothing in the DOL reads `r13+0x5A1C`; `r13+0x5AF0` is read only by `__start` itself; `0x80000048` only
by debugger code. Scan: every load/store through `lis -0x8000` bases in 0x38..0x4F and every r13-relative access
(doldis dump of all text).

## What the native boot does

- Registers: r1 = 0x8060C5E8 with -1 at 0x8060C5E8/EC, r2, r13, r14 = r15 = 0, HID0 |= 0x800 (a reviewer found that the
  CPU reset state lacks this bit: the first capture diff would have shown it). Other registers, HID2, GQRs, FPRs are the
  CPU's reset state (HID2 PSE/LSQE are machine invariants in Cpu).
- Memory: 0x80000044 = 0 (same as the image; the guest reads it), `r13+0x5A18 = 0x80000040`.
- Refuses (stops with a message) when BI2 names a debug monitor (2/3/4) or passes arguments, or when 0x800000F4 is not
  a readable block (the original had a separate path for a null pointer; `System` always writes it).
- Runs the guest in the original order with the original link register: OSInit (LR 0x80003268), the constructors
  (LR 0x800032A0), `main(r3 = r14, r4 = r15)` (LR 0x800032AC), then tail-calls exit with LR unchanged. The three return
  points are owner entries (extract/native-owners.txt); a second entry in the wrong phase stops.
- Retired, not replicated: MetroTRK, argument parsing, `__init_data`, the ICFI-less hardware setup, DBInit's hook word
  (0x80000048) and verbosity flag (r13+0x5A1C).
- Deliberate differences from the original, classified below: the constructors are entered at 0x803796CC (not through
  the wrapper 0x803796AC): 8 bytes less stack depth, LR 0x800032A0 instead of 0x803796BC inside the walker.

## Gates

| gate | evidence |
|---|---|
| 1 what_if | `python -B tools/platform/what_if.py --name native-boot --own 0x80003154 --root 0x803796CC --root 0x800510C0 --root 0x80370E68 --root 0x803A3B04 --release-dir .build/watch --dolrecon-dir ../source/.build/dolrecon`: frees 11, revives 0, 0 stale listed sites, no refusal (capped, lock `generator`; the other session's measurement agrees). Real build: 762 pruned rows (751 before) |
| 2 contract vs the original boot | NOT in the reference interpreter: the original boot ran translated in the baseline exe (base.exe 7b091c3c...) via `--native-boot-contract-dir` (the project's capture tool: CPU block + every guest region at a boundary), the native boot in cand.exe (6c02fa3d...). `tools/agents/boot_contract_diff.py ORIG NATIVE --boundary X`. **main-entry**: registers equal (r1 0x8060C5E8, r3 = r4 = 0, LR 0x800032AC, MSR, all SPRs, FPRs, GQRs) except r31 (the original's `mflr r31` in `__init_hardware`, dead: callee-saved, never read) and pc/npc (translated-code capture artefacts: the original capture reports the caller's stale pc). Memory: 150 words differ = `0x80000048` (retired hook word), `0x805F1F1C` (retired flag), 0x8060C5F4 (`__init_hardware`'s saved LR above the root frame) and 147 words of stack garbage at 0x8060C340..0x8060C5F4 below r1 (the ctor wrapper's frames, 8-byte shift, return addresses, locals). constructor-entry / game-init-entry / game-runtime-entry boundaries: only the same classes (16 and 5 words); CPU equal at the last two. Negative control: `--self-test` flips one bit of one word and one bit of r5 in a copy of the capture and requires exactly those two reports (passes); the diff itself reported the three real retirements, proving it sees one-word differences |
| 3 no surviving reader | `--watch-guest` (ranges 0x80000048, 0x805F1F1C, 0x805F1FF0), cold-boot Westopolis tape, 60 s, armed from the first instruction. Baseline: writes by the boot (and 0x5AF0 by the CRT clear and `__start`'s own read), and ONE read each of 0x80000048 / 0x805F1FF0 attributed to the host `--state-report` writer (`state_report_json` lambda) at exit. Candidate: no write, and only those two host reads; no guest access at all. Symbols are not available in the watch build ("symbols NO"), so attribution of the baseline's translated accesses is by address/order, not row names; the candidate result does not depend on it |
| 4 game behaviour | Westopolis tape, 90 s, alternating A(base) B(cand) A B: PCM sha256 b8e749ac... in all four (note: not 015445f2..., which the coordination file quotes from another build; A = B here), 3,645,000,000 ticks, 2,697 VI, 5,359 render frames in all four. A/A and B/B 64 KiB-block hashes identical. A/B: 3 blocks (image 0, 95, 96); every differing word (peeked): `0x80000048`, `0x805F1F1C` (retired, gate 3), `0x8060C5E4` (stale LR save of `__init_hardware` r31 area, dead stack), `0x8060C5F4` (same), `0x8060C594` (0x803A1560 vs 0x80586814: a stack slot below the live frame holding a constructor-time spill; the 8-byte shifted ctor wrapper frame changes which stale value sits there; not read: PCM and frames identical) |
| 4b cold boot, no tape | `--boot` without `--boot-stage`/tape, 90 s, A B A B: 3,645,000,000 ticks, 2,249 VI, 4,501 render frames in all four; A/A, B/B identical; A/B differ in the same 3 blocks. No audio was produced (empty pcm) and I did not look at the screen: this shows the unattended boot behaves identically, it does not by itself prove the title screen was displayed |
| 5 performance | Westopolis, same pair, runner CPU seconds per 90 s run: base 11.0, 11.1, 7.9, 9.3; cand 10.3, 11.1, 8.8, 10.0 (first of each pair is not discarded here; 4 each). Spread overlaps: no measurable change. No FPS claims |
| build / tests | watch tree + tests tree built (capped, 10 %). CTest (79 tests of platform_/native_/boot/reset/ipl/liveness/wait_points/owners): 72 pass. Failing: `native_liveness_roots`, `native_wait_points`, `platform_runtime_coupling`, `native_file_api_tests`, `native_host_isa_gate` = all in the coordination file's baseline-failure list. `shipping_platform_selection` fails because the tests tree has no ShadowTheHedgehogReference exe (FileNotFound, unrelated to this change). `native_cri_file_device_tests` failed once under `ctest -j2` and passes alone and with the other session's binary (parallel-run interference, not reproducible). `platform_gc_removal_plan` passes |

## Notes for the main session

(Step 1 notes; items about OSInit and the coupling count are SUPERSEDED by "Step 2" at the end.)

- `platform_runtime_coupling` still fails and the count moved: "7 native -> SDK calls, ratchet 6" (it was failing at the
  snapshot). The native boot is one new guest-call user (OSInit 0x80370E68, which goes away when OSInit is owned; the
  constructors, main and exit are not platform SDK). I did not change the test or its ratchet.
- `native_file_api_tests`: I added `NativeBoot` to its runtime owner list (runtime/tests/native_file_api_tests.cpp); the
  remaining "listed, not installed" lines are the CRI owners only (baseline).
- `native_liveness_roots` was stale at the snapshot (171,881 addresses ... the test's own message). I did NOT regenerate
  the file (a regeneration adds ~500 unrelated roots and would change the generated code); I appended my own literals.

## Open problems

- OSInit (0x80370E68) remains the guest's; it is entered from the native boot, from the game (0x80051028; a no-op after
  the first call: `r13+0x5A40` guard) and the runtime. Owning it is the next os.core step (state: OSInit's exception
  table, arena words 0x80000034 etc. are read by surviving code, see retired_readers_osinit).
- 7 more rows (DBInit, `__init_data`, 0x80003424, the ctor wrapper ...) wait for the original-boot capture tool's and the
  bootstrap-image test's literals to go.
- The contract was not run in the reference interpreter (translated original vs translated native).

## Shared list lines changed

- `extract/native-owners.txt`: appended a commented block with 0x80003154, 0x80003268, 0x800032A0, 0x800032AC.
- `extract/liveness-roots.txt`: appended 0x80003268, 0x800032A0, 0x800032AC, 0x80370E68, 0x803A3B04, 0x80000040,
  0x80000044, 0x800030E4, 0x800030E6 at the end.
- `CMakeLists.txt`: `runtime/src/gekko/native_boot.cpp` after native_reset.cpp.
- `runtime/include/gekko/system.h`, `runtime/src/gekko/system.cpp`: member `boot_`, accessor, construction and `install()`.
- `runtime/tests/native_file_api_tests.cpp`: one line (owner list).
- New: `runtime/include/gekko/native_boot.h`, `runtime/src/gekko/native_boot.cpp`, `tools/agents/boot_contract_diff.py`.

Capped-runner CPU shares: baseline watch build 7.84 %, candidate build 8.70 %, tests build 9.33 %, game runs 0.3-1.2 %,
generator runs about 3 %.

## Step 2: OSInit is native (2026-11-07, same branch, after merging `gc-removal` 99fe8c3)

The main session's merge conditions: (1) own OSInit so `platform_runtime_coupling` stays at 6, (2) the contract against the
ORIGINAL boot in the reference interpreter, (3) cold boot through the title screen with frame captures, plus the boot-chain
rows (0x80370CDC, __OSPSInit 0x80371714, __OSCacheInit 0x80372838, OSGetConsoleType 0x80370E04, the os_exception_entry.cpp
literals, OSInitBATs 0x80376A48 / 0x80376AC8, the data word 0x80378320) and the OSInit-only interrupt rows (__OSExceptionInit
0x80371348, __OSSetInterruptHandler 0x80376168, __OSMask/UnmaskInterrupts 0x803764E4 / 0x8037656C, OSSetErrorHandler 0x80373378).

### What changed

- `NativeBoot::os_init` (runtime/src/gekko/native_boot.cpp) is OSInit, and `0x80370E68` is its native owner. `__start`
  calls it directly (no guest call, no `0x80003268` continuation any more); the game's second call at 0x80051028 enters the
  owner and returns at once (`r13+0x5A40` guard), as the original did. What it does, from the disassembly of OSInit and its
  callees (every value is derived the way the original derived it, none replayed from a capture): the "initialised" flag; OSGetTime
  into `r13+0x5A50/54` (time base + the loader's 0x800030D8/DC); the 0x1C-byte copy at 0x80586C90 (or its clearing, by the
  loader's pointer at 0x800030F0); HID0[SPD] and FPSCR[NI]; the boot-information words (`r13+0x5A20/24/5C58/5B54`, bytes
  0x800030E8/E9); the arena bounds (`r13+0x26E8` / `r13+0x5A70`, from 0x80000030/34 or the defaults); `__OSExceptionInit`
  (the 15 vector pages and `__OSExceptionTable`, `r13+0x5A44`) with `__OSInitSystemCall` (page 9's PPCSync head); `__OSInitAlarm`
  (`r13+0x5A58/5C`, the decrementer handler); the FP-unavailable handler and `0x800000D8`; `__OSThreadInit` (the default thread,
  its context pointers 0x800000C0/D4/DC/E0/E4, the run queues, the stack guard word 0xDEADBABE); `__OSInterruptInit`, `EXIInit`,
  `__OSInitSram`, `__OSInitAudioSystem` as served owners in the original order (`Cpu::serve_owner`, new: it runs the installed
  native owner of an address, never a guest body); `__OSInitMemoryProtection`'s one lasting effect (MEM_ADDRESS unmasked in the OS
  mask word, through `NativeInterrupts::unmask_os_interrupts`); `OSEnableInterrupts` (MSR[EE]); MSR[FP] cleared by
  OSSetCurrentContext.
- The vector pages come from `runtime/include/gekko/os_exception_vectors.h` (the 38 instructions of `__OSExceptionVector` and
  PPCSync's 7-word head, from the retail DOL). `Cpu::os_vector_installed` (os_exception_entry.cpp) now checks against that
  table instead of reading the template out of the guest image, so `0x80371620`, `0x8037829C` and the vector pad rows are no
  longer named by the runtime. (card-saves announced the same change for os_exception_entry.cpp: expect a textual conflict
  there; the content should be the table form.)
- RealMode `0x80376B48` (game_memory.cpp) is retired with its two BAT-configurer literals: its only callers were
  `__OSInitMemoryProtection`'s. `GameMemory::Stats::bat_configurations` and its report line went with it.
- Reference runner only: `--original-boot` (the owners of NativeBoot decline, so the interpreter runs the original `__start`
  and OSInit) and `--native-boot-contract-dir` now works in the reference runner; two capture boundaries were added
  (`osinit-entry`, `osinit-return`).

### Rows (measured)

- `what_if --name osinit --own 0x80370E68` (before adding any literal root): **49 rows freed**, 0 revived (cpu.config 13,
  os.other 10, os.interrupts 6, debug.trk 4, cpu.cache 4, os.core 3, os.threads 3, os.alarms 2, os.memory 2, unidentified 1,
  os.time 1). Rooting the four handler addresses the native OSInit writes (0x80371E14 is the one that reaches into a row) and
  retiring RealMode changes that by a few rows; the final generator state is in the build, measured below.
- Boot-chain rows now pruned: 0x80370CDC, 0x80371714, 0x80372838 (__OSCacheInit), 0x80371348, 0x80376168, 0x803764E4,
  0x8037656C, 0x80373378, 0x80376B60 (__OSInitMemoryProtection), 0x803717A8 (OSInitAlarm), the RealMode owner and the two BAT
  configurers (their literals are gone). **Still live, with the reason**: `OSGetConsoleType 0x80370E04` (three live callers in
  the EXI / UART code: 0x803AF5A8, 0x803B3F90, 0x803B57D8; not an OSInit-only row), `0x803716BC` (the default exception handler:
  the native OSInit writes it into the table and the guest runs it, it must stay), the data word 0x80378320 at 0x805EEC10
  (`r13+0x2710`: the default switch-thread callback, a `blr`; the data word is image data).
- Native -> SDK calls (`runtime_coupling.py`, tests tree): **5** (ratchet 6; the boot's guest call to OSInit is gone and the
  tree has one fewer than the main session's 6). `platform_runtime_coupling` passes.
- Platform-SDK bodies compiled (`removal_plan.py`): tests tree **916** (ratchet 969); the watch tree (lifted renderer, not
  the shipping configuration, which I did not build here) **798**. I did not touch the ratchet.
- `fn_80378320` (the empty body at the data word 0x805EEC10 = `r13+0x2710`, the default switch-thread callback) stays: the
  scheduler (0x803787B0, 0x803788E8) and the guest thread code read that word and call through it; it is image data, so
  the generator roots it. `OSSetArenaHi 0x80372224` is pruned with OSInit (it was in the 49).

### Gates

| gate | evidence |
|---|---|
| 2 contract vs the ORIGINAL boot, in the REFERENCE INTERPRETER | the original boot ran in `ShadowTheHedgehogReference --no-recomp --original-boot --native-boot-contract-dir` (seven captures: start, crt-entry, dbinit-entry, osinit-entry, osinit-return, constructor-entry, main-entry; the interpreter, pinned DOL, every guest instruction interpreted); the native boot ran in the translated `ShadowTheHedgehog.exe` (`cand4.exe`). `boot_contract_diff.py` at **main-entry**: one register differs, r31 (the original's `mflr r31` in `__init_hardware`, dead); memory: 244 words, all classified below. At **constructor-entry**: r0 / r4 / r6 / r8 / r9 / r11 / r12 / r31 / XER / CR are caller-saved garbage the original's OSInit left, LR / r1 differ by the constructors' wrapper (0x803796AC, not run natively: LR 0x800032A0 and r1 0x8060C5E8 versus 0x803796BC and 0x8060C5E0), 134 words. Negative control: `--self-test` flips one bit of one word and of r5 and requires exactly those two differences (passes). The exception vectors (570 words), the exception table, the thread block, the context pointers, the arena words, the interrupt mask word 0x800000C4 (after the MEM_ADDRESS unmask was added: before it the diff showed F7FFFFE0 vs FFFFFFE0) are all EQUAL, which is the contract. Classified differences: `0x80000048` (DBInit hook word), `0x805F1F1C` (DBInit flag), `0x80000060..83` (debugger stub copy), `0x80003040..53` (interrupt handlers 0-4, MEM), `0x805612FC / 0x80562600 / 0x805F1FB0/B4` (the version list: OSRegisterVersion's), `0x80586CB4` (error handler 1), `0x80568F24..3B / 0x80568F54` and `0x805A6438..7B` (libc stdout FILE state and the print buffer of the console banner prints), `0x805F2388 / 0x805F23A0 / 0x805F23DC/E0` (libc console lazy-init state), `0x805874DC` (a context's saved XER: the first exception saves the dead XER, which differs), and the stack words 0x8060C1C0..0x8060C5F4 below r1 (the original's OSReport / OSInit frames: dead). |
| 3 guest watch of the retired ranges | `--watch-guest retired2.ranges` on the Westopolis tape, candidate (`F-W1`): **the retired state IS touched by survivors, not unread**: the game's own `printf` path reads and writes the libc stdout FILE state, the print buffer and the console-init flags (766/216 and 443/90 accesses), and `OSRegisterVersion` callers read and write the version list head (3/2). In the candidate those words start at zero and the first guest print re-initialises them (the watch shows the writes to the init callback / flag / magic: 30 / 1 / 2); the version list is only inserted into. Every other retired range (hook word, debugger stub, MEM handlers except the owner's own zeroing, error handler 0x80586CB4, debugger-link flag) shows only the host's end-of-run state report (the same counts as the exit peek) and no guest access. Caveat: the proof is the replay, not a theorem; and the libc state is not "unread" but re-derived: the A/B below is the evidence it makes no difference. |
| 4 game behaviour (headless, Westopolis tape, 90 s of game time) | `base2.exe` (merged gc-removal + the previous native-boot commit, rebuilt from that tree) vs `cand4.exe`, alternating A B A B: PCM sha256 `b8e749ac...` in all four, 3,645,000,000 ticks, 2,697 VI, 5,359 render frames in all four; A1/A2 and B1/B2 memory identical (0 words); A/B differ in **23 words** at the end (peeked blocks 0x80000000, 0x80560000, 0x80580000, 0x805F0000), all classified: `0x80000060..80` (9, debugger stub), `0x80003040..50` (5, MEM handlers), `0x805612FC / 0x80562600 / 0x80568754 / 0x80568758 / 0x805F1FB0 / 0x805F1FB4` (version list: later registrations link differently), `0x80568F38 / 0x80568F54` (libc byte counter: 0x27D vs 0x1B2, differ by exactly 0xCB = the 203 bytes of banner text not printed), `0x80586CB4` (error handler). (The earlier A/B runs in this session, with the previous candidate, were windowed and without the `game-window` lock: no game of the user's was running (`tasklist`), but the rule was not followed; they are not used for the gates.) |
| 4b cold boot through the title screen (headless, `ui-test mash.uitest`) | an unattended cold boot waits at the PAL "60 Hz mode?" prompt (viewed: `cap80.png`), it does not reach the title. `tools/ui-test/mash.uitest` (A / START with gaps) goes through: captures at game tick 1.0e9 = Dolby Pro Logic II, **1.5e9 = the title screen** (SHADOW THE HEDGEHOG, 1P GAME / 2P GAME / OPTIONS; viewed), 2.0e9 = the Story Mode menu (viewed). Base vs candidate, two runs each, per tick: the PNG sha256 is identical in all four (0ffed20f0358... / ee144813cce0... / 3fd8be4d0dbe...), PCM identical (1290bf01...), 2,659 VI, 4,771 render frames. |
| 5 performance | headless Westopolis, same pair, alternating, user+sys CPU seconds of `run.sh` (first pair discarded): base 0.61 0.60 0.63 (mean 0.61), cand 0.64 0.58 0.69 (mean 0.64): spread overlaps, **no measurable change**. |
| build / tests | watch tree (game + reference) and tests tree built at 10 % CPU (job CPU 55 min and 67 min, 8.4-8.9 %). Full CTest (tests tree, -j2, 194 tests) then a re-run of the affected set after the fixes below. Regressions I caused and fixed: gekko_core_tests, ppc_recomp_tests, native_fallback_tests, native_execution_tests, compiler_poll_tests (their fixtures built the vector page with the template bytes at 0x80371620 being zero in the fixture and only the `li r3` patched: the CPU now checks the real page, so the fixtures write the whole page from `os_vectors::write_page`; a negative of the old check is the original failure); lift_l1_evidence (`tools/lift/verify_l1_input.py`: 11 sites, OSInit as a native owner may hold the SIInit call; the removed site is OSInit's 0x8037101C); lift_sites_reference_tests (kSitesNothing without 0x8037101C, 1046 sites = 11 + 1018 + 4 + 10 + 3: the test still fails on the stream-ring sites, a baseline failure); platform_gc_removal_plan and platform_retired_readers_osinit (the positive control was OSInit itself, which is now owned and frees nothing: control row is OSSetCurrentContext 0x80372B80, frees 2 rows and has 2 proven surviving readers of 0x800000D4; the retired-readers test is renamed `platform_retired_readers_context`). Passing in the re-run: gekko_core_tests, ppc_recomp_tests, native_fallback_tests, native_execution_tests, compiler_poll_tests, lift_l1_evidence, platform_gc_removal_plan, platform_runtime_coupling, platform_retired_readers_context / _reverb, native_gx_sites(_reference), native_video_reference, gameplay_scheduler_reference, platform_rtfs_async_proof. Still failing, all baseline: shipping_platform_selection (no reference exe in the tests tree), native_file_api_tests (CRI owners; none of mine), lift_sites_reference_tests (stream-ring sites), difftest_f1b, f1c_canary_longrun, native_liveness_roots, native_wait_points (stale list files), render_core_tests, options_*_tests, ax_pb_tests, lift_purity, render_determinism, native_host_isa_gate. Not completed: dolrecon_lift_tests (timeout 1800 s) and rwcore_palquant_tests (timeout 900 s) at the 10 % cap; neither depends on this change as far as I can tell, I did not prove it. |

### Retired and why it is safe (and what is not proven)

DBInit's hook word and verbosity flag, the debugger-link flag, the debugger stub at 0x80000060, interrupt handlers 0-4 of the
memory interface, the version list, the cache-init error handler, the console banner prints and the libc state they touched,
the arena / stack zero-fills (no-ops: the capture shows no word of the arena region differing), the performance-monitor SPR
writes (zero in the reset state), L2 / cache enables (equal in the capture), the stack fill (zeros over zeros). Not proven:
that no other run touches the retired words (the watch covers one replay and the cold boot).

### Open problems

- Rows still live that this step could free if the literals in the capture tool header (`native_pc_boot_contract.h`) and the
  bootstrap-image test went (DBInit, `__init_data`, 0x80003424, the constructors' wrapper 0x803796AC): a separate decision.
- OSGetConsoleType 0x80370E04 has live EXI / UART callers (not an OSInit row).
- The three vector-check test fixtures changed; card-saves changes os_exception_entry.cpp too.

### Shared list lines changed (step 2)

- `extract/native-owners.txt`: added `0x80370E68 OSInit`; removed `0x80003268 entry:` (the continuation is gone); removed
  `0x80376B48 RealMode` and its comment.
- `extract/liveness-roots.txt`: removed 0x80003268, 0x8037101C, 0x80376A48, 0x80376AC8, 0x80376B48; appended 0x80000030,
  0x800030C8/CC/D0/D8/DC/E8/E9/F0, 0x80371E14, 0x80586C90, 0x80587038, 0x80587450, 0x805FC5EC, 0x8060C5F0, 0x808000C0,
  0x808000D4, 0x80A53000, 0x817A0000 (literals the scanner finds in native_boot.cpp / os_exception_vectors.h; the vector
  table's instruction words look like addresses), 0x80370E68.
- `extract/lift-sites.txt`: removed `site 0x8037101C`; `extract/lift-sites-native.txt`: removed `site 0x803712E8`,
  `site 0x80371328` (DVDInit and DVDInquiryAsync calls inside OSInit); `extract/wait-points.txt`: removed 0x80372684,
  0x80376540, 0x803765C8; `extract/platform-sites.txt`: removed 0x80376B88, 0x80376B94, 0x80376C1C (all inside rows that left
  the build with OSInit).
- `CMakeLists.txt`: `--control-own 0x80372B80` and the retired-readers control `context;0x80372B80;readers`.
- `runtime/src/gekko/cpu.cpp/.h` (`serve_owner`), `os_exception_entry.cpp`, `game_memory.cpp/.h`, `system.cpp`
  (`boot_->bind_interrupts`, the stats line), `shadow_main.cpp` (`--original-boot`, capture in the reference runner),
  `native_pc_boot_contract.h` (two boundaries), `tools/lift/verify_l1_input.py`, the five test fixtures and
  `runtime/tests/lift_sites_reference_tests.cpp`.
- New: `runtime/include/gekko/os_exception_vectors.h`.

> **Correction (2026-11-07, found in step 3):** every PCM hash quoted in steps 1 and 2 (`b8e749ac...`) is the hash of 11,447,680
> ZERO bytes: my harness ran with `SHADOW_NATIVE_JOBS=1` (native jobs on), under which `--audio-dump` is silent (and the run has
> 5,359 render frames instead of the canonical 5,360). Those PCM equalities proved nothing about audio; the other evidence in
> those rows (ticks, VI, render frames, memory words, captured frames, the contract) is unaffected. The canonical PCM is
> `015445f2` (jobs off); step 3 redoes the A/B with jobs off. Also: the lifted roots file committed in 9545590 / b0c48fe was
> generated from an incomplete uncompiled-sources list (a comment's `)` cut the CMake list short: `native_hud.cpp` missing), so
> `native_liveness_roots_lifted` failed; it is regenerated correctly from the step-3 commit on.

## Step 3: the decrementer exception is not delivered (os.alarms, 5 rows)

Direction from the main session: no native re-implementation of the OS decrementer handler / OSLoadContext (a first draft
of both was discarded before any commit); measure whether anything ever sets an alarm, and if not stop delivering the
exception.

### Evidence that no alarm is ever set

- Every function that can link an alarm into the queue (`r13+0x5A58`) is NOT compiled: OSCreateAlarm 0x80371800, OSSetAlarm
  0x80371A60, OSCancelAlarm 0x80371AC8 and all their callers (the DVD library 0x80379xxx, 0x8038CDDC). The only compiled
  writers of the queue were the decrementer callback 0x80371BE4 and the insert 0x80371810 that only the callback reaches
  (`mtdec` itself, 0x80370B0C, is called from those three only; the DOL has one other `mtdec`, 0x8039F7F8, not compiled).
- The CPU arms the decrementer at power-on (`Cpu::reset`: DEC = 0xFFFFFFFF, edge armed) and re-arms it 2^32 ticks after
  every edge. So the edge was NOT idle: `dec_edge_total` = 0 in a 90 s run and **3 in every 400 s run** (edges at about
  106 s, 212 s, 318 s), on all four recordings (final-haunt stg0604, lethal-highway-sfx stg0202, user-westopolis-ramp and
  westopolis-cpu-spike stg0100). Each edge ran the guest handler 0x80371E14 -> 0x80371BE4, which found the queue empty and
  resumed the interrupted code (`OSLoadContext`).

### What changed

- `Cpu::set_decrementer_delivery(bool)` (default true: the architecture). `NativeBoot::install` turns delivery off; the
  edge is then counted (`dec_edge_total`) and dropped (`dec_edge_dropped_no_consumer`, new, in the run report's
  `gekko_conformance` block and in the verify snapshot) and the next edge is still armed 2^32 ticks on. A guest `mtdec`
  (interpreter `mtspr` and `ppc::write_spr`) turns delivery back on: then exception 8 reaches the OS's default handler, and
  `Cpu::enter_os_exception` stops the machine with "the decrementer exception was taken at pc ..., but this program has no
  decrementer handler: no alarm code (OSSetAlarm and its callers) is compiled, and the guest armed DEC itself with mtdec"
  instead of entering the "unhandled exception" report. `--original-boot` (reference runner) turns delivery back on.
  Delivery is configuration, not state: not in a savestate; set at construction.
- The native OSInit no longer writes handler 0x80371E14 into `__OSExceptionTable[8]` (the default 0x803716BC stays) nor
  roots it (literal removed); `__OSInitAlarm`'s other effect, the empty queue, is still written.
- No guest handler is installed and nothing is saved or restored natively.

### Rows (measured, `what_if --name dec-root --root 0x80371E14` on the final lists: rooting the handler revives exactly the rows the change frees)

**5 rows freed**: 0x80370B0C (the `mtdec` wrapper), 0x80371810 (alarm insert), 0x80371BE4 (the decrementer callback; the
handler entry 0x80371E14 lies inside this row), 0x80371E5C, 0x80379648 (OSGetTime). Matches the planner's os.alarms line
(1 frontier row, 5 bodies).

### Gates

| gate | evidence |
|---|---|
| 2 contract vs the original boot (reference interpreter) | same captures as step 2; the only new difference at constructor-entry / main-entry is `0x80003020` (exception table entry 8: original 0x80371E14, native 0x803716BC): +1 word, all other differences as in step 2. Test: `gekko_core_tests` "decrementer delivery: off by the boot, on by mtdec, named stop": delivery default on and an edge latched; off: nothing pending, counted and dropped (total and dropped), next edge armed +2^32; a guest `mtspr DEC,r3` turns delivery on and writes DEC; with the OS's default handler at exception 8 a latched edge stops the machine with a detail naming "no decrementer handler" and "mtdec"; with a registered handler the exception is entered as before. |
| 4 behaviour, jobs=0 (canonical), recordings longer than 106 s, delivery ON (the previous candidate, handler runs 3 times) vs OFF (this one): final-haunt, lethal-highway-sfx, user-westopolis-ramp, westopolis-cpu-spike, 400 s each, headless | PCM identical in all four pairs (5ae9f9c8, 459b216c, fdae7920, 6639c9f8: four different recordings, real audio), 16,200,000,000 ticks, 11,997 VI, 23,960 / 23,959 / 23,960 / 23,960 render frames equal per pair; `dec_edge_total` 3 = `dec_edge_dropped_no_consumer` 3 for OFF, 3 / 0 dropped for ON. End memory (64 KiB block hashes plus word peeks of 0x80000000 / 0x805A0000): the exception table word `0x80003020` and, in three of four recordings, five words at 0x805AF364..0x805AF374 (`80587768 80000000 80587038 805AF660 80371C08` in the ON run, zero in the OFF run): the dead stack frame of the callback the ON run executed (0x80371C08 is a return address inside it), below the stack pointer of the interrupted thread. Nothing else differs. |
| 4 behaviour vs the MERGED baseline (gc-removal 7e3cda2 without the native boot, `base3.exe`, built in a worktree under .build) | Westopolis 90 s, headless, jobs=0, alternating A B A B: PCM 015445f2 in all four (11,072,027 non-zero bytes; the canonical value), 3,645,000,000 ticks, 2,697 VI, 5,360 render frames; A/A and B/B memory identical (0 words); A/B 26 words: `0x80000048` and `0x805F1F1C` (DBInit's hook word and flag, no longer stored: no reader, step 1), `0x80000060..80` (debugger stub), `0x80003020` (exception 8), `0x80003040..50` (MEM handlers), `0x805612FC / 0x80562600 / 0x80568754 / 0x80568758 / 0x805F1FB0 / 0x805F1FB4` (version list), `0x80568F38 / 0x80568F54` (libc byte counter), `0x80586CB4` (error handler). |
| The 28 words at 0x80587C28..0x80587D50 | NOT caused by this branch: base-vs-base across the merge (pre-merge gc-removal 99fe8c3 `base2.exe` vs merged 7e3cda2 `base3.exe`, both without any native-boot code) already differ in exactly those words (30 words: those plus 0x80000048 and 0x805F1F1C of the DBInit state, which base2 had because it ran the guest boot). The words are the VI library's state (every reference to them is a `vi_*` row: 0x8037E290, 0x8037E824, 0x8037ED28, 0x8037EFFC, 0x8037F19C, 0x8037F9A4): the merged **agent/vi** work stopped the guest VI library from writing them (base 0x0F060001 0x476901AD ... -> 0). |
| PCM caveat found while checking the baseline | `--audio-dump` is ALL ZEROS when `SHADOW_NATIVE_JOBS=1` (11,447,680 zero bytes = b8e749ac, 5,359 render frames), with or without `--audio`, headless or windowed: every PCM hash I quoted in steps 1 and 2 (b8e749ac) was therefore of silence. Ticks, VI, render frames, memory words, captured frames and the contract were not affected; the PCM equality is now re-established with jobs=0 (015445f2 above and the four recordings). Open finding, not fixed here: why the dump is silent under native jobs. |
| performance | not measured for this step (no code on a hot path changed: one extra branch in `latch_decrementer`, three times per 400 s). |

## Step 4: os.reset / os.time / cpu.config (2026-11-07, after merging gc-removal b4c53ee as 660a971): analysis, no code change

- **os.reset**: of its four compiled rows only `0x803771F0` (`__OSResetSWInterruptHandler`, OS interrupt 22) could be mine
  (my native OSInit registers it in the handler table at 0x80003098; `what_if --drop-root 0x803771F0` on the pre-merge lists
  freed that 1 row). The merged work (native_interrupts.cpp) has meanwhile made 0x803771F0 and `MEMIntrruptHandler`
  0x803769DC native owners that stop with an explanation if ever entered ("the PC has no reset switch and nothing dispatches
  through the OS's handler table"), so the row is no longer compiled whatever I write, and keeping the table write keeps the
  contract diff one word smaller. Tried, measured, reverted. The other os.reset rows are not OSInit rows: `0x80376D6C`
  (OSRegisterVersion insert, still called from 0x80380EB8), `0x80376FB8` (reset entry used by data at 0x80003100 / 0x800053E0),
  `0x803771B8` OSGetResetCode (a native owner already).
- **os.time**: OSGetTime `0x80379648` left the build in step 3; what is compiled is `OSGetTick` (`0x80379628`, `0x80379640`, 2
  leaf rows, 39 guest call sites in hot code). Owning it means a native-owner dispatch (`mftb`) per call instead of translated
  code. Not done: needs the main session's decision and an alternating performance measurement.
- **cpu.config**: nine compiled leaf rows: PPCMfmsr 0x80370ADC, PPCMtmsr 0x80370AE4, PPCSync 0x80370B14 (an owner), PPCHalt
  0x80370B1C, the `mffs` / `mtfsf` wrappers 0x80370B60 / 0x80370B80, the `mtspr 0x398 / 0x399` setters 0x80370BA8 / 0x80370BB0 /
  0x80370BB8. The planner's one frontier row is PPCHalt, entered by os-other's native OSPanic (native_os_panic.h calls it as a
  guest function); owning it frees nothing further: the other rows are pinned by live guest callers in other services (the
  unhandled-exception report 0x80373590, GX 0x80393A88, DSP 0x80382ACC, 0x80393754, 0x80394BDC). They can be owned one by one
  (a few lines and a contract test each); that removes only those bodies. Not done: waiting for the main session's decision.
- **Gates for the committed sources (cand8) against the gc-removal tip without the native boot (base4, built in a worktree,
  deploy disabled)**, jobs=0, headless: Westopolis 2 x 2 alternating: PCM `015445f2` in all runs, 3,645,000,000 ticks, 2,697 VI,
  5,360 render frames, A/A and B/B memory identical, A/B 24 words (all in the classified sets of steps 1-3: DBInit hook word and
  flag, debugger stub, exception table 8, MEM handlers, version list, libc byte counter, error handler). The four 400 s
  recordings (final-haunt, lethal-highway-sfx, user-westopolis-ramp, westopolis-cpu-spike) base4 vs candidate: PCM
  `5ae9f9c8` / `459b216c` / `fdae7920` / `6639c9f8`, 16.2e9 ticks, 11,997 VI and 23,960 / 23,959 / 23,960 / 23,960 render frames
  equal in each pair; end-state words differ in the same 24. The 28 words at 0x80587C28.. of step 3 do NOT appear against this
  baseline (both sides contain the merged vi work), confirming they were the merge's. Contract vs the original boot
  (reference interpreter): constructor-entry 12 registers / 135 words, main-entry 1 register (r31) / 245 words, the step-3 classes.

## Step 5: input.pad 0x803817D4 (SPEC2_MakeStatus): the data word at 0x805EEC84 is retired (1 row)

The row is `SPEC2_MakeStatus`, the PAD library's decode of the raw SI poll words into PADStatus (a pure data transform, not
SI / pad polling: nothing here reimplements hardware). It was live only because of the data word 0x805EEC84 = r13+0x2784,
the library's `MakeStatus` pointer slot (set by PADSetSpec 0x8038148C).

Evidence that the slot has no live reader (scan of the DOL, `.build/padscan.py`, not kept): every r13-relative access to
0x2770..0x27A0 is inside the PAD library (0x80380xxx..0x80382xxx); the only LOAD of 0x2784 is 0x8038125C in PADRead
(0x80381008), which is pruned, and the stores are PADSetSpec's. No `lis`/`addi` pair forms an address in
0x805EEC60..0x805EECA0 and no data word points into it. Controller input is the lifted peripheral layer (L1);
`verify_l1_input.py` (claim 6: every PAD / SI caller is a lowered site or the library) still passes (11 sites, 5 retired
words).

Change: `retire 0x805EEC84 0x803817D4` in extract/lift-sites-native.txt (same mechanism as the alarm / CARD reset records).
No code. Generator (what_if, watch tree): pruned rows 1332 -> 1333, the difference exactly 0x803817D4 (fn_803817d4, 284 B
of translated code). SDK bodies still compiled: one fewer; ratchet not touched.

Gates: 1 what_if: the one row; 2 no owner, nothing replaced: the evidence above; 3 alternating 2x2 Westopolis 90 s,
headless, jobs=0, exe without (cand9a) vs with (cand9b) the line, both on the merge of gc-removal 1858197: PCM 015445f2 in
all four, state report byte-identical (3,645,000,000 ticks, 2,697 VI, 5,360 render frames, the four 64 KiB peeks of
0x80000000 / 0x80560000 / 0x80580000 / 0x805F0000); 4 CTest subset (32 tests) on the tests tree after the change: 29 pass,
the 3 known baseline failures (shipping_platform_selection, native_file_api_tests owner list, lift_sites_reference_tests).
Performance: none (a function that never ran is not emitted).

## Step 6: audio.ax mixer entries MIXQuit 0x8038A2BC and MIXSetSoundMode 0x8038A2C0 (2 rows, released by audio-ax)

audio-ax kept the rest of the MIX group (AXFXSetHooks 0x8038A12C, MIXInit 0x8038A1F4 and the hook words 0x805EECB8 /
0x805EECBC: they write mixer channel state through its VoiceState) and released these two.

Original bodies (read from the pinned DOL, not from a name): MIXQuit is `blr` (0x4E800020); MIXSetSoundMode is
`stw r3,0x5DC8(r13); blr`. The word r13+0x5DC8 (0x805F20C8) is read by live mixer code (0x8038A138, 0x8038A2C8,
0x8038B1F8; __MIXResetPan compares it with 3), so the store is state a survivor reads and the owner performs exactly it.
No DSP / mixing behaviour is imitated. Callers: MIXQuit from 0x801F2DA8 (0x801F2E14); MIXSetSoundMode from 0x801F21AC
(three sites).

Change: `runtime/include/gekko/native_mix_entries.h`, `runtime/src/gekko/native_mix_entries.cpp` (`mix_entries::install`,
called from System after os_font), `extract/native-owners.txt` (2 rows), liveness roots regenerated (+2: the entries),
`native_file_api_tests` owner-list registration, CMake (source + test).

Gates: 1 what_if (`--own 0x8038A2BC --own 0x8038A2C0`): the owners replace 2 frontier rows; no further row is freed
(the bodies are 4 and 8 bytes with no callees), pruned 1347 -> 1347; 2 contract vs the original:
`mix_entries_contract_tests` runs the original bodies from the DOL in the reference interpreter and the owner for 9
arguments x 2 r13 bases x both entries (36 cases), comparing all 32 GPRs, lr / ctr / cr / xer / pc and an 8 KiB window
of the SDA area byte for byte; negative control: an owner storing to sda+0x5DC4 instead is caught; the original is also
checked to change exactly one word. 3 behaviour, jobs=0, headless, alternating 2x2 Westopolis 90 s, exe without (cand10a)
vs with (cand10b) the owners on the merge of gc-removal 4aa07a5: PCM 015445f2 x4, state report byte-identical x4
(3,645,000,000 ticks, 2,697 VI, 5,360 render frames, peeks of 0x80000000 / 0x80560000 / 0x80580000 / 0x805F0000, which
includes the sound-mode word). 4 CTest (34 tests on the tests tree): 31 pass, the 3 known failures (shipping_platform_
selection, native_file_api_tests owner list, lift_sites_reference_tests). Performance: none (two calls per sound init).

## Step 7: RenderWare driver rwR1 device entries 0x804960FC, 0x804987C8, 0x80498854 (3 rows, exact owners) and the test literals

Survey of the RW driver rows main listed (callers from the plan, bodies read from the DOL with tools/doldis.py, hits from the
committed replay census test_data/platform/gx_rw_frontier.csv, which has no skinned / matfx coverage):

| row | what it is | called by (RW core) | census hits |
|---|---|---|---|
| 0x804960FC | camera view-matrix upload (RwMatrixMultiply into a stack temp, GXLoadPosMtx / NrmMtx / SetCurrentMtx) | pTank 0x80449D54, skin 0x8044EEC4, 0x80452C1C, default 0x80463964, 0x804A0B68, 0x804A62D0 | 0 |
| 0x804987C8 | mode setter: caches the alpha-compare / z-compare-location mode in the device record (+0x3C = 0x805E430C) | 0x80449D54, 0x8044EEC4, 0x80463964, 0x804A3F74, 0x804A4240, 0x804A4358 ... (11) | 0 |
| 0x80498854 | SubmitNoLightNode: texture bind 0x8049AC88, then the same mode logic from the raster flag | 0x804A0B68 | 0 |
| 0x8049AC88 | GX/TextureBind | dropped from this survey by main (axfx-effects) | 0 |
| 0x80494EB4, 0x8049BFE4 (plugin destruct), 0x8049C048 / 0x8049C11C (constructors), 0x8049C160, 0x8049C3A0, 0x8049C7BC, 0x8049C850, 0x8049CE40, 0x80465818, 0x804662B0 | RenderWare plugin registry / world-sector stream entries | 0x80487548 (registry add), 0x8049E120 / 0x8049E320, 0x804643AC.., 0x80464D78, 0x80462D14 ... | >0 (executed: 1..28) |

vi (rwR3 / rwR4 owner) confirmed it holds none of 0x8049BFE4..0x8049CE40: they are the "translated by design" rwR5 rows its test
pins (native_rw_world_reference_tests). They are RenderWare engine bodies (plugin registry callbacks, the world-sector reader),
executed in every recording, and each touches engine structures; owning them means reimplementing RenderWare, not the SDK, so
I left them (decision; the RenderWare reconstruction programme owns them).

Absence evidence is NOT used: the first draft made the three 0-hit rows stop owners (the census has no skinned / matfx path).
Main rejected that (a stop would turn an unrecorded path into a crash); they are exact owners, contract-tested:

- 0x804960FC: no guest-visible effect. RwMatrixMultiply 0x8047FC08 stores only through its out pointer (the stack temp), the
  three GX calls are the production silent owners; the owner is a silent return with r3 = 0 (the last call's argument).
- 0x804987C8: stores the mode into 0x805E430C when it differs; r3 as the original leaves it (r3 & 0xFF after a change).
- 0x80498854: if [[r13+0x620C]] is nonzero calls the texture bind 0x8049AC88(P, 0) through gekko::guest_call (stays a guest
  function), derives mode = (([R + [r13+0x6200] + 0x14] & 1) ^ 1) from the raster record and applies it as above.

Files: runtime/include/gekko/native_rw_device_state.h, runtime/src/gekko/native_rw_device_state.cpp (installed by
LiftedRender::install_owners), extract/native-owners-r2.txt (3 rows, lifted build), CMake, runtime/tests/
rw_device_state_contract_tests.cpp.

Test literals: runtime/tests/native_rw_world_reference_tests.cpp now carries `// liveness-roots: reference-interpreter-only`
(it only runs original words in the reference interpreter; it includes no recomp header), so its 12 RW addresses
(0x80465818, 0x804662B0, 0x80494EB4, 0x8049BFE4, 0x8049C048, 0x8049C11C, 0x8049C160, 0x8049C3A0, 0x8049C6C4, 0x8049C7BC,
0x8049C850, 0x8049CE40) and its heap literals leave extract/liveness-roots*.txt. Measured with what_if --drop-roots-from on
both the lifted (watch) and the full (tests) tree: frees 0 rows - those rows are live through their RW-core callers anyway,
so the test never pinned them (0x8049C6C4 is a test hook only; the plan's "runtime-literal" entry disappears from the
frontier list). Hygiene only, no row change. (guest_call_cases.h / 0x80492B04 is sdk-identify's.)

Gates: 1 what_if (--own the three rows, lifted tree): frees 0 further rows, pruned 1348 -> 1348 (the owners replace 3
frontier rows; their GX callees are silent owners already); 2 contract: `rw_device_state_contract_tests`, original bodies from
the DOL in the reference interpreter vs the owners, with the production silent GX owners on both machines and the texture
bind as one recording stub on both: 104 cases (mode setter 8 modes x 5 caches; SubmitNoLightNode [P] 0 / nonzero x 6 flag
words x 5 caches; view-matrix upload camera 0 / matrix x normal 0 / 1), comparing r3, r1, r2, r13, r14..r31, f14..f31, lr,
two memory windows (0x805D0000..0x80600000, 0x81300000..0x81310000) byte for byte and the stub's arguments and call count;
negative controls: skipped store, wrong stored value, missing texture bind, wrong raster bound, a store in the view-matrix
upload - each told apart; 3 behaviour, jobs=0, headless: Westopolis 90 s alternating 2x2 exe without (cand12a) / with
(cand12b) the owners on the merge of gc-removal 0aba489: PCM 015445f2 x4, state report byte-identical x4 (3,645,000,000
ticks, 2,697 VI, 5,360 frames, four 64 KiB peeks); the four 400 s recordings (final-haunt, lethal-highway-sfx,
user-westopolis-ramp, westopolis-cpu-spike), same pair each: PCM 5ae9f9c8 / 459b216c / fdae7920 / 6639c9f8 (as in step 3),
16,200,000,000 ticks, 11,997 VI, 23,960 / 23,959 / 23,960 / 23,960 frames, state reports byte-identical; 4 CTest on the tests
tree: rw_device_state_contract_tests, native_rw_world_reference_tests, platform_gc_removal_plan,
platform_runtime_coupling (the owner file's address uses are classified: owner-table, owner-hook, address-arith for the guest
call link), platform_retired_readers_*, native_liveness_roots(+lifted) pass; shipping_platform_selection,
native_file_api_tests, lift_sites_reference_tests are the known failures; rw_raster_contract_tests was not built in this tree.
Caveat: the recordings do not enter the three rows (census 0), so the behaviour gate shows nothing broke elsewhere; the
exactness of the owners rests on the contract test. Performance: none (rows never run in the recordings).

### Step 7b: 0x80498854 not owned (seam rule)

Main's rule: the guest-call seam is for RenderWare core only; the texture bind 0x8049AC88 is RW GameCube driver code. The
SubmitNoLightNode owner (its only guest call) is removed; 0x804960FC and 0x804987C8 stay (contract test trimmed to them: 44
cases, 3 negative controls; platform_runtime_coupling, retired_readers, liveness_roots, native_rw_world_reference pass).
The "table-format fix" of Step 7 was in my own owner header (the Address array is declared with `= {` and one row per
line, which is the layout tools/platform/runtime_coupling.py recognises as owner-table); tools/platform/runtime_coupling.py
was NOT changed. Observation: that tool's guest-call rule matches `call_guest*` and `call(kX` but not `guest_call(`, so a
`guest_call(...)` of a platform target is classified by whatever else is on its line (address-arith for `kX + n`); the
removed owner's call was classified address-arith, i.e. the coupling gate could not have caught it. RW-core guest_calls
(allowed by the seam rule) are equally invisible. Not fixed here (main's tool).

## Step 8: the RenderWare plugin-registry, device-pool and world-sector rows (exact owners, 16 entries)

Main's instruction: these rows are RenderWare driver code classified platform; they run in every recording; own them as exact native
C++, RenderWare core through guest_call, GC-hardware parts dropped with evidence. Files: runtime/include/gekko/native_rw_registry.h,
runtime/src/gekko/native_rw_registry.cpp (`rw_registry::install`, from LiftedRender::install_owners), extract/native-owners-r2.txt,
tests runtime/tests/rw_registry_contract_tests.cpp, rw_pool_contract_tests.cpp, rw_world_contract_tests.cpp.

| entry | what the original does (read from the DOL) | guest state it leaves |
|---|---|---|
| 0x80494EB4 (+ 0x80496440, 0x8049AC48 inlined) | RenderWare plugin init: creates the raster and texture free lists (RwCore 0x8048ACA4 / 0x8048E2D0) | r13+0x6200, r13+0x6218; returns 1 |
| 0x8049AB74 / 0x8049AB88 | the texture free list's element initialiser / terminator (RenderWare core calls them back) | element+[0x6218]+0x20 = 0x01000000; the 8 bound-texture slots 0x8056FCDC.. cleared where equal |
| 0x8049BFE4 / 0x8049C048 | driver plugin destruct / construct: instance pointer, offset and reference count (r13+0x6228/622C/6230), a 0x74-byte clear (memset 0x8000540C), two RW lists built or released (0x804A08FC/0A6C, 0x804A09E0/0998) | those words; 0x74 bytes of the instance |
| 0x8049C11C / 0x8049C160 (+ 0x8049C2A8, 0x8049C248 inlined) | device-pool construct / destruct: the pool's backing block (0x804A01B8), pool manager (0x80480C54) and its list init (0x804A02F4) / their release | r13+0x6238/6240/6244, manager fields +0x30/0x34/0x38 |
| 0x8049C7BC | device-pool object create: allocation through the engine hook [engine+0x118], clear 0x34 bytes; on failure RenderWare error 0x80000013 (0x8047EC78 / 0x8047EBD4) | the object |
| 0x8049C850 | device-pool object destroy: removes every entry (0x8049C3A0), frees its block, hands the object to [engine+0x11C] | entries, object fields +8/+0x20/+0x24 |
| 0x8049C3A0 (+ fragments 0x8049C434..C4CC) | entry remove: releases the entry's blocks through the engine free hook, compacts the three parallel arrays (0x28-byte entries, 0x80-byte blocks, 12-byte records), renumbers the indices other entries hold, decrements the count | the arrays, the count |
| 0x8049CE40 (recursive) | entry swap in the pool's priority structure: swaps two entries, their blocks and records, renumbers the indices, counts the entry and follows the indices it holds | the arrays, the count (r3 is not a result: both callers ignore it) |
| 0x80465818 / 0x80465800 / 0x80465810 / 0x80465814 | world plugin attach (registers a plugin with RwCore 0x8045BAF0) and its element destructor and two empty callbacks | r13+0x60D8; returns r3 >= 0 |
| 0x804662B0 (+ 0x80465AE4, 0x80465C10 inlined) | world-sector reader: chunk lookup, three 4-byte reads, the engine allocation, two block reads, the pointer relocation (an unrolled loop, 8 at a time), the object header, DCFlushRange + GXInvalidateVtxCache | the object (+0..+0x14, the relocated tables), [r4+0x34], halfword at header |

GC-hardware parts dropped: only DCFlushRange 0x80372534 and GXInvalidateVtxCache 0x80395D38 in the world reader; both are native
owners that return (extract/native-owners.txt: "No cache exists here"; gx_state: dead pipe) so the original's calls already did nothing.
RenderWare core reached through guest_call: the free-list / list / pool helpers, the engine allocator hooks, the stream functions and
the error functions; no SDK or driver function is called (the only callbacks passed as values are the owners' own rows).

Bugs the gates found (all fixed before commit): the registry test's stub log recorded stray argument registers (stubs now log only
the registers each callee takes); a negative control (an element terminator skipping the 8th slot) showed the compare window did not
cover 0x8056FCDC (window widened to 0x80560000..0x80600000); the Westopolis / recordings A/B stopped the machine at the first
stage load with "execution fault, a guest load from 0x34000000 in RwStreamRead": the world-sector owner built its temporaries
(stream tag, counts, version) in memory BELOW r1 and guest_call runs the callee with the current r1 as its frame top, so the callee's
frame overwrote them (the original's frame sits above its callees). Fixed with a Frame guard that lowers r1 for the body (the pool
create error path had the same defect); the contract tests' stubs now scribble the words below r1 like a real callee, which makes
that defect visible to them.

Gates: 1 what_if (lifted tree, before the owners): owning the nine rwR5 rows frees 23 rows (12 layer:rw RenderWare-core rows stay
live because they are guest_call targets; 11 platform driver rows pruned); the eleven rows with the world pair free 26 in the
what_if; 2 contract vs the original: rw_registry_contract_tests 231 cases, rw_pool_contract_tests 223 generated pool states (0
skipped, memory window and r3 / r1 / r2 / r13 / r14..r31 / lr / f14..f31 compared, four negative controls), rw_world_contract_tests
70 cases (scripted stream chunk, version, tag, counts, short reads; two negative controls); 3 behaviour, jobs=0, headless, on the
merge of gc-removal round 12: alternating 2x2 Westopolis 90 s without / with the owners: PCM 46a70a10 (the new default canonical)
x4, state reports byte-identical; the four 400 s recordings (final-haunt 99d7fcfc, lethal-highway-sfx ef34d4c7, user-westopolis-ramp
131d24e6, westopolis-cpu-spike 13e61bd5), each pair byte-identical (16,200,000,000 ticks, 11,997 VI, 23,959-23,960 frames) for
the registry+pool owners; and, after the frame fix, again for the world-sector owner (cand13b vs cand15); 4 CTest after the
change: rw_registry / rw_pool / rw_world / rw_device_state / mix_entries contract tests, native_rw_world_reference_tests,
platform_gc_removal_plan, platform_retired_readers_*, lift_l1_evidence, native_liveness_roots (+lifted) pass;
platform_runtime_coupling (before the round-15 merge; 5 native -> SDK sites, within the ratchet): the owners' link / value addresses are classified address-arithmetic (no row is called by them);
the remaining failures are shipping_platform_selection, native_file_api_tests (owner list), lift_sites_reference_tests (known).

After the final merge of gc-removal round 15 (a93f6de, roots regenerated) nothing was rebuilt or re-gated here; main's integration build
re-gates it.

## Step 9: GcnNativeTextureWrite 0x8049A604 (exact owner, contract-tested)

Survey result for the remaining graphics.rw-gamecube-driver frontier rows on the merged tree (gc-removal round 15, plan: 11 rows,
71 bodies freed if all owned): all but one are held. vi: GcnSystem 0x80494EDC and ShowRaster 0x80495CBC (the display-swap unit,
docs/agents/vi.md Step 12, waits for cri-threads' sleep / wakeup), GcnRasterGetSize 0x8049A57C (with 0x804965A8 / 0x80492B04, agent/vi
f09fbbc), the Render wrappers 0x80499A9C / AC4 / AEC (they only call 0x80499924, owned by os-other). card-saves: RasterCreate
0x80497CA8 and the raster life cycle (agent/card-saves 29e52a5). sdk-identify: GcnNativeTextureRead 0x8049A8DC (parked) and RwImageGetRaster.
axfx-effects: TextureBind 0x8049AC88; 0x80498854 follows it. Released to me by sdk-identify and unclaimed by the others:
**GcnNativeTextureWrite**.

0x8049A604 serializes a texture to a RenderWare stream: RwStreamWriteChunkHeader (0x8047DF40), the texture header (filter bits,
the GX texture object's wrap modes, mipmap flag and LOD bias, the two 0x20-byte names), the raster header (type, format, size,
depth, the raster filter query 0x8048AD34), the palette, the pixel size and the pixel block (RwStreamWrite 0x80481F20). The four
GX getters it calls (0x803982AC wrap S, 0x8039828C wrap T, 0x80398298 mipmap flag, 0x8039824C LOD bias) are bit extractions of the
texture object's first word and are inlined (the LOD bias with the original's double-magic conversion and the constants it loads
from the read-only small data). The raster size row 0x804965A8 is entered through its native owner (Cpu::serve_owner; it is vi's on
agent/vi): with no owner installed the run stops with that row's name. Nothing in the game writes textures: 0 census hits in every
recording. The owner holds a frame (the stream functions are handed pointers to its temporaries).

Contract: `rw_texture_write_contract_tests` (file name chosen so as not to collide with sdk-identify's stash): 90 seeded states
(texture / raster fields, 7 formats, palette and no palette, six depths, GX word variants with and without the 0x01000000 flag, four
raster sizes, a failure injected at each of the five stream writes and at the chunk header), the stream-write stub captures every
buffer so the written BYTES are compared, plus r3 / r1 / r2 / r13 / r14..r31 / lr / f14..f31 and both memory windows; two negative
controls (writes nothing; chunk header only + reported success) are caught. The stubs scribble below r1. CTest: the five RW contract
tests, platform_gc_removal_plan, platform_runtime_coupling (clean), native_liveness_roots (+lifted) pass. what_if: this row alone
frees 0 rows (the getters and 0x804965A8 stay live for the other rows); together with 0x8049A57C, 0x8049A8DC and 0x80497CA8 the plan
frees 21 rows, so the payoff comes when the four owners are merged. No behaviour gate: no recording runs the row; the gate is
the contract test.

## Step 10: RenderWare vector / matrix math (native math programme, docs/NATIVE_MATH_METHOD.md item 4)

Assigned by main: the RenderWare side of "lightweight native math, identical behaviour, no obsolete behaviour" (the native-math session
holds the game-side math; it holds no RW row). Existing: RwMatrixMultiply 0x8047FC08 already has the matrix_multiply_intrinsic owner
(docs/CPU_FINAL_HAUNT.md); source/rwcore has the proven C++ ports of the rest.

Call counts (GEKKO_CALL_CENSUS_COUNTS, declining hooks, Westopolis 90 s tape = 3.64 G ticks | Final Haunt 4.86 G ticks):

| row | what | Westopolis | Final Haunt |
|---|---|---:|---:|
| 0x804803E8 | RwMatrixScale | 864,480 | 1,528,555 |
| 0x804805EC | RwMatrixTranslate | 770,463 | 1,380,085 |
| 0x804834A8 | RwV3dTransformPoint | 488,873 | 1,073,660 |
| 0x80483554 | RwV3dTransformVector | 341,468 | 610,352 |
| 0x80480100 | RwMatrixInvert (744 B) | 341,468 | 610,352 |
| 0x80483390 | _rwSqrt | 273,427 | 737,709 |
| 0x8048077C | RwMatrixTransform | 218,860 | 804,389 |
| 0x8047FD24 | RwMatrixRotateOneMinusCosineSine | 184,400 | 286,482 |
| 0x80483440 | RwV2dLength | 170,734 | 305,176 |
| 0x804833E4 | _rwInvSqrt | 167,438 | 325,062 |
| 0x8047FBF0 | RwMatrixUpdate | 137,780 | 301,789 |
| 0x80480018 | RwMatrixRotate | 134,905 | 242,882 |
| 0x8047F9E0 | RwMatrixOptimize | 104,909 | 138,101 |
| 0x8047F754 / F7A0 / F7F8 | determinant, orthogonal / normal error | 1,111 each | 1,244 each |
| 0x8048327C, 0x80483350 | V3dNormalize, V3dLength | 2,636 / 70 | 0 / 150 |
| 0x804834F4 | RwV3dTransformPoints | 0 | 0 |

### 10a: RwV3dTransformPoint 0x804834A8, RwV3dTransformVector 0x80483554, RwV3dTransformPoints 0x804834F4 (served natively)

`gekko::rw_vector` (runtime/src/gekko/native_rw_vector.{h,cpp}, installed by System next to the matrix-multiply intrinsic;
`GEKKO_NATIVE_RW_VECTOR=0` runs the guest bodies for A/B). The originals are leaf paired-single bodies; the owner loads the three floats and
the matrix rows (psq_l, GQR0 float), forms each component with the CPU model's own helpers in the original's order (point: right * x + pos,
then up * y + that, then at * z + that, each one fused rounding to single; vector: right * x as a product first) and stores three floats. It keeps
no paired-single register, GQR, CTR or temporary state (volatile f0..f11, r4..r12, CTR, CR are left as they were: nothing reads them after a leaf
call). The FPSCR sticky status bits the original sets are not modelled: the only readers of FPSCR in the DOL are PPCMffs / OSSaveContext /
MSL fenv (mffs at 0x80370B60, 0x80372A50, 0x8039DD4C; no mcrfs), so nothing inspects them. The array form reads element i + 1 before it stores
element i as the original's pipeline does (an output overlapping the next input element sees the old value); count 0 (the original loops 2^32
times) stops by name. The owner declines (the guest body runs) when the FPU is disabled or GQR0 is not the float format.

A finding of the fuzz test: with FPSCR[NI] = 0 the CPU model's psq_l still reads a subnormal single as zero, so the first draft (plain host
floats) differed from the original; the owner now uses the model's own load / store conversions and ppc::fps_mul / fps_madd.

Gates: contract `rw_vector_contract_tests`: 1,680 original-vs-native cases (random bit patterns from every float class: normals of both signs,
zeros, subnormals, products near the 2^-126 underflow boundary, infinities, quiet and signalling NaNs with payloads, extremes; both NI settings;
out = in, out overlapping the next component, out inside the matrix, counts 1..9 with out = in, in + 12, in + 4 and overlapping the matrix),
r3 / r1 / r2 / r13 / r14..r31 / lr / f14..f31 and a 4 KiB window compared; a count of 0 stops by name; two negative controls (unfused chain,
wrong order) are caught. Behaviour (jobs=0, headless, one executable, GEKKO_NATIVE_RW_VECTOR=0 vs default): Westopolis 90 s alternating
2x2: PCM 46a70a10 x4, state reports byte-identical, the owners served 488,873 point and 341,468 vector transforms per run; the four 400 s
recordings (final-haunt, lethal-highway-sfx, user-westopolis-ramp, westopolis-cpu-spike): PCM 99d7fcfc / ef34d4c7 / 131d24e6 / 13e61bd5, each
off / on pair byte-identical. Game-thread CPU (host.guest_run_cpu_seconds, alternating A B A B): Westopolis off 10.61 / 10.84 s vs on 10.36 / 10.30 s
(about -3 %); recordings (one pair each, so not conclusive): haunt 53.66 -> 52.89, lethal 53.73 -> 55.14, ramp 34.41 -> 34.39, wcs 56.25 -> 55.61.
Not run, with the reason: the old-loader mode (GEKKO_NATIVE_BG_READ=0 GEKKO_NATIVE_EXPAND_JOBS=0 GEKKO_NATIVE_ONE=0, canonical 015445f2) and the strict
oracle in mode B (--verify-player-strict, SHADOW_NATIVE_CRI=0, SHADOW_ERRATA=0) both stop at 1 s on this tree with "os.threads: OSYieldThread /
OSSleepThread with a second guest thread" (the guest-thread paths were removed by cri-threads), identically with the owners off and on, so neither can
be a gate here; the strict scope file is the loader ranges, not RenderWare math. The five PNG baselines were not captured in this step.

### 10b-10f: RwMatrixInvert, Scale, Translate, Transform, RotateOneMinusCosineSine, Update, Optimize and the square-root family (served natively)

`gekko::rw_matrix` (native_rw_matrix.{h,cpp}; `GEKKO_NATIVE_RW_MATRIX=0` runs the guest bodies) and the square-root family in `gekko::rw_vector`
(`GEKKO_NATIVE_RW_VECTOR=0`). Rows: RwMatrixInvert 0x80480100, RwMatrixScale 0x804803E8, RwMatrixTranslate 0x804805EC, RwMatrixTransform 0x8048077C,
RwMatrixRotateOneMinusCosineSine 0x8047FD24, RwMatrixUpdate 0x8047FBF0, RwMatrixOptimize 0x8047F9E0, _rwSqrt 0x80483390, _rwInvSqrt 0x804833E4,
RwV3dLength 0x80483350, RwV2dLength 0x80483440, RwV3dNormalize 0x8048327C. Each owner runs the original's chain of single-precision operations in its
order with the CPU model's own helpers (ppc::fps_*; lfs / stfs conversions, stfs without the psq_st flush), so NaN selection, the FPSCR[NI] policy
and rounding are the guest instructions'. The square-root rows are integer table lookups on the single's bit pattern (tables the engine builds, reached
through r13 + 0x615C / 0x6140); RwV3dNormalize declines before it changes anything when the length is not above zero (the guest body runs the RenderWare
error path). Transform and Rotate-One-Minus-Cosine-Sine multiply through RwMatrixMultiply with `guest_call` (a RenderWare core function) on a private frame.
Declined (the guest body runs): FPU disabled, GQR0 not the float format, an operation outside 0..2 (the original raises a RenderWare error).
Not modelled (and not read by anything): the volatile registers, the sticky FPSCR bits, the stack residue of the original's frames, and the three pad
words (+0x1C, +0x2C, +0x3C) of a matrix that Transform and Rotate leave holding stale stack bytes in the original: the owners do not write them
(replicating stale stack bytes would be garbage replication). What is kept: the second slot of f29..f31 (the original's psq_st / psq_l save pair rounds it to
a single), found by the contract test.

Contract tests (reference interpreter, pinned DOL, original body vs owner on byte-identical memory, 4 KiB window + r3 / r1 / r2 / r13 / r14..r31 / lr /
f14..f31 both slots, f1 for the sqrt family): `rw_matrix_contract_tests` (Invert, Scale, Translate, Transform, Rotate, Update, Optimize: every flags
value, optimisations word, destination overlap, operation 0 / 1 / 2, both NI settings, near-identity / rotation / ordinary / random-class matrices, random
tolerances, default tolerances; eight negative controls, each run on its own entry, all caught) and `rw_vector_contract_tests` (1,680 transform cases +
the sqrt family: any double, widened singles, table words random; four negative controls for the square-root and transform owners). Both pass.
Findings the fuzz made: stfs does not flush a subnormal while psq_st does; the reciprocal zero test needs the no-flush conversion; the identity copy
needs the original's interleaved load / store schedule under overlap; the f31 second slot.

Behaviour, candidate 18 (all of the above; headless, jobs=0, `GEKKO_NATIVE_RW_MATRIX=0 GEKKO_NATIVE_RW_VECTOR=0` vs default, one executable):
Westopolis 90 s alternating 2x2: PCM 46a70a10 x4, the four peeked guest blocks (0x80000000, 0x80560000, 0x80580000, 0x805F0000, 64 KiB each) identical in
every pair. The four 400 s recordings (final-haunt, lethal-highway-sfx, user-westopolis-ramp, westopolis-cpu-spike), off / on: PCM 99d7fcfc / ef34d4c7 /
131d24e6 / 13e61bd5, each pair identical. Pixels (SHADOW_R2_MSAA=1, headless, `--capture-at-tick` 5e6 / 2e8 / 9e8 / 2e9 / 3.5e9, `--stop-after-capture`):
owners off and on give byte-identical PNGs at all five ticks (22fdf629, 7bfd6854, c44bde60, d471361d, 0b42b725). The baselines quoted for this gate
(22fdf629, d22a1750, b6511d3d, 37299f60, ae967db4) match only at the first tick: the other four differ with the owners OFF as well, so that difference is
the capture configuration (flags / environment of the baseline runs), not these owners. Game-thread CPU (guest_run_cpu_seconds, the machine was not quiet:
other sessions were building): Westopolis 13.80 / 13.47 s off vs 14.25 / 13.47 s on (no measurable difference at this noise); recordings, single pairs:
haunt 65.1 -> 62.5, lethal 62.3 -> 55.5, ramp 36.3 -> 37.4, wcs 54.4 -> 57.0 (not conclusive; an earlier quieter candidate-17 run read Westopolis
-4 %).
State report: the per-block memory hash differs off vs on in 22 blocks in both pairs, the peeks do not. With only the vector owners on (candidate 17,
`GEKKO_NATIVE_RW_MATRIX=0`) twelve peeked blocks were identical; with the matrix owners 113 words differed, 57 of them pad-word triples (+0x1C / +0x2C / +0x3C
of a matrix, the off value a stack pointer: the stale bytes the original copies), the others at pad-word offsets of further matrices and two 15-word runs
(0x805B3640.., 0x8060C3B0..) that are the original's own frame temporaries. Read-free evidence, by perturbation instead of the multi-hour page watch
(which was abandoned): a probe build (not committed; the owners wrote the NaN marker 0x7FC0DEAD into the three pad words of every matrix Transform and Rotate
produce, `GEKKO_RW_PAD_POISON=1`) against the same build without the marker: Westopolis 90 s and the four 400 s recordings give identical PCM (46a70a10,
99d7fcfc, ef34d4c7, 131d24e6, 13e61bd5), identical game ticks / render frames, identical peeked blocks and no differing memory block outside the heap; the
9e8 and 3.5e9 PNGs stay equal to the baselines; and a sweep of 38 stage boots (every `--boot-stage` id the game accepts, 25 s of game time each,
marker vs no marker) is identical in PCM, frames and peeks in all 38 (stg0700 / stg0710 are not Select Mode stages). So nothing those paths reach reads a
pad word; it covers only the paths those runs reach, as stated.
Not run, with the reason: the old-loader mode and the strict oracle (they stop at 1 s on this tree: "os.threads: OSYieldThread / OSSleepThread with a
second guest thread", with the owners off and on alike).

## Step 11: the driver camera view-matrix cache, rows 0x804759B4 and 0x80475A1C (exact native code on the device-state words)

Identified from their instructions (neither is RenderWare core: both work on the GameCube driver's device-state global 0x805E2A10; RenderWare core
is only what they call): 0x804759B4 (5 call sites) translates the current matrix of the driver's matrix stack ([+0x244] indexes the pointers at
+0x1C4) by {f1, f2, 0} (pre-concatenate) and clears the two cache flags +0x318 and +0x32C; 0x80475A1C (4 sites) rebuilds the cached view matrices
when +0x318 is clear (current * view matrix at +0x184, scaled by {s, s, 1}, scaled by {-m, m, 1}, translated by {0.5 * a * s, -(0.5 * b * s), 0}; the 16
words go to +0x258, their inverse to +0x298, +0x318 is set) and always forms +0x2D8 = (+0x258) * the camera frame's LTM (RwFrameGetLTM 0x804882E8), returning
&obj[0x2D8]. `gekko::rw_view_cache` (native_rw_view_cache.{h,cpp}; `GEKKO_NATIVE_RW_VIEW=0` runs the guest bodies): the single-precision parts are the
original's chain on the CPU model's helpers (lfs / stfs conversions, fmuls, the fcmpo / bge minimum with a NaN taking the second operand), the RenderWare
core callees (RwMatrixMultiply, Scale, Translate, Invert, RwFrameGetLTM) run through `guest_call` on a private frame, the original's dead frame
temporaries and the stale pad words they leave in the cached matrices are not reproduced, and f30 / f31 keep the original's psq save-pair rounding of
their second slots. `rw_view_cache_contract_tests` (original DOL vs owner, device block + 4 KiB window + r3 / r1 / r2 / r13 / r14..r31 / f14..f31 both
slots; matrix stack index 0..3, dirty flag 0 / 1, camera projection type 1 / 2 / 3, random and ordinary matrices of every flag word, random extents and
scale, f1 / f2 as widened singles or arbitrary doubles, both NI settings): all cases pass, three negative controls (never sets the dirty flag, leaves the
second flag, returns the wrong matrix) are caught.

Behaviour, candidate 20 (everything above, `GEKKO_NATIVE_RW_MATRIX=0 GEKKO_NATIVE_RW_VECTOR=0 GEKKO_NATIVE_RW_VIEW=0` vs default, headless, jobs=0): Westopolis
2x2 PCM 46a70a10 x4, peeked blocks identical; the four recordings off / on PCM 99d7fcfc / ef34d4c7 / 131d24e6 / 13e61bd5, identical per pair; the five PNGs
WITHOUT SHADOW_R2_MSAA (5e6 / 2e8 / 9e8 / 2e9 / 3.5e9 ticks) are 22fdf629 / d22a1750 / b6511d3d / 37299f60 / ae967db4, the integration baselines (the earlier
runs with SHADOW_R2_MSAA=1 gave 22fdf629 / 7bfd6854 / c44bde60 / d471361d / 0b42b725 with owners off and on alike: the environment variable, not the owners,
changes four of them). Owner hits in a 90 s Westopolis run (the census native-math measured): Scale 864,480, Translate 770,463, Invert 341,468 (= the
view cache's 341,468 calls of each row), Rotate-One-Minus-Cosine-Sine 184,400, Optimize 104,909, Update 137,780, Transform 218,860, V3dTransformPoint
488,873, _rwSqrt 270,721, _rwInvSqrt 164,802, V2dLength 170,734. Game-thread CPU (guest_run_cpu_seconds, alternating off / on): Westopolis 9.22 / 8.94 s off vs
8.86 / 8.86 s on (-3 to -4 %); recordings (single pairs) haunt 45.8 / 45.8, lethal 51.2 / 50.8, ramp 31.5 / 32.6, wcs 51.0 / 50.7 (not separable from noise).
State report: peeks identical, 23 memory blocks differ (the pad words and dead frames above, which the perturbation probe shows nothing reads).
Not done: the old-loader and strict-mode-B gates wait for cri-threads' 92c3c6c to be merged; the decline paths and the `GEKKO_NATIVE_RW_*` switches stay
until cri-threads' cut 3 keeps MSR[FP] set and a census shows GQR0 never changes (main's condition).

## Step 12: the decline paths and the `GEKKO_NATIVE_RW_*` switches are gone from the math, vector and view-cache owners

Condition (main's): cri-threads' cut 3 keeps MSR[FP] set, and a census shows GQR0 never changes. Both hold in the single-thread configuration
(`os_threads::single_thread_configuration()`, true by default; the lifted build requires it): cut 3 never clears MSR[FP], and the GQR0 census found
only two guest writers in the DOL, 0x80371738 (writes 0) and 0x8039F768 (the TRK context restore), both pruned in the lifted build; native_interrupts
restores GQR1..7 only. The census is static (a DOL mtspr/mfspr scan plus the runtime sources); a dynamic confirmation is the loud stop below, which
no gate run hit.

Change: the owners no longer decline when the FPU is off, GQR0 is not the float type, or the matrix op is outside 0..2 (cases the contract excludes).
`gekko::rw_refusal::refuse` (native_rw_refusal.h) stops the CPU with `StopReason::ExecutionFault` naming the owner in the single-thread
configuration, and still returns false (the guest body runs) in the reference / guest-thread configuration, where the lazy FPU switch is kept.
RwV3dNormalize's zero-length path is an error path of the original and stays a decline. The `GEKKO_NATIVE_RW_MATRIX/VECTOR/VIEW` getenv switches are
removed. Sixteen rows were added to `extract/native-owners-r2.txt` (the lifted build emits no guest body for them).

Ran: `rw_matrix_contract_tests`, `rw_vector_contract_tests`, `rw_view_cache_contract_tests` 3/3 pass; `native_liveness_roots`,
`native_liveness_roots_lifted`, `platform_gc_removal_plan`, `platform_runtime_coupling` 4/4 pass; the shipping lifted build links (about 8.8 % CPU
under the cap); candidate 21 gates: Westopolis PCM 46a70a10, the four recordings 99d7fcfc / ef34d4c7 / 131d24e6 / 13e61bd5, the five PNGs
(without SHADOW_R2_MSAA) 22fdf629 / d22a1750 / b6511d3d / 37299f60 / ae967db4, all equal to the baselines, no refusal stop.
Not run: strict mode B (SHADOW_ERRATA=0 reference build) and the old-loader gate, both unblocked now that 92c3c6c is an ancestor; no gameplay,
audio or GPU claim beyond the headless gates above.
