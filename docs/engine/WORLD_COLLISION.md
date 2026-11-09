# World collision: the game's line picks and sphere queries

Status 2026-10-31 (Phase IX part 2): every world query the game makes - line picks and sphere queries - runs as native
engine code below RenderWare's dispatcher, `runtime/src/gekko/native_world_collision.cpp` (header
`native_world_collision.h`), on by default. The earlier owner of the line query alone (`native_world_line.cpp`, Phase
IX part 1, PowerPC-exact helpers) now runs only when the native collision owner is switched off.

## The execution chain (static + runtime census)

Every world query reaches RenderWare through the dispatcher fn_80443AF4, from exactly two wrappers (call sites from
`tools/dolxref.py`; no address of these functions is materialised anywhere, so there are no indirect callers):

```
game: 16 callers of fn_80021E24, 22 of fn_80021FE4 (player ground 0x80097EC0 ~60 % of picks, enemies 0x801A64C8 ~20 %),
      7 of fn_80021CA4 and fn_80021934 -> fn_80021480 (sphere: player 0x8009741C / 0x800985F0, enemies 0x801A62A4, ...)
  fn_80021E24 / fn_80021FE4   for each loaded stage block (u16 id list at obj+0x828, RpWorld at block+0xB8):
                              pick, then shorten the line to the hit (later blocks only find closer hits);
                              flag bit 2: also the object/character query 0x800C5D20 / 0x800C615C (native since
                              2026-10-28, chara_colli_intrinsic.cpp); the previous result object is deleted
    fn_80021678               LINE PICK: line from two vec3s; [r13+0x2BF0] = filter, [r13+0x2BF4] = filter data;
                              t_min = 1e7; profiler stubs 0x80037A04 / 0x80037A08 (empty); on a hit: point = S + t D,
                              distance, three result objects (0x6C face from the record at 0x80571CCC, 0x48 contact,
                              0x24 result)
  fn_80021480 / fn_80021CA4
    fn_800215C0               SPHERE QUERY: sphere {centre, radius}; [r13+0x2BF0/4] = filter / data; [r13+0x2BE4] =
                              the result list it returns, [r13+0x2BE8] = keep hits behind the plane
      fn_80443AF4             RpCollisionWorldForAllIntersections dispatcher: type 1 line -> fn_804421D8,
                              3 sphere -> fn_80443260, 5 atomic (bounding sphere) -> fn_80443260; returns the world
        fn_804421D8           the line query (part 1 below)
          0x80020DC0          the only line callback: surface attribute = the sector's first user-data array
                              [triangle index] (when any array has data); filter; if t < t_min: t_min = t, record ->
                              0x80571CCC, attribute -> [r13+0x2BEC]; always returns the triangle (never stops)
        fn_80443260           the sphere query (below)
          0x80020C24          the only sphere callback: attribute; filter; drop hits with distance < 0 unless
                              [r13+0x2BE8]; then the result list fn_80020D40 and the result builder fn_800201D4
                              (closest point on the triangle, a 0x6C face object, a contact); never stops
            filters           mask tests on the attribute (the record's +8 is always 0: both callbacks build it from
                              all-zero templates 0x804AB880 / 0x804AB894): player 0x80099454 (reject data[1], need
                              data[0]); 0x801A658C (need 0x1AB3); 0x801C80C0 (reject 0x00100108); 0x801C80A4 (reject
                              0x00050008); 0x80222138 and 0x8005B6C8 (reject 0x000C0018); 0x8006DDA8 (reject 0x40000,
                              need 0x1A93); 0x800F9394 (data != 0); 0x801C0910 (need data[0]); 0x803350F0 (need
                              data[2]; seen only in Doom's Base, stg0601); none
```

**Early termination and nested queries are unreachable**: both callbacks return their (non-NULL) record on every path,
and guest code inside a query is only an unknown filter or the sphere result builder; no nested query was seen on 15
stages. The NULL-stop paths stay implemented and are runtime-unproven. The guest-filter path is runtime-proven: on
stg0601 the then-unknown filter 0x803350F0 ran as guest code 3,328 times inside verified queries, identical (it is
native now).

## The sphere query fn_80443260 (from the disassembly)

1. Boxes: the sphere's box C +- R; the cube inscribed in the sphere C +- 0.57 R (0x3F11EB85 ~ 1/sqrt 3).
2. World BSP (explicit stack): if the node's right child lower bound (+0x14) > box max: left only; else the right
   child, plus the left child on top when its upper bound (+0x10) >= box min.
