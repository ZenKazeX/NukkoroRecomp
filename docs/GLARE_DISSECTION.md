# The glare (bloom), dissected from the PAL DOL against the native renderer (2026-10-27)

The native glare (render/shaders/glare.hlsl, renderer.cpp add_glare, scaffold/rw_driver/glare.cpp) cites
docs/effects-2026-09-21/BLOOM_SPEC.md, which is no longer in the tree. This document re-derives every step from the
disassembly (tools/doldis.py) so the implementation can be checked without it. G = the glare object
(EffectManagerPJS +0x40 chain, `layout::kEffectGlare`).

## The chain in the game

| step | where | what the code does |
|---|---|---|
| 1 save | PreGlare `0x80430D64`, copy at `0x80430FAC` | the finished scene is copied out; the EFB is cleared to transparent black |
| 2 rasters | PreGlare `0x80431028..0x804312AC` | level A = a 128 x 128 sub-raster, level B = 64 x 64 (`li r0,0x80` / `0x40`), each in a parent of (viewport offset + size); UV rects at G+0xA8.. / +0xC0.. = offset / (offset + size) .. 1 |
| 3 sources | (glare world, rm+0x24) | the glare-source draws land on black over the scene's depth |
| 4 capture | PostGlare `0x80430B58` copy into G+8; filter state 9 = 2 (LINEAR) at `0x80430B2C`, address state 7 = 2 | |
| 5 level A | ladder `0x8042F258`: quad over the 128 x 128 level sampling the whole source, blend ONE/ZERO (`0xA`=2, `0xB`=1), vertex colour = trunc(G+0xE5 x 0.95) (constant `0x8056BCF0`), alpha 255 | one textured quad: a single bilinear sample per level texel |
| 6 blur A | `0x804302EC(G, ..., taps_h=G+0xF0, taps_v=G+0xF4, spread=G+0xE8)` | per axis: a centre quad, then for i < taps a quad at offset (i/2 + 1) x s x spread - 0.5, s = +1, -1, +1, ... (constants `0x8051C420` 1.0, `0x8051C428` -1.0, `0x8051C424` 0.5); blend ONE/ONE; vertex colour 255; quads clipped to the level by `0x80430850` (nothing outside: a zero border); horizontal half first (f1 = x), then vertical |
| 7 level B | cascade (G+0xE1 set, G+0xE4 clear): quad from level A (128) into level B (64), vertex colour = G+0xE6 **raw** (no 0.95); from the sources (G+0xE4 set, or level A off): trunc(G+0xE6 x 0.95) | 2 : 1 bilinear at texel centres = the exact 2 x 2 box |
| 8 blur B | `0x804302EC` with G+0xF8 / +0xFC / +0xEC | as step 6 |
| 9 merge / composite | ladder `0x8042FA18..`, PostGlare | additive ONE/ONE quads (state 1 raster, `0xA`/`0xB` = 2/2): B magnified onto A (unless G+0xE3 separate), then the scene and the level(s) magnified (LINEAR) onto the sources |

Every intermediate is an EFB copy, 8 bits per channel, so each step clamps and rounds.

## The native implementation, step by step

| step | native | verdict |
|---|---|---|
| gains | glare.cpp `glare_params`: trunc(byte x 0.95) / 255, level B's cascade arm raw | exact |
| blur taps | PSGlareBlur: centre + taps at -(n/2 + 1) x side x spread, side from +1, horizontal first, zero outside | exact (same tap set, order, border) |
| level B from A | PSGlareBox: gain x 0.25 x 2 x 2 sum | exact (the 2 : 1 bilinear at centres is that box) |
| merge | PSGlareMerge: saturate(A + B magnified, centre on centre) | matches |
| composite | sources + levels magnified, ONE/ONE, saturating | matches |
| level size | (96 W/H, 128) texels: 128 x 128 at 4:3, the texel shape (H/96 x H/128 px) kept at every aspect | the original's level at 4:3; wider views keep its texel |
| **capture (step 5)** | PSGlareCapture: each level texel = the exact area average of its source footprint | **differs**: the game takes ONE bilinear sample per level texel (5 x 3.75 source pixels per texel at 640 x 480), so it reads only the pixels around the sample point and skips the rest |
| rounding | float, rounded to UNORM8 | GX TEV truncates; measured 99.2 % of blur pixels identical (gekkoforge-bloom-parity notes), the rest 1 LSB |

## The one structural difference: the capture filter

The game minifies the glare-source image into level A with a single bilinear tap per texel (filter LINEAR, no mip
levels): at 640 x 480 a level-A texel covers 5 x 3.75 pixels, and the tap reads the 2 x 2 around its centre. A
glare source narrower than the gap between taps contributes or not depending on where it falls, so small glowing
things shimmer in the glare as the camera moves; averaged over positions the energy is the same as an area average.
At the port's higher internal resolutions the same rule reads an ever smaller share of the image (2 of every ~8
columns at 1080p), so a literal copy of the tap would shimmer more than the original did.

The native capture integrates the footprint: the same mean, steady under motion, independent of the internal
resolution. This is a deliberate choice of the native implementation, not an accident; whether to keep it is the
user's decision (see the report of 2026-10-27).
