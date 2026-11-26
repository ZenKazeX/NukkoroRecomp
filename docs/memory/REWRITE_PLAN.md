# Rewriting the game as a 64-bit program: what it takes, and the order of the pieces (2026-11-26)

This plan builds on `GAME_MEMORY_MAP.md` (beside this file). The marks are the same:
- **MEASURED**: counted or run.
- **READ**: in code or in a project document.
- **INFERRED**: concluded, not checked.
- **UNRESOLVED**: not known.

The plan has seven sections:
1. The short answer.
2. What a 64-bit program needs, with counts.
3. The rules every piece follows.
4. The pieces.
5. What cannot be done piece by piece.
6. The size estimate.
7. The first three pieces.

## 1. The short answer

1. **INFERRED.** The game already runs as a 64-bit Windows program.
   - Its translated code keeps 32-bit values, and each value is an offset into one 4 GiB host reservation.
   - This is the "compressed pointers" design that the Java and V8 virtual machines use.
   - The memory limit the mods hit is therefore **not** the 32-bit width. It is the size of the one heap region mapped in the window: 18.7 MB on the console layout (MEASURED). With the release session's change it becomes about 250 MiB whenever mods replace files (READ).
2. **INFERRED.** The present window design can be stretched to about 1 GiB of game heap with no change to any structure layout (piece W1).
   - Above that, the SDK's physical-address arithmetic stands in the way (`GAME_MEMORY_MAP.md` section 1.2).
   - Asset bytes that only native code reads can also leave the window altogether, as the motion clips, sound banks and stream ring already did (pieces M1, M2).
   - These pieces remove the practical memory limit in weeks, not years.
3. **INFERRED.** Making every game structure hold real 64-bit pointers is a different thing. It is the whole native rewrite that `NATIVE_WINDOWS_MIGRATION.md` already plans: 37,739 translated functions and about 1.2 M instructions (MEASURED, section 2).
   - It cannot be done by widening pointers in the translated code. Every structure layout, every field offset baked into every function, every vtable and pointer table in the image, and every relocated file format would have to change at once (section 5).
   - It can only be done subsystem by subsystem, as a native rewrite of each subsystem's code and data together. Each subsystem's structures become native C++ with 64-bit pointers on the day its last translated reader is gone.
   - **READ** (`NATIVE_WINDOWS_MIGRATION.md` section 4): the guest heap and the window go last.
   - Pointer width is an outcome of that rewrite, not a separate step.
4. **INFERRED.** Wider pointers are not free.
   - Pointer-heavy structures grow: RW frames, object lists, heap cells.
   - More cache traffic follows, and the owner's rule is that nothing gets slower.
   - Each subsystem's native rewrite must therefore win back at least that cost. The usual sources are no byte swap per access, real C++ types, and no dispatcher. Each must measure it (section 3).

## 2. What a real 64-bit program would require, with counts

Each row is a class of code or data that holds or forms a 32-bit address today.

