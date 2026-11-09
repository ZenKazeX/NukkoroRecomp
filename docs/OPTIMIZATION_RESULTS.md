# Current optimization evidence

This is the current working record for OPTIMIZATION_GOAL.md, not a claim that console removal is complete.

## First increment: attribution and shared dispatch

The new PE/map audit attributes 88,281,899 bytes of the baseline's unwind-covered code to generated game code. It uses actual exception-directory ranges, counts folded aliases once, and explicitly leaves leaf code without ranges unattributed. It checks map/image consistency and hashes both inputs.

The generator now shares the local-target dispatch block in functions containing multiple non-linking computed jumps. It preserves each branch's actual site, tests backward direction only for known local targets, retains external/interior handling and target hooks, and leaves single-site functions specialized. Both hot and cold entry paths use the same rule. No game functionality, rendering quality or workload was removed.

| Measurement | Control | Candidate | Change |
| --- | ---: | ---: | ---: |
| Executable bytes | 109,440,512 | 109,389,312 | -51,200 (-0.0468%) |
| .text file bytes | 94,979,584 | 94,928,896 | -50,688 |
| Generated code, exact unwind attribution | 88,281,899 | 88,231,335 | -50,564 |

Control SHA-256: `a4a5580694950fa7d40241175bf6c8cc27265e65855528bc93ecfa78bd7c9e5b`.
Candidate SHA-256: `3c4d61bf43fe28671d1e6cb5176c12510c03d27f14a1cfed1cef28c5eef3dc6a`.

The control was rebuilt before the generator edit. Its 944-file source-input manifest is `.build/optimization/control/source-inputs.json`, SHA-256 `331a2f68c31c64941118f85f6d85287fc409c59f6bb976838013f2125935e0a4`. The earlier gameplay smoke binary has a different PE timestamp/hash and is not substituted for this control. The playable executable and comparator reference have not been replaced.

This is a code-generation size improvement, **not a new native semantic replacement**. The three largest generated functions still account for about 4.5 MB and require contract recovery. Their size alone does not establish a gameplay hot spot. The generated-code, CPU-state, memory-window and platform scaffolding remaining in the image prevents calling the whole game native.

## Measurement

`tools/stage_benchmark.py` records explicit input hashes, the asset-tree digest, isolated persistence, invocation, output, workload counters and every pair. It clears inherited SHADOW_/GEKKO_ instrumentation settings, checks for concurrent game/build/test processes and uses alternating run order. Its tests reject missing counters, failed work, changed workloads, missing/duplicate pairs, changed binaries and unequal verification evidence.

The current headless implementation builds packets but does not render every frame. Its dropped queue packets are unconsumed packets, not measurements of player frame drops. Timing here covers **simulation and packet construction only**, with audio output disabled in both arms. Captures and debug-layer requests run in a separate verification group. No full-rendering speed, player FPS or audio performance claim follows from these runs.

Westopolis, 22,000 game milliseconds, 1280x720, identical movement tape and persistence:

| Three pairs | Control median | Other-arm median | Difference |
| --- | ---: | ---: | ---: |
| Identical-binary calibration, wall seconds | 1.875358 | 1.887047 | +0.6233% |
| Candidate comparison, wall seconds | 1.893822 | 1.906340 | +0.6610% |
| Candidate comparison, process CPU seconds | 1.984375 | 1.968750 | -0.7874% |

All three candidate wall-time pairs were slower; CPU-time deltas had mixed signs. Calibration also showed variation between identical binaries. **No speed or memory improvement is established.** Calibration is not subtracted from candidate measurements. The current verified gain is file/code size only.

Raw evidence is in `.build/optimization/simulation-calibration/` and `simulation-comparison/`; all samples are retained in their summary JSON. Failed preliminary calibration correctly rejected a live build worker and then an incorrect assumption that headless mode rasterized every frame. Those runs supply no accepted performance evidence.

## Correctness evidence

- Existing instruction/interrupt, native execution and polling tests pass with the new generator. New shared-dispatch cases compare seven routes against the instruction interpreter, including cold entry, CTR low bits, untaken conditional jumps and unknown interior/external targets; they also verify the actual yield site, forward-poll exclusion and served/declined target hooks.
- Output-structure tests require one shared dispatch block as computed-jump site count increases. The size-audit tool passes eight synthetic PE/map tests, including aliasing, padding, missing symbols and invalid input rejection.
- Westopolis and Final Haunt each passed two baseline/candidate verification pairs. Every capture and complete reported memory-block digest, clock and frame field matched across all four runs of each route. This is the existing state-report contract, including its renderer-word clearing; it is not a comparison of all host state.
- Both release builds pass the production executable boundary check. This proves the checked reference/interpreter components are absent, not that all translated game code is gone.
- The original suite's two B45.mem1 fixture failures remain unresolved. Tests and assertions are retained. The suite must not be reported entirely green.

## Next work

Use the size attribution together with current route profiling to select a bounded algorithm, recover its ownership/data/behavior contract from the original DOL, implement its native value/pointer API in the owning subsystem, and remove the original body and dependencies from generation. Require genuine reference equivalence and repeatable runtime/resource evidence before claiming the requested native replacement or faster gameplay.

## Confirmed compiler suppression (second increment)

