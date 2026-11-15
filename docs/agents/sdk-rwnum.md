# sdk-rwnum (2026-11-12): the five "numeric" entries inside 0x8049A124

Branch agent/sdk-rwnum, cut from b75201a. Contract: docs/agents/SDK_2026-11-12.md, row sdk-rwnum, group D.

## RE finding (corrects the contract's premise)

The body at 0x8049A124 (0x3E4 bytes) is **not numeric and not RNG code**. It is `GcnRasterClearRect(_, rect*, pixel)` of the
RenderWare GameCube driver, which is already an exact native owner (native_rw_raster.cpp, `kClearRect`, extract/native-owners.txt
line `0x8049A124 GcnRasterClearRect`; the external decomp agrees on "RenderWare raster code", reference only). Evidence from the words:

- Prologue saves r28..r31 and LR in a 0x30-byte frame; r28 = device, r29 = rectangle, r30 = pixel, r31 = "this call locked the raster".
- It reads the current raster `*(r13 + 0x6210)`, dispatches on the raster type byte (`+0x20`: 0, 4, 5 CPU fill; 1, 2 go to
  ClearQuad 0x80499E58; 3 and >= 6 report error 0x8000000D), locks (RwRasterLock 0x8048AFC0) unless private flag 0x04, and then
  **switches on the raster depth** `*(raster + 0x14) - 4` (cmplwi 0x1C, jump table at 0x8056FC1C, 29 words).
- The table has five distinct targets, which are exactly the five "entries": depth 4 -> 0x8049A304, depth 8 -> 0x8049A364,
  depth 16 -> 0x8049A3B4, depth 32 -> 0x8049A41C, every other depth -> 0x8049A480.
  - 0x304 (4-bit): pixel nibble in both halves of a byte, `memset(pixels + stride*(y+i) + (x>>1), byte, w>>1)` per row.
  - 0x364 (8-bit): `memset(pixels + stride*(y+i) + x, pixel & 0xFF, w)` per row.
  - 0x3B4 (16-bit): `sth` of the low halfword, w times, at x*2.
  - 0x41C (32-bit): `stw`, w times, at x*4.
  - 0x480: `RwErrorSet(0x8000000C)` (through the variadic 0x8047EC78 + 0x8047EBD4), nothing filled.
  - all arms fall into the common tail 0x4A4: unlock the raster (RwRasterUnlock 0x8048AB90) if r31, `r3 = 1`, epilogue, `blr`.
    (The depth error answers 1 as well.)

**Why the game's Random functions "enter" them (a false edge).** Random::NextFloat 0x8040FB60 and NextFloatInclusive 0x8040FD70
are a plain MT19937 step + tempering + `(double)(0x43300000:u32) - 2^52` scaling; their only control transfers are `blt`/`bge`/`bdnz`
inside their own extent and one `blr` (scanned with tools/doldis.py: no `b`, `bl`, `bctr`). The generator's edge Random -> arms comes from
its conservative lis/addi pairing (ppcrecomp "formations (H6): ... kept the conservative pairing"): at 0x8040FBC4 `lis r4, 0x8057`,
then r4 is overwritten (`slwi r4, r8, 2` at 0x8040FBD0) but the pairing still forms `0x80570000 + (-0x388)` at `addi r0, r4, -0x388`
(0x8040FBE4, 0x8040FDF4 in the twin) = 0x8056FC78, a word INSIDE the jump table (0x8056FC1C..0x8056FC90). A row that "can form an
address inside a verified jump table" is made a reader of it, so Random gets an edge to every table target (live graph:
`edge 0x8040FB60 0x8049A304 ...` for all five). Nothing in the RNG executes these arms; the census (test_data/platform/gx_rw_frontier.csv,
rows 127-132) agrees: 0 hits on the three recordings.

Decision (evidence): own the five arms (the generator's owner-entry rows), not the Random functions. Owning Random would touch the hot
gameplay RNG to remove an edge that is false, and it would not make the arms' words disappear any faster; owning the arms is
inert for the RNG (Random is not touched at all: its words, its callers, its numerics unchanged). No generator change was needed.

## What was added

| file | change |
|---|---|
| runtime/src/gekko/native_rw_raster.cpp | `clear_arm()`: each arm as the CONTINUATION it is - entered with the body's registers (r29 rect, r30 pixel, r31 unlock flag, r1 = the function's frame): fill exactly as the arm does (raster context, stride, pixel pointer, rectangle and loop bounds re-read from guest memory every row/column as the original does), the common tail (RwRasterUnlock through guest_call when r31 != 0), r3 = 1, the epilogue (r28..r31 and LR from the frame, r1 += 0x30). Installed with `install_hle` at the five addresses; `kAddresses` lists them. A negative byte count (arms 4 and 8) would make the original's memset write ~4 GiB: the owner stops (`stop`, named). Link literals 0x8049A4B4 / 0x8049A4A4 are `// liveness-roots: value-only`. |
| extract/native-owners.txt | one appended block, five rows (0x8049A304, 0x8049A364, 0x8049A3B4, 0x8049A41C, 0x8049A480) |
| extract/liveness-roots.txt, extract/liveness-roots-lifted.txt | regenerated with the tests' exact arguments (+5 owner addresses each; the link literals add nothing) |
| runtime/tests/rw_raster_contract_tests.cpp | `test_clear_arms` (also `ONLY_TESTS=arms`) |
| CMakeLists.txt | SHADOW_GC_REMOVAL_RATCHET (lifted branch) 41 -> 11, comment line |

