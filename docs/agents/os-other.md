# os-other (agent os-other, branch agent/os-other)

Scope: removal-plan service os.other (20 frontier rows, 45 bodies). Step 1 of this report: group B, the console
settings accessors.

## Row classification (from the removal plan, wt-gc-removal removal_plan.json)

| group | rows | disposition |
|---|---|---|
| B settings | OSGetSoundMode 0x80377DA0, OSSetSoundMode 0x80377E20, OSGetLanguage 0x80377EC4, OSGetProgressiveMode 0x80377F30, OSSetProgressiveMode 0x80377FA0 | owned here (this step) |
| A heap | OSAllocFromHeap 0x80371FB0, OSFreeToHeap 0x803720AC, OSCreateHeap 0x803721A8, OSInitAlloc 0x80372138, OSSetCurrentHeap 0x80372128 | owned here (step 2, below) |
| crash text | 0x8037324C (game 0x80050FF4) | mine after A |
| C boot | 0x80370CDC, __OSPSInit 0x80371714, __OSCacheInit 0x80372838, OSGetConsoleType 0x80370E04, 0x803716BC, 0x8037829C, OSInitBATs_Real 0x80376A48, 0x80376AC8, data word 0x80378320 | handed to native-boot |

Names come from extract symbols / the call graph; the entries are game code only (801F21AC, 801F2E58, 8035AA28,
8035B414, 803060AC, 80306100, 801DFA98, 80051684, 803070B4, 8030703C).

## Group B: what the originals do and what is owned

Each original takes the SRAM lock of the OS control block (Scb 0x80586FE0: Scb.enabled = interrupt level, Scb.locked =
1), reads or changes one field of the cached SRAM (+0x11 bit 0x40 progressive, +0x12 language, +0x13 bit 0x04 sound
mode) and releases it through UnlockSram 0x803778E8 (already a NativeIpl owner): getters commit = 0, setters commit = 0
when the value is unchanged, else commit = 1 offset 0 after storing the field. The SRAM image itself is NativeIpl's
settings image, which already carries the PC settings (language, sound mode).

