# ARAM: every access path (gate 1, 2026-10-29)

The GameCube's 16 MiB auxiliary RAM, as this runtime still carries it, and every path into it - the inventory the
ARAM retirement starts from (the user's plan: subsystem by subsystem, each replaced natively and validated against
recorded original behaviour before its legacy path is deleted). Static call sites from `tools/doldis.py --xref`;
runtime traffic from the census `SHADOW_ARAM_CENSUS=1` (NativeAudio::aram_dma: each transfer's guest caller chain past
the AR/ARQ SDK, its direction and ARAM range; the report prints at exit).

## The runtime today

- `SampleMemory` (runtime/include/gekko/sample_memory.h): the 16 MiB store, demand-zero, outside the game's address
  window. Owned by `System`; savestates carry it (savestate.cpp 281-306, 528); the boot contract digests it
  (native_pc_boot_contract.h:189).
- Native owners (native_audio.cpp `install`): ARInit 0x80382960 (reports 16 MiB internal, stack from 0x4000),
  ARStartDMA 0x80382794 (`NativeAudio::aram_dma`: a synchronous copy between guest memory and the store, then the
  ARAM interrupt), ARGetDMAStatus 0x80382758 (never busy), __ARClearInterrupt 0x80382A9C, __ARGetInterruptStatus
  0x80382ABC; the interrupt runs the guest's registered callback (`aram_handler`).
- Still guest code: ARAlloc 0x80382884, ARFree 0x803828EC, ARRegisterDMACallback 0x80382714 (only from ARQInit),
  the ARQ queue (ARQInit 0x80384490, ARQPostRequest 0x80384500, __ARQServiceQueueLo 0x803842C0, the DMA callback
  0x803843C0). ARInit/ARQInit are called once, from 0x801F2E58.
- Readers of the store besides the DMA: the AX voice decode (system.cpp 3729-4037: ADPCM headers, PCM16, PCM8 read
  straight from the store by the parameter blocks' ARAM addresses); the far-object enemy stand-ins (far_objects.cpp
  `enemy_for`, read-only, through `LiftedRender::set_aux_memory`); the savestate image and boot digest. Dead in the
  native runtime: __ARChecksize 0x80382A24 / 0x80382A38 (ARInit is native).

## The ARAM map (allocation order: ARAlloc is a stack from 0x4000)

| range | owner | size | evidence |
|---|---|---|---|
| 0x000000-0x004000 | SDK reserve | 16 KiB | ARInit's first allocation |
| 0x004000-0x804000 | GCAX sound banks | 8 MiB, `ARAlloc(0x800000)` 0x8043F7A4 (in 0x8043F744), freed 0x8043F730 | census: 0x4000-0x517220 used (Westopolis) |
| 0x804000-0x844000 | stream ring (CRI ADX: music, voices, movie audio) | 256 KiB, `ARAlloc(0x40000)` 0x803D06E0, freed 0x803D0658 | census: 0x804000-0x814000 |
| 0x844000-0xE44000 | motion clips | 6 MiB, `ARAlloc(0x600000)` 0x804225CC, freed 0x8042259C | census: 0x844000-0xAA8940 |
| 0xE44000-0xF64F00 (inferred: next on the stack) | disc-error screen backup | 0x120F00 (progressive / DTV) or 0x20120, `ARAlloc` 0x8032E23C | allocated lazily by the per-frame drive check 0x8032D444; no traffic |

## Every path

