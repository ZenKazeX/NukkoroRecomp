# sdk-identify: the 61 `sdk:unidentified` frontier rows, named from evidence (2026-11-07)

Analysis only. No runtime, generator input or `../source/.build` file was changed. Deliverables (all on branch `agent/sdk-identify`):

| file | content |
|---|---|
| `test_data/platform/unidentified_names.csv` | all 211 manifest functions that the manifest calls `sdk:unidentified`, plus the interior row 0x80385A20: proposed name, service, confidence, evidence, name kind, current name, live / pruned, frontier flag, size, who enters the row |
| `test_data/platform/unidentified_names.symbols.patch` | proposed patch for `pal-v30/config/symbols.txt` (162 renames: every PROVEN / INFERRED row whose name is an SDK symbol); `git apply --unidiff-zero -p1` checked against a copy of the file |
| `test_data/platform/unidentified_names.platform_deps.patch` | proposed 29 exact-name rules for `tools/dolrecon/platform_deps.py` (statics without a library prefix, and `SPEC2_MakeStatus`, which the existing `SP` rule would send to audio); `git apply --check` passes |
| `test_data/platform/unidentified_names.service_overrides.json` | address -> proposed service for every identified row, and the rows the generator's name rules would not derive |
| `tools/platform/apply_unidentified_names.py` | regenerates the two patches and the re-plan from the CSV (read-only on every input) |

## Result

* **61 of 61 frontier rows named, 0 UNKNOWN: 40 PROVEN, 21 INFERRED.** By service: audio.ax 44 (24 PROVEN, 20 INFERRED), saves.card 11 (all PROVEN), os.font 3 (2 PROVEN, 1 INFERRED), graphics.vi 1, input.pad 1, os.debug 1 (all PROVEN).
* Over the whole unidentified set (212 rows): 45 PROVEN, 123 INFERRED, 44 UNKNOWN. The UNKNOWN rows are all non-frontier: pruned or not-in-graph functions I did not open (CARD / PAD / AR internals, OSFatal statics) and AX helpers whose role is known but whose SDK symbol is not provable (name kind `descriptive`, e.g. `AX_depop_fade`).
* The removal plan counts 166 compiled `sdk:unidentified` bodies: 131 function-start rows plus 35 interior rows (jump-table targets and return points) that take their function's identity. After the proposed services 5 rows stay unidentified; the `sdk:unidentified` frontier goes 61 -> 0.
* The planner's service table changes materially (see "Summary by service"): the sound library appears as a 44-row `audio.ax` frontier, `saves.card` grows from 6 to 17 frontier rows, and two new small services (`os.font`, `input.pad`) appear.

## Method

Inputs (all read-only): the 2026-11-07 10:26 planner output in `../wt-gc-removal/.build/platform/removal_plan-watch/` (`live_graph.txt`, `removal_plan.json`; the 61 rows and who enters them come from there; I did not run the generator, so no `generator` lock was needed), `../source/.build/dolrecon/pal-v30` (platform manifest, `symbols.txt`), the pinned DOL through `tools/doldis.py` / `tools/dolxref.py`, `extract/functions-dolphin-sigs.csv`, `../research/sys_recon/sdk_verified_identities_v2.csv` (the apploader trailer correlation), `docs/GCAX_SOUND_MAP.md`, `docs/CRI_SERVERS.md`, the runtime sources.

Evidence, in the order I trusted it:

1. **Behaviour of the body**, read instruction by instruction: constants (`0xA5FF005A`, `0xBABE0180`, SJIS lead-byte ranges, parameter ranges), MMIO (`0xCC00206E` = VI_DTV_STAT), SDA globals and who else reads or writes them (found by scanning the DOL for `lwz/stw rN, d(r13)`: e.g. `0x5BDC`/`0x5BE0` are read only by the VI retrace handler `vi_8037E290`), structure offsets (the AX parameter block layout is consistent across the eight voice setters).
2. **Call graph and call sites**: the arguments the callers pass (GCAX passes `AXAcquireVoice(0x1B, 0x8043E74C, gvoice)`; the effect installer 0x8043F304 passes the AXFX callbacks and context objects to 0x80385050 / 0x8038505C), the order of sub-calls (AXInitEx and AXQuit call six sub-routines in the SDK's order), position inside the SDK link order (OSFont between OSReset and VI; VI.c starts with the two retrace-callback setters).
3. **Data words**: `0x805EEC84` is the MakeStatus slot of the PAD spec table; `0x805EECB8/BC` equal `r13+0x27B8/0x27BC`, the initial AXFX allocate / free hooks.
4. **The Dolphin signature names** (`functions-dolphin-sigs.csv`) and the apploader-trailer match (`sdk_verified_identities_v2.csv`: the disc's trailer DOL is a second image of the same Nov 2004 SDK). Both are *corroboration*, never the deciding evidence, because the signature database matches by instruction shape: every "store the argument into one global, return the old one" setter gets the same name. I rejected the signature on 0x8037E4F0 / 0x8037E534 (`AIRegisterDMACallback`, really `VISetPre/PostRetraceCallback`), 0x80385F60 (`AIRegisterDMACallback`, really `AXRegisterCallback`), 0x80382714 (really `ARRegisterDMACallback`), 0x80385050 / 0x8038505C (`AXFXSetHooks`, really `AXRegisterAuxA/BCallback`; the real `AXFXSetHooks` is 0x8038A12C), 0x80385068 / 0x8038646C (`IPCGetBufferLo`), 0x8038A2C0 (`IPCSetBufferLo`, really `MIXSetSoundMode`), 0x803B2F50 (`tanf`, really `powf`), 0x80392548 (`gdev_cc_shutdown`) and names from unrelated games (`LooseBallAnims::Destroy`). The CARD names are the case where the signature stands: the signature, the trailer's normalised match, and the body's own calls all agree.
5. **Reference sources**: the RW 3.7 SDK in `../RW37SDK` has no GameCube OS or audio code (nothing to match); the Dolphin and libogc snapshots in `../research/sources/repos` hold no SDK AX / MIX / AXFX / OSFont sources (libogc uses different names and structures), so they served only for hardware facts (VI DTV status register, EXI / CARD behaviour). The AX library names therefore rest on the SDK's *public API shape* plus the evidence above, and the confidence column says so.

Confidence:

* **PROVEN**: the function's observable behaviour is traced to the end and is decisive for the name (unique constants, argument shapes, a data word that can only be this slot, or call sites that pass exactly what the API takes), independently of any signature.
* **INFERRED**: the library and role are certain from structure and position, but the exact SDK symbol rests on the SDK's public API list or on internal statics I recall from the Nov 2004 library layout (`__AXOutNewFrame`, `__AXSyncPB`, `ReadFont`, ...). Static names may differ from Nintendo's; rely on the *role*.
* **UNKNOWN**: not identified, or role known without a provable symbol (name kind `descriptive`: the name is only a label).

## Per-row table: the 61 frontier rows

Full evidence for every row, and `entered_by` untruncated, is in the CSV. "Entered by" is the removal plan's description of who reaches the row from outside the SDK.

| address | name | service | confidence | entered by | decisive evidence |
|---|---|---|---|---|---|
| `0x80375150` | `OSGetFontEncode` | os.font | PROVEN | runtime literal in runtime/src/gekko/native_video.cpp | behaviour: u16 cache sda+0x2708 (0xFFFF = unset); if the TV-format word 0x800000CC is 0 reads VI_DTV_STAT 0xCC00206E bit1 -> SJIS(1) else ANSI(0); stores the string parser 0x80375618 in sda+0x5A88; callers = DVD ShowMessage ... |
| `0x80375DA0` | `OSInitFont` | os.font | INFERRED | runtime literal in runtime/src/gekko/native_video.cpp | one argument (font buffer) saved to sda+0x5A80 (ANSI header) / 0x5A84 (SJIS header); switch on OSGetFontEncode result (0 ANSI, 1 SJIS, 3..5 both) calling ReadFont 0x803751B4 then ExpandFontSheet 0x803759F4, returns 1/0; single ... |
| `0x80375FAC` | `OSGetFontTexture` | os.font | PROVEN | runtime literal in runtime/src/gekko/native_video.cpp | behaviour: (string, void** image, s32* x, s32* y, s32* width): parses one glyph code through the parser in sda+0x5A88, divides by cells-per-sheet (header +0x1A*+0x1C) to give sheet pointer / column / row, width from the byte ... |
| `0x8037E4F0` | `VISetPreRetraceCallback` | graphics.vi | PROVEN | game 0x8032D150 | swap-global-with-interrupts-off returning the old value; the global sda+0x5BDC is read and called with the retrace count (sda+0x5BCC) by the retrace handler vi_8037E290 at 0x8037E38C; game 0x8032D150 registers 0x8032D17C. The ... |
| `0x803817D4` | `SPEC2_MakeStatus` | input.pad | PROVEN | data-word: word at 0x805EEC84 | entered by the data word at 0x805EEC84: the MakeStatus slot of the PAD spec table (0x805EEC70..); decodes the raw SI poll words into PADStatus (button halfword, stick/substick/trigger/analog bytes), switching on analog mode ... |
| `0x8038465C` | `AXInit` | audio.ax | PROVEN | game 0x801F2E58 | wrapper that calls 0x80384680 with r3=0; game ctor 0x801F2E58 calls it after AIInit and before MIXInit (docs/GCAX_SOUND_MAP.md section 0) |
| `0x803846CC` | `AXQuit` | audio.ax | PROVEN | game 0x801F2DA8 | calls the six quit routines in the same order as AXInitEx; game dtor 0x801F2DA8 |
| `0x80384718` | `__AXServiceCallbackStack` | audio.ax | INFERRED | runtime literal in runtime/src/gekko/measure_probes.cpp | pops voices off the callback stack, calls vpb->callback (+0x10) with the vpb, removes it from its priority stack (0x80384940) and pushes it on the free stack (0x803848F4); called from __AXOutNewFrame 0x80385804; entered by a ... |
| `0x80384914` | `__AXPushCallbackStack` | audio.ax | INFERRED | runtime literal in runtime/src/gekko/measure_probes.cpp | pushes a voice on the list in sda+0x5CE8 (link at +8); entered by a runtime measurement literal; called from __AXSyncPBs 0x80386D1C |
| `0x803849D4` | `AXFreeVoice` | audio.ax | PROVEN | middleware.cri 0x803CF9E0; middleware.cri 0x803CFAB0; middleware.cri 0x803D0230 | interrupts off, removes the voice from its stack, if PB state == 1 sets vpb+0x20 = 1, __AXSetPBDefault 0x80386E44, pushes on the free stack; callers = CRI voice release 0x803CF9E0/0x803CFAB0/0x803D0230 (GCAX never frees) |
| `0x80384A54` | `AXAcquireVoice` | audio.ax | PROVEN | game 0x8043E528; game 0x8043E66C; middleware.cri 0x803CFAB0; unresolved 0x8043E74C; +1 more | (prio, callback, context): pops a free voice or steals the lowest-priority one below prio (calling its callback), links into the priority list, stores prio/callback/context at +0xC/+0x10/+0x14, __AXSetPBDefault; GCAX calls ... |
| `0x80385050` | `AXRegisterAuxACallback` | audio.ax | PROVEN | game 0x8043F304; game 0x8043F33C | stores (callback, context) into sda+0x5CF0/0x5CF8; call site 0x8043F3C4 (GCAX effect install 0x8043F304) passes the AXFX effect callback (0x803885A0 ReverbHi, 0x803890B0 ReverbStd, 0x8038980C Chorus, 0x80389B5C Delay) and its ... |
| `0x8038505C` | `AXRegisterAuxBCallback` | audio.ax | PROVEN | game 0x8043F304; game 0x8043F33C | twin of AXRegisterAuxACallback on sda+0x5CF4/0x5CFC; same install function, bus 1 (0x8043F3D0) |
| `0x803850AC` | `__AXNextFrame` | audio.ax | INFERRED | runtime literal in runtime/src/gekko/system.cpp | builds the DSP command list (16-bit command words through sda+0x5D2C into one of two 0x300-byte lists at 0x8058ABC0; calls the aux buffer accessors 0x80384CBC.. and __AXGetStudio/__AXGetPBs); called by __AXOutNewFrame ... |
| `0x80385784` | `AXSetMode` | audio.ax | INFERRED | game 0x8043E66C; game 0x8043F480 | stores the output mode in sda+0x5D38 only when it changes (read by __AXNextFrame/__AXProcessAux, value 2 selects the surround path); callers GCAX 0x8043E680 (mode 0 at init) and 0x8043F4AC/0x8043F4B8 (2 = surround on, 0 = off) |
| `0x80385798` | `__AXOutNewFrame` | audio.ax | INFERRED | runtime literal in runtime/src/gekko/measure_probes.cpp; runtime literal in runtime/src/gekko/measure_probe... | frame step: OSGetTime stamps into the profile record, __AXSyncPBs, mails 0xBABE0180 and the list address to the DSP (DSPSendMailToDSP), __AXServiceCallbackStack, calls the user callback sda+0x5D58 (AXRegisterCallback), ... |
| `0x80385950` | `__AXOutAiCallback` | audio.ax | INFERRED | runtime literal in runtime/src/gekko/measure_probes.cpp; runtime literal in runtime/src/gekko/measure_probe... | AI DMA-done callback (registered by __AXOutInit through AIRegisterDMACallback): state machine sda+0x5D48 -> __AXOutNewFrame or DSPAssertTask(task 0x8058FBC0), AIInitDMA of the next 0x280-byte buffer; no static caller |
| `0x80385A20` | `__AXOutSPCallback` | audio.ax | INFERRED | runtime literal in runtime/src/gekko/measure_probes.cpp | interior generator row of 0x80385A18 (same function); rename the function at 0x80385A18 |
| `0x80385F60` | `AXRegisterCallback` | audio.ax | PROVEN | game 0x801F2E58 | swap-global returning the old value; global sda+0x5D58 is called once per 3 ms frame by __AXOutNewFrame; game ctor 0x801F2E58 calls it with the GCAX frame callback 0x8043F3E4 (docs/GCAX_SOUND_MAP.md section 0). Dolphin ... |
| `0x80386474` | `__AXSyncPB` | audio.ax | INFERRED | runtime literal in runtime/src/gekko/measure_probes.cpp | copies the dirty parts of a voice's PB (vpb+0x138, sync flags at vpb+0x1C: bit0 select, bit2 state, 0x21000 addr ...) into the DSP-visible array (0x8058FCA0 + vpb+0x18 * 0xF4); increments sda+0x5DA8; entered by measure_probes ... |
| `0x80386BC0` | `__AXSyncPBs` | audio.ax | INFERRED | runtime literal in runtime/src/gekko/measure_probes.cpp, runtime/tests/ax_pb_tests.cpp, runtime/tests/gekko... | loops the voice priority lists via __AXGetStackHead, DCInvalidateRange of the PB array, __AXSyncPB / depop accumulate, __AXPushCallbackStack for finished voices, DCFlushRange; called first thing by __AXOutNewFrame 0x803857C8; ... |
| `0x80387090` | `AXSetVoiceSrcType` | audio.ax | PROVEN | game 0x8043DACC; middleware.cri 0x803CED50; middleware.cri 0x803CFAB0 | type 0..4 selects pb srcSelect (+8) / coefSelect (+0xA): 0->(2), 1->(1), 2->(0,0), 3->(0,1), 4->(0,2), sync bit 1; GCAX table docs/GCAX_SOUND_MAP.md section 4; CRI 0x803CED50/0x803CFAB0 |
| `0x80387154` | `AXSetVoiceState` | audio.ax | PROVEN | game 0x8043CE54; game 0x8043DACC; middleware.cri 0x803CF650; unresolved 0x8043D0D4; +1 more | stores the PB run state halfword at pb+0xE (vpb+0x146), sync bit 4, state 0 also sets vpb+0x20 = 1; 5 callers (GCAX 0x8043CE54/0x8043DACC, CRI 0x803CF650, unresolved 0x8043D0D4/0x8043E74C) |
| `0x803871B0` | `AXSetVoiceMix` | audio.ax | INFERRED | game 0x8043D2F4 | copies the mixer halfwords from pb+0x12 (vpb+0x14A) and ORs a per-field sync flag (1,9,2,0xA,0x10,0x50,0x20,0x60,... 0x200) for every non-zero field; GCAX 0x8043D2F4 |
| `0x80387404` | `AXSetVoiceAddr` | audio.ax | PROVEN | game 0x8043DACC; middleware.cri 0x803CF650 | switch on the address-block format at +2: 0x0A (PCM16) / 0x19 (PCM8) / default (ADPCM) clears the loop/current fields in the PB address block (vpb+0x1A6) and sets sync 0x21000; copies 16 bytes |
| `0x80387514` | `AXSetVoiceAdpcm` | audio.ax | INFERRED | game 0x8043DACC | copies the ADPCM block into vpb+0x1B6, sync bit 0x20000 |
| `0x803875B8` | `AXSetVoiceSrc` | audio.ax | INFERRED | game 0x8043DACC; middleware.cri 0x803CED50; middleware.cri 0x803CFAB0 | copies the 7-halfword SRC block (ratio hi/lo, current frac, last samples) into vpb+0x1DE, sync bit 0x40000 |
| `0x80387650` | `AXSetVoiceSrcRatio` | audio.ax | PROVEN | game 0x8043D1C0 | (vpb, float ratio): ratio*65536.0 (lfs 0xF50(r2)) converted twice, clamped at 0x40000, stored as hi/lo halfwords at vpb+0x1DE/0x1E0, sync bit 0x80000; GCAX pitch 0x8043D1C0 |
| `0x803876E8` | `AXSetVoiceAdpcmLoop` | audio.ax | INFERRED | game 0x8043DACC | copies the 3-halfword ADPCM loop block (pred_scale, yn1, yn2) into vpb+0x1EC, sync bit 0x100000 |
| `0x80388FF8` | `AXFXReverbStdInit` | audio.ax | PROVEN | game 0x8043F9A0 | interrupts off, clears tempDisableFX byte +0x13C, passes the five floats at +0x140..+0x150 to __ReverbSTDCreate; the parameter set is {coloration 0..1, mix 0..1, time 0.01..10, damping 0..1, preDelay 0..0.1} (constants at ... |
| `0x80389064` | `AXFXReverbStdShutdown` | audio.ax | PROVEN | game 0x8043F674 | interrupts off, __ReverbSTDFree, restore; called from the sound shutdown 0x8043F674 on the same context (GCAX-0x1FC) |
| `0x803890B0` | `AXFXReverbStdCallback` | audio.ax | PROVEN | game 0x8043F304; game 0x8043F33C | aux callback (passed to AXRegisterAuxACallback by 0x8043F304 case 1): thin wrapper calling __ReverbSTDCallback 0x80388B20 |
| `0x80389464` | `AXFXChorusInit` | audio.ax | PROVEN | game 0x8043F9A0 | validates baseDelay in [5,15] ms, variation <= 5 ms and period in [500,10000] ms, allocates 0x1680 bytes (3 channels x 0x780 sample lines) through the AXFX alloc hook sda+0x27B8; installed as 'type 3' (callback 0x8038980C, ... |
| `0x803896AC` | `AXFXChorusShutdown` | audio.ax | PROVEN | game 0x8043F674 | interrupts off, frees the chorus lines through the hook sda+0x27BC; sound shutdown 0x8043F674 |
| `0x8038980C` | `AXFXChorusCallback` | audio.ax | PROVEN | game 0x8043F304; game 0x8043F33C | aux callback of the chorus (0x8043F304 case 3), 848 B, calls the two chorus helpers 0x803890E0 / 0x80389278 |
| `0x80389B5C` | `AXFXDelayCallback` | audio.ax | PROVEN | game 0x8043F304; game 0x8043F33C | aux callback of the delay effect (0x8043F304 case 4, context GCAX+0x1D4) |
| `0x80389FF4` | `AXFXDelayInit` | audio.ax | PROVEN | game 0x8043F9A0 | zeroes +0x30..+0x38 under interrupts-off then __DelayInit; parameter ranges delay 10..5000 ms, feedback/output 0..100 % (three channels); 'type 4' of the installer |
| `0x8038A03C` | `AXFXDelayShutdown` | audio.ax | PROVEN | game 0x8043F674 | interrupts off, frees the delay buffers (160 B); sound shutdown 0x8043F674 |
| `0x8038A0DC` | `__AXFXAllocFunction` | audio.ax | INFERRED | data-word: word at 0x805EECB8 | calls OSAllocFromHeap(sda+0x26E0 = __OSCurrHeap, size) 0x80371FB0; entered by the data word 0x805EECB8 = r13+0x27B8, the initial value of the AXFX alloc hook |
| `0x8038A104` | `__AXFXFreeFunction` | audio.ax | INFERRED | data-word: word at 0x805EECBC | calls OSFreeToHeap(__OSCurrHeap, ptr) 0x803720AC; entered by the data word 0x805EECBC = r13+0x27BC, the initial free hook |
| `0x8038A12C` | `AXFXSetHooks` | audio.ax | PROVEN | game 0x8043F9A0 | stores (alloc, free) into sda+0x27B8/0x27BC, the hooks read by the three effect inits; Dolphin signature (axfx.a AXFXHooks.o) agrees |
| `0x8038A1F4` | `MIXInit` | audio.ax | PROVEN | game 0x801F2E58 | initialises 64 channels of 0x60 bytes at 0x805A14A0 (defaults: update 0x50000000, aux -960, pan 64, span 127 ...), sets sda+0x5DC0/0x5DC4 = 0 and the sound mode sda+0x5DC8 = 1; game ctor calls it right after AXInit ... |
| `0x8038A2BC` | `MIXQuit` | audio.ax | INFERRED | game 0x801F2DA8 | empty blr; game dtor 0x801F2DA8 calls it next to AXQuit |
| `0x8038A2C0` | `MIXSetSoundMode` | audio.ax | INFERRED | game 0x801F21AC | stores r3 into sda+0x5DC8, the value __MIXResetPan compares with 3 (DPL2) and MIXInit sets to 1 (stereo); game 0x801F21AC calls it at 0x801F2210/0x801F222C/0x801F2248. Dolphin signature IPCSetBufferLo rejected |
| `0x8038A2C8` | `MIXInitChannel` | audio.ax | PROVEN | game 0x8043DACC; middleware.cri 0x803CFAB0 | 8 arguments (vpb, mode, input, auxA, auxB, pan, span, fader) initialising the channel at 0x805A14A0 + vpb+0x18*0x60; GCAX calls MIXInitChannel(vpb,0,0,-60,-60,64,0,0) (docs/GCAX_SOUND_MAP.md section 4); CRI 0x803CFAB0 |
| `0x8038B144` | `MIXReleaseChannel` | audio.ax | INFERRED | game 0x8043CE54; middleware.cri 0x803CF9E0; middleware.cri 0x803CFAB0; middleware.cri 0x803D019C; +3 more | clears the active word of channel[vpb+0x18]; 7 callers (GCAX voice release 0x8043CE54/0x8043E74C, CRI) |
| `0x8038B164` | `MIXSetInput` | audio.ax | INFERRED | middleware.cri 0x803CECA4 | stores r4 into channel +8 (input) and sets update flag 0x10000000; caller CRI adxt_SetOutVol 0x803CECA4 |
| `0x8038B18C` | `MIXSetPan` | audio.ax | INFERRED | middleware.cri 0x803CEBF0; middleware.cri 0x803CFAB0 | clamps r4 to 0..127 into channel +0x14, __MIXResetPan, update flag 0x40000000; CRI 0x803CEBF0/0x803CFAB0 |
| `0x8038B1F8` | `MIXUpdateSettings` | audio.ax | PROVEN | unresolved 0x8043F3E4 | walks the 64 channels applying the update flags to the voice PBs (mixer/aux volumes) and sets the stream volume through AISetStreamVolLeft/Right 0x803821EC/0x80382218; called once per frame at the end of the GCAX frame ... |
| `0x8038CDD8` | `__CARDDefaultApiCallback` | saves.card | PROVEN | runtime literal in runtime/src/gekko/native_card.cpp, runtime/tests/gekko_core_tests.cpp | a single blr; its address is materialised by CARDFormatAsync/CreateAsync/ReadAsync/WriteAsync/DeleteAsync as the default API callback (6 sites); runtime/src/gekko/native_card.cpp:38 already calls it that |
| `0x803918C4` | `__CARDFormatRegionAsync` | saves.card | PROVEN | runtime literal in runtime/src/gekko/native_video.cpp, runtime/tests/gekko_core_tests.cpp; runtime literal ... | 1624 B; takes __OSLockSramEx/__OSLockSram, OSGetTime x8 to build the format serial, materialises __CARDDefaultApiCallback; Dolphin signature; entered by runtime literals 0x80391910/0x80391914 (return points inside the body, ... |
| `0x80391F1C` | `CARDFormatAsync` | saves.card | PROVEN | game 0x8029AF60 | (chan, callback): obtains one word via 0x8038DEE8 and passes it as the region argument to __CARDFormatRegionAsync 0x803918C4; entered by the game save code 0x8029AF60 |
| `0x803923D8` | `CARDOpen` | saves.card | PROVEN | game 0x8029B188; game 0x8029B270 | Dolphin signature + trailer match (71 ins, B-normalised); game save code 0x8029B188/0x8029B270; brackets the work with the card control-block get/put pair 0x8038DF28/0x8038DFE0 and uses __CARDGetFileNo/__CARDCompareFileName |
| `0x803924F4` | `CARDClose` | saves.card | PROVEN | game 0x8029B94C; game 0x8029B9C0; game 0x8029C034 | Dolphin signature + trailer match; game 0x8029B94C/0x8029B9C0/0x8029C034 |
| `0x80392680` | `CARDCreateAsync` | saves.card | PROVEN | game 0x8029AE64 | Dolphin signature + trailer match (136 ins); game 0x8029AE64 |
| `0x80392B88` | `CARDReadAsync` | saves.card | PROVEN | game 0x8029AC18 | Dolphin signature + trailer match; calls __CARDSeek and __CARDRead 0x8038F56C after DCInvalidateRange; game 0x8029AC18 |
| `0x80392EEC` | `CARDWriteAsync` | saves.card | PROVEN | game 0x8029AB1C | Dolphin signature + trailer match; __CARDSeek, DCStoreRange, __CARDEraseSector 0x8038DD2C; game 0x8029AB1C |
| `0x803930A4` | `CARDDeleteAsync` | saves.card | PROVEN | game 0x8029AD14 | Dolphin signature; behaviour: __CARDGetFileNo, memset(entry, 0xFF, 0x40) 0x8000540C, update directory 0x8038FCC4; game 0x8029AD14 |
| `0x803933AC` | `CARDGetStatus` | saves.card | PROVEN | game 0x8029A66C; game 0x8029A690; game 0x8029AFC8; game 0x8029B0F4 | Dolphin signature + trailer match; game 0x8029A66C/0x8029A690/0x8029AFC8/0x8029B0F4 |
| `0x803934C0` | `CARDSetStatusAsync` | saves.card | PROVEN | game 0x8029A85C | Dolphin signature; OSGetTime for the modify time, UpdateIconOffsets, directory update; game 0x8029A85C |
| `0x803B57D8` | `InitializeUART` | os.debug | PROVEN | unresolved 0x803AF5A8 | behaviour: sda+0x5EE4 == 0xA5FF005A -> return 0; else OSGetConsoleType() 0x80370E04 bit 0x10000000 clear -> Enabled = 0, return 2; else Enabled = 0xA5FF005A (sda+0x5EE0), return 0; entered by 0x803AF5A8 (unresolved layer: the ... |

## Findings other sessions need

* **axfx-effects**: the three effects the sound start-up initializes but never registers are identified by their parameter validation (PROVEN): 0x80388FF8 / 0x80389064 = **AXFXReverbStd** init / shutdown (context GCAX - 0x1FC; five floats coloration, mix, time, damping, preDelay with their documented ranges; callback 0x803890B0 -> 0x80388B20), 0x80389464 / 0x803896AC = **AXFXChorus** (GCAX + 0x138; baseDelay 5..15 ms, variation <= 5 ms, period 500..10000 ms; callback 0x8038980C; helper init 0x803896FC), 0x80389FF4 / 0x8038A03C = **AXFXDelay** (GCAX + 0x1D4; three delays 10..5000 ms, feedback and output 0..100; callback 0x80389B5C; helper 0x80389D2C). Registration is by `0x8043F304` (type 1 = Std, 2 = Hi, 3 = Chorus, 4 = Delay) through `AXRegisterAuxA/BCallback`. All three allocate through the AXFX hooks `sda+0x27B8/0x27BC` (defaults `__AXFXAllocFunction` 0x8038A0DC / `__AXFXFreeFunction` 0x8038A104, which are `OSAllocFromHeap` / `OSFreeToHeap` on `__OSCurrHeap`). Std reverb creation calls `powf` (0x803B2F50, a float wrapper of MSL `pow`) with base 10.
* **card-saves**: `saves.card` has **17** frontier rows, not 6. The 11 added rows are the SDK API entry points the game's save code calls directly: `CARDFormatAsync` 0x80391F1C, `CARDOpen` 0x803923D8, `CARDClose` 0x803924F4, `CARDCreateAsync` 0x80392680, `CARDReadAsync` 0x80392B88, `CARDWriteAsync` 0x80392EEC, `CARDDeleteAsync` 0x803930A4, `CARDGetStatus` 0x803933AC, `CARDSetStatusAsync` 0x803934C0, plus `__CARDFormatRegionAsync` 0x803918C4 and `__CARDDefaultApiCallback` 0x8038CDD8 (entered by runtime literals). A Saves contract that owns only the 6 previously named rows leaves these 11, and the card library's interior (65 bodies freed in the cumulative order), compiled.
* **cri-threads / CRI**: CRI enters the AX library directly (frontier rows `AXAcquireVoice`, `AXFreeVoice`, `AXSetVoiceSrcType`, `AXSetVoiceState`, `AXSetVoiceAddr`, `AXSetVoiceSrc`, `MIXInitChannel`, `MIXReleaseChannel`, `MIXSetInput`, `MIXSetPan` from 0x803CEBF0..0x803D0230), so the CRI voice code and GCAX share the 44-row audio frontier.
* **native-boot**: `InitializeUART` 0x803B57D8 is entered only from the unresolved-layer function 0x803AF5A8; the OSFont rows are entered by game code (0x8032D4CC.. font setup) and DVD `ShowMessage`; neither is on the boot path.
* **Generator row anomalies seen**: the live graph lists `0x80375618` (`ParseStringS`) as a 16-byte row while `symbols.txt` gives it 0x13C bytes; `0x80385A20` is live as an interior row of `0x80385A18` (`__AXOutSPCallback`), which is not itself a live row. Same family as the `fn_801645D0` anomaly noted in `docs/GC_REMOVAL_PLAN.md`.

## Summary by service, with the removal leverage each identification changes

`tools/platform/apply_unidentified_names.py` re-runs the planner's own classification, frontier, freed-set and greedy-order definitions (`removal_plan.Graph`) on the existing live graph with the proposed services substituted. The *before* half reproduces `removal_plan.json` exactly (same order, same counts), which is the check that the re-plan is faithful. It is a prediction from the planner's reachability model, not a generator run; `what_if.py` remains the gate for any real step.

Capped run (`capped_run.ps1`, 10 % cap): the re-plan took 24.5 s wall, 23.3 s job CPU = 2.98 % of 32 logical processors.

Platform bodies compiled 1044 -> 1043 (`powf` is MSL, not SDK); frontier rows 289 -> 289 (the 61 rows were already frontier; they were only unassigned).

| service | bodies before | bodies after | frontier before | frontier after | freed before | freed after |
|---|---:|---:|---:|---:|---:|---:|
| graphics.rw-gamecube-driver | 383 | 383 | 96 | 96 | 368 | 368 |
| graphics.gx | 292 | 292 | 64 | 64 | 158 | 158 |
| audio.ax | 0 | 104 | 0 | 44 | 0 | 65 |
| saves.card | 26 | 48 | 6 | 17 | 6 | 40 |
| os.other | 45 | 45 | 20 | 20 | 24 | 24 |
| graphics.vi | 12 | 31 | 6 | 7 | 5 | 5 |
| os.threads | 26 | 26 | 17 | 17 | 7 | 7 |
| cpu.config | 22 | 22 | 1 | 1 | 0 | 0 |
| debug.trk | 16 | 17 | 5 | 5 | 8 | 8 |
| os.interrupts | 16 | 17 | 4 | 4 | 2 | 2 |
| os.core | 17 | 17 | 3 | 3 | 33 | 33 |
| os.font | 0 | 10 | 0 | 3 | 0 | 7 |
| sdk:unidentified | 166 | 5 | 61 | 0 | 90 | 0 |
| os.memory | 5 | 5 | 3 | 3 | 0 | 0 |
| cpu.cache | 5 | 5 | 0 | 0 | 0 | 0 |
| os.sram | 4 | 4 | 0 | 0 | 0 | 0 |
| os.time | 3 | 3 | 2 | 2 | 0 | 0 |
| os.alarms | 2 | 2 | 0 | 0 | 0 | 0 |
| os.debug | 1 | 2 | 1 | 2 | 0 | 0 |
| audio.dsp | 2 | 2 | 0 | 0 | 0 | 0 |
| input.pad | 0 | 1 | 0 | 1 | 0 | 0 |
| os.reset | 1 | 1 | 0 | 0 | 0 | 0 |
| audio.ai | 0 | 1 | 0 | 0 | 0 | 0 |

What it means:

* **audio.ax (new, 104 bodies, 44 frontier rows).** The previous plan showed the sound library as nothing (0 bodies): its entry rows were hidden in `sdk:unidentified`. A native AX / MIX / AXFX engine (GCAX phase 3, or retiring GCAX) has to own 44 rows: 24 PROVEN public API entries (AXInit, AXQuit, AXAcquireVoice, AXFreeVoice, the voice setters, AXRegisterAuxA/B, AXRegisterCallback, AXFXSetHooks, the three AXFX effect trios, MIXInit, MIXInitChannel, MIXUpdateSettings) and 20 INFERRED statics reached by runtime measurement literals, tests or other rows (`__AXSyncPBs`, `__AXSyncPB`, `__AXNextFrame`, `__AXOutNewFrame`, `__AXOutAiCallback`, `__AXOutSPCallback`, `__AXServiceCallbackStack`, `__AXPushCallbackStack`, the MIX setters). Owning them frees 65 bodies on their own and 109 in the cumulative order (step 9). The static rows are entered by `measure_probes.cpp` and test literals, so removing those literals (or moving the probes to owner rows) is part of the work.
* **saves.card: 6 -> 17 frontier rows, 6 -> 40 freed alone.** The Saves contract must cover the API calls above; the cumulative step now frees 65 (was 21).
* **os.font (new, 10 bodies, 3 frontier)** and **input.pad (1)** are small, independent steps: OSInitFont / OSGetFontEncode / OSGetFontTexture (game font setup) and the PAD spec-table slot; os.font frees 10 in the cumulative order, right after card.
* **sdk:unidentified is gone from the plan** (166 bodies, 61 frontier, 167 freed -> 5 bodies, 0 frontier): the 167-row step the old order placed 6th was really the audio engine (44 rows), the card API (11), the font library (3) and three singletons (VI, PAD, UART); the greedy order now puts audio.ax 9th, after the OS services whose frontier was already known.
* **graphics.vi** gains the two retrace-callback setters (frontier 6 -> 7, bodies 12 -> 31 with the interior jump-table rows).

Greedy removal order before:

| step | service | frontier | freed | left |
|---:|---|---:|---:|---:|
| 1 | os.core | 3 | 36 | 1008 |
| 2 | graphics.rw-gamecube-driver | 96 | 441 | 567 |
| 3 | graphics.gx | 64 | 255 | 312 |
| 4 | saves.card | 6 | 21 | 291 |
| 5 | debug.trk | 5 | 14 | 277 |
| 6 | sdk:unidentified | 61 | 167 | 110 |
| 7 | os.other | 20 | 53 | 57 |
| 8 | os.interrupts | 4 | 10 | 47 |
| 9 | graphics.vi | 6 | 11 | 36 |
| 10 | os.threads | 17 | 29 | 7 |
| 11 | os.memory | 3 | 3 | 4 |
| 12 | os.time | 2 | 2 | 2 |
| 13 | os.debug | 1 | 1 | 1 |
| 14 | cpu.config | 1 | 1 | 0 |
| 15 | os.sram | 0 | 0 | 0 |
| 16 | os.reset | 0 | 0 | 0 |
| 17 | os.alarms | 0 | 0 | 0 |
| 18 | cpu.cache | 0 | 0 | 0 |
| 19 | audio.dsp | 0 | 0 | 0 |

Greedy removal order after:

| step | service | frontier | freed | left |
|---:|---|---:|---:|---:|
| 1 | os.core | 3 | 36 | 1007 |
| 2 | graphics.rw-gamecube-driver | 96 | 441 | 566 |
| 3 | graphics.gx | 64 | 255 | 311 |
| 4 | saves.card | 17 | 65 | 246 |
| 5 | os.font | 3 | 10 | 236 |
| 6 | debug.trk | 5 | 14 | 222 |
| 7 | os.other | 20 | 53 | 169 |
| 8 | os.interrupts | 4 | 10 | 159 |
| 9 | audio.ax | 44 | 109 | 50 |
| 10 | graphics.vi | 7 | 12 | 38 |
| 11 | os.threads | 17 | 29 | 9 |
| 12 | os.memory | 3 | 3 | 6 |
| 13 | os.time | 2 | 2 | 4 |
| 14 | os.debug | 2 | 2 | 2 |
| 15 | input.pad | 1 | 1 | 1 |
| 16 | cpu.config | 1 | 1 | 0 |
| 17 | sdk:unidentified | 0 | 0 | 0 |
| 18 | os.sram | 0 | 0 | 0 |
| 19 | os.reset | 0 | 0 | 0 |
| 20 | os.alarms | 0 | 0 | 0 |
| 21 | cpu.cache | 0 | 0 | 0 |
| 22 | audio.dsp | 0 | 0 | 0 |
| 23 | audio.ai | 0 | 0 | 0 |

## Cross-check against the stopped duplicate pass (aside/gc-duplicates-main, 9956f56)

Read only as a comparison (`git show 9956f56:docs/agents/sdk-identify-classification.csv`); none of its names were copied. Of 211 shared rows, 159 agree in name or fall in the same "unnamed helper" class; 52 differ, mostly because it left OSFont/OSFatal statics and AX accessors generic (marked L/M). Substantive disagreements, with the side my own evidence supports:

| address | this report | duplicate pass | decided by |
|---|---|---|---|
| 0x80384914 / 0x803848F4 | `__AXPushCallbackStack` / `__AXPushFreeStack` | swapped | 0x80384914 links onto the list in sda+0x5CE8, which `__AXPopCallbackStack` 0x80384924 and `__AXServiceCallbackStack` drain; 0x803848F4 pushes onto table 0x80587DC0, the free stack `AXAcquireVoice` pops. Mine. |
| 0x80386474 / 0x80386BC0 | `__AXSyncPB` / `__AXSyncPBs` | `__AXSyncPBs` / `__AXServiceVPB` | 0x80386BC0 is the loop over the voice stacks and calls 0x80386474 once per voice; 0x80386474 copies one PB. Mine. |
| 0x803B2F50 | `powf` | `tanf` | callers pass two float arguments (f1 = 10.0, f2 = -3*N/(time*rate)); `tan` takes one. Mine. |
| 0x80384680 | `AXInitEx` | `AXInitSpecifyMem` | it takes one argument (output mode, forwarded to `__AXOutInit`), no memory arguments. Mine. |
| 0x8038B164 / 0x8038B18C | `MIXSetInput` / `MIXSetPan` | `MIXSetFader` / `MIXSetInput/Pan` (L) | 0x8038B18C clamps 0..127 into the field MIXInit defaults to 64 (pan): agrees with both. 0x8038B164 writes channel+8, the first value after the mode/update words, which I take as input; this stays INFERRED and could be the fader. Unresolved. |
| 0x8038CDD8 | `__CARDDefaultApiCallback` (PROVEN) | blr stub, L | six CARD API functions materialise its address as the default callback and `native_card.cpp:38` names it. Mine. |

Where it adds information I did not have: its `what_if` runs (per its hand-over, not re-run by me): owning the os.font rows is refused until the stale sites 0x80375178 / 0x80375DDC / 0x8037600C leave the site lists; the 10 card rows free 18; ax-core 41; the axfx rows free only 3 although the helpers have the owned init as sole xref (cause unknown, check before relying on it). Those are for the step owners to re-measure; my re-plan above is a graph prediction, not a generator run.

## Other unidentified bodies (non-frontier)

All 151 non-frontier manifest rows are in the CSV. Highlights: the whole AX internal structure (`__AXAlloc*`, `__AXVPB*`, `__AXSPB*`, `__AXAux*` accessors, `__AXCl*`, `__AXOut*`, `__AXSyncPB(s)`, `__AXSetPBDefault`) is INFERRED from the AXInitEx / AXQuit call order and the data each touches; the OSFont internals (`GetFontCode`, `Decode` (a Yaz0 decoder), `ReadFont`, `ExpandFontSheet`, `ParseStringS`), `OSFatal` (called by DVD `ShowMessage` with two colour pointers and a message) and `OSLoadFont` / `OSGetFontTexel` sit in the OSFont / OSFatal blocks; the PAD, AR, ARQ, EXI, SI and CARD-internal names are the Dolphin signature plus the trailer's normalised match, marked INFERRED (not independently traced; most are pruned from the build). `powf` (0x803B2F50) is an MSL float wrapper and belongs to no SDK service.

## Open problems

* The names of AX / MIX / AXFX statics (`__AX*`, `__MIX*`, `__ReverbSTD*`) cannot be proven against Nintendo's symbol table here; no copy of the library or its map is in the repository. A map file, or another game built with the same library, would turn the INFERRED rows into PROVEN.
* UNKNOWN, non-frontier: `0x803745B4`, four `0x80380xxx` functions that are not in the graph, `0x80381308`, `0x803814EC` (744 B, probably a PAD `MakeStatus` sibling), `0x8038CDDC..0x8038DBF0` (CARD, pruned), `0x80392548` (an 8-byte `return 0` stub in the card library), `0x80392CCC` (live, card; callbacks), and the descriptive AX helpers. None is frontier; the live ones are interior to the card and AX libraries and fall with their service.
* The `AXSetMode` / `MIXSetSoundMode` argument meanings (stereo / surround / DPL2) come from the game's call sites (0, 2 and 3) and the table switch in `__MIXResetPan`; the enum names are not proven.
* Not done, by design: no runtime, extract-list or `SHADOW_GC_REMOVAL_RATCHET` change; no gameplay, audio or GPU run (nothing executable changed, so nothing could be validated that way).

## How to apply the proposal

```
# from the dolrecon project directory (copy first; this tree is shared)
git apply --unidiff-zero -p1 <worktree>/test_data/platform/unidentified_names.symbols.patch    # config/symbols.txt
git apply -p1 <worktree>/test_data/platform/unidentified_names.platform_deps.patch              # tools/dolrecon/platform_deps.py
# regenerate the manifest, then re-run the planner:
python -B tools/platform/removal_plan.py --release-dir <a release tree> --dolrecon-dir <dolrecon dir> --reuse-graph
```

The re-plan above predicts the tables the planner should then print; `--check-what-if` must still pass.

## 2026-11-09: the last sdk:unidentified frontier row (0x80393634) is a GX default region callback

Plan read: `wt-gc-removal/.build/platform/removal_plan/removal_plan.json` (generated 2026-11-09 05:02 by the planner on gc-removal 3d85e87; not re-run here). It lists 46 platform bodies and 22 frontier rows; `sdk:unidentified` is 1 body and 1 frontier row, `0x80393634`, entered only by runtime literals in `runtime/src/gekko/native_gx_sync.cpp` (the `{0x4C8, 0x80393634}, {0x4CC, 0x80393730}` rows of the `__GXData` image). The other frontier rows are named services (audio.ax 9, graphics.rw-gamecube-driver 10, os.core 1, graphics.vi 1), listed in that file; none is unidentified.

Result (rows in `test_data/platform/unidentified_names.csv`, all read from the instructions of `.build/dolrecon/pal-v30/out/asm/auto_03_80362B40_text.s`):

* `0x80393634` is **not card code**. It is `(const GXTexObj*, GXTexMapID) -> GXTexRegion*`: format = obj+0x14 (`0x8039822C`), mip flag = obj+0x1F bit 0 (`0x80398234`), id mod 8, region = `__GXData` + 0x208 / 0x288 / 0x308 + id*16 (RGBA8 6: mip 0x308 else 0x288; CI formats 8..10: 0x208; all others: mip 0x288 else 0x208). Proposed name `__GXDefaultTexRegionCallback`. Its live-graph row is 288 B because it spans the next function too.
* `0x80393730` `(u32) -> GXTlutRegion*`: NULL for >= 20, else `__GXData` + 0x388 + n*16. Proposed `__GXDefaultTlutRegionCallback`.
* Slot setters `0x80398800` / `0x80398814` swap the words at `__GXData`+0x4C8 / +0x4CC and return the old one; GXInit's tail passes the two defaults to them (`0x803944C4..0x803944D8`). Proposed `GXSetTexRegionCallback` / `GXSetTlutRegionCallback`.
* Getters `0x8039822C` / `0x80398234`: field reads whose offsets match what the object initializer `0x80397E34` writes (`stw` at +0x14, `ori 1` on byte +0x1F for a non-zero mipmap argument). Proposed `GXGetTexObjFmt` / `GXGetTexObjMipMap`.

What this does and does not establish: the behaviours above are read from the instructions and are what the CSV rows' PROVEN grade rests on (the grade's definition is the one above: behaviour decisive for the role, independent of any signature). The *names* are the Dolphin SDK's conventional names for those roles; no symbol table of this build exists here to confirm them. The Dolphin signature table (`extract/functions-dolphin-sigs.csv`) is wrong in two places and was not used as evidence: it calls the 8-byte `lwz r3,0x14(r3)` body `CBGetBytesAvailableForRead` (a size-8 collision with a TRK function) and gives `0x80398814` the name `GXSetTexRegionCallback` twice. Not done: no contract test against the interpreted original (the behaviour is a table lookup; the existing `gx_state_contract_tests` already run the original GXLoadTexObj / GXLoadTlut through these callbacks); no symbols.txt change, no runtime change; no gameplay or GPU run.
