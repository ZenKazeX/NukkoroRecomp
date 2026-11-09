# DOL disassembly and reconstruction system (dolrecon)

The user's specification (2026-10-31): a system that disassembles GameCube DOLs into reproducible assembly projects,
rebuilds them byte for byte, processes thousands of functions in parallel workers, finds similar functions, and later
supports controlled edits and C/C++ reconstruction. Code: `tools/dolrecon/`. Projects: `.build/dolrecon/<name>/`.
Tools live on H: (the user's rule: nothing on C:): `H:\Tools\dolrecon\`.

## Status (2026-10-31)

| milestone | state |
|---|---|
| 1 exact round trip | DONE for the three Shadow DOLs (below); accuracy audit + corrections DONE ("Accuracy") |
| 2 durable jobs, bounded workers, 10k-job benchmark | not started (SQLite first, PostgreSQL-compatible schema) |
| 3 similarity indexes | not started |
| 4 controlled editing | not started |
| 5 C/C++ + REL | not started (no CodeWarrior compiler on this machine: matching C needs mwcc) |

## Tools (pinned; digests = GitHub's published SHA-256)

| tool | version | file | SHA-256 |
|---|---|---|---|
| decomp-toolkit | 1.8.4 (a0c455e4) | H:\Tools\dolrecon\dtk-1.8.4\dtk.exe | 6a693e95aa4d30acccc673343302a4abdfa3146bd0fb15cb1a2bce924cbc782a |
| gc-wii-binutils | 2.42-2 (GNU as / ld 2.42) | H:\Tools\dolrecon\binutils-2.42-2\windows-x86_64.zip | bd95bd57607efb45c437e97b77ec199073e242869027eb72196b0720611bb1c1 |

Ghidra 12.0.3 is installed at C:\Dev\ghidra; the GameCube loader 1.3.1 is built for Ghidra 12.1 - not integrated.
PostgreSQL is not installed.

## Milestone 1: what exists

- `dol.py`: header / section validation (bounds, alignment, memory overlaps, entry in text, BSS without file bytes),
  separate file offsets and addresses, `layout()` = every file byte exactly once (header, sections, gaps, trailing),
  `manifest()` (identity, sections, regions with SHA-256, diagnostics, config).
- `project.py <main.dol> <dir>`: copies the DOL, writes dtk's config (SHA-1), runs `dtk dol split -j 16` (analysis:
  functions, symbols, relocations, splits) and then `rebuild.run`.
- `rebuild.py <dir>`: plan (each assembly block's [start, end) from dtk's "# 0x..0x" lines; overlaps refused) ->
  prepare (build copy of the asm with recorded transforms, below) -> assemble (powerpc-eabi-as -mgekko, 16 workers) ->
  link (GNU ld script generated from the plan; every block pinned at its original offset with ". = OFFSET" and an
  ASSERT on its end) -> package (header regenerated from manifest fields; sections from the linked ELF; uncovered
  bytes as preserved fragments; ownership checked: every byte exactly once) -> verify (level 1 per block, level 2
  whole file; mismatches report file offset, address, expected / actual bytes and the unit).
- Transforms of dtk's asm (all counted in report.json):
  1. CodeWarrior '@' names (`@62_80514448`, `"@1_805610F8"@ha`) -> `__at_...`: GNU ld reads '@' as a symbol version
     ("multiple definition of `no symbol' (*IND*)"). Not inside .string / .ascii (game data). A rename that would hit an
     existing name stops the build.
  2. Linker-defined symbols (`_SDA_BASE_`, `_SDA2_BASE_`, `_stack_addr`, `_rom_copy_info`, `_bss_init_info`, ...)
     recovered from the ORIGINAL instruction operands at their @ha / @h / @l relocation sites (every site must give one
     value; symbols.txt must agree where it states one), emitted as absolute symbols in the ld script.
  3. dtk 1.8.4 static-prediction bug: e.g. `41 A1 FF E4` (backward bgt, y=1) printed `bgt+`; GNU as needs `bgt-`.
     The suffix is recomputed from the original word and the branch direction (raw `bc BO,...` forms untouched).
     4 fixes per DOL.

## Results (16 workers, i9-13900K, H: drive)

| DOL | SHA-256 | rebuilt | assembled bytes | preserved | generated | dtk split | rebuild |
|---|---|---|---|---|---|---|---|
| PAL GUPP8P | fde4fa6f... | byte-exact | 5,772,400 (99.989 %) | 396 | 228 | 39 s | 11.5 s |
| USA | c4ab471c... | byte-exact | 5,768,192 (99.989 %) | 380 | 228 | 22 s | 13.7 s |
| Japan | bb124abb... | byte-exact | 5,769,848 (99.989 %) | 388 | 228 | 22 s | 13.8 s |

Preserved = header padding (28 bytes) + the zero padding at the end of each DOL section that no dtk block covers
(8-188 bytes each). Generated = the header fields, rebuilt from the manifest.

Coverage vs accuracy: dtk found 34,037 functions (PAL), 4.86 MB of code, all emitted as assembly that reassembles to
the original bytes. 330 functions have names (SDK / runtime / signatures); 33,707 are `fn_XXXXXXXX`. Sizes: 4,079 <= 8
bytes, 10,273 of 12-64, 15,739 of 68-256, 3,489 of 260 B-1 KB, 457 above 1 KB. Function BOUNDARIES are dtk's
analysis, not independently verified; exactness of the bytes does not prove them.

## Accuracy (2026-10-31, second pass)

A byte-exact rebuild proves the bytes, not the analysis (a wrong boundary or a missed pointer reassembles to the same
bytes at the original layout). Three additions measure and improve the analysis itself:

- `audit.py <project>` judges dtk's analysis against the original bytes (its own branch decoder) and the compiler's
  own tables, never against dtk's text: B1 extabindex (exact start / size), B2 calls into function bodies (labelled
  second entries counted apart), B3 branches into other bodies, B4 functions without an unconditional end, B5 text
  outside functions (with the owning symbol), B6 vtable slots must be function starts, R1 unrelocated address words
  in data, R2 relocation targets inside code (code labels / jump tables apart), R3 raw `lis` pairs judged by the
  full constructed value. 6 s per DOL. -> build/audit.json.
- `linker_tables.py`: `_rom_copy_info`, `_bss_init_info`, `_eti_init_info` and the `.ctors` / `.dtors` null
  terminators are generated from the layout (accepted only when they reproduce the original bytes) instead of kept
  as raw bytes. Preserved bytes 396 -> 192 (PAL), and the audit checks: all 192 remaining are zero padding.