## Gates

**1. Contract test against the pinned DOL (reference interpreter).** `rw_raster_contract_tests`, `test_clear_arms`: the ORIGINAL words are
entered at each arm with a prepared frame (saved r28..r31 sentinels, LR = finish stub), against the owners; compared: r3, r1, LR, r14..r31,
every guest word written outside the stack (address and value), the lock/unlock doubles' logs, the RwErrorSet record, draw-sync state.
Grid per arm: 6 x 8 x 4 x 5 rectangles (x, w, y, h with zero/odd/negative values) with pixel/stride/flag cycled; the full pixel (7) x stride (4)
x flag (3, incl. 0xFFFFFFFF) product on 4 rectangles; 1,500 random rectangles. Result: **13,612 cases equal, 368 owner-only refusals** (negative
byte counts, where the original never ends), 5,902 unlocks, 2,796 depth errors, 175,723 pixel words filled. The exhaustive input space
(2^32 pixels x rectangles x strides) is not enumerable: the 32/16/8/4-bit fills are per-element functions of the pixel, covered by 7 edge pixels + 1,500 random
per arm; rectangle arithmetic by edge and random geometry. Negative controls (one code word of the original patched; the comparison must
fail): 8 of 8 told apart (nibble copy -> nop, fixed fill byte, low-byte halfword, word 4 bytes late, other error code, unlock test against 9,
answer 2, r28 restored from the wrong slot), plus the unmutated control equal. The full `rw_raster_contract_tests` exe ran (all earlier
sections unchanged, equal); the first run's only FAIL was my own miscalibrated case-count threshold (>15,000; fixed to >13,000; ctest `rw_raster_contract_tests` then PASS, 574 s at the 10% cap).

**2. what_if prediction, then the real generator run.** `what_if.py --own 0x8049A304,0x8049A364,0x8049A3B4,0x8049A41C,0x8049A480`: frees 0 pruned
rows, 0 revived, 0 stale sites (the five rows turn from `body` into `owner` rows: they stop being compiled, no row is pruned). Planner prediction
(live graph with the five owned, platform bodies compiled): 16 -> **11**, the five rows gone, none new. Real run on the new shipping tree
(.build/watch, lifted): `removal_plan.py --release-dir .build/watch --out .build/platform/rp_watch --dolrecon-dir .build/dolrecon
--max-platform-bodies 11` -> "platform bodies compiled 11, frontier 8, left if all frontier owned 0" (graphics.rw-gamecube-driver 9 -> 4,
sdk:unidentified 7). `--max-platform-bodies 10` fails as it must. Tests tree (non-lifted): 420 -> 415 compiled bodies (its ratchet 443 is untouched).
Ctests in the tests tree: `platform_gc_removal_plan` PASS, `native_file_api_tests` PASS (owner list = installed list), `native_liveness_roots`
and `native_liveness_roots_lifted` PASS. `platform_runtime_coupling` FAILS - **pre-existing, not mine**: identical output (1 unclassified use, 3
shipping uses of 0x8037ECD4 at native_vi_wait.cpp:32/45/59) from the coordinator's c81197c tests-tree plan with my branch's script (it is on
docs/AGENT_COORDINATION.md's baseline failure list); my change adds no runtime SDK call.

**3. Westopolis replay (behaviour gate).** tools/behaviour_run.sh, base-b75201a.exe vs my .build/watch exe (same options, 316 game s, headless,
10% cap, 2 runs each): `behaviour_diff.py base cand` -> "no field differs in any bit over 6320 rows; events identical (1521)", VERDICT PASS (both pairs; base A/A
also PASS). state.json md5 3a75268e85cd9037a60d04f739f10686 in all four runs (identical, and equal to opt-behave-r3's value). The arms are unreachable in the
replay by construction (the owner serves 0x8049A124 whole; census hits 0; Random has no transfer into them), so equality is the expected result, not a
demonstration that the arms ran; their behaviour is proven by gate 1.

**4. Liveness lists.** Both regenerated with the native_liveness_roots(_lifted) tests' exact arguments, `--check` rc 0 for both.

## Open / notes

- No speed claim (gate 5 not run: nothing on the hot path changed; the five thunks are never entered).
- The arms are served as exact continuations although nothing reaches them: the generator's reachability is what retires the rows, and a served
  (not stopped) entry keeps the "an owner serves the call completely" rule even for a future caller that enters mid-function.
- Not done by me (other rows of the table): AXFX callbacks, GX tex-region callback, the three RW table callbacks, 0x8049AC88.