An isolated compile of fn_803E1D58 with the actual release compiler and /w14883 reported C4883 for both generated bodies: function size suppresses optimizations. Changing the 24 FP operations from forced to discretionary inline reduced the object but did not resolve either warning, so that source change was reverted completely. Microsoft documents /d2OptimizeHugeFunctions as the override for this specific suppression ([Microsoft Support](https://support.microsoft.com/en-au/topic/visual-c-optimizer-fixes-for-visual-studio-2015-update-3-4de690cf-20e9-e0bf-7fd2-f82b29e266a9)). The full generated target now uses that switch and /we4883: any future suppression fails the build. Floating-point flags, arithmetic, workloads and the inline helper definitions are unchanged.

The complete release built successfully: 105,695,744 bytes, SHA-256 06f212925e31f5b2e99161c2daac892772bcf514289a7e27dc86ef69b0bc1d3e. This is 3,744,768 bytes (3.42%) smaller than the original control, including the first dispatch increment. Generated unwind-covered code is 84,537,867 bytes. The isolated hot-body prologue shrank from a 110,312-byte allocation to eight register pushes plus 56 bytes (120 fixed bytes total). The audit now reads v1 unwind prologues and chained records; unsupported records stay unknown, and fixed frames are explicitly not peak thread stack usage.

Five alternating Westopolis simulation pairs are recorded in .build/optimization/optimizer-comparison. Candidate wall median was 1.906995 s, +0.9239%; signs differ between pairs. No gameplay speed or whole-process memory gain is claimed. The optimized large functions have not been established as hot on this route. Two verification pairs each in Westopolis and Final Haunt passed the complete reported-state and image comparisons before the scalar replacement.

The full suite immediately before this compiler change passed 94/96 checks. Both remaining failures require B45.mem1. This result must not be represented as validation of later changes.

## Native scalar replacement (third increment, validation in progress)

The recovered scalar square-root contract is implemented in runtime/lift/math/scalar.cpp. The original software routine is no longer generated or linked as x64 code. The original-word numeric oracle passed 40,480 cases and 18,162,056 instructions; the build's post-link pruning/absence gate passed. See NATIVE_SCALAR_MATH.md for the value/error contract and remaining call-site marshalling.

Current release: 105,688,576 bytes, SHA-256 b7b8da9cd11b0fa0c332066a638bf5ebf671780c353a1d05cbadfa5880341cd0. The scalar change removes another 7,168 file bytes; total reduction from the original control is 3,751,936 bytes (3.43%). Generated unwind-covered code is 84,530,411 bytes. The final linked fn_803E1D58 hot body is 119,129 bytes with 120 fixed stack bytes, versus 742,845 and 110,312 respectively in the control; its cold body is 161,761/88 versus 779,341/106,888.

The original 5,773,024-byte DOL remains embedded exactly as resource/bootstrap data at current file offset 99,148,096. This includes original PowerPC code bytes. Removing the superseded translated x64 body is not removal of every original byte or complete console dependency removal. No packer, resource stripping or missing feature produced the reported file reduction.

Five new alternating Westopolis timing pairs are in .build/optimization/native-math-comparison. Wall medians: control 1.893474 s, candidate 1.902870 s (+0.4962%); four candidate pairs are slower and one faster. Both process-CPU medians are 1.953125 s and both peak resident-memory medians are 226.011719 MiB. These runs establish no gameplay speed or whole-process memory improvement.

Westopolis passed two complete image/state verification pairs. Final Haunt's images match in all four runs, and each arm's state is self-identical, but the strict cross-build state gate fails: one 64 KiB image block (0x80600000..0x8060FFFF) differs. The result remains a recorded failure pending exact byte-level attribution. No field or block has been excluded. The new state_delta.py reports every changed byte count and range, plus CPU and trailing-state comparisons; it never grants equivalence automatically.

Both complete presets built after integration. The expanded full suite initially passed 96/99 checks: the two missing-fixture failures and an old assertion that the global lift catalog contained only eleven input sites. The repaired catalog test explicitly requires all eleven input sites plus the pinned scalar call, and the targeted rerun passed all four checks (catalog, numeric oracle, generated scalar caller and linked absence). Effective current coverage is 97/99 passing; the B45.mem1 failures remain. The previous 94/96 result is historical.

### Exact snapshot investigation and caller lifetime

The additional raw-snapshot pair in `.build/optimization/math-state-audit` was compared without exclusions. Both CPU states (all three serialized blocks), heap regions, locked cache, trailing serialized state and game ticks match. Exactly 28 image bytes differ in five ranges:

| Start | End (exclusive) | Bytes |
| --- | --- | ---: |
| 0x8060BDF8 | 0x8060BDFC | 4 |
| 0x8060BE00 | 0x8060BE04 | 4 |
| 0x8060BE08 | 0x8060BE10 | 8 |
| 0x8060BE40 | 0x8060BE48 | 8 |
| 0x8060BE80 | 0x8060BE84 | 4 |

Both current stack pointers are 0x8060C538. The first three ranges retain the old scalar frame's back chain, input and result; the enclosing return address 0x803AF818 remains nearby. A subsequent hardware-watch run traced the other two ranges to the old routine's input spill at 0x803B2D30 (stack pointer + 8), with the exact final values. All 28 bytes are now attributed to removed scalar-private storage. The watched control's complete snapshot is byte-identical to the earlier control. The strict byte gate still records the real differences and `byte-delta.json` retains `equivalence_proven: false`; write attribution is combined with the separate caller-lifetime tests, not substituted for them.

To test the recovered lifetime contract directly, the original-word oracle now executes 1,203 paired enclosing math calls, exercising all four scalar call sites. Native and original enclosing results match, and access tracing found no read of retired scalar scratch before overwrite. This supports the native value/error boundary without reproducing obsolete stack stores. It does not silently convert the full gameplay snapshot failure into a pass. No scalar algorithm or production executable changed during this test strengthening.

### Final Haunt timing and current gameplay captures

Three identical-binary calibration pairs and five alternating candidate pairs are retained in `final-haunt-calibration/` and `native-math-final-haunt/`. Calibration wall medians were 2.477554/2.498425 seconds (+0.8424%). Candidate-comparison medians were 2.485016/2.508511 seconds (+0.9455%); three candidate pairs were slower, two faster. CPU medians were 2.53125/2.5625 seconds (+1.2346%). Peak resident medians were 224.656250/224.457031 MiB; private-memory medians 185.441406/185.058594 MiB. These small, variable changes do not establish faster or lower-resource gameplay. Calibration is not subtracted, and no samples are discarded.

The current binary also completed separate 22-second windowed Westopolis and Final Haunt runs at 1280x720 with audio and the D3D12 debug layer requested. Each reported 1,279 frame packets, 1,269 presentations, nine dropped packets, one capture and zero render failures. Audio was active with zero underruns, open failures or render errors; assets reported zero failures. The telemetry's presentation-interval section is unavailable, so this is not an FPS benchmark. The locked desktop was not unlocked, and audio was checked through telemetry rather than claimed listening. Both real screenshots were visually inspected and copied to `game/screenshots/latest-*.png`. Evidence is in `native-math-gameplay/`; its rejected preliminary launch used an invalid stage ID, exited before gameplay and contributes no validation result.

The objective remains active: size reduction and bounded native replacements are demonstrated; repeatable gameplay speed/resource gains, missing-fixture checks and substantial remaining console dependencies are unfinished.

Current source-input provenance: 954 files in `.build/optimization/source-inputs.json`, SHA-256 `c881f500fa6ba87fb804b5bd3b662e1a07dbb4ca8002bad42c55452530a2e1bd`. Compared with the control manifest, eleven existing files changed and ten files were added; none disappeared. The temporary FP-helper experiment is absent. Standalone tool checks pass: thirteen size/unwind audit cases, eight benchmark-validation cases and four exact-state-delta cases.

All 56 pinned personal files and both retained playable executable hashes were rechecked unchanged. After verification, 385 owned per-run input/persistence copies (1,291,951,353 bytes) were deleted. The pinned original inputs, all sample summaries/invocations/logs, screenshots, and the two diagnostic snapshots remain; `.build/optimization/disposable-cleanup.json` records the cleanup scope.

## Matrix copy and runtime attribution (fourth increment, validation in progress)

The current release is 105,688,064 bytes, SHA-256 `9fa88867e6c23dc581d6afe261593ec4d6bc36b35e66f77d5fc11342a31a47c6`. It is another 512 bytes smaller than the scalar candidate, for a total reduction of 3,752,448 bytes (3.43%) from the rebuilt control. Exact generated unwind attribution is 84,530,056 bytes. The matching map SHA-256 is `158d95b663dcdac0fd1e172ed70c661be7ea368cf81a56da84a480336bd6a57a`. These figures are in `.build/optimization/matrix-size.json`.

Runtime wall-time sampling identified matrix multiplication and copying among the repeatedly observed generated routines. The recovered 48-byte copy now uses an ordinary host-pointer API, preserving forward-overlap behavior and every element representation. Its two callers emit the native operation; the original body is absent from generated entries and the linked map. The release post-link gate passes for both math replacements. `NATIVE_MATRIX_MATH.md` records the contract and original-word oracle; `NATIVE_DIAGNOSTICS.md` records the new sampling and hardware-watch tools and their limitations.

Both complete build presets succeeded. The full suite passed 99/101 tests; only the two existing B45.mem1 fixture failures remain. Generator structure, lift catalog, actual generated matrix calls, purity and linked absence all pass. The strengthened matrix oracle then passed 45,056 original-word cases (2,342,912 instructions), 516 unaligned native cases, and 1,024 paired enclosing multiply/singular-inverse calls with identical output, initialized stack and return values. Each copy site was exercised 512 times. Logs are in `.build/validation/matrix-ctest.log`, `matrix-ctest-console.log` and `matrix-caller-oracle.log`.

Two current-binary verification pairs per route are recorded in `matrix-verification-westopolis/` and `matrix-verification-final-haunt/`. Westopolis passes all image/reported-state comparisons. Final Haunt images match in all four runs and each arm is self-identical; the strict cross-build snapshot gate records exactly the same 28 scalar-private bytes described above. CPU blocks, heap, locked cache and trailing serialized state match. Comparing the matrix snapshot with the previous scalar snapshot finds zero changed bytes anywhere. The matrix replacement introduces no additional snapshot difference on this route. These are correctness checks, not timing samples.

Five new alternating simulation pairs per route are retained in `matrix-simulation-westopolis/` and `matrix-simulation-final-haunt/`; all workload checks pass. Westopolis wall medians are 1.888302/1.896162 s (+0.4162%) and both CPU medians are 1.968750 s. Final Haunt wall medians are 2.514268/2.500331 s (-0.5543%); CPU medians are 2.546875/2.531250 s (-0.6135%). Pairwise time deltas still have mixed signs. Final Haunt peak private medians are 185.789062/185.070312 MiB (-0.3869%), with all five candidate values lower; peak resident medians are 224.531250/224.589844 MiB. These bounded observations do not establish broadly faster or lower-resource gameplay. Calibration is not subtracted.

The three-pair identical-binary rendered/audio calibration in `rendered-calibration/` is rejected by the equal-workload gate: presentations/GPU frames vary between 1,277 and 1,278, dropped packets between one and two, and consumed audio frames between 686,120 and 686,400. Individual runs have no reported renderer/audio errors, but their completed work differs. Process CPU medians also vary by 18.2% between labels despite identical executable hashes. The subsequent five-pair comparison in `matrix-rendered-final-haunt/` also fails the equal-workload gate. All samples and failures are retained. These measurements cannot establish an optimization gain and are not adjusted or normalized to hide the failure. Earlier scalar timing and screenshots above belong to the previous hash and are not relabelled as current-binary evidence.

All 34 focused tool checks pass: thirteen size-audit, ten benchmark-evidence, four exact-state-delta, three sampling-index, two hardware-watch/layout and two native-code-removal cases. The removal gate rejects surviving interior entries as well as the original base entry; both actual release replacements pass the stronger gate. The benchmark now also supports paced rendering/audio resource comparisons, requiring equal complete workloads and keeping verification separate. The 451,670,700-byte watch log was compressed losslessly to 18,759,335 bytes and round-trip SHA/record counts verified before removing the uncompressed copy. No executable compression is involved.

Current source-input provenance: 963 files, manifest SHA-256 `a8cb6fda0110e32e6a05157cbe28c96e3890c6e12f343150af773815791013e5`. Compared with the rebuilt control, eleven existing input files changed and nineteen were added. The detailed manifest/delta replace the previous scalar-stage provenance at `.build/optimization/source-inputs.json` and `source-input-delta.json`.

### Fresh rendered captures and remaining accounting limits

The current executable completed fresh 22-second windowed Westopolis and Final Haunt runs at 1280x720 with audio and the D3D12 debug layer requested. Both reported zero render failures, asset errors, audio underruns, dropped audio frames, open failures and audio render errors. Each produced 1,279 packets and a real screenshot, visually inspected before publication to `game/screenshots/latest-*.png`. Westopolis image SHA-256 is `bc6f536f6d300b48b6fd50d628d0876b5b89da14ed6ed6efc6e94c2fc5259afa`; Final Haunt is `8c1083cb91ac18fe4d6de3d82d33d5fd08bf633435993eb30650892165270390`.

The first new validator incorrectly counted a screenshot as a separate queue consumer. Reading `LiftedRender::present_newest` establishes that capture renders the same packet before presentation: it adds a GPU render, not a consumed packet. The corrected regression covers this case and rejects using a capture to excuse an unpresented packet. Original results are retained alongside `matrix-gameplay/*/reanalysis.json`, which pins the raw inputs and corrected validator. Westopolis accounts for all packets (1,271 presented + eight dropped). Final Haunt accounts for 1,278 (1,268 presented + ten dropped), leaving one packet unaccounted for at shutdown; its complete-packet gate remains failed. Both GPU counts equal presentations plus the one capture. This is not represented as fully passed rendering validation or an FPS measurement.

The final targeted CTest rerun passes benchmark tools, native math absence and lift purity. The production binary did not change during validator correction. All 56 pinned personal files and the deployed/reference hashes were verified unchanged. Cleanup removed 240 isolated run-input copies and five byte-identical snapshots, reclaiming 1,015,199,892 bytes. `matrix-disposable-cleanup.json` maps every removed snapshot to one of the two retained canonical snapshots in `math-state-audit/`; per-run JSON hashes, logs and images remain. No personal file or test assertion was removed.

Next implementation target: recover the complete matrix-multiplication contract identified by sampling, including aliasing, arithmetic order, rounding and retained exception-table data, before replacing the enclosing translated owner. The matrix copy is a bounded native operation; its surrounding console state and the embedded original DOL remain. Repeatable faster/lower-resource gameplay and the missing historical fixture checks are still unfinished.

## Native affine composition (fifth increment, validation pending)

The pure native matrix implementation now composes a 3x4 affine transform with packed x64 FMA for ordinary finite values and explicit exceptional-value handling. All 68 original direct callers use one generated marshalling boundary. The retired multiply and its internal copy call are absent from the generated test build; the inverse copy remains. The exception-index range key is exempted from executable roots only after pinning its registration, lookup, sorted index and descriptor. No DOL data is rewritten.

The independent oracle passes 29,700 original-word numerical cases (5,227,200 instructions), plus 256 paired enclosing-transform calls with identical matrices and no read of retired private stack before overwrite. Original exception registration and lookup also pass. Four removal-evidence tests pass, including changed-consumer/metadata rejection and an unexpected second code pointer. Lift purity passes for 69 files. Logs are `.build/validation/compose-oracle.log` and `compose-oracle-build.log`.

The complete test build succeeds. The new boundary test passes all 68 sites across 2,992 endian/overlap/observation/journal cases and 272 invalid-span refusals, preserving nonvolatile state and game time. The independent numerical oracle also verifies unchanged floating-point control/status. The first targeted run caught a stale catalog assertion expecting the removed internal copy; the revised test explicitly requires that call's absence and the retained inverse call's presence. Its rerun and both matrix tests pass. Generator structural tests pass, including exclusive callee removal and stale call-word rejection. Evidence is in `compose-targeted.log`, `compose-catalog-recheck.log` and `compose-generator.log`.

The release build succeeds: 105,664,000 bytes, SHA-256 `897f60e39ff851fca7c5a32f661f7e0ee2012960d5ac987821ae1aebddc08725`. It is another 24,064 bytes smaller, for a total reduction of 3,776,512 bytes (3.45%) from the rebuilt control. Exact generated unwind attribution is 84,501,840 bytes. The matching map SHA-256 is `8cfe4d5dd9da1f031b191eb0820e6541fbcf39121d5571f84a45762e02b95d66`. All three native math removal gates pass against the actual release map. `compose-size.json` records these measurements; `compose-host-code.json` confirms the ordinary numeric path contains packed multiply, two packed FMA instructions and separate addition. That instruction inventory establishes emitted code, not execution frequency or speed.

The full suite completed: 100/102 checks pass. Only the two existing missing B45.mem1 fixture checks fail. Gameplay evidence in preceding sections still belongs to their stated older hashes. No composition speed gain has been measured. Both the existing missing-fixture failures and rendered workload/accounting limits remain unresolved. Current source inputs are pinned at 964 files, manifest SHA-256 `d903dd3e21c976372dded653996f9abc4592bf6a24299297488534f0b5de423b`; eleven existing files differ from the rebuilt control and twenty were added.


### Composition gameplay investigation (ongoing)

Two verification pairs per route use the current composition executable. Westopolis passes every image and reported-state comparison. Final Haunt images match across all four runs and each build is self-deterministic, but the strict cross-build state gate fails. Against the previous matrix-copy snapshot, the candidate changes two architected FPSCR bytes, 291 image bytes and 232 heap bytes; no other serialized region differs. No fields are excluded and no performance samples have been taken for this increment.

`compose-verification-final-haunt/padding-audit.json` structurally attributes all 116 changed heap words to the modelling/LTM padding of 58 RwFrames (42 pad2 pairs, 16 pad3 pairs). Every other byte in each complete frame matches, including all transform coefficients. SDK layouts and a self-rooting frame pointer provide the structural check. Hardware watches separately identify two pad2 writers: a 64-byte matrix copy after a routine that writes coefficients but leaves padding uninitialized, then the modelling-to-LTM copy. This is bounded attribution, not a blanket equivalence assertion.

The first broad status trace timed out and is retained as failed evidence. A narrower trace disarmed its CPU watch after the first masked status change and completed with no debugger errors; its entire snapshot equals the uninstrumented control. The extra FPSCR bits 0xA0080000 first arise at the ordered comparison at original PC 0x8040DEE0, after vector normalization overflow. At that event the input vector is approximately (61.3833275, 3.6893488e19, 180.066666); caller linkage identifies 0x80097A1C. The cause of that input and the remaining private-stack differences are under investigation. This does not establish that the differences are harmless, nor that the optimization is ready for acceptance.


The next bounded hardware-watch pair completed with no errors and exact per-build snapshot matches. It stopped observing after the 88th write to the selected normalization input from caller link 0x80097A08, then allowed the game to finish normally. Control's stale collision-output word is from the removed matrix routine's saved floating-point register; candidate retains a previous stack linkage value instead. Both queries return -1 (no hit). The original caller nevertheless subtracts and normalizes that untouched output before testing failure. Its discard path ignores this local vector. The independent original-word regression now reproduces the exception difference while proving both stale inputs reach the same discard branch without writes outside the local frame in 32 bounded radius/motion cases. It does not claim exhaustive collision semantics or excuse every remaining stack byte. Logs are `compose-discard.log` and the trace pairs under `compose-write-watch/failure-*/`.

The watch tool now supports an explicit watched-address/link/occurrence stop condition: it disarms observations, not the game. It captures the terminal frame and supports direct lossless gzip output. Five tool unit checks pass. The old 1.62 GB broad trace and three smaller streams were compressed and round-trip hashes/counts verified before removing their raw copies; the failed timeout summary is preserved. No game bytes or strict comparison fields were changed to match legacy side effects.


### Uninstrumented composition measurements

Three identical-binary calibration pairs and seven alternating control/candidate pairs were run separately for each stage. All simulation workload checks pass; every pair is retained. Westopolis calibration wall medians are 1.898961/1.895420 s (-0.1865%), with equal CPU medians. Final Haunt calibration wall medians are 2.513154/2.499912 s (-0.5269%), CPU medians 2.578125/2.593750 s (+0.6061%). Calibration is reported, never subtracted.

| Simulation route | Control wall median | Candidate wall median | Wall change | CPU change |
| --- | ---: | ---: | ---: | ---: |
| Westopolis | 1.893214 s | 1.843591 s | -2.6211% | -3.9370% |
| Final Haunt | 2.495180 s | 2.406910 s | -3.5376% | -3.6810% |

All seven elapsed-time and CPU deltas favor the candidate on both routes. These measurements support reduced CPU cost for the tested simulation/frame-packet workloads. They exclude full GPU rendering and audio output, and do not prove higher player FPS or whole-game equivalence. The benchmark keeps `performance_gain_proven: false` for that broader claim. Westopolis private-memory medians are 186.449219/186.015625 MiB; Final Haunt 185.128906/185.148438 MiB. Working-set changes are below 0.15%; no meaningful whole-process memory reduction is claimed. Provenance and all samples are in `compose-calibration-*` and `compose-simulation-*`.


### Current rendered captures, final checks and cleanup

The composition executable completed fresh 22-second windowed runs on both routes with audio and the D3D12 debug layer requested. Both captures were visually inspected and match the already-published `game/screenshots/latest-*.png` bytes exactly; no older capture was substituted. Current invocations, executable hashes and images are in `compose-gameplay/`, with `published.json` mapping the fresh captures to the same published pixels. Audio is active with zero underruns, dropped audio frames, open failures and render errors; asset failures and renderer failures are zero. Audio was checked through telemetry, not claimed listening.

Both complete-rendering gates remain failed: Westopolis has 1,272 presentations + six dropped packets, Final Haunt 1,267 + eleven, against 1,279 produced packets each. Each leaves one unaccounted packet at shutdown. GPU frame counts are presentations plus one real screenshot. These captures are gameplay evidence, not fully passed rendering validation or FPS measurements. The initial capture preparation failed before launch because two tool fingerprint schemas were mixed; the copied input was verified and the schema corrected before either game ran.

The follow-up original-word regression, purity and liveness gates pass. All 39 focused tool cases pass. The complete suite remains 100/102 passing, with the same two missing historical fixture failures. All 56 pinned personal files and the deployed/reference executable hashes were rechecked unchanged. Current source-input provenance is 964 files, manifest SHA-256 `ddb9f7832b11c7a7ae8774fd58b4b2a89b9fa3394bd1e20c435056c0402b0689`. No production code changed after the measured composition release; subsequent edits strengthened tests, diagnostics and documentation.

Cleanup removed 289 owned disposable files, reclaiming 1,400,508,398 bytes: isolated per-run inputs/persistence, snapshots verified byte-identical to retained canonical copies, and one redundant broad trace replaced by bounded causal repeats plus an explicitly partial event extraction. `compose-disposable-cleanup.json` records paths, hashes and replacements. Earlier raw traces were removed only after lossless compression verification; all failure summaries, sample results, commands, captured images and required evidence remain.

The objective remains active. This increment demonstrates a smaller executable and repeatable reduction in the tested simulation CPU workloads. Remaining stack attribution, complete rendered packet accounting, player FPS/resource validation, missing fixtures and most console dependencies are unfinished. The original embedded DOL and translated game code remain; this is not a fully native game.


## Native frame-queue ownership and current measurements (sixth increment)

The missing rendered packet was still owned by the queue when the UI loop ended. `PacketQueue::close` only set a flag and woke waiters; pending packets and their vector storage survived until the queue's destruction, without being popped or counted as backlog drops. Close now cancels and releases those packets, rejects later pushes without consuming their argument, and wakes consumers. Payload destruction occurs outside the lock. Shutdown cancellations have their own counter and trace event; the trace flush follows cancellation. Newest-frame delivery and asynchronous gameplay remain.

The allocation probe ruled out an initial per-frame-allocation concern. Across 20,000 push/pop cycles, this compiler's deque made only two allocations (384 bytes) for one-packet bursts and four (1,024 bytes) for three-packet bursts. No alternate container or allocation speedup is claimed. The ownership test checks actual frees, producer/close races, consumer wakeup and rejected-push ownership. Its first attempt confused aligned vector interiors with allocation addresses and failed. Correcting the probe exposed a 16-byte offset and passed without changing the queue fix; the failure log remains.

Both complete build presets succeed. The release is 105,664,512 bytes, SHA-256 `b0076bf858380ea381fd587df93665f9b01fe8f11a4d3cb8d26649f34ccd340e`. The fix adds 512 bytes versus composition alone, leaving a net 3,776,000-byte reduction (3.45%) from the rebuilt control. Matching map SHA-256: `2cc0868ca202818ce69f9517614e0301ce2f9feb281a559a245665ddf9b94648`. Generated unwind attribution remains 84,501,840 bytes and all three retired math-body gates pass. `queue-size.json` records the exact sections. The shutdown fix itself is not an additional size or runtime speed optimization.

Both fresh rendered routes pass complete packet-ownership, GPU-work, audio and asset checks: each has 1,279 pushes, 1,270 pops/presentations, eight backlog drops, one shutdown discard and zero pending packets. GPU work includes one additional real capture. Audio is active with zero reported underruns, dropped audio, open failures and render errors; no listening claim is made. Fresh screenshots were visually inspected and are byte-identical to preceding composition captures and the published images. `queue-gameplay/published.json` pins the current executable and images. Reported game state also matches. Headless Final Haunt records zero pops, 1,277 backlog drops, two shutdown discards and zero pending; its complete snapshot equals the composition candidate. These checks verify queue ownership without inventing an extra presentation or suppressing state differences.

The complete suite passes 101/103 tests in 363.26 seconds. Only the two existing missing B45.mem1 fixture checks fail; all results remain in `queue-ctest-console.log` and `queue-ctest.log`. Read-only scans of retained real comparison snapshots found eight sectors, ten static models and three skinned models, versus the historical fixture's eight/twenty/five. Substituting those snapshots would reduce coverage and break independently checked negative controls, so no fixture was fabricated and no test expectation reduced. `fixture-recovery-scan.json` records the scan. The renderer core WARP/debug-layer and queue ownership tests pass. The latest focused tool run passes thirteen benchmark and five hardware-watch cases.

### Remaining matrix-stack attribution

`matrix-stack-watch/attribution.json` covers all 289 changed matrix-related stack bytes in 81 aligned words across five old frame bases. Twenty-one sequential watch batches retain final writers and exact event counts, with no chronological stream. Their selected runs observe 16,264,874 writes and every complete snapshot matches the uninstrumented control. The initial first-batch result lacked an explicit fresh-file flag, so a separate confirmation repeated its four watches; both runs observed 83,064 writes and matched the control. The confirmation also verifies that the snapshot was freshly written.

Thirty words (94 changed bytes) are direct matrix-prologue stores: backchains, caller linkage and saved registers. Thirty-eight words (148 changed bytes) are the temporary twelve-element matrix copy. Thirteen words (47 changed bytes) are paired floating-point saves. The latter have exact helper-writer attribution plus the unique direct guest return link and matching frame/store footprints; parent-routine attribution is strongly supported, not a captured native call stack. All final words match the canonical control and preceding copy-only snapshot. Original DOL body/store words and all trace summaries are pinned.

This completes bounded final-writer attribution, not an all-path read/lifetime proof. The separate 256 transform-caller and 32 collision-discard cases retain their stated limits. Heap padding and the no-hit caller's exception status retain the earlier causal evidence. Strict control/candidate snapshot equality remains false; no bytes are ignored and no obsolete stack writes have been restored to manufacture equality.

### Presentation measurement correction

The benchmark now retains the existing host presentation-interval histogram and validates its sample count. It reports percentile bounds rather than treating the final [100 ms, infinity) bucket as an exact 100.1 ms measurement. Its threshold counts correctly mean at least 20/34/50 ms, despite the legacy text saying 'longer than'. Missing measurements remain explicitly unavailable. These are intervals after host render/present returns, not GPU execution or display scanout. The current capture/debug-layer logs have 1,269 intervals each, with maximums 615.8 and 609.3 ms; these verification runs are not performance samples. `queue-gameplay/intervals.json` retains all values and parser provenance.

### Fresh uninstrumented current-build measurements

Three identical-control calibration pairs and seven alternating control/candidate pairs per stage ran separately from diagnostics, screenshots and audio/GPU rendering. Every workload gate passes and every pair is retained in `queue-calibration-*` and `queue-simulation-*`, with the commands and tool hash in `queue-measurement-plan.json`.

| Simulation route | Control wall median | Candidate wall median | Wall change | CPU median change |
| --- | ---: | ---: | ---: | ---: |
| Westopolis | 1.889262 s | 1.834433 s | -2.9021% | -2.4000% |
| Final Haunt | 2.488659 s | 2.401331 s | -3.5090% | -5.5215% |

Every elapsed-time pair favors the candidate. Six of seven Westopolis CPU pairs favor it; the final pair regresses by 0.09375 CPU seconds and remains included. All seven Final Haunt CPU pairs favor it. Westopolis CPU medians are 1.953125/1.906250 seconds; Final Haunt 2.546875/2.406250. Calibration wall median differences are +0.0253% and +0.0627%; CPU differences are +1.6260% and -0.6135%. Calibration is reported, never subtracted. The Westopolis CPU effect is less consistent than the preceding release's run; these results do not establish that the shutdown fix itself improves steady-state performance.

Working-set median changes are +0.0881%/-0.0122%, private-memory changes +0.0859%/-0.3475%, within observed run variation. No meaningful whole-process memory reduction is claimed. These results support lower cost in the tested simulation/frame-packet workloads; player FPS, full-rendering CPU gains and full-game equivalence remain unproven. The benchmark's broad `performance_gain_proven` flag remains false.

### Preservation and cleanup

All 56 pinned personal files and both deployed/reference executable hashes remain unchanged (`queue-preservation.json`). Current source/tool/test provenance contains 965 files, manifest SHA-256 `634c7a9ed88cf875a10c9e4f0915fd2fb7693284f98bda7814c8de15bc633a00`; fourteen existing inputs differ from the rebuilt control and twenty-one were added. Later changes strengthen diagnostics and tests; the measured production binary is unchanged.

Cleanup removes 222 owned disposable files totaling 822,188,964 bytes: isolated input/persistence copies and two snapshots verified identical to retained canonical versions. `queue-disposable-cleanup.json` records every path, hash and replacement. All commands, measurements, failures, final-writer summaries, captures and required source remain. The objective stays active: most translated game code and the original embedded DOL still remain, alongside incomplete full-game and rendered-performance validation.


## Second matrix-copy body (seventh increment, in progress)

Fresh game-thread IP diagnostics of release b0076bf8 cover 90 simulated seconds each in Final Haunt and Westopolis. Their complete workload checks pass; the instrumented timings are not performance measurements. The remaining translated body at 0x8040D198 is a frequently sampled twelve-float copy. Its complete 32-byte body is identical to the already-replaced 0x8040CD04 implementation (SHA-256 949be76368af4f56469cd79b3c4ca0f058309b6098a4bc4649b7d31b069a4faa). The original image has exactly these two aligned text matches, 683 direct callers of the duplicate, no data pointers into its body and no constant-address materializations found by the diagnostic scanner. Computed-address retention remains the generator's conservative gate. The incoming-edge hash is 7a40bd50b71c34d1d3963dd7b65af59e35b30ed27fc97146db5a7058ae1b0c75.

The source now routes those calls to the existing pure native copy algorithm, through one temporary, non-inlined marshalling boundary shared with the inverse's existing copy site. The boundary test reads the maintained site catalog and exercises all 684 generated copies; test metadata does not introduce retired entry points as runtime roots. The original-word oracle now executes both complete copy bodies and adds 768 paired executions of the original matrix-stack push caller. Six tool checks pass, including negative controls for changed body bytes, incoming edges, data references and duplicate inventory alignment/section scope. Exact text matches are candidates, not automatic semantic replacements: relative targets and return-address materialization still need recovered contracts. The cross-reference tool was made importable without executing its CLI.

Build, generated-code removal, gameplay comparison and current-increment performance remain pending. Existing published measurements and screenshots continue to describe the b0076bf8 queue-fix build. No gain is claimed for this new source change yet. Evidence starts at matrix-copy-clone-contract.json and copy-family-inventory.json.


The seventh increment's tests build succeeds. Seven focused gates pass, including all 684 generated copy sites (8,208 overlap/observation/journal cases and 1,368 invalid-span refusals), 90,112 original-word copy cases (4,685,824 instructions), 768 paired original matrix-stack pushes, and the existing 1,024 enclosing matrix-algorithm pairs. The duplicate is pruned and absent from the test executable's linked symbols. Release compilation is still pending, with the preceding b0076bf8 release temporarily pinned under increment-control for a direct comparison.

A separate read-only profile investigation follows the explicit x64 chained-unwind records from the unnamed 32,815-byte hot range to handle_reverb_kernel<false>, with no nearest-symbol guess. The chain and binary/map pins are in cpu-profile/unwind-owner-observation.json. Microsoft's x64 exception-handling documentation defines that chain's final entry as the primary procedure entry. The source header already identifies this reverb as a hand-specialized translation, not a native algorithm; this is a substantial next replacement target. Reusable sampler attribution is not yet implemented, and no runtime gain follows from identifying it.

The user's latest end-state clarification is recorded in OPTIMIZATION_GOAL.md. Eliminating the remaining GameCube burden is still active; these bounded increments do not redefine completion.

The first release of this increment (0b9a736287e475d0440d960386d1806b47a931c0a01013def3eb4a929c7a5f4f; map 4b98f7b91e23f230e4021533487d49cf26a0aee9ec0b50d8f77f2f49f9f2d91f) builds and passes all four linked-body removal gates, but is 58,368 bytes larger than b0076bf8: +44,032 text, +12,800 read-only data, +1,536 relocations. This is a rejected size result, not an optimization claim. The shared value copy was already compact; generated callers still repeated cut/continuation/exit bookkeeping. That temporary call-boundary bookkeeping is now consolidated in one function without changing its order. New compiled-call tests also cover prior faults and failed spans. The revised build is pending. The first release audit remains in copy-family-size.json and copy-family-size-delta.json until superseded with explicitly identified final evidence.


The first matrix-copy candidate's complete CTest run finishes at 101/103 (403.14 seconds); the only failures remain the absent B45.mem1 texture/geometry fixture. This result predates the compact call-boundary revision. The revised generator and C++ fault/continuation tests are building. The native sampler now has additive explicit chained-unwind ownership metadata; its fixture tests are pending, and existing profiles are not relabelled. The standalone dolview tool's stale pre-cleanup DOL path was repaired and its constant-byte dump checked against doldis.

Reverb contract discovery is pinned in reverb-contract.json: the 1,292-byte original kernel has three direct calls from the three-channel callback, which can first perform stereo crossfeed. Its scalar constants are 0.3f and 0.6f. Buffer allocation/reset/free, live pre-delay behavior and the native ownership contract remain to be recovered. The existing pre-delay cursor write targets line base+0x1B8, so an idealized reverb cannot be substituted without tracing the actual consequence. No audio implementation or gain is claimed from this static inventory.

The compact revision builds in the tests preset and passes all six selected gates (8.65 seconds). Both paths execute 16,416 copy overlap/observation/journal cases, 2,736 invalid-span refusals, and 684 prior-fault cuts. The independent original-word/caller checks still pass. All 19 PE/sampler fixture checks pass. Applying the new chain metadata to the preserved b0076bf8 diagnostic samples independently reproduces the exact reverb chain for Final Haunt (290 samples) and Westopolis (317); new unwind-attribution.json files explicitly identify this as postprocessing, not a new run. Source inputs now contain 965 files, manifest SHA-256 6c69f16a296fd7a9e6c99bc614838e7b5866ea740a63bfa16738aa7aafb6ef35. The compact release build is pending.


### Seventh increment: current release evidence

The compact release is 105,594,368 bytes (SHA-256 6a113ad8f3ee02d37007c25f52600058dcd25f66ed8d69bba6e139acd468a8fd), 70,144 bytes smaller than b0076bf8 and 3,846,144 bytes smaller than the original rebuilt control. Its matching map is 85,144,903 bytes, SHA-256 344da52461f54dd8346b8a09c47cc870413d7c57488c12840df264f8f356f200. All four translated math-body removal gates pass; these implement three native algorithms. Two alternating verification pairs per stage pass in Westopolis and Final Haunt against b0076bf8, with matching captures and reported state. These instrumented runs are not performance measurements. Fresh performance work is deferred for the user's graphics/texture/sound bug-fix priority; earlier timing gains remain explicitly attached to b0076bf8.

The first current-release Lost Impact diagnostic captures six actual gameplay frames and 21.45 seconds of raw stereo PCM. It reports no asset/texture import failures, render errors, dropped audio or underruns. All 22,809 perspective frustums use the displayed camera; no offset/camera fallback occurs. World-to-screen projection reports zero window mismatches. The opening-room movement route does not reproduce the reported edge disappearance or exercise a lock-on weapon (zero marks). No visual or audio bug is claimed fixed. The existing one-capture checker initially reports two count mismatches for this six-capture run; explicit sequence accounting with negative controls now checks six captures and 1,243 GPU frames = 1,237 presentations + six captures. The original result remains intact and the corrected scope is separate in presentation-bugs/lost-impact-before/sequence-validation.json.

### Presentation and resource investigation (current work)

The 6a113ad8 candidate completed 38 stage opening routes (22 simulated seconds,
frame-960 captures, raw PCM). The Last Way and Devil Doom did not run: these
IDs are not Select Mode stages accepted by --boot-stage. This is not 40-stage
certification. Lost Impact and Egg Dealer camera sweeps each completed nine
captures with no reported render/audio failures. The early-culling and target
layer symptoms in the user's unchanged bug notes remain unreproduced.

Lost Impact's raw PCM is identical across current, preceding b007 and rebuilt
a4 controls. The retained deployed and comparator-reference executables match
each other but differ from this source lineage, starting at stereo frame 49935.
That difference predates these optimizations; no listening or fix claim is made.

A separate unchanged-binary checkpoint reproduction confirms two differing
channel samples after load: only the first stereo frame changes (maximum
absolute sample delta 2660), then the remaining 10.01-second continuation is
exact. Both runs end at tick 891598050; the checkpoint is at tick 486193050.
The loader resets mixer/decoder history rather than saving it. This defect is
open; no checkpoint format or personal file has been changed. Evidence is in
.build/validation/presentation-bugs/checkpoint-before/.

Native GPU memory traces of Lost Impact and Final Haunt are in
.build/validation/resource-usage/before-*/. On these routes the upload stream
holds 16 MiB; the main multisampled colour/depth targets allocate 32/16 MiB,
and glare sources 16 MiB. These are measurements, not proposed quality cuts.
The trace distinguishes allocation sizes, allocator block slack, driver usage
and mapped system memory. Optimization and fresh paired measurements follow.


### Eighth increment: exact math and resource use (validation in progress)

The user's corrected target is at least 800 FPS at native 2560x1440 (1.25 ms per fresh rendered frame), with
unchanged game speed, intended graphics and sound. This is not achieved.
The current native presenter consumes one completed game packet per render;
`--render-rate` applies to the older presentation branch and cannot create
fresh state in the active lifted branch. Uncapped presentation and separate
GPU timestamp diagnostics are being measured on the i9-13900K / RTX 4080
at native 2560x1440. Fixed-step physics is explicitly required; no duplicated
frames or accelerated game clock count as success. Earlier 720p resource
diagnostics are not evidence for the corrected performance target.

The selected matrix change accumulates SIMD validity masks and makes one
final decision, preserving the same input/intermediate acceptance domain,
arithmetic order, scalar fallback and overlap semantics. Nine alternating
pairs on each of three fixed corpora give median kernel reductions of
4.08%, 5.58% and 6.40%; all 27 candidate pairs are faster and output digests
match. This is 4,194,304 operations per run, not a gameplay/FPS result.
The first bounded-input shortcut was rejected: despite improving ordinary
values, it forced a valid small-normal corpus onto a nearly four-times-slower
fallback. Its measurements remain visible; that restriction is not shipped.

Composition callers now share their existing cut/link/continuation boundary;
this reduces temporary R2 bookkeeping and is not a new emulation facade.
Resource reports no longer instantiate eight idle worker threads at shutdown.
Large screenshot readback buffers own their allocation so fence-complete
release can return the heap instead of caching an empty 8 MiB block.
This targets host/shared graphics memory, not texture quality or dedicated
VRAM reduction. D3D12MA nested JSON now strips its interior BOM before
embedding, so allocator evidence is valid JSON. Measurements and gameplay
gates for the final combined build are still pending.

Five focused native math/caller/generator/job gates pass. Both final build
presets succeed; the full CTest suite is running. Twenty Python checks pass
for stage workload accounting, resource summaries and frame-budget diagnostics.
The new diagnostics reject corrupt/incomplete samples and preserve outliers.
Evidence for this pass is in .build/validation/resource-usage/.


### Eighth increment: final evidence and corrected target

The final release is 105,577,984 bytes, SHA-256
6a4504d8d907c29b58a7c1f932488c498123a071d6016647ef18e06935823fee.
Its matching map is 85,132,825 bytes, SHA-256
22a78d3f9c943a05f49f514d2c38e0036a0e4f39317eb980416355e7fb6162c3.
The preceding 6a113ad8 release is 16,384 bytes larger: text -14,848,
read-only data -1,024, unwind data -512. Total reduction from the original
rebuilt control is 3,862,528 bytes (3.53%). All four old math-body removal
gates still pass. This increment does not remove another native algorithm.

Both build presets pass. Full CTest: 101/103 in 435.83 seconds; only the two
historical B45.mem1 fixture failures remain. The new readback capacity regression
passes on WARP with the debug layer requested. The expanded tool gate passes
21 Python cases. Separate clock drift and simulation-isolation proxy tests now
include 800 Hz and pass; these are not claimed as new gameplay physics tests.

Two alternating verification pairs per stage in Westopolis and Final Haunt
pass against 6a113ad8, including complete snapshot equality within each stage.
Lost Impact and Final Haunt 720p allocation diagnostics separately match PNG,
PCM and reported game state. Their retained readback blocks fall from 8 MiB
to zero after capture completion. Sampled peak zero does not mean no temporary
allocation: the one-second samples can miss the short capture lifetime. The
unit regression explicitly checks live allocation and full release. Job reports
now show one caller and zero queue capacity instead of creating eight unused
workers during reporting/shutdown. This is not eight fewer workers throughout
gameplay. The repaired allocator maps parse as ordinary JSON. Earlier before
runs used an ignored SHADOW_D3D12_DEBUG variable; after runs requested the real
SHADOW_R2_DEBUG_LAYER. Therefore their process RAM/CPU times are not compared.

The user corrected the goal to at least 800 FPS at native 2560x1440, with a
fixed timestep for physics. Fresh uninstrumented windowed/audio runs, VSync
off and unchanged game pacing, produce 1,277 successful fresh presentations
each in 22.047173 / 22.046111 seconds: 57.9213 / 57.9240 presentations per
wall second including loading. Median host return intervals are [16.6,16.7)
ms; p99 is [33.4,33.5) / [33.5,33.6) ms. The maximums are 610.5 / 610.9 ms.
All samples and loading stalls remain included. Game speeds are 0.998530 /
0.998578, with 60 Hz host fixed pacing. This does not establish 800 FPS.

Separate window-sized timestamp runs cover 1,276 actual GPU-rendered frames each.
Lost Impact direct-GPU mean/p99 is 5.2143/10.0526 ms; Final Haunt is
4.2241/8.6385 ms. 1,146 / 1,161 frames exceed the 1.25 ms GPU budget.
CPU record-to-submit mean is 0.2331 / 0.2333 ms, but excludes slot/swap-chain
waits and the entire gameplay thread. GPU timestamps exclude copy uploads
and scanout and can include scheduling/preemption. These instrumented
measurements identify work to investigate; their inverses are not player FPS
or a hardware throughput ceiling. Scene, glare and composition passes carry
material measured cost. The fresh-state production path also remains tied to
legacy game packets; changing a presentation-rate setting does not decouple it.

Real 2560x1440 captures from both stages were inspected. The GPU allocation
maps identify the RTX 4080; input tapes specify the same native viewport.
Render, asset and audio gates pass for these bounded routes, with no audio
underruns or dropped samples. The earlier reported culling/reticle symptoms
and reproduced first-stereo-frame checkpoint discontinuity remain unresolved.
No whole-game FPS, dedicated-VRAM reduction or complete native-port claim is
made. Final structured evidence: .build/validation/resource-usage/summary.json.


Final source-input manifest: 969 files, SHA-256
 d308a64324224dc13039bb7b27943560e015673d200ac88097e2a5b743a11f7c.
All 56 pinned personal files and both selected playable executables were
rechecked unchanged. No deployment occurred. Cleanup removed 163 verified
owned temporary files (1,415,441,035 bytes): old control binaries/maps, copied
run inputs and exact-equality validation snapshots. Their identities/results,
current builds, active source, actual captures and concise evidence remain.
The native-port and 800 FPS objective remains active and unmet.


### Ninth increment: shared pipeline cache and verified render dimensions

The retained renderer change removes the separate composition-pipeline map and
creation function. Final composition uses the existing cache entry for the same
point-copy shader and render state. Stored resolves and their colour rounding
remain. The shader comment now accurately describes UNORM input and destination
rounding. No gameplay-clock or physics code changed. This is native renderer
code cleanup, not another ported GameCube algorithm.

The final release is 105,576,960 bytes, 1,024 bytes smaller than the preceding
6a4504d8 release. Final runtime/capture results are recorded in
.build/validation/resolve-composition/. Eight selected CTests pass, including
renderer core, stage self-determinism, timing, packet ownership, native immediate
modes, architecture checks and the benchmark tools. The last full suite remains
101/103, with the two missing historical B45.mem1 fixtures still unresolved.

#### Rejected pass fusion: evidence retained, implementation removed

A tested candidate, SHA-256
53ffb891c54111a4961ebf7d61505e3e8df317414310f0d0d15e76711401d9a1,
computed the same precise ordered MSAA mean directly into final output, removing
an intermediate UNORM16 texture and its rounding. Its independent WARP and
RTX 4080 mean checks passed. Real 1440p captures differed by at most one UNORM8
code; reported game state and generated PCM remained exact and self-deterministic.
The conversion distinction follows [Direct3D rules, sections 3.2.3.5/6](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).

At explicitly forced 1440p it removed 30 MiB of allocated GPU texture memory,
but reserved DEFAULT heap size did not decrease. Three alternating timestamp
pairs per stage showed worse median total direct-GPU time: Lost Impact
3.89585 -> 4.33289 ms (+11.22%); Final Haunt 2.90210 -> 4.31383 ms (+48.65%).
Resolve-plus-composition became cheaper, but this does not override worse totals.
These are instrumented component intervals, not FPS or throughput ceilings.

Supplementary read-only NVIDIA clock diagnostics found candidate median memory
clocks at 810 MHz versus control 5,001 MHz in both pairs, consistent with a power
management effect. The logger left truncated/unflushed tails at termination;
complete-prefix samples and every malformed tail are recorded explicitly.
They do not prove causality, whole-run power savings, or an end-to-end speed gain.
No clock/power settings were changed and no timing was normalized by frequency.

The fusion was rejected, removed and rebuilt. Its dedicated temporary fixture
was removed with it; CMakeLists.txt and render_core_tests.cpp were restored
byte-for-byte to the preceding source manifest. All pre-existing regression
tests remain. The final build claims neither its 30 MiB allocation reduction
nor a speed improvement from it. One fused-candidate Final Haunt capture run
failed for an audio underrun of 55 output frames. A separate earlier control
trace failed for two underruns / 3,142 output frames. Both failures remain visible;
no passing rerun replaces them.

#### Measurement scope correction and permanent tool gate

The eighth-increment harness and initial ninth-increment cohort used
--native-output 2560x1440 and a 2560x1440 game-output tape, but omitted live
render dimensions. --native-output controls captures; presentation defaults
to the Windows client size. This desktop is 1920x1080 at 60 Hz. Their earlier
live FPS/GPU/memory measurements are NOT certified 1440p, and that attribution
is withdrawn. Screenshot files really are 2560x1440; image comparisons remain
valid. The initial 47 MiB allocation result belongs to that window-sized cohort.

Corrected native1440-* and final1440-* runs explicitly set --render-width 2560
--render-height 1440 and require the renderer's chosen-size announcement.
They render scene pixels at 1440p, then downsample into the desktop window;
that scale-pass cost is included. Physical 1440p scanout is not claimed.

The maintained frame_budget_report.py now records chosen dimensions and supports
--require-render-size 2560x1440. It rejects missing, different or changing live
sizes. Capture dimensions, game aspect and requested window size cannot satisfy
the gate. New regressions cover that exact mistake; the tool suite passes.
The fixed timestep requirement remains mandatory and the 800 FPS target remains
unmet. Known edge-culling, reticle and checkpoint audio-history bugs remain open.


Final candidate SHA-256: efdf02f20d15222344438358e736bb2018553f82e2c154a6e78f5704b88a4287.
Matching map SHA-256: b962fb0c386c022e480f89cfea732d7ffecf30965fcdb9cb01290b6772cbb57c.
Total reduction versus the original rebuilt control: 3,863,552 bytes (3.53028%).
All four final 22-second capture runs pass render/asset/audio gates. Twelve PNGs,
four generated PCM dumps and reported state match the earlier fixed-input
control runs exactly; each candidate route also repeats exactly. The comparisons
are correctness checks, not newly interleaved timing pairs or full snapshot proofs.

Separate final uninstrumented VSync-off runs, with explicit 1440p rendering and
normal game timing, each present 1,278 fresh frames. Lost Impact: 22.040934 s,
57.9830238 presentations/s including loading, game speed 0.998813. Final Haunt:
22.035383 s, 57.9976304 presentations/s, game speed 0.999064. Host pacing is
60 Hz. Both bounded runs pass render/asset/audio gates. One observation per stage
cannot establish a speed improvement; no 800 FPS achievement is claimed.

The final source manifest has 969 files, SHA-256
cc2c456640bee8025563788c56d0b1f9d9c5e7f30104163ae67830d30c4a4196. Only renderer.cpp,
renderer.h, utility.hlsl, frame_budget_report.py and its test differ from the
preceding source-input manifest. All 56 pinned personal files were rechecked
unchanged. Final evidence: .build/validation/resolve-composition/summary.json.

After validation, cleanup removed 271 verified temporary files
(1,143,379,023 bytes): the comparison-control binary/map, isolated
input copies, obsolete generated experiment headers/orchestration and exact
duplicate media. Fourteen distinct captures/audio files remain; cleanup-plan.json
maps removed duplicates to their retained identical file. No failed-run report,
active regression test, current build or personal file was removed. Final hashes
confirm both selected playable executables and all 56 personal files unchanged;
no deployment occurred. The native-port/800 FPS goal remains active.


## Tenth increment: preserve audio history across checkpoints

The current release is 105,581,568 bytes, SHA-256
8bca38b9d112b3cf2cc3c4709fdfb51ca17d0b8b9f814dedff93262a6c54b7c1.
This correctness change adds 4,608 bytes over efdf02f2; the net reduction from
the original rebuilt control is 3,858,944 bytes (3.5261%). No performance gain,
GPU-memory reduction or new native reverb replacement is claimed.

The load path used to discard all host decoder/mixer history. Source positions
alone cannot reconstruct fractional phase, the ADPCM predictor and decoded
sample ring, SRC taps, compressor release or delayed auxiliary returns. The
owning mixer now exposes a plain-value history record. System serializes its
persistent decoder state in audio_state.cpp, outside the per-frame path. Only
readable ring samples are stored; temporary source windows, working mix buses,
list traversal and diagnostic counters are rebuilt. The compressor table cache
is invalidated on reset/load. Three write-only fields (published-head copies
and an obsolete ADPCM remainder array) were removed.

The v20 file appends a length-checked history block after the renderer. It
rejects truncated records, invalid fractions/cursor flags/compressor state and
incompatible diagnostic decoder modes. The v19 prefix remains readable, with
an explicit warning about missing history. No personal checkpoint was rewritten.
The state-difference tool accepts the unchanged v20 CPU layout and still refuses
to silently compare unequal-length serialized blocks.

Pinned efdf02f2 controls and this candidate ran cold for 22 game seconds, saved
at tick 486193050, and independently restored through tick 891598050. Each
restored output contains 320,320 stereo frames (10.01 seconds):

| Stage | Control differing channel samples | Candidate | Largest control error |
| --- | ---: | ---: | ---: |
| Lost Impact | 2 | 0 | 2,660 |
| Final Haunt | 141,405 | 0 | 886 |

Uninterrupted PCM is exactly equal across builds. The first control difference
is restored stereo frame 320, after the already queued samples; this refines
the earlier description of the first-frame discontinuity. The audio blocks
add 3,159 and 61,287 bytes to these two checkpoints. A candidate loading the
control's v19 file reproduces the control's old PCM exactly and emits the warning.

Eight selected CTests passed: core, host timing, audio runtime, mixer, AX PB,
benchmark tools, lift purity and scaffold inventory. Nine PCM8/PCM16/ADPCM
checkpoint cases compare 16 subsequent frames in reused and fresh Systems,
including fractional ratios, loops, mute/unmute and reordered voices. Seven
cold-reset controls change PCM. Another 100 mixer continuations cover SRC,
compressor release and delayed returns; 99 cold-reset controls differ. Five
Python state-difference tests pass. Final test error-path cleanup was rebuilt
and the checkpoint regression rerun. Four native-math production absence gates
still pass. The full suite was not rerun; the two B45.mem1 failures remain open.

Six cold rendered captures at 2560x1440 match efdf02f2 exactly. Generated PCM
also matches; the entire reported-state difference is savestate_version 19 ->
20. Rendered restores produce exactly the same PCM as the headless continuations,
with no reported audio underrun/drop/device error or render failure. Corrected
restore runs confirm chosen 2560x1440 rendering. Output is scaled to the host's
1920x1080 desktop; no physical 1440p scanout or host-device startup-latency parity
is claimed. The audio was compared numerically, not certified by listening.

All failed diagnostics remain explicit. An initial --vsync off invocation was
rejected before startup; the correct switch is --no-vsync. The first rendered
restores chose 2611x1440 to preserve the live 1924x1061 output aspect and failed
the strict 2560x1440 evidence gate. Corrected runs use a neutral output-only tape
to hold 16:9; tape-start and window-size warnings are retained. Neither those
runs nor the successful correctness captures are FPS benchmarks. Dropped render
packets during restored resource startup are reported in the raw logs.

Evidence: .build/validation/audio-checkpoint/summary.json, final-size.json,
selected-tests.log, final-checkpoint-test.log and per-run invocation/session/log
files. Cleanup aliases identify removed duplicate media. All 56 pinned personal
files and the deployed/reference executables are unchanged; no deployment.

Reverb lifecycle discovery advanced independently: the original initializer
allocates 18 delay lines plus three optional pre-delay lines, uses registered
allocator/free hooks, unwinds partial allocation failure, and registers the
callback with auxiliary A/B. Hashes and exact call sites are in
.build/optimization/reverb-contract.json. Live parameter consumers, callback
shutdown ordering and the pre-delay defect still need proof before replacing
that owner. Its hand-specialized console kernel remains. The native ownership
objective and 800 FPS target remain active; fixed-step gameplay is unchanged,
and edge-culling/reticle bugs remain unresolved.

Final cleanup removed 92 disposable files (603,994,345 bytes), including the
temporary control executable, generated checkpoints, copied persistence and
byte-identical media. The two current restored-gameplay captures and distinct
PCM evidence remain. final-integrity.json verifies 970 source inputs, all 56
personal files, the current EXE/map, selected playable pair and every retained
canonical media alias. Source manifest SHA-256:
fd6f75275089dcf8b361fd9bcd49f6fe5fadaa8ff203e430cba18c9ef5f1d839.

## Eleventh increment: original reverb contract and missing effect return

This increment adds development-only validation; the production candidate is
still 105,581,568 bytes, SHA-256
8bca38b9d112b3cf2cc3c4709fdfb51ca17d0b8b9f814dedff93262a6c54b7c1.
No speed, size or audible-sound fix is claimed. Fixed 60 Hz host pacing remains
unchanged, and host_timing_tests passes. Its render-rate invariance checks use
synthetic state; they do not certify all game physics/collision contracts.

Source inspection found an incomplete effect path: Mixer::mix accumulates
auxiliary sends but never reads FrameSettings::aux_a/aux_b, and run_ax_mix
publishes only the stereo result. The translated reverb callback executes
without that send/return connection. Removing it now would optimize an
incomplete sound path. Native migration must restore the full intended effect
and own its lifetime before removing the obsolete callback and buffers.

Two isolated 22-game-second headless diagnostics, Lost Impact and Final Haunt,
used the nonshipping tests executable with legacy capture, not the production
lifted renderer. Both initialize reverb once and execute its callback 4,281
times; neither reaches shutdown. Both use mode 0 and the same authored values:
color 0.5, level 1, time 1, damping 0.8, pre-delay 0.01 and crossfeed 0.3. Their
voice snapshots record 665 and 7,445 nonzero auxiliary-volume observations;
these are not nonzero-output measurements. All auxiliary A buffers are zero
in the terminal snapshots. Entry probes and terminal snapshots do not prove
all parameter writers, the entire lifetime or device-delivered audio.

The maintained axfx_reverb_contract_tests executes the original pinned DOL
through the reference instruction interpreter, independently of the generated
reference-body fixture. It verifies 21 allocation-failure positions, release
twice, 12 invalid finite parameter cases, and 256 complete three-channel
callbacks against the current R18a kernel, including delay wraps, tails,
integer conversion extremes and bypass. Allocation guards and all retained
sample/delay bytes agree. It observes 1,920 pre-delay samples across calls and
channels: despite allocating 320 floats per channel, the original uses a
one-sample delay. It reloads the unchanged state cursor every sample and writes
the incremented cursor at line-base+0x1B8. A test-only moving-cursor negative
control differs at 159 of 160 observed samples. This proves the bounded
initialized case; it does not permit arbitrary aliasing or corrupt state.

The original auxiliary initializer, registration, list builder and callback
processor establish three rotating slots. Marker exchanges cover all three
output modes, 45 callbacks and 35 returns after two DSP frames, plus disable.
The original frame owner's submission-before-processing/build-after-processing
order is pinned by the DOL. This is a buffer protocol test, not DSP arithmetic,
cache-coherency or end-to-end acoustic latency. The isolated fixture validates
and returns from cache-maintenance calls because no OS syscall vector/DMA
cache exists there. The initial failure at that boundary is retained. The
reference interpreter and R18a share low-level floating-point helpers, so this
is independent instruction/control-flow evidence, not a silicon oracle.

The new oracle, the existing 3,000-case reverb regression, host timing, AX PB
checkpoint continuation and liveness-root checks pass. A stale root check from
the preceding checkpoint addition exposed its extra data literal 0x80011000.
That test now reuses the existing separated PB fixture locations 0x80010000 and
0x80020000; all nine continuation cases still pass. No reachability rule or
assertion was weakened, and extract/liveness-roots.txt is unchanged. The full
104-test suite was not rerun; the two historical B45.mem1 failures remain open.

Evidence: .build/validation/reverb-native/live-contract.json, summary.json,
per-route invocation/probe/session files, build/CTest logs and
.build/optimization/reverb-contract.json. The next required work is all game
parameter writers and shutdown ordering, an owned native effect/voice path,
original numerical and output-composition comparisons, checkpoint continuity,
real audio delivery and linked legacy removal. The 800 FPS target is unmet.

## Twelfth increment: owned native reverb values

A new runtime/lift/audio/reverb component holds its signal history in one
20,296-float array with compact cursors, feedback and filter values. It accepts
three ordinary signed-sample arrays, any block length and explicit history
restore/reset. It has no CPU state, guest addresses, callbacks, allocation hooks
or device protocol. Configuration and processing allocate no dynamic memory.
This is the numerical component of the pending owner migration: it is not
connected to gameplay, and no translated effect code has been removed yet.

The full original-DOL callback now agrees with this component for 768 blocks
(160 samples on each of three channels): six configurations, both floating
modes, full-range integer inputs, tails, wraps and split 7+153 processing.
All future-readable delay samples, feedback, low-pass and pre-delay history
match bit for bit. There are 480 post-restore blocks. Another 32 blocks start
from seeded signed-zero/subnormal boundary history, with nontrivial ring
positions. Fifty-three invalid configure/restore operations leave the existing
history and subsequent output intact; reset clears all signal history.

The first implementation failed at an integer conversion boundary. Positive
2^31 must become INT32_MIN in the authored flush mode, whereas standard mode
saturates positive overflow. The first differing stored delay value was
0xCF000000 versus 0x4F000000, frame 0, channel 0, comb 0, pending sample 1630.
The conversion was fixed; no expected values or gates were relaxed. Failed
logs ctest-owned-initial.log and ctest-owned-detail.log remain. An intermediate
edit stopped before writing because its newline anchor was wrong; its following
run covered the previous 512-block version only. That run was replaced by the
actual expanded validation. Existing source line endings are preserved.

The final implementation uses direct x64 FMA where the configured host ISA
supports it, with a standard-library fallback. Zero/subnormal rounded results
avoid unnecessary exact-residual work; only a result at the normal boundary
needs it. All comparisons still pass. This is not a measured speedup. Original
instructions and native code have distinct control/data implementations, but
the interpreter remains a software numeric oracle, not independent hardware.

Static ownership discovery also identifies constructor and tuning parameter
writers, two direct enable callers passing object+0x28, and the destructor's
conditional disable before resource teardown. The destructor skips disable
when that flag is clear. Its flag invariant, every parameter/flag writer,
indirect calls and live shutdown still require proof. Seven original instruction
ranges plus both enable call sites are pinned in the contract report; this
bounded discovery does not certify the whole lifecycle.

Six selected tests pass: the expanded contract, existing 3,000-case reverb,
host timing, lift purity, scaffold inventory and liveness roots. The expanded
contract executes 48,875,597 original instructions. Fixed 60 Hz simulation
pacing and its render-rate invariance checks remain unchanged. The full suite
was not rerun; the two missing B45.mem1 fixture failures remain open.

The production candidate remains 105,581,568 bytes, SHA-256
8bca38b9d112b3cf2cc3c4709fdfb51ca17d0b8b9f814dedff93262a6c54b7c1.
No new game executable, gameplay capture, device-audio result, resource saving
or FPS measurement was produced. The 800 FPS target at native 2560x1440 is
unmet. Next is the actual owning voice/effect output path, preserving full
mix composition and checkpoint continuity before deleting the obsolete code.
Evidence: .build/validation/reverb-native/summary.json, ctest-owned-final.log,
build-owned-final.log and .build/optimization/reverb-contract.json. The deployed
pair and all 56 pinned personal files are checked unchanged; no deployment.

## Thirteenth increment: native surround output and checkpoint history

The active mixer previously discarded main surround in stereo output. A pure
OutputStage now retains the preceding 160-sample block, initializes stereo
left/right from it (or negative/positive for opposite output), then accumulates
voices. It also owns the existing matrix history. No new device emulation or
guest-address wrapper was added. Audio blocks stay 5 ms at 32 kHz, and game
pacing remains fixed at 60 Hz. This is a correctness prerequisite for the
pending effect-owner migration, not a measured performance gain.

A bounded test-only interpreter executes the original output words from the
pinned DOL. It covers 200 surround blocks, 64 three-channel exchanges, 32 matrix
blocks and 32 four-channel exchanges, totaling 536,720 instructions. Values
remain in signed 24-bit mix headroom. Synchronous fixture transfers do not prove
hardware/cache/timing behavior; semantics use Dolphin's primary ISA sources.
Matrix arithmetic is covered, but its original buffer delay still needs a
dedicated ownership check. docs/AUDIO_OUTPUT_CONTRACT.md records these limits.

The initial integrated run passed five of seven checks. Its independent mixer
model omitted the newly recovered surround (343/400 mode-0 frames differed),
and the reference-only header lacked the required liveness annotation. The
model now includes the original-word-proven term; no assertion was removed.
The header received the existing checked test-only declaration; the root list
is unchanged. The first failed log is retained. The corrected seven checks pass,
plus rebuilt core/checkpoint and both reverb regressions: ten selected CTests.
Five Python state-delta tests also pass. The full suite was not rerun; the two
historical missing B45.mem1 failures remain open.

Version 21 appends 640 bytes of surround history. Nine whole-system stream
cases restore into reused and fresh owners; 100 mixer continuations remain
exact, and two nonzero surround-only cases expose missing-history controls.
Version 20 loads with an explicit missing-surround warning; version 19 retains
its decoder/mixer warning. Malformed and truncated records are still rejected.
Real Lost Impact and Final Haunt runs each continue for 10.01 seconds with
byte-identical generated PCM after a v21 restore; real v20 control files also
load. Both cold routes match the previous release's PCM and their checkpoint
surround values are zero. No audible improvement is claimed from these routes.

Rendered restores use native 2560x1440 scene rendering, audio playback and the
D3D12 debug layer. Each capture matches the preceding release exactly, and
rendered PCM equals the numerical restored run. Reported audio underruns,
open/render errors and renderer failures are zero. There were 570/567 presented
frames and 30/33 dropped packets over about ten seconds, with capture/startup
stalls retained. These are instrumented correctness runs, not FPS measurements;
the scene is scaled to the 1920x1080 desktop. The output-only synthetic tapes
now name the actual restore tick. The window/output-size warning remains
explicit. Graphics edge-culling and reticle issues are still unresolved.

The actual new release is 105,585,152 bytes, SHA-256
dba31b08a712942e0e868cf792c227c1e62ee98760bc6f009ffb4218dc2ff5b8.
It adds 3,584 bytes to the previous release, while remaining 3,855,360 bytes
(3.52%) below the original rebuilt control. The PE audit was refreshed. No
translated effect body was removed; the pure Reverb still lacks gameplay
ownership/output integration. No CPU/RAM/VRAM or FPS gain is asserted, and the
800 FPS target remains unmet. Fixed-step timing tests pass, without claiming
that every gameplay physics subsystem has been certified.

Evidence: .build/validation/reverb-native/output-summary.json,
output-source-inputs.json (977 inputs), ctest-output-corrected.log,
ctest-output-regressions.log, build-output-release.log and per-run invocation,
session and stdout/stderr files. The deployed pair and all 56 pinned personal
files are verified unchanged. Temporary control binaries, copied persistence,
PCM and checkpoints are removed after their hashes/results are recorded; the
two current screenshots and concise evidence remain. No deployment occurred.

## Fourteenth increment: matrix-history ownership across output changes

Original game-level tests now execute repeated enable/disable controls and two
destructor paths. Four enables/four disables preserve effect history; the two
destructors unregister before voice-stop/resource teardown/platform shutdown.
Those external operations are explicitly stubbed. An inconsistent game active
flag leaves a registered callback at teardown, so lifecycle closure is still
conditional. This is not proof of all callers, aliases or shutdown interleavings.

The original auxiliary processor establishes two different timing contracts:
three effect slots return after two blocks, while the matrix L/R channels retain
two blocks and advance their phase every audio block, even outside matrix mode.
The composed test runs original PPC list-builder/processor/reverb words together
with original DSP output instructions. Across 192 blocks (three output modes,
two floating modes), native effect history and output match. Removing the return
changes 39,374 channel samples; 717,120 output instructions execute. This supplied
mixed-bus fixture excludes voice decoding, compressor, device timing and dynamic
effect controls. The complete reverb contract executes 58,046,877 PPC instructions.

The mode-transition sequence found a real production-stage bug. At block three
of modes 2,2,0,2, sample zero was left 2100/right 1800 instead of 1100/800. The
native stage had retained only the latest matrix block and frozen it outside
matrix mode. The fix owns both blocks and advances a boolean phase every block;
inactive modes require no matrix-buffer copy. All 18 original-code transitions
now match, including history restore every third block. The initial failure is
preserved in ctest-owner-transitions-initial.log; the expected result was not
relaxed to pass the test.

Version 22 adds the second 1280-byte block and one-byte phase to checkpoints.
Versions 20/21 load with an explicit limitation warning and treat their one block
as the next return; version 20 retains the missing-surround warning. Malformed
phase bytes are rejected. Whole-file tests add two distinct nonzero history
cases covering both phases, with fresh/reused owners and 16 exact continuation
blocks each. Missing-bank and wrong-phase controls each change two PCM blocks.
The preceding nine decoder/SRC/surround checkpoint cases still pass.

The release builds and ten unique selected CTests pass: audio_output_contract_tests,
axfx_reverb_contract_tests, native_audio_mixer_tests, ax_pb_tests, gekko_core_tests,
host_timing_tests, native_axfx_reverb_tests, lift_purity, lift_scaffold_inventory
and native_liveness_roots. Five state-delta Python tests pass. The full suite was
not rerun; the prior 101/103 result still has two missing historical B45.mem1
fixtures, and two contract tests have since been added. No full-game claim follows.

Release: 105,585,664 bytes, SHA-256
3029186573394a51449150d136f38567c5567af75d70e4fb6bd20951defffd3d.
It grows 512 bytes from the preceding dba31b08 release. The total reduction from
the original 109,440,512-byte rebuilt baseline is 3,854,848 bytes (3.52232%).
No translated effect body is removed, and no speed or memory saving is claimed.
The source manifest covers 977 unchanged-after-build inputs. The PE/linker audit
is refreshed; output filenames alone are not treated as evidence of native code.

Real-game validation repeats the isolated Lost Impact and Final Haunt routes:
control/candidate cold runs, v22 restore, v21 load and rendered v22 restore. Cold
PCM is unchanged. Each restore reproduces 1,281,280 bytes of continuous PCM
(320,320 stereo sample frames, 10.01 seconds), with matching final game ticks.
Both stages use mode 0; checkpoint surround and both matrix banks are zero.
Thus these routes prove continuation and unchanged output, not an audible
mode-switch improvement. That correction is proven by the nonzero regressions.

The two new native 2560x1440 captures match the preceding release exactly and
were visually inspected. Audio and D3D12 validation are enabled, with no reported
underruns, open/render errors or device invalidations. The renderer presents
569/567 packets and drops 31/33 of 600, with zero render failures. Live render-size
announcements and output-tape events confirm 1440p rendering, scaled into this
1080p desktop. These instrumented ten-second runs are not throughput benchmarks.
Fixed 60 Hz pacing is preserved; not every physics contract is certified, and
the 800 FPS target remains unmet. Existing culling/reticle issues remain open.

Evidence is in .build/validation/reverb-native/owner-summary.json, with exact
per-run invocations, hashes, logs and screenshots. A report-collector field error
was corrected to read the renderer's actual stdout counters; completed runs were
retained after matching arguments and identities. No failed gameplay run was
silently discarded. The deployed/reference pair and all 56 pinned personal files
were verified unchanged. Temporary binary, checkpoint, PCM and persistence copies
are removed after comparison; concise evidence and active regression tests stay.

Next work: prove remaining dynamic effect controls, parameter writers and lifetime
conditions; replace the owning voice/effect path and connect its complete signal
composition before deleting translated callbacks. Do not extend the existing
DSP scaffold with a permanent guest-address wrapper. The native Reverb is still
unconnected to gameplay, and the overall optimization objective remains active.

## Fifteenth increment: native pending effect ownership (2026-10-24)

The new runtime/lift/audio/effect_stage is an owned native signal component:
room effect, pending room samples, spatial-right delay, surround/matrix history,
and scheduled/current controls. It takes plain sample arrays, owns all retained
values and exposes validated reset/control/history operations. Its render path
allocates no dynamic storage and contains no device, address-window, callback
table or command-list mechanism. Voice resources/decoding, final compression/PCM
and device delivery are still separate owners to migrate. It is not yet connected
to gameplay, and its history is not silently added to current game checkpoints.

Original-code recovery established that previously scheduled send/return work
survives a later control change, while callback processing uses current enable
state. Enable/disable preserves pending samples and freezes the effect tail.
The new dynamic oracle executes original PPC controls, list builder and callback
plus original DSP output instructions. It supplies mixed voice buses and covers
360 blocks across all three output modes and both floating modes: 204 callbacks,
66 control changes after scheduling, and 30 native history restores. Every output
channel and future-readable effect delay sample matches. Clearing pending data
on disable changes 48,152 channel samples. The dynamic test executes 989,700
output instructions; the full contract now executes 67,859,789 PPC instructions.
The original numerical fixture remains bounded to the documented mix headroom;
this is software reference evidence, not hardware or device-latency certification.

Game controls were strengthened independently. The previously stubbed control
entry 0x804402D4 now executes intact, including its exclusion/update path. It
ignores incoming r3, brackets the update with an exclusion-depth increment and
restores the depth; a nested update records contention and skips voice work.
Bank-selection tests cover 80 cases with 64 explicit asset-load boundaries and
two stop-all cases. They execute selector/virtual dispatch/control code intact;
asset I/O is a fixture. Bank selection always re-enables reverb, even after load
failure or a missing/out-of-range selector, without resetting the effect. The
original inconsistent-active-flag teardown counterexample remains; this is not
closure over every alias, parameter writer, construction failure or lifetime.

The complete native stage replaces the temporary test-only scheduling model.
Eight rejected operations (invalid mode/reset/phase/control/history values)
leave future output intact. Valid reset matches a fresh owner for 32 blocks.
Six selected CTests pass: the original reverb/control contract, original output
arithmetic, fixed-step host timing, native purity, scaffold inventory and liveness
roots. Both test and release builds succeed. The full suite was not rerun; its
prior 101/103 result retains two missing B45.mem1 fixture failures, with two
contract tests added afterward. No test or assertion was weakened.

Current executable: 105,585,664 bytes, SHA-256
5558b31ab0cdc2bf23b0c34dda2138fb4957632edec9414f606d447144b086a8.
There is no size delta in this increment. The rebuilt file is not byte-identical
to release 30291865: exactly four bytes differ, two in the COFF timestamp and two
in the debug-directory timestamp. All other bytes, including the entire .text,
are identical. Every changed offset/value is retained in dynamic-summary.json;
none was discarded from the comparison. The new stage has zero production-map
entries, so this work is not claimed as a runtime optimization. The overall
reduction from the original rebuilt baseline remains 3,854,848 bytes (3.52232%).

No new gameplay or screenshot run is claimed for this increment. Prior PCM,
1440p captures and device-audio checks remain the fourteenth increment's evidence;
the full byte comparison shows unchanged production logic/data apart from the
timestamps. The 979-input source manifest and actual PE/linker audit are refreshed.
The deployed/reference pair and all 56 pinned personal files remain unchanged.
The temporary comparison executable is disposable after this exact comparison.

Evidence: .build/validation/reverb-native/dynamic-summary.json,
dynamic-source-inputs.json, build-dynamic-release.log and
ctest-native-effect-stage-final.log, plus the updated reverb-contract.json. The
new component is active migration work, not a replacement for the still-missing
gameplay connection. Next work must migrate native voice/resource ownership and
direct game controls, integrate the complete signal path, then remove translated
callbacks/allocators with proven caller/lifetime closure. Adding permanent effect
returns to the command-list scaffold would not satisfy that requirement. Fixed
60 Hz simulation pacing stays intact; full physics certification and 800 FPS at
native 1440p remain unmet. The overall objective remains active.

## Sixteenth increment: vector envelope and bus gains

The active mixer's signed Q15 envelope and unsigned Q15 bus gains now operate
on eight independent samples with AVX2. Phase/interpolation arithmetic remains
unchanged. Gain ramps preserve their low-16-bit wrap; envelope samples saturate
to signed 16-bit and bus additions saturate to signed 32-bit, including extreme
restored histories. Products fit signed 32-bit by range proof. No voice decode,
effect, sound channel or timing work was omitted. This optimizes existing
scaffolding; it does not replace the voice/resource owner or connect EffectStage.

The first attempt merely narrowed scalar products. It passed arithmetic checks
but was 8.9-12.2% slower in seven alternating pairs; that implementation was
replaced. The retained vector version passes seven selected CTests, including
2,230,400 output samples plus source history against the existing wide model,
extreme restored bus saturation, 800 randomized mixer blocks, compressor and
checkpoint continuation tests, fixed host timing and source-boundary checks.
The existing interpolation-rounding model still has its documented single-source
limitation; these tests are not a new complete original-DSP oracle.

On the i9-13900K, seven alternating control/candidate pairs each process 12,000
blocks at 1/16/64 voices. All output hashes match. Median times in milliseconds:
24.8562 -> 13.0059; 213.5341 -> 96.8201; 867.5259 -> 401.8257. Reductions are
47.7%, 54.7% and 53.7%. All pairs are retained. This isolated CPU workload includes
output hashing and excludes device/rendering work; it is not gameplay FPS,
whole-process CPU savings, RAM/VRAM savings or evidence of reaching 800 FPS.

Release build: 105,586,176 bytes, SHA-256
f1d8d43211207b3749a4e53b8cb9c22977b95b5138809e3844cd604a74568c02.
The extra vector code costs 512 executable bytes. Overall the candidate is
3,854,336 bytes (3.52%) smaller than the initial rebuilt control. Only
native_audio_mixer.cpp and native_audio_mixer_tests.cpp changed among the 979
build inputs. The exact size/map report, matching source manifest, seven CTest
results, all rejected/accepted kernel measurements and hardware identities are
in .build/validation/audio-width/.

Four cold and ten restored native-1440p diagnostics cover Lost Impact and Final
Haunt. Both binaries produce identical complete checkpoints, cold/restored PCM
and final memory reports. All ten windowed/audio runs have 600 pacing boundaries
at 60 Hz, zero skips over 10.01 game seconds, and zero reported underruns or
render failures. Initial candidate runs presented 569/567 of 600 submitted
packets; the 31/33 dropped packets are retained in evidence. Debug-layer/capture
runs are correctness checks, not performance measurements. The native 1440p
scene is scaled into the 1080p desktop. These neutral routes do not certify all
physics, collisions, sounds or stages.

Lost Impact images are exact. Final Haunt's initial images differed at three
pixels by one RGB level. Predetermined repeat runs reproduced the same two
pixel patterns in both binaries (five control and three candidate runs total).
PCM and final state reports remain exact across all repeats. The variation is
therefore present in the unchanged control; its rendering cause is still
unlocated. No image bytes were excluded, and pixel self-determinism is not
claimed. Exact coordinates/values, all run identities and both patterns remain
in capture-variability.json and the retained images. No new visual bug fix is
claimed.

On 2026-10-24 the user corrected the historical bug status: Bugs.txt described
issues fixed long ago and should be deleted. game/Bugs.txt (298 bytes) was
removed. Earlier paragraphs reporting edge-culling/reticle work as pending are
historical and superseded by that correction. This is separate from the current
three-pixel capture finding. All 56 pinned personal files and the selected
executable pair were verified unchanged before the user's requested Westopolis
launch. The latest candidate was launched fullscreen with audio and the full
card; it is user-owned. No deployment or further heavy validation while playing.
The overall native-ownership and 800 FPS objective remains active.


## Seventeenth increment: single-period waiting (2026-10-24)

The user's requested removal of the 60-to-30 fallback prompted a DOL and
runtime audit. The positive interval fallback was already suppressed: the
recording build ignored 972 requests for multiple periods during Westopolis.
The original limiter body remains pruned. This increment removes the leftover
interval multiplication, generic multi-period deadline, unreachable long-wait
branch and clamp helper. Zero reset/disc-error mode, loading progress, movie
waits, elapsed-time math and fixed host pacing retain their behavior. This is
cleanup of an existing adapter, not a completed native timing migration or a
new 30-to-60 FPS improvement. Contract: docs/FRAME_PACING.md.

Release size is 105,584,640 bytes, SHA-256
ec7bde88bf1a98f80ecf9761513ad6662db001cbc904f5301f838e8fd0d29261.
The executable loses 1,536 bytes: 1,024 file bytes of .text and 512 of .rdata.
Virtual .text falls by 1,008 bytes. Generated game code remains 84,397,272
attributed bytes. Three build inputs changed (limiter header, implementation,
core regression source). Five selected CTests pass, including 9,600 frames
through the actual installed limiter, interrupted waits, zero/loading paths
and reset cancellation. Full-suite fixture limitations remain as above.

Both builds replayed all 4,968 recorded input changes from cold boot with zero
late entries, ending at the same 315.391183 game seconds. Full checkpoints at
120.012667 seconds, complete PCM, final memory reports and final captures are
byte-identical. That final capture is a save prompt, not active gameplay.
Both arms then resumed the same route for 30.013317 game seconds at native
2560x1440 with audio and D3D12 validation requested. The first pair mistakenly
asked for capture frame 8500 after the restored renderer reset its counter;
it had zero captures and failed that gate. Both were rerun at frame 1200.
The corrected gameplay captures match byte-for-byte; all four rendered runs
also have identical PCM and final memory reports, 1,800 fixed pacing
boundaries, zero skips, audio underruns, open failures or render failures.

Corrected runs submit 1,799 game packets and present 1,774 control / 1,773
candidate, dropping 25/26 while the diagnostic capture blocks. Those drops
remain in the evidence. Median present intervals are 16.7 ms in both; p99 and
maximum are not improved. No speed or thermal improvement is claimed from
instrumented runs. Each restored tape reports 2,127 old entries while seeking
the known checkpoint. The picture follows recorded 1920x1080 output and is
scaled into a 1920x1061 client; native scene and capture remain 2560x1440.

Evidence: .build/validation/frame-pacing/summary.json and all six invocations.
The original user recording, input tape and initial persistence remain intact.
No cooler-temperature marker was supplied; attribution of the temperature
spike remains open. The lifted CSV renderer fields still need correction.
All 58 protected personal/deployed-file fingerprints match; no deployment.
The 800 FPS target remains unmet. Real presentation stalls and native
voice/resource ownership remain work for subsequent increments.


## Eighteenth increment: measured presentation reporting (2026-10-24)

The live lifted renderer bypassed the old telemetry collectors: the runtime
CSV mislabeled its backend and showed zero frame/GPU fields, the per-frame CSV
had no rows, and final JSON claimed no presentation. This increment takes
measurements at the actual render owner, reports each attempted frame and
separates packet waiting, diagnostic capture and CPU-side render/present wall
time. Optional observers use a short timing lock; queue and renderer resources
retain their existing locks. Empty waits create no frame rows. GPU execution
and legacy presentation statistics are explicitly unavailable. The launcher
uses this schema and no longer shows the old renderer's zero measurements.

Release and nonlifted configurations build; seven selected CTests pass. Two
new tool tests contain 14 cases covering all-frame accounting, bad timestamps,
missing/invented measurements, overflow outliers, render failures, game-time
rewinds, rate/counter contradictions and launcher formatting. Historical
missing B45.mem1 fixtures remain unresolved; the full suite was not rerun.
Current CTest inventory is 107.

The candidate is 105,581,568 bytes, SHA-256
8608faaa2874b9fd7817715bc1e8397693c68a83dbbbd6ea1fc708aaf6213d6a:
3,072 bytes smaller after unused legacy reporting paths were excluded. .text
file size falls by 3,072 and virtual size by 3,264 bytes. Attributed generated
game code stays 84,397,272 bytes. This is a measurement correction and code
cleanup, not a new native gameplay replacement or a speedup claim.

Both arms replayed the first 121.013667 game seconds of the user recording
from cold boot. Checkpoints at 120.012667 seconds, PCM and final memory reports
are byte-identical. Headless JSON correctly has no presentation average or
quantiles. Both restored arms then ran another 30.013317 game seconds with
native 2560x1440 scenes/captures, audio and D3D12 validation requested. PCM,
final memory reports and gameplay captures match byte-for-byte. Both completed
1,800 fixed pacing boundaries with no skips, render failures, audio underruns
or device errors. Only execution wall time and its derived speed differ in
guest execution counters; conformance reports match in full.

Each rendered run produced 1,799 packets, presented 1,774 and dropped 25.
The candidate's 1,774 CSV rows reconcile exactly against the final JSON and
29 interval rows. All 1,773 return intervals remain in the distribution:
average 59.218887 returns/s, median 16.6373 ms, p95 29.0281 ms, p99 35.0224 ms,
maximum 430.4687 ms. That maximum contains 425.9472 ms of synchronous capture.
Total packet waiting is 26.089624 s and render/present wall time 3.443533 s;
these include waiting and are not GPU execution or process CPU measurements.
The control histogram has p95 29.0 / p99 34.9 ms, so similar scheduling
variation exists in both. No samples, capture stalls or drops are excluded.

These diagnostics are instrumented and do not establish observer overhead,
whole-game speed or CPU temperature improvement. The 800 FPS target is unmet;
fresh packet production still follows the approximately 60 Hz game loop and
the lifted path does not apply the legacy render-rate option. The recorded
cooler temperature spike remains unattributed. Restores seek the same 2,127
old tape entries; the scene follows the recorded 1920x1080 game output while
the physical window client is 1920x1061. This increment checks a partial route;
full 315-second route parity remains evidence of the preceding increment.

Evidence: .build/validation/presentation-telemetry/summary.json and four exact
invocations; schema contract: docs/PRESENTATION_TELEMETRY.md. The capture is
identical to the retained frame-pacing/candidate-rendered-capture/frame.png,
so duplicate image files can be removed. Temporary executable, checkpoint,
PCM and persistence copies are discarded after hashing. All 58 protected
personal/deployed files and the original tape remain unchanged; no deployment.
Native frame production and voice/resource ownership remain active work.


## Nineteenth increment in progress: native runtime type queries (2026-10-24)

The refreshed release profile locates 215 of 5,241 game-thread wall-time samples
inside the original class-cast body, the highest sampled translated body. The
native replacement algorithm uses interned identities, immutable descriptor
spans and host-sized offsets. It passes 15,603 synthetic and 11,880 actual-DOL
metadata queries against 4,894,403 original instructions; three selected CTests
pass. No per-call allocation or string comparison is needed in the native API.

Production integration is pending. The static census finds 268 external direct
calls, no external conditional edges, 13 nearby reference-cast requests and an
exception-index record. First-entry development observations only show pointer
casts; they cannot justify deleting the reference-failure path. Metadata
ownership/lifetime, caller scratch state, actual exception unwinding and linked
body retirement must be proved before acceptance. The release remains
105,581,568 bytes, SHA-256
8608faaa2874b9fd7817715bc1e8397693c68a83dbbbd6ea1fc708aaf6213d6a.
No new executable reduction, gameplay speed or thermal benefit is claimed.

Contract and next gates: docs/NATIVE_TYPE_CAST_CONTRACT.md. Evidence:
.build/validation/frame-work/summary.json. The 22-second nonlifted development
probe is separate from the 90-second release replay profile, has no replay tape,
and retains only its first 256 argument rows. It counted 117,146 entries; these
are not release performance figures. Shipping correctly rejected development
probes before that supplemental run; the rejection is recorded. All 58 protected
files remain unchanged; temporary input copies were fingerprinted for cleanup.
The broader native-ownership and 800 FPS objective remains active.

Nineteenth increment, metadata-validation continuation: the maintained catalog
tool now recovers 4,112 descriptors and 2,197 structural virtual-table candidates,
interns 1,661 exact class-name identities, and shares semantically identical
ordered class views. The original-instruction oracle passes 225,185 comparisons
(110,307,876 instructions), including 197,702 new generated-catalog queries.
Five selected CTests pass, with 22 catalog-tool cases; 109 CTests are registered,
and the full suite was not rerun. The native x64 array estimate falls from
386,808 to 186,824 bytes through sharing, but this is neither an executable-size
result nor production integration. The cost excludes bindings and PE alignment.
One overlapping caller entry and six unresolved indirect branches remain
explicit in the analysis. Exception unwinding and complete immutable ownership
remain integration gates.

The unchanged release replayed the complete 315.391183-second recorded route
and a 30.013317-second restored continuation. Every catalog metadata span and
candidate virtual-table header matches the DOL at all three captured points;
these headless observations do not prove lifetime immutability or gameplay FPS.
The first restore command was rejected for incompatible cold-boot/load flags;
the corrected run passed and both invocations remain recorded. Production size
and hash are unchanged. Evidence: .build/validation/frame-work/type-catalog.json,
catalog-test-details.log and metadata-observation.json. See
NATIVE_TYPE_CAST_CONTRACT.md for the exact scope and remaining work.

Nineteenth increment, compiled-query and unwind continuation: 105 recovered
target identities need 315 successful class/target records. The new native
lookup searches checked compact records (1,260 bytes for matches alone); the
input identity and result offset retain their full widths. Unsupported source
restrictions, ambiguous class views and lossy bounds are rejected by the tool.
This does not include binding/header/code costs or establish an EXE reduction.

The expanded lookup oracle passes 656,945 original-code comparisons, including
431,760 compact-query cases, and executes 149,985,008 original instructions.
A separate 72-pair original/recovered failure-entry test executes actual catch,
cleanup and rethrow code with the original cast instructions erased from the
recovered fixtures. It preserves the exception payload and rejects a deliberately
wrong unwind PC; it executes another 157,195 original instructions. The failure
boundary remains a test witness until the actual emitter is connected. Six
selected CTests pass (26 catalog-tool cases); the strengthened unwind test and
its three affected gates also pass. There are 110 registered tests; the full
suite was not rerun. Production size/hash and deployed builds remain unchanged.
Evidence: .build/validation/frame-work/compiled-queries.json,
compiled-query-test-details.log, unwind-retirement-test-details.log and
continuation-owner.json. The remaining work is native ownership/binding,
caller lowering, actual linked-body removal, and before/after gameplay and
resource measurements. No FPS gain is claimed from these isolated tests.

Nineteenth increment, metadata-ownership continuation: the new store audit
examines 39,355 declared entry contexts and recovers 13 null-first-method
virtual tables missed by the earlier structural scan. Nine new roots add eleven
descriptor aliases; the existing 1,661 shared class views and 1,260 bytes of
compact match records remain sufficient. An effect constructor demonstrably
changes its object type twice, so permanent per-object type caching is invalid.

The audit tracks explicit SDA-register writes and separately marks assumptions
at independent entries. This corrects a false literal-write attribution caused
by the shared GX classifier's fixed-register assumption. One assumption-dependent
overlap and the unresolved stores/control flow remain visible; metadata
immutability is not certified. No runtime or production executable was changed.

The expanded original-instruction oracle passes 658,313 comparisons (150,109,251
instructions), covering 4,123 descriptors and 2,210 virtual tables. All seven
selected CTests pass, including 41 metadata-tool cases and the 25 existing CFG
classifier cases. The 72-pair unwind test also passes. Evidence is
.build/validation/frame-work/metadata-ownership.json and
metadata-uses-test-details.log. The original snapshot observations retain their
narrower catalog scope. Remaining work is RTTI ownership/consumer integration,
actual body removal, and measured before/after gameplay; no FPS, thermal or
executable-size gain is claimed for this continuation.

Nineteenth increment, consumer and exception continuation: the recovered unwind
fixtures erase 209,589 bytes in 5,733 class-metadata spans and replace 2,210
virtual-table descriptor fields with native IDs. All 72 unwind comparisons
still pass, plus five actual typed-exception matcher pairs. The complete test
executes 158,539 original instructions. Exception strings/index records remain;
this is a test-only boundary, not a production migration.

The new consumer audit records 215,231 explicit load sites (185,817 unknown
addresses), 603 metadata-valued call registers (63 outside the cast), and 176
external data references (162 to names, 14 to descriptors). Callee inspection
proves shared class-name bytes are copied into gameplay objects and passed to
ordinary string routines. They must be preserved; the exception fixture's
blanket erasure cannot be applied to gameplay. Four new analyzer cases bring
the metadata-tool total to 45. Seven selected CTests pass in 27.25 seconds;
658,313 lookup comparisons and the 25 CFG cases still pass. Current evidence:
.build/validation/frame-work/metadata-consumer-contract.json and
type-consumer-test-details.log. Native ownership, remaining consumers,
restoration and actual caller integration remain pending. The production
executable is unchanged, so no new size, resource or FPS improvement is claimed.

Nineteenth increment, native query owner: the emitted runtime-facing header is
now separate from the oracle's original addresses and descriptor aliases. Its
315 matches and 105 compact ranges occupy 1,680 bytes in the compiled test.
This includes range headers; live-object bindings, caller code and executable
alignment remain unmeasured. The owner is not linked into the release yet.
All 432,915 compiled-query comparisons now invoke this owner; the oracle no
longer contains a separate compact match table. Generating identical class
semantics at relocated image addresses yields identical native owner source,
and stale query semantics are rejected.

Both C++ test targets now share one metadata-generation dependency. The initial
build ran the expensive scan twice; the corrected build ran it once. Seven
selected CTests pass in 22.86 seconds, including 658,313 total lookup comparisons,
72 unwind pairs, five typed matcher pairs, 47 metadata-tool cases and 25 CFG
cases. This is build/test evidence, not gameplay performance. The initial
Python run failed one old fixture-layout assertion after a text replacement
refused the file's LF endings; the assertion now verifies the moved table in
the native owner and the address aliases in the fixture. No behavior expectation
was removed. Source, generated data, binaries and logs are pinned in
.build/validation/frame-work/native-query-owner.json. The original production
cast, shared gameplay names and deployed game are unchanged. Live-object type
identities, initialization/restoration, caller integration and production
retirement remain pending.

Nineteenth increment, generated call preparation: the pinned-image generator
now emits disposable object/type bindings and the reference-failure entry.
All 435,125 prepared-query comparisons agree with original instructions,
including all 2,210 original virtual tables and vptr offsets 0, 0x38 and 0xA8.
The total lookup suite reaches 660,523 comparisons and 150,519,072 original
instructions. All 72 unwind pairs now use the generated failure entry; five
typed matcher pairs, metadata erasure, and the wrong-return-PC mutation gate
still pass. Exception writes are checked through journal rollback as well.

Seven selected CTests pass in 23.51 seconds (48 metadata-tool and 25 CFG cases).
The first run failed the liveness gate because the non-dispatch preparation
helper was placed in the dispatcher namespace. It now has its own namespace;
both oracles still link the reference CPU without shadow_recomp. Neither the
liveness guard nor its root list was weakened. The rejected log remains.
Temporary binding data adds 16,492 bytes to the 1,680-byte native query owner;
that does not include code or executable alignment and is not a size saving.
The source-only scaffolding inventory does not count these generated bindings;
they are recorded separately in type-call-boundary.json. The release executable
is unchanged. Actual recompiler dispatch, immutable metadata/restoration
ownership, old-body retirement and production measurements remain pending.

Nineteenth increment, generated caller transfer: dedicated type-query lowering
now preserves normal return, fault, exception and unexpected-return continuations.
The dispatcher accepts an explicit continuation result instead of overwriting
prepared exception state. Existing lowerings retain sequential behavior; records
remain 16 bytes on x64. Pruning follows throw/destructor dependencies and removes
the old cast edge in an isolated generator fixture. Production manifests remain
unchanged.

An unchanged original caller passes 8,246 descriptor comparisons through normal
and cold interior entry, plus null cases, with old cast instructions erased in
generated runs. The test executes 394,334 original instructions and checks object
flags, return/control/nonvolatile state and time. Exception probes verify transfer
mechanics; the existing original unwind oracle remains separate. Fifteen selected
CTests pass in 37.77 seconds, including controller, scalar, matrix, reference and
production dispatcher regressions and generator refusal/pruning gates.

The first fixture build lacked required hex prefixes; the first test run passed
11/12 and caught an incorrect fixture-entry count. Both failures are retained and
corrected, including requiring the real post-call entry and a true cold interior
path. A stalled sandbox MSBuild was cancelled only after live-process inspection;
its replacement ran with normal worker permissions. Full evidence is
.build/validation/frame-work/type-call-integration.json. All 58 protected files,
the release executable and matching map remain unchanged. This is integration
validation, not release size, gameplay FPS, or resource improvement. Remaining
work includes all callers, metadata lifetime/restoration, final production body
retirement and measured native-1440p gameplay.

Nineteenth increment, compact temporary bindings: exact membership for all 4,123
descriptors now uses 1,493 bitmap bytes, with 1,960 bytes of explicit identities
for the 490 aliases that can match any compiled query. Known unqueried classes
retain ordinary cast failure; unknown inputs still stop explicitly. The native
query owner and catalog are byte-identical. Binding payload falls from 16,492 to
3,453 bytes; combined query/binding payload is 5,133 bytes before code/alignment.

The identical C++ test harness passes both layouts and exhaustively checks
196,606 descriptor-domain inputs. All 15 selected CTests pass in 25.75 seconds,
including 660,523 original lookup comparisons, 8,246 generated original-caller
comparisons, 72 unwind pairs, five typed matcher pairs and 53 metadata-tool tests.
The initial direct Python invocation hit two temp-directory permission errors;
unchanged tests passed with task-local TEMP/TMP and normal worker permissions.

The component-test EXE shrinks from 801,792 to 788,992 bytes (12,800 bytes).
Its virtual read-only data falls by 13,056 bytes, while virtual code grows by
448 bytes. This result includes file alignment, uses an identical C++ harness,
and supplies no production size or gameplay speed claim. The temporary control
is deleted after retaining its hashes and PE report. Evidence is
.build/validation/frame-work/compact-bindings.json. The release and all 58
protected files are unchanged; metadata lifetime, native object ownership and
complete caller/exception integration still prevent production activation.

Nineteenth increment, caller scratch-state audit: the new tool follows direct
callees without assuming ABI clobbers, covering 268 literal cast sites and 271
contexts. It retains 250 possible register-use cases, 952 contexts with barriers,
and 1,842 with return escapes (overlapping categories). The reads occur at 17
instructions: 16 stack-relative stores and one r11 read. A checked negative-byte
branch invariant excludes the latter from its function's normal direct entry;
arbitrary entries and memory/stack observations remain unresolved.

Eighteen new tests bring the metadata-tool gate to 71 passing cases. The shared
wait-point classifier (nine cases) and its 442 back-edge manifest also pass.
The final run repeats only the changed metadata/guard tests. Runtime source and
release executable are unchanged; no C++ rebuild, gameplay or speed claim follows
from this analysis. Evidence: .build/validation/frame-work/caller-scratch-evidence.json.

Three empty variadic-body candidates have 941 literal calls and 1,663 exclusive
unwind code bytes in the release; leaf/cold code and alignment are not fully
attributed. Their stack-only work and caller-visible register changes need native
retirement contracts before removal. Source/body/map identities and call sites
are retained in variadic-empty-candidates.json, not treated as measured hotness.


## Empty variadic retirement and static continuation policy (twentieth increment)

Three original routines only spill variadic arguments into private stack frames;
they never consume those values or perform logging, formatting, math, allocation
or I/O. Generation now retires their bodies and interior dispatch ranges and
shares their necessary return effects across 941 direct calls. The disposable
translated caller boundary remains; this does not make the whole game native.
Exact original templates, incoming edges and pointer lifetime are build gates.
The old DOL remains embedded as reference/resource data.

The first full release was rejected at 105,598,976 bytes (+17,408): returning a
continuation enum from every lowering suppressed native tail calls and added
unwind records. The corrected policy fits in the existing 16-byte dispatch
record. Original opcode words remain in a separate nonshipping validation array,
and the production post-link gate rejects that array if linked. The corrected
release is 105,577,472 bytes, SHA-256
`c350a565e2caaca3d8a6746e97a1120aef8776f84a25886fbe55e4fcce8aad18`:
4,096 file bytes smaller than its retained 105,581,568-byte control. Virtual
code falls by 4,240 bytes, read-only data by 80 and unwind data by 36. Exact
unwind attribution changes generated code by -1,736 bytes and runtime by +12;
this is not attribution of every byte or padding. Total file reduction from
the original rebuilt control is 3,863,040 bytes (3.53%).

Twelve selected CTests pass, including 12,288 original leaf comparisons,
288 original caller pairs, 507 checked private-pointer lifetime contexts,
11 invalid generator cases and existing generated math/controller, dispatcher,
wait-point and liveness tests. The caller fixtures expose their supplied
20-byte allocation and do not claim to validate the original allocator.
The full suite was not rerun; its historical missing B45.mem1 fixtures remain
unresolved. Contract details: docs/EMPTY_VARIADIC_CONTRACT.md.

Both builds complete the full 315.391183-second recorded Westopolis route with
byte-identical final state reports, PCM, checkpoint and capture. A separate
30.013317-second restored run with live sound and native 2560x1440 rendering
matches state, PCM and screenshot bytes, with 1,800 fixed pacing boundaries,
zero skips, render failures or audio underruns. The fresh candidate capture
was visually inspected; the D3D12 debug layer was requested and no errors were
logged. Those capture runs are correctness evidence, not speed measurements.

The first three alternating uninstrumented pairs render 1,797 frames each.
CPU medians are 8.437500/8.140625 seconds (-3.52%) but ranges overlap. An apparent
16 MiB working-set increase is confounded by different executable paths/names:
control.exe versus ShadowTheHedgehog.exe. Separate same-path diagnostics give
final working sets of 206.660156/206.542969 MiB and peaks of
212.769531/212.523438 MiB. The responsible host component is not identified;
these observations establish path sensitivity, not an optimization saving.
Virtual-memory metadata snapshots and external process-counter timelines are
retained separately and never counted as performance samples.

The benchmark now uses one absolute executable pathname for both arms,
verifies the staged bytes, records both identities and removes only its own
copy after each child exits. Three added tests reject stale inputs, protect
existing/source files and verify cleanup after exceptions. All 17 benchmark
unit tests and the combined benchmark CTest pass. The original six results
remain visible; separate matched-path performance results are in
.build/validation/frame-work/empty-variadic-matched-summary.json.

Matched-path process CPU samples (control/candidate) are
[8.531250, 7.593750, 8.312500] / [8.671875, 8.203125, 9.093750] seconds.
The median is 4.32% higher and all three pairs are slower. Peak working-set
medians are 218.589844/218.449219 MiB, with overlapping ranges; no RAM saving
is established. Six subsequent runs using identical control bytes at the same
path give [7.578125, 8.109375, 8.281250] / [7.734375, 9.140625, 9.031250]
seconds: an 11.37% difference between labeled-arm medians despite identical
code. All twelve runs render 1,797 frames and retain equal game/audio/asset
work counters. Calibration is retained without subtraction. CPU causation
remains unresolved, so this candidate is a verified file-size reduction and
correctness result, not an accepted gameplay speed/resource improvement.
The new continuation-policy branch executes zero times on this route according
to the existing dispatcher counter; code-layout and host-scheduling effects
are not excluded. No deployed executable is replaced.

A separate counter-only diagnostic observes approximately wall seconds 3..28,
without suspending threads or changing priority/affinity. Game-thread CPU is
5.843750/6.187500 seconds and main-UI CPU is 0.437500/0.890625 seconds. These
single diagnostic samples identify broad owners, not a code-level cause or
performance gain. The control executable and checkpoint remain available for
follow-up; the native type-query replacement is still disabled in production.

All 58 pinned executable/personal files are unchanged. Cleanup removes 156
owned disposable files (710,645,138 bytes): duplicated persistence, matched PCM,
extra captures/checkpoint, unused fixture input and superseded temporary runners.
Every removed file's size/hash is retained in empty-variadic-cleanup.json.
The inspected candidate image, source tests, failed-gate logs, raw measurements,
invocations and active CPU investigation inputs remain. Current candidate/map
hashes and all 998 source inputs are verified; only two benchmark-tool files
changed after compilation, recorded separately in empty-variadic-provenance.json.
The objective remains active; no deployment or 800 FPS result is claimed.

## Twenty-first increment: render-target memory (2026-10-25)

Attribution first. tools/memory_map_sampler.py launches an owned game and walks
its address space by region kind, protection and allocation base on every new
private-commit maximum. At native 2560x1440 (Westopolis input tape, windowed,
audio) the control held about 569 MiB private commit; about 450 MiB of it was
write-combined regions whose sizes match D3D12 default-heap allocations, not
game memory (25 MB guest regions, 16 MiB card image, 16 MiB sample memory).
A standalone D3D12 probe on the same RTX 4080 driver confirmed the mechanism:
every default-heap byte (committed resource or plain heap) adds the same amount
to process private commit. Video memory waste therefore also costs RAM commit.

GEKKO_GPU_MEMORY maps showed render targets are about 95% of the default heap;
all asset textures and meshes are about 21 MiB. Two defects wasted capacity:

1. The texture pool reserved every graph texture for the whole frame. A
   mid-frame "scene snapshot" or "history resolve" (the same 2560x1440 RGBA16
   description as the view's final resolve) got its own 30 MiB texture, freed
   three idle frames later and recreated: 20 create/release cycles in 40 s.
   FrameGraph::compile now assigns pooled textures by kept-pass lifetime: a
   resource whose last kept pass precedes another's first hands its texture on,
   and transition planning tracks state per physical texture. The existing
   first-use rule (clear or full overwrite) guarantees no stale contents are
   observed. The glare sources' resolve now shares the output target too.
2. D3D12MA placed output-sized targets in 64 MiB blocks, leaving 62.6 MiB of
   empty block capacity. Textures of 1 MiB or more now own their allocation,
   matching the existing rule for large upload/readback buffers. A first 4 MiB
   threshold was rejected: at 720p a 3.8 MiB target alone opened a new 32 MiB
   block and raised headless peak private commit by 15 MiB.

Alternating runs, same staged pathname, same instrumentation in both arms
(control 1fb47b46..., candidate ef6935b5..., MiB):

| Westopolis | Control | Candidate |
| --- | ---: | ---: |
| 1440p rendered, D3D12 local usage max | 427.6 | 357.4 |
| 1440p rendered, default-heap block bytes | 410.0 | 339.8 |
| 1440p rendered, private commit steady (20-38 s) | 569.4-570.3 | 497.4-498.9 |
| 1440p rendered, private commit peak | 626.9 | 555.0 |
| 720p rendered, D3D12 local usage max | 145.2 | 132.0 |
| 720p rendered, private commit steady | 286.2-300.9 | 275.3-281.6 |
| 720p headless + capture, private commit peak | 295.2-295.8 | 283.0-283.4 |

Earlier 1440p pairs with the lifetime sharing alone gave 395.7 MiB local usage
and 536 MiB steady private commit, separating the two causes. Memory figures
repeat within about 1 MiB per arm; CPU time and presentation percentiles varied
more between identical-arm runs than between arms, so **no CPU or frame-time
gain is claimed**. The executable grows 7,680 bytes (105,585,152).

Correctness: stage_benchmark --verification accepted both pairs (captures at
frames 500 and 1100, inside scene-snapshot windows), with identical complete
state reports and images (50f9d1dd3c86..., bcc861bcffc1...). Every headless
capture in the memory runs matched too. All seven render-labelled CTests pass,
including the new render_core_tests case (shared texture, chained barrier
before-state under the debug layer, per-holder readback contents, overlapping
lifetimes kept apart). Tool tests: 28 pass. Only Westopolis was measured; other
stages, split screen and resolution changes were not run. No deployment.
Evidence: .build/validation/resource-attribution/ (pair1-4, headless, verify-1m-*).

Remaining at 1440p: view colour (4x RGBA16, 116 MiB), view depth (4x D32,
60 MiB) and glare sources (4x RGBA8, 60 MiB, released and recreated whenever
glare pauses for more than three frames) are the scene's chosen quality
settings; they were not reduced.

### CPU attribution after the twenty-first increment (diagnostic only)

tools/native_sample.py now also attributes samples outside the executable to
their loaded module. Candidate ef6935b5, 1440p Westopolis tape, windowed with
audio. Thread CPU over 40 s: game 6.08 s, main-ui 0.42 s, audio 0.19 s,
render-resource-worker 0.14 s (7.47 s total). A 120 s game-thread-only 1 ms
sample (67,966 wall samples) is 80% ntdll waits, 19% in the image. Of the
13,078 in-image samples: generated game code 59.9% with no function above 3%;
AX audio on the game thread about 15% (reverb kernel 4.9%,
select_ax_parameter_blocks 2.8%, run_ax_mix 2.0%, ADPCM decode 1.8%,
render_voice 1.7%, plus read16); paired-single/compare/indirect-call helpers
5.0%. These are wall-time samples perturbed by suspension, not benchmark
results. The reverb output is still not returned to the mix (open audio
finding), so its cost must not be removed before that path is restored.
Evidence: .build/validation/cpu-attribution/sample-game/.

## Twenty-second increment: AX voice selection and ADPCM decode (2026-10-25)

Cost census first. Temporary steady-clock scopes (removed afterwards) on a
120 s 1440p Westopolis route (24.86 s process CPU) measured: translated reverb
callback 866 ms (71,643 calls, 12.1 us each), run_ax_mix 591 ms in total, of it
voice selection 134 ms (5.6 us per 5 ms frame) and ADPCM decode 91 ms. The
wall-time IP sampler had put AX at about 15% of game-thread samples; the
runtime's own timer shows about 3x less. Wall samples include preempted time;
rank with timers, not sampler shares. The chain held 64 blocks in all 7,891
frames of a 40 s run.

1. Voice selection (gekko/ax_voice_select.h). The former code compared every
   block with every earlier one, every slot with every block, and searched all
   slots per block. A frame whose chain is the previous accepted chain (each
   previous slot still holds its block, same nonzero count) now keeps every
   slot. Re-running the former rules on it is idempotent: no new block, only the
   now-empty slots reset again. Only the host reset and checkpoint load write
   the slots otherwise, and both zero the previous count. Any other chain takes
   a sorted overlap check, falling back to the original quadratic scan to report
   the same failing block, link and count, then a sorted assignment.
2. ADPCM (gekko/ax_adpcm_decode.h, moved verbatim from system.cpp). Samples
   between a frame's header and the frame's or sound's end are decoded in a
   tight loop with the frame's predictor and scale; boundary samples take the
   unchanged per-sample step (headers, one-shot end, loop reload).

Oracles in ax_pb_tests (default run and CTest) compare with verbatim copies of
the former code. Selection: 200,000 overlap cases (141,776 with an overlap),
160,000 assignment frames and 240,000 System-path frames (106,981 unchanged
chains, 11,976 simulated checkpoint loads) are identical in slots, order and
resets. Their inputs keep the invariant checkpoint loads enforce (distinct
nonzero slots); my first test version violated it and both implementations
then ran past slot 63, a test defect, not a code difference. ADPCM: 180,000
continuing calls, 62,915,630 samples, 27,271,712 loop reloads, 9,984 finished
one-shots identical in cursor, ring and counters. Kernel timing (diagnostic,
best of 7 alternating): selection at 64 blocks 2,661 -> 32 ns repeated chain,
3,974 -> 1,879 ns reordered; ADPCM 382 -> 255 ns per 161-sample call (each
sample depends on the two before it, so this is near the serial floor).

Gameplay gates, control ef6935b5 (twenty-first increment) vs candidate
c5e051ba (105,589,248 bytes, +4,096), alternating, headless with --audio-dump:

| Route | PCM / state / capture | AX mix, control | AX mix, candidate |
| --- | --- | ---: | ---: |
| Westopolis tape, 120 s | identical in 4 runs (15,289,600 B) | 434.0, 432.8 ms | 333.2, 334.2 ms (-23%) |
| Final Haunt, no input, 60 s | identical in 4 runs (7,608,320 B) | 138.7, 138.2 ms | 95.7, 94.9 ms (-31%) |

The per-frame AX cost fell from 18.9 to 14.7 us (Westopolis) and 12.2 to 8.5 us
(Final Haunt). Paced rendered 1440p pairs (selection only) also measured lower
AX time in both pairs (35.8 -> 24.5, 25.7 -> 21.0 us per frame) with zero
underruns; process CPU varied 6.1-9.7 s between runs, so **no process-level
CPU gain is claimed** (the saving is about 0.8 ms per second of game time).
Seven audio CTests pass. Opt-in ax_pb_tests --adpcm-loop-oracle fails with an
identical message when built from the original system.cpp: a pre-existing
failure outside the default run, reported, not changed.

Not done: the reverb. Production runs the original callback on auxiliary
buffers the mixer never fills or reads back (eleventh increment), so it
processes silence at 3.5% of process CPU. Speeding it up in place would tune a
path that must be rebuilt; the next step is restoring the native aux send/return
(audible), then retiring the callback. Evidence:
.build/validation/audio-selection/summary.json (all hashes; duplicate PCM
removed after hashing) and .build/validation/cpu-attribution/stage-timers-run/.

## Twenty-third increment: the room reverb is heard (2026-10-26)

Before: the mixer accumulated aux A sends but never sent or returned them, and
the game's ReverbHi callback ran translated code (R18a) on always-zero guest
buffers: 3.5% of process CPU spent on silence (twenty-second increment).

Recovered contract (main.dol). The effect control's selector 0x8043F304 has two
callers: enable 0x80440290 registers type 2 (ReverbHi callback 0x803885A0,
context audio object + 0xFF58) on aux A, disable 0x8044024C registers none.
Nothing registers aux B; the three other initialized effects are never
registered. Start-up 0x8043F9A0 calls ReverbHiInit 0x803884E4 (parameters at
+0x1C8); ReverbHiShutdown 0x80388554 has one caller. The callback returns at
once while byte +0x1C4 is set, else runs the crossfeed 0x80387D24 and
HandleReverb 0x80387EB4 per channel. Its only caller is the SDK aux processor.

Change. EffectStage gains a split block (begin / finish / process) driven by
the command list's own schedule and by the game's callback dispatch; render()
is the same sequence. The mixer owns it (fold, voices, aux send/return,
compressor, output, as the microcode orders them). The callback is a native
owner (extract/native-owners.txt): bypass byte and FPSCR[NI] are honoured;
an unknown object stops the run. Init/shutdown stay translated and only hand
over parameters and object (SCAFFOLDING: their delay-line allocations are now
unused). Checkpoint version 23 adds binding, parameters, tail, pending blocks
and phase; older files warn and rebind from the game's parameters with an
empty tail. Retired: callback body, 0x80387D24 (100 instructions) and
0x80387EB4 (323) pruned by the generator, absent from the linker map; the R18a
kernel port, its owner and its regression test deleted (copies outside the
project). Executable 105,506,304 bytes, 82,944 smaller; final SHA-256
1ef49e04dbb4335be0cb1198934855067e0d44c1ed01e17c8b6533ebefe5c0e8.

Validation against original code (axfx_reverb_contract_tests, 78,449,385
original instructions): original callback vs the owner's parameter read,
bypass gate and native effect, 512 callbacks incl. 8 bypassed, 2 floating
modes, full retained history; a stage driven only by the original aux
processor's calls into 0x803885A0 through the production owner function
matches every original CPU+DSP output word over 360 blocks, 204 callbacks,
66 late controls and 30 restores. native_audio_mixer_tests: aux A routing and
return timing match an independent model over 120 blocks (a mutated return
delay gives 119 mismatches). ax_pb_tests: audible return, exact 24-block
checkpoint continuation of a nonzero tail into reused and fresh owners, v22
load changes 24 blocks, bypass equals an absent callback, a foreign object is
refused. Full suite: 111/113; the two failures are the long-missing B45.mem1
fixture (rw_geometry_convert_tests, gx_texture_decode_tests).

Gameplay (headless, alternating, control c5e051ba vs candidate): each arm is
self-deterministic (PCM, state report, full snapshot). Captures identical.
Game CPU state identical. Every differing guest byte is attributed, none left:
AX output buffers (905 / 0 bytes), the old callback's cursors in the reverb
object (60 / 62), its pre-delay cursor words (12 / 12), the sound thread's saved
FP area (35 / 60) and dead stack below a thread's SP (97 / 101) for Westopolis /
Final Haunt; every thread's SP and stack bounds are equal. The effect ran on
every scheduled block (Westopolis: 23,880 scheduled, 23,881 processed). PCM:
the reverb starts at 1.77 s in Westopolis, difference -50.3 dBFS RMS against a
-16.2 dBFS mix on 92.4% of samples; Final Haunt -76.3 dBFS on 13.5%; clipping
41 -> 39, peaks unchanged. These measure the change, not its audible quality:
the reference for "correct" is the original-word contract above.

Cost: native effect 19.8 us (headless) / 24 us (paced 1440p) per 5 ms frame.
Headless 120 s wall and CPU are within run spread (control 18.78/19.18 s,
candidate 18.81/18.68 s); paced 1440p runs had zero underruns or render
failures, CPU spread larger than any difference. No CPU saving is claimed; the
effect now does real work where it previously processed silence.

Also fixed: liveness-roots.txt was stale since the twenty-second increment (a
data literal of its new test); native_liveness_roots now passes. state_delta.py
accepts v23 and counts trailing bytes of differing length instead of failing.
docs/AUDIO_OUTPUT_CONTRACT.md was normalized to CRLF while editing (it had
mixed endings). Evidence: .build/validation/reverb-integration/summary.json,
attribution.json, state-delta.json per stage.

User listening session on the candidate (2026-10-26, fullscreen, audio, full-save card, normal boot;
.build/play-session-reverb): 216 s, 12,456 frames, 0 render failures, room effect 35,543 blocks processed at
25 us per block, average 57.6 FPS with interval p99 33.6 ms and max 592 ms, one 1.25 ms device underrun at audio
time 24.8 s, 6 clipped samples, peak private 505 MiB. The hitches and the underrun are not yet attributed; they
are workstream 0 of docs/NEXT_SESSION.md.

## Twenty-fourth increment: snapshots resolve what their distortions read (2026-10-26)

Attribution first (workstream 0). The play session's "29% of quanta > 17 ms" is not over-budget work: a quantum is
the wall time between two observed boundaries, sleep included, so it is 16.67 ms plus the change in guest work
before the crossing. Its 612 quanta >= 20 ms average guest 6.5 + sleep 16.0 ms and are each followed by a short one;
only 2 of 12,985 boundaries were behind. Game speed held; the stutter is in delivery to the display.

New tool: tools/present_trace_report.py (+ test_present_trace_report.py, in CTest presentation_analysis) joins a
GEKKO_PRESENT_TRACE capture (non-shipping build) per present with DXGI frame statistics and classifies every refresh
that showed an old frame against the packet's game step (a 30 Hz movie step wants two refreshes) as packet_late,
render_late or gpu_late. Display measured 60.00057 Hz: the 60 Hz pacing ladder drifts against it by ~10 ppm (one
refresh per ~29 min), so push phase is quasi-static within a run and differs between runs.

Westopolis gameplay replay (--boot-stage stg0100, the recorded Westopolis tape, fullscreen 1920x1080, render
2560x1440 4x, 90 s, diagnostic build): 5,292 fresh refreshes, 19-25 repeats per run, of them 17-24 gpu_late, 1
packet_late; 27 drops. The gpu_late frames come in clusters and each holds 4-5 "snapshot" passes of 1.4-3.2 ms
(GEKKO_GPU_TRACE): a distortion draw (heat haze, refraction) samples a snapshot of the scene colour, and every
snapshot resolved the whole 1440p 4x RGBA16F view although the draws cover a small patch (their own pass: ~0.05 ms).
Measured splits on the same replay: the driver's ResolveSubresource instead of PSResolve 1.02 -> 0.66 ms per
snapshot; PSResolve scissored to 16 x 16 (picture wrong, measurement only) 1.02 -> 0.012 ms. So the cost is the
full-area resolve, not the multisampled target's transition. Both probes were removed after measuring.

Change (runtime/lift/render/snapshot_reads.h, renderer.cpp). Before a frame's views are recorded, every snapshot
gets the texel rectangle its readers can sample: per triangle of each canvas draw that reads it, the bound of its
texture coordinates mapped exactly as the draw's pass maps them (canvas or region placement, canvas_image_rows),
widened per axis by half the triangle's texel change per pixel (the attribute is evaluated at the centre of a pixel
the triangle only partly covers), by the bilinear footprint (clamped to the edge texels, as the sampler clamps) and
by one texel of float slack. The snapshot's PSResolve is scissored to it (still the renderer's own bit-exact
resolve; a single-sampled view copies the region). A snapshot no draw reads resolves one texel (kept for the graph's
order). What cannot be bounded - a draw that is not on the canvas, a read from another view, lines, a placement
whose w is not 1 - resolves the whole view as before. The view's shared snapshot texture is unchanged: each
snapshot's readers read only inside its own rectangle.

Exactness. Headless replay captures at 2560x1440 of frames 440-737 (100 frames, the heavy distortion stretch):
without multisampling (SHADOW_R2_MSAA=1) control is self-identical and the candidate matches it in all 100 frames,
so no rectangle misses a read texel. With 4x the control differs from itself between runs in 5 of 100 frames by
1-6 pixels at 1 LSB (the known RTX 4080 MSAA variance); candidate vs control differ in 7-8 frames by 1-5 pixels at
1 LSB, the same envelope. Game state reports identical. render_core_tests gains
snapshot_reads_bound_the_sampled_texels (exact rectangles, union, clamped edge, degenerate triangle, refused lines and
projective placement); the WARP end-to-end snapshot test passes through the region path.

GPU on the same traced replay (diagnostic builds, one run each; GPU clocks vary between runs, so whole-frame
figures carry that spread): snapshot 1.023 -> 0.032 ms each (2,068 snapshots on frames with equal counts); GPU frame
p99 14.67 -> 10.70 ms; frames over 16.6 ms 28 -> 12; repeats 25 -> 9 (gpu_late 24 -> 6); drops 27 -> 13; present
intervals > 20 ms 96 -> 23, p99 24.1 -> 17.7 ms. The remaining over-budget frames (ticks 3045-3065) are a motion
trail moment: scene 7 ms, a second scene pass 3.4 ms, the view resolve 3.2 ms and the history resolve 1.8 ms - a
full-view resolve far slower than its bandwidth, probably reduced GPU clocks in a mostly idle 60 Hz load; open.
Display-side A/B in one traced diagnostic binary (SHADOW_R2_WHOLE_SNAPSHOTS=1 restores whole-view snapshots;
compiled out of shipping), alternating whole / regions / regions / whole, 90 s each, all runs kept: repeated
refreshes 18, 20 -> 7, 7 (gpu_late 15, 18 -> 5, 5); dropped packets 42, 51 -> 2, 5; GPU frames over 16.6 ms
21, 35 -> 0, 2; GPU p99 13.75, 14.86 -> 10.44, 10.84 ms; snapshot mean 0.81 / 1.09 -> 0.028 / 0.032 ms. Three
repeats at ticks 16-26 (stage start) occur in every run.

Shipping executables (control 1ef49e04 vs candidate, same replay, alternating, untraced): dropped packets 27, 32 ->
11, 12. The CPU-side present intervals (session JSON / stdout "longer than 20 ms") did not improve - control 31, 44,
candidate 57, 39 - and the A/B shows why that figure is not what the display shows: a regions run with 258 intervals
over 20 ms had 7 displayed repeats. Render/present returns jitter around the vblank without the display missing it.
The play session's 480 "intervals > 20 ms" are this CPU-side figure, so its display-side stutter count is unknown;
judge stutter with the present trace. The candidates' interval trains at ticks 1394-1440 and 2392-2450 (a long
return every third frame) did not show as displayed repeats in the traced runs; not attributed further.