| # | client | functions | direction | runtime traffic (Westopolis tape / Glyphic Canyon tape / story + movie) |
|---|---|---|---|---|
| 1 | GCAX silence block | 0x8043BBBC -> ARQPostRequest 0x8043BC70, from 0x8043F818 (bank setup) | to ARAM | 1 x 0x500 at 0x4000 (each run) |
| 2 | GCAX wave banks | gcaxAramDma 0x8043BACC -> ARQ 0x8043BB84, from 0x8043C140 <- 0x804408AC (docs/GCAX_SOUND_MAP.md) | to ARAM | 8 uploads, 4.55 / 4.37 / 0.84 MiB, at stage start |
| 3 | AX voices (the DSP stand-in) | system.cpp voice decode | read | every audio frame, GCAX and stream voices |
| 4 | stream ring | 0x803CEFB8 -> ARQ 0x803CF0D0, 0x803CF174 -> ARQ 0x803CF21C, from 0x803CEEE8 <- 0x803C5974 <- 0x803C2460 | to ARAM | 4,846 x (18.52 MiB) / 2,758 (10.70) / 2,772 (10.80), all play long |
| 5 | motion upload | 0x804222C0 -> ARQ 0x80422314, from 0x804148A8 / 0x804146E4 / 0x804146F8 <- 0x80414278 <- 0x80413C10 / 0x80413D88 | to ARAM | 12 + 44 transfers, 2.39 / 2.25 / 1.29 MiB, at stage start |
| 6 | motion fetch (first use of a clip, and again after it was dropped) | 0x80422234 -> ARQ 0x80422284 (the caller waits for it), from 0x804144FC / 0x8041451C / 0x80414544 (one each for a clip's name, data and events) <- 0x8041ADD8 <- 0x8041A3A4; 0x80413668 <- 0x800AA314 / 0x800AA324 / 0x800AA3D0 / 0x8041BC44 / 0x8041BCB8; 0x8041388C <- 0x804137EC | from ARAM | about 3,000 transfers, 8.1 MiB over the Westopolis tape (the same clips again and again); 425 / 1.36 MiB in Glyphic Canyon |
| 7 | disc-error backup | 0x8032D4CC -> ARQ 0x8032D690, 0x8032D7CC -> ARQ 0x8032D844 (from the drive check 0x8032D444 <- 0x800511F0) | both | none: the native file API has no drive errors |
| 8 | MetroTRK debugger | TRKTargetAccessARAM 0x8039DD94 -> 0x8039FA0C / 0x8039FBF8 -> ARStartDMA 0x8039FB24.. / ARGetDMAStatus | both | none: debugger only |
| 9 | far-object enemy stand-ins (runtime) | far_objects.cpp `enemy_for` | read | at each enemy kind's first resolve |
| 10 | savestate / boot digest (runtime) | savestate.cpp, native_pc_boot_contract.h | read, write | per save / load |

No other ARStartDMA, ARAlloc, ARFree or ARQPostRequest call site exists in the DOL (the xref lists: ARStartDMA 12 -
8 inside ARQ, 4 in the TRK pair; ARAlloc 4; ARFree 3; ARQPostRequest 8; ARGetDMAStatus 2), and no data word points at
an AR/ARQ function. `aramGetFirstUserAddress` (0x802CDAE0) in the Dolphin signature list is a mismatch: it returns a
game object's +8.

## Observed weight

- Westopolis tape: 7,982 ARAM copies, 33.7 MiB; Glyphic Canyon: 3,253, 18.7 MiB; story + intro movie: 2,775, 12.9 MiB.
- Motion: of 6 MiB reserved, 2.4 MiB used; every first play of a clip copies its name, data and events back into the
  game's heap through the ARQ (the game thread waits), and a dropped clip is copied again on its next play.
- Stream ring: the largest traffic by bytes, continuous while music or voice plays.

## Retirement order (each its own gate)

1. Motion clips (paths 5, 6, 9): the clips stay in a native store; the clip fetch (0x80414370 and its three ARQ
   copies) is served natively without a copy or a wait; the 6 MiB ARAlloc goes. Validate: every clip's bytes equal
   to the ARAM copy, bone poses identical over the tapes (`tools/motion/motion_pose_check.cpp`), the game state
   identical (guest memory hashes) and no game-thread wait.

   **Status 2026-10-29: done (design C), not committed.** `runtime/src/gekko/native_motion_store.cpp` owns pool
   create 0x804225B0, pool destroy 0x80422550, upload 0x804222C0 and fetch 0x80422234. The pool's addresses are a
   native arena (0x90000000, 64 MiB reserved, committed as used: 2.41 MiB Westopolis, 2.27 MiB Glyphic Canyon); the
   6 MiB ARAlloc, every motion ARQ request and the `[r13+0x5FA0]` busy-wait are gone. Upload and fetch are plain
   copies. Where the original's wait loop would take a due interrupt, the owner leaves to the dispatcher (measured 0
   times on both tapes). The builder, acquire, release, the pool allocator and the per-clip heap nodes stay the game's
   code, so the heap's allocations equal the original's.
   Deviation from the plan above: the per-clip copy into a heap node stays. Serving clips in place (resident banks)
   changed a Black Arms soldier's animation for 271 frames on the Westopolis tape although the clip bytes were
   identical. The animation depends on the per-clip nodes' lifetime, most likely `[r13+0x5F70]` (events pointer set
   by 0x80412E3C) outliving its node. That needs reversing before the copy can go.
   Evidence (legacy and design C built from the same tree, only the owners toggled):
   - upload hash sequence identical (`[motion-span]`);
   - skin hash identical every frame on both tapes (3412 frames in Glyphic Canyon);
   - native screenshots identical (12 frames per tape, frames 500-3250, default and 1x draw distance);
   - guest memory identical at ticks 150M/300M/340M/600M except the 959 relocated pool addresses and the ARAM
     stack/ARQ leftovers.
   Earlier screenshot differences came from goldens built from different trees (other sessions' math changes
   landed in between), not from this change.
   Performance: `guest_run_wall_seconds` and peak private memory are unchanged within run-to-run noise
   (Westopolis 11.39-11.55 s both; Glyphic Canyon noisy, 5.45-7.36 s both). The ARAM copy was already a
   synchronous host copy, so removing the wait changes no measurable wall time. The 6 MiB of ARAM still exists
   inside `SampleMemory` until step 5.