3. Sector: the tree's bounding box (RpCollTree +4 sup, +0x10 inf); the squared distance from the centre (per axis
   below inf / above sup / 0; fused accumulation) must be <= R^2, and the six box overlaps must hold. Root clip flags:
   bit 2a when inf < the inscribed cube's min along axis a, bit 2a+1 when sup > its max.
4. Collision tree with {descriptor, flags} entries: a node with flags 0 lies in the inscribed cube, so its whole
   contiguous entry range (leftmost leaf start to rightmost leaf end) is reported without tests; otherwise the same
   box split as the BSP, the pushed left child clearing its high bit when its upper bound <= the cube's max, the right
   child clearing its low bit when its lower bound >= the cube's min.
5. Triangles: flags 0 -> the plane distance with the table-rsqrt normal; otherwise RenderWare's sphere-triangle test
   fn_8047C770: a separating-axis reject on the vertices relative to the centre by their sign bits (all positive and
   all beyond +R, or all negative and all beyond -R, by the sign of OR / AND of the float words), the normal (zero
   area misses), the plane distance within [-R, R], a hit if a vertex is inside, a miss if the nearest vertex's two
   edges both lead away (dot > its squared distance), then each edge's closest point (against R^2 and the opposite
   vertex). The distance passed on is the plane distance / R.

## The native owner (native_world_collision.cpp)

- **Native data.** Each world's plane-sector BSP and each sector's RpCollTree (RenderWare 3.7 `rpcollis.h`: bbox +4,
  numEntries +0x1C, numSplits +0x1E, splits +0x20, map +0x24) are mirrored into host structures on first use: per tree
  entry the three vertices and both edges in host floats with the guest vertex addresses for the record, per node its
  whole-subtree entry range. Trees are validated when mirrored (indices, ranges, depth <= 200); anything else is left to
  the original. Each world mirror holds its sectors' mirrors (rebuilt in place); the last world is cached.
- **Lifecycle.** Chained hooks on the collision plugin's sector constructor 0x80441DF8, stream reader 0x80441ED0 and
  destructor 0x80441DA8 (from RpCollisionPluginAttach, fn_80442078) drop a sector's mirror and every world mirror that
  lists it; each query also re-checks the sector's collision / triangle / vertex pointers.
- **Maths.** Host single precision in the originals' operation order; fmadds / fmsubs use the host FMA (AVX2 build;
  std::fma elsewhere). Single-precision add, multiply and divide from a double intermediate round like host floats, so
  no FPSCR or PowerPC rounding is emulated. The sign-bit tests are integer tests on the float words, as the original.
  Normals use RenderWare's table reciprocal square root (the table mirrored once); the line pick computes it once, for
  the winning triangle.
- **Callbacks and filters** run natively; an unknown filter is called as guest code in the original's frames. The
  sphere path calls the game's result builder (fn_80020D40, fn_800201D4) for every accepted hit, with the record in
  the original's frame (see the boundary below).
- Switches: GEKKO_NATIVE_LINE_PICK=0, GEKKO_NATIVE_SPHERE_QUERY=0; GEKKO_VERIFY_WORLD_COLLISION=n;
  GEKKO_WORLD_COLLISION_STATS=1 (counters and hits per filter).

### Validation (2026-10-31, diag tree)

Every served call also run as the original from the same state (GEKKO_VERIFY_WORLD_COLLISION=1); registers and every
byte either run wrote outside the dead stack compared, including everything the sphere result builder wrote:

| route | calls verified (line + sphere) | differed | declined |
|---|---|---|---|
| Westopolis tape | 245,519 (203,656 + 41,863) | 0 | 0 |
| Final Haunt tape | 259,441 (221,944 + 37,497) | 0 | 0 |
| Lethal Highway tape (stg0202; 32 sectors mirrored, 23 dropped) | 200,378 (168,715 + 31,663) | 0 | 0 |
| 12 stages idle 30 s (0200 0201 0300 0301 0302 0401 0403 0501 0502 0503 0601 0602) | 444,229 | 0 | 0 |
| total | 1,149,567 (919,126 + 230,441) | 0 | 0 |

Gates vs base8: audio and frame 3000 byte-identical on both tapes. Same-binary savestate A/B (2-40 s,
`wl_ab_states.ps1 -Var <switch> -Tag <lp|sp>`), line pick and sphere query each: frame at 40 s identical; differing
bytes are dead stack only (line 155-297, sphere 188-477), 0 unexplained.