Candidate release executable 105,511,424 bytes. Measured as SHA-256 644bbc9c76046300d3f567288f2f5eaacf02c2a7fe56ca9b32c7119bc388f6e2;
the final rebuild after the diagnostic A/B switch (compiled out of shipping) is
1a0fd1ae98b37ac7c76744229095db051a22920f49af5ee20ac549498063ae40 and differs from it only at 4 bytes (COFF and
debug-directory timestamps, offsets 264-265 and 96630772-96630773). Not deployed. Full suite 111/113, the same two
missing B45.mem1 fixture failures. Evidence: .build/validation/present-trace/ (westopolis-1, -2, -hwresolve,
-scissorprobe, -regions-1, ab/), .build/validation/snapshot-regions/ (captures, paired/, ctest.log).

## Twenty-fifth increment: the stray menu sound, and phase 1 of the native sound path (2026-10-26)

User report (first logged 2026-09-28): a pause-menu sound plays out of nowhere in Lethal Highway. The user recorded a
284 s run in the diagnostic build (.build/recordings/lethal-highway-sfx; no Start press in it). A headless replay with
the audio audit named every voice from the bank files: 5 plays of B0_SYSTEM program 0, SE_SY00_WINOPEN. `--watch` on
PlaySE shows each is the game's own request for 0x7160 SE_Z1090_WARPHOLE (a Black Arms tank warping in, with its
engine loop 0x714D). AudioDataStageCity.bin row 90 is unfilled in the shipped data - bank 0, program 0 - so the
faithful translated GCAX plays the menu sample: an original-data defect, not a port fault. B71_CITY's program 90 is
the only program no row names (rows follow row i -> program i); the user chose it as the correction. Full map of the
GCAX sound path, the evidence and the phase plan: docs/GCAX_SOUND_MAP.md.