Owners: `runtime/src/gekko/native_os_settings.{h,cpp}` (`gekko::os_settings::install`, called in System right after
`ipl_->install()`; it finds the UnlockSram owner among the installed entries and calls it for the release, so stats and
persistence stay NativeIpl's). Same field reads/writes, same Scb.enabled value, same UnlockSram arguments, no
OSDisableInterrupts/OSRestoreInterrupts pair (net MSR[EE] unchanged). A held lock (the original reads through a null
base register) stops the run instead. The setters' r3 is UnlockSram's result (Scb.sync): the setters' callers pass it
on (0x80306140 calls 0x8005328C without reloading r3), so it is kept.

Files: native_os_settings.h/.cpp, os_settings_contract_tests.cpp (new); small edits to the shared lists:
extract/native-owners.txt (5 rows + comment), CMakeLists.txt (source + test target), system.cpp (one call), system.h
(include), native_file_api_tests.cpp (owner list).

## Gates

| gate | evidence |
|---|---|
| 1 what it frees | what_if (generator lock), release dir wt-gc-removal/.build/watch: 0 rows freed, 0 revived, 0 stale sites, with and without the rows. The five are leaf accessors (their callees OSDisable/Restore and UnlockSram stay native or shared). Their gain is dropping five bodies of game-entered frontier rows: the original bodies are pruned (a run of the baseline image without the rows has the bodies; an image with the rows but without the install stops with "native owner 0x80377da0 is not installed"). |
| 2 contract | os_settings_contract_tests: the original DOL instructions vs the owners over 9,920 calls (every flags byte class x progressive byte x language x interrupts on/off x argument): result register, the 0x50 control-block bytes, the UnlockSram (commit, offset) sequence and MSR[EE] equal; a held lock is refused. Negative control: the sound-mode mask changed to 0x08 fails with "result register differs"; restored passes. UnlockSram is the same recording stub on both sides (the NativeIpl owner has its own tests) |
| 2 no surviving reader | guest watch over Scb + 0..0x50 (candidate image, headless, Westopolis tape, 60 s): all accesses come from native code (the owners, NativeIpl, boot memset, save store/state report); no translated game function touches it. The owners keep writing Scb.enabled / Scb.locked, so nothing is retired here |
| 3 game behaviour | Westopolis 90 s, baseline (owner rows and install removed, regenerated) vs candidate, base,cand,base,cand: PCM 015445f2... in all four, 3,645,000,000 ticks, 2,697 VI / 5,360 render frames; every 64 KB memory block identical in all pairs (A/A, B/B, A/B). Both exes in .build/ab-jobs (base.exe, cand.exe); the candidate was rebuilt once and not byte-identical to the first build (timestamps), cand.exe is the one run |
| 4 tests | native_file_api_tests still fails with the same 66 baseline mismatches (CRI owners, 0x8037621x); none of the five new rows is among them |
| ratchet | unchanged by instruction; measured body counts not taken (0 rows freed) |

Performance: not measured (no claim). CPU of the capped runs: builds 8.4-9.0 %, game runs about 3 %.

## Open

0x8037324C next, then whatever os.other rows remain once native-boot owns OSInit. The boot/exception rows went to native-boot.

## Step 2: heap group A (OSInitAlloc, OSCreateHeap, OSSetCurrentHeap, OSAllocFromHeap, OSFreeToHeap)

Owners: `runtime/src/gekko/native_os_heap.{h,cpp}` (`gekko::os_heap::install`, in System after the settings owners). The heap
descriptors, cells and the free / allocated lists are guest memory (the game stores and compares the pointers), so the
owners work on those words exactly as the originals and return the original result in r3 (init: the arena start, create:
the heap index or -1, set-current: the old heap, alloc: cell + 0x20 or 0; free returns nothing). They keep nothing else:
no stack frame, no scratch register, no CR / CTR (first version replicated them; removed on the main session's review: no
caller can rely on them, and no OSFreeToHeap caller reads r3 afterwards: RwFree 0x80050F58 clobbers it at once,
__sys_free 0x803A3A2C and the SDK wrapper 0x8038A118 return void). The compares keep their signedness (a request above
0x7FFFFFE0 wraps and fits the first cell, like the original). OSFreeToHeap carries its only callee 0x80371F04 (the
free-list insert) inside. Not kept: an endless loop on a cyclic list (the owner counts steps and stops the run with an
execution fault past 2^24). The runtime-literal entries 0x803A3994 / 0x803A3A4C are MSL guest code that call these rows;
they keep running and call the owners, nothing to move.

| gate | evidence |
|---|---|
| 1 what it frees | what_if os-heap (generator lock, release dir wt-gc-removal/.build/watch): frees 1 row, revives 0, 0 listed sites become work items. The gain is the five bodies of game-entered frontier rows plus the helper 0x80371F04. |
| 2 contract | os_heap_contract_tests vs the pinned DOL in the reference interpreter: 33,465 calls, 66,942 comparisons, PASS. After every call r3 (not for OSFreeToHeap), the preserved registers (r1, r2, r13-r31, LR) and every byte of the heap, the descriptors and the small-data cells equal the original; scratch registers, CR, CTR and the dead stack are not compared. Shapes: the game's one-heap shape (128 KiB x3 seeds with every byte compared after every call, 8 MiB), three heaps with a refused fourth create, descriptor counts 0 / -1 / 1 / 2, unaligned bounds, 17,161 allocations (1,146 refused, 191 aimed at exact fits, 1,239 at the 0x40 split boundary), 14,839 frees, wrapped sizes 0x7FFFFFE0..0xFFFFFFFF. Negative control: the size rounding changed 0x3F -> 0x3E fails ("the heap memory differs after 80371FB0"); restored passes. |
| 2 no surviving reader | not applicable: the heap is game-visible state the owners keep reading and writing; nothing is retired. |
| 3 game behaviour | Westopolis 90 s game time, baseline (heap rows and install removed, regenerated, heap-base.exe) vs the results-only candidate (heap-cand2.exe), base,cand,base,cand: all four state reports are byte-identical (same hash): 3,645,000,000 ticks, 2,697 VI / 5,360 render frames, every 64 KB memory block equal (so no differing stack word either); PCM 015445f2... in all four. Nothing to classify. (The first, register-exact version gave the same result.) |
| 4 tests | native_file_api_tests owner list includes the five rows (os_heap::Owners). |
| ratchet | unchanged by instruction; body counts not taken (what_if: 1 row freed). |

Performance: not measured (no claim). The allocators were 0.04 % of game-thread samples before (docs/MEMORY_ALLOCATORS.md
section 5). CPU of the capped runs: builds 8.3-8.6 %, test runs about 3 %.

## Step 3: OSPanic 0x8037324C

Original: masks interrupts, prints the message (vprintf), " in \"%s\" on line %d.
", the frame header and up to 17 stack
rows through OSReport (the debug UART, absent on a retail console), then PPCHalt 0x80370B1C (`sync`, endless loop): it never
returns. The game reaches it from one place: 0x80050FF4, called by the RenderWare event handler at 0x80051A9C with file
cell r2-0x79A8, line 0x318 and the message "The End." - the program's own end.

Owner: `runtime/src/gekko/native_os_panic.{h,cpp}` (`gekko::os_panic::install`). It stops the machine naming file, line
and message: StopReason::HleExit (the program ends, like OSResetSystem in native_reset.cpp) when the return address is
the one inside the game's end routine (0x80051018), StopReason::ExecutionFault for any other caller (none exists in the
game). The message's format string is quoted as it stands.

| gate | evidence |
|---|---|
| 1 what it frees | what_if os-panic: frees 0 rows, revives 0, 0 listed sites become work items. The gain is dropping the body of a game-entered frontier row (the print / halt machinery stays: OSReport and the C library are shared). |
| 2 contract | os_panic_contract_tests vs the pinned DOL: 576 variations (4 return addresses x 4 lines x 3 file / message pairs x 6 stack-chain lengths x 2 chain terminators), PASS. The original (vprintf / OSReport replaced by recording stubs, the rest real) prints the message through vprintf, reports file and line, prints the header and 1-17 frame rows, then reaches PPCHalt and stays in its loop; the owner stops with HleExit exactly for LR 0x80051018 (144 variations) and a fault otherwise, the detail naming file, line and message. Negative control: the return-address rule inverted fails ("HleExit differs from the return address rule"); restored passes. |
| 2 no surviving reader | not applicable (no state). |
| 3 game behaviour | not reachable in a replay: the only caller is the game's end, after which the console halts. No A/B possible; the Westopolis A/B above ran with the owner installed (candidate heap-cand2.exe also installs it, its native-owners row was added after that build) and is identical to the baseline. |
| 4 tests | native_file_api_tests owner list includes os_panic::Owners. |
| ratchet | unchanged by instruction. |