Cost (game thread, 4 interleaved rounds, three arms: all translated / line pick native / all native; the "all
translated" arm still uses part 1's native line query):

| route | all translated | + line pick | + sphere query | total |
|---|---|---|---|---|
| Westopolis | 10.380-10.457 s (median 10.424) | 10.252-10.415 (10.294) | 10.225-10.261 (10.251) | -1.66 %, no overlap |
| Final Haunt | 11.532-11.602 s (11.594) | 11.392-11.483 (11.450) | 11.270-11.370 (11.314) | -2.41 %, no overlap |

The sphere query alone: Westopolis -0.42 % (4 of 4 pairs, ranges touch), Final Haunt -1.19 % (no overlap). With part 1
(-0.9 % / -1.4 % against fully translated), world collision has removed about -2.6 % / -3.8 % of the game thread.

## The boundary: result objects (deliberately not reconstructed yet)

What remains translated in the chain is the glue that turns hits into the game's result objects:

- fn_80021678 after the pick: hit point, distance, then a 0x6C face (fn_80036038; base class ctor 0x800292C4,
  vtable 0x8051DB98, face vtable 0x8051DE8C, the three vertices at +0x38, the normal at +0x5C with a valid flag
  +0x68), a 0x48 contact (fn_8002686C: the 0x38-byte contact record at +0xC, distance +0x44) and a 0x24 result
  (fn_8001FBD8: +8 flag, +0xC face, +0x10 0, +0x14 contact), each from its own lazily created free-list pool
  (fn_8001FFA4 / fn_8001FC88 / fn_80026518, 128-entry pool at 0x80571D9C, heap fallback 0x80049BB8);
- fn_80021E24 / fn_80021FE4: block iteration, line shortening, merging with the object query, deleting the previous
  result (0x800265A4: virtual destructor at vtable +0x2C, contact delete, pool return);
- the sphere result builder fn_800201D4 (~0.1-0.2 %).

These objects are the interface to 45+ gameplay call sites (player ground / wall / ceiling probes, enemies, objects,
cameras) that are still translated and read them in guest memory. Rebuilding them natively now would mean writing the
same pooled, refcounted, vtable'd objects byte for byte for guest readers - a transliteration that the consumers'
reconstruction would throw away. They move to native form together with their consumers (player update next).
Measured cost of this glue: fn_80021678 minus the pick ~0.5 %, the sphere builder ~0.1-0.2 %, the block loops'
own bodies ~0.1 %.

## Part 1: the query owner (native_world_line.cpp)

## What it is

`fn_804421D8(r3 world, r4 line, r5 callback, r6 data)` is RenderWare's `RpCollisionWorldForAllIntersections` for an
`rpINTERSECTLINE`, with the world-sector and collision-tree walks inlined. It is shared by everything that casts a
ray against level geometry (player, enemies, cameras, objects). In the Phase IX coverage map it was the largest
translated self-cost on the game thread (2.0 %).

Frame: `stwu r1, -0x640(r1)`; the RpCollisionTriangle passed to the callback lives at r1 + 8; the collision tree's
root descriptor is built at r1 + 0x64; the world stack starts at r1 + 0x22C (also the normal return value in r3).

## Algorithm (from the disassembly)

1. **Setup.** S = line start (+0/+4/+8), E = line end (+0xC..), D = E - S stored as singles. Per axis the inverse
   1/D (0 when D is 0; x divides by the delta still in its register, y and z by the reloaded single) and the
   tolerance 1e-6 x max(|S|, |E|) (z's maximum rounded with frsp, x's and y's reloaded as singles). Constants from
   .sdata2: 1.0, 0.0, 1e-6 (0x358637BD), the determinant epsilon 1e-8 (0x322BCC77), the edge slack 1e-5 (0x3727C5AC).
2. **World BSP.** An explicit stack of {node, t0, t1} (t as stored singles), starting at {world+0x1C, 0, 1}. A node's
   first word is its type: negative = world sector; otherwise axis = type >> 2 (byte offset 0/4/8), planes at
   +0x10 (lo, widened +tol) and +0x14 (hi, widened -tol), children at +8 / +0xC. The interval's entry and exit
   coordinates (t0, t1 through S + t D, fused) decide: wholly on one side -> replace the top node with that child;
   straddling -> push the near child with t1 clipped, and turn the top entry into the far child with t0 clipped
   (clip = t + inv x (plane - coordinate), fused). The direction (D > 0) picks near / far.
3. **Sector.** Collision data = *(sector + plugin offset [r13 + 0x5FC8]); none -> next entry. Triangles at
   *(sector + 4), vertices at *(sector + 8). The root descriptor at r1 + 0x64: u16 0 at +2, byte +1 = 0xFF when
   the tree has nodes (u16 coll+0x1E != 0), else the triangle count (low byte of u16 coll+0x1C).
4. **Collision tree.** Same clip walk with its own stack. Descriptor byte +1 == 0xFF -> interior: node
   T = *(coll+0x20) + 16 x u16(desc+2), axis = (T[0] & 0xC) >> 2, planes at T+4 / T+0xC, child descriptors at T and
   T+8. Otherwise a leaf: triangles [u16(desc+2), + count), through the remap table *(coll+0x24) when present.
5. **Triangle.** Vertices v0 = tri+0, vB = tri+4, vC = tri+2 (u16 indices, 12-byte vertices). Moller-Trumbore in the
   original's exact operation order (fused multiply-subtract cross products): determinant must exceed 1e-8; u, v,
   u + v and t are tested against lower = 1e-5 x (-det) and upper = det - lower; t /= det.
6. **Hit.** RpCollisionTriangle at r1 + 8: +0 normal, +0xC point (v0's words), +0x18 index, +0x1C/+0x20/+0x24 the
   vertex pointers (v0, vC, vB). The normal is the cross product scaled by RenderWare's table reciprocal square
   root 0x804833E4 (word + 0x800; result = ((~w >> 1) & 0x3FC00000) + table[(w >> 10) & 0x3FFC]; table =
   *(*(r13+0x615C) + *(r13+0x6140) + 4)).
7. **Callback.** r1 = frame, r3 line, r4 sector, r5 &triangle, r6 data, f1 = t, r12 = ctr = callback, link
   0x80442BA4. A NULL r3 return ends the query with result 0.

## The native owner

- Same traversal order and comparisons; every float operation through the core's single-precision helpers
  (`ppc::fps_*`, which also implement FPSCR[NI]), every stored-and-reloaded value through stfs / lfs.
- Contract left behind: r1-r3, r13-r31, f14-f31, LR, CR2-4, FPSCR's control byte, the caller's LR save word and
  the back chain. Dead stack below the caller's r1 is not reproduced (only the frame words the callback can see).
- Declines (guest body runs) when MSR[FP] is clear, an FPSCR enable bit is set or RN is not round-to-nearest. The
  game runs with FPSCR = NI (0x4) throughout; NI is served.
- `GEKKO_NATIVE_WORLD_LINE=0` disables it; `GEKKO_VERIFY_WORLD_LINE=n` runs the guest body from the same state on
  every n-th call (memory rolled back through the write journal) and compares the contract registers and every byte
  either run wrote outside the current thread's dead stack (OSThread stack end up to the caller's r1);
  `GEKKO_WORLD_LINE_STATS=1` prints the counters.

## Validation (2026-10-31, diag tree)

- **Every call verified**, `GEKKO_VERIFY_WORLD_LINE=1`, full tapes:
  - Westopolis: 203,656 calls, 1,339,433 triangles tested, 67,392 hits; verified 203,656, differed 0,
    unverifiable 0, declined 0.
  - Final Haunt: 221,944 calls (222,324 sectors), 1,798,909 triangles, 76,912 hits; verified 221,944, differed 0.
  - Not exercised: a callback returning NULL (stopped = 0 on both tapes); nested queries (none occurred).
- **Gates vs base8:** audio PCM and the frame-3000 capture byte-identical on both tapes.
- **Same-binary savestate A/B** (`.build/validation/perf-2026-10-30/wl_ab_states.ps1`, states at 2/10/20/30/40 s,
  `tools/state_delta.py` (now accepts v24: only the ARAM tail changed) + `msl-bench/classify.py`, nothing excluded):
  frame at 40 s identical; 0 unexplained bytes. Every differing byte is dead stack below a thread's r1 (498-1105
  bytes) or the 4th word of a 16-byte row whose other 12 bytes are identical in both arms (2-44 bytes: stack
  residue carried into pad slots by struct copies, e.g. 0x8058747C holding a stale stack address) - the same classes
  as docs/NATIVE_MSL_MATH.md. Counts do not grow over time.
- **Cost**, 3 interleaved rounds per arm, `guest_run_wall_seconds`, headless:
  - Westopolis: off 10.487-10.667 s (median 10.545), on 10.411-10.468 s (median 10.452): -0.9 %.
  - Final Haunt: off 11.632-11.795 s (median 11.754), on 11.557-11.599 s (median 11.591): -1.4 %.
  - Ranges do not overlap on either route. Less than the 2.0 % self-cost: the owner still reads guest memory
    through the core's accessors and calls the callbacks through `call_indirect`.

## Open

- The callbacks themselves (the per-hit consumers in player / enemy code) are still translated; the next gain is in
  them and in the callers that build the lines.
- The stop path and nesting are implemented from the disassembly but unproven at runtime.