Change (phase 1): the SE table lookup 0x801D88CC is a native owner (gekko/native_sound_tables.h) with the original
contract (slot from the id nibble, first matching row, disabled rows, instance limits); its helper 0x801D8838 is no
longer generated. The one erratum is applied when its row is resolved and matches the whole shipped row and the
table's row count. The rows stay in the game's buffer for the still-translated SE manager (SCAFFOLDING until phase 2).
F8 user marks now work in the lifted presentation loop (they were only in the legacy loop), stamped with the game
tick; the audit itself is compiled out of shipping.

Validation. sound_table_contract_tests: 17,772 lookups (16,759 found) identical to 5,695,758 original instructions
over all 23 shipped tables, every id nibble, ids above 0xFFFF, disabled rows, instance limits past their maximum and
empty/negative counts; exactly one erratum, changing exactly that row's bank and program. Lethal Highway tape: no
WINOPEN voice left (5 before), City program 90 at the same five moments on the same voices, identical game time and
frame-limiter counts, the only differing guest-state block is the corrected row's (0x809F23D8), PCM differs only from
each warp-in for the new sample's length and its reverb/compressor tail; the shipping candidate's PCM equals the
diagnostic build's. Westopolis gameplay replay (headless, 120 game s, capture at frame 3000): control and two
candidate runs identical in PCM, state report, full checkpoint and the 2560x1440 capture (no warp hole there; 0
errata). Full suite 112/114: the same two missing B45.mem1 fixtures; sound_table_contract_tests, native_file_api_tests
(owner list) and native_liveness_roots (regenerated: +0x801D88CC) pass.

