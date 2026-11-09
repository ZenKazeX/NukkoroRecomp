# Native output composition

The native OutputStage owns 160 signed samples of surround history and two
retained stereo matrix blocks with a return phase. One block is 5 ms at 32 kHz. It accepts
ordinary sample arrays and has no address translation, device state or allocation.
Compression and final signed-16-bit conversion follow output composition.

## Recovered behavior

- Stereo output initializes both main channels from the preceding surround
  block, then adds the current voices. Current surround starts at zero.
- Opposite output initializes left from negative preceding surround and right
  from positive preceding surround.
- Matrix output adds returned B-left to main left and subtracts returned
  B-right from main right. Two blocks alternate; the phase advances every
  audio block, including stereo/opposite blocks. Both retained blocks survive
  inactive modes. Continuous matrix output returns the preceding block; mode
  gaps can return older data. Original list-builder/processor execution proves
  this timing. The native stage previously kept one block and froze it outside
  matrix mode. Matrix, matrix, stereo, matrix reproduced a first mismatch at
  sample zero: left 2100 instead of 1100, right 1800 instead of 800.
- Ordinary auxiliary returns add three channels to main left/right/surround.
- Four-channel auxiliary output sends A's three channels plus B surround;
  its returns add to main left/right and B left/right respectively.

The original frame builder passes a fixed surround location at 0x80385850
(r31+0x780), while stereo output uses a rotating location. The list builder
uses the surround location for both the initial fold and final surround store.
The native stage retains this preceding-block value directly. The active mixer
previously discarded main surround in stereo mode.

The output stage is connected to the mixer through the EffectStage, which also
owns the room effect's send/return (see "Gameplay integration" below).

## Reference and limits

audio_output_contract_tests reads the original microcode from main.dol and
refuses another full image. SHA-256 of the accepted DOL:
fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
Microcode begins at 0x80564E60; the bounded programs are word ranges 0x066A
(stereo), 0x020B (opposite), 0x0509/0x051D (ordinary auxiliary), 0x0DA1
(matrix) and 0x0E17 (four-channel auxiliary). Individual range hashes are in
.build/optimization/reverb-contract.json.

The test-only interpreter executes original words. It rejects unsupported
instructions, registers, memory-bank combinations, loop forms and transfers.
Transfer fixtures copy bounded arrays synchronously; they prove neither device
timing nor cache coherency. The ISA semantics were checked against Dolphin's
primary [instruction table](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/DSP/DSPTables.cpp),
[interpreter](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/DSP/Interpreter/DSPInterpreter.cpp),
[extended operations](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/DSP/Interpreter/DSPIntExtOps.cpp),
[arithmetic](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/DSP/Interpreter/DSPIntArithmetic.cpp)
and [loads/stores](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/DSP/Interpreter/DSPIntLoadStore.cpp).
This is a software reference using a single ISA source, not independent hardware.

Comparison values stay within signed 24-bit mix headroom. Original accumulator
overflow stores are not equivalent to ordinary saturating signed-32-bit math
for every possible corrupt bus value. The native stage uses saturating arithmetic
for safety, but no equivalence outside the tested valid domain is claimed.
Bounds of a future effect return must be established before that integration.

Current coverage: 200 stereo/opposite blocks, 64 ordinary auxiliary exchanges,
32 matrix blocks and 32 four-channel exchanges, totaling 536,720 original
instructions. The mixer adds 800 random multi-voice comparisons, two isolated
surround-tail cases and 100 exact checkpoint continuations. Whole-system tests
cover nine stream cases plus two nonzero matrix-phase cases restored into both
reused and fresh owners, each with 16 exact continuation blocks. Missing-bank
and inverted-phase controls each change two PCM blocks across the two cases.

The extended reverb contract combines the original PPC frame builder, auxiliary
processor and effect callback with original DSP output instructions. It supplies
already-mixed voice buses and compares 192 blocks across three modes and two
floating modes. Removing the return changes 39,374 channel samples. Eighteen
output-mode transitions match, including native history restores every third
block. This covers composition, not voice decode, compression or device timing.

Game-level tests execute repeated enable/disable calls and two destructor paths.
Effect history survives toggles. Destructors unregister before explicitly
stubbed resource/platform shutdown calls. An inconsistent active flag leaves a
registered callback at teardown; this is a counterexample, not an unconditional
lifetime guarantee. Other control interleavings and dynamic effect-return
transitions still need proof.

## Checkpoints and timing

Version 22 appends another 1280-byte matrix block and its one-byte phase to
version 21, which added 640 bytes of surround history. Versions 20/21 still
load with an explicit warning that mode-switch output may differ until both
blocks refresh. Their single matrix block becomes the next return. Version 20
also warns about missing surround; version 19 retains its broader missing
decoder/mixer-history warning. Truncated/incompatible audio records
remain rejected. Fixed simulation pacing stays at 60 Hz; audio block size and
sample rate are unchanged. These output checks do not certify every physics
subsystem or establish a rendering throughput gain.

Build and run audio_output_contract_tests, native_audio_mixer_tests and
ax_pb_tests with the tests preset. Integration and real-game evidence belong
to .build/validation/reverb-native/owner-summary.json (current v22 release) and
output-summary.json (preceding v21 release). Both real-game routes preserve PCM
and captures, but use mode 0 and zero matrix history. No audible mode-switch or
performance improvement is claimed from those routes.

