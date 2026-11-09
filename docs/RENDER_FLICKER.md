# Texture flicker: root cause, fix and the tools that found it (2026-10-27)

## The bug: glow shells raced the depth of their own model after a load

User report: in Final Haunt the locked spring's cage glows red, and right after a load the glow "blinks like hell, then
slows down until it goes away". Reproduced from the user's F5 snapshot (deterministic reload), 240 frames:

    before   ..#..#........#.........##........#...........#....#..#....#   (. = glow cap missing, # = lit)
             #.....#...#...##..#..#...#..#.##.##.#.#..#.#.#.#.#.##.#.#.#.
             ##.#.#.#.#.##.#.####.##.#.##.##.##.##.##.#####.##.######.###
             .###.###.###.###.####.####.####.####.#####.###########.#####   (last miss: frame 236)
    after    all 240 frames lit

Mechanism (measured):
- The cage (CASE.BIN, models CASEBK00-04 in stg_cmn_gdt.one) draws its glow walls (CASEBK04: environment map, a glare
  source, depth test on, no depth write, LESS_EQUAL) over the opaque cage (CASEBK02) with the very same cap vertices
  and the same frame matrix (bitwise). The glow cap is visible only where its depth equals the cap's own.
- The game re-reads its camera at every bracket, and the driver gives each pass that camera. After a load the game
  eases its far plane (Final Haunt: 3180.75, 3178.94, 3177.15, ... about 1.8 units a frame), and the frame's FIRST
  scene pass still carries the previous frame's far plane, while every later pass (the glare sources included) has the
  new one (material probe pass-camera series, frames 1-12).
- So the cap's depth was written with one projection and tested with another. Near and far change nothing on screen,
  only the depth value, by about 1e-7 here: in the 32-bit float depth buffer that rounds either way frame by frame. As
  the easing slows the misses thin out, and when the far plane stops moving the two projections match.

Fix (runtime/lift/render/renderer.cpp, add_draw_pass): depth is the renderer's own mapping, so every perspective pass
of a view maps eye depth with ONE near/far per frame - the view's first perspective pass's. Each pass keeps its own
camera for everything that shows (placement, clipping is the same up to the 1.8 units, fog works in eye depth). Any
surface the game draws twice (glow shells, decals over their model) now gets the same depth in both passes, at any
far-plane change - not only this cage.

Checked and NOT the cause (same snapshot and both tapes):
- Texture-pattern animations (0x8004F284 builds "%s.%d" frame lists, 0x8004F0FC picks the frame from a 60-per-second
  clock, RpMaterialSetTexture): authored pulses (s0604_zn061 etc.: 1..8..1, frame 8 held one frame by design), each
  zone's own set and material (14 sets over 330 s, never two on one material), clock exact.
- RpUVAnim (TextureUVAnimManagerTask -> 0x801EBA74 -> 0x80039BBC -> per material 0x804559B4 / 0x8045567C): an
  effect's two-step UV flip, authored.
- Coplanar overlap of different surfaces: GEKKO_DEPTH_TEST=less finds 0-5 pixels per frame at the spot.
- Particles: the PTanks that correlated with the spikes are effects of the same frames, not the glow.

## Second bug: glow quads 0.005 units over their wall z-fought (GUN Fortress corridor lights)

User report: in GUN Fortress's first corridor the red wall lights "blink". Recorded with F8 markers (85.5-89.2 s of
`.build/play/gun-fortress/input.tape`), replayed to the marker, then a synthetic tape (`strafe.tape`: Shadow steps
left / right, camera untouched) captured every frame (`.build/markers/gf-variant.sh`).

- The pulse itself is authored: texture pattern s0600_zn055 (period 64, keys 1..8..1 every 4 frames; frame 1 is pure
  black in stg0600_texCM.one, so the lights go dark for 6 frames each second).
- The blinking on top of it was ours: within a key the lit area jumped by up to 45 % for single frames, whole triangles
  of a light missing with sharp diagonal edges.
- The lights exist only as glare sources: world sector 813A5734 (an AF sector) mesh 1, additive, depth test
  less-or-equal, no write, drawn over the scene's depth. SHADOW_R2_DUMP_SECTORS measured its quads against the scene
  sectors: most lie 0.5-1.9 units in front of their wall, several only 0.004-0.005 (one 0.0009), at 100-265 units
  from a camera with near 1 / far 2000.
