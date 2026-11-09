# CRI middleware server chain (2026-11-02, read-only survey; to be re-verified per piece before reconstruction)

User directive 2026-11-02: reconstruct CRI's server ALGORITHMS as native C++20 (a native wrapper calling translated
code does not count). This file is the map; each piece is reconstructed with an oracle test against the original.
Addresses: PAL v30 dtk disassembly (auto_03_803A3920_text.s unless noted). Call-tree sizes are lower bounds (direct
calls only; function-pointer calls - lock hooks, user hooks, the ADXGC rna vtables - are missed).

## 1. SVM: the server table and dispatch (block S = 0x805C0E20; NOT CRI's thread block 0x805ACA10)

- S+0x00 init refcount; S+0x04 lock depth; S+0x08 lock "first" flag; S+0x3C optional test-and-set override; S+0x40
  0x80-byte message buffer; S+0xC0 error callback {fn, arg}; S+0xC8 unlock hook {fn, arg}; S+0xD0 lock hook {fn, arg}.
- S+0x118: 8 server types x 0x48 bytes, each 6 slots x 12 bytes {fn, arg, name*}; S+0x358 per-type "inside server"
  flag (1 around each call); S+0x378 per-type pass counter (incremented on exit).
- Servers (same shape): for slot 0..5, skip fn == 0, set the flag, call fn(arg), OR the results; return the OR.
  Vsync svtype 2 0x803CC3FC (table +0x90), svtype 4 0x803CC36C (+0x120), Main svtype 5 0x803CC2DC (+0x168), Idle
  svtype 6 0x803CC24C (+0x1B0). The servers take no lock; only table edits do.
- Registration: SetCbSvr(type, fn, arg, name) 0x803CC970 (first free slot, or -1 "too many server function"),
  SetCbSvrId(type, id, fn, arg, name) 0x803CC5F4, ClearCbSvrId(type, id) 0x803CC7D0 - each under the lock.
- Lock: enter helper 0x803CCCEC (also open-coded in 0x803CC010, 0x803CC5F4), leave 0x803CCC5C; they call the S+0xD0 /
  S+0xC8 hooks = ADXGC's fn_803C565C / fn_803C55F0 (installed by ADXGC init 0x803C5118 via 0x803CC49C / 0x803CC48C).
  These are the 92k lock calls per 90 s (table-edit and try-lock traffic). 0x803CBFB8: test-and-set (1 if the word
  was 0) - MWSFSVR's Idle / Vsync interlock (word at fn_80407F00()+0x58). 0x803BD240 / 0x803BD244 empty hooks;
  0x803BD268 / 0x803BD248 lock enter / leave.
- Return values: all callbacks return 0 except Sofdec's Idle callback MWSFSVR_IdleThrdProc 0x80408C44 -> 0x80408B30:
  nonzero while the decode status call 0x803FC394 != 1 (work remains); 0 when the init flag 0x805DE93C != 1 or the
  test-and-set 0x80404204 fails.

### 1a. Native SVM (DONE 2026-11-02, reconstruction step 1)

runtime/include/gekko/native_svm.h, runtime/src/gekko/native_svm.cpp: all eighteen functions of the module (its own
build string: "SVM/GC Ver.1.58 Build:Feb 28 2005") are native owners (extract/native-owners.txt); their bodies are pruned
(thunks only). The block S is native state (savestate v27; an older state imports S from guest memory). Facts the
reconstruction rests on (byte scan + full read of 0x803CBFB8..0x803CCD50): only these eighteen functions address S; S
is .bss; S+0xC0 (error callback) and S+0x3C (test-and-set override) have no writer anywhere in the image, so error
reports have no observable effect (logged in non-shipping builds) and the override path is dead; S+0x2C / S+0x34 are
only cleared; lock depth / type change only while a lock hook is installed; Init / Finish clear flags, hooks and pass
counters 0..5 but never the table or the user hooks; no caller reads r3 after the void entry points (scan of every
call site). Guest calls (server callbacks, lock / unlock hooks, user hooks) go through the continuation entry
0x803CC0D0 with the owner's progress in a 0x40-byte frame on the guest stack (reentrant: a callback may re-enter).
Oracle: CTest native_svm_tests - 48 scripted calls on the original words vs NativeSvm in the reference interpreter,
75 traced callback / hook events, results and the whole state equal after every call; addresses() = the owners list.