## Step 4: OSReport (withdrawn) and a roots fix

OSReport was started here and stopped on the main session's instruction: agent/axfx-effects 2b0e093 owns OSReport, printf and
fprintf with its own gates, so the work here was discarded uncommitted.

structure_match mechanical candidates for os.other (empty bodies, twins with similarity 0.9 or more): only 0x80378320
(an empty body in the boot group, native-boot's) and 0x80372128 (already owned with the heap). The queue has nothing left.

Fix found on the way: the contract tests os_heap / os_panic / os_settings declared "liveness-roots: reference-interpreter-only" inside
a sentence on a comment line. The roots tool accepts only the exact line, so those tests wrongly rooted the original
bodies they only run in the reference interpreter (0x803AA414 vprintf, PPCHalt 0x80370B1C, 0x8037AF54, 0x80051014 and the
format strings). The marker is now its own line; extract/liveness-roots.txt and -lifted.txt are regenerated with the
test's exact arguments (both --check pass; 9 roots fewer). Not game-validated: dropping a root can only remove bodies nothing
else reaches; the main session's integration build covers it.

## Step 5: the three frontier rows of os.other (removal_plan.py on gc-removal 1858197)

removal_plan.py (fresh run, generator lock) lists **3** os.other frontier rows (the "15" is the number of SDK bodies owning them frees,
cumulatively with the plan's other counts). Analysis of each:

| row | what it is | decision |
|---|---|---|
| 0x80370E04 OSGetConsoleType | leaf: r3 = word at (r13+0x5A20)+0x2C, or 0x10000002 when the pointer or the word is 0. Callers 0x803AF5D4, 0x803B4130, 0x803B57FC are all debug-console code. | **owned** (native_os_misc) |
| 0x80378320 | a lone `blr`, entered through the data word at 0x805EEC10 (DVD file-system static data, next to the "dvdfs.c" string) | **owned** (native_os_misc; returns at once) |
| 0x803716BC | the exception default handler: stores r0-r2, r6-r31, SRR/GQR words into the context at r4 and continues in 0x80373590, the full OSUnhandledException: it calls a user handler from the table 0x80586CB0 through a guest `blrl` (OSInit installs index 1 = 0x803726D8, a varargs dump routine), switches context through 0x80372C68, prints the dump with OSReport and ends in PPCHalt | **not owned**: the user-handler branch needs a native-to-guest call (none exists) and the dump text belongs to the OSReport / printf work of agent/axfx-effects; a fatal-stop-only owner would change the handler-registered and FP-exception behaviour, which I cannot prove |

Item 1 of the main session's message (OSPanic calls the guest PPCHalt): it does not. The owner in native_os_panic.cpp only
calls cpu.request_stop (HleExit for the game's own "The End.", ExecutionFault otherwise, the same fatal path system.cpp uses at
its other request_stop sites) and never calls guest code; PPCHalt is mentioned only in the header comment describing the original.

| gate | evidence |
|---|---|
| 1 what it frees | what_if os-misc (generator lock): frees 0 rows, revives 0, 0 stale sites (baseline pruned 1332 = after 1332). Both rows are frontier leaves; the plan's freeing needs all three owned, and the third is not. |
| 2 contract | os_misc_contract_tests vs the pinned DOL: 168 calls (2 entries x 3 boot-info pointers x 7 console-type words x 4 register seeds); r3, all general registers, LR and the memory ranges equal. Negative control: retail value 0x10000002 -> 0x10000003 fails ("r3 differs"); restored passes. |
| 3 game behaviour | **not run**: an A/B needs two generator builds and the rows are reached only on the debug console path (OSGetConsoleType's three callers) and through an empty data-word callback; the main session's integration build covers the image. |
| 4 tests | native_file_api_tests: no mismatch; owner list includes os_misc::Owners. |
| ratchet | unchanged; body counts not measured (0 rows freed on its own). |

## Step 6: DMAErrorHandler 0x803726D8 (the machine-check handler OSInit registers at index 1)

Original: prints "Machine check received\n" and "HID2 = 0x%x   SRR1 = 0x%x\n" (HID2, context->srr1) through OSReport, then
branches on HID2 & 0x00F00000 and SRR1 & 0x00200000. Not a DMA error: one more line, OSDumpContext 0x80372E58, PPCHalt (never
returns). A DMA error: the DMA lines, the HID2 write-back, **it returns**.

Owner: `runtime/src/gekko/native_os_machine_check.{h,cpp}`. It prints the two lines before the branch to the host debug console
(os_report::DebugConsole, the same console and retail-console rule as the native OSReport) and stops with an ExecutionFault
naming HID2, SRR1 and the context pointer, **for both branches**.

**Explicit difference from the original, not equivalence** (the main session's decision): the original returns in its DMA
branch; the native owner stops. The HID2 write-back is GameCube hardware recovery and is not reimplemented; the runtime never
raises a machine check, so there is nothing to recover from. The branch lines and OSDumpContext's text are not reproduced.
0x803716BC (the default exception handler) is still not owned; it follows once af380fa (the native-to-guest call helper) is on
gc-removal.

| gate | evidence |
|---|---|
| 2 contract | os_machine_check_contract_tests vs the pinned DOL: 80 variations (8 HID2 x 5 SRR1 x enabled / retail console). The original runs in the reference interpreter with OSReport, OSDumpContext and PPCHalt stubbed: its two OSReport texts equal the native console lines byte for byte (retail: dropped); the original returned in 36 variations (DMA branch) and halted after OSDumpContext in 44, the native owner stops in all 80 (the stated difference, asserted). Negative control: one space removed from the second line fails ("second line differs"); restored passes. |
| 3 game behaviour | not run: the handler is reachable only through a machine check, which the runtime never raises. |
| 4 tests | native_file_api_tests: no mismatch; owner list includes os_machine_check::Owners. liveness roots regenerated, both --check pass. |

## Step 7: the default exception handler 0x803716BC (entry of OSUnhandledException)

Original: the stub every `__OSExceptionTable` slot holds (native OSInit leaves them at it) stores r0-r2, r6-r31 and the GQRs into the
context at r4 and continues in 0x80373590. That looks the handler up in `__OSErrorTable` 0x80586CB0 (index = r3; index 16 for
a floating-point exception when SRR1 bit 0x00100000 is set and entry 16 is registered), calls it with a guest `blrl` and RESUMES the
context (OSLoadContext) when it returns; with no handler, or SRR1[RI] clear, it prints "Non-recoverable Exception %d" /
"Unhandled Exception %d", "
", OSDumpContext's dump and the per-exception detail lines, and calls PPCHalt.

Owner: `serve_unhandled_exception` in `native_os_machine_check.{h,cpp}`, installed at 0x803716BC.
  - handler 0x803726D8 (what OSInit registers at index 1): the native DMAErrorHandler of step 6.
  - no handler / non-recoverable: the first two OSReport texts of the original (the exception line and "
"), then an
    ExecutionFault naming the exception, SRR0, SRR1 and the context.
  - any other registered handler: an ExecutionFault naming its address, nothing printed.

**Explicit differences from the original, not equivalence:** (1) the stub's register stores into the context are not made;
(2) OSDumpContext's text, the time and the per-exception detail lines are not reproduced, and PPCHalt is replaced by the stop;
(3) a registered handler other than 0x803726D8 is not called: the original calls it and resumes the context through
OSLoadContext (0x803789D8, SDK). I did NOT use the guest_call seam for it, because resuming needs the SDK context switch that
this work removes, and no program path registers one (OSSetErrorHandler's only caller is OSInit, which registers 0x803726D8 only);
a handler written into the table by other means is a loud named fault, never a silent skip.

| gate | evidence |
|---|---|
| 2 contract | os_machine_check_contract_tests vs the pinned DOL: 256 variations (8 exception indexes x 4 SRR1 x 4 table states x enabled / retail console). The original runs from 0x803716BC in the reference interpreter (OSReport, OSDumpContext, PPCHalt, the time and context-switch callees stubbed); the native console text must be a prefix of the original's OSReport text; 48 variations with a game handler assert the named fault. Negative control: "Unhandled Exception" -> "Unhandled exception" FAILS ("native text is not a prefix ..."); restored passes. |
| 3 game behaviour | not run: reached only by an unhandled exception, which the runtime does not raise in a normal run (the decrementer is stopped with its own message before it). |
| 4 tests | native_file_api_tests passes; owner list includes the new address. |

## Step 8: PPCHalt 0x80370B1C (cpu.config)

Original: `sync`, then an endless loop (0x80370B20..0x80370B2C); it never returns. After step 7 the only live caller path is the C runtime's
exit / abort functions (0x803A3BDC, 0x803A3C78 via 0x80379720). Owner: `os_panic::serve_halt` in `native_os_panic.{h,cpp}`,
installed with OSPanic; it stops the run with an ExecutionFault naming the link register.

**Explicit difference, not equivalence:** the original hangs the console in the loop; the owner stops the run instead. Nothing the
program can observe continues in either case.

| gate | evidence |
|---|---|
| 2 contract | os_halt_contract_tests vs the pinned DOL: 12 variations (4 link registers x 3 r3); the original never leaves the loop in 5000 steps; the owner stops with a fault naming LR. Negative control: the loop extent narrowed from 0x14 to 0x10 FAILS ("the original left the PPCHalt loop"); restored passes. os_panic_contract_tests (576) still passes. |
| 3 game behaviour | not run: reached only at program exit or a fatal error. |
| 4 tests | native_file_api_tests: no mismatch (owner list includes PPCHalt). Roots regenerated, both --check pass. |

Remaining frontier rows I do NOT take: 0x80372C68 (OSLoadContext) and 0x80373100 (OSSwitchFPUContext), cri-threads' work.

## Step 9: survey of the compiled bodies of os.other / os.core / cpu.config / audio.dsp / os.interrupts, and two exact owners

Survey (live graph of the plan on the round-9 tree, callers by address; `scratchpad/survey.py` over `removal_plan.Graph`):
  - Kept alive only by the old OSUnhandledException body 0x80373590 (so dead once 0x803716BC is owned, step 7): 0x8037170C, 0x80373590,
    0x80373684, 0x80373708, 0x8037379C, 0x803737B4, 0x803737C8, 0x803737E0, 0x80373844 (os.other), OSDumpContext 0x80372E58 (os.core,
    also called only from 0x80373590 / 0x80373708), 0x80370ADC, 0x80370AE4/AE8, 0x80370B60, 0x80370B80 (cpu.config).
  - Kept alive by cri-threads' work (OSLoadContext 0x80372C68, OSSwitchFPUContext 0x80373100, os.threads callers 0x803787B0 /
    0x803788E8 / 0x803789A8 / 0x80378A44 / 0x80378C2C / 0x80378D10): 0x80372B80..0x80372BE8, 0x80372D24, 0x80372D78, 0x80372D9C,
    0x8037292C, 0x80372A50, 0x80372B78, 0x80376128 (the OSDisableInterrupts tail entry, called by OSLoadContext), 0x80376C78 (thread queue
    drain via 0x803793B0). Not touched.
  - Entered from unidentified audio / init code, owned here:
      * 0x8038C384 (audio.dsp, caller 0x80385A9C): `lwz r3, 0x5DD0(r13); blr`. Owner `serve_load_cell_5dd0`.
      * 0x8037177C (os.other, caller 0x80384680): `OSReport("%s\n", r3)` (format at r13+0x26DC). Owner `serve_report_string` hands it to the
        native OSReport formatter.
  - Not owned: 0x8038C3FC (audio.dsp, caller 0x80385F08): OSDisableInterrupts, `*(r3+8) |= 2`, OSRestoreInterrupts, returns r3. The two calls
    are the native interrupt gates with recognition points that can hand back to the dispatcher inside the window; an owner would have to
    reproduce exactly that interleaving, which I cannot show equal here. Left for the interrupts owner.

Owners: `native_os_misc.{h,cpp}` (install now takes the debug console). Both exact: the getter returns the same word; the report prints the
same text the original sends to OSReport (a retail console prints nothing). No intentional difference.

| gate | evidence |
|---|---|
| 1 what_if | `--own 0x8037177C --own 0x8038C384` (plus 0x803716BC, 0x80370B1C) against current gc-removal: 0 rows freed (baseline pruned 1375 = after 1375); gc-removal already owns 0x803716BC / 0x80370B1C, and the two new rows free no callees. The survey above is the evidence for what step 7 frees. |
| 2 contract | os_misc_contract_tests: 252 calls (OSGetConsoleType, empty callback, getter: result, all GPRs, LR, memory equal to the original) plus 8 report-string calls (native console text equals the original's OSReport line; retail prints nothing). Negative control: getter reading +4 FAILS ("r3 differs"); restored passes. |
| 3 game behaviour | not run: neither routine is reached in the first frames; no game A/B claimed. |
| 4 tests | native_file_api_tests: no mismatch. Roots regenerated, both --check pass. |

## Step 10: RW driver rows nobody had started (survey only; nothing owned yet)

Frontier of graphics.rw-gamecube-driver on the round-9 plan (33 rows) against the docs of vi, native-boot, card-saves and sdk-identify:
  - claimed or already owned: rwR1 device rows and render wrappers (vi: GcnSystem, ShowRaster, Im2D triangle / line, 0x804960FC /
    0x804987C8 / 0x80498854, RasterSetImage, RasterRender*), the raster life cycle (card-saves: Create, Destroy, Lock, Unlock, UnlockEnd,
    UnlockPalette, SubRaster, SetContext, GetMipLevels and the helpers), native texture Read / Write / GetSize with the level helpers
    0x804965A8 / 0x80496478 and ImageFindRasterFormat (sdk-identify), rwR5 registry rows and 0x80465818 / 0x804662B0 ("translated by
    design", native-boot and vi), TextureBind 0x8049AC88 (dropped by main).
  - no claim found: **GcnImageGetRaster 0x80493A40** (slot 6, 308 bytes) and its three single-caller converters **0x80493118,
    0x8049336C, 0x80493660** (149, 189 and 248 instructions). Announced to sdk-identify, card-saves and native-boot.

Shape of the unit: ImageGetRaster locks the raster when needed (0x8048AFC0 / 0x8048ACE4, RenderWare core), switches on the raster
format (4 / 8 / 32 bits per pixel) into one converter, which switches on the pixel format (0x100..0xA00) to pick an RGBA-to-pixel
function (the standard functions already native in native_rw_pixel) and on the image depth (a 29-entry jump table at 0x8056F1F4) to
convert each row through an indirect call, reporting unsupported combinations through the RwEngine error path (0x8047EC78 /
0x8047EBD4, RenderWare core), then unlocks. Owning it exactly needs: the three converters' depth-by-format conversion loops
reproduced, the pixel-function calls dispatched to the native standard functions without guest code, and a contract test per
(raster bpp x pixel format x image depth) against the original in the reference interpreter. That is a large unit (about 590
instructions); it is the next item, not started in code in this step. No difference from the original is intended.

Correction (sdk-identify, after step 10 was written): GcnImageGetRaster 0x80493A40 and the converters 0x80493118 / 0x8049336C /
0x80493660 are ALREADY DONE on agent/sdk-identify as 1a08287 (rwR2 step 3, native_rw_image.cpp, rw_image_contract_tests: 756 scenes
equal to the interpreted original). I do not take them. The other rows sdk-identify named as unstarted (GetMipLevels 0x804966D8,
SetContext 0x8049A570, UnlockEnd / UnlockPalette 0x804972B8 / 0x80497388) are owned by card-saves (native_rw_raster slice 1), and the
Render* wrappers are vi's by the split in card-saves.md. So no RW driver row is left for me; the claim above is withdrawn.

## Step 11: audio.ax, the parts audio-ax released (MIX table class D owned; AXQuit chain analysed, not owned)

Released by audio-ax (nothing of its files is touched; the new owner has its own header and test): (a) the two MIX table data words
0x8038A0DC / 0x8038A104, (b) the AXQuit chain.

(a) Owned: `native_ax_mix_alloc.{h,cpp}` (`ax_mix_alloc::install`). 0x8038A0DC = OSAllocFromHeap(*(r13+0x26E0), r3), 0x8038A104 =
OSFreeToHeap(*(r13+0x26E0), r3): `mr r4,r3; lwz r3,0x26E0(r13)` and a tail call into the heap routines, which are native owners
(native_os_heap), so the owner runs the same heap code on the same guest heap. No intended difference.

(b) Read body by body (audio-ax's list corrected by its own message and by my reading):
  - 0x80384840: zeroes the 32-word stack head table 0x80587DC0 and tail table 0x80587E40, sda 0x5CE8 = 0 (AXInit's twin of 0x8038478C).
  - 0x8038708C, 0x803863D4, 0x80385780: empty (`blr`).
  - 0x80384CAC: sda 0x5CF0 = 0, 0x5CF4 = 0.
  - 0x80385F08: OSDisableInterrupts; sda 0x5D58 = 0; 0x8038C3FC(0x8058FBC0) (OSDisableInterrupts, `*(r3+8) |= 2`, OSRestoreInterrupts);
    **0x803792C4(r13+0x5D68)** - not an alarm cancel: it puts the CURRENT THREAD to sleep on the queue at r13+0x5D68
    (state 4, queue insertion by priority, `__OSReschedule`), i.e. AXQuit blocks until the frame engine wakes it; then AIStopDMA
    0x80381F14 (already native) and OSRestoreInterrupts.
  AXQuit is not owned: its tail is a blocking handshake with the AX frame callback (the thread sleep and the wake-up belong to the
  thread scheduler, cri-threads' OSSleepThread / OSWakeupThread work, and to the audio-ax frame engine which keeps AXInit and the
  frame callback). An exact owner needs both ends of that handshake; it is called once, from the game's shutdown destructor
  0x801F2DA8, never in play. Left unowned and unlisted as a wait point.

| gate | evidence |
|---|---|
| 1 what_if | `--own 0x8038A0DC --own 0x8038A104`: 0 rows freed (the heap routines stay reachable); no stale sites. |
| 2 contract | ax_mix_alloc_contract_tests: 960 wrapper calls (6 heaps x 160 random allocations / frees, 108 refused allocations): result, preserved registers, LR and every byte of the heap equal to the original run in the reference interpreter. Negative control (r4 = r3 + 0x40) FAILS ("heap memory differs after op 0"); restored passes. |
| 3 game behaviour | not run: the two table entries are reached only through the AXFX effect allocator, which the game does not use in the replay. |
| 4 tests | native_file_api_tests: no mismatch. Roots regenerated, both --check pass. |

## Step 12: the Im2D line / triangle emitters (owned) and the RasterRender pair (decoded, not owned)

Instruction chain: main -> "disassemble every instruction and write down validation, error paths, guest state left behind and exactly
what is drawn, then replace natively". All bodies below were read in full from the pinned DOL.

### RwIm2DRenderLine 0x80491D0C (93 insns) and RwIm2DRenderTriangle 0x80491B08 (129 insns): OWNED

Arguments: r3 = RwIm2DVertex array (24-byte vertices: x, y, z at +0, +4, +8; RGBA at +0xC..+0xF; u, v at +0x10, +0x14), r4 = vertex
count (never read), r5, r6 (, r7) = vertex indices. Vertex k is `r3 + k * 0x18`.
- Validation: none. No type check, no bounds check, no error call. Both always return r3 = 1.
- Body: `bl 0x8049148c` (GX state setup from the render states), GXBegin (0x80396934) with GX_LINES 0xA8 / GX_TRIANGLES 0x90,
  vertex format 0, 2 / 3 vertices, then the vertices to the GX FIFO (0xCC008000) in the order v1, v2 (, v3): three floats x, y, z, the
  RGBA bytes, and u, v only when the device record word at *(r13 + 0x620C) is non-zero (two code paths, same order); finally
  `bl 0x80491774`. The same order is asserted instruction by instruction in im2d_prims_tests.
- 0x80491774 (229 insns), read in full: with frame = *(*(r13+0x615C)) + 0x60; if frame == *frame it returns; otherwise it sets the GX
  viewport and scissor from the frame's origin (+0x1C, +0x1E, halfwords) and size (+0xC, +0x10) - the jitter variant (0x8039A444)
  when a debug flag byte at *(r13+0x61F4)+0x18 is set (its last argument is the result of 0x80380004 xor 1), else
  GXSetViewport (0x8039A49C) - then GXSetScissor (0x8039A4E4) from the same halfwords and sizes; and a second copy of the same
  logic under r13+0x6198 != 0. Guest state left behind: only the GX shadow (the GX owners' scaffold state), nothing the game reads.
- Owner: `scaffold/rw_driver/im2d_prims.{h,cpp,_install.cpp}`: the SAME draw GcnIm2DRenderIndexedPrimitive makes (`rw_im2d_render_indexed_primitive`)
  as RenderWare line list (type 1) / triangle list (type 3) over the array with indices (v1, v2 [, v3]); r3 = 1.
  Stated differences: (1) indices are written just below the stack pointer (the immediate path reads indices from guest memory);
  (2) the vertex count handed on is max(r4, highest index + 1), so an index past r4 draws the vertex the original would read instead of
  being refused; (3) GX state setup, FIFO stores and the viewport/scissor restore are not carried - the lifted build has no GX
  consumer and the native renderer takes the draw as values (the render states in force are the scaffold's, as for the primitive
  entry).
- Gates: im2d_prims_tests (new, `reference;native-contract`): 20 original instructions equal the decoded rule (index arithmetic, add
  registers, GX_LINES / GX_TRIANGLES constants, vertex counts, vertex store order, `li r3,1`) and 1536 emitter calls (type, vertex
  array, count, index order read back from guest memory, r3 = 1); negative control (indices swapped) FAILS ("index order"), restored
  passes. shadow_render_lifted builds with the owners installed from LiftedRender::install_owners. Rows added to
  extract/native-owners-r2.txt. NOT run: a pixel test (needs the driver harness; the pixels are the immediate path's, already
  covered for the primitive entry by rwim_tests and the replay) and a game A/B (0 hits in the replay).

### RwRasterRender core 0x80499924 (94 insns): decoded, NOT owned

(r3 = source raster, r4 = rect {x, y, w, h} words, r5 = scaled flag, r6 = fast flag), reached through the three 40-byte wrappers
0x80499A9C (r5 = 0, r6 = 1), 0x80499AC4 (0, 0), 0x80499AEC (1, 1).
1. Rect: copies words 0,1 of r4 to a local; words 2,3 from r4 when r5 != 0, else from the raster's width / height (+0xC, +0x10).
2. T = byte at +0x20 of the record at *(r13 + 0x6210) (the current target's type), S = byte at +0x20 of the raster (its type):
   - T == 2: S == 0, 4 or 5 -> the draw body 0x80499394(raster, &rect, r5, r6), returns 1; S == 1 or 2 -> error; S == 3, S >= 6 or
     S negative -> nothing is drawn, returns 1.
   - T == 0 or 4 or 5: S == 3, S >= 6 or negative -> nothing, returns 1; S in 0..2 or 4..5 -> error.
   - any other T -> error.
   Error = `RwErrorSet`-style: a 12-byte record {1, rwEngine error code via 0x8047EC78(2, string 0x8056FB9C)} passed to 0x8047EBD4
   (RenderWare core, reachable by guest_call), then returns r3 = 0.
Nothing is written to guest memory except that error record on the stack.

### Draw body 0x80499394 (356 insns): decoded, NOT owned
(r3 = raster r30, r4 = rect r31, r5 = scaled r27, r6 = fast r28; r29 = *r30, the raster's parent.)
- Saves render states 14, 9, 3, 4, 1 (GcnGetRenderState 0x804984C4 into stack slots), calls 0x80396B54(0) (a GX call with 0; not identified),
  sets blend states: fast -> state 5 = 6 and (0x80498910) pair (5, 6), else (2, 1); state 14 = 0; state 9 = 1 (r5 == 0) or 2 (scaled);
  state 2 = 3; state 1 = the parent raster (r29); binds the Im texture's raster (0x80498854) and the raster's texture (0x80499004), then
  GXBegin(GX_QUADS 0x80, format 4, 4 vertices) with, per vertex, two halfword positions (target origin from the record at
  *(r13+0x6210): +0x1C, +0x1E, plus the rect corner) and two float texture coordinates ((source origin from the raster: +0x1C, +0x1E,
  plus width / height of the source for the far corners) * (1 / parent width, 1 / parent height)); the scaled and unscaled paths
  differ only in which size (rect w, h or the source's) the far corners add. Then restores the five states.
- What it draws: the raster (or its sub-rectangle) as a 2D textured quad onto the target at the rect, texels normalized by the parent
  raster's size, filter and blend from the state values above. A native version is a textured Im2D quad on the raster's texture in
  the target's pixel coordinates with those states set and restored around it; the scaffold has the pieces (render_state_set / get,
  im2d_bind, texture build) but no entry that binds an arbitrary raster as the Im texture without writing the game's Im-texture word.
  Owning it exactly needs that entry (or a faithful state/bind sequence through the existing owners) and a pixel test on a synthetic
  scene; it is the next item and is not started in code.

### Step 12b: the pixel harness `rw_im2d_pixel_tests` (rasterized check of the Im2D owners)
`runtime/tests/rw_im2d_pixel_tests.cpp`, a target of the lifted tree (`cmake --build .build/watch --target rw_im2d_pixel_tests`, EXCLUDE_FROM_ALL;
also an add_test for trees with testing on). It builds a REAL RwDriver on a Renderer made on the WARP (software D3D12) adapter - no
GPU of the machine is used - with the scene in test guest memory like the game's RenderWare objects (a 640 x 480 camera raster of type camera, a camera
with its frame and view window, a vertex array of 24-byte RwIm2DVertex records). A frame is camera_begin, the draw, camera_end,
submit_frame; the exact frame packet the driver hands over (set_frame_observer) is rendered by Renderer::render_to_image (RGBA8).
Checks, all WARP image against WARP image (nothing stored): two empty frames are identical; the line owner draws exactly the image the
plain primitive entry draws for a line list of the same two vertices (320 pixels drawn); the triangle owner draws exactly the image
of the triangle-list primitive over its three vertices, in both windings (59,600 pixels drawn); different vertices draw different
images. Result: PASS. Negative control: the line owner's second index changed to r7 -> "the line owner drew nothing" FAIL; restored PASS.
What it does not cover: the textured variants (the Im texture word is zero in the harness), the HUD layout paths (the draws
sit inside the canvas), and the lifted game's own frame; nothing is compared with a stored image. The harness is reusable for
anything drawn through RwDriver (the raster quad is next).

## Step 13: RwRasterRender (core 0x80499924 owned; draw body 0x80499394 carried by RwDriver::raster_quad)

Files: `scaffold/rw_driver/raster_render.{h,cpp}` (the core: rect, target/source type table, three error sites),
`raster_render_install.cpp` (the error report through the guest-call seam in the original's own 0x30-byte frame, held for the
whole report; `RwDriver::raster_quad`; the install), one declaration added to vi's `rw_driver.h`, install line in
`lifted_render.cpp`. The draw body has one caller (the core), so it is not installed separately.

What the original does that the pixel work depends on (read in full): the raster is bound by RwRenderStateSet(1, parent) - that
writes the Im texture's raster field (and marks it stale) and the same setter restores it; the Im-texture pointer word is never
written. States 14 (fog off), 9 (filter 1 nearest unscaled / 2 linear scaled), 2 (address 3, clamp) are set and 14, 9, 3, 4, 1
restored in that order. The blend (5, 6 when fast = alpha blend, else 2, 1 = replace) goes to GX only (0x80498910) and the driver's
cache is untouched; here it is the recipe's blend for that one draw, restored. Quad: four corners TL, BL, BR, TR; positions are
16-bit sums (target origin + rect corner; far corners add the rect's size when scaled, the raster's otherwise); texture
coordinates are single-precision (source origin or origin + size) x 1 / parent size; TEV is REPLACE with no vertex colour, so the
texel is the colour (white vertex colour here).

Stated differences: (1) GX-only state (texgen, vertex format, TEV, viewport and scissor for the rect, num chans) is not carried;
(2) the quad goes through the Im2D path (HUD layout policy, canvas projection), as the other Im2D owners do; a draw outside a
screen view is dropped there; (3) texture-coordinate precision is single as the original's.

| gate | evidence |
|---|---|
| 1 what_if | not run for this row (the lifted-only owner); roots regenerated, both --check pass. |
| 2 contract | raster_render_contract_tests: 1080 calls (T x S x scaled x fast x 3 seeds) on the original core in the reference interpreter with its draw body and the two RW error functions recorded: outcome, error site (the formatter call's return address), rect handed to the draw, flags, error record, r3 equal. Negative control (S == 5 no longer draws) FAILS at T=2 S=5; restored passes. |
| 3 pixels | rw_im2d_pixel_tests on WARP: an 8x8 GX RGBA8 raster (red, green at alpha 128, blue, yellow) drawn scaled to (50,50) 200x200: each quadrant's colour is the expected one at the quadrant centre, nothing outside the rect; fast mixes the half-alpha texel with the background and leaves opaque texels unchanged; unscaled draws 8x8 at the rect origin, nearest, the rect's size words ignored; render states 1, 3, 4, 9, 10, 11, 14 and the Im texture's raster and filter words equal before/after. Negative control (bottom-left texture v wrong) FAILS "bottom-left quadrant is not blue"; restored passes. |
| 4 game A/B | not run: no replay scene calls RasterRender (0 hits was not measured here; no claim). The error path through the real RW core was not run either - only through recorders. |

## Step 14: what keeps os.other alive now (removal_plan.py on gc-removal + ed4e9fe, plan10)

Plan: 143 platform bodies compiled, frontier 38. os.other: 4 bodies, **frontier 0**, so nothing in os.other is ownable on its own any more.
The four (0x8037292C 292 B, 0x80372A50 304 B, 0x80372B78 8 B and one more) are the FPU register save / load helpers. They are
reached only from 0x80373100 / 0x8037310C, which is the FP-unavailable exception handler (exception index 7, `native_boot.cpp`
kFpUnavailableHandler = OSSwitchFPUContext): it saves the old owner's FPRs (0x80372A50) and loads the new owner's (0x8037292C).
That handler is os.core (cri-threads' row: OSLoadContext 0x80372C68 and OSSwitchFPUContext stay theirs per the coordination
rules). Once those two are native owners, the helpers fall with them (no other reachers in the graph). Nothing for os.other to do
before that.
The 12 frontier rows of `sdk:unidentified` (42 bodies) are the AX library range 0x80381E30-0x80385xxx: AXQuit (0x803846CC) and the
init/quit twins read in step 11. They belong with audio-ax's frame engine; AXQuit needs `serve_sleep_thread` and is only called
from the shutdown destructor.

## Step 15: the 3x4 matrix leaves of the 0x8040C* family (native_math_affine)

Owned: 0x8040C6CC (point = m x [p 1]), 0x8040C5EC (translate_by), 0x8040C7E0 (scale columns). Same fused-madd / single-rounding
operations, operand roles and order as the original (ppc::fps_*), including FPSCR sticky / FPRF bits.

| gate | result |
|---|---|
| contract | math_affine_contract_tests: 18,000 calls (6,000 per entry) against the pinned original DOL (FNV 0xD2CFB4A1CA897F9B) in the reference interpreter; zeros, subnormals, inf, NaN payloads, FPSCR with/without NI; 15,000 with output/vector overlapping the matrix. All GPRs, LR/CTR/CR, f8-f31, FPSCR and the whole 0x1000 arena equal. PASS |
| negative control | last fmadds replaced by fmul + fadd: FAIL "memory differs ... iteration 23 mode 5"; restored, rebuilt, PASS |
| game A/B | not run; no claim |

Stated difference: f0-f7 (the original's volatile scratch FPRs) are not reproduced. Not compared by the test. No reader across the ABI
boundary is proven by a watch; this is the ABI argument only.
Not done: the rotation builders C110 / C188 / C200 (they call sincos 0x80031568, which leaves f31.ps1 single-rounded via a psq frame
save), and AXQuit.
