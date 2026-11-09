# audio-ax (audio.ax core) - analysis and step plan, 2026-11-07 (in progress)

Worktree `wt-agent-audio-ax`, branch `agent/audio-ax`, cut at 00f2648. Nothing implemented yet. A clean baseline build
(`.build/watch`, untouched sources) was started first: `.build/watch-build-base.log`; do not edit tracked sources until it
has compiled (the saves.card attempt lost its baseline that way).

## The 44 frontier rows (names: test_data/platform/unidentified_names.csv; callers: the 2026-11-07 live graph)

Only 40 of the 44 are mine; the four AXFX effect families (ReverbStd / Chorus / Delay init, shutdown, callback, the
alloc/free hooks, AXFXSetHooks: 12 rows) belong to axfx-effects.

Families by data ownership (the in-frontier call graph, `edge` lines of live_graph.txt):

| family | rows | entered by | shared with |
|---|---|---|---|
| frame engine | __AXOutNewFrame 0x80385798, __AXOutAiCallback 0x80385950, __AXOutSPCallback 0x80385A20 (interior of 0x80385A18), __AXNextFrame 0x803850AC, __AXSyncPBs 0x80386BC0, __AXSyncPB 0x80386474, __AXPushCallbackStack 0x80384914, __AXServiceCallbackStack 0x80384718 | runtime literals only (measure_probes.cpp, system.cpp, ax_pb_tests.cpp, test literals) plus the AI DMA callback pointer the guest AXInit registers; no game caller | none. A closed cluster: AiCallback -> NewFrame -> {SyncPBs -> {SyncPB, PushCallbackStack}, NextFrame, ServiceCallbackStack} |
| voice setters | AXSetVoiceSrcType 0x80387090, State 0x80387154, Mix 0x803871B0, Addr 0x80387404, Adpcm 0x80387514, Src 0x803875B8, SrcRatio 0x80387650, AdpcmLoop 0x803876E8 | GCAX 0x8043xxxx (Mix 0x8043D2F4 and SrcRatio 0x8043D1C0 are game only); SrcType / State / Addr / Src also the CRI ADX voice (0x803CED50, 0x803CF650, 0x803CFAB0): CRI-shared | cri-threads |
| voice manager | AXAcquireVoice 0x80384A54, AXFreeVoice 0x803849D4 | GCAX plus the CRI voice release 0x803CF9E0 / 0x803CFAB0 / 0x803D0230 | cri-threads |
| lifecycle / registration | AXInit 0x8038465C, AXQuit 0x803846CC, AXSetMode 0x80385784, AXRegisterCallback 0x80385F60, AXRegisterAuxA/B 0x80385050 / 0x8038505C | game ctor / dtor 0x801F2E58 / 0x801F2DA8, GCAX 0x8043E66C / 0x8043F480 / 0x8043F304 | none (but they build the state every other family reads) |
| MIX | MIXInit 0x8038A1F4, MIXQuit 0x8038A2BC, MIXSetSoundMode 0x8038A2C0, MIXInitChannel 0x8038A2C8, MIXReleaseChannel 0x8038B144, MIXSetInput 0x8038B164, MIXSetPan 0x8038B18C, MIXUpdateSettings 0x8038B1F8 | game 0x801F2E58 / 0x801F21AC / 0x8043xxxx; InitChannel / ReleaseChannel / SetInput / SetPan also CRI (0x803CFAB0, 0x803CF9E0, 0x803CECA4, 0x803CEBF0) | cri-threads for those four |

State the families share (SDA words and arrays; from the sdk-identify evidence): voice priority stacks and the free stack
(r13+0x5CE8.., AXVPB array 0x805989A0 stride 0x22C), the DSP-visible PB array 0x8058FCA0 (64 x 0xF4, read by the native
mixer: runtime/include/gekko/audio_categories.h), the two 0x300-byte command lists at 0x8058ABC0, the callbacks
(r13+0x5D58 frame, 0x5CF0/0x5CF8 aux A, 0x5CF4/0x5CFC aux B), the output mode r13+0x5D38.

## What the existing native layer already does (read, runtime/include/gekko/native_audio.h)

AI and DSP are native owners; the AX library is translated guest code that builds a DSP command list (0xBABExxxx +
address mailed through the native DSPSendMailToDSP), and the native `System::run_ax_mix` mixes that list. So the
"game-visible result" of the frame engine is exactly that command list plus the callbacks it calls (the frame callback
0x8043F3E4, voice callbacks from the callback stack).

## Consequence for the steps

The frame engine and the setters/manager/lifecycle exchange state through the voice lists and the PB array; the command
list is the console's DSP protocol (hardware detail, not game behaviour). So the rows cannot be freed one at a time
without a native owner of the voice state: freeing the setters alone only moves the same field writes into C++ (a port of
SDK logic, one PB layout), and freeing the frame engine alone needs the voice state in native form first.

Proposed order (each step through all gates, each committed):

1. Lifecycle + registration (AXInit, AXQuit, AXSetMode, AXRegisterCallback, AXRegisterAuxA/B, MIXInit, MIXQuit,
   MIXSetSoundMode): game-only entries. Needs the owner to decide what AXInit's allocations / PB-array setup mean while the
   guest voice code still reads that state - so it is first only for the registration half (callbacks, mode), which is
   state the frame engine reads; AXInit itself waits for step 3.
2. Voice setters that have no CRI caller: AXSetVoiceMix 0x803871B0, AXSetVoiceSrcRatio 0x80387650 (and the AXSetVoice*
   family once cri-threads agrees to the shared four). Owner = the PB field contract (offsets above), tested against the
   original in the reference interpreter for every field, with the sync flag bits the frame engine reads.
3. Native voice state (voices, priority stacks, free stack, PB array) behind AXAcquireVoice / AXFreeVoice / AXInit, then
   the frame engine rows (their only entrants are the measurement literals and the AI callback), with the mixer fed from
   the native voice state instead of a command list (NE-AUDIO in native_audio.h).