2. GCAX banks (paths 1, 2, 3 for GCAX voices): the banks native at their real size (GCAX_SOUND_MAP.md "sound
   phase 4"); the voice decode reads the native bank store.

   **Status 2026-10-29: done, not committed.** `runtime/src/gekko/native_sound_banks.cpp`.
   - The GCAX init 0x8043F744 stays the game's code with two lowered call sites (extract/lift-sites.txt):
     - its ARAlloc (0x8043F7A4) reserves a native arena (guest window 0x94000000, sized to the game's own request,
       a power of two) and returns the sound address 0x01000000, the first address past the 16 MiB of ARAM;
     - its zero-block upload (0x8043F818 -> 0x8043BBBC, no other caller) is a plain copy.
   - gcaxAramDma 0x8043BACC is a leaf owner: a plain copy. It keeps the original's request bookkeeping (the first
     free busy byte, the callback and argument words) and returns the request index.
   - Its only caller (gcaxLoadMPB, via the single gcaxSetLoad call 0x801F2D50 with callback -1) never sets a
     callback, so a set one stops the run.
   - The slot allocator, the hard-coded slot table, the program data and the note-on address arithmetic stay the
     game's. Pages commit as bank bytes land: 5.08 MiB Westopolis, 4.91 MiB Glyphic Canyon. The original reserved
     8 MiB of ARAM for this.
   - The AX voice decode (system.cpp) picks each voice's source by where its range starts. Past 16 MiB, it is the
     bank arena through its mask, and a bank voice reading past the committed banks stops the run. Below 16 MiB, it
     is ARAM (the stream ring), whose addresses keep wrapping at the top as the hardware's do; ax_pb_tests pins
     that contract.
   - The lowered sites have their own list, extract/lift-sites-native.txt (lift-sites.txt is L1 input only;
     lift_l1_evidence checks it). lift_sites_reference_tests counts 769 sites.
   - Rejected first design: a native owner of the whole init that called the game's helpers. A nested guest call
     cannot resume a native body when the callee leaves to the dispatcher (an event delivered mid-call; frequent
     since the 1/300 s pacing tick). Lowering the two call sites keeps every guest call in translated code.
   - Consequence: GCAX takes no ARAM block, so the stream ring's ARAlloc now returns 0x4000 (was 0x804000). The
     CRI code builds its addresses from that result.
   - Evidence: Westopolis and Glyphic Canyon tapes, against a baseline built from the same tree with only the two
     sites and the owner off (scratchpad goldens banks3off / banks4).
     - Audio output (`--audio-dump`, S16 stereo 32 kHz): byte-identical, 89.4 s and 59.4 s.
     - Skin hashes: identical every frame (5251 / 3412 frames).
     - Screenshots: 24 of 24 identical (frames 500-3250).
     - ARAM census: only the stream ring remains (Westopolis 4,846 + 27 transfers, Glyphic Canyon 2,758 + 9; the
       original's counts), now at ARAM 0x4000. No GCAX client.
     - Interrupt points: 0.
     - Test suite: ax_pb_tests, lift_l1_evidence, lift_sites_reference_tests and native_file_api_tests pass.
       native_liveness_roots fails only on other sessions' missing rows.
   - Dormant, left for step 4: the AudioGCN destructor 0x801F2DA8 -> 0x8043FC4C -> 0x8043F674 ends in ARFree
     (0x8043F730). It never runs (the sound object lives for the program; 0 hits on the tape). If it ran it would
     pop the stream ring's block, so step 4 must own it.
3. Stream ring (paths 4, 3 for stream voices): CRI's ring native; the voice reads it directly.

   **Status 2026-10-29: done, not committed.** `runtime/src/gekko/native_stream_ring.cpp`, on a shared
   `SoundArena` (`gekko/sound_arena.h`, which the GCAX banks now use too).
   - **What the game does.** CRI's ring setup fn_803D069C takes 256 KiB of ARAM (ARAlloc 0x40000 at 0x803D06E0;
     base kept at 0x805C4D24). Its finalize fn_803D0530 frees it (ARFree at 0x803D0658, then checks the size) and
     never runs in play. The stream server (in 0x803CEEE8) decodes into main memory and moves each chunk into the
     ring with ARQPostRequest: 0x803CF0D0 is the steady feed, 0x803CF21C the 0x2000-byte start-up fill. It then
     spins on the chunk's busy word. The request's completion callback (0x803CF32C / 0x803CF260) clears that word.
     ARQ's service routine 0x803843C4 runs the callback at the DSP DMA interrupt, with r3 = the request; the
     callback reads the request's owner word (+4).
   - **Native.** Four lowered sites in extract/lift-sites-native.txt:
     - The ARAlloc reserves a native arena (guest window 0x95000000) and returns the sound address 0x02000000,
       past the banks' span. The whole 256 KiB is committed at once: voices loop over the ring before CRI has
       written all of it, and the unwritten bytes must read as zero, as ARAM's did.
     - The ARFree unmaps it and writes the requested size back.
     - Each ARQPostRequest fills the request record as 0x80384500 does ({+0 0, +4 owner, +8 type, +0x10 source,
       +0x14 destination, +0x18 length, +0x1C callback or 0x803843C0}), copies the chunk into the ring, and calls
       NativeAudio::complete_transfer.
   - **Completion.** complete_transfer queues {callback, request} and raises the DSP DMA interrupt (OS cause
     DSP_ARAM). Its handler runs each queued callback in interrupt context at the same point the original's ran:
     the spin loop's first back edge. The ARQ queue is never touched.
     - Rejected first: calling the callback from the lowering. A `call` site's continuation overwrites pc with
       site + 4 before its exit check, and the dispatcher's lift-site path never runs published transfers. Either
       way an interrupted callback would be lost.
     - Savestates are unchanged: a completion cannot be pending at a save point, and save_state reports one if it
       ever is.
   - **Voice routing.** The AX decode picks each voice's source by where its range starts: the banks' span, the
     ring's span, or ARAM.
   - **Evidence.** Westopolis and Glyphic Canyon tapes, against a baseline from the same tree with only the four
     ring sites off (goldens ring0 / ring2).
     - Audio output: byte-identical, 89.4 s and 59.4 s.
     - Screenshots: 24 of 24 identical.
     - Skin hashes: identical every frame (5251 / 3412).
     - Transfers: 4,873 / 2,767, each with its completion at the DSP DMA interrupt (the original's 4,846 + 27 and
       2,758 + 9).
     - ARAM census: **0 clients**. No ARAM transfer is left in play.
     - Tests: lift_sites_reference_tests (773 sites), lift_l1_evidence, native_file_api_tests, gekko_core_tests
       and ax_pb_tests pass. native_liveness_roots fails only on other sessions' missing rows.
   - **Left for later steps.** The disc-error screen backup (path 7) does call ARAlloc(0x20120) during play
     (0x8032E240 from the drive check), though it never transfers: an allocation dependency for step 4. Step 5
     deletes SampleMemory, the AR/ARQ owners and the ARAM side of NativeAudio's handler (aram_callback_due_).