Release candidate 105,507,328 bytes (4,096 smaller), SHA-256
ccd38863929e24b4f78817491715ce3c9eeb593068cade7acbbe62b50a9b38a3. Not deployed. Evidence: .build/validation/sfx/
(replay-1, replay-fixed, row-address, gate/, WAV renders; large outputs hashed in removed-outputs.json and removed).

Correction the same day. The user played the original in Dolphin: every Black Arms tank warp-in is silent there, so
the "original-data defect the console plays too" reading above was wrong - the original plays nothing for this
request and the port played an audible sound. The program-90 erratum gave it a sound the original never plays and
was withdrawn; the row is now disabled (program -1), so the lookup refuses it (the user's choice until the mechanism
is found). Why the original is silent is an open question (docs/GCAX_SOUND_MAP.md). Contract test: 17,772 lookups
(16,757 found) identical to 5,695,756 original instructions, one erratum turning that lookup into a refusal. Lethal
Highway tape: 5 requests refused, no WINOPEN and no replacement sound, bank-0 voices 153 -> 148, all other banks
unchanged, identical game time; the shipping build's PCM equals the diagnostic build's. Westopolis gameplay replay:
identical to the control in PCM, state report, full checkpoint and the 1440p capture. Candidate 105,507,328 bytes,
SHA-256 2f561e4a86e25802aeeb04cb254d3442c2b19f3c8bf5e3104ec317230d87fe8a (replaces ccd38863). Not deployed.
Evidence: .build/validation/sfx/replay-silent, gate2/, prefix-voice.