- The conventional mapping (depth = f (z - n) / ((f - n) z)) stores 1 - n / z: next to 1.0 a float step is 6e-8,
  0.004 units of eye depth at 250 units - the same as the margin, so the per-vertex rounding decided triangle by
  triangle and frame by frame.

Fix: reversed depth (render/frame_math.h kFarDepth): near maps to 1, far to 0, depth clears to 0 and draws pass
GREATER_EQUAL (D3D12 and render_vk); canvas depth is 1 - z. The buffer then holds n / z, whose float steps are relative:
1e-5 units at 150 units. Reversed depth is exactly 1 - the conventional depth, so every order and every equality of the
old mapping is kept; only the precision changes.

Verified (release build): the light's lit area now steps exactly with the pattern keys (10/11/22/31/38/45/55/73 k
pixels, 4 frames each, no dips); frames with the lights off differ from the old build in ~40 of 2.07 M pixels; the
Final Haunt cage snapshot (240 frames) matches the previous fix within 2 pixels a frame; CTest 118/118.

Diagnostics added: `SHADOW_R2_DUMP_SECTORS=<dir>` writes every world sector's imported mesh as `<dir>/<sector>.obj`;
`SHADOW_R2_TRACE_FRAME` now also lists the scene's sector draws (tagged `[scene]`).

## Third: the corridor lights against their painted slots (retail data; deliberate deviations)

The retail game (Dolphin-checked by the user) shows these authoring defects in GUN Fortress's corridors; all are
fixed in the port as DELIBERATE DEVIATIONS, in the data the port draws, from measurements of the disc:

The walls (texture *_zn013_mp) carry a black slot painted on every texture tile (u 0.25..0.75, v 0.875..0.9375);
each slot's blinking light (texture pattern *_zn055.1..8) is a separate quad in the stage's glare worlds
(*_G_NN.RG1), laid over it by hand. tools/stage_preview/world_dump.cpp dumps every world of a stage from the disc;
tools/assets/light_slots.py matches lights to slots one to one in the walls' texture coordinates. stg0600: 576 slots,
568 lights.

1. Lights at or behind their wall: 19 lights lie on, across or behind the wall they light (down to 0.094 units
   behind); the glare sources test the scene's depth, so the wall's own depth hides them, wholly or in part, and
   with the old conventional depth they flickered as it rounded either way (light 11 of the first corridor; the
   light the user marked going dark near the screen edge at 75 units). The other 549 lie clear of their walls (most
   0.25..0.35 units). Each faulty light is set flat at the offset its sound neighbours on the same wall have (their
   median; the nearest sound light on a parallel wall when the wall has none).
   (An earlier attempt lifted every glare source's depth test by a tuned amount, first a fraction of the distance,
   then 0.05 units. It masked the data and missed the 0.094 one; it is gone.)
2. Lights off their slots: 418 lie off along the wall (up to a fifth of a panel, 12 units). Every vertex is moved so
   the light's edges land on the slot's, its height kept.
3. Slots without a light: 8 (x -21858..-21716, both walls). Each gets a copy of the nearest sound light of a wall
   facing the same way, laid into the slot.

Runtime: runtime/lift/assets/light_slots.{h,cpp} with the generated table light_slots_table.inc (1834 vertex moves,
8 added lights), applied when the port builds a glare sector's mesh (scaffold/rw_driver/pipelines.cpp; positions are
the disc's floats, matched exactly; the lights are only drawn, never collided with). stg0801 also has these walls,
but its lights are half a slot wide and centred on every wall alike - its own design, left alone.