4. MIXInitChannel / UpdateSettings (3.7 KB and 4.2 KB: the mixer's panning / auxiliary routing) with the voice state.

## Questions for the main session

* Shared rows (SetVoice* with CRI callers, AXAcquireVoice / AXFreeVoice, MIXInitChannel / ReleaseChannel / SetInput /
  SetPan) need a decision with cri-threads: who writes the owner.
* Step 3 changes how `run_ax_mix` is fed. Is the command-list boundary allowed to go (it is the console's DSP protocol)?
  Gate 4 (PCM 015445f2...) then has to prove the native mix equals the translated one.

## Step 1 done 2026-11-07: registration rows owned

Owners: `runtime/include/gekko/native_ax_registration.h` (header only, `ax_registration::install(cpu)` called from
`System` after `install_reverb_owner`): AXRegisterCallback 0x80385F60, AXRegisterAuxACallback 0x80385050,
AXRegisterAuxBCallback 0x8038505C, AXSetMode 0x80385784. They write the SDK's small-data words where the guest frame
engine still reads them (sda+0x5D58, 0x5CF0/0x5CF8, 0x5CF4/0x5CFC, 0x5D38); AXRegisterCallback returns the old frame
callback. Their interrupt masking only protected the swap from the frame interrupt (owners run between guest
instructions; the MSR is untouched and the original restores it).

Shared list lines changed: `extract/native-owners.txt` (4 rows appended, own comment block), `runtime/src/gekko/system.cpp`
(one call + comment after `install_reverb_owner()`), `runtime/include/gekko/system.h` (one include), `CMakeLists.txt`
(the new test target before `sound_table_contract_tests`), `runtime/tests/native_file_api_tests.cpp` (the new owner list
added to the owner/row comparison; nothing weakened).

| gate | evidence |
|---|---|
| 1 measure | what_if `ax-registration` (--release-dir .build/watch): frees 0 rows, 0 revived (the owners are leaf stores; no callee body to prune). Real build: native owner rows 209 -> 213 |
| 2 survivors | not applicable: no state is retired, the owners write the same words the original did |
| contract | `ax_registration_contract_tests` (ctest, Release): original bodies from the pinned DOL vs the owners, 3 presets x 4 entries x 5 values x 2 = 120 cases + the AXSetMode same-value case: r3, MSR and every word of 0x80000000..0x80600000 equal; negative control (aux A original vs aux B owner) differs |
| 3 behaviour | Westopolis 90 s headless (`--boot-stage stg0100 --input-tape-play`, harness copied to `.build/ab-jobs/run.sh`), base b1, cand c1, base b2, cand c2 alternating: audio.pcm sha256 b8e749ac2ab87aed... in all four (11,447,680 bytes), 3,645,000,000 ticks, 2,697 VI, 5,359 render frames in all four; memory digest (64 KiB blocks, all regions): 0 differing blocks in every pair (b1/b2, c1/c2, b1/c1, b2/c2, b1/c2) |
| ratchet | not touched; this step compiles 0 fewer platform bodies |

Note: the reference PCM named in AGENT_COORDINATION (015445f2...) was not reproduced by the clean baseline of this
worktree either (b8e749ac... with 5,359 render frames; step 2's report had 5,360): the A/B criterion (baseline == candidate
in every run) holds, the absolute hash differs from the snapshot figure, so the comparison is against this worktree's
own baseline exe. Cause not investigated (card image / settings copies differ from the earlier run are possible).
CPU of the runs: the A/B sequence used 0.39 % of the machine (capped runner), builds 8-9 %.

## Stage 2 analysis 2026-11-07 (read-only; nothing implemented)

Merged gc-removal (7f142c2). Findings so far:

* Sizes of the original code the native voice state must replace: __AXSyncPB 0x80386474 (469 instructions), __AXNextFrame
  0x803850AC (445), __AXSyncPBs 0x80386BC0, __AXPushCallbackStack / __AXServiceCallbackStack (50 / 177), AXAcquireVoice
  (free stack 0x80587DC0, priority stacks, drop callback at AXVPB+0x10, __AXSetPBDefault 0x80386E44), plus MIX (InitChannel
  3.7 KB, UpdateSettings 4.2 KB). The AXVPB (0x22C) embeds a PB copy at +0x138 and the sync flags (+0x1C/+0x20/+0x28/+0x146);
  __AXSyncPB copies changed fields into the DSP PB array (0x8058FCA0 + 0xF4 * index, index at AXVPB+0x18).
* Guest watch (.build/ab-jobs/w1, baseline exe, Westopolis 90 s, PB array + AXVPB array, capped runner): the translated code
  reads the PB array 2.3 M times and writes it ~0.2 M times, but the exe was built without symbols ("symbols NO"), so every
  row is an anonymous native RVA. Naming the readers needs a `-DSHADOW_WATCH_SYMBOLS=ON` build of this tree
  (.build/watch config exists; a full rebuild under the 10 % cap is hours). Without the names the "every PB byte the game/CRI
  reads is served or proven unread" condition cannot be discharged.
* Consequence: the stage (setters + acquire/free + MIX + frame engine + a run_ax_mix fed from native voice state, with PCM and
  AI-frame identity) is a port of roughly 2.5 K instructions of SDK logic plus the DSP-protocol boundary; it is not a
  one-sitting change. Proposed order of commits, each gated: (a) symbolised watch to name the PB/AXVPB readers; (b) native
  VoiceState model + contract test of __AXSyncPB (original vs model on recorded AXVPB images); (c) setters + acquire/free
  over that state; (d) MIX rows; (e) frame engine + run_ax_mix from native state, command list removed.

## Baseline correction 2026-11-07

The step-1 A/B (b8e749ac..., 5,359 render frames) ran through run.sh mode `1` = SHADOW_NATIVE_JOBS=1, not the default
configuration. Default configuration (run.sh mode `d`: no env var, windowed-config headless-free as the harness does,
capped runner), baseline exe d1 and step-1 candidate exe d2: audio.pcm sha256 015445f28cd10698 in both (the reference),
CP breakpoint 5,359 / VI 5,395 / AI 17,887 identical. Step-1 evidence on the default configuration therefore holds; the
SHADOW_NATIVE_JOBS=1 runs stay as extra evidence only. All later gates use mode `d`.
extract/liveness-roots-lifted.txt regenerated (23c7227): +32 roots, all from owners added on gc-removal (OS arena/clock,
GX state, InitializeUART); the check command of native_liveness_roots_lifted exits 0.

## Candidates from structure_match.py (gc-removal 50174a0; main session, 2026-11-07)

Empty bodies: __AXClQuit 0x80385780, __AXSPBQuit 0x803863D4, __AXVPBQuit 0x8038708C, MIXQuit 0x8038A2BC. Getters (twins of
OSGetArenaHi): __AXGetCommandListCycles 0x80385068, fn_8038646C. Setter MIXSetSoundMode 0x8038A2C0 (twin of OSSetArenaLo).
Interrupt-bracketed swap AIRegisterDMACallback 0x80381E30 (audio.ai) and the audio.dsp AudioGCN__ctor sub-getter 0x8038C384.
Placement: the empty quits, MIXQuit and MIXSetSoundMode go with stage (d) (MIX) and AXQuit's callee chain in (e); the two
getters die with the frame engine in (e) (their callers are inside it); each still needs its own gates. Not started.

## ax_pb_tests segfault: cause and fix (2026-11-07)

Symptom: `ax_pb_tests` died with a null read and no output (exit 0xC0000005). Found with a temporary vectored exception
handler and a link map (scratch, removed): the frames were `checkpoint_audio_history` -> `save_state(System&, ...)` ->
`NativeJobs::save_state` with `this` = null. Run 1 and 2 of the test (the PB-chain section) passed; the crash is the next
section. Cause: `System::native_jobs_`, `native_cri_`, `native_svm_` and `native_expand_jobs_` are created in
`enable_file_api()` (needs a mounted asset set); `savestate.cpp` dereferenced them unconditionally, so saving a bare
`System` (the unit tests') crashed. Not in the AX service itself: any test saving a System without assets hits it.
Fix (savestate.cpp only; the existence test is the existing `System::native_expand_jobs()` accessor, since enable_file_api creates the four together): without the services, save writes the four services'
idle (initial) state from locally constructed instances, load reads those blocks into idle instances and drops them; with
the services nothing changes (same bytes, same objects). Gate: `ax_pb_tests` all sections PASS, ctest `^ax_pb_tests$` 100 %;
the opt-in `--adpcm-loop-oracle` still fails exactly as documented before (NEXT_SESSION.md:947, not in CTest).
Not run: the other savestate tests, the game (a savestate round trip with the services present is unchanged code).

Getters asked about (unproven, from disassembly): 0x8038646C returns r13+0x5DA8, the per-frame count `__AXSyncPBs`
zeroes (0x80386BF0) and `__AXSyncPB` increments (0x80386484): a count of PBs synced this frame (single caller 0x803858CC,
inside `__AXNextFrame`). 0x80384DF8 stores 0x80589540 + r13[0x5D1C] * 0x780 through r3 (an output-buffer getter; single
caller 0x80385464, inside `__AXNextFrame`). Both die with the frame engine.

## (a) Symbolised guest watch 2026-11-07 (names the PB / AXVPB accessors)

Why the first watch had "symbols NO": the exe was copied away from its PDB. Run in place (`.build/watch/bin/Release/
ShadowTheHedgehog.exe`, step-1 build) on Westopolis, default configuration (`run.sh w2 d`, headless, capped; 520 s):
`.build/ab-jobs/w2/guest-watch.csv`, "symbols yes", 0 unattributed. Ranges: PB array 0x8058FCA0 (0x3D00), AXVPB array
0x805989A0 (0x8B00). Reads / writes per translated row:

| row | PB array r/w | AXVPB array r/w | what it is |
|---|---|---|---|
| 0x80385798 __AXOutNewFrame (with the inlined NextFrame) | 18.5 M / 0.5 M | - | builds the frame (the mixer's PB walk) |
| 0x80386474 __AXSyncPB | 3.6 M / 4.6 M | 6.6 M / 3.6 M | AXVPB -> PB copy |
| 0x80386BC0 __AXSyncPBs | 0 / 1.1 M | 6.1 M / 5.0 M | per-frame walk |
| 0x803863D8 (depop accumulate) | 7 K / 0 | - | reads PB depop words |
| 0x80386E88 AXInit, 0x80386E44 PB default, 0x8000543C (memset/clear), 0x80384A54 / 0x803848F4 | 0 / ~9 K | 0 / ~11 K | init, clears |
| setters 0x803871B0 (Mix), 0x80387650 (SrcRatio), 0x80387514, 0x803875B8, 0x80387404, 0x80387154, 0x803876E8, 0x80387090 | - | 0.2 M / 4.4 M (Mix) ... | write AXVPB fields |
| MIX 0x8038A2C8 InitChannel, 0x8038B144, 0x8038B164, 0x8038B18C, 0x8038B1F8 | - | 1.6 K / 17 K | AXVPB fields |
| **0x8043C64C (GCAX)** | - | **221,752 reads / 0** | game reads AXVPB fields |
| **0x803C5964 (CRI)** | - | **16,944 reads / 0** | CRI reads AXVPB fields |
| native code (row 0) | 1,952 reads | 4,448 reads | run_ax_mix / voice-select (native readers) |

Consequences: no translated code outside the AX library touches the PB array; the PB array's only translated readers are
the frame engine (__AXOutNewFrame, __AXSyncPB, 0x803863D8). The AXVPB array is read by two outside rows, GCAX 0x8043C64C
and CRI 0x803C5964, which the native voice state must serve (first addresses: see the csv's first_guest_address column;
to be mapped to AXVPB fields in the next step). The setters' dominant traffic is AXVPB writes (Mix 4.4 M).
First outside read addresses mapped: GCAX 0x8043C64C first reads 0x805A118E = AXVPB[62] + 0x146 (the voice state halfword, the one
__AXSyncPB / AXAcquireVoice also test); CRI 0x803C5964 first reads 0x8059A9BA = AXVPB[14] + 0x1B2 (inside the embedded PB
copy, +0x138 + 0x7A = the PB's current-address low word: CRI's playback position). Only first addresses are recorded by
the watch; the full field set needs the next watch with per-offset output.

## Savestate gate for the ax_pb_tests fix (30c744a) 2026-11-07

Base exe = merged HEAD with the old savestate.cpp, new exe = HEAD (one-file difference); `SHADOW_NATIVE_JOBS=0`, headless,
capped (`.build/ab-jobs/ss.sh`). Save at game tick 1,822,500,000 on the Westopolis tape: state files byte-identical
(`cmp`; sha256 1eee21dd05049d94..., 33,493,134 bytes), save-run PCM identical (93916ee886f82f75...). The same state file
loaded in both exes (30 s, no tape): PCM identical (07ecae927744ec85...), VI 1800 / AI 6000 / CP breakpoint 1800 equal.
End memory not compared directly (the state report is one line); the byte-identical state files cover guest memory at the
save point. CTests (tests tree, Release): ax_pb_tests, gekko_core_tests, compiler_body_tests, native_expand_jobs_tests: 4/4
pass. Note the savestate tick argument is in guest instructions (202,500 per game tick), not game ticks.

## Frontier survey (gc-removal 1858197, removal_plan.py, 34 rows; no ownership taken)

Own manifest (`regen_manifest.py`), `removal_plan.py --release-dir .build/watch --dolrecon-dir .build/dolrecon` under capped_run, lock `generator`: audio.ax 82 bodies, frontier 34, frees 52.

- **A. Game-only callers (GCAX 0x8043xxxx / 0x801F2xxx), natively reachable now (12):** 8038465C, 803846CC, 803871B0, 80387514, 80387650, 803876E8, 803890B0, 8038980C, 80389B5C, 8038A12C, 8038A1F4, 8038A2BC. Voice/PB and MIX API; owner needs the native VoiceState (stages b-d).
- **B. Also called from CRI (6):** 803849D4, 80384A54 (+ 8043E74C unresolved), 80387090, 80387154 (+ unresolved 8043D0D4/8043E74C), 80387404, 803875B8. Owner must serve CRI's reads of the same state (the AXVPB fields CRI 0x803C5964 reads); CRI must not be changed.
- **C. Runtime literals only (8):** 80384718, 80384914, 803850AC, 80385798, 80385950, 80385A20, 80386474, 80386BC0 (also ax_pb_tests, gekko_core_tests). These die with the frame engine (stage e), not separately ownable.
- **D. Data words (2):** 8038A0DC, 8038A104 (words at 805EECB8/805EECBC): MIX table entries; owner must also be installed through the table.
- Unresolved callers: 8043D0D4, 8043E74C (both reached via B rows).

### Survey correction (my sed cut the listing at 28 of 34 rows)
Missing 6, all in 0x8038A2C0..0x8038B1F8: A += 8038A2C0 (game 801F21AC) -> A = 13. B += 8038A2C8 (game 8043DACC + CRI 803CFAB0), 8038B144 (game 8043CE54 + CRI 803CF9E0/FAB0/D019C/D0230 + unresolved 8043D0D4/E74C) -> B = 8. New class E, CRI-only callers: 8038B164 (803CECA4), 8038B18C (803CEBF0, 803CFAB0) = 2. New class F, unresolved-only caller: 8038B1F8 (8043F3E4) = 1. Total 13+8+2+1+8(C)+2(D) = 34.
Design condition from main: VoiceState is the native mixer's own state written directly by the AX API; no __AXSyncPB/dirty-bit/PB DMA imitation. Contract test compares API return values and the AXVPB fields game and CRI read back, with the B/E rows' CRI readers explicit.

## Stage c (setters) 2026-11-08: the eight AXSetVoice* rows

Design decision (main's condition): no PB mirror, no DSP copy. The AXVPB in guest memory stays the single voice record
(the game's GCAX 0x8043C64C and CRI 0x803C5964 read it directly), and the owners write its fields exactly as the SDK did
(`runtime/include/gekko/native_ax_voice.h`, `gekko::ax_voice`). `__AXSyncPB` is NOT imitated: it stays guest code until the
frame engine (stage e) reads the AXVPB directly. The interrupt bracket is dropped (owner runs between guest instructions;
same precedent as AXRegisterCallback).

Rows owned (A/B classes): AXSetVoiceMix 0x803871B0, SrcRatio 0x80387650, SrcType 0x80387090, State 0x80387154, Addr
0x80387404, Adpcm 0x80387514, Src 0x803875B8, AdpcmLoop 0x803876E8. Offsets, decoded from the pinned DOL: Mix 18 halfwords
-> vpb+0x14A.., flag halfword vpb+0x144 (mask table in the header), sync |= 0x12; SrcRatio = fmuls(65536) -> saturating
u32 (0x803A1DC8) clamp 0x40000 -> +0x1DE/+0x1E0, sync |= 0x80000; SrcType switch on the embedded PB (+0x138) words +8/+0xA,
sync |= 1; State halfword +0x146, sync |= 4, state 0 also stores 1 at +0x20; Addr 16 bytes -> +0x1A6 plus the 0xA / 0x19 loop
blocks, sync clear 0x1E000 set 0x21000; Adpcm 40 bytes -> +0x1B6, sync |= 0x20000; Src 14 bytes -> +0x1DE, sync clear
0x80000 set 0x40000; AdpcmLoop 6 bytes -> +0x1EC, sync |= 0x100000.

Contract test `ax_voice_contract_tests` (original DOL in the reference interpreter vs the owners on a second machine; random
AXVPB images incl. random dirty word, interrupts on and off): 2262 cases equal in MSR, f31 and all of guest memory
0x80000000..0x80600000; negative control differs. The CRI readers are covered because the test compares the whole AXVPB bytes
CRI reads (it reads them straight from guest memory; nothing is served any other way). Not compared: FPSCR (the setter's
fcmpu/fctiwz leave FPCC/FI bits; no code reads FPSCR after these calls - NOT proven by a watch yet) and r3-r12 residue.
Pre-existing, not mine: native_file_api_tests lists 66 "listed, not installed" addresses on gc-removal 64f51fb as well.
Roots (liveness-roots.txt, -lifted) regenerated for the two new addresses (the test checks every owner address is listed).

### Stage c, gates (AXFreeVoice, AXAcquireVoice and the eight setters; tree = gc-removal 4691f55 + this branch)

Owners added since the setters: AXFreeVoice 0x803849D4, AXAcquireVoice 0x80384A54 (plus owner entry 0x80384AB8, the steal
search's loop head: the generator keeps the loop tail 0x80384B4C as a row of its own that jumps there, so the head must be a
declared entry; entering it stops loudly - nothing can stand inside the atomic owner). Acquire's drop callback runs through
`guest_call` (native_guest_call.h) with MSR[EE] cleared around it, as the original's disable/restore bracket does.
1. Contract test `ax_voice_contract_tests`: 2622 cases (setters + 120 Free + 240 Acquire over random doubly linked priority
   stacks), r3 of Acquire, MSR, f31 and all of 0x80000000..0x80600000 equal the original; 81 of the 240 Acquire cases stole a voice and
   ran a drop callback that recorded its argument (equal), 7 returned null; negative control differs. A first Free run found my
   0x200A4 for the SetPBDefault dirty word: the original is lis 0x20 + 0xA4 = 0x2000A4 (fixed; the test caught it).
2. Planner (removal_plan.py, lock generator, own manifest), before this stage vs after with the 11 owner rows: audio.ax 82 bodies /
   34 frontier -> 70 / 23 (code:game 25 -> 12, middleware.cri 18 -> 8, unresolved 6 -> 3). `what_if --own` of the same rows reports 0 freed
   because the build it measures already contains them; the planner pair is the measurement.
3. Game behaviour: Westopolis 90 s headless, baseline = main's gc-removal 452099e exe (differs from this branch's merge base 4691f55 only
   in tools/platform/runtime_coupling.py), candidate = this branch, alternating A B A B, in `SHADOW_NATIVE_JOBS=0` and in the default
   configuration: PCM identical in all 8 runs (46a70a10..., see below), 3,645,000,000 ticks, 2,697 VI, 5,360 render frames in all;
   A/A and B/B memory block digests identical; A/B: one 64 KB block (0x80600000) differs. NOTE the PCM is no longer 015445f2...: the
   baseline exe itself now produces 46a70a10... (cri-threads S4 made native async loading the default, round 11), so the old
   reference is stale for trees after 4691f55; A = B here. On the previous tree (round 10) A/B was 015445f2 in all 8 runs.
   Differing words (peeks on the round-10 pair, 15 words in 0x80600000 / 0x81320000 / 0x81580000): all stack-frame shaped (back chains
   0x8081xxxx / 0x8060xxxx, floats, return addresses 0x8043D8C0 = the instruction after `bl AXSetVoiceMix` in GCAX, 0x8043D9EC); the
   baseline has the elided SDK frames' saved LR/registers, the candidate has other residue. Not proven below any live sp.
4. Perf: not measured separately (the owners replace ~1.2 K executed instructions/frame with C++); headless 90 s wall time 11-16 s in both.

## Stage d (MIX rows) 2026-11-08: MIXInit, hook setter 0x8038A12C, MIXInitChannel, MIXUpdateSettings, MIXReleaseChannel, MIXSetInput, MIXSetPan

Commits 5babb71, bec3a27, efd525b (merged with gc-removal 0da0b25 round 12 = a9f06a5). Same design as stage c: the MIX channel table
(0x805A14A0, 64 x 0x60, indexed by AXVPB+0x18) and the AXVPB stay guest memory; owners in `gekko::ax_voice` (native_ax_voice.h).
The gain derivation (`mix_derive_gains(channel, delta)`) is shared by InitChannel (targets at +0x3C.., delta 0) and UpdateSettings
(targets at +0x3E.., delta 2); the ramp step is the original's mulhw(0x66666667) >> 6 with sign fix (d / 160 toward zero). UpdateSettings'
stream-volume tail calls the two audio.ai owners (AISetStreamVolLeft/Right 0x803821EC / 0x80382218) through guest_call.
1. Contract test (ax_voice_contract_tests, now 5396 cases): MIXInitChannel 600 randomized cases over sound modes -1..4, flag bits 0..2,
   level clamps; MIXUpdateSettings 150 cases (random channel table with free channels, random pending-change bits, modes -1..4, stream
   volume words); MIXInit 24; SetPan 1600 (pan clamp, both table widths); SetInput / ReleaseChannel 400. All equal in MSR, f31 and all of
   0x80000000..0x80600000 (the AI stream-volume functions run in the interpreter on both machines, their MMIO writes cancel).
2. Planner (removal_plan.py, lock generator): audio.ax 70 bodies / 23 frontier (after stage c) -> 62 / 16; the frontier is now
   runtime-literal 15 (frame engine), code:game 8 (AXInit/AXQuit and friends), data-word 2 (class D). No CRI-coupled or unresolved row is left.
   Platform total 312 -> 308 bodies (compiled), frontier 79 -> 65 across all groups (shared with other agents' work in the same tree).
3. Game behaviour: Westopolis 90 s headless, base10.exe (same tree with the three commits reverted in the working tree, built, then
   restored) vs cand10.exe, alternating A B A B, jobs=0 and default: PCM 46a70a1051347069 in all 8 (the canonical reference since round 11),
   3,645,000,000 ticks, 2,697 VI, 5,360 render frames; A/A and B/B memory identical; A/B one 64 KB block (0x80600000) differs (dead stack
   frames, as in stage c; not re-peeked for this batch). Perf: wall seconds per run baseline 12-15 vs candidate 12-14 (jobs=0) and 12-13 vs
   12-13 (default): no measurable difference.

## Stage e plan (AXInit chain + frame engine) 2026-11-08 - what is left and why it is one change

Left in audio.ax after stage d: 62 bodies, 16 frontier rows: the frame engine (15 runtime-literal rows: __AXOutNewFrame 0x80385798, __AXOutAiCallback
0x80385950, __AXOutSPCallback 0x80385A20, __AXNextFrame 0x803850AC, __AXSyncPBs 0x80386BC0, __AXSyncPB 0x80386474, __AXPushCallbackStack 0x80384914,
__AXServiceCallbackStack 0x80384718, the getters 0x8038646C / 0x80384DF8, ...), AXInit/AXQuit (code:game) and the two class D data words.
Read for AXInit (0x8038465C -> 0x80384680): OSReport banner via 0x8037177C, then the memory initialisers 0x8038478C (stack heads/tails = 0, sda 0x5CE8),
0x80386E88 (AXVPB / PB / command-list arrays: ~0x4D00 + 0x11000 bytes of zeroing, then the 64 AXVPB records: index at +0x18, sync word 0x2000A4,
+0x30 = self+0x34, pushed on the free stack), 0x803863A8 (520 insns: SPB / command list setup), 0x80384BC8 (aux buffer pointers cleared, sda
0x5CF0.. , 30 x 0x40 bytes), 0x8038575C (sda 0x5D38 mode = 0, 0x5D28.., 0x5D34 = 1) and 0x80385B68 (the AI DMA / DSP task registration).
Correction to my message to os-other: 0x80384840 is NOT a voice-free loop; it is AXQuit's twin of 0x8038478C (it zeroes the same stack heads/tails and sda
0x5CE8). Those memory initialisers are cheap to port exactly, but they build the arrays the guest frame engine reads; porting them first would only
move the memset and keep every array. The real removal is the engine: native per-AI-frame processing that reads the AXVPB embedded PB (+0x138, with
the sync-flag word +0x1C as the only dirty protocol the guest setters already write) directly into the native mixer, serves the frame / aux / voice
callbacks through guest_call, deletes the PB array 0x8058FCA0 and the 0x300-byte command lists, and then lets AXInit shrink to the AI/DSP
registration. Evidence needed for that single change: PCM 46a70a10 bit-identical (the mixer inputs are the same numbers, the engine only changes who
copies them), ticks / VI / frames, the contract test of "native engine frame vs original __AXNextFrame + run_ax_mix" on recorded AXVPB images
(one frame, then 30 frames with callbacks), the guest watch showing no translated reader of the PB array remains, and perf (the engine replaces
~2.5 K guest instructions per frame). Not started; needs the run_ax_mix refactor (system.cpp ~4333-4800) designed against parse_ax_list's contract.

## Stage e: the engine table (disassembled to the instruction, 2026-11-08; nothing implemented)

Method: every body below was read in full from the pinned DOL (tools/doldis.py --func). Readers outside the engine: (1) a scan of every
load/store in the DOL's text with base r13 and offset 0x5CE0..0x5DD0 / 0x27B0..0x27C0: **no instruction outside 0x80384000..0x8038C300 (the AX / MIX / AI SDK) touches any of the engine's
SDA words** (the only hit is 0x5DD0 inside audio.dsp); (2) the getters' callers (xref): every helper below is entered only from inside the engine;
(3) the guest watch of stage (a) for the arrays: the AXVPB array has two outside readers (GCAX 0x8043C64C: AXVPB+0x146 state, 222 K reads;
CRI 0x803C5964: AXVPB+0x1B2 = embedded PB (+0x138) word +0x7A, the play position, 17 K reads); the PB array 0x8058FCA0 has none.
Classes: **G** game-visible (a reader, callback or ordering outside the engine), **E** engine-internal (state only these bodies read), **D** DSP-side traffic
that the native mixer makes unnecessary.

| body | what it does (instruction reading) | outside reader? | class |
|---|---|---|---|
| __AXOutNewFrame 0x80385798 (r3 = DSP-time delta) | per AI frame: OSGetTime (0x80379628) into a 7-entry profile record (+0x4A50..+0x4A80 off 0x8058B1C0); __AXSyncPBs(r3); 0x80385FB0 (profile / cycle stats, 774 insns); 0x80385070 = next command-list buffer (2 x 0x300, toggled by sda 0x5D28 & 1); DSP mailbox handshake: send 0xBABE0180 + list address (0x8038C2AC), poll the mailbox (0x8038C274) twice; __AXServiceCallbackStack; aux flush 0x80384E14; user frame callback sda 0x5D58 (GCAX 0x8043F3E4, which runs MIXUpdateSettings); __AXNextFrame(list info = base+0x780, out buffer = base + 0x280 * sda 0x5D40); rotate sda 0x5D40 mod 3 (sda 0x5D70 == 1) or & 1; DCStoreRange of the finished output buffer (0x80381E74); more OSGetTime; copies the 56-byte profile record to the AXGetProfile slot (0x80387754). Reads sda 0x5D40 0x5D58 0x5D70; writes the profile record, sda 0x5D40, the command list, the out buffers | callbacks are G (frame cb, voice drop cb, aux cbs); the rest E/D | G + E + D |
| __AXOutAiCallback 0x80385950 (AI DMA done) | state machine on sda 0x5D48: first call stores OSGetTime in 0x5D50/54; state 1 -> 0 and __AXOutNewFrame(0); else state = 2 and DSPSendMail (0x8038C43C, task object at 0x8056FBC0): the DSP-interrupt path; with sda 0x5D70 == 1 DCStoreRange + rotate sda 0x5D44 mod 3. Reads sda 0x5D44 0x5D48 0x5D70; writes 0x5D44 0x5D48 0x5D50 0x5D54 0x5D5C | none | E + D |
| __AXOutSPCallback 0x80385A20 / DSP done | state 2 -> 0, delta = OSGetTime - 0x5D50/54 (>> 2), __AXOutNewFrame(delta); else state = 1. Reads 0x5D48 0x5D50 0x5D54; writes 0x5D48 | none | E + D |
| __AXNextFrame 0x803850AC | builds the DSP command list at sda 0x5D2C (halfword opcodes: 0x00 PB array address via 0x80385FA4, 0x07 / 0x11 by sda 0x5D38 mode, 0x02 + out buffer, 0x03, 0x13 aux A/B buffers via 0x80384CBC/CF0/D10/D2C/D4C/D6C/D8C/DC0/DDC/DF8 which compute buffer addresses from sda 0x5D10 / 0x5D14 / 0x5D18 / 0x5D1C, 0x10 ...) and a running cycle estimate in sda 0x5D30 (0x1A9 + per-command costs 0x2E44, 0x546, 0x5E6, 0xDED ...); DCFlush of the list (0x80372534). Reads sda 0x5D2C 0x5D30 0x5D34 0x5D38 (output mode, written by AXSetMode); writes the command list, sda 0x5D28 0x5D2C 0x5D30 0x5D34 | only the native run_ax_mix (system.cpp ~4333-4800, parse_ax_list) | D (the command list is the DSP protocol) |
| __AXSyncPBs 0x80386BC0 (r3 = list cycles) | DCInvalidateRange of PB array 0x8058FCA0 (0x3D00) and 0x805939A0 (0x1000); sda 0x5DA8 = 0; for priority 31..1 and then 0 walks the voice stacks (0x80384700 = head getter): per voice: depop flag +0x20 -> 0x803863D8 (accumulates PB depop words); if state +0x146 == 1 or dirty +0x28 != 0: DSP cost estimate (table 0x80563348 indexed by src type +0x1DE, mix flags +0x144, +0x1F2, +0x140) summed in r29; if sda 0x5DA0 (the per-frame budget = bus clock * 0x51EB851F >> 7) > the sum: __AXSyncPB(vpb), else the voice is dropped: PB fields 0x4C/4A/48/46/44/0E = 0, vpb+0x146 = 0, __AXPushCallbackStack(vpb); clears vpb +0x1C +0x20 +0x28 +0x24, +0x30 = self+0x34; ends with sda 0x5DA4 = the total cost; priority-0 (free) list: clear depop flag and PB fields; DCStoreRange x3 (0x8058FCA0 0x3D00, +0x3D00 0x1000, +0x4D00 0x4000). Reads AXVPB +0x146 +0x28 +0x20 +0x18 +0x1DE +0x140 +0x144 +0x1F2, sda 0x5DA0; writes AXVPB +0x1C +0x20 +0x24 +0x28 +0x30 +0x146, the PB array, sda 0x5DA4 0x5DA8 | G: the cost budget decides which voices are dropped and their drop callbacks run; AXVPB +0x146 is read by GCAX 0x8043C64C | G (budget + drop) + D (PB DMA) |
| __AXSyncPB 0x80386474 (469 insns) | the AXVPB -> PB copy: tests the dirty bits of +0x1C (0x1 src type, 0x2 mix, 0x4 state, 0x80000 ratio, 0x100000 adpcm loop, 0x200 input gain ...) and copies the matching AXVPB fields (+0x138.. embedded PB, +0x14A mix block, +0x1A6 addr, +0x1B6 adpcm, +0x1DE src, +0x1EC loop) into the PB at 0x8058FCA0 + 0xF4 * index (+0x18); sda 0x5DA8 += 1. Reads the AXVPB fields the setters wrote and sda 0x5DA8; writes the PB array and sda 0x5DA8 | none (the PB array has no translated reader outside) | D (the dirty-bit protocol the main condition says not to imitate); the native mixer must read the same field values from the AXVPB |
| __AXServiceCallbackStack 0x80384718 | pops vpb from the callback stack (sda 0x5CE8, linked through AXVPB+8, __AXPopCallbackStack 0x80384924), for each with priority +0xC != 0: calls its drop callback +0x10 (guest function, r3 = vpb), 0x80384940 unlink from the priority stack, 0x803848F4 push on the free stack (head 0x80587DC0; vpb+0xC = 0). Reads sda 0x5CE8, AXVPB +8 +0xC +0x10; writes the stacks, AXVPB +0 +0xC | G: drop callbacks (GCAX, CRI) | G |
| __AXPushCallbackStack 0x80384914 | vpb+8 = sda 0x5CE8; sda 0x5CE8 = vpb | only __AXSyncPBs and the native voice-end handling | E |
| aux flush 0x80384E14 (168 insns) | calls the aux A / B callbacks (sda 0x5CF0 / 0x5CF4 with contexts 0x5CF8 / 0x5CFC) with the aux buffers of the finished frame (sda 0x5D10 / 0x5D14 buffer index * 0x780), then clears the buffers | G: the GCAX aux callbacks (reverb) read the aux buffers the mixer wrote | G |
| getters 0x8038646C (sda 0x5DA8), 0x80385068 (sda 0x5D30), 0x80385FA4 (PB array address 0x8058FCA0), 0x80386E38 (same), 0x80384700 (stack head), 0x80387754 (profile slot) | | callers only inside the engine | E (die with it) |
| profile 0x80385FB0 (774 insns) and 0x803863A8 (519 insns) | per-frame statistics in sda 0x5D78..0x5D98 and the record at 0x8058FCA0 - 0x3A0 (frame count, cycle min/max/avg through the 0x66666667 divisions) | none outside (the AXGetProfile slot 0x80387754 is read only by the engine) | E (statistics nobody reads; a guest watch of the profile record should confirm before dropping) |
| AXInit 0x8038465C -> 0x80384680 | OSReport banner (0x8037177C); 0x8038478C (stack heads / tails = 0, sda 0x5CE8 = 0); 0x80386E88 (sda 0x5DA0 = busclock * 0x51EB851F >> 7, 0x5DA4 = 0; zero PB array 0x3D00, 0x1000 at +0x3D00, AXVPB array 0x8B00; per voice i in 0..63: +0x18 = i, +0x1C = 0x2000A4, +0x30 = self + 0x34, pushed on the free stack via 0x803848F4); 0x803863A8 (profile reset); 0x80384BC8 (aux buffers: sda 0x5CF0.. = 0, 0x5D10 = 0, 0x5D14 = 1, 0x5D18 = 0, 0x5D1C = 1, 0x5D20 = 2, 30 x 0x40 words of two aux regions zeroed); 0x8038575C (sda 0x5D38 mode = 0, 0x5D28 = 0, 0x5D2C = command list base 0x8058ABC0, 0x5D34 = 1); 0x80385B68 = __AXOutInit(mode): sda 0x5D40/44/64 = 0, 0x5D70 = mode; zero the 6 output buffers (0x140 words x 6 + 0x780); 0x80385A9C builds the DSP task (iram 0x80564E60, callbacks 0x80385A0C / 0x80385A18 / 0x80385A70, DSP mail area sda 0x5D68); AIRegisterDMACallback(0x80385950) (0x80381E30); first __AXNextFrame; DCStoreRange; AIInitDMA (0x80381EFC); sda 0x5D48 = 1, 0x5D58 = 0 | the AXVPB array state (+0x18 index, stacks) is read by GCAX / CRI through AXAcquireVoice / the setters; the rest only by the engine | G (stacks / AXVPB init) + D (DSP task, AI DMA) |
| AXQuit 0x803846CC | 0x80384840 (stack zeroing, = 0x8038478C), 0x8038708C / 0x803863D4 / 0x80385780 (empty), 0x80384CAC (sda 0x5CF0 / 0x5CF4 = 0), 0x80385F08: OSSleepThread(queue sda 0x5D68) until the engine's callback wakes it, then AIStopDMA (blocking handshake with the frame engine and the scheduler; the wait point goes to extract/wait-points-retired.txt when removed) | game dtor 0x801F2DA8 | G (blocking) |
| AXFX hooks 0x803890B0 / 0x8038980C / 0x80389B5C | not engine: AXFX allocation hooks called from GCAX 0x8043F304 / 0x8043F33C (axfx-effects' families) | | other agent |

### What the table implies (the owners follow from it)
1. Game-visible behaviour the native engine must keep: (a) the DSP budget drop in __AXSyncPBs (cost table 0x80563348, budget sda 0x5DA0, per-voice cost from src type / mix flags / depop): pure arithmetic on AXVPB fields, port it as is; (b) the drop callbacks and their order (ServiceCallbackStack walk order, interrupts); (c) the frame callback and the two aux callbacks with the aux buffers the mixer wrote, in the original order within a frame (SyncPBs -> command list send -> ServiceCallbackStack -> aux flush -> frame callback -> NextFrame); (d) the AXVPB state words GCAX / CRI read back (+0x146 = state; embedded PB +0x138 + 0x7A = play position, which advance_ax_accepted_voices already writes natively); (e) AXQuit's sleep / wakeup handshake.
2. DSP-only traffic that disappears: the PB array and its three DCStore / Invalidate ranges, the command list and its cycle estimate (sda 0x5D2C/30/28/34), the mailbox handshake (0xBABE0180, list address, polling), the DSP task struct and its callbacks 0x80385A0C/18/70, the profile record (E, unread), __AXSyncPB's field copy (the native mixer reads the AXVPB embedded fields directly; the dirty word +0x1C then only has to be cleared, because the guest setters keep writing it: the stage c/d owners can stop writing it once the engine is native).
3. Remaining work, in order: (i) __AXSyncPBs' budget walk as a native function (contract: the set of dropped voices and the final +0x1C/+0x20/+0x28/+0x24/+0x30/+0x146 words vs the original on random AXVPB images; the PB copy part removed from the compared set); (ii) the frame sequencing (AI DMA callback -> native frame: SyncPBs, service callbacks via guest_call, aux flush, frame callback, mixing from AXVPB fields instead of the command list: this is the run_ax_mix refactor and the one with PCM risk); (iii) AXInit = native AXVPB / stack initialisation + native AI registration (the AI and DSP owners are already native), AXQuit = the wakeup handshake; (iv) delete the PB array, command lists and the 15 literal rows. PCM must stay 46a70a10 at each step.

## Stage e (i): real voice-drop counts (the "console DSP budget") 2026-11-08

Measured with the existing r5probes (`--measure r5probes --measure-probes sets=r5`; "force drops" = calls of __AXPushCallbackStack from __AXSyncPBs' over-budget branch, LR 0x80386D20; "stolen-voice predictions" = AXAcquireVoice entries that would steal), headless, SHADOW_NATIVE_JOBS=0, exe cand10.exe, via `.build/ab-jobs/drops.sh`:
Westopolis tape 90 s: 17,887 AX frames, 0 drops, 0 steals. Final Haunt tape (stg0604): 59,887 frames, 0 / 0. Lethal Highway tape (stg0202): 59,887 frames, 0 / 0 (both stopped at the same ~300 game-second cap). user-westopolis-ramp tape: 59,887 frames, 0 / 0. All 38 Select Mode stage boots (stg list of test_data/stage_loading/load_times_baseline.csv, no input, ~40 game seconds each, 7,887 frames): 0 drops, 0 steals in every one (per-run lines in docs/agents/audio-ax-drop-counts.txt). Total about 497,000 AX frames, **zero budget drops, zero steals**. The lava-shelter user recordings were not run (no boot stage recorded). Caveat: the stage boots are idle, so heavy-SFX moments (boss fights, 2P) are covered only by the tapes above.
Decision (main): the budget stays exact in step (ii), labelled "console DSP budget, preserved for identical output"; dropping it is a separate measured change.
Step (i) alone has no payoff: __AXSyncPBs' only consumer of its PB copy is the native mixer, so a native budget walk would still have to call __AXSyncPB (469 insns, SDK) per voice; the budget walk is therefore part of the native frame of step (ii).

## Stage e (ii) design: the native frame (written 2026-11-08 after reading __AXSyncPB and run_ax_mix; nothing implemented)

Findings that shrink the problem:
1. **The native mixer writes only three PB fields** (system.cpp advance_ax_accepted_voices): the current address words pb+122 / pb+124 (0x7A / 0x7C) and the state pb+14 (0xE). Everything else it keeps in host state (ax_position_fraction_, ADPCM history, SRC history, restart flags) and only READS the PB array. So the PB array's "DSP-owned" content is exactly state + current address; every other PB field is a copy of the AXVPB embedded block (+0x138, identical offsets).
2. **__AXSyncPB's copy direction** (read in full): with no dirty bits (+0x1C == 0) it copies PB -> AXVPB for state (+0xE), +0x64, the position halfwords +0x7A / +0x7C; with dirty bits it pushes AXVPB -> PB per bit (+8/+0xA src type (bit 0), +0xC (1), +0xE state (2 = bit 0x4: guest state wins, otherwise PB -> AXVPB), +0x10 (3), +0x12..0x34 mix block (4), +0x36..0x42 / +0x40/+0x42 (6 / 5: the 0x34-pointer ring reset), +0x44..0x4C ramp + the pending-write ring +0x28/+0x30/+0x34.. copied to 0x8058FCA0 + 0x4D00 + 0x100 * index (the DSP's update list), +0x52..0x62 (7), +0x64 / +0x66 (bits 0x15 / 0x16: both directions), +0x68..0x6C (0x14), +0x6E / +0x72 / +0x76 / +0x7A (bits 0x12..0xF: the loop / end / current address words; absent bits copy position back PB -> AXVPB), +0x7E..0xA2 (0xE: ADPCM coefficients), +0xA6.. (0xC / 0xD: ADPCM gain / pred-scale / yn1 / yn2), +0xB4.. (0xB), +0xBA.. +0xC0 (9 / 0xA: src history). bit 31 = full copy of the 0xCC-byte block. In other words: the PB array is a lagging copy of the AXVPB whose only guest-visible effect is the copy-back of state and position, one frame late.
3. **PB chain = all 64 PBs in index order** (AXInit 0x80386FF0.. links PB i -> i+1; the native chain walk (select_ax_parameter_blocks) gets ax_pb_count_ = 64 each frame and an unchanged identity assignment), so the native frame does not need the command list to know the voice order.
4. Nothing outside the AX SDK reads or writes the engine's SDA words (static scan, see the engine table).
5. The drop budget never fires on ~497 K measured AX frames, but stays exact (label: "console DSP budget, preserved for identical output").

Design (the owner `ax_frame` = __AXOutNewFrame's body, installed as an owner at 0x80385798, entered from the AI-DMA callback chain exactly where the guest engine was):
  a. **native sync** (replaces __AXSyncPBs + __AXSyncPB + the PB array): per voice in the original's priority-stack walk order (31..1, then 0): the depop part (0x803863D8: read its 15 insns first), the cost / budget test (cost table 0x80563348, budget sda 0x5DA0 = busclock * 0x51EB851F >> 7, total in sda 0x5DA4), the drop branch (PB fields to 0 => here: native DSP state of that voice reset; AXVPB +0x146 = 0; __AXPushCallbackStack(vpb) = native push on the sda 0x5CE8 list), the dirty-bit protocol reduced to what is visible: (state, position) copy-back / push between the native DSP state and the AXVPB embedded block per the bits above, then clear +0x1C/+0x20/+0x28/+0x24, +0x30 = self+0x34 exactly as __AXSyncPBs does. sda 0x5DA8 (sync count) keeps counting if anything reads it (nothing outside does: dropped with the profile).
  b. **native mix** from the AXVPB embedded blocks: AxPbView(memory, vpb + 0x138) replaces AxPbView(memory, pb); parse_ax_list's FrameSettings come from the sda state (mode sda 0x5D38, out buffer base + 0x280 * sda 0x5D40, aux buffers from sda 0x5D10 / 0x5D14 / 0x5D18 / 0x5D1C and the aux enable words) instead of the command list; the one-frame latency of the DSP result (mix of frame N is copied back at frame N+1's sync) is kept by holding state / position in native arrays until the next sync.
  c. **callbacks in the original order** via guest_call: __AXServiceCallbackStack (drop callbacks), aux flush (the aux A/B callbacks with the aux buffers), the frame callback (sda 0x5D58).
  d. **AXInit / AXQuit**: native init of the AXVPB array (index +0x18, sync +0x1C = 0x2000A4, +0x30 = self + 0x34), the stacks, the aux and output buffers; AI registration through the existing native AI owners; AXQuit's OSSleepThread / wakeup handshake on sda 0x5D68.
  e. delete: PB array, both command lists, the DSP task, the profile record, the ~25 guest bodies (15 literal rows) and the native parse_ax_list / command-list mailbox path.

Gate plan (main): (1) contract test: the native sync on random AXVPB images / priority stacks vs the original __AXSyncPBs (AXVPB dirty words, state, position copy-back, dropped set, callback stack contents, with the PB array reduced to {state, position} on the original side as the only compared PB bytes); (2) one-frame and 30-frame native frame vs original __AXOutNewFrame + run_ax_mix on recorded AXVPB images, per-voice comparison (position, state, mix output samples) as well as the final PCM; (3) Westopolis 2+2 alternating: PCM 46a70a10 at the default AND 015445f2 with GEKKO_NATIVE_BG_READ=0 GEKKO_NATIVE_EXPAND_JOBS=0 GEKKO_NATIVE_ONE=0, ticks / VI / frames, memory digests; (4) the measured drop count stays 0 (r5probes) on the four tapes + the 38 stage boots; (5) planner / what_if / coupling ratchet; (6) perf.
Open question for main before the mix-from-AXVPB part: the host decoder / mixer slot assignment (select_ax_parameter_blocks, ax_voice_pb_[] identity per PB address) is keyed by PB address; keying by voice index must keep the slot identity stable across frames (it does today for the unchanged chain), so reset_ax_voice() behaviour must stay identical for newly started voices.

### Amendment to the stage e (ii) design (2026-11-08, after step 1 passed its contract test)

Reading run_ax_mix against NativeAudio showed that "mix from the AXVPB instead of the PB array" would change the output: the guest setters and
MIXUpdateSettings run inside the frame callback, AFTER __AXSyncPBs and BEFORE the command list is mixed, and the original DSP only sees those AXVPB
changes at the NEXT frame's sync (the PB array is the DSP's lagging copy). A mixer reading the AXVPB would be one frame early: audible and
PCM-visible. The one-frame-late behaviour is the observable contract (main: keep it). So the lag has to be stored, and the PB array is exactly that storage.
Revised step order:
  step 1 (done, 84b2bf6): native __AXSyncPBs, PB array still in guest memory, mixer unchanged.
  step 2: move the lagging copy out of guest memory: a native per-voice image (64 x 0xF4 PB bytes + the 0x100-byte update-list ring), written by the native sync,
          read by the mixer (AxPbView over the native bytes, voices keyed by index, ax_voice_pb_ identity by index); state and current address written by
          advance_ax_accepted_voices into the image; savestate: the image is saved (version bump; a state of the old version converts by copying the guest PB array
          region 0x8058FCA0 / ring 0x805949A0 into the image); gate = PCM, per-voice comparison, savestate round trip mid-playback.
  step 3: the frame sequencing and AXInit / AXQuit native, the command list / mailbox path removed, guest bodies deleted.

### Stage e (ii) step 1 gates: native __AXSyncPBs (84b2bf6)
1. Contract test `ax_sync_contract_tests`: 260 random worlds (64 voices over priority stacks 0..31, random dirty words incl. the whole-block bit,
   states, depop flags, write rings, budgets from all-dropped to none-dropped; 177 worlds dropped at least one voice) equal the original in MSR and all of
   0x80000000..0x80600000 (PB array, AXVPB array, stacks, SDA words, update rings). The test found two reading errors of mine on the way: the whole-block
   arm copies the update ring DSP side -> AXVPB (not the other way), and DCInvalidateRange / DCStoreRange need a blr in the reference interpreter (no memory effect).
2. Game: Westopolis 90 s headless, alternating base11 (HEAD with 84b2bf6 reverted in the working tree, built, restored) / cand11: default configuration PCM
   46a70a1051347069 x4, SHADOW_NATIVE_JOBS=0 46a70a10 x4, old-loader switches off (GEKKO_NATIVE_BG_READ=0 GEKKO_NATIVE_EXPAND_JOBS=0 GEKKO_NATIVE_ONE=0)
   015445f28cd10698 x4; 3,645,000,000 ticks, 2,697 VI, 5,360 render frames in all 12; **memory block digests identical A/A, B/B and A/B (0 differing blocks)** in all three
   configurations. The owner serves one call per AX frame (5,887 in a 30 s run; the guest __AXSyncPB probe sees 0 executions, "pb table INVALID" there is the probe).
   Wall seconds per run: default 16/15/12/12 (A B A B), jobs=0 12/12/12/13, old 15/15/15/15: no difference.
3. Budget: unchanged arithmetic, 0 drops measured on ~497 K frames (see above); labelled "console DSP budget, preserved for identical output" in the header and the owner row.

### Stage e (ii) step 2 gates: the PB image is native (69316e3)
Design as amended: the DSP side's lagging copy of the PBs (64 x 0xF4) and the update rings (64 x 0x100) live in `AxPbImage` (ax_pb_image.h), written by the native sync
(native_ax_sync.h, now taking the image), read by the mixer (AxPbView over the image bytes for the SDK array's addresses, guest memory for any other PB address) and written
by advance_ax_accepted_voices (state, current address) through `System::ax_pb_write16`. Voices stay named by their former PB address, so the slot identity and the category tables
are untouched. Savestate version 31 appends the image after the audio block's effect history (+32,000 bytes: 33,491,058 -> 33,523,058); a state of an older version is converted by
`import_ax_pb_image_from_guest()` (copy of the guest PB array 0x8058FCA0 and rings 0x805949A0 that the old file restored).
1. Contract test ax_sync_contract_tests (260 worlds, 177 with drops) now compares the image to the original's guest PB array and update rings (every byte) and the rest of guest memory with those two regions masked: pass.
   ax_pb_tests passes after adapting its fixtures (they assumed: the audio block ends with the effect history -> both offsets-from-end now skip the 32,000-byte image; a PB at the SDK array's address
   lives in guest memory -> the same-identity runs use 0x80F20000/0x80F200F4; r13 = 0 in the fixtures -> `sync_entry()` sets the SDA base before calling the owner, and runs it before the MSR[DR] switch).
2. Game: Westopolis 90 s headless, alternating cand11 (step 1) / cand12 (this commit): default 46a70a10 x4, SHADOW_NATIVE_JOBS=0 46a70a10 x4, old-loader switches off 015445f2 x4; 3,645,000,000 ticks, 2,697 VI, 5,360 frames in all;
   A/A and B/B memory identical; A/B exactly one 64 KB block (0x80590000) differs, and peeks show all 2,571 differing bytes lie inside the guest PB array 0x8058FCA0..0x805939A0 (the guest copy is no longer written; the update-ring region and everything else is byte-identical).
   Wall seconds per run 11-18 both sides (no difference).
3. Savestate round trip mid-playback (save at tick 1,822,500,000 while running, then load the file; headless, jobs=0): baseline cand11 (v30 file) and candidate cand12 (v31 file): continuing-run PCM 97065cfafb0f27f4 identical,
   load-run PCM bdb766e4a15cbe7f identical, ticks 3,037,500,000 / VI 2,247 / frames 1,800 identical, memory blocks after the load run differ only in 0x80590000 (the PB array). Conversion: the OLD v30 file saved earlier (s9_base9) loaded by cand12 gives PCM 07ecae927744ec85,
   identical to cand11 loading it. Observation (not caused by this change): in BOTH builds the load run diverges from the continuing run at the same place (first differing PCM byte 38,401 = 0.3 s after the load start), i.e. a resumed run is not bit-identical to the uninterrupted one in the baseline either.

### Stage e (ii) step 3a gates: __AXNextFrame is native, the command list is a native next-frame list (878748c)

`gekko/native_ax_frame.h` owns `__AXNextFrame` (0x803850AC). The original assembles, once per AI frame at the end of `__AXOutNewFrame` (and from AXInit), a
0x300-byte DSP command list in one of two ping-pong slots (0x8058ABC0 + 0x300 n) from sda state: the output mode (sda 0x5D38), the aux A / aux B callbacks being
registered (0x5CF0 / 0x5CF4), the compressor switch (0x5D34) and the frame's output buffer; the NEXT frame hands it to the DSP. The list is a protocol between two
halves of one device and the only reader left is the native mixer, which needs the frame settings and the output buffer: the builder now stores those in
`AxLists` (two slots) and no list bytes exist. The mixer takes the slot the DSP mail names. The cost the list adds to the sync's budget (sda 0x5D30: 0x1A9 + 0x2E44 +
0x546 / 0x5E6 for mode 0 / 1 + 0xDED per aux / matrix command + 0xBB8 compressor + 0x2710 + 2) is reproduced exactly: "console DSP budget, preserved for identical
output". `parse_ax_list` stays only as the importer of states older than version 32 (the two guest slots) and as the grammar check of the tests' hand-built lists.
Savestate version 32 appends the two lists (13 bytes each) after the PB image.

* Contract test `ax_frame_contract_tests`: 600 random worlds (modes 0 / 1 / 2 / out of range incl. negative, aux A / aux B registered or not, compressor on / off, both slots)
  equal the original in the cost, in the list decoded from the original's bytes (head, output, mode, aux flags, compressor threshold / release / table address) and in
  every other guest word (list bytes and the left-behind list cursor masked).
* Game, Westopolis 90 s headless, cand12 (step 2) vs cand13, alternating A B A B: default PCM 46a70a1051347069 x4, jobs=0 46a70a10 x4, old-loader switches 015445f28cd10698 x4;
  3,645,000,000 ticks, 2,697 VI, 5,360 render frames in all 12. Memory block digests A/A and B/B identical; A/B differ in 3 blocks (the list bytes, the list cursor sda 0x5D2C
  and the dead stack frame of the original builder).
* Savestate: the uninterrupted run PCM 97065cfa (cand12 and cand13), the same v31 state loaded by cand12 and by cand13 (list import) bdb766e4, a v32 state loaded by cand13 bdb766e4.
* After merging gc-removal f700fa6 (01faf96): cand13 vs cand14 default and jobs=0 PCM 46a70a10 again (memory blocks differ only where the merged tree changed other subsystems).
  The old-loader configuration cannot be gated on the merged tree: it faults at game second 1.44 ("native interrupt delivery: the delivery's end needs the scheduler", the
  GCAX frame callback) in gc-removal f700fa6 itself (the exe in wt-gc-removal/.build/watch does the same, PCM e161989a). Reported to main; reference 015445f2 was last gated
  on step 3a before the merge.

### Stage e (ii) step 3b: the frame sequencing, AXInit and AXQuit are native; the DSP task path of the engine is gone

What the original engine did per 5 ms pass, measured and read: the AI DMA callback (`__AXOutAiCallback` 0x80385950) and the DSP task's resume callback
(`__AXOutSPCallback` 0x80385A20) alternated through a state word (sda 0x5D48); `__AXOutNewFrame` (0x80385798) ran from whichever came second with the time between them as
the sync's cycle argument. On the Westopolis tape (3,000 frames, a probe at the sync) the argument was 0 in every frame: the DSP always resumed before the next pass, so the
frame always ran from the AI callback with the state at 1. There is no DSP now: the AI DMA handler (NativeAudio::ai_handler) runs the frame as a step machine
(`ax_frame::frame_advance`) in the original's order:

  1. `__AXSyncPBs` (native since step 1), 2. the mixer takes the list the previous frame built (this is where the DSP's work happened inline), 3. `__AXServiceCallbackStack`:
  the drop callbacks of voices the sync dropped (guest calls), the voices go to the free stack, 4. the aux flush: aux A / aux B callbacks with the frame's aux buffer list
  (guest calls), the aux indices advance, 5. the frame callback (GCAX), 6. `__AXNextFrame`, 7. the output index advances and the next pass is programmed (AIInitDMA).

The callbacks run as the delivery's own guest calls (the handler asks for a call, the return resumes the next step; the phase lives in the delivery's scratch words), NOT as
nested `guest_call`s inside native code: the first attempt did that and faulted at game second 0.565 ("a nested guest call did not return to its link"): the GCAX frame callback
wakes a thread and the delivery's end has to reschedule, which a nested call cannot do (the documented limit of the seam). The scratch words also keep a saved state resumable.

Not reproduced because nothing reads them: the profile record (OSGetTime stamps, AXGetProfile's buffer: the game registers none), the depop block (`0x80385FB0`) and the depop sums
(sda 0x5D78..0x5D98: they only fed the list's depop record), the cache range calls on the aux buffers, the version banner (OSReport), the DSP task (struct, DSPAddTask, its INIT
callback and the spin on sda 0x5D5C, the cancel callback and AXQuit's OSSleepThread on sda 0x5D68), the DSP mailbox waits of `__AXOutNewFrame`. AXInit and AXQuit are owners
(0x8038465C, 0x803846CC): the AXVPB array, the voice and callback stacks, the free-stack pushes, the DSP side's PB image (links, own addresses, buffer and ring addresses), the budget
(sda 0x5DA0 = bus clock * 0x51EB851F >> 39), the aux sends, the output buffers, the first list, the AI callback (a token 0xAB5A0001, not an address: nothing may call it; the AI handler
recognizes it) and the first pass (AIInitDMA, AIStartDMA). Retired wait points (extract/wait-points-retired.txt): 0x803857EC, 0x80385800 (the mailbox spins of the frame), 0x80385B50 (AXInit's INIT spin).
The PB chain the mixer walks is derived (the SDK's PB i links to PB i + 1 and nothing ever changes it): no link words are read.

* Contract test `ax_out_contract_tests`: 400 random frames (random voice stacks, random callback stack with and without drop callbacks, aux A / aux B registered or not, modes 0..3,
  aux indices and flags, list and output indices, frame callback present or not, interrupts enabled or not) against the original's `__AXOutAiCallback` + `__AXOutSPCallback`: all guest
  memory, the order of every callback (a shared order log written by guest stubs), the arguments of every aux call (buffer list and context), the dropped voices in order, the list address handed
  to the mixer, the AIInitDMA arguments, the PB image against the original's PB array; 40 random AXInit + AXQuit rounds against the originals. Masked (not written natively): the command-list bytes, the DSP task and
  profile record, the retired depop sums and handshake words.

#### Step 3b gates (tree = gc-removal f700fa6 merged + step 3a; cand14 = 3a merged, cand16 = 3b; alternating A B A B)

* Westopolis tape 90 s headless: default PCM 46a70a1051347069 x4 and SHADOW_NATIVE_JOBS=0 46a70a10 x4 (both builds identical); 3,645,000,000 ticks, 2,697 VI, 5,360 render frames, 17,887
  audio blocks in all; memory block digests A/A and B/B identical, A/B differ in 4 blocks (command lists, the DSP task / profile / depop block, the retired sda words, the original's dead stack frames).
  Wall seconds 9-13 for both builds (no measurable change). The old-loader configuration is not available (pre-existing fault in gc-removal, see above).
* Other tapes, cand14 vs cand16, same PCM: Final Haunt (stg0604) 46d7f03da1fd3ba3, Lethal Highway SFX (stg0202) 96e02a3da206e766, user Westopolis ramp (stg0100) e05f516eca8b95e0.
* Savestate (v32 -> v32, no layout change): the uninterrupted run 97065cfa; the v31 state of cand12, the v32 state of cand13 and cand16's own state, loaded by cand16: bdb766e4 each (the same as the
  baseline's load; the resume divergence of the open finding below is unchanged).
* Tests: ax_out_contract_tests (400 frames, 40 init / quit rounds), ax_frame_contract_tests (600), ax_sync_contract_tests (the depop sums masked), ax_voice_contract_tests, ax_pb_tests, gekko_core_tests,
  native_file_api_tests (now lists ax_frame::Owners and names the address when one is owned twice), native_liveness_roots (both), native_wait_points, platform_runtime_coupling (the AI callback is a token, not an
  SDK address, so no new coupling). Not run on this step: the measured drop counts (the probes sit on the retired guest functions); the contract test and the unchanged sync arithmetic carry that.
* Still to do (separate commit): delete the DSP task / mailbox half of NativeAudio (no AX task is ever added now) and migrate the tests that drive the mixer through the mail protocol (ax_pb_tests) to
  the native list; then the savestate resume divergence.


### Stage e (ii) step 3c: the DSP half of NativeAudio is deleted

With the AX frame native no AX task is ever added, so the DSP task manager and the mailbox model of NativeAudio (`DSPCheckMailToDSP`, `DSPCheckMailFromDSP`, `DSPReadMailFromDSP`,
`DSPSendMailToDSP`, `DSPAddTask`, `DSPAssertTask`, `__DSP_insert_task` / `remove_task` / `boot_task`, the DSP interrupt handler with the task protocol, the AX microcode identification by
content and its constants, the mixer hook `set_mixer`) are removed: about 300 lines of native_audio.cpp and the owner rows. Kept: `DSPInit` (CARDInit, the card agent's save_store.cpp, still
serves it as its one shared effect: the DSP_DSP unmask and the DSP task words; drop the call there and this owner goes too), the AI and OS audio owners, the DSP DMA interrupt (ARAM transfer
completions: a different thing that only shares the name). `System::run_ax_mix` reports its list to the audio layer for the report counters (`note_command_list`). The savestate record of
NativeAudio keeps its ten words with the four retired ones written as zeros and ignored on load (the layout is unchanged; fold this into the next version that touches the record).
`ax_pb_tests` drive the mixer through `System::mix_ax_list` with the old command grammar parsed by the importer instead of mailing it to a DSP.

Savestate: version 33 (no layout change). States of older versions are converted at load: the AX engine's state is all in the SDK's words, the lists and the PB image, so
once the AI DMA callback word names the native frame (a token) and the DSP resume callback's effect is applied (state word 0x5D48 = 1) the engine continues natively, the pending DSP interrupt is dropped.
A state saved while the CPU was inside the former guest frame code (`__AXOutNewFrame`, the AI / DSP callbacks, the service of dropped voices, the aux flush, AXInit's handshake) has no code to
resume in: it faults loudly at the first DSP register access ("a guest 2-byte store of 0xBABE to 0xCC005000 reached the DSP interface", pc named). It is not refused up front: a pc range test needs
SDK address literals in the loader, which the runtime coupling ratchet counts. Loads: the v31 state of cand12, the v32 states of cand13 and cand19 and the v30 state of the step 1 baseline give
bdb766e4 / bdb766e4 / bdb766e4 / 07ecae92, identical to the baseline's loads; the uninterrupted run is 97065cfa.

Gate (cand16 = 3b vs cand17 = 3c, Westopolis, alternating): default and jobs=0 PCM 46a70a1051347069 x4 each, 3,645,000,000 ticks, 2,697 VI, 5,360 frames, **memory block digests identical A/A, B/B and A/B
(0 differing blocks)**; Final Haunt, Lethal Highway SFX and the ramp tape PCM equal to cand16's. Tests: ax_pb_tests, ax_out / ax_frame / ax_sync / ax_voice contract tests, native_file_api_tests, save_store_contract_tests,
os_interrupts_contract_tests, audio_runtime_tests, native_audio_mixer_tests, axfx_*, gcax_midi, options_audio_mix, native_liveness_roots (both), native_wait_points, platform_runtime_coupling.

## RESOLVED (2026-11-08): the "savestate resume divergence" was the test harness, not the savestate

The earlier round trips (scripts ss9..ss20.sh) loaded the state WITHOUT the Westopolis input tape (`--input-tape-play`), while the uninterrupted run replayed it: from the first
tape input after the load (0.3 s in) the game receives different controller input, the player and camera drift, and the 3D sounds' volumes follow. Found by tracing the mixer's inputs
(a temporary GEKKO_AX_TRACE in run_ax_mix, not committed): the first differing input of the loaded run is frame 58 after the load, voice 24's bus volumes (0x201F / 0x403F against 0x1EC0 /
0x3D80, the game's MIXUpdateSettings output), with identical source windows, ratios and everything before; nothing in the AX / audio state differs. With the same tape passed to the loaded run
(`--load-state --state-file ... --input-tape-play <tape>`) the loaded run is **bit-identical** to the uninterrupted run over the whole overlap (1,920,000 PCM bytes = 15 s, cand20 and the trace
build; the loaded run's first 4,096 bytes sit at offset 5,687,680 of the uninterrupted PCM). So a save / load resumes bit-identically as far as audio goes; the round-trip scripts above compare
loaded runs with each other (valid for conversion checks) and not with the uninterrupted run (the hash 97065cfa vs bdb766e4 difference is the missing input, nothing else). Gate scripts for future
audio state changes should pass the tape to the loaded run.

### Probe rows on retired AX functions removed (liveness)

`measure_probes.cpp` was the only seed keeping the retired guest bodies 0x80384914 (`__AXPushCallbackStack`), 0x80386474 (`__AXSyncPB`), 0x80384718 and 0x80385798 alive. Its six
built-in "r5" rows on retired functions (ai_dma_cb, ax_frame, ax_sync_pbs, pb_sync, ax_force_drop, ax_cb_service) read zero since step 3b/3c; they and their decoders / JSON sections
are removed (the AXAcquireVoice, AXRNA, Sofdec and movie rows stay). What-if checked by regenerating `liveness-roots*.txt` after the removal: 11 roots drop out of both files
(0x80384718 0x80384914 0x80385798 0x803857CC 0x80385808 0x80385990 0x80385A54 0x80386474 0x80386CD4 0x80386D20 0x80386D2C), nothing is added. Ran: numeric_probe_tests, ax_pb_tests,
native_liveness_roots (both), native_wait_points, platform_runtime_coupling: pass. One earlier regeneration of the lifted file produced extra unrelated roots and failed the check;
a second regeneration from the same tree gave only the 11 deletions and passed. The cause of the first output was not established. `sample_pbs` (the former ax_sync_pbs sampler) is now unreferenced dead code.
