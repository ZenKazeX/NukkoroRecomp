# Native coverage audit (2026-11-02)

Independent recount of how much of Shadow the Hedgehog's original PowerPC code the current build has replaced with
native C++. Reproduce everything with:

    python -B tools/audit/native_coverage.py --skeleton ../source/.build/dolrecon/skeleton/functions.csv \
        --run-log .build/ab-jobs/audit/out.log --samples .build/ab-jobs/samp-audit.csv \
        --map .build/watch/bin/Release/ShadowTheHedgehog.map --json .build/audit-native-coverage/audit.json

Build audited: worktree wt-gc-removal (uncommitted state of 2026-11-02 evening), release tree .build/watch
(ShadowTheHedgehog.exe built 16:41), DOL game/sys/main.dol (PAL). Runtime sample: Westopolis tape, 90 s
(.build/ab-jobs/audit).

## Method (ground truth = the generator, not a registry)

1. The release build's exact ppcrecomp command (from .build/watch/generated/ensure_outputs_Release.cmake) is run with
   `--annotate-lines`, which tags every emitted guest instruction with its address. Unique tagged addresses = the
   original instructions the executable still contains as translated code.
2. The same command with every native list emptied (both `--native-owners` lists, all four `--lift-sites` lists,
   `--platform-sites`) gives what the original game compiles to with no native replacement under the same liveness
   rules: the denominator.
3. Every word of the DOL's text (1,216,776) is classified: translated (emitted now), owner body (emitted originally,
   not now, inside a function whose start is a native owner), freed (emitted originally, not now, elsewhere), never
   live (not emitted even originally).
4. Layers come from the dolrecon skeleton (34,037 functions, no overlaps, 4 gaps of 22,336 bytes - 14,144 of them the
   gap between the two text sections; the rest 2,054 words of padding / data at 3 places).

Checks: nothing is emitted now that was not emitted originally (0 words); the fresh script run reproduces the earlier
scratch runs exactly; the generator's own report agrees (owners 162 rows, 728 pruned rows); every one of the 38,223
generated function bodies is linked into the exe (its link map), none removed after generation.