Milestone proof (user's seven items, 2026-11-02):
1. Original functions replaced: 18 - test-and-set 0x803CBFB8, SVM_Finish 0x803CC0CC, SVM_Init 0x803CC194, servers
   0x803CC24C / 0x803CC2DC / 0x803CC36C / 0x803CC3FC, SetCbUnlock 0x803CC48C, SetCbLock 0x803CC49C, user hook call
   0x803CC4AC / set 0x803CC4F0, SVM_SetCbSvrId 0x803CC5F4, SVM_DelCbSvr 0x803CC7D0, SVM_SetCbSvr 0x803CC970, error
   0x803CCB2C / errorf 0x803CCB84, svm_unlock 0x803CCC5C, svm_lock 0x803CCCEC.
2. Guest bodies removed (generator, same inputs with / without the SVM owners): native owners 144 -> 162 (18 thunks),
   pruned rows 713 -> 728 (+15 SVM return-site rows), guest instructions emitted 1,918,731 -> 1,918,277 (-454),
   recompiled rows 38,643 -> 38,628; exe -29,696 bytes. No shared callee freed (strncpy / vsprintf have other users).
3. Remaining guest calls / context dependencies: the server callbacks (ADX 0x803B7424 / 0x803B7400 / 0x803B7448,
   Sofdec 0x80408CEC / 0x80408C44 / 0x80408C98), the lock hooks (ADXGC 0x803C565C / 0x803C55F0 - native counts under
   the CRI bridge), user hook 6 (adxm_goto_mwidle_border); continuation entry 0x803CC0D0; 0x40-byte frames on the
   guest stack; r1 / r3 / r12 / CTR / LR at those calls.
4. Compiled platform-SDK functions: unchanged (CRI is middleware, not the GameCube SDK; ratchets unchanged).
5. Native implementation: NativeSvm. Consumers migrated: every caller of the 18 entries - ADXT_Init / Finish, ADXGC
   init, MWSFSVR (Sofdec) init / finish, ~56 lock / unlock sites (ADX, ADXF, SJ), the test-and-set users (ADX
   0x803C09C0, Sofdec 0x80404210), the CRI thread loops and the CRI bridge's upcalls.
6. Validation: native_svm_tests (oracle); Westopolis 90 s A/B vs the pre-SVM exe, bridge off and on: PCM byte-identical
   (015445f2...), AI censuses identical, 5,360 frames, 0 underruns; savestates: a v27 save at 20 s resumed to 30 s
   equals the uninterrupted run in every memory block and in PCM; a v26 state (pre-SVM exe) imported by this exe
   resumes with identical PCM (memory differs only in the now-unused S block and CRI's thread stacks).
7. CPU: no measurable change - 3 alternating 90 s runs each, process CPU base 19.17 / 18.14 / 17.69 s, native
   18.72 / 20.20 / 16.75 s (spread > difference). No improvement is claimed.

## 2. What is registered (every call site of the three registration functions)

- ADXT_Init 0x803B72D8 (refcount 0x805A6910): svtype 2 id 1 -> 0x803B7424 -> **0x803C23D0 (ADX vsync)**; svtype 4 ->
  0x803B7400 -> **0x803BE1E0 (ADX stream / file server, handle table 0x805A90E8)**; svtype 5 -> 0x803B7448 ->
  0x803BE430 -> **0x803C9278 -> 0x803C9AC4 (ADXLSC stream controller, inferred)**; error handlers 0x803B746C /
  0x803B7490; teardown 0x803B7240.
- MWSFSVR_Init 0x80407BE0: svtype 2 id 0 -> **0x80408CEC (Sofdec vsync: test-and-set, then 0x803FC468 ->
  0x803FE6B0)**; svtype 5 -> 0x80408C98 (only if ctx+0x10 == 1); svtype 6 -> **0x80408C44 (Sofdec idle: 8 handles,
  stride 0x2A8, per-handle decode 0x80408B10)** (default).
- Nothing else (no AHX; file and stream work reaches the servers through ADXT's svtype 4 / 5 callbacks).

## 3. Weight and what is already native

| callback | funcs / ~insns | role |
|---|---|---|
| 0x803C23D0 ADX vsync | 245 / 16.7k | re-entry guard 0x805AC5E4; ADX-wide vint 0x803B77B4 -> 0x803B7810 (110 / 11.5k); per playing handle (16 at 0x805A6928, stride 0xC4) adxt_ExecHndl 0x803C3F4C (162 / 8.6k, ADX decode); ADXGC "rna" output 0x803C5964 -> 0x803CEEB4 -> 0x803CEF10 (9 / 569: decoded PCM out, DCFlushRange, ring posts 0x803CF0D0 / 0x803CF21C); voice updates via the rna layer 0x803CF650 (AXSetVoiceState / Addr), voice open 0x803CFAB0, destroy 0x803CF9E0 |
| 0x803BE1E0 svtype 4 | 57 / 5.8k | ADX stream / file server (ADXSTM / ADXF exec, inferred) |
| 0x803BE430 svtype 5 | 53 / 4.6k | ADXLSC stream controller (inferred), from the game frame |
| 0x80408B30 Sofdec idle | 134 / 8.1k | 8 handles, per-handle SFD / MPEG decode 0x80408B10 (130 / 7.9k); user hooks ctx+0x40 / 0x48 / 0x50 |
| 0x80408CEC Sofdec vsync | 38 / 4.2k | Sofdec vint 0x803FE6B0 |

Already native: the CRI file device gcCi (native_cri_file_device.cpp: 0x803CDADC..0x803CE924, GetInterface 0x803CEA9C,
interface table 0x80569FC8); the stream ring (native_stream_ring.cpp: ARAlloc 0x803D06E0, ARFree 0x803D0658, posts
0x803CF0D0 / 0x803CF21C, completion at the DSP DMA interrupt); the AX mixer / PB decode (voices read the ring).
Not native: SVM, ADX decode and vint, ADXLSC / stream servers, all of Sofdec.

## 4. Reconstruction order (each with its oracle)

1. SVM table + dispatch + registration + lock + test-and-set (~400 insns). Oracle: the original vs native over a
   scripted register / call / dispatch sequence in the reference interpreter; compare S+0x118..0x3A0 and returns.
2. ADXGC rna output (0x803C5964, 0x803CEEB4, 0x803CEF10; ~600 insns). Oracle: one vsync tick from a savestate; diff the
   posted ring bytes and the AX PB mirror.
3. ADX handle exec + vint (0x803C3F4C, 0x803B7810; ~20k insns). Oracle: recorded ADX streams; PCM byte-identical
   (the --audio-dump gate).
4. Stream servers (0x803BE1E0, 0x803C9AC4; ~10k). Oracle: the gcCi request log compared across both.
5. Sofdec (0x80408B10, 0x803FE6B0; ~12k+). Oracle: movie frame hashes per tick and the Idle return sequence.

## 4a. Step 2 discovery (2026-11-02): the rna output depends on CRI's SJ stream joints

0x803C5964 only calls 0x803CEEB4: for 16 handles at 0x805C3E4C (stride 0xEC) with byte +0 == 1, 0x803CEF10(handle).
Per handle (flags at +1: bit 1 -> 0x803CF408 first; bit 0 -> the play path, else bit 1 -> the silence path):
- play path, per channel (count = signed byte +3; per-channel arrays at +0x28/+0x30/+0x38/+0x40/+0x50/+0x60, stride
  4 / 8 / 0x20): if +0x08 and the channel's busy word +0x60 == 0: SJ(+0x38)->GetChunk(lane 0, 0x2000) and
  SJ(+0x30)->GetChunk(lane 1, that size) (vtable +0x18), both rounded down to 32 bytes (0x803CBF34 splits), the rest
  put back (vtable +0x1C); the two chunk sizes must match (else a deliberate `b .` hang at 0x803CF070); the chunk
  recorded at +0x40/+0x50, size/2 at +0x68; DCFlushRange (0x80372534); busy = 1; ARQPostRequest(+0xA8 request, owner
  +0x28, type 0, priority 1, source chunk, destination, bytes, completion 0x803CF32C) at 0x803CF0D0 - the native
  stream ring's post; then a spin at 0x803CF0D4 until the completion clears busy.
- silence path (when +0x7C < +0x20), per channel with busy +0x70 == 0: GetChunk(lane 0, 0x2000) from +0x38, rounded,
  rest put back; ARQ post from the zero buffer word at 0x805C2D7C (DCFlushRange 0x1000 of it), completion 0x803CF260,
  spin at 0x803CF220.
Consequence: a native rna output needs native SJ objects (SJRBF / SJMEM: their GetChunk / PutChunk / split) or keeps
those as guest calls; with the native stream ring the "post + spin" collapses to a synchronous copy whose completion
is known - the wait disappears instead of needing an interrupt. Next: map SJ (vtable users, object layout) as the
prerequisite of step 2.

## 4b. Native SJ stream joints (step 2a, 2026-11-03)

Discovery (the original's code, PAL DOL; names from the functions' own error codes E2004090201..52 and the order of
CRI's SJ interface - Destroy +0x0C, GetUuid +0x10, Reset +0x14, GetChunk +0x18, UngetChunk +0x1C, PutChunk +0x20,
GetNumData +0x24, IsGetChunk +0x28, EntryErrFunc +0x2C; correction to 4a: the rna output's "put back" is UngetChunk):
- SJCRS: Lock 0x803C9E30 / Unlock 0x803C9DE4 = OSDisableInterrupts / OSRestoreInterrupts with a nesting depth
  (0x805BBD78, saved level 0x805BBD7C); Init / Finish 0x803C9EAC / 0x803C9E7C = a user count (0x805BBD80). Lock /
  Unlock are also called by ADX's own wrappers (0x803C90D4 / 0x803C90F4, 0x803CEB80 / 0x803CEBA0).
- SJMEM (0x803C9EDC..0x803CAAA4, vtable 0x80569D40): 32 joints x 0x24 at 0x805BBD8C; a fixed data buffer.
- SJRBF (0x803CAACC..0x803CBC44, vtable 0x80569D70): 256 joints x 0x40 at 0x805BC214; a ring with an extra area
  after its end mirroring the head (layout in gekko/native_sj.h); vtable Get / Put / Unget are lock wrappers
  (0x803CB580 / 0x803CB320 / 0x803CB120) around bodies 0x803CB378 / 0x803CB178 / 0x803CAF08.
- SJ_SplitChunk 0x803CBF34, the error forwarder 0x803CBF98 (-> SVM error report), the CRI tag search 0x803CBD20 and a
  third pool's init / finish 0x803CBC6C / 0x803CBCC0 (0x805C021C, 0xC00; not SJ's interface; left translated).
Use (Westopolis tape, 90 s, call census .build/ab-jobs/sj-census.csv): SJRBF GetChunk 40,924, UngetChunk 36,199,
PutChunk 25,899, GetNumData 20,674, SJ_SplitChunk 32,527, GetBufPtr 4,530, 35 rings created / 19 destroyed, one
SJMEM; never called: EntryErrFunc, IsGetChunk, the transfer counters, any error path.

Reconstruction: runtime/src/gekko/native_sj.cpp owns 38 functions (extract/native-owners.txt; continuation 0x803CA9F4
for guest error callbacks, called inside the guest SJCRS lock as in the original). The joints stay in their guest
pools: handles are guest addresses whose vtable the translated users call through. SJCRS_Lock / Unlock stay
translated; native functions take no lock where no guest code runs (no guest interrupt can occur inside native code
and the original's lock / unlock pair has no net effect - except the saved level word 0x805BBD7C, dead between calls).

Oracle (CTest native_sj_tests): the original words vs NativeSj in the reference interpreter, 3,094 calls (scripted:
every function, null / destroyed handles, illegal lanes, a zero-size ring, guest error callbacks, signed remainders,
pool exhaustion, Init / Finish counts; then 3,000 seeded random operations on two rings and a memory joint). After
every call: r3, both pools and counts, SJCRS words (minus the dead saved level), every buffer and mirror byte, chunks,
callback trace - 0 differences. Negative controls (each a one-line mutation of native_sj.cpp): no mirror-to-start
copy 3,041 failures; unsigned remainder 5,711; callback outside the lock 6,023; counter update before the callback
3,012 - all killed.

Seven-item proof (Westopolis tape, 90 s; base = .build/ab-jobs/base-presj.exe, native = .build/watch 439d2b5a):
1. Functions eliminated: 38 bodies pruned to thunks (generator: native owners 162 -> 200 rows, pruned 728 -> 736
   rows / 39,117 instructions); exe 111,550,976 -> 111,464,960 bytes (-86,016).
2. Calls served natively (runs j1 / k1): gets 40,924, puts 25,899, ungets 36,199, splits 32,527, GetNumData 20,674,
   creates 36, destroys 20, 231,972 mirror bytes copied; equal to the original's census call for call.
3. Guest dependencies left: the joints' pools (guest addresses are the handles), SJCRS_Lock / Unlock (translated,
   shared with ADX), the continuation 0x803CA9F4 for guest error callbacks (none in the replay).
4. Audio: PCM sha 015445f2... in all four runs (j0 base / j1 native with the CRI bridge off, k0 / k1 with it on);
   0 underruns.
5. Gameplay / frames: 5,360 render frames in all four; end state: one 64 KB block differs between base and native
   (0x805B0000) - a word-level dump (runs p0 / p1) puts all 33 differing words in CRI's svtype4 and vsync thread stacks
   below their tops: the dead frames the original SJ functions left (their LR words point into SJ code). Pools,
   rings and SJCRS words identical.
6. Oracle: native_sj_tests (above); native_svm_tests still passes (NativeSvm gained the error_report entry).
7. CPU: 3 alternating 90 s runs each, process CPU base 19.23 / 18.69 / 18.19 s (mean 18.70), native 17.73 / 17.78 /
   17.69 s (mean 17.73): lower in every pair, ranges do not overlap, about -0.97 s (-5 %); n = 3 per side, so a
   measured result on this machine, not a general claim. PCM 015445f2... in all six (runs cb1-3 / cn1-3).
Static coverage (tools/audit/native_coverage.py, .build/audit-native-coverage/audit-sj.json): SJ owner bodies 1,949
instructions; translated 96.72 % -> 96.56 % of the original's live code; CRI layer 2.20 % -> 4.45 % native.
Next: the rna output (step 2) on native joints - the ARQ post + spin at 0x803CF0D0 / 0x803CF0D4 becomes a synchronous
native copy.

## 4c. Native ADX output to the voices (step 2, 2026-11-03)

Discovery (full description in gekko/native_adx_output.h): 0x803CEEB4 (every used handle of 16 x 0xEC at
0x805C3E4C) -> 0x803CEF10 per handle: flag 2 -> 0x803CF408 consume (the last channel's AX parameter block word
+0x1B2 less the voice's ring base = the play position; history of 32 at 0x805C2D88 / 0x805C2D8C; samples played since
the cursor +4, modulo 0x1000, rounded down to 0x800, leave each output joint: GetChunk(1) + PutChunk(0)); flag 1 ->
play: per channel GetChunk(0, <= 0x2000) on the output (+0x38) and GetChunk(1, that) on the input (+0x30), both cut to
their common length rounded down to 32 (SJ_SplitChunk + UngetChunk of the rests), nothing to move ends the call,
ARQPostRequest input -> output, spin on busy +0x60 until 0x803CF32C (input PutChunk(0), output PutChunk(1), busy 0,
at the last channel +0x6C += +0x68); else flag 2 and +0x7C < +0x20 -> silence from the zero buffer *(0x805C2D7C),
busy +0x70, completion 0x803CF260. The transfer owner word +0x28 is handle index * 2 + channel (the completions
decode it). Two `b .` hangs: a play position outside [0, +0x20] (0x803CF494) and unequal chunk lengths (0x803CF070).
Use (Westopolis 90 s, census .build/ab-jobs/rna-census.csv on the native-SJ build): 5,367 passes (one per vsync),
21,324 handle calls, 8,472 consumes, 4,846 play completions, 27 silence completions.

Reconstruction: runtime/src/gekko/native_adx_output.cpp owns the five functions. Each transfer fills the ARQ record
as before, copies the chunk into the stream ring's arena (NativeStreamRing::copy) and runs its completion at once:
the post + spin + DSP DMA interrupt round trip is gone, so the vsync server no longer waits for an interrupt (the
reason section 5's bridge needed thread-context upcalls). The joints are called through NativeSj's direct interface
(get / unget / put on native chunk values; dispatch on the vtable word; a joint that is neither SJMEM nor SJRBF, or a
guest error callback, stops the run with the reason). The two hangs stop the run with the reason. DCFlushRange is not
called (no cache here). Removed with it: the two lowered ARQPostRequest sites (extract/lift-sites-native.txt), the
stream ring's interrupt-completed post (NativeStreamRing::post / scaffold::stream_ring_post), four wait points and
two liveness roots inside the pruned bodies. Left: NativeAudio's DSP DMA completion queue now has no producer (kept:
its savestate field; remove with the next savestate version).

Oracle (CTest native_adx_output_tests): the original five functions + original SJ code vs NativeAdxOutput + NativeSj
in the reference interpreter; the original's ARQPostRequest is a test stand-in doing what the runtime's transfer did
(record, copy, completion before the spin's first read) and DCFlushRange a no-op (it ends in `sc`). Three handles
(stereo play + consume, mono consume then play, silence fill), SJRBF rings, 600 seeded passes feeding random PCM and
advancing the voices' positions: 2,988 calls, 542 transfers, 1,514 consumes - 0 differences in the handle table,
the position history, both SJ pools, every buffer byte. Both machines also agreed on a scenario error (a position
past +0x20): the original hung at 0x803CF494 on the same pass and handle where the native stopped. Negative
controls, all killed: history wrap at 33 (2,926 failures), consume rounding 0x400 (2,978), transfer rounding 64 (841),
an empty channel continuing (2,070), the total added at the first channel (2,972).

Seven-item proof (Westopolis tape, 90 s; base = .build/ab-jobs/base-prerna.exe (native SJ), native = .build/watch):
1. Eliminated: 5 bodies pruned to thunks (generator: owners 200 -> 205 rows, pruned 736 -> 742 rows / 39,406
   instructions), 2 lowered sites and the stream ring's post path removed; exe -15,872 bytes. Static coverage
   (audit-adx.json): owner bodies +436 instructions; translated 96.56 % -> 96.52 %; CRI 4.45 % -> 4.95 % native.
2. Calls served natively (r1 / q1): 5,367 passes, 21,324 handle calls, 8,472 consumes (5,470,208 samples), 4,846
   transfers (19,422,912 bytes), 27 silence transfers (86,336 bytes) - equal to the original's census call for call.
3. Guest dependencies left: the handle table, joints and voices' parameter blocks in guest memory (the ADX layer and
   AX code read them); the CRI vsync server that calls the output is still a guest thread (section 5); NativeAudio's
   DSP DMA completion queue has no producer now (its savestate field remains).
4. Audio: PCM 015445f2... in r0 / r1 (bridge off) and q0 / q1 (bridge on); 0 underruns.
5. Frames / state: 5,360 render frames in all four. End state base vs native: blocks 0x805A0000 / 0x805B0000 only. A
   word dump (v0 / v1) shows 23 words: 13 in the vsync thread's stack below its top (dead frames) and 10 in the
   vsync thread's saved OSContext (B+0x348): r0 and r4..r12 only - volatile registers of a context saved by the OS's
   own switch at a call boundary (state 1, srr0 = lr = 0x803788B4 in both; r1 / r2 / r13 / r14..r31 / cr / ctr / xer
   identical), dead on resumption. The handle table, history, joints and rings are identical.
6. Oracle: native_adx_output_tests (above); native_sj_tests, native_svm_tests, gekko_core_tests,
   native_sound_store_tests pass after the change.
7. CPU: 3 alternating 90 s runs each, process CPU base 16.94 / 17.63 / 17.84 s, native 18.09 / 15.59 / 19.17 s -
   overlapping and contradictory: no measurable change, no claim.
Next: with the output's wait gone, revisit the CRI thread bridge (section 5): the vsync server's work no longer
blocks on an interrupt, so its upcall may become a plain native call at the retrace point - then ADX exec (step 3).

## 5. The thread bridge (native_cri.cpp) result

First A/B (Westopolis 90 s): FAILED - the stage load stalled (only 1 loader thread created vs 36; Idle server 2 calls
vs ~5,300; PCM diverged at byte 217,621). Traced in two steps (2026-11-02, no bypass):
1. No preemption: CRI's priority-12 / 14 threads preempted the running thread at each retrace, putting it at the tail
   of its priority's run queue; without them the main thread kept running and the priority-16 loader never got the CPU
   at frame 87. Fix: the delivery that runs a tick ends with SelectThread(1) (request_yield_on_finish).
2. The stall at game frame ~137: the game thread spun at 0x803CF0D4 - adxgc's rna output (inside the Vsync server's
   ADX work) posts an ARQ transfer at 0x803CF0D0 (native stream ring: copied at once, completion handed to the DSP DMA
   interrupt) and waits for its callback 0x803CF32C to clear a flag. The bridge ran the servers INSIDE the retrace
   delivery (MSR[EE] clear), so the completion interrupt could never be delivered. On the console the servers were
   threads: interruptible. Fix: thread-context upcalls (NativeInterrupts::request_upcall): the server work runs after
   the delivery, interrupts enabled, its own OSContext current (SelectThread refuses to switch while it runs, as no
   game thread ran before a higher-priority server slept again), nested deliveries normal.
3. Then a gameplay divergence, traced with the guest watch + call census: EnemyBaseAI (GunSoldier, constructed at tick
   899,100,000) never initialises its state machine's lock byte (+0xC; 0x8019F528 skips the first transition while it
   is nonzero and only clears it after a transition - so a nonzero garbage byte freezes that AI for good). The byte is
   heap garbage left by an earlier thread stack in the same memory: baseline 0x00, bridge 0x80 (0x80379494, the
   return address OSWakeupThread leaves when it calls SelectThread). In the baseline itself another enemy
   (0x813255BC) reads 0x21 (17 of 30 over the 90 s tape) and skips its initial state: an ORIGINAL GAME BUG whose outcome depends on stack garbage, i.e. on thread
   scheduling. First mitigation: the upcalls run on CRI's own thread stacks (OSCreateThread's r6, block B), as the
   console ran them; the drain too. The divergence remains (the garbage comes from scheduler paths on a loader stack).