Second correction: a Dolphin reference trace of the retail disc (tools/dolphin_dump.py; Dolphin from a private user
folder with two Gecko codes reproducing --boot-stage stg0202) shows the ORIGINAL plays the menu sound too: the same
AX voice with the same envelope and gains as the port, and the port's WINOPEN signal present in Dolphin's own DSP
output at 0.958 of its volume (misaligned controls -0.22..+0.02). The "original is silent" premise above was wrong;
the first diagnosis (an original-game data defect, faithfully reproduced) was right. The user decided to keep the
warp-ins silent as a deliberate, documented fix of that original-game bug (docs/GCAX_SOUND_MAP.md). The erratum is
unchanged; only comments and the test's message changed: the rebuilt candidate,
SHA-256 2db5170d000fa9ad7cffacc5e3763375cc68b81c876e9fac2e954919b413553b (105,507,328 bytes), differs from the measured
2f561e4a only at 4 bytes (COFF and debug-directory timestamps, offsets 272-273 and 96626548-96626549). Evidence:
.build/validation/sfx/dolphin-ref-1, clips *_stage_start.wav.

## Twenty-sixth increment: the native SE manager - phase 2 of the native sound path (2026-10-26)

The game's sound-effect manager now runs natively (docs/GCAX_SOUND_MAP.md "Phase 2"; static map docs/SE_MANAGER_MAP.md).
Native owners: GCAX's ring push gcaxPortPush 0x8043F054 (gekko/native_gcax_midi.h, with the sound object's eight
MIDI methods for ports 0-2), the request wrappers PlaySimple / PlayFlags / Play3D / PlayRing, the four request-gate
entries and FrameSE 0x801D6270 (gekko/native_se_manager.h), and the positional math ListenerDistance, MaxDist,
PosVolume, PosPan and DopplerBend with the MSL atan2/sin/sqrt they use (gekko/native_se_math.h, computed with the
port's own PowerPC FP primitives). The native manager calls no translated code: its MIDI messages go straight into
GCAX's port rings, which the still-translated GCAX drains. PlaySE, the allocator, FindOwned, RowFlags, Start, the
per-frame counters, UpdateChan, SendPan, the request helpers and the math are no longer generated (597 rows, 31,653
instructions pruned in all). StopAllSE, SetSoundMode, Pause/Resume, the stage functions, ForceStop, InitChan, Route
and UpdateFrame stay translated and share the channel memory (SCAFFOLDING until phase 3). The table lookup gained a
native entry (sound_tables::lookup) and its header's "live instances" reading was corrected: +0x10 counts starts in
the current SE frame and is cleared every frame.

Validation.
- se_math_contract_tests: 205,414 calls bit-exact (result and FPSCR) against 110.7 M original instructions over
  synthetic fixtures, direct MSL routine checks and the Lethal Highway image; 118 compiled-in DOL constants checked.
  It corrected docs/SE_MANAGER_MAP.md: DopplerBend's "> 800" test is on the previous distance.
- gcax_midi_contract_tests: 33,368 method calls identical to the originals (push incl. full and wrapping rings).
- se_manager_contract_tests (new; test_data/se-manager/lethal-highway-mem1.raw, a Dolphin MEM1 image of the original
  at the start of Lethal Highway): seeded random requests, frames, gates and state changes; after every call r3 and
  all 24 MiB of MEM1 equal the original's. Longest runs 11,900 and 8,950 calls (134.4 M / 80.7 M original
  instructions); keep-alive, distance cull, no free channel, retrigger, expiry, ring sound and both gates are all
  required to occur. Rows on both sides get distance-cull / no-pan / row-flag bits (Lethal Highway's tables have none;
  Ark1, LastBKBoss and LastEggBoss ship cull rows). No shipped row of the 23 tables selects the special ring, whose
  channels above 0x26 would need GCAX ports 4-7 (not native; such a message would be counted, not sent).