| Class | Count | Mark | What would change |
|---|---|---|---|
| Translated functions | 37,739 recompiled of 39,388 in the table; 628 native-owner thunks; 34,003 address ranges; 164,104 entry points (126,365 extra mid-function entries) | MEASURED (`.build/release/generated/recomp_report.txt`, `recomp_table.cpp`) | each one works on `u32 gpr[]` and `u32` effective addresses; every one is rewritten or deleted |
| Static instructions in the DOL's code | about 1.217 M | MEASURED (decoded from `generated/embed/main.dol`) | n/a |
| Word loads through a non-stack, non-SDA base (the pointer-load candidates) | lwz 90,685 + lwzx 3,137 + lwzu 1,374 | MEASURED | a pointer load becomes an 8-byte load at a new offset; a float word loaded with lwz stays 4 bytes. **No existing tool says which is which** (READ: `census-v2.json` has `memory_values_read: false`) |
| Word stores through a non-stack base | stw 48,792 + stwx 1,037 | MEASURED | same |
| Stack and small-data word traffic | lwz on r1 47,179 / r13 6,505 / r2 445; stw on r1 54,087 / r13 1,568; lmw / stmw 9,496 / 9,494 | MEASURED | frames are rebuilt by the C++ compiler once a function is native |
| Indirect calls | bcctrl 6,550 static; `call_indirect` 11,492 and `jump_indirect` 578 emitted sites; 195 jump tables | MEASURED | a function pointer or vtable slot becomes a host pointer or a native virtual call |
| Emitted guest memory accesses | `mem_read*` 381,715 / `mem_write*` 362,911 emitted sites (bodies are emitted more than once) | MEASURED (`rg --count-matches` over 48 shards) | n/a |
| Pointer words in the image's data sections | 24,318 words whose value falls in code (23,008 in D5) + 26,466 that fall in data or BSS (D5 16,536, D6 7,838, D4 1,523): about 50.8 K | MEASURED (scan of aligned words; INFERRED that all are real pointers, since the dtk audit found only 3 unrelocated pointers, READ `docs/DOLRECON.md`) | every table becomes a native table or is rebuilt from the dtk project's relocations |
| Classes, vtables | 1,620 classes; 6,988 vtable slots; 1,157 constructors and 1,367 destructors store a vtable | READ (`docs/DOLRECON.md`) | `+0` of every object is a 32-bit vtable address the translated code calls through |
| Allocator metadata | OS heap cells, HeapDesc, MSL pools, Sonicteam heap, RW free lists, GCAX 'MF' / 'MB' cells | READ (`MEMORY_ALLOCATORS.md`) | replaced by native allocators when their users are native |
| Relocated file formats | "1B" containers 28,218 relocated words (1,862 blocks); .SNB 1,613 relocations (222 files); `.one` directories; stage set records +0x28 | READ (`docs/re/formats.md`, `docs/ONE_LAYER.md`, `docs/STAGE_LOADING.md:186`) | converted offline or parsed into native structures (`NATIVE_WINDOWS_MIGRATION.md` section 5: offline conversion, not started) |
| RenderWare streams | parsed into newly allocated objects, not relocated | INFERRED | the parser stays; the objects it builds become native |
| Mtn, CSD (.GNCP) files | n/a | UNRESOLVED (`docs/re/assets.md` does not exist) | n/a |
| Save data | 0x1F5C-byte record; documented fields are options, flags and a stamp; the "icon / comment address" fields are offsets in the save file | READ (`docs/SAVE_FORMATS.md` section 2.2); INFERRED that no memory address is stored (not proven field by field) | none expected |
| Host code that names guest addresses | 400 `install_hle` uses in 94 files; 461 + 149 owned function starts; 15 files compare with `game_memory_end()` / 0x81800000 | READ (`NATIVE_WINDOWS_MIGRATION.md` section 2; this record) | rewritten with each subsystem |
| Savestates and the tape gate's state hash | version 34; the state hash is one hash per 64 KiB block of each window region plus the native arena records | READ / MEASURED (`state.json`) | the gate's state hash is defined over the window; it ends with the window (section 5) |

## 3. Rules every piece follows

These are the owner's standing rules, made concrete. They come from `docs/AGENT_COORDINATION.md` section 4 and the memory notes.

1. **No gameplay change.** The behaviour trace (`behaviour.jsonl`), the audio (`audio.pcm`) and the 10 screenshots of the tape gate (`wt-fix-shoes\tools\native\gate.ps1`) must stay **bit-identical** for every piece.
2. **The state hash.** It hashes the window's bytes, so it must stay bit-identical for every piece that does not change what the heap holds.
   - **INFERRED:** a piece that removes allocations from the heap changes the address of every later allocation. It cannot keep the state hash. Such a piece proves itself in two steps instead:
     1. **Poison step.** The heap block is still allocated, so every address stays the same. The guest copy is filled with a fixed poison pattern, and all real readers use the native copy. A `--watch-guest` trace of the poisoned blocks shows **0 translated reads**. Behaviour, PCM and shots are identical. The state hash differs only in the poisoned blocks: a gate with the guest-copy write left on must be fully bit-identical.
     2. **Release step.** The allocation is no longer made. Behaviour, PCM and shots are identical again. The new state hashes are recorded as new references, with the piece's name, in `gate_references.json`.
