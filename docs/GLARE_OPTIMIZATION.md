# Glare optimization (2026-10-27)

The first layer of the effect-by-effect optimization the user asked for (EFFECTS_INVENTORY.md). The glare's
semantics are GLARE_DISSECTION.md; this page records what was measured, what changed and what was rejected.

Method: the Westopolis tape (`.build/recordings/westopolis-cpu-spike`, `--boot-stage stg0100`), headless, 2560x1440
4x, frames 1800-1919 captured, every GPU pass traced (GEKKO_GPU_TRACE), diagnostic build, one binary for both arms
(`SHADOW_R2_GLARE_COMPUTE=0` is the old path), arms alternated new / old / new / old. Scripts:
`.build/validation/glare/{run.ps1, summarize.py, compare.py}`. Times are medians over the captured frames; the GPU
ran at low clocks headless, so compare arms, not absolute values with the presented game.

## Where the time goes (baseline)

| glare pass | ms |
|---|---|
| capture (sources -> level A, PSGlareCapture) | 0.39 |
| composite | 0.36 |
| resolve of the 4x sources (PSResolve) | 0.28 |
| sources draw | 0.06 |
| blur x, blur y | 0.05, 0.04 |
| box, merge | 0.01, 0.02 |
| total | about 1.2 |

The level chain itself (blur, box, merge on 170 x 128 and 85 x 64 texels) is nearly free. The cost is full
resolution: the resolve and the capture each read every source pixel, and the capture ran one thread per level
texel (about 21,700 threads looping over a 15 x 11 pixel footprint).

## G1: the capture in exact integer arithmetic (landed, default)

shaders/glare.hlsl CSGlareRows + CSGlareColumns, renderer.cpp capture_level_compute. A resolved source pixel is an
8-bit integer, and measured in 1 / level-size pixel units every footprint edge is an integer, so the level texel's
area average is one integer sum and one division: associative, so the summation order cannot change it, and no
rounding before the end. Rows: one 256-thread group per (the level columns that fit, one source row); neighbouring
threads read neighbouring pixels into group memory, then one thread per column sums its footprint. Columns: one
thread per texel sums its rows and divides. Bounds: a group's columns must fit 256 pixels and the sum 32 bits
(16.8 million source pixels); outside them the old capture runs.

| arm | capture ms | glare total ms |
|---|---|---|
| old (PSGlareCapture) | 0.397 / 0.299 | 1.235 / 0.874 (second run at a higher clock) |
| new (rows + columns) | 0.304 + 0.039 = 0.343 | 1.184 / 1.181 |

Pictures: new against old identical in 120 / 120 and 118 / 120 frames (2 single pixels, 1 step) - the same noise
as old against old (1-3 pixels, the known NVIDIA MSAA variance). About 13 % off the capture, 4 % off the glare.

## Tried and rejected (measured)

- **Resolve fused into the capture, float sums** (one compute pass reading the 4x samples): 0.47-0.70 ms against the
  old resolve + capture 0.68 ms - no gain; and its float summation order flipped one level texel by 1 step in 17 of
  120 frames, which the chain (level B's 4.7x gain, the blur's tap sums) spread into a 60 x 45 pixel patch of up to
  5 steps. That sensitivity is why G1 sums in integers.
- **Resolve fused, integer sums**: 0.80 ms (slower: multisampled loads in compute cost more than the pixel-shader
  resolve), and its integer resolve rounds differently from the render target's, so every glowing edge changed by
  1 step in every frame.
- **The driver's ResolveSubresource** for the sources: rejected before (renderer.cpp add_resolve; R2_DRIVER.md 4.9):
  it differs in the last bit between processes, which breaks byte-exact determinism.

## G2: the sources' tiles (landed, default)