- Replays in the diagnostic build, against the translated manager's push stream recorded with `--watch` before the
  change: Lethal Highway tape 115,365 messages and Westopolis gameplay replay 37,121 messages identical in order; PCM
  byte-identical on both. The end-of-run checkpoints (Westopolis) differ in 20 bytes, all dead stack below the stack
  pointer (frames the original functions wrote and the native code does not); the state reports differ in exactly
  one 64 KiB heap block per run, the one holding that stack.
- Full suite (Release preset): 115/117, the same two missing B45.mem1 fixtures as before. native_host_isa_gate failed
  once without output inside the suite run (right after an aborted Debug run in the same tree) and passed when rerun
  directly and through CTest. native_file_api_tests (owner list), native_liveness_roots (regenerated: +10 owner
  and +6 data roots; the three contract tests are marked reference-interpreter-only) and the three contract tests pass.

Release candidate 105,399,296 bytes (108,032 smaller than increment 25), SHA-256
5f55ea2d6725cb8a6c935a6913d2ee2eb74787bcfc97d5c7bc3a8526ceb9bcaf. Not deployed. Evidence: .build/validation/sfx2
(push-baseline-*, cand-*, ss-base/ss-cand checkpoints, asm/).

## Phase A of the optimization plan: measurements (2026-10-26)