4. Dormant paths 7 and 8: native owners that keep their contract without ARAM.

   **Status 2026-10-29: done, not committed.**
   - **Path 7 (disc-error screen backup)** was not dormant: the per-frame drive check 0x8032D444 calls
     fn_8032E200 once, which ARAllocs the backup (0x20120 bytes, 0x120F00 progressive) at 0x8032E23C in play. Its
     save and restore (ARQ at 0x8032D844 type 0 / 0x8032D690 type 1, completion 0x8032DB64) need a drive error, plus
     a failed heap allocation for the error screen's frame buffer. A native store for it was built and unit-tested,
     then withdrawn: at the user's request the top session removed the whole disc-error screen (docs/DISC_SYSTEM_MAP.md
     "Cut 1": the drive check at 0x800511F0, the screen, and the backup allocation). With it the generator prunes
     ARAlloc 0x80382884 and ARFree 0x803828EC.
   - **Path 8 (MetroTRK)** is already gone. TRKTargetAccessARAM 0x8039DD94 and its helpers 0x8039FA0C / 0x8039FBF8
     are pruned: no root reaches the debugger, so the binary has no such code.
   - **The GCAX shutdown's ARFree** (0x8043F730 in 0x8043F674, reached only from the AudioGCN destructor; 0 hits in
     play) is a lowered site, `scaffold::sound_banks_release`. It releases the banks' arena and writes the size back,
     as ARFree does.
     - native_sound_store_tests pins the banks' and the ring's releases: the size reported, the arena unmapped, and
       the ring reservable again.