The glare sources light 2.8 % of the pixels on this route, scattered: their bounding box is 40 % of the view, the
16 x 16 tiles they touch 4.6 %. So the GPU marks the tiles itself: in the glare-sources pass the draw pixel shaders
are GLARE_TILES variants (shaders/glare_tiles.hlsli; CMake's fifth shader field) that set their pixel's tile in an
R32_UINT texture (cleared by CSGlareTilesClear before the view's first sources pass). Then:

- the sources' resolve draws only the marked tiles (VSGlareTiles: one instanced quad per tile, an unmarked tile
  degenerate; PSResolve shades it) into a target cleared to 0 - exactly what the full resolve writes on a tile no
  source pixel reached, since it holds the sources' clear;
- CSGlareRows works per (columns, one tile row of 16 source rows): a group whose tiles are all unmarked reads
  nothing. One group per tile row instead of per source row (990 groups instead of 15,840 at 1440p) is also what
  made the rows step cheap without tiles.

Exactness: over-marking can only cost time (a marked tile is resolved and read as before), never change a pixel;
under-marking is impossible because every pixel a source draw writes runs the marking shader. Two things learned on
the way, both measured: a pixel shader that may write a UAV lost the early depth test, and the sources pass went
0.06 -> 0.27 ms, so the variants carry [earlydepthstencil] (the pass writes no depth, so testing early changes
nothing but the cost; hidden pixels then also stop marking); and a per-pixel tile check costs as much as the load it
saves, so the check is per group.

A/B on one binary (`SHADOW_R2_GLARE_TILES=0`, and both G1 and G2 off as the original chain), same clock state:

| pass | original | G1 + G2 |
|---|---|---|
| capture (rows + columns) | 0.381 | 0.086 + 0.037 |
| sources resolve | 0.274 | 0.087 |
| sources draw | 0.057 | 0.077 |
| tiles clear | - | 0.024 |
| composite, blurs, box, merge | 0.46 | 0.47 |
| **glare total** | **1.177** | **0.810 (-31 %)** |

Pictures: G1 + G2 against the original chain, 120 / 120 frames bit-identical; G2 against G1 alone 1-3 single
pixels (the NVIDIA noise both arms show against themselves).

## Final Haunt (glare nearly everywhere)

The Final Haunt tape (`.build/recordings/final-haunt`, `--boot-stage stg0604`), same window and method; the
original chain (both switches off) and G1 + G2 alternated twice, plus one G1-only run. Medians, ms:

| pass | original | G1 only | G1 + G2 |
|---|---|---|---|
| capture -> rows + columns | 0.47 | 0.133 + 0.039 | 0.097 + 0.039 |
| sources resolve | 0.285 | 0.286 | 0.105 |
| sources draw | 0.15 | 0.15 | 0.167 |
| tiles clear | - | - | 0.025 |
| **glare total** | **1.09 / 1.12** | **0.80** | **0.63 (-43 %)** |

The tiles pay even here. Pictures: G1 + G2 against G1 alone within the noise (23 single pixels over 120 frames, as
each arm against itself). G1 against the original chain: 6,500 pixels over 120 frames, 1-2 steps, of which 6,444
in one frame (1830) as one 126 x 132 pixel patch - one level texel on the other side of a rounding boundary, spread
by the blurs and level B (the G1-only arm shows it too, so it is the capture's arithmetic, not the tiles). The
integer capture is exact by construction; the original's float sum of ~170 terms put that texel on the other side.
Deliberate: the correct sum (evidence level STRONGLY SUPPORTED: patch shape and the G1-only attribution; the
texel's exact value was not dumped). The user accepted this difference and G1 + G2 as the default (2026-10-27).

## Next candidates

The composite (0.36 ms headless) is the glare's largest remaining pass; in presented frames it is folded into the
view's resolve when only 2D follows (B3a), so its cost there is mostly the resolve's. The level chain (blurs, box,
merge, about 0.15 ms) is six tiny passes whose cost is per-pass overhead; one compute pass per level would remove most
of it. Final Haunt (glare nearly everywhere) has not been measured with G1 + G2.
