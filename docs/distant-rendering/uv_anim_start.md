# What starts and advances a model's UV animation (dash ramp, Westopolis)

Static analysis only (PAL GUPP8P main.dol, doldis.py / capstone; stg_cmn_gdt.one members extracted to
`scratchpad/one/`). r13 = 0x805EC500, r2 = 0x805FA780. VERIFIED = read in the instructions or the data;
INFERRED = follows from RW 3.7 semantics or naming, not every instruction read.

## Short answer

- The arrows' UV animation lives on the **library** material (shared by every clone, and by the far proxy).
- It is advanced by one global task, **TextureUVAnimManagerTask**, which only advances clumps that are **registered**
  in its name-keyed map. A model holder registers a part's library clump (key = the part record's `.UVA` name) when
  it **instances** the model, i.e. when the game spawns the object, and unregisters it when the last instance goes.
- So before the first JUMP_BOARD spawns, nothing advances `JBOARD01.UVA`: its interpolator time stays 0 and
  RpMaterialUVAnimApplyUpdate has never run, which leaves the MatFX UV-transform matrix pointer NULL (identity).
- The native renderer draws the proxy from the library atomic and reads the material's MatFX matrix live at every draw,
  so the proxy faithfully shows **static arrows** until the spawn (frame 805). From the spawn on, the task advances the
  shared material, and proxy and real ramps scroll together.
- The user's "ramp spawns in but its UV animation starts later" is this: the ramp they see appear is the proxy. Its
  arrows stay still until the layout manager actually spawns the entry. The animation data has no hold at the start
  (keys move from t = 0), and no per-class gate exists. The visible delay equals the proxy's lead time before the
  game's spawn, so it depends on the draw-distance setting, not on the animation data.

---------------------------------------------------------------------------------------------------------------------

## 1. The complete path

### 1.1 Binding at load: DFF stream read (RpUVAnim material plugin)
- UVAnim plugin registration 0x80454AE8: `li r4,0x135` / `bl 0x80486DC8`. Material plugin, size `li r3,0x28`
  (0x80454B0C), offset stored at `stw r3,0x6044(r13)` (0x80454B30; word 0x805F2544). Stream callbacks
  (0x80454B44 `bl 0x8045C35C`): read 0x80455000, write 0x80454E74, size 0x804552E4. VERIFIED.
- Plugin data (material + [0x805F2544]), 0x28 bytes: +0x00 `RwMatrix* uv[0]`, +0x04 `uv[1]`, +0x08..+0x24
  `RtAnimInterpolator* interp[8]` (0x8045596C `lwz r3,8(r31)` loop over 8 slots). VERIFIED.
- Stream read 0x80455000: for each set bit of the slot mask, it reads a 32-byte name (0x80455080 `bl 0x80481D9C`)
  and gets the current UV-anim dictionary (0x8045509C `bl 0x8047A114` = `lwz r3,0x14(r3)` on schema 0x8056E808).
  - Found (`bl 0x8047A124`, RtDictFindNamedEntry): refcount++ (0x80455668: anim +0x14 -> +0x40).
  - Not found, or no current dict: a **placeholder** 2-keyframe identity animation (0x804554EC, keys written by
    0x80454068 twice).
  - Then RtAnimInterpolatorCreate (0x80455260/0x8045526C) into `interp[i]` (`stw r3,8(r31)`) and
    RtAnimInterpolatorSetCurrentAnim 0x80478B08, which writes time 0 (`stfs f0,4(r3)` at 0x80478B34).
  - Then 0x80454D58 allocates `uv[0]` / `uv[1]` with RwMatrixCreate 0x804809E4 (identity: flags 3, diagonal 1.0,
    0x80480A18..0x80480A54). VERIFIED.
- The stream read does **not** touch MatFX. Its matrices are linked only later, by ApplyUpdate (1.5). VERIFIED: the
  only call from the UVAnim module into MatFX is 0x80455914 -> 0x804A3C30. No code outside 0x80454000-0x80456000
  reads `[r13+0x6044]` (scan of every `lwz rX,0x6044(r13)`).