Verified: asset_import_tests checks the table against the disc (1834 vertices moved, 8 lights added); the user's two
F8 markers: the second corridor's light fills its slot; the light that went dark near the screen edge stays lit
through an 11-second camera pan (every frame lit except the pattern's own all-dark frames); the first corridor's light
11 shows whole. The 8 added lights lie past the recorded tapes - not yet seen in a capture.

## Fourth bug: the wall boxes' glow placed one frame behind its box (Final Haunt; deliberate deviation)

User report: in Final Haunt the boxes that slide out of the walls near the vacuum gun blink by design, but on a moving
box the blink "gets crazy and starts vibrating". Recorded with an F9 range (`.build/play/final-haunt-uv`, range 1,
171.4-180.8 s; the second box near its end). Not a UV animation: the material probe shows no UV toggles there.

Mechanism (measured; `tools/native_watch.py` hardware write watches, `--watch` function-entry watches, SHADOW_R2_TRACE_FRAME):
- The glow is a separate model: an `Effect::PJS::EffectParticle3DPJS` (its draw 0x80027DC4 -> fn_804341D0 writes the
  glow's root frame from the effect's emitter matrix, 0x804342DC..0x8043431C), drawn depth-tested without depth write
  over the box (s0601_sn009 over s0601_sn008).
- The object attaches it: every object's update starts with the shared base step 0x800E5280, which runs the attach
  step 0x80167A00 (RwFrameGetLTM of the attach entry's frame, transposed into entry+0x14, handed to the effect's
  SetMatrix 0x80426EB8). Only afterwards does the object move its model (0x801637C0 -> RwFrameTransform 0x80488738,
  175 call sites). The effect's update 0x80426C84 runs later and builds its emitter from that matrix.
- So the glow always carries last frame's transform: on frame 151 the box is at z -16102.9, its glow at -16103.3 (on
  150: -16103.3 and -16103.8). Along the faces parallel to the motion both lie in one plane, drawn from differently
  placed vertices: the depth tie flips per triangle and per frame - the glow vanishes, or shows cut along a diagonal.
  A box at rest has a glow with the bit-identical transform and never flickers.
- This is the original's order (the native scheduler walks the task tree exactly as the original does), so the retail
  game places the glow one frame behind as well.

Fix (DELIBERATE DEVIATION; runtime/src/gekko/native_attached_effects.cpp, contract in its header): the native owner of
the effect's SetMatrix (0x80426EB8). Called by the attach step, it does what the original did and also points the
effect's own follow-matrix field (+0x48, which the effect's update already honours at 0x80426CA4) at the attach
entry - only for an effect that follows nothing else. The effect's update then asks for that matrix after the object
has moved, and the owner takes the frame's local-to-world matrix as RenderWare's sync produces it at that moment (the
root's modelling matrix, children by RwMatrixMultiply 0x8047FC08 ported operation for operation in fused single
precision), clears the field, and the effect proceeds as before. No depth code changed: an earlier depth tolerance for
overlays was withdrawn (the user's ruling on tuned values).

Verified on the recorded range: the trace places box and glow at the same position on frames 150 and 151; the
material probe's shells section (overlay vs depth-writing draw of the same rotation within 2 units) goes from
"s0601_sn009 over s0601_sn008: 1384 exact / 386 apart, max 0.417 units" to "1770 exact / 0 apart"; the same frames
146-153 before/after show the glow on the box every frame (`.build/uvanim/glow-before-after.png`).

## The tools (all diagnostics; inert unless enabled)

- `GEKKO_MATERIAL_PROBE=<report>` (scaffold/rw_driver/material_probe.*): per-draw A->B->A detector over every field
  of a draw's material and recipe; `<report>.particles.txt`: per frame, particle batches/particles per texture NAME
  (s = scene, g = glare sources) and the first view's scene/glare pass cameras (far plane + bit hash);
  `<report>.texanim.txt` with `GEKKO_TEXANIM_TRACE=<base names>`: texture-pattern definitions and their per-frame clock.
  The report ends with the texture handle -> name table (handles follow creation order and differ between builds).
- `GEKKO_DEPTH_TEST=less`: the overlap map (docs/tools/REPLAY_MARKERS.md "Overlap").
- The material probe's "shells" section: every overlay (depth tested, no depth write) drawn with a depth-writing
  draw's rotation within 2 units of it, by texture pair - frames exactly on it and frames a little apart (largest
  offset, last place). "apart" frames are shells placed off their model: they tie in depth by rounding.
- F5 snapshot -> `--load-state` reloads one moment deterministically; `SHADOW_R2_TRACE_FRAME=<n>` traces its draws.
- Replay markers (F8, `tools/replay_markers.py`): docs/tools/REPLAY_MARKERS.md.