5. Delete `SampleMemory`, the AR/ARQ owners and the ARAM interrupt; savestates without the image.

   **Status 2026-10-29: done and validated, not committed.**
   - Evidence:
     - **Pruned:** the generated code holds no AR/ARQ function. ARInit, ARQInit, ARRegisterDMACallback, ARQ's ISR
       0x803843C4 and service routine 0x803842C0, ARQPostRequest, ARStartDMA, ARAlloc and ARFree all have 0
       definitions. Two roots had kept part of it alive:
       - the stream ring's literal for ARQ's default callback 0x803843C0 (a bare blr whose extent runs into the
         ISR). Removed: a null completion now stops, since both stream-server transfers always name theirs;
       - the census's augmented root 0x803843C4, recorded with layout evidence only. It gained its constructor,
         ARQInit's lis at 0x803844C0, so it is conditional now.
     - **Westopolis tape** against a baseline of today's tree without step 5 (goldens s5base / s5on): audio
       byte-identical (89.4 s), 6 of 6 screenshots identical, skin hashes identical (5,251 frames), 4,873
       completions on the DSP DMA interrupt.
     - **Old savestate:** ssl_340000000.gekkostate (version 23, saved by the original-path build, so the GCAX
       banks, the ring and the motion pool are all in ARAM). Loaded in the original-path exe and in the no-ARAM
       exe, then run 30 s: audio byte-identical, skin hashes identical (1,800 frames).
     - **Version 24 round trip:** that state loaded, a v24 state saved at tick 600,000,000 and loaded in a new
       process. Its 23.58 s of audio are byte-identical to the uninterrupted run's same span; skin hashes
       identical.
     - **Tests:** gekko_core_tests (new: native DSP DMA completion), ax_pb_tests (samples from an adopted store;
       pcm16_store_top), native_sound_store_tests, lift_sites_reference_tests (1046), lift_l1_evidence,
       lift_purity and se_manager_contract_tests pass. native_liveness_roots fails only on other sessions'
       missing rows.
     - audio_state.cpp's history loader accepts version 24 (its block is version 23's).
   - **Bring-up.** The game constructor 0x801F2E58 calls ARInit (0x801F2EA0) and ARQInit (0x801F2EA4) once each.
     Both calls are lowered sites. ARInit's only effect that outlives ARAM is unmasking the DSP DMA interrupt (OS
     cause DSP_ARAM), which the native stores' completions use; that is kept, as `scaffold::dsp_dma_interrupt_init`.
     The AR allocator's bookkeeping (r13+0x5CA0..0x5CBC) and the size word 0x800000D0 are not written: a scan of
     every load and store in the DOL's text finds their only readers inside the AR library, which nothing reaches
     now. ARQInit is `nothing`.
   - **Pruned with it.** The AR/ARQ library: ARInit, ARQInit, ARRegisterDMACallback, ARQ's service routine and
     queue, ARStartDMA. Their native owners (ARInit, ARStartDMA, ARGetDMAStatus, __ARClearInterrupt,
     __ARGetInterruptStatus) and their liveness rows are deleted. ARAlloc and ARFree went with the disc-error cut;
     MetroTRK was already pruned.
   - **Deleted.** `SampleMemory` (runtime/{include,src}/gekko/sample_memory.*), NativeAudio's ARAM DMA, the ARAM
     census and the AR-callback half of the DSP DMA handler; the far-object reader's ARAM image (`set_aux_memory`).
   - **Renamed.** The interrupt source is `NativeInterrupts::Source::DspDma` (OS cause DSP_ARAM). The AX decode
     reads a voice only from a native sound store; a voice addressing none stops the run.
   - **Savestates, version 24.** No ARAM image. The banks' and the ring's placement (SoundArena::save_state)
     follows the audio block. The stores' bytes are regions of the guest window, which its own record carries.
     This also fixes a gap in steps 2-3: a state saved by those builds kept the regions but not their placement.

   **Old savestates (versions 19-23).** These still load, with their sound (the user's requirement). Their game
   state names ARAM addresses wherever it kept sound data. Rewriting those addresses (GCAX slot bases, CRI's
   per-channel ring addresses, motion pool nodes and bank records, every voice's parameter block) is not a reliable
   conversion, so the stores take the addresses over instead. adopt_aram_image (savestate.cpp) reads each block
   from the game's own records and hands its bytes from the image to its store:
   - GCAX: the context [r13+0x5FAC], block at +0x52B0 of +0x52A4 bytes.
   - CRI's ring: the record 0x805C4D10, block at +0x10 of +0xC bytes.
   - Motion clips: the pool [r13+0x5F98], 6 MiB.
   An adopted store is the smallest aligned power-of-two window around its block (SoundArena::adopt), so the AX
   decode's mask indexing is unchanged. The motion store serves addresses below 16 MiB from its arena at the same
   offset (NativeMotionStore::pool_address). A store whose block already sits at its native base (a v23 state from
   a step 2-3 build) resumes the region the state restored. The image's other bytes (the SDK reserve, space nothing
   allocated) have no reader and are dropped. ax_pb_tests uses the same adoption: its voices play from a store
   adopted over sound addresses 0..16 MiB.

Separately (not ARAM): `setid.bin` preloading (every class archive a stage may use stays resident all stage) -
reverse its consumers before changing residency.