3. **Speed.** Use `run_tape.ps1 -Exclusive -Repeat 5` on Westopolis and on Final Haunt, A (without the piece) against B (with it), run alternately. Compare the median `game_thread_cpu_s` and cycles.
   - A piece may not be slower beyond the spread of the A runs.
   - **READ:** earlier converted functions were held out at +2 %. That threshold, or the A spread, whichever is smaller, is the limit.
   - Real play with sound on (the memory note of 2026-11-25) is reported too where the piece touches audio.
4. **Heavy jobs.** Every heavy job goes through `.coord\coord.ps1`, one at a time, with at most 20 of 24 cores and 32 GB.
5. **Reports.** A piece's report separates MEASURED / READ / INFERRED / UNRESOLVED. It is in English, written for a non-specialist.
6. **Where work goes.**
   - Files listed as owned by another session are left alone. On 2026-11-26 the release session owns `guest_memory.h/.cpp`, `game_memory_end.h`, `system.cpp` and `shadow_main.cpp`.
   - A piece that needs those files waits until the release session's work is committed. Its dependency says so.
   - Nothing is pushed.

## 4. The pieces, in order

### Stage A: remove the memory limit without changing any structure layout

**M0: Heap attribution census (measurement only).**
- **What.** Record, for every live OS-heap cell, who allocated it. The hooks go into the native OSAllocFromHeap / OSFreeToHeap owners (`native_os_heap.cpp:110-165`).
  - Record the return site two frames up: OSAllocFromHeap ← RwMalloc / `__sys_alloc` / AudioGCN ← the real caller.
  - For RwMalloc calls, also record the RenderWare memory hint (r4 at RwMalloc's entry, for example 0x30411 for raster levels).
  - Once per game second, write live bytes by caller and by hint beside `GEKKO_HEAP_TRACE`. A new variable, `GEKKO_HEAP_ATTRIBUTION=<csv>`, turns it on.
  - This is the per-kind split that `GAME_MEMORY_MAP.md` section 0 could not measure.
- **Depends on:** nothing.
- **Files:**
  - `runtime/src/gekko/native_os_heap.cpp`: hook points, read-only for guest memory.
  - New `runtime/include/gekko/heap_attribution.h` and `runtime/src/gekko/heap_attribution.cpp`.
  - `runtime/src/gekko/heap_census.cpp`: the writer, ticked from the existing `HeapTrace::tick`, so `shadow_main.cpp` is not touched.
  - A test in `runtime/tests/` (a new `heap_attribution_tests.cpp` and its CMake entry).
- **Done when:**
  - In every sample, the attributed bytes sum exactly to the census's allocated bytes.
  - The gate is bit-identical with the variable off and with it on (it only observes).
  - With the variable off, the speed A/B shows no difference: the hook is one predictable branch.
  - The result table for Westopolis and Final Haunt is added to `GAME_MEMORY_MAP.md` section 4, with texels, geometry, collision, motion nodes, file buffers, objects and the rest.
- **Size:** 1 agent, 1-2 days.

**W1: A larger window heap, up to about 1 GiB (mod runs only).**
- **What.**
  1. Move the motion pool's handle base out of 0x90000000, but only in runs whose heap is extended. Use a value below 0x80000000 that no other handle range uses, for example 0x03000000, beside the sound handles. Do the same for the retired-arena guard.
  2. Raise `kLegacyHeapMaxEnd` from 0x90000000 to just under 0xC0000000. The physical-address rule `from_os_physical` allows that much.
  3. Before raising it, audit the translated code and the owners for:
     - the uncached-mirror arithmetic (`+0x40000000`, `oris rX, rX, 0x4000` / `0xC000`): an address above 0xA0000000 would land on the locked-cache scratch at 0xE0000000;
     - compares of a pointer with a console constant such as 0x81800000 or 0x817FFFFF.
     The address census `test_data/address-census-20261005-v1` and `tools/ppcrecomp/scan_address_formation.py` are the starting tools.
  4. Add a command-line switch that forces the larger heap in a run without mods, for testing only.
- **Depends on:** the release session's memory work being committed, since it touches its files.
- **Files:**
  - `runtime/include/gekko/guest_memory.h` and `runtime/src/gekko/guest_memory.cpp`;
  - `runtime/include/gekko/native_motion_store.h` and `runtime/src/gekko/native_motion_store.cpp`: a base chosen at start-up instead of `kPoolBase`. 14 uses of `kPoolBase` (READ);
  - `runtime/src/gekko/cpu.cpp`: the retired-range stop;
  - `runtime/src/gekko/savestate.cpp`: the label of the pool record;
  - `runtime/src/shadow_main.cpp`: the call;
  - `scaffold/rw_driver/far_objects.cpp:2043`: a pool reader.
- **Done when:**
  - Without mods, the gate is **fully bit-identical**: nothing changes in such a run.
  - With the forced large heap on Westopolis and Final Haunt, behaviour, PCM and shots are identical to the references. The state hash differs only in the low-memory size words, the heap descriptor, and the heap's free tail.
  - The 15-mod set from `RELEASE_VALIDATION.md` runs its 5-minute check with no allocation failure.
  - Speed: no-mod A/B unchanged.
- **Size:** 1 agent, 2-4 days, most of it the audit.

**M1: Texture texels out of the window, behind handles.**
- **What.**
  - Today the native raster owners allocate every mip level through the engine's allocator, which is the heap (`native_rw_raster.cpp:511-603`). The GPU copy is built from those heap bytes, so each texture is held twice.
  - Serve level storage from a new native store, following the `NativeMotionStore` pattern: `NativeArenaStorage`, a handle range below 0x80000000, and a savestate record.
  - `raster->pixels` then holds a handle.
  - Every native reader resolves the handle: the texture reader 0x8049A8DC, tile / untile, the clear paths, the renderer's upload and texture packs.
  - Only one path still hands texels to translated code: RwRasterLock 0x8048AFC0, whose owner returns the pixel address.
    - The piece must first list every translated caller of RwRasterLock and of GcnRasterLock 0x80496CA4, with a call census.
    - Each such lock is then served by a window-resident lock buffer, copied in on lock and copied out on unlock (both owners are already native).
- **Depends on:**
  - M0 for the size of the win (texel bytes: **UNRESOLVED**).
  - The list of RwRasterLock callers (part of the piece).
  - It does not depend on W1.
- **Files:**
  - `runtime/src/gekko/native_rw_raster.cpp` and `runtime/include/gekko/native_rw_raster.h`;
  - `runtime/src/gekko/native_rw_raster_image.cpp`, `native_rw_image.cpp` and `native_rw_display.cpp`: the other users of the lock / pixel words (READ: they name `kRwRasterLock`);
  - new `runtime/include/gekko/native_texel_store.h` and `runtime/src/gekko/native_texel_store.cpp`;
  - `runtime/scaffold/rw_driver/textures.cpp`: the upload from texels;
  - `runtime/src/gekko/savestate.cpp`: one new arena record;
  - `runtime/src/gekko/system.cpp`: install; this waits for the release session's commit;
  - a contract test against the original raster functions in the reference interpreter (as `sonicteam_heap_tests`).
- **Done when:**
  - Poison step and release step of section 3.2 both pass on both tapes.
  - The heap census shows the attributed raster bytes gone from the heap.
  - The speed A/B is no slower. A texel read becomes a host pointer with no byte swap, so it may be faster (INFERRED).
- **Size:** 1 agent, about 1-2 weeks.

**M2: File staging buffers out of the window.**
- **What.** Some heap blocks exist only to receive a file read, which a native consumer then expands or parses:
  - PRS-compressed reads handed to the native expand jobs;
  - `.one` archive bodies read by `native_one.cpp`;
  - the TBGRead 0x2000-byte chunks.
- Those blocks can be read straight from the mapped file (the file API already maps every file) and never enter the heap.
- **Depends on:** M0, to find which blocks these are and how large they are. Each candidate's readers must be shown to be native, by the same poison step.
- **Files:**
  - `runtime/src/gekko/native_expand_jobs.cpp`, `native_one.cpp`, `native_file_api.cpp`, `asset_runtime.cpp` / `asset_mount.cpp`;
  - for TBGRead, the stage-loading owners named in `docs/STAGE_LOADING.md`.
- **Done when:** as M1, one consumer at a time.
- **Size:** 1 agent per consumer, 3-5 days each.

**M3: Reconstruct the MSL malloc pools (no memory win; an enabler).**
- **What.** Reconstruct the block and sub-block formats and the size classes byte for byte (`MEMORY_ALLOCATORS.md` section 3, open). Prove the reconstruction with the same interpreter pairing test as the OS heap and the Sonicteam heap.
- MSL serves 566,004 mallocs in 316 s (MEASURED). Every C++ object of the game comes from it.
- Stage B's object pieces need a native allocator that can stand in for it.
- **Depends on:** nothing.
- **Files:** new `runtime/include/gekko/msl_pool.h`, `runtime/src/gekko/msl_pool.cpp`, `runtime/tests/msl_pool_tests.cpp`. It is not installed as an owner (the +2 % lesson).
- **Done when:** 0 differences over at least 30,000 random operations against the original words, in the reference interpreter.
- **Size:** 1 agent, 3-5 days.

### Stage B: subsystem closures that take their data out of the window

This is `NATIVE_WINDOWS_MIGRATION.md` section 4 applied to memory.
- A subsystem's structures can leave the window, and become native C++ with 64-bit pointers, only when every reader and writer of them is native.
- That includes every virtual overrider and every caller of its entry points.
- Each closure is a programme of several agents, not one piece. The list below gives the order and the first agent-sized slice of each.

| Order | Closure | Memory it frees from the window | Needs first | First slice (one agent) |
|---|---|---|---|---|
| B1 | Motion clip nodes | the per-clip heap copies (19.75 MiB copied in this run, MEASURED) | Mtn fully native (`docs/ARAM.md`), and the 271-frame dependency on node lifetime reproduced as an explicit rule (`native_motion_store.h:22-26`) | name the code that reads [r13+0x5F70] after its node is freed, and write the rule as a contract test |
| B2 | Effect system (Sonicteam heap) | the effect heap buffer (size UNRESOLVED) | the effect bodies native; the heap class is already proven (`sonicteam_heap.cpp`) | measure the buffer size and its readers with M0 and a `--watch-guest` trace |
| B3 | Geometry vertex and index arrays | the share M0 attributes to RW geometry | the renderer fully lifted; every translated reader of geometry arrays (instancing, skinning, collision build) native | list the translated readers of geometry arrays with an access trace |
| B4 | GCAX / AX context (921,600 B) and voice blocks | about 0.9 MB | GCAX phases 3-4 (`docs/GCAX_SOUND_MAP.md`) | as planned there |
| B5 | RenderWare object model (frames, atomics, clumps, materials, textures) | all RW objects and free lists | `docs/RENDERWARE_RECONSTRUCTION.md`; assets converted offline (`NATIVE_WINDOWS_MIGRATION.md` section 5) | the offline converter driver with a versioned manifest |
| B6 | Game objects, by class family | the objects themselves (MSL pools) | M3; B5 for anything that holds RW objects; a class with every overrider and caller native | the smallest leaf class family by the `--why-live` closure report |
| B7 | Task tree, main loop, stage manager | the remaining objects and the stage tables | B6 | n/a |

**INFERRED.** The order of the first four rows follows how little each depends on. The last three follow the migration record's "leaf-up by data ownership" rule.

### Stage C: the switch-off

Possible only after Stage B is complete:
1. Delete the guest heap region, the window and `Cpu`.
2. Turn the image's data tables (about 50.8 K pointer words) into native tables.
3. Retire the savestate format and the window-based state hash. The tape gate's "state" check is then redefined over native state, with the owner's agreement before it is.

## 5. What cannot be done piece by piece

1. **Widening pointers inside the translated code.** **INFERRED.**
   - A 32-bit field that becomes 64-bit moves every later field of its structure. Every function that touches that structure type at a fixed offset would have to change in the same build.
   - The translated code has no types. A field offset in it is just a displacement on an lwz / stw, and no tool can tell which of the 90,685 base-register loads read a pointer (section 2).
   - So pointer width can only change together with the code that owns the structure: a closure in Stage B. No "widen the pointers" piece exists.
2. **The heap itself.**
   - While any translated code still allocates or dereferences, the OS heap must stay a window region with 32-bit cells.
   - Pieces can shrink what lives in it (Stage A) and grow it (W1), but cannot remove it before the last translated reader. **READ:** `NATIVE_WINDOWS_MIGRATION.md` section 3, the "Memory" row: "last: goes with the last translated reader".
3. **The image's data sections.** **INFERRED.** Vtables are read by every virtual call in translated code (6,550 static `bcctrl`), so they stay 32-bit until their classes are native.
4. **The state hash.** **INFERRED.** It is defined over window bytes, so pieces that change the heap's contents cannot keep it. Section 3.2 is the substitute until Stage C.
5. **What cannot be proven at all with today's tapes.** **READ** (`NATIVE_WINDOWS_MIGRATION.md` section 6).
   - Only two gameplay tapes exist, and there is no stage-to-stage or save / load tape.
   - A piece that frees heap memory can be bit-identical on both tapes and still differ on a stage neither tape visits.
   - More tapes are a standing prerequisite for Stage B. This is UNRESOLVED and has no owner.

## 6. Size estimate

| Part | Estimate | Basis |
|---|---|---|
| Stage A (M0, W1, M1, M2 on 2-3 consumers, M3) | about 6-10 agent-weeks; 4-8 calendar weeks with the one-heavy-job-at-a-time machine | INFERRED from the pieces' sizes above |
| Machine time per piece | at least 1 gate (about 6 min) + 2 x 5 exclusive tape runs per tape (about 15 min), plus builds. **INFERRED:** at most 3-4 pieces per hour of machine time, whatever the number of agents | READ (`.coord` estimates); MEASURED (one tape run, 43 s wall) |
| Stage B + C (the real 64-bit program) | the remaining translated code: 37,739 functions, about 1.2 M instructions. At the project's measured rate (about 3.5 % of the instructions in six weeks, READ `NATIVE_WINDOWS_MIGRATION.md`), about **170 weeks, roughly 3 years**, at the same effort | READ, INFERRED |
| What could shorten it | more parallel agents help only until the machine (one heavy job at a time) and the review of each closure become the bottleneck. **INFERRED:** at best a factor of 2-3, so **1-1.5 years**, with the owner's rule that nothing gets slower adding to it | INFERRED |

**INFERRED.** The honest summary:
- The memory **limit** can be gone in about two months (Stage A).
- The game **as a real 64-bit program** is a multi-year native rewrite. The memory work cannot shortcut it.
- Stage A should not be sold as that rewrite. It is the part that fixes the mods.

## 7. The first three pieces

1. **M0: Heap attribution census.** Without it, M1 and M2 choose targets blind. It changes no behaviour, and it is the only piece with no dependency.
   - **Files:**
     - `runtime/src/gekko/native_os_heap.cpp`
     - new `runtime/include/gekko/heap_attribution.h` and `runtime/src/gekko/heap_attribution.cpp`
     - `runtime/src/gekko/heap_census.cpp`
     - new `runtime/tests/heap_attribution_tests.cpp` and the test's CMake entry
2. **W1: A larger window heap for mod runs (to about 1 GiB).** It is the direct continuation of the release session's 256 MiB fix, and it is bit-identical in runs without mods.
   - **Files:**
     - `runtime/include/gekko/guest_memory.h` and `runtime/src/gekko/guest_memory.cpp`
     - `runtime/include/gekko/native_motion_store.h` and `runtime/src/gekko/native_motion_store.cpp`
     - `runtime/src/gekko/cpu.cpp`
     - `runtime/src/gekko/savestate.cpp`
     - `runtime/src/shadow_main.cpp`
     - `runtime/scaffold/rw_driver/far_objects.cpp`
   - Starts after the release session commits its memory work.
3. **M1: Texture texels out of the window, behind handles.** It is the first move of main-memory data (not ARAM data) out of the window. It proves the poison / release method that every later piece reuses, and it removes a duplicate: each texture is held in the heap and on the GPU.
   - **Files:**
     - `runtime/src/gekko/native_rw_raster.cpp` and `runtime/include/gekko/native_rw_raster.h`
     - `native_rw_raster_image.cpp`, `native_rw_image.cpp`, `native_rw_display.cpp`
     - new `runtime/include/gekko/native_texel_store.h` and `runtime/src/gekko/native_texel_store.cpp`
     - `runtime/scaffold/rw_driver/textures.cpp`
     - `runtime/src/gekko/savestate.cpp`
     - `runtime/src/gekko/system.cpp` (install)
     - a new raster contract test
