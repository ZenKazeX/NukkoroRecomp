# Semantic puzzle engine (prototype, 2026-11-07)

Status: Phase 11 prototype. Tool `tools/platform/puzzle_engine.py` (extends `tools/platform/structure_match.py` and the
`tools/dolrecon` decoder; nothing replaced). Uncommitted. Every number below was produced by the commands in
"Reproduce"; outputs are deterministic JSON in `.build/puzzle/`.

**Bottom line.** The engine adds measurable value in two places and none in a third:

- **Subsystem reconstruction (B): yes, where a service is scattered in memory.** Hiding every label of a service and
  regrowing it from calls, shared globals and rare basic blocks recovers `bus.exi` at recall 0.95 (address runs 0.55),
  `os.other` 0.56 (0.16), `os.interrupts` 0.67 (0.60), `os.reset` 0.57 (0.43). For contiguous services address runs win.
- **Semantic naming (C): yes, through a mechanism rule, not through similarity.** The callback-registration rule found
  and named the GX command-processor breakpoint setter `fn_80394F3C` as `GXSetBreakPtCallback` (PROBABLE ROLE, four
  independent pieces of evidence below).
- **Service prediction for single functions: no.** Address locality alone is right 92.6 % of the time on held-out
  symbol-named functions; all evidence combined 91.0 %. The combination fixes 2 of address's 19 errors and breaks 6.
  The engine therefore never overrides an address-consistent service with a structural guess.

## 1. Existing-tool inventory (Phase 1)

| source | where | used here |
|---|---|---|
| original PowerPC words | `../game/sys/main.dol` via `tools/dolrecon/dol.py` | yes: blocks, globals, fingerprints, data flow |
| function extents and static calls | `extract/functions.csv` (39,355 functions, `callees`) | yes: call edges |
| generator liveness graph | `<tree>/platform/.../live_graph.txt` (39,356 rows, 192,606 edges) | yes: row kinds; edges not yet (see blind spots) |
| platform manifest (service, evidence, owner) | `<dolrecon>/pal-v30/platform_manifest.json`, sdk-identify's names applied | yes: labels |
| native owners | `extract/native-owners.txt` | yes: owned / not owned |
| removal plan | `removal_plan.json` (services, frontier, rows) | yes: scope |
| replay hit counts | `test_data/platform/gx_rw_frontier.csv` (160 GX / RW rows) | yes: priority only |
| reference interpreter, contract tests, guest watch | runtime + CTests | not called by the engine: they are the verifiers the agents run (Phase 9) |
| recompiled C++ bodies | `<tree>/generated/recomp_*.cpp` (49 shards) | no (they are a translation of the same words) |

Reconciliation of "855": that number is **generator rows** of platform services still compiled as bodies
(`removal_plan.json platform_live_bodies`), not functions. Counts on the current tree:

| quantity | value |
|---|---:|
| functions in the binary (`functions.csv`) | 39,355 |
| generator rows | 39,356 |
| rows compiled as bodies / pruned / owner thunks | 38,135 / 955 / 266 |
| platform functions in the manifest (sdk 965, CRI 1,178, RW 158) | 2,301 |
| platform rows still compiled (the ratchet's number) | 855 |
| platform functions the engine fingerprints (have a size in `functions.csv`) | 2,245 |

Reachable during execution: only the 160 GX / RW frontier rows have replay hit counts; everything else is "reachable by
the generator's graph", not "executed". Replaced by native owners: the owner list. Still needing original behaviour:
the 855 rows.

## 2. Architecture (Phase 10)

    FunctionInventory      functions.csv + live graph + manifest + owners        (Phase 1)
    PartialPatternExtractor  basic blocks, field offsets, small-data globals       (Phase 2)
    SemanticEvidenceGraph  calls / reads-or-writes-global / shares-rare-block /     (Phase 3)
                           structural twin / registers-callback-invoked-by, each with provenance
    CandidateMatcher       per-family service distributions from VISIBLE labels    (Phase 4)
    SubsystemReconstructor connected components of the non-address graph           (Phase 4)
    SemanticNameProposer   graded names; mechanism rules (callback registration)   (Phase 5)
    IndependentVerifier    NOT automated: contract test / guest watch / A/B gates  (Phase 9)
    NextCandidatePlanner   unknown neighbours, competing hypotheses, hits          (Phase 8)

One pass (no iteration yet, see blind spots). No LLM anywhere. Inputs read only.

## 3. Evidence types and patterns (Phase 2)

- **Basic block**: leaders at branch targets and after branches. Token per instruction: opcode + extended opcode;
  registers dropped; for loads / stores off a non-stack, non-small-data base the **field offset is kept**
  (`32@+0x14`); small-data accesses become `@g`; small compare immediates are kept. A block counts only with >= 3
  instructions and when it occurs in at most 1 % of the functions (prologues / epilogues are excluded); its weight is
  its inverse document frequency.
- **Small-data global** (`r13+off`, `r2+off`): weight = IDF; globals touched by more than 40 functions carry none.
- **Calls**: static `bl` targets and callers.
- **Structure**: whole-function fingerprint Jaccard (`structure_match.py`), counted only at >= 0.6.
- **Mechanism (callback registration)**: setter template "interrupt-bracketed update" (stores a global between
  `OSDisableInterrupts` and `OSRestoreInterrupts`) + a consumer that loads the same global and moves THAT register
  into CTR / LR (straight-line data flow, null-check branches allowed, volatile registers die at `bl`) before an
  indirect call.

## 4. Data schemas (`.build/puzzle/*.json`)

`functions` {address, name, service, service_evidence, layer, row, owned, size, template}; `patterns` {address,
block, idf, shared_with}; `evidence_edges` {from, to, type, provenance, weight?}; `candidate_matches` {address, name,
manifest_service, hypotheses[{service, score}], agreeing_independent_families, address_family};
`proposed_names` {address, current_symbol, proposed, level, evidence_families, address_says, verify_next};
`subsystem_clusters` (leave-one-subsystem-out results per service); `verified_identifications` (EXACT = manifest
symbol names only); `replacement_readiness` {address, service, compiled, callers, template, callbacks_or_indirect,
touches_hardware, interrupt_bracketed, readiness}; `investigation_priorities`; `callback_roles`; `validation`.

## 5. Naming rules (Phase 5)

| level | rule in the engine |
|---|---|
| EXACT IDENTITY | the manifest's symbol name (evidence `name`); not re-verified by the engine |
| VERIFIED SEMANTIC EQUIVALENCE | never assigned by the engine: only a passed contract test / gate promotes |
| PROBABLE ROLE | a mechanism rule fires (callback registration), or >= 2 non-address evidence families agree with the combined top AND the top holds >= 80 % of the score |
| TENTATIVE ROLE | some evidence, below the PROBABLE bar |
| UNKNOWN | no evidence |

Names keep the address (`GX_SetCallbackInvokedBy_fn_803949C0` until a reference name is established). A service-level
PROBABLE ("belongs to graphics.gx") is NOT a semantic name; see the false-progress warning in section 8.

## 6. Validation (Phase 6), no leakage

- Ground truth = manifest services whose evidence is a **symbol name** (531 labels, 512 with a size). Labels derived from
  address ranges (`layer`, `layer+range`, `link-order`, about 1,600) are evidence only: as truth they would make the
  address baseline right by construction.
- Split by address index: dev 256 (weights tuned on these only), test 256.
- Leave-one-function-out: the held-out function's label is invisible to every family.

| method (test, 256 functions) | top-1 | top-5 |
|---|---:|---:|
| address only (nearest labeled neighbour each side) | **92.6 %** | 96.9 % |
| calls only | 50.0 % | 62.5 % |
| shared globals only | 32.0 % | 33.2 % |
| rare basic blocks only (partial match) | 28.1 % | 46.9 % |
| whole-function structure only | 24.2 % | 24.2 % |
| combined (weights from dev) | 91.0 % | **97.3 %** |

Marginal value over address: of the 19 test functions address gets wrong, the combination fixes 2; it breaks 6 that
address gets right. Calibration of the combined top-1 by score share: 80-100 % -> 214 / 218 = 98 %; 40-60 % -> 52 %;
60-80 % -> 44 %. The PROBABLE bar measured on the test set: **95 / 95 correct, coverage 95 / 256 (37 %)**; with the label-free hardware family (section 11) 109 / 109, coverage 43 %.

Leave-one-subsystem-out (every label of the service hidden; recall / precision of the best-matching component of the
non-address graph vs the best address run between visible labels):

| service | n | graph | address run |
|---|---:|---|---|
| bus.exi | 20 | **0.95** / 0.91 | 0.55 / 1.00 |
| os.other | 64 | **0.56** / 0.95 | 0.16 / 1.00 |
| os.interrupts | 15 | **0.67** / 0.91 | 0.60 / 1.00 |
| os.reset | 7 | **0.57** / 1.00 | 0.43 / 1.00 |
| graphics.vi | 23 | 0.96 / 1.00 | 1.00 / 0.92 |
| saves.card | 81 | 0.97 / 0.97 | 0.99 / 0.99 |
| graphics.gx | 170 | 0.63 / 0.97 | **1.00** / 0.99 |
| cpu.config | 22 | 0.14 / 1.00 | **1.00** / 1.00 |
| (all 24 in `subsystem_clusters.json`) | | | |

Negative control (partial matching): pairs of labeled functions linked by a rare shared block belong to the same
service in 65.6 % of 30,982 links; with labels shuffled, 27.3 %. The block evidence is real (2.4x chance) and weaker
than address.

Callback rule on functions WITH known names (its own control): it fires on 5 known setters, all of which store a
function pointer something calls (VISetPreRetraceCallback, VISetPostRetraceCallback, ARRegisterDMACallback,
AXRegisterCallback, DVDSetAutoFatalMessaging); it misses AIRegisterDMACallback (its consumer's flow is not straight
line). An earlier version without the data-flow check fired on OSDisableScheduler, OSEnableScheduler, DVDResume (false
positives: their consumers make indirect calls, but not through that global); the data-flow requirement removed them.

## 7. Newly proposed identities

| address | proposal | level | evidence (independent) | next verification |
|---|---|---|---|---|
| 0x80394F3C | GXSetBreakPtCallback | PROBABLE ROLE | (1) interrupt-bracketed swap of r13+0x5E34 returning the old value; (2) the only consumer, 0x803949C0, loads r13+0x5E34 into CTR and calls it, saving / restoring the OS context around it; (3) it calls only when the stored CP status word (+0xC) has bit 0x10 and the control word bit 0x20, exactly the breakpoint condition of the GX CP interrupt handler (libogc `__GXCPInterruptHandler`, reference only); (4) callers: GX init 0x80393868 and the RW driver set-up 0x804954A0 | contract test of the setter vs the original body; census of calls |
| 0x803949C0 | GXCPInterruptHandler | PROBABLE ROLE | reads the CP status register through the CP base r13+0x5DFC, stores status / control words, handles overflow / underflow (thread wake-up), breakpoint callback (above) | as above |
| 0x803966B0, 0x8039677C | callback setters (manifest names `__GXShutdown__sub2/3` are split artefacts) consumed by 0x803966F4 / 0x803967C0 | TENTATIVE (mechanism fired; roles not yet named) | callback rule | read the two consumers' PE register accesses (token vs finish interrupt) |

Service-level PROBABLE roles for unnamed functions: 717, of which 554 are CRI middleware - they confirm the manifest's
address-derived service and are **not** new identities. They are listed in `proposed_names.json` but must not be read
as discoveries.

## 8. False positives and false-progress warnings

- Similarity-based service naming is worse than address (section 6): never use it to rename.
- The first callback rule had 3 false positives on known names (removed by data flow); it still misses 1 known setter.
- 1,069 functions reach a service-level PROBABLE, mostly by agreeing with address-derived labels: counting them as
  "discoveries" would be false progress. The real new identities in this run are the two GX rows in section 7.
- Readiness classes (`trivial` 40, `needs-oracle` 64, `candidate` 351 in the removal plan's services) are template
  heuristics, not verification.

## 9. Native replacement opportunities (Phase 9) and next pieces (Phase 8)

Already routed to agents (structure_match): 19 mechanical candidates (empty bodies, exact twins of owned functions).
New from this engine: `GXSetBreakPtCallback` 0x80394F3C (card-saves' GX queue; a native owner only after its contract
test). Highest-ranked unknown platform functions inside the removal plan's services (unknown neighbours, callers): RW
driver 0x8049148C (21 unknown neighbours), 0x80499004, 0x80499B14 (18 each); GX `__GXInitGX` 0x80394088 (17),
0x80396934 (13, MMIO: needs the oracle); threads `SelectThread` 0x803787B0 (10). Full list:
`investigation_priorities.json` (the ranking is unknown neighbours + competing hypotheses + replay hits; it does not
yet estimate uncertainty reduction).

## 10. Remaining blind spots

- **No iteration (Phase 7).** One pass; nothing is promoted automatically. Promotion must come from passed gates and is
  then picked up on the next run through the manifest / owner list.
- Indirect edges of the live graph (192,606) and vtable / callback tables are not used yet (only static `bl`).
- No runtime evidence beyond the 160 GX / RW hit counts.
- Data flow is straight-line only (it misses AIRegisterDMACallback).
- Evidence families are correlated (callers / callees of a library sit in the same library), so "two families agree"
  is weaker than two independent proofs; the PROBABLE bar was therefore calibrated and measured (95 / 95), not assumed.
- Leave-one-subsystem-out measures cluster recovery, not naming the hidden service.

## 11. Label-free anchors (`tools/platform/sdk_anchors.py`, added 2026-11-07)

Every other family reads the manifest's labels; these read only each function's own code, so they cannot agree with
the manifest by construction. Per function (all 39,355), by constant propagation inside each basic block (lis / addi /
ori, r13 and r2 fixed, `bl` kills volatiles, block leaders reset):

| anchor | what | how it is checked |
|---|---|---|
| mmio | loads / stores in 0xCC000000-0xCC00FFFF, named by device (CP, PE, VI, PI, MI, DSP, DI, SI, EXI, AI, write-gather pipe) | device -> manifest service confusion table (`anchor_validation.json`) |
| spr | mfspr / mtspr numbers, MSR, rfi, cache-block ops (LR / CTR / XER dropped: every function has them) | find precision below |
| wgp signature | order and width of the stores into the GX write-gather pipe (`bw` = BP command byte + register word) | groups GX command emitters; no naming from it |
| lowmem | OS low-memory globals 0x80000000-0x800030FF | listed only |
| strings | C strings at resolved data addresses; source-file names and "<< Dolphin SDK - XX" banners as module hints | module hint -> service table: 11 / 11 consistent |
| manifest gaps | unlabeled functions strictly inside one service's address run, called by nothing outside the platform | hide every 10th sdk row, re-find |

Measured (manifest pal-v30 with sdk-identify's names; graph = the pre-merge removal plan's live graph):

- **Hardware anchors** (mmio or a system SPR): 345 functions, 236 already platform rows (68 %); **109 are not in the
  manifest, 57 of them compiled.** These are platform code by their own instructions: the low exception vectors and
  `__init_hardware`, two OS routines using MSR / SRR0 / rfi (0x80370C2C, 0x8037167C), and 54 tiny write-gather-pipe
  emitters 0x8039A784..0x8039AE18 beside GXClearGPMetric (the GX performance-counter cases; signatures `bw` / `bww`).
  ~~The platform ratchet counts manifest rows, so it undercounts the compiled GameCube layer.~~ **Wrong (corrected
  2026-11-07):** the planner classifies rows by service address ranges (`what_if.services`), not by manifest entries,
  so these functions already count toward the ratchet; they are only missing from the manifest's entry list.
- **Manifest gaps:** validation hid 92 sdk rows; the rule re-found 44 (recall 48 %), service right for 43 (98 %). On
  the full manifest it lists 545 functions inside sdk-service runs (CRI and RW-driver runs excluded: those layers are
  partial by design), about 370 compiled: debug.trk runs 224, graphics.gx 126, os/audio a few. Grade: TENTATIVE (an
  address rule); some debug.trk-run functions may be runtime-library code linked between TRK units. Each needs evidence
  before it becomes a row.
- **Name hints from strings are rejected:** of 8 symbol-named functions whose strings begin with an identifier and
  "(" or ":", 0 match the function's own name (the strings are messages about a callee or a TRK command, and the truth
  names are split pieces such as `TRKDispatchMessage__sub11`). Strings are module evidence only.
- **Hardware family in the engine** (fixed device -> service table, never fitted): alone 12.9 % top-1 (coverage is
  small); the dev-tuned weight is 0, so combined top-1 is unchanged (91.0 %). As an independent agreeing family it
  lifts the PROBABLE bar from 95 / 95 to **109 / 109 (coverage 43 %)**, precision still 100 %.

Outputs: `anchors.json`, `anchor_find_candidates.json`, `anchor_compiled_missing.json`, `anchor_manifest_gaps.json`,
`anchor_wgp_groups.json`, `anchor_name_candidates.json`, `anchor_validation.json`. Tests: `tools/test_sdk_anchors.py`
(9: device from lis + displacement, pipe signature order, `bl` and block leaders kill constants, system SPR vs LR,
hint regexes, gap rule incl. outside callers).

## 12. Split functions and register-level identities (2026-11-07)

**Rejoin rule** (`sdk_anchors.rejoin`, code only): an entry is a piece of another when it starts inside the earlier
entry's range (`inside`), when the previous adjacent entry does not end in a terminator (`fallthrough`; `bla` to the
vectors and nop/zero padding end a function), when it is a target of a `lis/addi + lwzx + mtctr + bctr` table
(`jumptable`), or when only one entry branches to it with a plain `b` and it has no frame set-up of its own
(`branch-only`; a tail call reaches real functions, which have one). Negative control: of 452 symbol-named function
starts (artefact `__subN` names excluded), **0 are joined** (the first version joined 5: `bla 0x60`, padding and tail
calls, which the rule now handles). Positive controls: `__DBExceptionDestinationAux`'s second half and all 60 pieces of
GXSetGPMetric join their roots; the second half of `OSExceptionVector` (0x8037167C) is missed (a nop placeholder is
read as padding). On the binary: 5,401 pieces; **1,051 belong to platform functions, none of them a manifest row, 842
compiled** (gx 123, RW driver 203, CRI 311, OS 13 on the pre-merge graph). They are not uncounted: the planner
classifies by service address range, so SDK pieces are inside the ratchet's count (checked on the merged shipping
build: 133 compiled SDK pieces, within the 520; 479 CRI pieces, outside it by design). They are pruned with their
root when the root is owned. **Verification of 520** (b1456bc): the shipping build's generated code has 715 rows the
planner classifies as platform; 195 are native-owner stubs (`c.serve_native_owner(...)`, "the original body is not
emitted"), 520 are translated SDK bodies - equal to the planner's live count.

**GX register signatures:** the constant command / register ids a function writes through the write-gather pipe
(0x61 + BP word, 0x10 + XF header, 0x08 + CP byte). Register meanings checked against the local Dolphin headers
(`research/sources/repos/dolphin/Source/Core/VideoCommon/{BPMemory,XFMemory,CPMemory}.h`). Most setters write a
shadow word loaded from the GX state, so most ids are unknown (`BP:?`); only constant ids are used.

New identities (each read from the disassembly; level PROBABLE until a contract test against the original passes):

| address | manifest today | identity | evidence |
|---|---|---|---|
| 0x80370C18 (+ piece 0x80370C2C) | `ref_DBExceptionDestination` (link-order), piece absent | `__DBExceptionDestinationAux` | loads the string "DBExceptionDestination\n" (0x805610E0) for OSReport, then OSDumpContext of the context at physical 0xC0 + 0x80000000, then PPCHalt; `__DBExceptionDestination` (0x80370C60) sets MSR IR/DR and branches here |
| 0x80372BE8 | `SelectThread__sub1` (link-order) | `OSSaveContext` | stmw r13..r31 at +0x34, GQR1-7 at +0x1A8..+0x1C0, CR / LR / SRR0 / MSR / CTR / XER / r1 / r2 into the OSContext layout, +0xC = 1, returns 0 |
| 0x8037829C | `fn_8037829C` (link-order) | `SystemCallVector` (the sc handler template) | HID0 \| 8, isync, sync, HID0 restored, rfi; `__OSInitSystemCall` follows it |
| 0x80395578 | link-order | `__GXSetVCD` | CP 0x50 VCD_LO, CP 0x60 VCD_HI, XF 0x1008 VTXSPECS |
| 0x80395FC8 | link-order | `GXSetNumTexGens` | XF 0x103F SETNUMTEXGENS |
| 0x803964E8 | link-order (plan: "signature GXDrawDone") | `GXDrawDone` | interrupts off, BP 0x45 SETDRAWDONE = 2, flush, clears r13+0x5E48, sleeps on r13+0x5E4C until set |
| 0x80397738 | `__GXInitGX__sub62` | `GXClearBoundingBox` | BP 0x55 / 0x56 CLEARBBOX1 / 2 |
| 0x80397C80 | link-order | `GXSetNumChans` | XF 0x1009 SETNUMCHAN |
| 0x803987B8 | link-order | `GXInvalidateTexAll` | BP 0x66 TEXINVALIDATE twice; 12 callers incl. game code |
| 0x8039A0B4, 0x8039A158 | link-order | `GXSetProjection` / `GXSetProjectionv` (order settled in 12b) | XF 0x1020 SETPROJECTION |
| 0x8039A3B4 | link-order | `__GXSetViewport` | XF 0x101A SETVIEWPORT block (6 words) from the GX state; called by 0x8039A444 / 0x8039A49C (the two public viewport setters) |
| 0x8039A59C | `__GXInitGX__sub19` | `GXSetClipMode` (TENTATIVE: only the register) | XF 0x1005 CLIPDISABLE |
| 0x8039A5C4 | link-order | `__GXSetMatrixIndex` | CP 0x30 / XF 0x1018 MATINDEX_A, CP 0x40 / XF 0x1019 MATINDEX_B |
| 0x8039A648 (+ 60 pieces to 0x8039AE8C) | `__GXInitGX__sub71`, pieces absent | `GXSetGPMetric` | switches on the old perf0 / perf1 at +0x59C / +0x5A0 (NONE = 0x22 / 0x15), writes XF 0x1006 SETGPMETRIC, BP 0x23 / 0x24 PERF0, BP 0x67 PERF1, CP 0x20; GXClearGPMetric follows |
| 0x8039F67C, 0x8039F6FC, 0x8039F7EC | `OSGetTime` (functions.csv) for 0x8039F6FC: **wrong** | MetroTRK extended-register save / restore blocks (processor variants) | mfspr of performance-monitor and 976-982 SPRs into the TRK CPU state through r2 (TRK sets r2 to its state block); not the time base |

The `__GXInitGX__subN` names are parent-derived artefacts: those functions are real GX setters that GXInit calls.

### 12b. Second pass (register id through `rlwimi`, OS thread layout)

Most GX setters insert the BP register id into the word's top byte with `rlwimi rX, rConst, 24, 0, 7`; the scanner now
keeps that byte (any other write to the register forgets it; unit test). Known GX pipe register ids: 31 -> 94 of 198.

**Independent check.** card-saves named GX functions from a Dolphin signature table (`native_gx_state.cpp`
kAddresses) and `native-owners.txt`; their evidence is not register ids. Where both name the same address they agree
6 / 6, 0 conflicts: GXSetNumTexGens, GXSetNumChans, GXSetFog, GXSetScissorBoxOffset, GXSetDrawSync (BP 0x47 / 0x48),
and GXSetProjection (0x8039A0B4) vs GXSetProjectionv (0x8039A158), which settles the order the register alone could
not.

| address | manifest today | identity | evidence |
|---|---|---|---|
| 0x803970E4 | link-order | `GXSetDispCopyYScale` | BP 0x4E COPYYSCALE; converts the scale with the float -> unsigned helper; callers: the game's ScreenTest, RW raster code |
| 0x803971B0 | link-order | `GXSetCopyClear` | BP 0x4F / 0x50 / 0x51 CLEAR_AR / CLEAR_GB / CLEAR_Z; callers GcnCameraClear, GXInit |
| 0x80397444 | link-order | `GXCopyDisp` | BP 0x4B EFB_ADDR among the copy registers; callers: the game's present (0x8032DBB4..) and GcnRasterShowRaster |
| 0x803975AC | link-order (pruned) | `GXCopyTex` | same signature as GXCopyDisp, beside it |
| 0x803997BC | `__GXInitGX__sub35` | `GXSetZTexture` | BP 0xF5 ZTEX2 |
| 0x80399C20 | `__GXInitGX__sub44` | `GXSetFogRangeAdj` | BP 0xE8 FOGRANGE |
| 0x80399F94 | `__GXInitGX__sub53` | `GXSetFieldMask` | BP 0x44 FIELDMASK |

OS (from the OSThread / OSContext field layout the code writes: state +0x2C8, suspend +0x2CC, priority +0x2D0, base
+0x2D4, queue +0x2DC, links +0x2E0 / +0x2E4, mutex +0x2F0..+0x2F8, stack bounds +0x304 / +0x308; run-queue bits
r13+0x5AE0, reschedule hint r13+0x5AE4):

| address | manifest today | identity | evidence |
|---|---|---|---|
| 0x8037848C | link-order | `OSGetCurrentThread` | lwz 0x800000E4 |
| 0x80378498 | `...Thread__dtor__sub0` | `OSIsThreadTerminated` | state == 8 (moribund) or 0 |
| 0x80379574 | link-order | `OSGetThreadPriority` | returns +0x2D4 |
| 0x80378A08 | link-order | `OSYieldThread` | interrupts off, SelectThread(1); game callers |
| 0x80378A44 | link-order | `OSCreateThread` | priority <= 31, state 1, suspend 1, OSInitContext, stack bounds |
| 0x80378C2C | link-order | `OSExitThread` | OSClearContext, state 8, __OSUnlockAllMutex, wakes the join queue |
| 0x80378D10 | link-order | `OSCancelThread` | per-state removal, state 8, unlock mutexes |
| 0x80378ECC | link-order | `OSResumeThread` | suspend count down; ready / waiting re-queue; callers incl. the GX CP handler |
| 0x80379154 | link-order | `OSSuspendThread` | suspend count up; waiting thread re-queued at priority 0x20 |
| 0x803792C4 | link-order | `OSSleepThread` | state 4, priority-ordered insert into the queue at +0x2DC; callers DVD, AX quit, VIWaitForRetrace |
| 0x803793B0 | link-order | `OSWakeupThread` | every queued thread to state 1 and the run queue |
| 0x803794B4 | link-order | `OSSetThreadPriority` | priority <= 31 to +0x2D4, effective priority recomputed |
| 0x80376C78 | link-order | `__OSUnlockAllMutex` | walks the thread's mutex queue +0x2F4 / +0x2F8, wakes waiters |
| 0x8037854C / 0x803785B4 / 0x803785F0 | link-order | `UnsetRun` / `__OSGetEffectivePriority` / `SetEffectivePriority` | run-queue removal + bit clear; min(base, mutex waiters); per-state re-queue |
| 0x80372D9C | link-order | `OSInitContext` | SRR0 = r4, r1 = r5, MSR 0x9032, every register slot cleared |
| 0x80372D40 | link-order | `OSGetStackPointer` | mr r3, r1 |
| 0x80370E04 | link-order | `OSGetConsoleType` | boot info r13+0x5A20 -> +0x2C, default 0x10000002 |
| 0x80378320 | link-order | default switch-thread callback | empty; installed through r13+0x2710 |

### 12c. The planner's 101 "sdk:unidentified" bodies (shipping build, 2026-11-07)

The planner classifies against `../source/.build/dolrecon/pal-v30/platform_manifest.json`, which does not carry
sdk-identify's names (they live in its private manifest until regen_manifest.py is applied). Through that named
manifest 91 of the 101 have services: audio.ax 79, os.font 8, os.interrupts / graphics.vi / input.pad / audio.ai 1
each. The ratchet total is unaffected; only the per-service split in REMOVAL_PLAN.md is. The 10 without a name:

| address | identity | level | evidence |
|---|---|---|---|
| 0x80003488 | `.init`-section byte copy (memcpy shape) | TENTATIVE (MSL has its own memcpy) | lbzu/stbu loop over (dst r3, src r4, n r5); callers MetroTRK and InitializeProgramEndTrap |
| 0x80375628 + 0x80376060 | `OSGetFontTexture` (two pieces) | PROBABLE | calls GetFontCode, reads the font data r13+0x5A80, then the image address from sheet width x height and the sheet index |
| 0x80385A20 | `__AXDSPDoneCallback` | PROBABLE | no static caller (a registered callback); if the DSP-ready flag r13+0x5D48 == 2: clear it, OSGetTime - start >> 2, `__AXOutNewFrame` |
| 0x80385A80 | AX DSP callback waking a waiting thread | TENTATIVE | stores the flag r13+0x5D60, OSWakeupThread |
| 0x80384B40, 0x80385154 | jump-table pieces (one `b`) | structural | targets of AX switch tables |
| 0x80384EC8, 0x80384F24, 0x80384F8C | aux-buffer flush pieces of the AX aux processing | structural | DCFlushRangeNoSync(buffer, 0x780) |

## Reproduce

    python -B tools/platform/puzzle_engine.py \
        --manifest ../wt-agent-sdk-identify/.build/dolrecon/pal-v30/platform_manifest.json \
        --graph .build/platform/removal_plan-watch/live_graph.txt \
        --plan .build/platform/removal_plan-watch/removal_plan.json
    # (run under tools/agents/capped_run.ps1; about 20 s at 3 % of the machine)
    python -B tools/platform/sdk_anchors.py \
        --manifest ../wt-agent-sdk-identify/.build/dolrecon/pal-v30/platform_manifest.json \
        --graph .build/platform/removal_plan-watch/live_graph.txt
    # (about 3 s)