- `corrections.py <audited project> <new project>`: evidence-derived dtk config changes, applied to a NEW project,
  rebuilt (must stay byte-exact) and re-audited; corrections.json lists each with its evidence:
  - 48 add_relocations: 24 `lis`/`ori` pairs in the debugger's interrupt-vector code that build
    `TRKInterruptHandler` (0x8039DA04) without a relocation (now `TRKInterruptHandler@h` / `@l`).
  - 19 code labels at `__OSUnhandledException` jump-table targets (dtk's `@87` table pointed at fn+offset).
  - 2,920 virtual-function names `Class__vfN` from the game's RTTI: {name, vtable} records in .sdata, accepted only
    when the vtable header points back at the record (rejects {name, float} parameter records like LineHang = 1.0
    and string tables like FAN.BIN), method runs bounded by dtk's data objects (a jump table often follows a vtable);
    a function in several unrelated vtables is left unnamed (404). dtk's own names win.

| check (PAL; USA / JP identical in kind) | v1 (dtk as is) | v2 (corrected) |
|---|---|---|
| B1 extabindex entries exact | 511 / 511 | 511 / 511 |
| B2 calls into bodies (unlabelled) | 0 (34 labelled entries, `_savefpr_N` ... dtk named them) | 0 |
| B3 branches into other bodies | 0 | 0 |
| B6 vtable slots on a function start | 4,997 / 4,997 (1,081 classes) | same |
| R1 unrelocated pointers in data | 3, all inside the generated `_eti_init_info` | same |
| R2 relocations into code bodies, unlabelled | 19 | 0 |
| R3 raw `lis` pairs that build a symbol start | 24 | 0 |
| named functions | 330 | 3,250 |
| preserved (raw) bytes, all zero | 396 | 192 |
| byte-exact rebuild | yes | yes |

Remaining, explained: B4 3 functions without an unconditional end + 1 call to 0x60 = the OS exception-vector
templates (code copied to low memory at run time); B5 non-function text = gTRKInterruptVectorTable (copied code),
the generated tables, the end of __OSSystemCallVector.

Cross-region consistency of independently derived names (pal-v2 / usa-v2 / japan-v2): PAL vs JP 3,226 names in
both, all on functions of identical size; PAL vs USA 3,223 of 3,227 identical, the 4 others are code that changed
between the USA and PAL/JP builds (Player::Behavior::SpinDash vf2 / vf5, WarpHole::ManagerTask vf0, one dtor).

Evaluated and rejected: extract/functions-dolphin-sigs.csv (6,236 names from a signature database). 5,579 names
map to several addresses each; the list carries Wii / other-game libraries (J3D, homebutton, Mario Strikers'
cFielder); only 2 names pass uniqueness + size + GameCube-library rules. Importing it would lower accuracy.
extract/functions.csv (the recompiler's 39,355 "functions") is not a boundary source either: its extra starts are
call-return continuation points (e.g. 0x80009490 after a bctrl).

Run order: `project` -> (`rebuild`) -> `audit` -> `corrections <dir> <dir>-v2` (does its own rebuild + audit).

## Next

1. Milestone 2: SQLite job store (leases, heartbeats, retries, idempotent publish, checkpoints), per-function jobs
   (feature extraction, per-function reassembly check), bounded pools; 10,000+ jobs benchmark with measured
   throughput and memory.
2. Milestone 3: exact (original bytes) / normalized (local labels, relocation targets abstracted) hashes, MinHash/LSH
   over instruction shingles, detailed rerank; within PAL, PAL vs USA vs JP (three builds of the same game: a natural
   recall benchmark).

## C++ reconstruction (2026-10-31, tier 1: exact register-level C++, verified)

No CodeWarrior compiler exists on this machine (searched H:, E:, C:\Dev), so C that recompiles to the identical
machine code cannot be produced or checked here; nothing is claimed as "matching". Tier 1 is behaviourally verified
C++: `tools/dolrecon/lift_cpp.py <corrected project> runtime/tests/generated` turns each function's verified
instructions into C++ on locals (no Cpu object, no decoder, no dispatcher; calls are direct C++ calls between lifted
functions; indirect calls through a sorted address table; returns checked against the entry LR; r2 / r13 as the
EABI small-data bases; symbols from the corrected project in comments and constants). Refusals are explicit and
counted (dolrecon_lift_report.json): FP arithmetic (tier 2: needs an exact Gekko FP layer independent of the
interpreter), paired-single quantized loads / stores (tier 3), supervisor / system instructions.

Verification: `runtime/tests/dolrecon_lift_tests.cpp` (CTest dolrecon_lift_tests) runs every lifted function against
the ORIGINAL instructions in the reference interpreter on randomized states (pointer inputs found by a pointer-flow
analysis, interprocedural; unmapped accesses stop, as production runs) and compares all GPRs, both FPR halves, CR,
XER, CTR, LR and every byte written. `tools/dolrecon/lift_verify.py` runs it crash-tolerantly in 16 processes (the
reference core's fault handler aborts on a host access fault at the top of the guest window; the function is then
recorded, never passed). Bugs the harness caught and that were fixed: stfs conversion below exponent 874 (the store
keeps the sign), ps1 written by lfs, the interpreter's default of NOT stopping on unmapped accesses.

| run 2026-10-31 | value |
|---|---|
| entries (functions + labelled entries) | 34,121 |
| lifted to C++ | 31,572 (893,510 instructions) |
| verified (>= 8 valid randomized cases, 0 differences) | 14,666 |
| FAILED (open) | 3 (fn_800DE374, fn_8025E2C8, fn_8025E350: lifted access refused) |
| unverified: too few valid inputs | 10,602 |
| unverified: reaches a callee that is not lifted | 803 |
| unverified: device registers | 6 |
| worker crashed (recorded, unverified) | 5,492 |
| full verification time, 16 workers | 480 s |

Next: the 3 failures; input generation for the unverified (pointer-typed memory images from recorded game states:
test_data MEM1 fixtures / savestates instead of random scratch); tier 2 FP; tier 3 paired singles; tier 2+ readability
(expression folding, structure recovery: if / loops instead of goto, typed this-pointers from RTTI).

### Harness findings, second round (2026-10-31 evening)

- Divergence localization: both sides record a call trace (function entered by a branch + r1, r3-r10, f1); a FAILED
  case reports the first differing event ("entered X vs Y", "r5 differs entering X") or "traces agree: the
  difference is in the function's own body". Order-dependent failures are found by bisection with --range / --skip.
- ROOT CAUSE of the last 3 FAILED (test-order dependent): an earlier function's random case stored through a pointer
  into .text (fn_800DDD0C -> fn_800DF838 -> copy loop fn_800DF89C with r4 = 0x80000000), so the interpreter later ran
  damaged ORIGINAL instructions while the lifted C++ (from the clean DOL) did not. Both memories were byte-identical,
  which is why the RAM check could not see it. Diagnosed here by bisection + a fresh-Cpu experiment, and
  independently by GPT-6-Astra (xhigh, read-only, .build/dolrecon/astra/answer1.md), which also traced the exact copy
  path. Fixes: pristine image restored into both memories before every function; a case whose original writes into
  a text section is invalid input and the code bytes are restored at once; the RAM integrity check stays.
- Pointer flow: the EABI preserves r1, r2, r13, r14-r31 across calls, so their pointer origin now survives a call
  (Astra's observation); that turns such inputs into valid pointers instead of edge integers.
- Host access faults inside the reference core (top-of-window straddles on random input) are caught per case (SEH)
  and counted invalid; workers no longer die for them.
- Cpu::reset() restores SPR / SR / MSR / FPSCR itself (Astra checked; an earlier harness comment claiming otherwise was
  wrong and the redundant snapshot code is removed).
- New options: --mem1 <24 MiB image> (real game memory as background: live global pointers) and
  tools/dolrecon/lift_merge.py (FAILED if any run failed; verified if any verified; nothing upgraded).

## State 2026-10-31 19:15 (latest; supersedes the tables above where they differ)

Lifting (tools/dolrecon/lift_cpp.py, generated into runtime/tests/generated/, 16 shards + table + decls):
34,045 of 34,121 entries (functions + labelled entries) = 99.8 %, 1,214,216 instructions; 33,727 with every direct
callee lifted. Tiers: 1 integer / memory / control flow / calls; 2 scalar FP (runtime/tests/dolrecon_fp.h: an
independent formulation of the reference interpreter's Gekko FP rules - NaN priority A>B>C, FPSCR[NI] operand and
result flush, single rounding with exact fused tie-breaks via a signed 32-bit-limb accumulator, fctiw NI rule,
fcmpu/fcmpo FPSCR bits, mffs / mtfsf / mtfsb / mtfsfi / mcrfs, CR1 record forms); 3 paired singles (psq_l / psq_st
+u/x/ux through GQR0-7 in the state, ps_ arithmetic in two single lanes, sums, muls0/1, madds0/1, sel, cmp, merges,
neg/mr/abs/nabs, dcbz_l). The 76 refused are OS / kernel code needing supervisor hardware (mfmsr, HID0, dcbi, SRR0/1,
sc, tw ...).

Verification (runtime/tests/dolrecon_lift_tests.cpp + tools/dolrecon/lift_verify.py + lift_merge.py): every lifted
function vs the ORIGINAL instructions in the reference interpreter on randomized states; compares all GPRs, both FPR
halves, CR, XER, CTR, LR, FPSCR, GQR0-7 and every written byte (RAM, guard, locked cache); a byte written by only one
side fails even with equal values. Per function: fresh Cpu, pristine image; per case: code bytes written by the
original are restored and the case is invalid; host faults caught (SEH); call-trace divergence localization;
instruction coverage of the original per function ("cov a/b"). Three passes: DOL image, Final Haunt MEM1, Lethal
Highway MEM1; merge = FAILED if any pass failed.

Results so far: before FP / PS tiers (31,572 lifted, DOL pass): 16,302 verified, 0 FAILED, 0 worker crashes. Smoke of
200 sampled FP / paired-single functions after the new tiers: 118 verified, 0 FAILED. RUNNING NOW: the full 3-pass
verification of all 34,045 (.build/dolrecon/lv3-*, merged lv3-merged.csv) - read its summary first next time.

Fixed in this round: mulli emitted s32*s32 (C++ undefined behaviour on overflow) -> unsigned multiply; the guard
region was not resynced per case (10 "memories differed" reports, no verdict affected).

GPT-6-Astra (Codex CLI, .build/dolrecon/astra/ask.ps1 <question> <answer> [effort]; ultra works and is confirmed in
the session header): question 1 answered (answer1.md); question 2 (adversarial review, question2.md) twice hit the
ChatGPT usage limit before answering - the user then said not to wait for Astra; the review was done here instead
(findings above).

Next: read lv3-merged summary; investigate any FAILED with the call-trace localization; raise valid-input rates for
the "unverified" functions (typed object pointers from RTTI vtables in the MEM1 images; recorded call arguments from
gameplay); then readability tiers (expression folding, structured control flow, typed this pointers).

## Names (2026-10-31 evening): tools/dolrecon/names.py -> .build/dolrecon/pal-v3 (byte-exact)

Evidence-only name recovery; every new name is in pal-v3/names.json with its rule and evidence; names given by dtk
(SDK / runtime) and by corrections.py (RTTI vf slots) are never replaced.

| rule | new names | evidence |
|---|---|---|
| N1 owner | 73 | extract/native-owners.txt "0xADDR Name" lines (project-validated native owners; prose lines ignored) |
| N2 construct | 1,031 | a non-virtual function loads class C's vtable POINTER -> C__construct |
| N3 string | 515 | references a distinctive literal used by at most 2 functions (counted by TEXT: the compiler keeps one copy per unit) -> ref_<text> |
| N4 helper | 4,205 | its only direct caller is named G -> G__sub<k>, up to 4 levels |
| D1 data | 12,276 strings, 4,675 floats, 429 doubles, 1,081 vtables, 1,080 RTTI records | str_<text>, kF32_<value>, kF64_<value>, C__vtable, C__rtti (.bss data has no value: no name) |

Functions with a name: 3,250 (dtk + RTTI) -> 9,074 of 34,037 (26.7 %). Findings on the way: the pointer a constructor
stores is the vtable header word holding the class's OWN RTTI record pointer (CameraModeMtnViewer: record says
0x8051D520, code stores 0x8051D52C), located per class; a shared message ("Memory allocation failure", 263 copies) is
no identity evidence. NOT used: extract/functions-named.csv (provenance unclear; e.g. LandManager_80047338), the
Dolphin signature list (rejected earlier). The generated C++ (runtime/tests/generated/) is now lifted from pal-v3:
same bodies, names in comments and in the table.

Next naming evidence (not done): debug / assert strings that name their function; names carried from the project's
RE docs with a citation (e.g. fn_800A8160 = player update, docs/NEXT_SESSION.md); field names from RTTI-typed
accesses.

## Matching decompilation (2026-10-31 20:30): tools/dolrecon/match.py

Compilers: H:\Tools\dolrecon\compilers-20251118\GC (all 20 GameCube CodeWarrior builds, from the decomp community's
files.decomp.dev bundle that dtk-template uses), objdiff-cli 3.6.1 at H:\Tools\dolrecon\objdiff-3.6.1.
`python -B -m dolrecon.match <project> <src.c|.cpp> [--fn NAME] [--cc 2.6] [--flags ...] [--sweep] [--objdiff]`.
A function is MATCHED only when M1 (same size), M2 (every word equal outside relocated bits), M3 (relocations at the
same offsets, types, addends) and M4 (same target symbol, or for a candidate's own literal/static the same bytes at
the original target in a section of the same class; pointers inside data compared by target) all hold. `// @sym
<candidate> = <original>` lines rename symbols one-to-one (a conflict is an error). --objdiff runs objdiff-cli on
the same pair: a difference objdiff reports that is not a literal operand it cannot read (the original literal lives
in another split object; M4 reads it from main.dol) un-matches the function (X1). Controls: a wrong literal
(20.0f -> 21.0f) is rejected by M4.

First results: fn_80008E1C, CameraModeMtnViewer::vf0 and ::vf2 match byte for byte (objdiff 100 / 100 / 99.31 with
exactly the 4 literal operands explained). vf2 matches under 1.3, 1.3.2, 2.0, 2.5, 2.6, 2.7 at -O4 (p or s, inline
on or off) and under no 1.0-1.2.5 or 3.0 build: the game's compiler is in 1.3-2.7. Next: more discriminating
functions (destructors, function-local statics, sp_counted_base::release from Boost source) to pin one version
and its flags, then a per-unit project layout (dtk units + recovered literal pools).

RTTI model corrected the same evening (corrections.rtti_classes): record {name, base list or 0}; base list
{record, offset}... 0; vtable = data object {record, offset, methods}. 1,620 classes, 6,988 slots (B6: all function
starts), up from 1,080 / 4,997 (root classes were missed). names.py D1 now names real vtables (C__vtable), base lists
(C__rtti_bases), records (C__rtti). Projects: pal-v4 (corrections), pal-v5 (names: 12,719 functions), byte-exact.
Game-logic layer map: docs/GAME_SPINE.md (tools/dolrecon/spine.py).

## Names, second round (2026-10-31 late): .build/dolrecon/pal-v10 (byte-exact)

New rules in tools/dolrecon/names.py (all with evidence in names.json; counts for PAL):

| rule | names | evidence |
|---|---:|---|
| M0 matched | 9 | source/decomp/matched.json (dolrecon.decomp_verify): compiles byte-identical; the only rule that may replace a name |
| N5 self-name | 36 | the only function using a message that names it: CRI `E02080849 adxt_ResetEntry: ...`, SDK `DVDOpenDir(): ...`, game `SetCameraDataManager::RequestFileLoad() ...` |
| N6 ctor | 1,157 | stores class C's vtable into `this` (r3 at entry, registers tracked exactly), never deletes it |
| N7 dtor | 1,367 | stores C's vtable into `this` and calls operator delete on it after testing the delete flag (extsh. of r4) |

names.json now carries a "classes" table: record, vtable, base list with offsets, slots, ctors, dtors, and sizeof
when proven by operator new sites (761 classes). Size evidence is checked against the hierarchy (a derived class is
never smaller than its base): 0 violations after three defects found and fixed on the way:
- a remembered `li r3, N` survived other writes of r3 (stale sizes);
- the base constructor called first at a site whose derived constructor was inlined gave the base the derived size
  (sites that store a vtable into the new object now count only that vtable);
- **this game uses virtual inheritance**: 0x802919BC tests the virtual-base flag (extsh. r4 at entry), stores
  BossCtrlUnit's vtable at this+0x30 (the virtual base) and the derived vtable at +4 from a table the RTTI scan does
  not list. Such constructors, and constructors that overwrite a known vtable with an unlisted one, get no class.
N2 construct (407 left) keeps the functions that load a vtable without storing it into `this` (factories, inlined
construction of other objects).

Source skeleton: tools/dolrecon/skeleton.py -> .build/dolrecon/skeleton/ (classes/*.h, functions.csv,
platform/boundary.h, README.md with coverage). Matched source: source/decomp/ (README there). match.py changes:
mapped data defined by a candidate is still compared by contents; @sym accepts addresses; unused @sym is reported;
compiles go through short paths (the 2002 compilers crash on long paths); `--extra` flags for sweeps.

## Mass matching, RenderWare naming, native RenderWare core (2026-10-31 night)

- **Game build settings found:** `-O4,s -use_lmw_stmw on -inline auto -Cpp_exceptions off -str readonly`, compiler
  1.3.2-2.7 (all give the same bytes so far). The stmw prologue needs size optimisation AND use_lmw_stmw (recovered
  CodeWarrior source, per Astra: stmw when > 4 saved GPRs, or size mode and > 1).
- **tools/dolrecon/autodecomp.py:** straight-line leaf decompiler (symbolic execution to C; variants: loads inlined
  where no store intervenes / temporaries, float constants as literals / as globals). 4,626 functions byte-identical
  in source/decomp/auto/<layer>/<unit>.c; with the hand-written files, decomp_verify: 4,636 / 4,636 matched.
  Tail calls and branches are not attempted; parameters and results are inferred and the files say so.
- **tools/dolrecon/coff.py + rwalign.py:** names the game's RenderWare from the RW 3.7 PC NULL static libraries
  (3,814 functions with modules, order, calls, strings). Rules: S0 seeds, C1 one-element call gaps, C2/C3
  caller/callee constraint intersection, O1 module order (whole segment must agree in callee count and size), S1
  shared string. 127 names (.build/dolrecon/rw/rw_names.json). Held-out seeds (6 runs, 60 hidden): 0 wrong. A
  behaviour check still found one wrong name from multi-element C1 gaps (0x804834A8 is RwV3dTransformPoint): C1 was
  restricted to one-element gaps; behaviour (the native oracle) is the final check of a name.
- **source/rwcore/:** the native RenderWare core (rwcore/README.md). First module: RwMatrixMultiply, RwMatrixUpdate,
  RwV3dTransformPoint, bit-identical to the original in 20,000 cases each (rwcore_matrix_tests, negative control
  fails).
- **Astra's review** (.build/dolrecon/astra/answer3.md): m2c (PPC MWCC backend) and decomp-permuter as next
  accelerators; doldecomp/mweppc (recovered CodeWarrior 2.0-2.7 source) as specification; verifier hardening still
  OPEN: fail decomp_verify on file-level errors, compare full relocation tuples and counts, bound unknown-size data
  only with string evidence; record matched bytes as well as functions.

## RenderWare names and connections from the RW 3.7 SDK (2026-10-31 night, second round)

Current named project: **.build/dolrecon/pal-v12** (byte-exact; 13,229 functions named).
- **rwalign.py, second round:** new evidence from the PC libraries: F1 ordered function-pointer lists (plugin and
  stream callbacks), D1 globals mapped one-to-one (24, e.g. lbl_8056E8D0 = geometryTKList, lbl_805F2598 =
  meshModule), D2 candidates must reference all mapped globals, K1 distinctive constants (32-bit immediates in the PC
  code). 206 RenderWare functions named (was 127). Held-out seeds, 10 runs / 100 hidden: 0 wrong, 39 recovered.
  Found on the way: uniqueness taken AFTER the size / callee-count filters named two functions wrongly when the
  filters dropped the true twin (RpMaterialUVAnimExists, size ratio 4.2); globals-only and constant evidence now need
  uniqueness BEFORE the filters (and >= 2 globals), call-graph evidence keeps the measured filtered rule.
- **rwheaders.py:** 2,183 prototypes and 174 callback types parsed from RW37SDK/Library NULL Hdr Files. Every game
  call to a named RenderWare function is checked for function addresses loaded into callback parameters (EABI
  register per parameter, same basic block): 57 game callbacks, 0 type conflicts (33 RpAtomicCallBack, 11
  RpMaterialCallBack, 10 RwFrameCallBack, 3 RwResEntryDestroyNotify).
- **names.py:** N8 (RenderWare functions + globals from rw_names.json, 230), N9 (callbacks: <caller>__<RwFunc>_cb,
  57); names.json "signatures": 175 exact C prototypes (RenderWare functions and callbacks). The skeleton's
  functions.csv has a signature column; platform/boundary.h lists RenderWare entry points with their SDK prototypes.
- **M0 provenance fix:** generated files (decomp/auto) give no M0 names (their names are the project's names of the
  day; matching proves code, not names). **Verifier fixes:** decomp_verify fails on file errors and on compiled
  functions that pair with no original (6 auto functions had silently dropped out after a rename); generated files
  pin every function by address (@sym), so renames cannot detach them; the matcher accepts a rename in either
  direction.

## Full verification of the C++ lift, three memory images (finished 2026-10-31 21:53)

`.build/dolrecon/lv3-merged.csv` (lv3-dol: DOL image; lv3-fh: Final Haunt MEM1; lv3-lh: Lethal Highway MEM1), all
34,045 lifted functions, 16 workers, 0 worker faults, **0 FAILED in any pass**. Merged: 22,430 verified (645,122
instructions, 67.1 % of them executed by the valid cases; 16,251 functions with every instruction covered),
10,579 unverified (too few valid random inputs), 942 unverified because a callee is not lifted, 94 because the
original touches hardware. Per pass: DOL 18,809 / FH 21,982 / LH 22,007 verified - real game memory images raise the
valid-input rate, as expected.

## RenderWare names, third round: wrong names found and removed, 310 names (2026-11-01)

Current named project: **.build/dolrecon/pal-v14** (byte-exact; 13,514 functions named; pal-v12 superseded).
The user asked to keep naming. Adding rules exposed wrong names in round 2 that its holdout (0 wrong) had not seen;
the rules were rebuilt around them, and every name is now also checked by evidence no rule uses.

Wrong names in pal-v12, each proven from the code:
- **RwErrorSet at 0x8047EC78 is `_rwerror`** (variadic: spills r3-r10 / f1-f8 for va_list, returns its first
  argument); the real RwErrorSet is 0x8047EBD4 (writes the error into the engine's error slot). Cause: C2 took the
  one candidate left after the size filter; CodeWarrior's variadic prologue makes _rwerror 6.5x its PC size, so the
  filter had dropped the true twin.
- **Six GameCube driver functions** named after PC functions (_rtImportBuildSectorTriangulize = the body of node
  "SubmitNoLight.csl", RwV3dLength, LtMapLightCB, RtLtMapAreaLightGroupCreate, rpPTankGENInsSetup,
  _rwFrameListStreamWrite): all call GX code with MMIO, so the NULL libraries cannot contain them.
- **0x8045B7B0 is LightTidyDestroyLight, not DestroyClumpLight** (_rpLightClose takes its address; on PC that slot is
  LightTidyDestroyLight). Each compiler inlined a different copy (MSVC: RpLightDestroy into LightTidyDestroyLight;
  CodeWarrior: DestroyClumpLight into RpClumpDestroy), so a single-neighbour rule could not tell them apart.
- Four more, by callees and globals: 0x80460FF0 is RpWorldDestroy (exactly its six callees, worldModule /
  worldTKList), not RpGeometryCreateSpace; 0x80488D58 is RwImageCreate (calls _rwPluginRegistryInitObject), not
  RwImageDestroy; 0x8048A508 is RwImageGammaCorrect (error calls only, imageModule), not _rt2dObjectStringInit
  (which calls Rt2dBrushCreate / font lookups); RwTextureDestroy is 0x8048DDDC (DeInitObject + raster destroy,
  textureModule), and pal-v12's 0x8045C2C0 is a 92-byte wrapper that calls it.
- A tool defect behind much of it: **coff.py dropped every relocation after a function's first reference to static
  data** (the string probe reused the variable holding the function's end), so PC call lists were truncated (e.g.
  RwTexDictionaryDestroy "called nothing"). Fixed: 10,943 PC calls / 15,313 references now read.

rwalign.py now (docstring has the full list): **T1** data tables paired with the PC libraries' data (pipeline node
definitions, RtDictSchema, interpolator info: coff.parse_data) and their pointers paired slot by slot; **C4** longer
call gaps agreed by >= 3 callers (finds _rwerror / RwErrorSet); C1 only when callee counts agree and no same-module
function could have been inlined (RpAtomicCreate inlines RpAtomicSetFrame on PC); C2/C3 need two known neighbours,
one of them no hub (RwErrorSet, RwEngineInstance), and must be mutual (no OTHER guest function fits the candidate);
acceptance checks **E1** (no driver code), **E2** (link order: not inside another library's block) and
**consistency** with every known neighbour (inlining-aware; a contradiction keeps the better-supported side).
Behaviour-proven functions (rwcore oracles) are seeds. The holdout now also counts a hidden seed's name given to
another function as wrong (it did not before).

Result: **310 RenderWare functions + 50 globals** (round 2: 206 + 24, of which >= 12 wrong). Holdout, 8 runs x 12
hidden seeds: 45 recovered, **0 wrong**, 43 not recovered. Independent checks, `python -B -m dolrecon.rwcheck`:
- field offsets (both builds 32-bit: a correct pair addresses the same struct fields; one constant shift allowed):
  named pairs median 1.0, random pairs (negative control) median 0.17; one pair left for review, _rt2dFontOpen
  (string evidence; guest offsets 2..16 vs PC 2, 4, 6, 204...). Rt2dGlobals fields sit 960 bytes further on PC.
- link order: 0 islands.
- RwCameraBeginUpdate = 0x8048652C (calls camera+0x18 = beginUpdate), RwCameraEndUpdate = 0x80486504 (+0x1C),
  RwCameraSetViewOffset = 0x80486554 (+0x78): read against the SDK struct; the older _mined note was one off.

Callbacks: rwheaders on the new names finds **235 callbacks** (round 2: 57; 0 type conflicts), mostly plugin
constructors / destructors / copy and stream read / write / size callbacks now that the RegisterPlugin functions are
named; 201 get N9 names, by role: `<caller>__<RwFunction>_<parameter>` (e.g.
rpClumpRegisterExtensions__RwCameraRegisterPlugin_destructCB). callbacks.json and rw_names.json globals are keyed
by address (names.py had silently skipped callbacks the analysed project had already named).

Generated source kept on current names: `python -B -m dolrecon.decomp_names <project> --pin-from <older>` pins every
external symbol of source/decomp/auto by address and renames pinned symbols to the project's current names (447
pins, 35 renames in 158 files); without it, the withdrawn wrong name RtLtMapAreaLightGroupCreate had detached two
matched functions.


## RenderWare names, fourth round: the GameCube driver by role, module order (2026-11-01)

Current named project: **.build/dolrecon/pal-v15** (byte-exact; 13,605 functions named; pal-v14 superseded).
- **GameCube driver, `python -B -m dolrecon.rwdriver <project> <spine> <out>`** -> rw/rw_driver.json, names.py rule
  N10. The driver has no PC twin, but every RenderWare driver registers through the same tables, and the NULL driver
  shows them with names. lbl_8051D400 is the GameCube {RwStandardFunc index, function} list (27 pairs, indices 1-28
  without 22 HINTRENDERF2B, listed in the same source order as NULL's saGStandards); lbl_8056F50C is the RwDevice
  block (its fpSystem is the only user of that list; same slots as NULL's dGNullDevice). 45 functions named
  Gcn<role> after the NULL names: GcnSystem, GcnRasterCreate (the "Failed to create surface" function),
  GcnRasterLock, GcnCameraBeginUpdate, GcnSetRenderState, GcnIm2DRenderPrimitive, GcnNativeTextureRead ...;
  _rwDeviceGetHandle (returns the block, as in NULL); pipeline nodes from their ".csl" names after the D3D9
  convention (GameCubeAtomicAllInOneNode / ...PipelineInit / RxNodeDefinitionGetGameCubeAtomicAllInOne,
  GamerCubeWorldSectorAllInOne* - RenderWare's own typo kept, SubmitNoLightNode, ImmInstanceNode). The GameCube
  prefix "Gcn" is a choice (no driver name survives in the game); the roles are evidence.
- **rwalign O2:** module order when the guest linker stripped functions: the guest run between two named functions
  of one module must embed into the PC module's run (up to 8 more) in exactly one compatible way. 45 more names.
  Driver API names that exist on PC (_rwDeviceGetHandle, ...) are seeds; the other driver functions are excluded.
- Result: **358 RenderWare functions + 56 globals aligned** (round 3: 310 + 50) plus 45 driver roles; holdout 8 x 12:
  45 recovered, 0 wrong; rwcheck: named median 1.0 vs control 0.20, flagged only _rt2dFontOpen (as before), 0
  islands. Of the 30 O2 names, the few with disjoint field offsets were read: list-node arithmetic with negative
  offsets on PC and indexed loads on GameCube (measurement limits, not contradictions). 239 callbacks.

## Names from string pools and CRI error texts (2026-11-01)

Current named project: **.build/dolrecon/pal-v16** (byte-exact; 13,733 functions named; pal-v15 superseded).
- **String pools, `tools/dolrecon/strrefs.py`.** CodeWarrior gathers a unit's literals into one data object and
  reaches each as pool address + offset in a register (`addi r31, r3, pool@l` ... `addi r3, r31, 0x24`). A
  relocation names only the pool, so names.py saw none of these strings and rwalign read every user's string as the
  pool's FIRST one (it credited "adxt_StartSj" to the wrong function). strrefs follows lis / addis / addi / ori /
  mr values per basic block (r2 / r13 = the small-data bases from the startup code) and counts a string start only
  when it is used as one: passed to a call, stored or returned - not when it serves as a base for offsets or loads
  (the first two versions did; EGGFLEET.BIN was the check: the function passes it twice with the cannon model names).
  13,713 pool strings, 2,483 references in 1,749 functions. names.py (N3, N5) and rwalign (S1) use it.
- **N5 self-names, extended:** CRI's other message forms ("(adxf_Stop)" at the end, "cvFsGetStat #1:",
  "2103102:SVM:svm_unlock:", for library-shaped names only) and owners by elimination: inlining copies a callee's
  check, message included, into its callers (adxt_Stop's message is also in adxt_StartSj and adxt_Destroy), so a
  function takes name F when F is its only self-name not yet given away and no other function is in that position.
  N5: 36 -> 70 (adxt_Pause, adxt_Stop, adxt_Destroy, adxt_GetErrCode, adxf_Stop, cvFsOpen ... cvFsAddDev,
  mwPlyCreate, mwPlyCreateSofdec, MWSFCRE_ResetSfdHn, LSC_Create, DSP_boot_task ...); N3: 151 new ref_ names
  (pooled file / model / message texts), 23 lost where a text turned out to have more than two users.
- RenderWare unchanged (358; its strings are rarely pooled). _rt2dFontOpen, the one field-offset flag, read and
  confirmed: the same two "fonts\" strings, the same callees and free-list globals; the game also calls
  Rt2dFontSetPath (inlined on PC); its fields go through Rt2dGlobals at other offsets.

## Astra review 4 (high effort) and what it changed (2026-11-01)

Question and answer: .build/dolrecon/astra/question4.md / answer4.md. Defects Astra found in the evidence gates,
all fixed (regression tests: `python -B tools/test_dolrecon_evidence.py`):
- strrefs: `mr` of a pool base counted its first string as used; `stfs f3` was taken as storing r3.
- rwheaders: `bctrl` / `blrl` did not clobber argument registers (a stale callback could be credited to a later
  RenderWare call).
- match.py: X1 accepted a failed objdiff run (no comparison = pass); one relocation per word was kept (a second went
  unchecked) and relocation sets were not compared in full; local data without a size was compared up to its first
  zero byte even when it was no string.
- rwdriver: the 12-byte getter's shape was not checked; a second standards table did not clear the first's names.
- rwcheck: never failed; now exits 1 while a listed pair is not in REVIEWED (read by hand, with the reason).
Measured effect: no name changed (pal-v17 = pal-v16 name for name; callbacks 239 before and after); the stricter
X1 un-matches one function, fn_80427274 (M1-M4 exact; objdiff cannot load unit auto_ref_EFFD_text: "Symbol
relocations must have zero addend") - 4,635 / 4,636 matched, the one recorded with its reason.
Open from the review (not done yet): N5 elimination can give an internal's name to an API wrapper (CRI adxt_* names
on functions bracketed by 0x803BD244 / 0x803BD240 and called by game code - kept, flagged); M0 should record body
match and identifier provenance separately; rwcore oracle pins only first / last words (hash whole bodies, add
boundary / subnormal cases); O2 / mutual uniqueness still use filtered candidate sets.

CRI reference policy: H:\Tools\refs\3s-decomp is AGPL-3.0. Only names, order and facts are taken from it (crisrc.py
index); no body or header text goes into source/decomp - matched CRI source, if any, is written from Shadow's
instructions.

## Object-type registrations, rule N11 (2026-11-01)

`python -B -m dolrecon.gamereg <project> <out>` -> game/registrations.json. The table 0x8052C1A0 (286 type records,
null-terminated) holds per type {name, +4, +8, +0xC create, +0x18 layout id, +0x1A kind}. Roles read from their
dispatchers (checked in the binary before naming): +4 in the stage set loader's loop 0x800CA780 (setid.bin, gate
+0x1B bit 0) -> <TYPE>__stageLoad; +8 in the teardown loop 0x800CA730 -> <TYPE>__stageRelease; +0xC after the
spawn check of a layout slot, 0x800CA8D8 -> <TYPE>__create. 640 callbacks named (505 were fn_), 187 of them 4-byte
empty bodies (counted apart); 121 shared callbacks left unnamed. Independent check: every stage-load callback that
loads a file loads its own type's assets (143 of 143; e.g. ART_GADGET -> ARTISTGADGET.BIN, ELEC_BAR -> BAR.BIN,
EggFleet -> EGGFLEET.BIN, which N3 had named ref_EGGFLEET_BIN).

## Class roles from Task / Gadget dispatch, rule N12; RTTI table corrected (2026-11-01)

**RTTI table (corrections.rtti_classes), two defects fixed.** (1) CodeWarrior emits a class's {name, bases} record
in every unit that needs it; when a name had several records the first was kept, often one no vtable points at.
Task has one beside ResourceManagerTask's base list (0x805E45AC) and one under its own vtable 0x8051E768
(0x805E4A3C): Task, Gadget and 71 other base classes (Enemy, Player::PlayerBase, Mission::Mission, ...) had no
methods. Now the record a primary vtable points at is the class's ("records" lists all). (2) A pure virtual's slot
is 0, and the vtable stopped there: Gadget {dtor, update, 0, blr, 0, checkRemove}; boost::detail::sp_counted_base
{dtor, 0, destruct, 0} = its source's pure dispose / get_deleter. Slots are now None inside dtk's object (trailing
zeros dropped); every consumer skips None. 6,988 -> 7,875 slots (B6: all on function starts) + 43 pure;
corrections' slot names 4,549 -> 5,042, ambiguous 138 -> 12. pal-v4 regenerated (byte-exact).

**Task / Gadget contracts (dolrecon.classroles -> game/classroles.json).** Read from the code, instruction words
checked before anything is named:
- Task's constructor 0x8004F014 stores vtable 0x8051E768 at this+0x18, links the task under a parent: +0x10
  parent, +0x14 first child, +0x8 / +0xC sibling ring; +0 a class-name pointer, +4 u16 flags, +0x20 u64 time.
- The task-tree walk 0x8004ECAC (recursive): a child with flag bit 0 (kill) gets its subtree marked
  (0x8004EECC) and walked, then vtable[0](c, 1) (deleting destructor); bits 1-3 suspend it and its children; else
  vtable[1](c, dt) (0x8004ED98; while the pause byte 0x805EF27C is set only tasks with flag 0x100 run), the timed
  variant 0x8004EDF0 when profiling is on, then its children. -> **slot 1 = update(dt)**.
- Gadget (Task at offset 0, 6 slots): its update 0x8015AD28 is `if (!this->vf5()) this->vf2(dt)`; Gadget's own
  slot 5 0x8015AC7C sets the kill flag and returns 1 when the layout record at +0x28 is out of range (100 x its
  range byte) or its removal condition holds. -> **slot 5 = checkRemove, slot 2 = exec(dt)**.
- Gadget slots 3 (default an empty blr, 137 classes) and 4 (pure, 156 distinct bodies, takes a second argument)
  have no dispatcher found yet: they keep vfN.

A body gets Owner__role only when every slot holding it is that one role slot (classes reaching the base through
offset-0 bases), Owner = the holder all others descend from. 385 names: update 222, exec 157, checkRemove 6 (10
four-byte bodies), all replacing corrections' C__vfN placeholders (N12 may replace only those). Independent check
(no dispatch evidence used): the first instruction touching f1 (dt) - update 34 % read it / 44 % call first with it
live, exec 64 % / 12 %; controls from the same classes: slot 0 destructors 0 % / 0 %, Gadget slot 4 0 % / 93 %,
slot 3 5 % / 42 %.

**names.py spelling defect found on the way.** ident() stripped leading underscores and collapsed `__`: 97
RenderWare internals were stored as rpClumpOpen instead of _rpClumpOpen (N8, one N10), every N11 name read
EggFleet_create instead of EggFleet__create, and N12 would have read Task_update. names.symbol() keeps an
identifier as it is (other text goes through ident() one `__` part at a time). Tests: RttiTests, ClassRoleTests,
SymbolNameTests in tools/test_dolrecon_evidence.py (16 pass; the RTTI tests fail when either fix is reverted).

**Result: .build/dolrecon/pal-v20** (pal-v18 / v19 deleted): 15,134 functions named (pal-v18 14,302), byte-exact;
N7 dtor 1,366 -> 1,434, N6 ctor 1,157 -> 1,198, N2 427, D1 vtables 1,500 -> 1,573, N12 385, N4 6,469. Spine: game
12,651 named (11,854), rw 529 (508). decomp_names re-pinned source/decomp/auto; decomp_verify 4,635 / 4,636 (the one
unverifiable fn_80427274, as before), 0 file errors. Run order now: `corrections pal pal-v4` -> `gamereg` /
`classroles <pal-v4> game` -> `names pal-v4 <a>` -> `rwheaders <a> rw` -> `names pal-v4 <new>` -> `decomp_names
<new> --pin-from <old>` -> `decomp_verify <new> --objdiff` -> `spine` -> `skeleton`.
Next on this line: dispatchers of Gadget slots 3 / 4 (calls through a typed pointer, or member-function pointers);
the same contract method for the other bases that now have vtables (Player::PlayerBase / Behavior / Executer,
Enemy*, Mission::Mission); Task field names from the walk (+0x10 parent, +0x14 child, +4 flags) into the skeleton.

## Class roles of three more bases: sp_counted_base, TEnemyAIState, Behavior (2026-11-01)

Chosen by what is still a placeholder: of 3,233 `C__vfN` names in pal-v20 the largest groups by introducing slot
were sp_counted_base slot 1 (217 bodies), Gadget 4 (156), TEnemyAIState 1-4 (84 each), PlayerCommand 0 / 1, Behavior
1-6 (38 each). Each contract below is in dolrecon/classroles.py with its instruction words (CHECKS) and a type proof
(classroles.type_errors, from constructors) that the dispatched object is that base; either failing names nothing.

**Finder: dolrecon/vcalls.py -> game/vcalls.json** (names nothing). vptr offset of each class from the code that
stores its primary vtable into this (1,480 classes; 3 with conflicting offsets get none); `this` type of each
vtable body (owner, as classroles.assign) and constructor; calls `lwz r12,K(this); lwz r12,8+4s(r12); mtctr;
bctrl` on this (225) or on a field this->F (289), with registers tracked by a forward must-analysis over the
function's blocks. Defect found by its own test before use: calls were recorded during the iteration, before a loop's
back edge had removed `this` from a register (the Task walk's shape: r31 = this, then r31 = r31->next) - reporting
now runs once at the fixed point. classroles.json was identical before and after the fix (the type proofs did not
depend on it).

**boost::detail::sp_counted_base** (vptr +0). Every sp_counted_base_impl constructor (0x8000D500 ...) stores the base
vtable, then 1 into +4 and +8. fn_8000945C is boost 1.31/1.32's sp_counted_base::release without threads: --(+4) == 0
-> **slot 1 dispose** (0x80009484), then --(+8) == 0 -> **slot 2 destruct** (0x800094A8); the base's slot 2 body
0x800094C8 is `if (this) this->dtor(1)`. The bodies agree: of the 217 slot 1 bodies 211 call the deleter object at
+0x10 with the pointer at +0xC and 6 call a function-pointer deleter stored at +0x10 with it; all 217 slot 3 bodies
compare the type_info argument with the deleter's and return this+0x10 (boost's get_deleter; no dispatcher in the
binary, so slot 3 keeps vfN). sp_counted_base_impl exists up to boost 1.32, whose slot 2 is named destruct.

**TEnemyAIState** (vptr +0). EnemyBaseAI: current state +0x2C, previous +0x30, owner +0x28. A state change (slot 11
0x8019D8C0; slots 13-20, 22 are the same code with other targets) takes the new state from a getter (fn_8019D970:
a function-static object constructed by EnemyAIState_EnemyBaseAI_Idle's constructor, checked to derive from
TEnemyAIState), calls cur->**slot 1 setOwner**(owner), cur->**slot 4 exit**, sets prev = cur, cur = new, then
setOwner and **slot 2 enter**. EnemyBaseAI slot 1 0x8019EACC stores its f1 at +0x20 and calls setOwner and
**slot 3 execute** on the current state. setOwner bodies: 72 are `owner = dynamic_cast<XxxAI*>(arg)` stored at +4,
12 store it unchecked. execute is not called "update": that EnemyBaseAI slot 1 runs every frame is not proven here.

**Player::Behavior::Behavior** (vptr +0). Player::Executer (a Task) keeps the current behavior as a shared_ptr at
+0x30 and a factory object at +0x38. The change 0x800A82FC: new = factory->slot1(id) (ShadowBehavior slot 1 0x800AE8D0
indexes the record table 0x80522DE8; each of its 35 records' factory constructs a Behavior subclass, Idle ... Super,
checked); new->**slot 1 canEnter**(previous id, args), zero refuses; cur->**slot 3 exit**(new id +0x10); cur = new;
cur->**slot 2 enter**(previous id, args). The executer step 0x800A8160 (called by SonicTypeExecuter / ShadowExecuter
update) takes the time step from player +0x60 and calls cur->**slot 5 update**(f1). The command handler 0x800721A8
(passes its this to the change; dynamic_casts its argument from CharCommand to DamageCommand) calls
cur->**slot 4 onCommand**(command). Of the 130 calls of the change, 62 are in behaviors' slot 5 (update) bodies, 8
in slot 4, 2 in slot 2, 58 elsewhere. Slot 6 (19 empty bodies) has no dispatcher found.

**Result: .build/dolrecon/pal-v21** (pal-v20 deleted): N12 385 -> 1,125 (update 260, exec 157, checkRemove 6, dispose
217, destruct 1, setOwner / enter / execute / exit 84 each for states, canEnter 38, enter / exit 38 each, onCommand
34, update 38 for behaviors; 73 four-byte bodies; none skipped for a conflicting slot). `C__vfN` placeholders 3,233 ->
2,493; helpers named after them (`C__vfN__subK`) 1,761 -> 1,494 (they now read e.g. HomingAttack__enter__sub2).
Functions named stays 15,134 (placeholders already counted), byte-exact. rwheaders' callbacks.json from pal-v21
has the same 271 callbacks (41 keys were fn_ in the pal-v20 copy, 2 had the old spelling); the second names pass
gave identical symbols. decomp_names re-pinned 29 files (121 symbols); decomp_verify 4,635 / 4,636, 0 file errors.
Independent check (first instruction touching f1): Behavior update 59 % read it, every
other Behavior and state role 0 %; Task update 35 %, Gadget exec 65 %. Tests: VirtualCallTests (vptr offset from a
constructor, a typed call on this, the loop case) - 19 pass.

**Defects seen on the way, not fixed here.** (1) N2 `C__construct` = any non-virtual function that loads C's vtable:
0x803A1AFC is CodeWarrior's __dynamic_cast (it builds a std::bad_cast to throw) but reads std__bad_cast__construct;
the Executer command handler 0x800721A8 reads Player__Behavior__InitializeArgs__construct because it builds
InitializeArgs temporaries on the stack. 427 N2 names need a dataflow check (vtable stored into this / a new object
vs a stack temporary). (2) N7 names a derived class's deleting destructor after the base when the base vtable is
the only one it stores: 84 of the 85 TEnemyAIState__dtor* names are slot 0 of exactly one EnemyAIState_* class.

## N2 / N7 naming defects fixed (2026-11-01)

Both defects above are fixed in names.py, with the store analysis on dolrecon.vcalls' must-analysis
(vcalls.vtable_stores: every `stw` of a loaded vtable address, by target - this, the object operator new returned,
a stack slot, a constant address, other).

- **N2 / N6** (names.construction). Of the 427 old N2 names: 288 store C's vtable into this at C's vptr offset (the
  offset every constructor of C uses; 278 are constructors with the virtual-base flag that N6 skips, because there
  the stored vtables need not name the class - a virtual base's vtable sits at another offset) -> now N6 C__ctor;
  97 store it into the new object of an inlined `new C` -> N2 C__construct as before; 42 build a C only on the stack
  or store it nowhere -> no name (12 got another rule's name: 5 N3 string, 7 N4 helper; 30 are fn_ again). The fact
  is kept as names.json "class_uses" {address: {class, evidence}}, which spine.py A3 reads - A3 had parsed the class
  out of N2's evidence TEXT, and losing those names first moved operator new / delete's unit from msl to
  "unresolved". With class_uses every function keeps its pal-v21 layer.
- **N7** (names.dtor_class). A deleting destructor is named after the class whose vtables hold its body when that
  class derives from the stored class: 431 renamed (EnemyAIState_EnemyBaseAI_Caution__dtor, not TEnemyAIState__dtor;
  sp_counted_base_impl<...>__dtor, not sp_counted_base__dtor); no body sat in an unrelated class's vtable.
- vcalls `mr.` (record form) now counts as a copy (the `new C` result is usually tested with `mr. r31, r3`).

Tests: ConstructionTests (store kinds incl. mr., stack temporary -> nothing, vptr offset -> ctor and a virtual base
at a foreign offset -> nothing, new object, destructor class) - 24 pass; the mr. test fails with the old is_mr.

**Result: .build/dolrecon/pal-v22**: N6 1,198 -> 1,486, N2 427 -> 97, N3 259 -> 264, N4 6,469 -> 6,476, N7 1,434 (446
renamed); 15,104 functions named (15,134: the 30 wrong names removed); byte-exact; rwheaders' 271 callbacks
unchanged; decomp_names re-pinned 13 files (24 symbols); decomp_verify 4,635 / 4,636, 0 file errors. Spine game
12,624 named, msl 58 (layers identical to pal-v21). Astra question 5 (.build/dolrecon/astra/question5.md): where the
same defect classes still live, and how to restructure the tools.

## Astra review 5: shared decoder, CFG-proven evidence (2026-11-01)

Question .build/dolrecon/astra/question5.md (where the defect classes of this session still live; how to restructure
the tools), answer answer5.md (GPT-6-Astra, high, read-only; 17 ranked items, deductions from the code, not measured
misnames). Done in this round:

- **dolrecon/ppc.py**: one instruction-effects decoder (GPR writes per family, branches with AA / LK / BO). It fixes
  what vcalls.writes() had wrong: stwux / stbux / sthux / stfsux / stfdux / lfsux / lfdux and psq_lux / psq_stux
  write rA, lswi writes ceil(NB/4) registers wrapping at 32, lswx is undecodable (count in XER); an undecoded word
  drops every fact. Volatile set r0, r3-r12 (r1 / r2 survive calls).
- **vcalls**: bc AA, absolute b sign-extended, no successor past the function end; bcl is a call; jump tables
  (`lwzx` of a constant table then mtctr / bctr) give edges to every in-function table word; interior entries
  (direct branch targets from other functions) start knowing nothing; sda (r2 / r13), stack and allocation
  addresses tracked symbolically; `vcall_at` proves a call by def-use in one block (no r12 / CTR write, no branch,
  no block boundary, displacement >= 8, r3 = the loaded object at the bctrl). vtable_stores keeps each store's
  object (this / allocation site / stack / global address) and offset, with unknown stores.
- **names.construction**: per (object, offset) the stored classes must form one base -> derived chain (no "last
  store wins"), an unknown store at a vptr slot or a vtable at another offset than the class's vptr offset (a member
  subobject) gives nothing, two objects of different classes give nothing. 6 N6 names withdrawn, e.g. 0x804271C8
  is not boost::detail::sp_counted_base's ctor: it stores the base vtable, then an unlisted derived one.
- **classroles.type_errors** proves returned-object provenance (vcalls.returned_construction): the state getter and
  each of the 35 behavior factories return, on every path, the receiver of a constructor that returns this and
  never calls operator delete (operator new / delete from decomp/matched.json). All contracts still pass.
- **rwheaders.scan_calls** reads instruction words on the must-analysis instead of assembly text per label block
  (lmw, update loads, mr., lis / ori, bla were mishandled) and keeps every use (names N9 saw only 8). 271 -> 275
  callbacks, none lost (0x8015E560: r28 set before a loop, passed in r4 on every iteration). N9 signatures by
  address.
- **strrefs.pooled_refs** on the must-analysis: a string counts where its value reaches a call argument, a store or
  a return on every path (the linear scan restored fall-through values at joins, lost in-place `ori r3,r3,lo` and
  `mr r3,r3`). names.py: a relocation to a string strrefs sees is no use of it (dtk splits a unit's literals into
  one object each; adxt_GetTime reaches its own message as adxt_SetKeyString's address + offset). N5 70 -> 62:
  functions whose messages now show more than one inlined self-name are left unnamed (mwPlyCreateSofdec's 14
  messages name 4 functions).
- **Spelling**: N1 and N5 keep valid identifiers (`__CARDRead`, `__OSGetDIConfig`, `SetCameraDataManager__RequestFileLoad`).
  corrections.sanitize collapses original underscores but changes no current RTTI class name.
- **Structured facts**: names.json rows of N2 / N6 / N7 carry "class" (N7 also "stored_class"); spine A3 reads it.
  spine A4: pooled strings per use, no layer from conflicting strings. Layer rules corrected (docs/GAME_SPINE.md
  "pal-v24"): G1 does not propagate "boost"; template / Boost / std anchors end S1 spans but seed none.

Tests: 46 in tools/test_dolrecon_evidence.py (PpcEffectTests, VirtualCallProofTests incl. jump table / interior
entry / bcl / receiver / r12 overwrite, ConstructionTests order-independence / overwrite / member offset / two
objects, ReturnedConstructionTests, CallbackEvidenceTests on words, PooledStringTests in-place ori / join).

**Result: .build/dolrecon/pal-v24** (pal-v21 / v22 deleted): 15,113 functions named, byte-exact; N9 211, N6 1,480,
N5 62, N3 286, N4 6,473; rwalign's 358 names unchanged; second names pass identical; decomp_names re-pinned 9 files.
decomp_verify 4,635 / 4,636, 0 file errors.

**Not done yet (Astra answer 5, in its order):** (1) N6 / N7 / sizes onto the CFG engine with a real deletion
predicate (the delete call control-dependent on extsh. of the flag), allocation identity and memory overwrites;
(8) RTTI: abstain on conflicting duplicate records / several primaries, keep secondary tables and thunks in
ownership; (11) facts keyed by DOL digest with producer / dependency IDs, so a stale JSON cannot feed a later pass;
(13) audit / corrections C2 `ori` source field, rwalign's linear high-half facts and first-name-wins tables,
rwheaders duplicate declarations, match / decomp_names alias choice; (14) gamereg / rwdriver / classroles: pin full
binary slices (pointer origin, CTR producer, predicates, the command dynamic_cast); then naming: shared_ptr copy /
assign summaries (0x8007259C, 0x800A8420), command slots, Gadget 4, member-function-pointer dispatch, a body rule
for sp_counted_base get_deleter.

## N6 / N7 / allocation sizes on the CFG engine (2026-11-01, Astra answer 5 item 1)

names.py's scan_object_code (a linear scan with one register state across branches, "last vtable stored" in
address order, a four-instruction countdown after extsh.) is gone. vcalls.object_code runs the must-analysis once
per function and reports the facts the rules need:
- **Delete predicate**: the entry flag (r4, its copies, its sign extension) is tracked; extsh. / cmpwi 0 of it sets
  a cr0 fact; a conditional branch on a cr0 bit adds the bit's value on each edge (CTR-decrementing and
  "branch always" forms add nothing; any other CR write or a call drops it). N7 needs operator delete called on
  this only where the flag is > 0 (GT set or EQ clear). A delete of this without that test (`delete this`) names
  nothing.
- **Constructors (N6)**: the class is the vtable each vptr slot of this holds at every return, on every path
  (the slot's last stored value is a must-fact; calls are assumed not to rewrite a slot after the function stored
  it). An unlisted base's vtable stored first is harmless; an unlisted derived vtable stored last, or paths that
  disagree, leave the function unnamed. A tested flag without a delete is a virtual-base constructor (N2 block).
- **Destructors (N7)**: the most derived of the stored chain (stores in any order), strict: another value at a vptr
  slot (an unlisted derived class) leaves it unnamed.
- **Allocation sizes**: `li r3, N` + operator new, then the class's vtable stored into that object at its vptr offset,
  or any call on the object (r3 = it) to an N6 constructor (the old scan took only the FIRST call, often a getter).
  Recorded and printed as an allocation size ("operator new allocates N bytes for it at every site found"), not as a
  proven sizeof.

Changes against pal-v24: N7 1,434 -> 1,433 (withdrawn: 0x80364D28 / 0x804270C0 / 0x804352C0, destructors of unlisted
derived classes named after their base; new: GameSystem__dtor 0x801E2C64; 0x80204E34 StageAction::EventAction is a
deleting destructor, not a constructor - its extsh. sat three instructions before the branch), N6 1,480 -> 1,479,
allocation sizes 767 -> 934 classes (167 gained, e.g. ElecAccessPanel 0x58: the first call after operator new was a
getter; none lost or changed). Tests: ObjectCodeTests (delete under the flag, wrong polarity, cr0 overwritten,
unconditional delete, last store with an unlisted base / derived, paths storing different classes, allocation +
constructor call) - 53 pass.

**Result: .build/dolrecon/pal-v25**: 15,110 functions named, byte-exact; classroles and callbacks identical;
decomp_names: nothing to re-pin. decomp_verify 4,635 / 4,636, 0 file errors. Spine: 10 functions moved (following the withdrawn destructor anchors).

## RTTI conflicts, secondary vtables and thunks in ownership (2026-11-01, Astra answer 5 item 8)

corrections.py splits the RTTI scan in two. `rtti_instances` keeps every record and every vtable instance; `rtti_classes`
builds the class view on it and abstains (`.conflicts`) when a name's records have different base lists, its primary
vtables different slots, or two secondary vtables at one offset different slots. Census of the PAL binary: 195 names
have several records, 0 conflict, 0 names have two primary vtables - the guard costs nothing here and keeps a
conflicting duplicate from silently choosing the first record. TEnemySetBase is a template whose RTTI name omits the
arguments (34 records, one primary vtable): its instantiations agree in structure, so they stand as one name.

What the scan was missing, found by measuring before changing anything:
- **Secondary vtables inside one data object.** dtk's object bounds stopped the method array at the next
  {record, offset} header. That header is now scanned too: 563 secondary vtables in 262 classes (BkChaos holds five in
  one object), 8,552 slots, every one a function start (audit B6 now checks 16,460 slots).
- **A leading pure slot.** A zero in slot 0 ended the array: 9 abstract interfaces had no vtable (IHeap {0, 0, 0, f},
  CameraMode, CameraAdjust, CameraFactory, RangeBase, CharaColliBase, CharaColliBaseCharInfo, PJSCameraMode,
  CameraTargetInfo). Their constructors store them (Heap's constructor 0x8040C048 stores IHeap's before Heap's), so
  the class's record is now the one the vtable points at (11 D1 rtti / rtti_bases names moved there).
- **Trailing zero words** (pure slots or alignment, undecidable from the table) are counted in
  `trailing_zero_words` (388 classes). vcalls.survey gives no introducing base for a call whose slot such a class may
  hide.

The base list is flattened. A CodeWarrior record's base list holds EVERY base subobject, indirect and virtual ones
included, at its offset in this class (EggPawnBase lists EnemyReferer, a virtual base, at 756; EnemyWalkerCommon at
624). A secondary vtable at offset -k belongs to the subobject at +k: `subobject` = the most derived class the list puts
there (563 of 563 resolve to one class).

Thunks (`vtable_thunk`), 1,647 secondary slots, each exactly its dtk function's size:
- `addi r3,r3,A; b T` with A = the table's offset (1,191; all of them). A method forwarding to a member has the same two
  instructions with A > 0, so A must equal the (negative) table offset.
- `li r11,F; lwzx r11,r3,r11; add r3,r3,r11; [addi r3,r3,A]; b T`: through a virtual base's offset field (456).

`slot_holders` lists every slot with its kind: primary; direct (a body in a secondary vtable: a method of the
subobject's class); thunk (compiler glue, never its target's name); override (a plain thunk's target: the table
class's implementation, since this += the table offset lands on its this); vtarget (a virtual-base thunk's target:
the offset is read at run time and may land on any subobject - 0x8018FF8C, reached so from BkWingLarge's
EnemyParamCommon table, is a body of its TEnemySetBase subobject - so it carries the slot's role but no class).
`body_classes` turns them into the classes whose this a function takes (None = a subobject without a single class).
C4 slot names, classroles.assign, names.py (holders, in_vtable), vcalls.survey (this types) and spine A3 all read it.
Spine A3 also marks a body "emitted in its users" by the class it belongs to (the holder all others descend from),
no longer by any template class that merely inherits it.

New names. C4 slot names 5,042 -> 5,507, none lost; 3 move to the base whose vtable is now found
(CharaColliBody::vf1/vf2 -> CharaColliBase, RangeTriangle::vf14 -> RangeBase). N13 thunk: all 1,039 thunk functions
-> <target>__thunk / __vthunk (every target already had a name). N6 / N7 +8 each: the constructors and destructors of
the abstract interfaces above. D1 vtable 1,573 -> 1,582 (secondary vtables that are dtk objects: C__vtable_0x<k>).
N4 helpers 6,473 -> 6,598 (more named parents). Class roles identical (1,125): no role body sits in a conflicting
secondary slot. Two defects found by the tests on the way: thunk targets through a virtual base first claimed the
table class, and C4 counted a virtual-base thunk's slot as the class's own slot (Delta::vf3).

Tests: MultipleInheritanceTests (conflicting duplicate records, conflicting primaries, leading pure slot and
trailing zeros, embedded secondary vtable and its subobject, plain and virtual-base thunks next to a positive-offset
forwarder, holders keeping thunk and target apart, a virtual-base target with no class claim, an ambiguous subobject,
a body reused by an unrelated secondary slot) and two ClassRoleTests (a role body in an unrelated secondary slot; an
inherited body through a secondary vtable keeps its role) - 64 pass. Mutation checks: accepting any `addi r3,r3`
as a thunk fails 2 tests, primary-only ownership fails 1.

**Result: .build/dolrecon/pal-v26** (pal-v25 deleted): 16,746 functions named (pal-v25 15,110), byte-exact;
callbacks identical; decomp_names re-pinned 20 files (122 symbols). decomp_verify 4,635 / 4,636 (the unverifiable fn_80427274, as before), 0 file errors. Spine: game 27,779 -> 28,056,
game-unit 1,206 -> 981, unresolved 876 -> 824 (A3 anchors 6,697 -> 8,198); no function left the game layer.

## Static storage: static initializers, exit-time destructors, singleton getters (N14 / N15, 2026-11-01)

First naming expansion after Astra answer 5 item 16. Census of pal-v26 first: 17,290 functions were still fn_; 7,104
of them have one direct caller that is itself unnamed (N4 cannot reach them), 1,566 are reached only from data.
Among the data-only ones, `.ctors` held 281 unnamed functions.

CodeWarrior's start-up / exit runtime, read from the binary and pinned word for word (names.RUNTIME_WORDS; a
function of that name doing anything else gives no N14 / N15 name):
- `__init_cpp` (from `__init_user`) builds `_ctors`' address and loops `mtlr r12; blrl; r31 += 4; r12 = *r31;
  cmplwi r12, 0; bne`: every .ctors word before the first 0 is called once, before main. That is a unit's static
  initializer (CodeWarrior's `__sinit_<file>_cpp`; the file name is not in the binary) -> **N14 `__sinit_<address>`**,
  281 (all before the first 0; the table is 288 words with 6 zeros at its end).
- `__register_global_object(object r3, destructor r4, node r5)` stores {chain, r4, r3} into the node and links it;
  `__destroy_global_chain` pops each node and calls `node->r4(node->r3, -1)`. The constant r4 at a call (245 sites,
  all constant) is that object's destructor: 103 of the 193 are already N7 destructors (an independent check of N7).
  A destructor registered by one function G -> **N14 `G__static_dtor[k]`**, k = its order among G's registrations,
  applied with N4 as registrants get names.
- **N15 singleton getter** (Astra: connect guard, constructor receiver and returned address). `names.singleton`:
  exactly one `__register_global_object` call, whose object O is a constant (kept in r3 across constructors proven to
  return this, vcalls.returns_this) and whose destructor D is a constant, and every reached return gives O or a load
  of one global cache C (vcalls.Function.loads: `lwz rD, d(constant | r13)` gives ("load", address)).
  `cache_holds_only`: C starts as 0 or O, and every direct store into its 4 bytes - r13-relative, or constant-based in
  a function that builds its high half (`store_index`) - is a `stw` of O or 0. Name: `C__instance` when D carries a
  class (N7), else `static_<O>__instance`. Not covered: a store through a pointer the must-analysis does not know.

Result: N14 static init 281, N14 static dtor 79, N15 singleton 152 (94 with a class, e.g.
`EggLastMech__AIState__Nop__instance`; 47 through a cache; 91 were fn_, 61 had only an N4 placeholder). N4 helpers
6,598 -> 6,917. 17,577 functions named (pal-v26 16,746), no name lost; spine layers unchanged. Tests:
StaticStorageTests (runtime words in order / swapped, ha-adjusted address, constant destructor argument, a call
clobbering it, a getter returning its object / its cache / another object, a constructor not proven to return this,
a cache written elsewhere with another value) - 75 pass.

**Result: .build/dolrecon/pal-v28** (pal-v26 / v27 deleted): byte-exact; callbacks identical; decomp_names re-pinned
125 files (166 symbols). decomp_verify 4,635 / 4,636 (the unverifiable fn_80427274, as before), 0 file errors.

## Boost shared_ptr / shared_count by body, rule N16 (2026-11-01, Astra answer 5 item 16)

The copy and assignment calls the Behavior contracts pass through (classroles: 0x8007259C, 0x800A8420) read, with
their callees, as Boost 1.32 exactly (this = r3, r = r4; shared_ptr {px, pn}, shared_count {pi},
sp_counted_base {vptr, use_count +4, weak_count +8}):

| function | body | Boost |
|---|---|---|
| 0x8000945C | --use_count; 0: dispose (vptr +0xC), --weak_count; 0: destruct (vptr +0x10) | sp_counted_base::release (the dispatcher of the classroles contracts) |
| 0x800097E0 | pi = r.pi; if (pi) ++pi->use_count | shared_count(shared_count const&) |
| 0x80009E4C | t = r.pi; if (t != pi) { if (t) ++t->use_count; if (pi) release(pi); pi = t } | shared_count::operator= |
| 0x80009408 | if (pi) release(pi); delete under the flag (dtk: dtor_80009408) | shared_count::~shared_count |
| 0x8007259C | px = r.px; shared_count copy of pn; return this | shared_ptr(shared_ptr const&) |
| 0x800A8420 | px = r.px; shared_count assignment of pn; return this | shared_ptr::operator= |
| 0x800093B8 | ~shared_count(&pn, -1); delete under the flag | shared_ptr::~shared_ptr |

So the payload pointer is copied and the control block is shared with its count raised: a copy or assignment
keeps the object's identity (the summary Astra asked for before typing through shared pointers).
`dolrecon/boostsp.py` pins the seven bodies word for word, external calls by role. The four primitives are
non-template code (exactly one body each, else the role and its callers abstain); a shared_ptr<T> member is emitted
per instantiation and unit, so every function with its body is one: copy 234, destructor 132, assignment 118.
**N16** names them boost__detail__sp_counted_base__release, boost__detail__shared_count__copy_ctor / __assign /
__dtor, boost__shared_ptr__copy_ctor / __assign / __dtor (address suffixes) with class = the Boost class; it may
replace dtk's dtor_XXXXXXXX placeholder only. 488 names: 344 were fn_, 141 N4 placeholders (e.g.
Player__PlayerShadow__vf10__sub46 - a Boost instance, not a helper of that method), 2 N14 static destructors
(global shared_ptrs), 1 dtk placeholder. Tests: BoostSharedPtrTests (every role; a second primitive copy abstains
with its callers; a copy body calling the assignment is an assignment; an unknown callee or a payload read from
another offset is nothing) - 80 pass.

Spine A3 now reads N16's class: 488 functions are boost (479 had been game: 285 by S1 link order, 194 by G1
callers - the weak spot noted in docs/GAME_SPINE.md). Their neighbours lost S1 claims: game 28,056 -> 26,952,
game-unit 981 -> 1,332, unresolved 824 -> 1,089, boost 878 -> 1,366. Kept so: S1 "game" inside a game unit always
meant game code or an unrecognised template instance, and these 479 were the proof.

**Result: .build/dolrecon/pal-v29** (pal-v28 deleted): 17,920 named, byte-exact, nothing lost; callbacks
identical; decomp_names nothing to re-pin. decomp_verify 4,635 / 4,636 (the unverifiable fn_80427274, as before), 0 file errors.

## Astra review 6: defects in the new rules fixed (2026-11-01)

Question and answer: .build/dolrecon/astra/question6.md / answer6.md (effort max: the API's levels are none, minimal,
low, medium, high, xhigh, max - ask.ps1's old default "ultra" did not exist). Astra read the code only; each fix
below was then measured on the binary.
- **N15 singleton** proved one registration and the returned value, not "constructs once"; its positive fixture was
  unguarded. The helpers moved to dolrecon/staticstore.py. `singleton` now proves the protocol:
  - one registration of a constant object O;
  - a constructor proven to return this ran on O on every path (vcalls ("built", O));
  - the registration block is dominated by a block entered only where a guard byte tested 0 (an "isz" edge fact);
  - the guard is set non-zero in the block right after the registration call;
  - every return (blr or a conditional bclr) gives O, or a cache read where it is known non-zero.

  `cache_holds_only` looks writers up by both the ha-adjusted and the plain high half (lis + ori), r2 and r13
  stores, and every code section. An indexed store near the cache is unprovable, and so is an unmapped cache outside
  .bss / .sbss. 147 getters (152): 5 construct their object inline (no constructor call, e.g. 0x8001FD6C allocates
  and links a free list), so their construction is not proven; their 5 destructors lose the getter-based name.
- **N14** checked an ordered subsequence (`in_order`) and an unrelated lis / addi pair. RUNTIME_BODIES now pins the
  three runtime functions whole: every word exact except the _ctors halves, which must build _ctors, and the
  destructor chain's r13 offset, which must be one value in every access. exit_destructors records each
  registration's object and node, and lists registrations without a constant destructor (0 in PAL).
- **N7** accepted "flag != 0" (EQ clear); the exit chain passes -1, so a delete must be on the GT edge. The N2 / N6
  fallback named functions without the first pass's eligibility; it now skips any function that deletes this.
  `finals` record conditional returns. No N6 / N7 name changed.
- **vcalls engine.**
  - D-form addresses with rA = 0 are the literal 0: `lwz rD, d(0)` was read as this->d when r0 held this.
  - `returns_this` / `returned_construction` / `singleton` see conditional returns (`is_return`).
  - Construction is a path fact ("built", V), merged by intersection. A null test of operator new's result
    (`mr.` + beq) marks the null path as vacuously built: CodeWarrior's `p = new X; if (p) X(p); return p`.
  - Static-storage facts (Function.statics: global loads, zero-test edges, stores, call / unknown-store kills).

  A constructor run on one branch only no longer proves the returned object. classroles' state getter 0x8019D970 is
  now proven as a static getter (staticstore.singleton plus the class of the constructor run on its object): it skips
  construction on its guard path, which the path fact correctly no longer accepts as "constructed".
- **RTTI consumers.** names.py keeps unknown ownership (None) apart from no holder, and an unknown one leaves a
  deleting destructor unnamed. rtti_classes(..., functions) accepts a thunk only when it is its whole function and
  its target is a function start (all 1,039 pass). N13 evidence lists every holder. The skeleton no longer writes the
  flattened base list as direct `public` bases: it lists the base subobjects, and the ones no other listed base
  derives from, as comments.
- **N12 names cut at 96 characters dropped the role**, e.g. boost__detail__sp_counted_base_impl_..._boost__check
  for a dispose. `names.fit` keeps the last `__` part: 185 names now end in __dispose / __destruct.
- **Spine.**
  - A class used, but not implemented, gives a layer only for a game class: N2 construct or class_uses of a Boost /
    std class no longer makes the user Boost / std.
  - G1 may not decide "game" inside an S2 interval (between a game anchor and a Boost anchor), where code called
    only by game code may be a template instance.

  game 26,952 -> 22,015, game-unit 1,332 -> 5,584, unresolved 1,089 -> 1,795. The 708 that became unresolved are
  the callees of those functions, outside the intervals: their only callers are now undecided.

Tests:
- StaticStorageTests: whole runtime bodies with a changed chain store, two chain offsets, another _ctors and another
  instruction; a recorded registration object; the guarded getter, unguarded, guard published before construction,
  wrong guard address, cache returned where it may be zero, constructor not returning this; lis + ori writer,
  indexed store and unmapped cache outside .bss.
- ObjectCodeTests: delete under "flag != 0"; conditional return.
- ReturnedConstructionTests: null-checked factory; constructor on one branch only.
- MultipleInheritanceTests: thunk extent and target entry.
- SymbolNameTests: long names keep their role.

94 pass.

**Result: .build/dolrecon/pal-v30** (pal-v29 deleted): 17,910 functions named (pal-v29 17,920: -5 getters with inline
construction, -5 of their destructors), byte-exact; corrections, classroles and callbacks identical. decomp_verify 4,635 / 4,636 (the unverifiable fn_80427274, as before), 0 file errors.
Not done from answer 6: its infrastructure and capability items (analysis service, fact store, rule scheduler,
body-family recogniser, record tables, typed dispatch, member pointers, CRI alignment, units, crosschecks, held-out
oracle, mutation gates), and the separate implements / uses / emitted-in facts for the spine.

## The full call graph, the game map and the GameCube census (2026-11-02)

`tools/dolrecon/callgraph.py` (`python -B -m dolrecon.callgraph ../.build/dolrecon/pal-v30 --vcalls
../.build/dolrecon/game/vcalls.json`, ~3 s, writes `pal-v30/callgraph.json`; tests `tools/test_dolrecon_callgraph.py`, 8
cases, 5 negative controls all killed by `.build/dolrecon/gameplay_scope/cg_mutants.py`): every edge dtk's symbolised
assembly shows - `call` 92,826, `tail` 1,056 (`b` to another function's start), `addr` 2,393 (a function's address
taken: callbacks, entries handed on), `data` 10,426 (a referenced data object holds the function, followed through up
to 3 levels of objects pointing at objects), `vtable` 35,839 (code referencing a class's table), `virtual` 458 (vcalls'
`this`-typed calls, to the slot of the class and of every class deriving from it at offset 0). 33,653 of 34,037
functions have a caller of some kind (24,764 by direct calls alone); 8,889 only through data or a taken address; 160
data objects holding functions are referenced by no code (vtables of classes built elsewhere, effect tables, exception
tables). Limit: a data word that equals a function address by coincidence would be a false edge (dtk symbolises by
relocation analysis; the rebuild cannot tell).

Indirect calls (`.build/dolrecon/gameplay_scope/bctrl_census.py`): of 6,244 `bctrl` sites, 4,024 load the target from
an object's vtable with the receiver's class unknown (3,661 in game code), 1,037 call through RenderWare's engine
function table (411 from game code), 355 call a function pointer in a structure, 224 are resolved by vcalls (receiver
`this`), the rest come from registers or globals. Typing receivers (member fields from constructors' stores, returned
objects, singletons) is the next step.

`.build/dolrecon/gameplay_scope/game_map.py` assigns every function to a subsystem on that graph (class rules, link-order
neighbours, callers) - the result is `source/docs/GAME_MAP.md` (unknown 865 KB with direct calls only, 440 KB with
the full graph). `gc_census.py` lists the GameCube facts of every game function's own instructions (3,601 of 29,394
functions: MSL 1,736, paired singles 1,099, RenderWare 562, runtime 208, SDK 147, CRI 37, GQR 5, low memory 3, MMIO 1,
cache 1). `inventory.py` is the first player / enemy / mission inventory (direct calls only; superseded by the map).