## Owned effect stage (2026-10-24)

runtime/lift/audio/effect_stage owns the room effect, three pending room blocks,
spatial-right history, output composition and both scheduled/current controls.
It accepts ordinary native sample arrays. A block's send/return and output mode
follow the preceding schedule; current control governs effect processing and
becomes the next schedule. Disabling preserves pending samples and freezes the
effect. Operations are serialized by the owning audio thread. Rendering performs
no dynamic allocation or device work. Voice resources/decoding, compression and
device delivery remain separate work.

The original-code dynamic comparison covers 360 blocks, 204 callbacks, 66 control
changes after scheduling and 30 native restores across all output modes and both
floating modes. All output channels and future-readable effect history match.
A negative control that clears pending blocks differs at 48,152 channel samples.
Eight invalid control/reset/history operations preserve subsequent output; a
valid reset matches a fresh stage. These are synthetic mixed-voice inputs, not
proof of every gameplay interleaving or the entire voice-to-device path.

Game bank-control tests cover 80 cases: active/inactive entry, both global bank
variants, successful/failed asset-load fixtures, matching/missing keys and an
out-of-range selector. All re-enable the effect after selection without clearing
its tail. Two stop-all paths leave it disabled. The original exclusion/update
code at 0x804402D4 now executes intact; it ignores its incoming r3, increases the
exclusion depth and calls the update routine, whose nested attempt is refused.
Resource and platform shutdown still use explicit fixtures.

Superseded on 2026-10-26: the stage is the mixer's (see below).
Its History is a native continuation value tested in isolation. No translated
callback removal, resource saving or audio improvement is claimed. The production
link contains none of the new stage's symbols; the rebuild differs from the
preceding tested executable only at four COFF/debug timestamp bytes. The exact
byte comparison is retained in dynamic-summary.json; PE field identification uses
Microsoft's [PE format specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format).

## Vector gain arithmetic in the active mixer

The sixteenth increment changes existing gain arithmetic, not routing or ownership.
The signed envelope and each source sample lie in [-32768,32767], so their
product is in [-1073709056,1073741824]. After arithmetic shift by 15 it saturates
to signed 16-bit. An unsigned bus gain in [0,65535] gives a product in
[-2147450880,2147385345], also representable in signed 32-bit. Ramps retain only
their low 16 bits before signed-envelope or unsigned-bus interpretation.
AVX2 multiplies eight independent lanes and preserves these operations exactly.
The bus sum can overflow even though each product cannot: same-sign inputs
with an opposite-sign wrapped result select INT_MIN/INT_MAX using the original
input's sign. The portable scalar path retains wide saturating accumulation.
Phase and interpolation keep the prior arithmetic. No floating-point mode is
changed and no allocation is introduced in mixing.

Tests exercise complete sample/fraction domains at boundary gains and tap pairs,
2,230,400 output samples with exact source history, restored bus extremes, random
ramps/routes, compressor and checkpoint continuation. Real Lost Impact/Final
Haunt PCM and complete checkpoints match the prior executable. Isolated kernel
measurements and their scope are in audio-width/vector-kernel-summary.json.
The console voice/resource scaffold and missing auxiliary effect return remain;
this gain optimization does not establish complete native audio ownership.

## Gameplay integration (2026-10-26)

The mixer owns the EffectStage in place of its bare OutputStage and keeps the
microcode's order: output-mode fold, voices, aux send/return, compressor,
output. Each block's send/return and output mode are the command list's own
(commands 0x04 / 0x13 present = scheduled). The effect pass is the game's
callback: the SDK's aux processor (0x80384E14) calls the registered ReverbHi
callback 0x803885A0, now a native owner (extract/native-owners.txt), which
processes the block sent one block earlier unless the object's bypass byte
(+0x1C4) is set; FPSCR[NI] selects the flush mode. A block whose callback does
not run leaves its pending samples and the tail frozen. ReverbHiInit
0x803884E4 and ReverbHiShutdown 0x80388554 still execute as translated SDK
code; their entries give the effect its parameters (+0x1C8) and object. The
game registers no other aux callback (0x8043F304's only callers pass type 2
or 0 on aux A).

Retired: the translated callback body, its crossfeed 0x80387D24 and
HandleReverb 0x80387EB4 are no longer generated or linked, and the R18a kernel
port with its owner and regression test was deleted. Remaining scaffolding:
init/shutdown and their (unused) delay-line allocations in guest memory.

Evidence: axfx_reverb_contract_tests drives a production-owner stage only from
the original aux processor's calls and compares every output word with the
original CPU and DSP code; the original callback is compared with the owner's
parameter read, bypass gate and native effect over 512 callbacks.
native_audio_mixer_tests checks the mixer's aux A routing and return timing
against an independent model (a mutated return delay is detected). ax_pb_tests
covers the System path: audible return, exact checkpoint continuation of a
nonzero tail (version 23), bypass equal to an absent callback and refusal of a
foreign object. Gameplay results are in OPTIMIZATION_RESULTS.md (twenty-third
increment).
