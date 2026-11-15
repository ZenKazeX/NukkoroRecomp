# sdk-axfx: the three AXFX aux callbacks owned (2026-11-12, branch agent/sdk-axfx, cut from b75201a)

## Finding (changes the shape of the work)

The callbacks ReverbStd 0x803890B0, Chorus 0x8038980C and Delay 0x80389B5C are **unreachable in the game**, so the
owners are **stop owners**, not native DSP ports. Evidence (all executed by `axfx_effects_contract_tests` from the pinned DOL):

* The selector 0x8043F304 (r3 = aux A/B, r4 = mode) registers, run in the reference interpreter: mode 1 -> 0x803890B0
  (context GCAX+0x10000-0x1FC), 2 -> 0x803885A0 (ReverbHi, native), 3 -> 0x8038980C (+0x138), 4 -> 0x80389B5C (+0x1D4), on aux A and B;
  mode 0 clears; modes 5, 6, -1, 0x80000000 register nothing.
* A scan of every instruction and data word of all DOL sections (b/bl/bc targets, lis+addi, lis+ori, data words) finds 11 references
  to the selector, AXRegisterAuxA/B (0x80385050 / 0x8038505C) and the four callbacks: exactly the selector's own (2 register calls,
  4 materializations) and its **two callers**, 0x80440278 (last r4 write before the bl: `li r4,0`) and 0x804402BC (`li r4,2`),
  no control flow in between. Nothing else names any of them.
* Even if entered, the originals could not run: the owned inits (docs/agents/axfx-effects.md) no longer build the delay lines.
So an owner that serves them "identically" has no behaviour to reproduce; a silent return would be a lie, so entry stops the run
(`ExecutionFault`, detail names the callback), the same pattern as RtGCNFS `refuse_async`.

## Owners

`runtime/src/gekko/native_axfx_effects.{h,cpp}`: `kStdCallback/kChorusCallback/kDelayCallback`, `serve_callback_entered`, installed by
`axfx_effects::install` (already called from System::start). Owners list `Owners::addresses` extended (9 rows).

## Gates

1. Contract test (`axfx_effects_contract_tests`, tests tree, ctest PASS 6.4 s): the proofs above plus **7 negative controls planted in the
   DOL image, each detected**: an extra selector caller, a direct bl to the ReverbStd callback, to the Chorus callback, to
   AXRegisterAuxA, a lis/addi naming the Delay callback, a data word holding the Chorus callback, a caller loading mode 3; image
   restored and re-verified. Also: each owner stops naming its callback, a silent-return mutant is detected, install registers all three
   (and not ReverbHi). The earlier init/shutdown sweeps (181 parameter sets) still pass.
2. what_if (`--own` the three, `--release-dir .build/watch`): frees 3 rows (0x80388B20 ReverbStd pass, 0x803890E0 and 0x80389278 chorus
   LFO helpers), 0 revived, no refusal, 0 stale sites. Real run (`removal_plan.py --release-dir .build/watch --max-platform-bodies 9999`):
   **16 -> 10 bodies**, frontier 13 -> 10; remaining are graphics.rw-gamecube-driver 9 + one sdk:unidentified (other agents' rows).
   `SHADOW_GC_REMOVAL_RATCHET` (lifted) 41 -> **10** (CMakeLists.txt line 3660; the non-lifted default 443 untouched, not measured).
   Tests tree: platform_gc_removal_plan PASS (60 s, after copying ppcrecomp.exe into .build/tests/Release). platform_runtime_coupling:
   passes on the shipping tree (`runtime_coupling.py --max-sdk-calls 2`: 0 unclassified); FAILS in the tests tree with 3 unclassified uses of
   0x8037ECD4 in native_vi_wait.cpp (not touched by this branch; a vi row).
3. Westopolis replay (`run.sh` with `--audio-dump audio.pcm` instead of `--no-audio`, 90 game s, base-b75201a.exe vs new exe, alternating
   2x each): state.json sha256 b6a29267293968f7 identical in all four, PCM sha 46a70a1051347069 identical in all four
   (11,447,680 bytes, cmp byte-equal), 3,645,000,000 ticks, 2,697 VI, 5,360 render frames. The run never selects chorus/delay (the new exe
   would have stopped); no stage selects them either, by the static proof, so there is no run that "uses" them. Tripwire census with the new exe,
   headless 15 s each: stg0100..stg0600 (those that boot) ran with no stop; stg0700..stg1200 are not stage names here (exit 2).
   Not a full-stage census.
4. Liveness lists regenerated (`liveness_roots.py --out`, +3 addresses each) and passing: native_liveness_roots, native_liveness_roots_lifted PASS.
   native_file_api_tests PASS (owner rows equal installed).
5. Speed: not measured (no code on a hot path; owners are never entered). No "no regression" claim beyond that.

## Not done / notes

* axfx_reverb_contract_tests, native_file_api_reference_tests were not built in the tests tree (only the two targets that matter were).
* Quick iteration trick used: test linked against wt-opt-integrate's libs (scratch only, not a gate); the gates above are the real tests-tree build.
* Shared lines: native-owners.txt (one block after the AXFX block), liveness-roots*.txt (+3 each), CMakeLists ratchet line.