Result after 1+2 (30 s and 45 s): the stage loads; PCM byte-identical to the baseline (SHA-256 ef77796a / 077f22eb);
render frames 1,760 = 1,760; Vsync ticks 1,767, Idle calls 1,668 (1,668 passes, none preempted); memory differences =
CRI's thread blocks / stacks, run-queue / retrace-queue links to CRI's threads, the idle context's saved registers,
and heap stack garbage (the EnemyBaseAI case above). The bridge stays OFF by default until the garbage-dependent AI
decision is made (user decision: reproduce console stack garbage, or fix the uninitialised lock byte - the
developer's clear at 0x8019F5C8 shows the intended initial value 0).

### 5b. The vsync tick as plain calls (2026-11-07, docs/agents/cri-threads.md)

With the native ADX output nothing in the Vsync / svtype 4 chain waits for an interrupt (reachable code: no halt / poll
wait, no blocking OS call; `System::idle_wait` stops the run if a tick ever waits), so the tick (Vsync server, user
callback, svtype 4 server) runs as plain guest calls of the retrace delivery, on the Vsync thread's stack
(`NativeInterrupts::relocate_frame`); a tick deferred by CRI's lock runs from the HostWork delivery the same way. Only
the Idle server keeps the thread-context upcall. Gated on the Westopolis replay (PCM 015445f2..., 3.645e9 ticks, 5,360
frames, AI lock-byte census identical, contract test with two killed controls, CPU: no measurable change). The default
stays OFF pending the main session's decision.