Diagnostic build after increment 26 (unchanged game code). Rendered runs: fullscreen 1920x1080, render 2560x1440 4x,
audio, 90 s, two runs per route: Westopolis (stg0100 + its tape), Lethal Highway (stg0202 + the SFX recording's tape),
Final Haunt (stg0604, no input). Evidence: .build/validation/phase-a/ (rendered/<route>-<n>/: gpu.csv, present.csv,
nvml/, budget.json, clock_join.json, present_report.json; pass_table.json; wp-stacks-*; wp-classes.json; wp-plain-*).

A1 - GPU clock state (new tools/gpu_clock_sampler.py: NVML beside the run on the trace's clock, then joined to the
GPU trace's frames). The RTX 4080 spends the runs in its low-power states: P5/P8 in 85-95% of samples, graphics clock
p50 570-780 MHz (p95 780-900; boost ~2.7 GHz), board power p50 39-41 W, utilization p50 27-30%. GPU frame time
depends on the clock it lands on (Westopolis: 250-499 MHz p50 8.4 ms; 500-999 MHz p50 4.1-4.3 ms). Every frame over
16.7 ms ran at low clock: Westopolis frames 3054-3059 (the motion-trail moment, 18.1-18.6 ms) at 690 MHz in P5;
Lethal Highway frames 273-285 (17.0-17.4 ms) at 300 MHz in P8; Final Haunt has none. So the remaining spikes are
work landing on a GPU the driver has clocked down for a ~28% average load; the fix space is the work in those frames
(phase B1) and how the load is presented to the driver (B2), not raw throughput.

A2 - per-thread cycles. The session report's host section now lists every thread (OS description, cycles from
QueryThreadCycleTime, kernel/user seconds, share of wall time) over the measured window. Identical headless
Westopolis replays: game thread 56.3 / 56.0 G cycles (0.5% spread), 99% of wall time; seven of the twelve threads
used no cycle at all in the window (idle pool threads: phase C4).

A3 - inclusive game-thread attribution. tools/native_sample.py --stacks walks each sampled stack with dbghelp
StackWalk64 over the process's own unwind data (stacks unwind through generated code to main; median depth ~15),
writes folded stacks, and ranks functions inclusively. tools/guest_classes.py recovers the game's classes from
main.dol's RTTI records (1,881 classes with vtables, 5,754 virtual slots; ResourceOneFile cross-check holds) and
tools/stack_report.py charges each sample to the outermost class method (the task) and the innermost (the owner).
Westopolis headless, three runs, 2,635 samples (each walk suspends the thread ~2 ms, so attribution only), by task:
Player::Shadow::ShadowExecuter 10.3%, Player::Npc::SonicType::SonicTypeExecuter (the Sonic NPC) 9.8%,
Effect::PJS::EffectTask 7.5%, MessageTask 3.8%, ShadowMotion 3.8%, Sonic MotionImpl 2.5%, Player::Controller 1.9%,
EffectDrawTrans/Shimmer tasks 1.7/1.6%, character collision 1.6% as a task (4-7% inclusive), cameras ~2%;
inclusive by class: Player::Behavior::Ground 11.6%, particles 7.8%. 42% have no recovered class method on the
stack: mostly a base update shared by dozens of enemy/object vtables (fn_801A6F08 -> fn_801A7280, ~7%),
RenderWare-range code (0x804xxxxx) and MSL/SDK code; naming them is RE work. Leaf time outside generated code:
fp_compare 1.9%, psq_store 1.5%, psq_load 1.3%, call_indirect 1.2%, the mixer/reverb ~2%.

A4 - per-pass GPU table (mean ms per frame at the clocks above; paired runs agree within ~0.15 ms):

| Pass | Westopolis | Lethal Highway | Final Haunt |
| --- | ---: | ---: | ---: |
| scene | 3.27 / 3.41 | 3.01 / 3.61 | 2.65 / 2.66 |
| resolve | 0.90 / 0.97 | 0.78 / 0.98 | 0.68 / 0.69 |
| present scale | 0.38 / 0.40 | 0.36 / 0.40 | 0.32 / 0.33 |
| compose | 0.23 / 0.24 | 0.21 / 0.24 | 0.21 / 0.22 |
| glare (all eight glare passes) | 0.60 / 0.63 | 0.64 / 0.56 | 1.74 / 1.76 |
| history resolve + save (trail frames only: p95 1.7 + 1.4 ms) | 0.02 | 0.02-0.03 | 0 |
| GPU frame p50 / p99 / max | 4.30-4.43 / 10.5 / 18.6 | 4.46-4.65 / 11.0-11.2 / 14.6-17.4 | 4.59-4.60 / 11.3 / 11.5-12.2 |

Final Haunt's glare composite is 1.13 ms per frame (p95 2.4) against 0.17-0.20 elsewhere. Display (present trace):
6-9 repeated refreshes per 90 s on every route (Westopolis 8+8, Lethal Highway 9+9, Final Haunt 6+7); on Westopolis
and Lethal Highway 6-7 of them gpu_late, on Final Haunt mostly packet_late; dropped packets 6/7, 4/20, 2/3. One run
(lh-1) skipped one draw in the whole run ("all frames 1"; lh-2 none): one of the renderer's eight skip paths fired
once - not identified (no per-reason counter yet); open.

A5 - the "+55 MiB stage-load peak" is not a stage load. Headless, commit reaches 188 MiB in 0.5 s and stays ~191 MiB.
Rendered at 1440p (tools/memory_map_sampler.py, 100 ms walk gap, GEKKO_GPU_MEMORY): startup 390 MiB, targets at
2-4 s -> ~500 MiB, then +57.5 MiB from ~14 s to ~60 s (peak 557.6 MiB) and back to 502.6 MiB. The extra commit is one
57.5 MiB private allocation while the D3D12 default heap stays flat; it follows the glare passes pausing for 11
frames (744-754 in both Westopolis runs), which releases the 60 MiB glare-sources target and recreates it a second
later (default allocations 327.7 -> 267.4 -> 327.7 MiB at 12.2 / 13.2 / 14.3 s). The released memory stays committed
for ~46 s. Keeping the glare targets alive (plan B3) removes both this commit and the recreation.

## Twenty-seventh increment: skipped-draw reasons and the glare keep-alive (plan B3b, B3; 2026-10-26)

Skipped draws (B3b). The renderer's eight skip sites now count a reason each (particle batch / pipeline / upload,
immediate invalid / pipeline / image / upload, mesh missing, skin missing, mesh pipeline / upload); the totals are in
Renderer::Stats::skipped_by_reason and on a "[lifted-render] skipped draws by reason" line, and the first 16 skips of a
run are logged with frame, draw index and reason. render_core_tests checks the reasons of its expected skips
(skin-missing, mesh-missing). Hunting the one skip of Phase A's lh-1: four more windowed Lethal Highway runs (21,397
frames) skipped nothing, so that skip occurred in 1 of 6 runs; the counters will name it when it recurs.

Glare keep-alive (B3). Cause (Phase A, A5): the frame graph's TexturePool released a render target after 3 unused
frames; the glare passes pause for 11 frames on Westopolis (743-754), so the 60 MiB glare-sources target and four more
were released and recreated, and the released memory stayed in process commit for a while (~46 s fullscreen).
Change: TexturePool::kKeepFrames = 3600 (60 s at 60 Hz) replaces the 3-frame default; the pool counts targets created
and released (new "[lifted-render] texture pool" line). Diagnostic A/B switch SHADOW_R2_POOL_KEEP_FRAMES (compiled out
of shipping). A target whose size or format is no longer requested is still released, a minute later.
Exactness: headless Westopolis replay, MSAA off, captures of frames 700-799 at 2560x1440: old policy (created 14,
released 5 inside the window), new policy (created 9, released 0), old policy again - all 100 frames byte-identical in
all three runs; state reports identical. render_core_tests: an 11-frame pause keeps the pooled textures, a full keep
window releases them all.
Measured (windowed Westopolis, 2560x1440 render, memory_map_sampler, one run each): process commit peak 566.8 MiB at
14 s (old) -> 516.9 MiB (new, no spike; its maximum is the slow end-of-run rise); steady 70-88 s 512.2-512.6 vs
516.2-516.9 MiB. When the glare resumes (frame 754) the CPU-side frame recording took 5.38 ms with the old policy
(creating the target) and 0.26 ms with the new.
Full suite (Release preset) 116/118: the same two missing B45.mem1 fixtures; render_core_tests and profiling_tools
pass. Release candidate 105408000 bytes, SHA-256 60eb0b6c19587978d1b75a9630b882bd238211e80257bb99454315decf703002. Not deployed. Evidence: .build/validation/phase-b
(skip-hunt/, glare-keep/).

## Twenty-eighth increment: native resolution and aspect handling (2026-10-26)

Full audit, changes, evidence and limitations: docs/RESOLUTION_AUDIT.md. The R2 renderer already rasterized at its
render size (the legacy EFB / 640x528 / integer-multiplier renderer is not in the release build), but the asked width
was ignored (height kept, width from the window's aspect: 2560x1600 rendered 2844x1600, 3440x1440 on 16:9 rendered
2560x1440, a windowed 2560x1440 ~2616x1440). Now:
- both a width and a height asked -> the game output and the render are exactly that size (camera, culling and HUD at
  its actual aspect); an input tape's output of another aspect gets the largest rectangle of its aspect inside the asked
  size, never a size larger than asked; render sizes above 16384 are refused;
- DXGI display detection (`--list-displays`: outputs, native desktop mode and refresh, supported modes, the
  established presets by aspect family; families by the actual ratio - 3440x1440 is 21:9 at 2.389:1);
- a `resolution` record in the session report; the launcher's Display section rebuilt (Display mode, Aspect ratio
  filter, Resolution = native / presets / monitor modes / custom; the internal-scale multiplier removed);
- no bars where avoidable: a windowed client takes the picture's shape and keeps it while resized; borderless
  fullscreen offers only sizes of the monitor's shape (the user's choice);
- evidence tooling: GEKKO_RENDER_TARGET_TRACE (the targets each pass binds as D3D12 reports them, every viewport).

Evidence: headless captures and target traces at 1920x1080, 2560x1440, 3840x2160, 1920x1200, 2560x1600, 3440x1440,
3840x1200, 5120x1440, 640x480 and 1000x777 - scene colour/depth (4x), resolved view, output and capture exactly the
asked size, every scene/resolve/compose viewport full size; same game tick and frame count at every size; guest state
byte-identical between sizes of one aspect (aspects differ in 4-5 of 385 64 KiB blocks: the game's own
aspect-dependent screen state). Rendered: borderless fullscreen 1080p / 1440p / 4K without bars; windowed 3440x1440
in a 1904x797 client without bars (it had 129-row bars). Cost per resolution: default heap 188 MiB at 1080p to
690 MiB at 4K; GPU timings confounded by the GPU's clock states (single runs). Tests: view_boundary_tests (size rule),
render_core_tests (graph_target_trace), test_resolution_presets (launcher table = engine table, fullscreen shape
filter); the B45.mem1 fixture restored (test_data/renderer-re/final-haunt-b45.mem1: the original run's image), so
gx_texture_decode_tests and rw_geometry_convert_tests pass again. Not deployed.
## Twenty-ninth increment: presentation follow-ups (2026-10-26)

Release candidate 105,427,968 bytes, SHA-256 1097134200ee9763d5ccd1c870825ffcaae605b8a61351cebef6ded5e248f686 (not
deployed). Full suite 118/118 (Release preset). Details and evidence: RESOLUTION_AUDIT.md 9d.

- The first frame after a video-mode switch (the PAL 50/60 Hz prompt) is shown black.
- Edge art (tall art flush with a canvas side) follows that screen side; full-canvas textured backgrounds take
  Anchor::Cover (aspect kept, scaled to cover, overflow cut) instead of stretching.
- Precision: draw matrices are composed in double and rounded once, and skin poses are written relative to their own
  origin. Fixes the results-screen pupils being cut by the head on 6 of 16 frames (stages sit ~25,000 units out).
- Real-time cutscenes (EventManager holding the frame interval, r13+0x3E5D) are boxed 4:3 unless two players play
  (0x8057E760+0xB2). A story cutscene was not captured: no boot route reaches one (see 9d).
## Thirtieth increment: plan B1 (history saves) and B3a (the glare composite folded into the resolve; 2026-10-26)

Release candidate 105,443,840 bytes, SHA-256 9d9c42650cf2ecd944044978b37f58a1fa7db977e11857be664a21d013d52065 (not
deployed). Full suite 118/118 (Release preset). renderer.cpp takes its math from render/frame_math.h (one definition,
shared with render_vk; token-compared, captures unchanged within MSAA noise).

B1. A history save resolved the 4x scene into a frame texture and then copied the whole picture into the view's
history (the history had no render-target view). The history is now created render-target capable with its own RTV
(Renderer::history_rtvs_) and a committed save resolves straight into it (a capture that is also presented still
resolves into a frame texture). Westopolis replay, 2560x1440, 300 consecutive frames from 1800 (76 with a save):
history save GPU time median 0.090 -> 0.049 ms (mean 0.120 -> 0.050); one 29.5 MiB transient target and one full-view
copy fewer per save. A render-target-capable texture must be initialised before first use (the D3D12 debug layer in
render_core_tests caught the first version): ImportedTexture::discard_first makes the history's first render-target
write (Load::Discard) issue DiscardResource after that pass's barriers; the history counts as initialised once a
frame has saved into it. Exactness: 40-frame runs base/base/new/base/new (every frame of each new run equals a baseline
run's frame; baseline runs differ among themselves on 1-2 frames by 1 pixel x 1 LSB, the known NVIDIA MSAA
variance); 300-frame A/B: 18 frames differ, each 1-2 pixels by 1 LSB, 17 of them frames without a save. Also tried:
showing the view from the history when the save is the view's last write (skipping the final resolve) - it never
applies (counter 0 over the run: the HUD is drawn into the view after the save), so it was removed.

B3a. The glare composite folded into the resolve (user's brief: "identical rendered behavior through fewer GPU
operations"; a first reading that called the cost unavoidable MSAA bandwidth was withdrawn).

What the pass costs and why (Final Haunt stg0604 and the Westopolis replay, 2560x1440, 4x, headless captures, same
binary, GPU per-pass trace). The often-quoted 0.03 ms Westopolis composite is a fast-clock frame: under the same clock
state (the fixed-work glare resolve 0.285 vs 0.286 ms on both) Westopolis takes 0.360 ms and Final Haunt 2.33 ms, 6.5x
not 78x. The view resolve, which reads every sample, is cheaper on Final Haunt (1.49 vs 1.83 ms), so colour compression
is not the difference. Attribution variants of the composite on Final Haunt: as is 2.335 ms; blending zero with no
texture work 0.356; texture work without blending 0.718; skipping pixels with no glare 2.325 (exact, no gain: Final
Haunt's glare is non-zero nearly everywhere). Westopolis's composite costs what blending zero costs. The ~2 ms is the
read-modify-write of every changed sample of the 4x R16G16B16A16_UNORM scene (8 bytes x 4 samples read and written per
pixel), not the shader (compiled at pixel frequency: SampleFrequency=0), not barriers (the scene target is not
transitioned around the composite).

The change (renderer.cpp later_canvas_spans / FoldedGlare, glare.hlsl PSResolveGlare). When nothing but canvas draws
follows a view's Glare pass and the scene has 4 or more samples, the composite's blend is drawn only on the 16-pixel
tiles those draws can reach (their placed bounds, +1 pixel, as disjoint row spans: about 97 per Final Haunt frame), in
the game's order; everywhere else the view's resolve adds the glare to each sample with the blend's arithmetic (clamp,
round to the 16-bit grid) before the same fixed-order mean, and re-resolves the spans plainly. Anything else after the
glare (3D, snapshots, history, a second glare), 1x/2x, and the glare debug options keep the plain composite.
Westopolis folds on 8 of its 120 checked frames (its trail frames follow the glare with a history save).

Measured (Final Haunt 4x, 120 consecutive frames, same binary, SHADOW_R2_FOLD_GLARE=0 as the control): composite
2.312 -> 0.148 ms, resolve 1.483 -> 2.345 ms, composite + resolve 3.795 -> 2.493 ms (-34%), sum of the frame's passes
10.27 -> 9.10 ms (-1.17 ms, -11%). VRAM: unchanged (no new target). Synchronisation: no new barrier (the resolve reads
the glare textures the composite read). 2x: folding measured 1.192 -> 1.248 ms (worse), so 2x keeps the plain path;
1x and 2x outputs are identical to the control.

Output. Not bit-identical, and why: per-sample captures of the 16-bit scene (diagnostics SHADOW_R2_RESOLVE_LOW_BYTE
1..5, the low byte of the mean or of one sample) show that the GPU's own MSAA blend writes about 0.19% of pixels (edge
pixels, mostly two-colour) as ONE value - the mean of their samples - instead of D3D's per-sample result; with the
composite left out, the two paths' scene samples are identical (5 of 3.4 million pixels, noise). The fold computes the
specified per-sample result. Net, against the control: about 0.1% of pixels differ by one 16-bit step; in the 8-bit
picture 1,314 pixels over 120 frames (about 11 per 2560x1440 frame), each by 1, all on edges in glowing areas; the
control against itself: 26 pixels over 120 frames. Westopolis (moving, tape): 17 pixels over 120 frames, 1 LSB. The
user accepted this difference (2026-10-26).

Tried and rejected, measured: (a) skipping zero-glare pixels (exact, no gain on Final Haunt); (b) evaluating the glare
at each sample's position (77% of samples then differ: the composite is per pixel); (c) seven per-sample rounding rules
(nearest-even, half-up, truncation, glare pre-rounded, fixed point with 0-12 fraction bits): the mismatch is unchanged
by all but truncation, because it is the GPU's sample collapse, not rounding; (d) a 256x256 R16_UNORM table of the
GPU's own UNORM16->float conversions for the per-sample values: no visible change, +0.7 ms resolve, removed.

## Thirty-first increment: plan B2 - one time base for frames, retrace and audio (2026-10-27)

What the periodic slowdown was. Not the GPU clock state Phase A suspected: on every stage, with input or not, the game
stepped two periods per frame for ~200 frames out of every 1,001 (33 ms game frames at frames 393-590, 1394-1591,
2392-2592). The console's 60 Hz video field is 525 half-lines x 1,287 ticks = 675,675 ticks (59.94 Hz); the game's
frame limiter put frames on multiples of 16,666 us (674,973 ticks) and also waits on retraces, so the two drifted
through each other once every 1,001 frames.

The fix (docs/FRAME_PACING.md, "One time base"):
- NativeFrameLimiter: frame k begins at time base k * 40.5e6 / hz (exactly 675,000 ticks apart at 60 Hz). The game
  still reads P = 16,666 us: elapsed = P x frames begun, so dt, step count and cap are unchanged.
- NativeVideo: the 60 Hz field is exactly one frame (675,000 ticks) and begins on the same grid (time-base aligned).
- NativeAudio: a DMA start's first pass ends on a multiple of the pass length (5 ms, 202,500 ticks); a pass end meets
  a frame boundary every 50 ms. (The pass is the game's 640-byte buffer at 32 kHz - not 1/60 s, measured.)
- The loading screen's draw (fn_802BE9D4) skipped while fewer than 16,683 us had passed since its last draw: li
  r0,0x412B = floor(675,675 / 40.5), the console field. With exact 1/60 s frames that check never passed after one
  frame, and loading draws slipped to every fourth audio pass (20 ms frames, 48 extra on the Westopolis load). The li
  is a new kind of platform site - a value site (ppcrecomp lowers a listed li to Cpu::platform_value; the
  interpreter applies the same table) - and NativeVideo answers display_field_us: the current field in whole
  microseconds (16,666 at 60 Hz, 20,000 at 50 Hz; the same formula gives the console's 16,683).

Measured (rendered Westopolis replay, 1920x1080 windowed, 90 s game budget):
- game frames after the stage hand-off: all 675,000 ticks (console timing: all 675,675; the beat shows in the game's
  own step count, not in packet spacing); the 23 PAL boot-field frames before the hand-off are unchanged (810,000);
- loading screen: 0 extra long frames (before the draw-site fix: 48 at 20 ms);
- displayed repeats: 2 (console timing 7; frame-locked without the loading fix 10);
- Final Haunt idle (earlier frame-lock run): repeats after 3 s 5 -> 0.

Suite 118/118 (test preset, Release); release ShadowTheHedgehog.exe 105449984 bytes, SHA-256
f4565c56a7e47f02c2dc428d5ce46392f4151c083320ff2d910f324041ad7687 (not deployed).

Rejected: keeping 59.94 Hz fields (the beat stays); a retrace phase offset (9 phases swept; the phase only moves
the retrace inside the frame - the loading throttle reads the field length). Not done: changing the limiter's P,
which would change the dt the game reads.