Update 2026-11-03 (native SJ, docs/CRI_SERVERS.md 4b; skeleton with the spine's A5 / S3 / G1-guard / S1b rules):
translated 1,172,764 (96.56 %), owner bodies 10,667 (0.88 %; native 10,454 = 0.86 % + 213 stop-only), freed 31,133
(2.56 %); CRI 4.45 % native owner bodies; unclassified 86 functions / 4,887 instructions
(.build/audit-native-coverage/audit-sj.json, static only). After the native ADX output (docs/CRI_SERVERS.md 4c):
translated 1,172,328 (96.52 %), owner bodies 11,103 (0.91 %), CRI 4.95 % native (audit-adx.json). The tables below
are the 2026-11-02 audit.

## Corrected static totals (denominator: 1,214,564 original live instructions)

| class | instructions | share |
|---|---|---|
| still translated PowerPC (emitted) | 1,174,713 | 96.72% |
| removed because of native work | 39,851 | 3.28% |
| - native owner bodies (a native implementation serves every call) | 8,505 | 0.70% |
| -- of which platform / game implementations | 6,735 | 0.55% |
| -- of which GameCube-contract compatibility (interrupt delivery, VI, AX / DSP, GX sync) | 1,770 | 0.15% |
| - stop-only owners (halt if reached: console-only paths) | 213 | 0.02% |
| - freed (no longer reachable: native code replaced their callers / roots) | 31,133 | 2.56% |
| not live even in the original (padding, data, dead words) | 2,212 | (not in the denominator) |

Within the translated 96.72%: 2,627 instructions (19 functions: RwMatrixMultiply, CharaColli tests / queries,
__dynamic_cast, camera visibility, world collision, 64-bit divides, ...) are served by native code at runtime on
every call that does not decline, but stay compiled as a fallback. They are native at runtime and translated in this
static count (0.22%).

## The previous dashboard vs corrected

| metric | dashboard | corrected | cause |
|---|---|---|---|
| native owners | 7,824 (0.64%) | 8,505 (0.70%) native + 213 stop-only | read only extract/native-owners.txt: missed native-owners-r2.txt (the lock-on mark draw and lens-flare build game owners, 895 instructions); counted stop-only owners as native |
| removed | 36,254 (2.98%) | 31,133 freed + 213 stop-only (2.58%) | summed recomp_pruned.csv `reachable_instructions` per row; generator rows overlap (extract/functions.csv: 2,573 overlapping rows), so overlapping pruned rows were counted twice; rows dead in the original (598 instructions) were counted as progress |
| translated | 1,170,644 (96.37%) | 1,174,713 (96.72%) | computed as a remainder of the two errors above, over skeleton extents that include 158 never-live words |
| total | 1,214,722 (skeleton extents) | 1,214,564 (originally emitted) | |
| out of translated code, total | 44,078 (3.63%) | 39,851 (3.28%) | net overstatement 0.35 points |

So the 0.6% headline was close to its own definition (native-owned function bodies over all functions) - corrected
0.70% - but the "removed" figure was inflated by double counting, and the overall 3.6% becomes 3.28%.

## Per layer (share of each layer's originally emitted code)

Layers from the dolrecon spine as reclassified on 2026-11-03 (source/docs/GAME_SPINE.md "pal-v30, spine rules A5 /
S3"): unclassified code went from 1,795 functions / 63,601 instructions to 61 / 3,286. The totals above do not
depend on layers and did not change.

| layer | original | translated | owner bodies | freed | why |
|---|---|---|---|---|---|
| game (gameplay classes) | 721,225 | 99.05% | 0.25% | 0.70% | owners: lens flare + lock-on mark (642), SE manager 378, frame limiter 185, motion store 117, object range 106, directory scan 80, game memory 77, sound banks / tables 113, disc error 34, GCAX MIDI 33, link groups 17; largest freed: fn_803B1108 (917), AudioManagerTask paths (392), the disc-error screen module (0x8032D4CC..0x8032DB70) |
| game-unit (game object files: game code or a template / std instance emitted there) | 205,639 | 99.83% | 0.12% | 0.05% | owners: part of the lens-flare / lock-on owner extents (253); freed: 5 small functions |
| RenderWare | 107,989 | 98.80% | 0.47% | 0.73% | owner: RtGCNFS file system (505); freed: its asynchronous read / error paths (0x8049xxxx) and a few driver helpers; now includes the plugins rpcollis / rphanim / rpptank / rpskin / rpuvanim / rpmatfx (+27,380) |
| CRI middleware | 86,775 | 97.80% | 2.20% | 0.00% | SVM (871) + file device (1,035); ADX / Sofdec decode untouched |
| unclassified | 3,286 | 87.49% | 0.00% | 12.51% | 61 functions with no evidence for one layer (list in GAME_SPINE.md) |
| GameCube SDK | 53,622 | 46.00% | 7.93% | 46.07% | owners: file API 936, IPL / EXI 677, AX 646, GX sync 510, VI 403, card 361, reset 323, interrupts 299, reverb 65, memory 32; largest freed: AudioGCN init path (1,533), DVD / EXI / OS internals, AXFX reverb callback |
| Boost (shared_ptr etc.) | 22,563 | 100.00% | 0 | 0 | untouched |
| C library (MSL) | 12,540 | 99.77% | 0.16% | 0.07% | owner: memcpy (20); the native math cut-in's pruned bodies are counted where the skeleton files them (freed) |
| runtime (crt / startup) | 924 | 95.02% | 0 | 4.98% | |

## Native work the static count does not show

- Lowered call / store sites inside translated functions: 1,719 call sites (lift-sites: 12 + 1,018 math + 13 native +
  676 renderer) and 1,527 GX pipe stores (1,487 lowered) run native code in place of the original instruction; the
  surrounding functions count as translated.
- Hardware replacement: the D3D12 renderer (runtime/lift/render, native_gpu.cpp, plume), audio mixer / reverb / ADPCM,
  host file system, memory card, savestates, presentation. These replace console hardware, which has no instructions
  in the DOL, so no static instruction metric can show them.
- Native C++ written (line counts, headers included, tests excluded; file-name heuristic): about 103,000 lines of
  engine / game / hardware replacement, 16,900 of execution machinery (recompiler runtime, guest memory, host loop),
  9,400 of diagnostics, 677 of bridges (native_cri.cpp, native_jobs.cpp - OFF by default, not counted anywhere above).

## Runtime coverage (Westopolis tape, 90 s, game thread, GEKKO_SELF_SAMPLE every ~0.56 ms)

| state of the game thread | samples | share of all | share of busy |
|---|---|---|---|
| idle (waiting for the next event: 60 Hz pacing, guest idle loop) | 136,543 | 81.9% | - |
| translated guest code (generated bodies + their inline helpers) | 20,564 | 12.3% | 68.3% |
| native C++ (owners, intrinsics, interrupts, audio mix, collision, scheduler) | 5,414 | 3.2% | 18.0% |
| execution machinery / host loop (dispatcher, HLE lookup, pacing loop) | 4,138 | 2.5% | 13.7% |

Calls: 13,533,053 guest calls were served by native handlers in the 90 s (6,338,990 by native owners; 5,531,572 by the
19 kept-compiled intrinsics above; the rest by continuation entries and interior hooks). The renderer, audio output
and PRS workers run on other threads and are entirely native; the sampler does not cover them (process CPU 22.5 s vs
about 16.8 s of game-thread busy time estimated from the samples).

## Remaining guest-runtime dependencies inside native code

19 continuation entries (owners file `entry:` lines) through which native code calls guest code; in
runtime/src/gekko/native_*.cpp, game_memory.cpp and runtime/game: 23 call_guest, 20 leave_to_dispatcher, 4 upcall_call,
7 call_indirect uses, ~480 guest-memory reads and ~490 writes, ~470 guest-register uses. The GameCube OS scheduler
(threads, SelectThread) is still translated guest code; the native interrupt / VI / AX / GX-sync owners (1,770
instructions) implement console contracts for it and the remaining guest code - native code, but GameCube-shaped, to
be retired as their consumers become native.

## What "0.6%" actually measured

The share of instructions inside functions listed in extract/native-owners.txt, over all skeleton function extents.
It is a static, unweighted measure of "functions whose bodies native code replaced outright". It is not runtime
coverage, not engine completeness, and not the amount of native code written. Under that definition the corrected
value is 0.70% (0.72% including the five stop-only owners).

## Recommendations for the dashboard

1. Use the generator's annotated emission (this script) instead of summing pruned-row counts: rows overlap.
2. Read every --native-owners list from the release command, not one file.
3. Report the denominator as originally live code (no-native generator run), not skeleton extents.
4. Split owners into native implementation / GameCube-contract compatibility / stop-only; never count bridges.
5. Show kept-compiled native intrinsics and lowered sites as their own rows.
6. Always pair the static number with a runtime split from a sampled replay, and track a per-function executed-set
   (which translated functions actually run) - static reachability includes much code a replay never executes.