- Data: JBOARD01.DFF (stg_cmn_gdt.one #118), material extension: MatFX 0x120 = `05000000 05000000 00000000`
  (UVTRANSFORM, no data), UVAnim 0x135 = slot mask 1, name `RenderWareShader7`. JBOARD01.UVA (#180) is a UVAnim
  dictionary (0x2B) holding one animation (0x1B), interp type 0x1C1 (parameterised), 3 keyframes, duration 0.625,
  name `RenderWareShader7`. VERIFIED (bytes). BALLOON01.UVA has the same name (another anim).
- **OPEN (static ordering concern):** the ONE loader 0x80013C30 handles files in archive order: TXD pass first, then
  one loop, 0x80013D80..0x800144AC. A DFF is stream-read immediately (0x80013E0C `bl 0x8004BB4C`). A UVA is read and
  made current only when it is reached (0x800142AC `bl 0x8004B734`, 0x800142C0 `bl 0x8047A11C` = `stw r4,0x14(r3)`).
  In stg_cmn_gdt.one every DFF (JBOARD01.DFF #118) precedes every UVA (#157..#193). Read literally, JBOARD01's
  material would take the identity placeholder, unless the current dict at that moment already holds
  `RenderWareShader7`. The user sees the arrows scroll, so something binds the real animation. The runtime check in
  section 5 (anim numFrames / duration) settles it. This does not change the "starts later" mechanism below.
- **SETTLED 2026-11-05 (runtime): no material takes the placeholder.** The stage gadget archive is not loaded in
  archive order: GadgetStageDataEnd builds a ResourceOneFileReq whose step (0x8006B1B8) reads the whole archive
  synchronously and handles only the REQUESTED members, in request order (notes: re_C_objects_resources.md 2.2), and
  that is guest code the port runs unchanged (native streaming replaces the I/O underneath, not the order). A
  temporary per-material audit on stg0600 (GUN Fortress, whose gdt holds 104 DFFs before its first UVA at index 105)
  printed every UV-transform material at its first draw: BS0000LIGHTGLARE `lambert2` 2 keys / 1.96667 s,
  BS0000WALLLIGHTGLARE `lambert2` 2 keys / 0.966667 s, stg_cmn `RenderWareShader1` 2 keys / 0.5 s - each the
  dictionary's own animation (same key count and duration as the archive member, refcount 2; the placeholder
  0x804554EC sets refcount 1), and each matrix went from NULL to live on the next frame (registered, ApplyUpdate).

### 1.2 Registration at instancing (the start)
Model holder instancing 0x80162EB4, called by 0x80162E4C, called by JumpBoard's constructor (0x8010F0A4 calls
0x80162E4C). Per part (record = r5 = `[r1+0x38]`):
```
80162F6C bl 0x80012700        ; resource(part+4 = DFF) -> LIBRARY clump
80162F70 stw r3,0x28(r1)
80162F78 lwz r4,8(r3)         ; part +0x08 = UVA name
80162F80 beq 0x80162FB4       ; none -> node UVA = 0
80162F90 bl 0x8004F8E0        ; std::string(name)
80162F94 bl 0x8010A840        ; TextureUVAnimManager singleton (0x80580B08)
80162F98 lwz r5,0x28(r1)      ; the LIBRARY clump (before the clone)
80162FA0 bl 0x801EB970        ; register(name, clump)
...
8016304C bl 0x804580E0        ; RpClumpClone - after the registration
```
VERIFIED. 0x801EB970 is a `std::map<string,{clump,count}>` insert. If the key exists: count++ (`lwz r4,0x1C(r31)` /
`stw r0,0x1C(r31)`, 0x801EB9B8). Otherwise a node with value {clump at +0x18, count 1 at +0x1C}
(0x801EB9D0..0x801EBA00 -> 0x801EC078). The key string is at node +0xC (compare at 0x801EC0D0). VERIFIED.

So the clump the task animates is the library clump of the **first** registrant. Later holders only bump the count.

Unregister: holder release 0x80162BE8 -> 0x80162C1C `lwz r4,0x10(r31)` (instance node +0x10 = UVA name) ->
0x801EB860: count--, erase at <= 0 (0x801EB8B8 `bgt` / 0x801EB8C4 `bl 0x801EBD14`). VERIFIED.

JUMPBOARD.BIN (stg_cmn_gdt.one #216), library at file +0x20 (VERIFIED, bytes):
- 2 parts:
  - part 0 `model0` / JBOARD00.DFF: +0x08 = 0, +0x0C = 0.
  - part 1 `model1` / JBOARD01.DFF: **+0x08 = "JBOARD01.UVA"**, +0x0C = 0, parent part `model0`, node 0x7D0.
- 1 model entry; 1 extra record ("mtn": JBOARD00.BON / JBOARD00_ANM.MTN, the base's motion); 1 collision record
  (JBOARD.CCL).

Correction to the brief: the record word **+0x0C** path (0x80162FD0..0x80163044 -> 0x8004FA40) is not the UV
attach. It makes the TXD `STG_GDT.TXD` current (0x80162FF4 `bl 0x8048E114`) and calls into the "Texture Change :"
module (0x8004F014..0x8004FD20). INFERRED: a texture-pattern animation. It is unused here (both parts +0x0C = 0).

### 1.3 The per-frame advance
- Created once at boot: 0x801E28A0..0x801E28B4: `bl 0x8001C4F8`, `bl 0x801EB614` (task-tree parent),
  `bl 0x801EB81C` (ctor; vtable 0x8053EDC4 at +0x18, type info 0x805E9E74 -> "TextureUVAnimManagerTask"). VERIFIED.
- vtable 0x8053EDC4: +8 dtor 0x801EB7C4, **+0xC exec 0x801EB790**. VERIFIED (`--u32 0x8053EDC4`).
- exec 0x801EB790: `fmr f31,f1` (dt), `bl 0x8010A840`, `bl 0x801EBA74`. VERIFIED.
  - docs/effects/lighting_materials.md 3.1 says "dt = fn_8010A840". That is wrong: 0x8010A840 returns the manager
    object 0x80580B08, and dt is the exec's f1.
- 0x801EBA74 walks every map node and calls `0x80039BBC(node+0x18 clump, dt, f2 = [r2-0x35D8] = -1.0)`
  (0x801EBAB4..0x801EBABC; 0x805F71A8 = BF800000). VERIFIED.
- 0x80039BBC calls RpClumpForAllAtomics(clump, 0x80039BF8, {dt,-1}). 0x80039BF8 calls
  RpGeometryForAllMaterials(atomic+0x18 geometry, 0x80039C44) (0x80039C0C `lwz r3,0x18(r3)`, `bl 0x8045A138`).
  VERIFIED.
- Per material 0x80039C44:
  ```
  bl 0x80455A2C            ; any interpolator? (plugin +8..+0x24 non-null)
  lfs f1,4(r31); fcmpo f1,[r2-0x7C18]=0.0 ; -1 >= 0 never -> SetCurrentTime 0x8045593C skipped
  lfs f1,0(r31); bl 0x804559B4   ; RpMaterialUVAnimAddAnimTime(mat, dt): each interp -> 0x80478F24
  bl 0x804A3334            ; RpMatFXMaterialGetEffects
  cmpwi 5 / cmpwi 6 -> bl 0x8045567C ; RpMaterialUVAnimApplyUpdate
  ```
  VERIFIED.
- RtAnimInterpolatorAddAnimTime 0x80478F24: returns when dt <= 0 (0x80478F48/0x80478F4C), `currentTime(+4) += dt`
  (0x80478F90/94), wraps by `duration = anim(+0)->+0xC` (0x80478F6C, 0x80478FAC). VERIFIED.
- Who calls the exec: the task-tree traversal 0x8004ECAC (bctrl at 0x8004ED94 with f1 = f31 = the frame dt; a
  "paused skip" exists). Native: runtime/src/gekko/gameplay_scheduler.cpp walks the same tree; kVtableExecute = 0x0C.
  INFERRED from that source's own annotations (not re-disassembled here).
- Other users of 0x80039BBC: 0x80064080, 0x800A506C, 0x801FDF60, 0x8023038C, 0x8027EB44, 0x80330724 (other systems'
  own clumps). None of them is in JumpBoard.
- Per-draw time override 0x800384AC (`lfs f1,0x28(r31)`; SetCurrentTime + ApplyUpdate) is installed only by the
  render callbacks 0x80038304 / 0x800383DC, not by the holder's 0x801645D0. 0x801645D0 only sets the light group
  and the material colour/alpha callbacks 0x80164798 / 0x801647AC. VERIFIED.

### 1.4 Where the interpolator lives
`interp = [material + [0x805F2544] + 8]`; `interp+0` = anim, `interp+4` = current time (float).
`anim+0xC` = duration (VERIFIED by the wrap); `anim+0x10` = keyframes, `anim+0x14` = custom data
(name +0, refcount +0x40) (VERIFIED in 0x804554EC / 0x80455668).

### 1.5 How the result reaches the material
ApplyUpdate 0x8045567C:
- resets `uv[0]` / `uv[1]` to identity (0x804556B8..0x80455740);
- builds each channel's matrix from the interpolated keyframe. The param branch (interp callback == 0x80454618) uses
  RwMatrixTranslate / Rotate 0x804805EC / 0x80480018 and the scale/skew build 0x8047FBF0;
- multiplies into `uv[n]` (0x804558DC `bl 0x8048077C`);
- then **0x80455914 `bl 0x804A3C30`** = RpMatFXMaterialSetUVTransformMatrices(mat, uv[0], uv[1]). That function
  finds the MatFX effect slot whose type (+0x14) is 5 and stores `+0 = uv[0]`, `+4 = uv[1]`
  (0x804A3C70 / 0x804A3C74). VERIFIED.

The only other caller of 0x804A3C30 is MatFX material copy (0x804A1C68). So until the first ApplyUpdate, the MatFX
base / dual pointers are whatever MatFX's stream read left. INFERRED: NULL, which means identity both on the GC
driver and in the port. Since the matrices are written in place, every later frame changes only the floats behind
the same pointer.

## 2. What decides when the arrows start moving

| candidate | verdict |
|---|---|
| Registration at instancing (0x80162FA0) | **The start.** Nothing advances JBOARD01's material until a JUMP_BOARD holder instances its model (JumpBoard ctor -> 0x80162E4C -> 0x80162EB4). VERIFIED. |
| Holder state bits (+0x3C: hidden, blink, fade) | Not involved. 0x80163394 has no UV call, and the task ignores holders. VERIFIED (xrefs: AddAnimTime only from 0x80039C8C, ApplyUpdate only from 0x80039CAC / 0x80038500). |
| Layout restricted / event modes | Only gate the spawn itself, and so the registration. The task is global. |
| Per-class gate in JumpBoard | None: no JumpBoard function calls any UV-anim function (functions.csv callees, xrefs above). VERIFIED for direct calls. |
| Animation data (hold at the start) | None. JBOARD01.UVA keys, param layout {time, theta, s0, s1, skew, x, y, prev} (VERIFIED from prev indices 0/1 and the header): k0 t=0 (θ 0, s 1/1, x 0, **y 0**); k1 t=0.29167 (y **2.0**); k2 t=0.625 (y **4.0**). Scroll starts at t = 0: 6.86 texture units/s, then 6.0/s; it loops every 0.625 s (37.5 frames at 60 Hz) seamlessly (integer offsets). Data-driven delay: **0 frames**. |
| Task order on the spawn frame | The game's first draw of the arrows (frame 805, last pass) may still be at time 0 / NULL matrix if the task runs after the layout manager in the tree. That is at most 1 frame. Runtime check. |

In the original game, at draw distance 1, the arrows start scrolling within 0-1 frames of their first appearance.
The delay the user sees is the proxy's lead time: the proxy appears when the entry gets within the slider's range
(2..7 x the authored 1000 units, "Far-object proxies" in DRAW_DISTANCE.md), and the arrows start moving only at the
spawn (frame 805 on the tape). So it is (time of the spawn) - (time the proxy first shows), a function of the
player's speed and the slider. INFERRED from the paths above; the runtime check in section 5 measures it.

Second visible consequence (INFERRED): once every JUMP_BOARD holder is released (count 0, node erased), the shared
material keeps its last `uv[0]`, and MatFX keeps pointing at it. Proxies drawn later show **frozen arrows at a
non-identity phase**, not identity.

## 3. Shared material or cloned?

- Shared. RpAtomicClone (inside 0x804580E0 at 0x804581AC..0x804581D8): `lwz r27,0x18(r30)` (source geometry),
  0x8045A89C = `lha r4,0xE(r3); addi; sth` (geometry refcount++), and the clone points at the same geometry.
  Materials belong to the geometry's material list, so the UVAnim plugin data, the interpolator and the MatFX matrix
  pointers are all one copy. VERIFIED.
- The registered clump is the library clump itself (0x80162F98 reads `[r1+0x28]` before the clone at 0x8016304C).
  So the task animates the library atomic's geometry 0x81081CC0 / material 0x81084740, and every instance and the
  proxy see the same phase. VERIFIED (control flow); the addresses come from the brief.
- A far stand-in drawn from the library atomic 0x81084B20 before the first spawn: interpolator time 0, MatFX base
  pointer still NULL (INFERRED). Native reads NULL, so it keeps the default `uv_transform = {{1,0,0},{0,1,0}}`
  (runtime/lift/render/frame.h:91/107): **identity, static**. If an earlier JUMP_BOARD had been registered and then
  released: **frozen at its last phase**. After the spawn: the live shared phase, identical to the real ramps.

## 4. Native runtime: can it delay or freeze the transform?

- runtime/scaffold/rw_driver/pipelines.cpp `matfx_layers` (599-643) reads `fx = [material + [kMaterialEffectsOffset]]`,
  the kind at fx+0x30 and **the matrix pointer `[fx+0]` and its six floats (+0, +4, +0x10, +0x14, +0x30, +0x34) at
  every draw**. It is called per mesh per draw from pipeline_atomic (853), draw_proxy_atomic (1031) and
  pipeline_sector (1334). Nothing is cached. VERIFIED (source).
- draw_proxy_atomic (970-1045) reads the library atomic's geometry and material live, with no snapshot. far_objects.cpp
  caches only the model's atomic list / poses per entry (`entries_`, 584), never material state. VERIFIED (source).
- material_probe.cpp hashes `uv_transform` only for its report and is inert unless GEKKO_MATERIAL_PROBE is set.
  VERIFIED.
- No native owner or hook exists for any function on the path: grep of runtime/ for 0x801EBA74, 0x801EB790,
  0x80039BBC/BF8/C44, 0x804559B4, 0x8045567C, 0x80478F24, 0x804A3C30, 0x801EB970, 0x801EB860, 0x8010A840,
  AddAnimTime and RtAnimInterpolator found nothing. The translated guest code runs, as EFFECTS_INVENTORY.md says.
  VERIFIED.
- material_import.cpp only parses UVAnim names (offline importer).
- Conclusion: the runtime adds no delay of its own. It shows the guest's state. The proxy exposes a state the original
  never displays: the library material before any holder registered it.

Fix directions for the parent (not done; each needs the user's ruling):
- (a) The proxy evaluates the UV animation itself (anim keyframes + its own clock) while the name is unregistered.
  The phase jumps at handover, because the guest's interpolator starts from 0 at the spawn.
- (b) DELIBERATE DEVIATION: register each proxied library's UVA at stage load, so the guest's shared interpolator runs
  from load. This writes guest state, so the "identical guest memory at distance 1 vs 8" property is lost.
- (c) Draw the proxy's UV-animated parts only once registered (the arrows pop in). Same family: DASHPANEL01,
  SPRING01, HINTRING00/01, SAVEPOINT0x, DASHRING0x, BALLOON01 ... in stg_cmn_gdt.one.

## 5. Runtime checks (Westopolis tape; the ramps spawn on trace frame 805)

Static words: UVAnim offset **[0x805F2544]** (r13+0x6044); MatFX material offset **[0x805F2750]** (r13+0x6250).
Material M = 0x81084740.

Per frame, from the frame the proxy first draws through about 850:
1. `P = M + [0x805F2544]`: P+0 (uv0 ptr), P+4 (uv1 ptr), `I = [P+8]` (interp). `[I+4]` float = current time
   (expect 0.0 constant before 805, then +dt per frame, wrapping at 0.625). `A = [I+0]`: `[A+0xC]` duration
   (0.625 means JBOARD01 bound; **1.0 means the placeholder**, which settles the OPEN item in 1.1), `[A+4]` numFrames
   (INFERRED offset; expect 3), name at `[[A+0x14]+0]` = "RenderWareShader7".
2. `F = [M + [0x805F2750]]`: `[F+0x14]` = 5 (slot 0 type), `[F+0x30]` = 5. **`[F+0]` = base matrix pointer: expect 0
   until the first ApplyUpdate, then == `[P+0]`.** Matrix floats +0x30 / +0x34 (pos), +0x00 / +0x14 (scale): the
   scroll value. The parameterised y drives one of pos.x / pos.y.
3. Watches (`--watch` function entry):
   - 0x80162EB4 (instancing);
   - 0x801EB970 (register; r4 = std::string* whose chars should be "JBOARD01.UVA", r5 = clump: expect the library
     clump, not an instance);
   - 0x801EB860 (unregister);
   - 0x801EBA74 (manager exec; f1 = dt);
   - 0x8045567C with r3 == 0x81084740 (first ApplyUpdate frame vs the spawn frame: settles the 0-1 frame order).
4. Sharing: `[0x81084B20+0x18]` (library atomic) == `[0x814C00E0+0x18]` (an instance atomic) == 0x81081CC0.
5. Map 0x80580B08 (std::map; nodes key at +0xC, clump +0x18, count +0x1C): no "JBOARD01.UVA" node before frame 805
   (a JUMP_BOARD alive anywhere earlier would make the arrows run before). The count should become 4 at 805.
6. SHADOW_R2_TRACE_FRAME on frames 804/805/806: the proxy's "proxy atomic ... material 81084740" lines next to
   item 2's matrix values. Plus the proxy's first-draw frame (SHADOW_FAR_TRACE=JUMP_BOARD:...) to measure the
   delay = 805 - first proxy frame.
