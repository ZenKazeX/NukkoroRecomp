# rwcore: RenderWare Graphics 3.7, native x64

The foundation of the PC port: RenderWare's engine core and world layer rebuilt as plain C++20 from the game's own
RenderWare functions (PAL main.dol), without the GameCube driver (GX, paired-single/locked-cache paths, ARAM, the
Dolphin OS). No guest memory, no emulation, no dependency on `runtime/`.

## Rules

- **Names and layouts are RenderWare's.** API names, structures and constants come from the RW 3.7 SDK headers
  (`RW37SDK/Library NULL Hdr Files/rwplcore.h`, `rwcore.h`, `rpworld.h`), so game code and the RW API reference read
  the same.
- **Behaviour is the game's.** Each function is written from the original RenderWare function in main.dol and keeps
  its arithmetic order (fused multiply-adds where the original fuses), so finite results are bit-identical. Where the
  original is wrong, unsafe or GameCube-bound in a case the game does not rely on, the port differs on purpose and
  says so ("Native contracts" below).
- **Every function is proven.** A `runtime/tests/rwcore_*_tests.cpp` oracle runs the ORIGINAL instructions in the
  reference interpreter and the native function on the same inputs and compares every output bit; each test states
  its input domain and has been shown to fail on a deliberately wrong implementation (negative control). Native
  tests check what an oracle cannot: correctness against double-precision math, the engine's life, 64-bit limits,
  and that other compilers reproduce the proven bits.
- **Names of original functions are evidence-based** (`tools/dolrecon/rwalign.py`, then each function's code and the
  PC libraries' source order, `RW37SDK/_mined/lib_function_order.csv`): when the code contradicts a name, the code
  wins and the header says so (0x804834A8 is RwV3dTransformPoint, not RwV3dTransformVector; 0x8047EB88 /
  0x8047EBC0, named after the plugin-registry callbacks they are passed as, are _rwErrorOpen / _rwErrorClose).
- **Only what the game links.** A RenderWare function the game does not contain has no original to prove against and
  is not provided (bavector's RwV2dNormalize, RwV3dTransformVectors). "Not linked" is checked against the code
  around each module: RwV3dNormalize (0x8048327C) and RwV3dLength (0x80483350) were first listed here wrongly.

## 64-bit model

RenderWare is written against `RwInt32` and pointers; the port keeps RenderWare's types and widens what is an
address or a size, so the core can use all of a modern machine's memory:

- **Types** (`rw/core/types.h`): RenderWare's fixed-size types keep their width (`RwInt32` is 32 bits, as in the
  binary stream format); pointers are native 64-bit; byte counts are `size_t` (RenderWare's own `RwMemoryFunctions`
  already take `size_t`).
- **Layouts**: structures follow the header's field order and names. Pointer-free structures keep the GameCube byte
  layout (`RwMatrix`, 64 bytes, asserted); structures with pointers grow on x64, which nothing depends on: modules
  reach their globals through `RwModuleInfo` offsets (`rw/core/engine.h`), as the originals do, and stream data is
  converted field by field, never copied as a struct image.
- **Memory** (`rw/core/engine.h`): the engine's `RwMemoryFunctions` default to the host allocator - no fixed arena,
  no 32-bit size or address arithmetic. An application may install its own (RenderWare's RwEngineInit contract).
- **Files**: streams read and write the host's files (`RwFopen` family over C files); there is no disc layer.
  File positions and seek offsets are RwInt64 (`_fseeki64` / `fseeko`), so files past 2 and 4 GiB work (the
  original's `long` / RwInt32 would turn a skip of 2 GiB or more negative). Memory streams keep size_t positions
  and sizes.
- **Counts and offsets**: RwSList counts and plugin offsets keep RenderWare's RwInt32; reaching its end fails
  cleanly (E_RW_NOMEM / -1) instead of wrapping as the original's arithmetic does.
- **Floating point**: no implicit contraction of a*b+c on any compiler (`/fp:precise`; `-ffp-contract=off` for
  GCC / Clang, `/clang:-ffp-contract=off` for clang-cl): Clang's default fuses on FMA-capable targets (AVX2, every
  ARM64 / Android device) and was measured to change the results (`rwcore_fp_digest_tests`).
- **Plugin data**: plugin slices of extensible objects start at multiples of 8 (pointer and double alignment),
  where the GameCube rounds sizes to 4 (`rw/core/plugin.h`).
- **Engine instance**: `RwEngineInstance` is RwGlobals (`rwplcore.h`, release layout, native pointers) followed by
  the module globals at their registered offsets.

## Native contracts (where the port deliberately differs from the original)

Each is a case the game does not rely on; each is covered by a test.

- **RwMatrixInvert with dst == src** reads from a copy and returns the right inverse. RenderWare forbids the overlap
  ("must not point to the same matrix"); the original then reads elements it has overwritten and returns a wrong
  inverse. All 9 calls in the game invert into another matrix. **RwMatrixMultiply** likewise computes every row before
  storing, so any overlap gives the true product.
- **Free lists** refuse an entry size or per-block count <= 0 and an alignment that is not a power of two
  (E_RW_BADPARAM); the original divides by zero / misplaces entries later. **RwSList** growth and **plugin** offsets
  stop at RwInt32's end; plugin sizes < 0 are refused.
- **Plugin slices** are 8-aligned (the GameCube's 4 would misalign pointers).
- **_rwVectorOpen** frees its first table when the second cannot be allocated (the original leaks it: no close follows
  a failed open).
- **Errors raised before the error module is open** (RwEngineOpen's) are held aside and handed to the error module when
  it opens; the original writes them over RwGlobals::curCamera.
- **The engine's life** checks each step's state (the original would call a NULL device function); RwEngineInit
  checks the state before touching anything; the engine instance is zeroed; a device that fails to register fails the
  open; missing standard functions become "not supported" ones; a device that refuses to stop still leaves the engine
  opened (its modules are closed by then).
- **Frames** (rw/core/frame.h): hierarchies of any depth (every walk is iterative, in the original's visiting order;
  the original recurses, unrolled five levels, and would exhaust its stack on deep chains); RwFrameDestroy turns its
  children into roots marked for an update (the original leaves their root pointing at the freed frame, so the next
  RwFrameGetLTM reads freed memory) and detaches attached objects (left pointing into the freed frame);
  RwFrameDestroyHierarchy detaches the hierarchy from its parent first (the original leaves a dangling child pointer);
  a cycle (RwFrameAddChild of an ancestor) is refused with E_RW_BADPARAM (the original builds it, and every walk then
  loops forever); RwFrameRemoveChild of a parentless frame changes nothing (the original dereferences NULL);
  RwFrameCreate fails when a plugin constructor fails (the original returns the half-built frame) or no frame module
  is open; a failed clone frees its unconstructed copies without destructors and restores the sources' root fields
  (the original runs destructors on plugin data never constructed and leaves the sources pointing at freed copies);
  copies do not inherit "in the dirty list" bits; frames are 16-byte aligned (the GameCube's are 4) so both matrices
  are; matrix pads start at 0. Each is covered by rwcore_frame_native_tests and has a negative control.
- **Cameras** (rw/core/camera.h): a refused RwCameraBeginUpdate restores the previous current camera (the original
  leaves the refused one current); RwCameraDestroy of the current camera clears RwGlobals::curCamera (the original
  leaves it dangling); RwCameraCreate zero-fills the camera (the original leaves the view matrix and frustum as memory
  held them until the first sync), fails when a plugin constructor fails (the original returns the half-built camera)
  and when no camera module is open; RwCameraStreamRead reads at most 32 bytes of the struct chunk and skips the rest
  (the original reads the whole chunk into a 32-byte stack buffer) and destroys the camera when its plugin data fails
  to read (the original leaks it). Cameras are 16-byte aligned. Unchanged on purpose: the frustum is the original's
  float arithmetic in world space, so a box narrower than a float step at its far corners (width below about far x 2^-23)
  loses its side planes exactly as the original does (corners coincide, the normal is 0); an invalid projection read
  from a stream still returns the camera with E_RW_BADPARAM; RwCameraShowRaster without a frame buffer fails
  (RwRasterShowRaster returns the raster). No aspect ratio is assumed anywhere: the view window's x and y are independent, and a Hor+
  window (width = height x the screen's aspect) gives square pixels at any aspect (rwcore_camera_native_tests).
- **Rasters and textures** (rw/core/raster.h, texture.h): RwRasterCreate / RwTextureCreate / RwTexDictionaryCreate
  fail when a plugin constructor fails (the originals return the half-built object) or their module is not open; the
  texture module's open undoes its instance count when it fails (the original leaves it counted); RwTexDictionaryDestroy
  unlinks a texture that survives it (still referenced elsewhere: the original leaves it linked into the freed
  dictionary). The host device keeps raster pixels in CPU memory, every mip level (8888 / 888 / default formats; others
  refused), with RenderWare's lock semantics; camera and Z rasters have none (the renderer's buffers). The default
  texture read callback finds nothing (no image files: stage textures come from converted dictionaries;
  RwTextureSetReadCallBack installs a loader); the GameCube's image reading, mipmap building and mipmap names are not
  ported. Kept as the original: RwTextureRead's hit adds a reference without checking the mask; names compare ASCII
  case-insensitively.
- **Materials and string chunks** (rpworld rw/world/material.h; rw/core/binary.h, texture.h): RpMaterialCreate fails
  when a plugin constructor fails or no material module is open; RpMaterialStreamRead reads at most 28 bytes of its
  struct and RwTextureStreamRead at most 4 (the originals read the whole chunk into 28- and 4-byte stack buffers),
  names into 128-byte buffers through _rwStringStreamFindAndReadCapped (the original reads any length into them);
  RwTextureStreamRead drops the reference it took when the plugin data fails (the original keeps it);
  _rwStringStreamFindAndRead frees the buffer it allocated when a read fails (the original leaks it on the ASCII path)
  and reads Unicode string chunks as RenderWare does on a PC (the GameCube's build loads each 16-bit unit big-endian
  and keeps its zero byte). Kept: the material's own version checks after RwStreamFindChunk (redundant: the find
  refuses those versions first).
- **Lights** (rpworld, rw/world/light.h): RpLightCreate zero-fills the light, fails when a plugin constructor fails
  (the original returns the half-built light) or no light module is open; RpLightStreamRead reads at most 24 bytes of
  the struct chunk and skips the rest (the original reads the whole chunk into a 24-byte stack buffer) and destroys
  the light when its plugin data fails to read (the original leaks it); _rpLightClose of a module whose open failed
  walks no list. Kept as the original: the cone angle's acosf uses _rwSqrt (worst error 8.8e-5 rad against acos,
  measured), angles outside 0 .. pi / 2 are refused, the pre-3.3 tangent conversion is unreachable after the version
  check.
- **Worlds** (rpworld, rw/world/world.h): RpWorldStreamRead reads at most 0x40 bytes of the world struct and 0x18 of
  a plane struct and skips the rest (the originals read the whole chunk into 0x40- / 0x18-byte stack buffers); it
  refuses GameCube-native worlds (rpWORLDNATIVE: the offline converter writes platform-independent sectors) with
  E_RW_BADPARAM and more than 8 texture coordinate sets; the world's single block is handed out with alignment (16 for
  sectors, 8 / 4 for arrays) and its end checked (the original trusts the struct's totals and overruns its block); a
  sector's counts must fit their 16-bit fields, its struct chunk must hold its 0x2C header and arrays (bytes past them
  are skipped), its triangles' vertex indices must lie inside it, a short prelit read fails (the original accepts any
  non-empty one), plane types must be 0 / 4 / 8; a material list count < 0, an index that is not an earlier entry and
  a failed append fail the read (the original reads uninitialised slots or carries on without the material); a mesh's
  count must fit 16 bits, its materials lie in the list and its indices within the total, and a failed mesh read
  frees its header (the original leaks it); a second mesh chunk frees the first; a sector whose world is not
  registered fails its mesh read (the original dereferences NULL); a sector without a mesh chunk fails the read with
  E_RW_BADPARAM (the original builds one: the mesh generators are not ported); a read failing before a sector's or
  the world's plugin data is constructed runs no plugin destructor on it (the original does); every walk is iterative
  (the original's use 64-entry stacks: deeper BSPs overflow them); renderCallBack is NULL (the GameCube's 0x80460B60
  draws with GX). Lights: RpWorldDestroy takes its lights out first (the original leaves their world pointers and
  links dangling), RpWorldAddLight of a light in a world moves it (the original links it twice), a light destroyed in a
  world leaves it, RpWorldRemoveLight resets the light's link, a failing tie allocation raises E_RW_NOMEM (the original
  does not check); the light ties' free list is the world module's (the original's is the world-object module
  0x509's). Kept as the original: the colSectorSize test on the unswapped bytes, worldOrigin = -1 x the stream's
  inverse origin, sector meshes indexing the world's whole material list (matListWindowBase is not applied), an
  all-zero mesh header never freed, short value reads converting the whole word (RwStreamReadInt32's rule).
- **Free-list speed** (rw/core/memory.h): each list keeps an index (blocks by address, non-full blocks newest first,
  a used count per block), so finding an entry's block and the first block with room are O(log blocks) instead of
  the original's linear scans; every block, bitmap and entry is still the original's (rwcore_memory_tests).
- **Writers** (RwStreamWriteInt32 / Real / Int16) write in one call on a little-endian host where the original swaps
  256-byte copies: the same bytes; a memory stream may grow in one step (capacity only).
- **Images** (rw/core/image.h): native structures (RwImage with 64-bit pointers, the module's buffer sizes size_t);
  RwImageCreate fails when a plugin constructor fails (the original returns the half-built image) or no image module
  is open, and starts the stride at 0 (the original leaves the free list's bytes until RwImageAllocatePixels);
  RwImageAllocatePixels refuses a negative size or a stride past RwInt32 with E_RW_NOMEM before allocating (the
  original's 32-bit arithmetic wraps); RwImageApplyMask returns NULL when the palettised image's new 32-bit pixels
  cannot be allocated (the original then copies into NULL pixels); RwImageCopy of a pair ImageConvertDepth refuses
  returns NULL and leaves the destination alone (the original then writes the gamma flag through NULL);
  RwImageFindRasterFormat's stack raster starts zeroed (the original's holds stack leftovers); _rwImageOpen undoes its
  instance count when the scratch buffer fails (the original leaves it counted); _rwpathisabsolute does not read past
  an empty name's NUL (same result). Kept as the original: RwImageFreePixels frees cpPixels whoever owns it;
  RwImageAllocatePixels of an allocated image allocates again; extensions compare case-sensitively (both of a
  format's strings); an empty path element searches the bare name; RwImageReadMaskedImage leaves an image whose mask
  is not found destroyed. The game links no RwImageRegisterImageFormat, so its format list stays empty and every
  image read finds nothing; the native keeps RenderWare's list (rwImageGlobals::imageFormatList), where the tests
  place formats. RwFexist is the file interface's fexist (else an fopen that succeeds), where the GameCube's is its
  disc file system. The gamma tables use MSL's pow (math/msl.h, proven by native_msl_tests) in IEEE mode: the game
  runs with FPSCR[NI], which changes no table byte (a flushed value is below 2^-126, where 0.5 + 255 x truncates to 0
  either way).
- **Resources arena** (rw/core/resources.h): RwResourcesAllocateResEntry refuses a negative size and an arena of size
  0 at once (no eviction, *ownerRef NULL, E_RW_INSUFFICIENTRESOURCES with the size; the original allocates a block too
  small for the entry header for the first and reads through NULL for the second); a failed open clears memHeap (the
  original leaves it pointing at the memory it freed); _rwResourcesPurge does nothing while no resources module is
  open; the heap refuses sizes whose rounding to 32 would wrap and heaps past RwUInt32 (the original wraps). 64-bit
  layout: the heap's block header stays 32 bytes (RwUInt32 size and flags), so blocks lie where the original puts
  them; rwResHeap (two pointers) is 16 bytes, so the first block is the original's for memory 0..16 or 25..31 bytes
  past a 32-byte boundary (every 8- and 16-byte aligned allocation); RwResEntry is 48 bytes (the GameCube's 24), so an
  entry of n data bytes takes the original's block only for n = 9..16 modulo 32 and up to 32 bytes more otherwise - the
  arena counts native bytes. An arena's capacity depends on the alignment of its memory, as in the original (32-byte
  aligned memory gives the GameCube's). Kept as the original: the destroy callback runs before the owner pointer is
  cleared; RwResourcesEmptyArena walks entriesA then entriesB; maxSize is RwInt32 (an arena of 2 GiB or more reads
  back negative; not exercised).
- **Resampling** (rw/core/image.h, baresamp): RwImageResample returns NULL and leaves the destination alone for a
  depth other than 32, missing pixels, a step below 1 with something to fill (a source of size 0, or a destination
  over 65536 times larger: the original loops on a wrapped count) and sources over 32766 pixels (their 16.16
  coordinates overflow); a box whose float step reaches past the source reads its last row, then last column (the
  original reads the memory after the buffer; the weight is at most 2.3e-4 of a pixel for sizes below 4000).
  RwImageCreateResample returns NULL for sizes below 1 (RenderWare's debug build asserts) and when RwImageCopy cannot
  convert the source (the original resamples the copy's uninitialised pixels), freeing what it made. Kept as the
  original: the 1/255 rounded up (0x3B808081, so an average of 255s comes out just above 1), the byte conversion
  fused, NaN spans (a step of 1: width 0) giving byte 0.
- **Palette quantisation** (rw/core/palquant.h): native nodes are 160 bytes (the GameCube's 0x5C), from a free list
  of sizeof node with their alignment; QuantDepth is the constant 6 (RwPalQuantSetMaxDepth is not linked). Native:
  RwPalQuantInit returns FALSE (what it made released) when the free list or the root cannot be allocated (the
  original returns TRUE and writes through NULL); a node that cannot be allocated leaves its pixel out (the original
  writes through NULL); sizes below 1 add / match nothing (the original loops on a wrapped count for negative ones);
  RwPalQuantResolvePalette takes maxcols over 256 as 256 (the original overruns its 256 boxes); when its median cut
  stops short of maxcols (no box left with variance, or a cut whose float sums cannot show the reduction: images of
  weight 0, or a few outlying pixels in a large image), the boxes not made keep their cleared entries and take no
  leaf - what the original gives on a zeroed RwPalQuant (it reads the boxes it did not set: an earlier resolve's, or
  whatever its stack held); a colour matched that was never added gives index 0 (the original reads through NULL).
  Kept as the original: palette images are read one index a byte (an index past the palette is the caller's error,
  as in RwImageCopy); dstPacked must be exactly TRUE to pack; leaf variance NaN (0 / 0 for weight 0) is kept, not
  clamped; RwPalQuantTerm leaves cubefreelist set.
- **Not yet**: the core modules not yet ported are not registered by RwEngineInit (pipeline, chunk group, immediate
  mode - in that slot order, between texture and resources); the host device has no image / raster
  exchange (rwSTANDARDIMAGEGETRASTER, RASTERSETIMAGE, IMAGEFINDRASTERFORMAT, PIXELTORGB say "not supported": the
  RenderWare PC driver brings them).

## Modules

| module (RW source) | header | functions (original address) | proof |
|---|---|---|---|
| bamatrix | `rw/core/matrix.h` | RwMatrixMultiply (0x8047FC08), MatrixMultiply (scalar, 0x8047F150), RwMatrixUpdate (0x8047FBF0), _rwMatrixDeterminant (0x8047F754), _rwMatrixOrthogonalError (0x8047F7A0), _rwMatrixNormalError (0x8047F7F8), RwEngineSetMatrixTolerances (0x8047F9B0), RwMatrixOptimize (0x8047F9E0), RwMatrixOrthoNormalize (0x8047FD04 -> 0x8047F2E8), RwMatrixRotateOneMinusCosineSine (0x8047FD24), RwMatrixRotate (0x80480018), RwMatrixInvert (0x80480100), RwMatrixScale (0x804803E8), RwMatrixTranslate (0x804805EC), RwMatrixTransform (0x8048077C), RwMatrixDestroy (0x804809A8), RwMatrixCreate (0x804809E4), _rwMatrixOpen (0x8047F8DC), _rwMatrixClose (0x8047F878) | `rwcore_matrix_tests`; open / close in `rwcore_memory_tests` |
| bavector | `rw/core/vector.h` | _rwVectorOpen (0x8048363C), _rwVectorClose (0x80483598), _rwSqrt (0x80483390), _rwInvSqrt (0x804833E4), RwV2dLength (0x80483440), RwV3dNormalize (0x8048327C), RwV3dLength (0x80483350), RwV3dTransformPoint (0x804834A8), RwV3dTransformPoints (0x804834F4), RwV3dTransformVector (0x80483554) | `rwcore_vector_tests` |
| baerr | `rw/core/error.h` | _rwErrorOpen (0x8047EB88), _rwErrorClose (0x8047EBC0), RwErrorSet (0x8047EBD4), RwErrorGet (0x8047EC34), _rwerror (0x8047EC78) | `rwcore_vector_tests` |
| bamemory | `rw/core/memory.h` | _rwFreeListEnable (0x80480A78), RwFreeListCreateAndPreallocateSpace (0x80480C54 -> FreeListCreate 0x80480A80), RwFreeListCreate (0x80480C28), RwFreeListDestroy (0x80480D2C), _rwFreeListAllocReal (0x80480E00), _rwFreeListFreeReal (0x80480FA0), RwFreeListForAllUsed (0x8048112C), _rwMemoryOpen (0x804812C4), _rwMemoryClose (0x804813DC), RwOsGetMemoryInterface (0x80481580) | `rwcore_memory_tests` |
| balist | `rw/core/list.h` | _rwSListCreate (0x8047ECE0), _rwSListEmpty (0x8047EE00), _rwSListGetNewEntry (0x8047EE0C), _rwSListGetNewEntries (0x8047EEF0), _rwSListDestroyEndEntries (0x8047EFEC), _rwSListGetArray (0x8047EFFC), _rwSListDestroy (0x8047F018), _rwSListGetNumEntries (0x8047F08C), _rwSListToArray (0x8047F094), _rwSListGetEntry (0x8047F11C), _rwSListGetBegin (0x8047F130), _rwSListGetEnd (0x8047F138) | `rwcore_list_tests` |
| batkreg | `rw/core/plugin.h` | _rwPluginRegistryOpen (0x80482BE8), _rwPluginRegistryClose (0x80482CA8, with rwDestroyEntry 0x80482C44), _rwPluginRegistryGetPluginOffset (0x80482DE8), _rwPluginRegistryAddPlugin (0x80482E18, defaults 0x80482DDC / E0 / E4), _rwPluginRegistryInitObject (0x804830EC), _rwPluginRegistryDeInitObject (0x80483194), _rwPluginRegistryCopyObject (0x80483200) | `rwcore_plugin_tests` |
| bastream | `rw/core/stream.h` | _rwStreamModuleOpen (0x80481CBC), _rwStreamModuleClose (0x80481D48), RwStreamRead (0x80481D9C), RwStreamWrite (0x80481F20), RwStreamSkip (0x8048214C), RwStreamClose (0x8048229C), RwStreamOpen (0x804823B8); files through RwFopen / RwFclose / RwFread / RwFwrite / RwFseek / RwFeof / RwFtell on the host's C files (the GameCube's are its disc file system, 0x8047B1C8..0x8047B810, not ported) | `rwcore_stream_tests` |
| babinary | `rw/core/binary.h` | _rwChunkIsComplex (0x8047DD48), _rwStreamReadChunkHeader (0x8047DE04), _rwStreamWriteVersionedChunkHeader (0x8047DF40), RwStreamFindChunk (0x8047DFA0), RwMemLittleEndian32 / 16 (0x8047E1A8 / 0x8047E218), RwMemNative32 / 16 (0x8047E2A4 / 0x8047E314), RwStreamReadInt16 (0x8047E6D4), RwStreamReadReal (0x8047E7D4), RwStreamReadInt32 (0x8047E8B8), RwStreamReadChunkHeaderInfo (0x8047E99C), RwStreamWriteReal (0x8047E3A0), RwStreamWriteInt32 (0x8047E4A8), RwStreamWriteInt16 (0x8047E5B0) | `rwcore_binary_tests` |
| baframe | `rw/core/frame.h` | _rwFrameOpen (0x804878D8), _rwFrameClose (0x8048797C), rwSetHierarchyRoot (0x804879E0), rwFrameCloneRecurse (0x80487AF4), _rwFrameCloneAndLinkClones (0x80487CAC), _rwFramePurgeClone (0x80487CF4), RwFrameDirty (0x80487D40), RwFrameCreate (0x80487D50), RwFrameDestroy (0x80487E7C), _rwFrameDeInit (0x80487F28), its free step (0x80487F84), rwFrameDestroyRecurse (0x80487FFC), RwFrameDestroyHierarchy (0x80488258), RwFrameUpdateObjects (0x8048827C), RwFrameGetLTM (0x804882E8), RwFrameGetRoot (0x80488328), RwFrameAddChildNoUpdate (0x80488330), RwFrameAddChild (0x80488398), RwFrameRemoveChild (0x80488498), RwFrameForAllChildren (0x8048857C), RwFrameTranslate / Scale / Transform / Rotate (0x80488600 / 0x8048869C / 0x80488738 / 0x804887D4), RwFrameSetIdentity (0x80488870), RwFrameRegisterPlugin (0x80488924) | `rwcore_frame_tests`, `rwcore_frame_native_tests` |
| basync | `rw/core/frame.h` | FrameSyncHierarchyRecurse (0x8048BE30), FrameSyncHierarchyRecurseNoLTM (0x8048C040), the per-root step (0x8048C258), _rwFrameSyncDirty (0x8048C3A4), FrameSyncHierarchyLTMRecurse (0x8048C410), _rwFrameSyncHierarchyLTM (0x8048C628) | `rwcore_frame_tests`, `rwcore_frame_native_tests` |
| batypehf | `rw/core/frame.h` | _rwObjectHasFrameSetFrame (0x8048E810), _rwObjectHasFrameReleaseFrame (0x8048E884) | `rwcore_frame_tests` |
| bacamera | `rw/core/camera.h` | CameraBuildPerspClipPlanes (0x80485318), CameraBuildPerspViewMatrix (0x80485AFC), CameraBuildParallelClipPlanes (0x80485C60), CameraBuildParallelViewMatrix (0x80486174), CameraSync (0x804862DC), CameraEndUpdate (0x80486340), CameraBeginUpdate (0x804863A4), _rwCameraClose (0x80486410), _rwCameraOpen (0x80486474), RwCameraEndUpdate (0x80486504), RwCameraBeginUpdate (0x8048652C), RwCameraSetViewOffset (0x80486554), RwCameraSetNearClipPlane (0x804865A0), RwCameraSetFarClipPlane (0x8048665C), RwCameraFrustumTestSphere (0x80486718), RwCameraClear (0x8048680C), RwCameraShowRaster (0x80486858 -> RwRasterShowRaster 0x8048AD94), RwCameraSetProjection (0x8048689C), RwCameraSetViewWindow (0x804869A0), RwCameraRegisterPlugin (0x80486A08), RwCameraDestroy (0x80486A48), RwCameraCreate (0x80486AA8); CameraUpdateZShiftScale inlined at its callers | `rwcore_camera_tests`, `rwcore_camera_native_tests` |
| babincam | `rw/core/camera.h` | RwCameraRegisterPluginStream (0x804845A4), RwCameraStreamRead (0x804845E0) | `rwcore_camera_tests`, `rwcore_camera_native_tests` |
| babbox | `rw/core/bbox.h` | RwBBoxCalculate (0x804844EC) | `rwcore_camera_tests` (inside the camera sync), `rwcore_camera_native_tests` |
| bamateri (rpworld) | `rw/world/material.h` | _rpMaterialOpen (0x8045C098), _rpMaterialClose (0x8045C128), RpMaterialCreate (0x8045C18C), RpMaterialDestroy (0x8045C244), RpMaterialSetTexture (0x8045C2C0), RpMaterialRegisterPlugin (0x8045C31C), RpMaterialRegisterPluginStream (0x8045C35C), RpMaterialStreamRead (0x8045C398; the rights a plugin claims, statics 0x805F2588 / 0x805F258C, invoked after the plugin data) | `rpworld_material_tests`, `rpworld_material_native_tests` |
| bamatlst (rpworld) | `rw/world/material.h` | _rpMaterialListDeinitialize (0x8045C740), _rpMaterialListInitialize (0x8045C7D8), _rpMaterialListGetMaterial (0x8045C7EC), _rpMaterialListAppendMaterial (0x8045C7FC), _rpMaterialListFindMaterialIndex (0x8045C93C); _rpMaterialListStreamRead (0x8045C974) | `rpworld_material_tests`, `rpworld_material_native_tests`; the stream read: `rpworld_world_tests`, `rpworld_world_native_tests` |
| babintex | `rw/core/texture.h` | RwTextureRegisterPluginStream (0x80484B4C), RwTextureStreamRead (0x80484D78) | `rpworld_material_tests`, `rpworld_material_native_tests` |
| bastring | `rw/core/binary.h` | _rwStringStreamFindAndRead (0x804840B0); the bounded form (native) | `rpworld_material_tests`, `rpworld_material_native_tests` |
| baraster | `rw/core/raster.h` | RwRasterUnlock (0x8048AB90), RwRasterDestroy (0x8048AC2C), RwRasterRegisterPlugin (0x8048ACA4), RwRasterGetNumLevels (0x8048AD34), RwRasterCreate (0x8048AEC4), RwRasterLock (0x8048AFC0), _rwRasterClose (0x8048B010), _rwRasterOpen (0x8048B07C); the host device's raster storage (native) | `rwcore_texture_tests`, `rwcore_texture_native_tests` |
| batextur | `rw/core/texture.h` | TextureDefaultFind (0x8048D96C), RwTextureSetMipmapping / Get / SetAutoMipmapping / Get (0x8048D9F8 / 0x8048DA10 / 0x8048DA24 / 0x8048DA3C), RwTextureSetRaster (0x8048DA50), RwTexDictionaryCreate (0x8048DAB4), RwTexDictionaryDestroy (0x8048DB88), RwTexDictionaryForAllTextures (0x8048DC30), RwTexDictionaryForAllTexDictionaries (0x8048DCA4), RwTextureCreate (0x8048DD2C), RwTextureDestroy (0x8048DDDC), RwTextureSetName / SetMaskName (0x8048DEA4 / 0x8048DF50), RwTexDictionaryAddTexture (0x8048DFFC), RwTexDictionaryFindNamedTexture (0x8048E04C), RwTexDictionarySetCurrent / GetCurrent (0x8048E114 / 0x8048E128), RwTextureRead (0x8048E180), RwTextureRegisterPlugin (0x8048E2D0), _rwTextureClose (0x8048E350), _rwTextureOpen (0x8048E52C; the naming project's "_rwPluginRegistryAddPlugin_*CB" labels of these two are wrong: RwEngineInit registers them in the texture slot) | `rwcore_texture_tests`, `rwcore_texture_native_tests` |
| baimage / baimras | `rw/core/image.h` | _rwImageOpen (0x80488964), _rwImageClose (0x80488C00), RwImageCreate (0x80488D58), RwImageDestroy (0x80488E00), RwImageAllocatePixels (0x80488E68), RwImageFreePixels (0x80488FA0), RwImageMakeMask (0x80488FF8), RwImageApplyMask (0x804890D8), RwImageSetPath (0x8048962C), ImagePathForAllFullNames (0x8048971C, ImageGetScratchMem inlined), ImageAttempRead (0x80489A54), ImageDetermineExtender (0x80489AC0), RwImageFindFileType (0x80489BA8), RwImageReadMaskedImage (0x80489BE4, RwImageRead inlined), RwRGBASetFromPixel (0x8048A168; labelled rtpitexdCreateImageFromRaster__sub0 by the naming project), ImageConvertDepth (0x8048A1AC), RwImageCopy (0x8048A408, ImageStraightCopy inlined), RwImageGammaCorrect (0x8048A508), RwImageSetGamma (0x8048A880, also at RwEngineStart), RwImageSetFromRaster (0x8048A9F8), RwRasterSetFromImage (0x8048AA6C), RwImageFindRasterFormat (0x8048AAE0), RwRasterGetOffset (0x8048AB7C, baraster), _rwpathisabsolute (0x80484494, osintf); RwFexist through the file interface | `rwcore_image_tests`, `rwcore_image_native_tests` |
| balight (rpworld) | `rw/world/light.h` | LightTidyDestroyLight (0x8045B7B0), LightSync (0x8045B7D0), RpLightSetRadius (0x8045B7D4), RpLightSetColor (0x8045B814), RpLightGetConeAngle (0x8045B868), RpLightSetConeAngle (0x8045BA94), RpLightRegisterPlugin (0x8045BAF0), RpLightRegisterPluginStream (0x8045BB30), RpLightStreamRead (0x8045BB6C), RpLightDestroy (0x8045BD24), RpLightCreate (0x8045BD84), _rpLightClose (0x8045BE70), _rpLightOpen (0x8045BEE8); RpLightSetFrame / GetFrame / GetType / GetFlags / SetFlags (macros) | `rpworld_light_tests`, `rpworld_light_native_tests` |
| baworld attach (rpworld) | `rw/world/world.h` | RpWorldPluginAttach (0x8046185C): material 0x501, mesh 0x502, light 0x505, 0x50A (0x804602F0 / 0x80460304), world 0x507, 0x50B (0x80456D48 / 0x80456D34); of _rpWorldObjRegisterExtensions (0x80462E70) the light's world extension 0x509 and the sector mesh stream 0x50E; geometry 0x503, clump 0x504, the other extensions and pipelines join as ported | `rpworld_light_native_tests`, `rpworld_world_native_tests` |
| baworld / basector / babinwor / bamesh (rpworld) | `rw/world/world.h` | RpWorldStreamRead (0x80456744) with AtomicSectorRead (0x80455D34), PlaneSectorRead (0x804562FC), _rpWorldSetupSectorBoundingBoxes (0x80460BF4), _rpWorldRegisterWorld (0x80460D4C); RpWorldDestroy (0x80460FF0) with SectorDestroySingleMalloc (0x80460730); RpWorldSectorGetWorld (0x80460EF0), RpWorldForAllWorldSectors (0x80461740), RpWorldRegisterPlugin / Stream (0x804617E0 / 0x80461820), RpWorldSectorRegisterPlugin / Stream (0x80460348 / 0x80460388); WorldOpen (0x80460AA0, without the pipelines) / WorldClose (0x80460A38); _rpMeshOpen (0x8045D00C) / _rpMeshClose (0x8045CFB4), _rpMeshRead (0x8045D7C0), _rpMeshDestroy (0x8045D324), readSectorMesh (0x80462DCC); world lights: WorldInitLightExt / DeInit / Copy (0x80462598 / 0x80462604 / 0x804625C0), WorldLightSync (0x804622F8), RpWorldAddLight (0x804635C8), RpWorldRemoveLight (0x80463668), RpLightGetWorld (0x8046373C). Not ported: RpWorldCreate (0x80461188, multi-malloc worlds), the mesh rebuild pass (0x80460E00 / 0x804604A8 and the generators), the GameCube sector data 0x510 | `rpworld_world_tests`, `rpworld_world_native_tests` |
| baresour / resmem | `rw/core/resources.h` | _rwResourcesOpen (0x8048158C), _rwResourcesClose (0x804816E0), RwResourcesFreeResEntry (0x8048176C), _rwResourcesPurge (0x8048181C, also from RwRasterShowRaster 0x8048AD94), RwResourcesAllocateResEntry (0x804818C8), RwResourcesEmptyArena (0x80481B58); _rwResHeapInit (0x804839D4), _rwResHeapClose (0x80483A24), _rwResHeapFree (0x80483A2C), _rwResHeapAlloc (0x80483AE4, splitBlock inlined) - labelled _rwResourcesOpen__sub2, _rwResourcesClose__sub1, RwResourcesAllocateResEntry__sub0 by the naming project; named by their code, which is the PC libraries' resmem.obj instruction for instruction; RwResourcesUseResEntry (RenderWare's release macro) | `rwcore_resources_tests`, `rwcore_resources_native_tests` |
| baresamp | `rw/core/image.h` | ImageResampleGetSpan (0x8048B1C8), ImageResampleGetAvgPixel (0x8048B55C; inlined in the PC build, named by its debug build), RwImageResample (0x8048B7E0), RwImageCreateResample (0x8048B9BC, RwImageResample inlined twice; labelled TextureRasterDefaultBuildMipmaps__sub6 by the naming project) | `rwcore_resample_tests`, `rwcore_resample_native_tests` |
| palquant | `rw/core/palquant.h` | FromMatchSpace (0x8048E8AC), InitLeaf (0x8048E8D0), LeafCalcStats (0x8048E8F8, ToMatchSpace inlined), StatsAdd (0x8048EA18), RepresentativeColor (0x8048EB18), InitBranch (0x8048EBF4), AllocateToLeaf (0x8048EC3C), RwPalQuantAddImage (0x8048EF00, LeafAddPixel and GetOctAdr inlined), assignindex (0x8048F350), addvolume (0x8048F4CC), FindBestCut (0x8048F798, BoxStats and StatsSub inlined), nCut (0x8048F9D8), CalcNodeWeights (0x8048FB74), CountLeafs (0x8048FF4C), ExtractNodes (0x80490234), GetIndex (0x80490504), RwPalQuantResolvePalette (0x80490620, AssignPalIndex inlined), RwPalQuantMatchImage (0x80490B00), RwPalQuantInit (0x80490F28, filling splice 0x805E3E18), DeleteOctTree (0x80491124), RwPalQuantTerm (0x80491448); QuantDepth (0x805EEFD0). Static names from the PC libraries (palquant.obj symbols; the debug build's function names for those it inlines); which GameCube function is which by its calls, structure and arithmetic | `rwcore_palquant_tests`, `rwcore_palquant_native_tests` |
| batkbin | `rw/core/plugin.h` | _rwPluginRegistryAddPluginStream (0x80482640), _rwPluginRegistryAddPlgnStrmlwysCB (0x80482684), _rwPluginRegistryAddPlgnStrmRightsCB (0x804826C0), _rwPluginRegistryReadDataChunks (0x804826FC), _rwPluginRegistryInvokeRights (0x804828C0), _rwPluginRegistryGetSize (0x8048294C), _rwPluginRegistryWriteDataChunks (0x804829D0), _rwPluginRegistrySkipDataChunks (0x80482B24) | `rwcore_pluginstream_tests` |
| badevice | `rw/core/engine.h` | RwEngineInit (0x80487548), RwEngineOpen (0x80487254), RwEngineStart (0x80487030), RwEngineStop (0x80486F44), RwEngineClose (0x80487188), RwEngineTerm (0x804874FC), RwEngineRegisterPlugin (0x80486DC8), RwEngineGetPluginOffset (0x80486E08), RwEngineGetVersion (0x80486DBC), _rwDeviceSystemRequest (0x80486C8C), _rwGetNumEngineInstances (0x80486DB4), MallocWrapper / FreeWrapper (0x80486C1C / 0x80486C4C), the colour module's open / close (0x8047EB60 / 0x8047EB74); the port's host device in place of the GameCube driver's | `rwcore_lifecycle_tests` (native: the GameCube device is the driver) |
| bastring | `rw/core/engine.h` | _rwStringOpen (0x80483E70: the host C library's string functions), _rwStringClose (0x80483FB0) | `rwcore_lifecycle_tests` |
| engine | `rw/core/engine.h` | RwEngineInstance, RwGlobals, RwModuleInfo, host RwMemoryFunctions | (structure; used by every test) |

`rwcore_matrix_tests` (20,000 cases x 19 functions, 32 million original instructions, about 9 s): every output word,
in place and separate where the original allows it (MatrixMultiply with dst = a or b; RwMatrixOrthoNormalize with
dst = src, whose interleaved loads and stores the port keeps; RwMatrixInvert separate only, see "Native contracts"),
near-orthonormal matrices and
identities so RwMatrixOptimize / RwMatrixInvert take every path, zero axes for RwMatrixOrthoNormalize, valid and
invalid combine ops (the held error compared), RwMatrixCreate / RwMatrixDestroy's free-list requests. Negative controls
(each fails the test): the scalar multiply in the paired-single order, RwMatrixInvert reading src->right.y before
storing dst->right.y, RwMatrixOrthoNormalize keeping another axis pair, RwMatrixScale's post-concatenation leaving
pos, RwMatrixRotateOneMinusCosineSine fusing its diagonal. Found on the way: the original's invalid-combine path of
RwMatrixScale / RwMatrixTranslate writes through NULL + 0xC (the interpreter reports those accesses as unmapped); the
native returns NULL with the same error. RwMatrixRotate's degrees-to-radians constant is the word 0x3C8EFA35, one
ulp above the float the decimal 0.01745329 gives: constants are taken from the binary by their bits. sin / cos are
the game's MSL math (`runtime/lift/math/msl.cpp`, library `shadow_msl`, its own oracle `native_msl_tests`), shared,
not copied.

`rwcore_memory_tests` (40,000 random operations on up to 20 free lists, 4 million original instructions, about 6 s):
structure compared after every operation, since addresses differ - each list's fields, its blocks in order with
their bitmaps, every returned entry as (block, index), the master free list, the order of the list of all free lists,
every RwMalloc (a block is exactly 8 bytes larger natively: the 16-byte link) and RwFree, with in-place lists,
failed allocations, foreign frees, repeated frees of entries whose block is still there, and RwFreeListForAllUsed's
visit order. Negative controls (each fails): entries not aligned, the bitmap scanned from the low bit, emptied blocks
never freed, the master list kept among the free lists; for the block index: the oldest non-full block first, a block
kept non-full when it fills, a freed entry not making its block non-full, a repeated free counted twice (it passed
until repeated frees were added), the index forgotten at destruction (caught by rwcore_lifecycle_tests'
_rwFreeListLiveIndexes check).
Then 64 rounds of bamatrix's _rwMatrixOpen / _rwMatrixClose (every fourth with a failed allocation): the matrix free
list (the original's is its static header 0x805E3B18), matrixModule, the optimizations, the multiply function, the
tolerance bits, the list of all free lists and the calls. Negative controls (each fails): alignment 8, no block
preallocated, a tolerance one ulp off, identity optimization off, a failed open counted.
The guest heap behind the stubs is first-fit with reuse (rwcore_oracle.h).

`rwcore_plugin_tests` (400 sessions, 30,000 operations, 3.7 million original instructions): open, plugins added to
up to six registries (sizes, IDs that repeat, bounded and unbounded registries, an open engine, failed allocations),
offset queries, objects initialised with failing constructors, de-initialised and copied, close; with the free-list
allocators and with an application's own memoryAlloc / memoryFree (whose entries close finds through its list of
registries). After each operation: every registry's sizes and entries in order (offset, size, ID, callbacks, links),
the result, the held error, every plugin callback (kind, objects, offset, size) and memory call (count, hint). The
original is given every size rounded to 8, the native rule (8-aligned slices, rw/core/plugin.h), so both agree and
the offsets must be equal; a separate check shows the native rounding of odd sizes. Negative controls (each fails): a
failed init also destroying the failing plugin, de-init in registration order, a duplicate returning -1, the size bound
inclusive, a custom-allocator close leaving the registries, open keeping the registry count (crashes), an open engine
without its error.

`rwcore_pluginstream_tests` (1,500 registries, 12,000 checks, 6 million original instructions): plugins with and
without read / write / size / always / rights callbacks (set for registered and unknown IDs), extension chunks with
known and unknown plugin chunks, wrong totals, versions outside the window, a leading chunk to skip, truncation; read,
skip, size, write (bytes compared), rights. Callbacks answer from one shared script; a reader consumes the stream on
both sides. Negative controls (each fails): unknown chunks not skipped, the chunk header not counted, a failed always
callback ignored, a zero size counted, version 3.7.0.1 written, rights without a callback succeeding, a callback set on
an unknown plugin returning 0.

`rwcore_stream_tests` (300 sessions, 20,000 operations, 6 million original instructions): memory streams (read,
write, append, invalid access), caller-opened files (some refusing RwFtell), files opened by name (some failing to
open or close), custom streams (with and without a close function), invalid types; reads, writes (growth past 512
bytes, failed RwMalloc / RwRealloc), skips (past the end), closes (with and without an RwMemory to receive a written
block). The original's disc file functions are host stubs over in-memory files and the native gets the same files
through rwFileInterface, so the oracle compares every file call with its arguments, every custom-function call, the
result, the bytes read (up to the data a memory stream holds: grown blocks are uninitialised on both sides), the
stream's state, the held error, the memory calls, and a written block's bytes at close. Negative controls (each
fails): memory always growing by the write's length, a short memory read without its error, a skip past the end
keeping the position, close handing over the size instead of the position, append starting at 0, a file read error
reported as end of stream, a caller's file not checked with RwFtell.

`rwcore_binary_tests` (52,000 checks, 41 million original instructions): _rwChunkIsComplex for every type
0..0x3F and random ones; 3,000 random little-endian chunk streams (all chunk types, new and old library IDs,
versions at and around the 3.5.0.0 .. 3.7.0.2 window, payloads to skip, truncation) read through memory streams with
_rwStreamReadChunkHeader (outputs NULL or not), RwStreamReadChunkHeaderInfo, RwStreamFindChunk and the Int16 / Real /
Int32 reads, comparing results, decoded values (the guest's big-endian words against the native's), the stream
position and the held error; 8,000 written chunk headers byte for byte. Negative controls (each fails): old library
IDs not shifted, version bits from bit 12, the window ending at 3.7.0.3 or starting at 3.5.0.1 (the first run of
these controls passed: edge versions were added to the inputs), a short value read failing, a string chunk complex,
the header's build not masked. Found on the way: the value reads fail only when NO byte arrives; a short read still
converts the whole request, as the original does. The writers (2,000 rounds of Int32 / Real / Int16 runs, a stream
that refuses writes): data, position and results compared; controls: a failed write returning the stream, the last
word dropped.

`rwcore_frame_tests` (60 sessions x 400 steps, 22,000 operations, 7 million original instructions): a forest of up
to 120 frames with a frame plugin and 24 attachable objects - create, destroy leaves, destroy root hierarchies, add /
add-without-update / remove children, the four transforms and set-identity (with planted matrix flag bits it must
keep), update-objects, get-LTM (+ RwFrameDirty / RwFrameGetRoot), the dirty sync, the LTM sync alone, attach /
detach objects, for-all-children with a callback that stops early, clone-and-link (compared before and after the
purge). After every operation: each frame's object bytes, links as identities, modelling matrix, LTM (wherever no LTM
update is pending), object list order, the dirty list order, every callback in order, the held error. Coverage
(printed, and required): hierarchies 26 deep (past the original's 5 inlined recursion levels), 1.8 million LTMs
compared, 8,000 object syncs, 1,200 clones. Cases where the native differs on purpose are not generated (see "Native
contracts"). Negative controls (each fails): dirty roots at the list's tail, the object-only sync clearing the LTM
bit, the LTM sync keeping the subtree bit, copies keeping their children in source order, a copy's plugins
constructed before its children, a former root left in the dirty list, a removed child keeping its sibling link,
RwFrameDirty reporting the LTM bit only, the LTM sync leaving the root's subtree bit, objects attached at the tail,
set-identity resetting the flags (survived until flag bits were planted in the inputs), the hierarchy destroyed
parents first, the LTM taken from the root instead of the parent. One mutant is equivalent (the dirty sync's children
inheriting all the root's flags: the walk tests only the LTM bit, which is the same).

`rwcore_frame_native_tests` (20,000 checks): a 100,000-frame chain built, synced (exact sum of translations),
reparented and destroyed; a 50,000-wide hierarchy; every "Native contracts" case for frames; a clone failing at each
allocation (no leak, destructors exactly match constructors - a plugin destructor checks its constructor ran - sources
restored); a sync callback walking another dirty hierarchy mid-walk; 20,000 random operations with the dirty-list
invariant after each and every LTM against the double-precision product of its chain (worst relative error 5.2e-6),
the two sync paths bit-identical. Negative controls (each fails): each "original" behaviour put back (stale roots,
objects kept, a dangling sub-hierarchy, a failing constructor ignored, destructors on an unconstructed copy, sources
left pointing at freed copies), frames without a frame module (crashes), one flag stack shared by all walks (the
first versions of the nested-walk test let it pass; a fan of siblings after the nested walk now catches it).

`rwcore_frame_bench` (not a test; medians of 7 runs, ms, this machine): 200,000 frames created then destroyed, 1,000
random 64-frame hierarchies built, every leaf moved then one dirty sync (64,000 LTMs), a 10,000-deep chain's LTM after
moving its root. With the original's linear free-list scans: 4,790-5,129 / 385-675 / 1.18-1.80 / 0.08-0.13 (destroy is
quadratic: 80,000 frames 264 ms, measured). With the free-list index: 17.3-19.6 / 4.1-4.4 / 1.04-1.15 / 0.07-0.14 -
about 270x and 100x faster where the allocator dominated, the same where it does not.

`rwcore_camera_tests` (30,000 random operations, 19.8 million original instructions, about 9 s): up to 8 cameras
with a camera plugin, each on a two-frame chain - create, destroy, view windows (4:3, 16:9, 16:10, 21:9, 32:10 Hor+
windows, random signs and sizes, 1e-6 x 1e5 shapes, mirrored), offsets, near / far planes (0, negative, 1e-7, 1e9,
equal), projections (both, 0 and 3..7 invalid), device Z ranges, frame translate / scale / rotate on either frame of
the chain, the dirty sync, begin / end update, clear and show raster (device standard functions scripted on both
sides, frame buffer present or not), sphere tests (at corners, random, near the centroid), cameras read from memory
streams (short structs, bad versions, invalid projections). After every operation: every camera's fields bit for bit,
its frame identity and whether that frame's hierarchy has an update pending; wherever none is pending, the view matrix
(rows and flags), the eight corners, the six planes with their closest-corner bytes and the box bit for bit; every
result, device / plugin callback in order, the current camera, the held error. Coverage (printed, and required):
141,870 synced frusta compared (39,483 parallel), 26,232 sphere tests (18,454 outside, 4,054 across, 3,724 inside),
1,673 cameras read, 409 invalid projections, 131 refused begin-updates. One hardware difference is allowed and
counted: a NaN the arithmetic generates is 0x7FC00000 on the Gekko and 0xFFC00000 on x86, so two NaNs compare equal
(7,095 of about 9.5 million compared words), and a closest-corner byte is not compared where its normal is NaN
(every dot product with that normal is NaN whichever corner is taken). Found on the way: RwRasterShowRaster returns
the raster, so RwCameraShowRaster of a camera without a frame buffer fails even when the device succeeds (the first
native version returned the camera).

`rwcore_camera_native_tests` (1.18 million checks, under 1 s): the mathematics from the definition of a RenderWare
camera, not from the original's code - 20,000 random frames (rotations from random quaternions, positions to 2,000),
windows 0.1 .. 10 in x and y independently, offsets, both projections, clip ranges to 1e-3 .. 1e7, device ranges
[0, 1], [-1, 1], [0, 65535]: a point at window coordinates (u, v) and depth z, placed through the view window and the
offset eye, comes out of the view matrix at (u z, v z, z) (perspective) or (u, v, z) (parallel); the corners are those
points at the window's corners; each plane holds four corners and has the rest inside; normals are unit (to
_rwInvSqrt's precision) and point out; far / near normals are the frame's at and its opposite; the box bounds the
corners exactly; spheres inside / across / beyond are classified so; the Z scale and shift put near and far exactly
1e-4 of the device range inside its ends. Side planes exist only where float corners resolve the box's width: 659 of
the 2,000 extreme-range boxes are too thin (as in the original), no ordinary one is (required). Square pixels for Hor+
windows at 1920x1080, 2560x1600, 3440x1440, 5120x1440, 3840x1200, 5120x2880 and 5120x1600, both projections, and the
same check seeing a 4:3 window stretched there. 3,000 attach cycles of 3 cameras over 6 frames in hierarchies (each
camera follows exactly its own frame's hierarchy; detached, none; object lists exact; a destroyed frame releases its
camera). Every native contract of cameras, the device calls of begin / end / clear / show with their arguments, every
setter marking the frame, a projection change remapping Z, stream cases (long, short, bad version, truncated, failing
plugin read, invalid projection). Memory counted: no allocation outlives a shutdown.

Negative controls for both camera tests (49, `.build/dolrecon/camera_mutations.json`): every arithmetic step of the
view matrices, corners, planes, Z mapping and box; plane order, signs and through-points; closest bytes inverted or
taken by value (-0); sphere-test early exit and boundary; begin / end / destroy current-camera handling; the dirty sync
in begin-update; defaults; constructor failure; the module check; setters not marking or not remapping; clear / show
arguments and results; the stream cap, leak, fog, version bound; 8-byte alignment. 48 fail at least one test (the
oracle alone misses the native contracts, the native test alone misses single-rounding changes - each covers the
other); three survivors of the first run led to the pending-update comparison in the oracle and the setter / Z checks
in the native test. One is equivalent: RwCameraStreamRead's own version check, since RwStreamFindChunk already refuses
versions outside 3.5.0.0 .. 3.7.0.2 (the original has the same redundant check).

`rpworld_light_tests` (oracle, 400,000 cone angles + 20,000 operations, 27.8 million original instructions, 6 s):
RpLightGetConeAngle over every acosf branch (each over 50,000 inputs; the branch boundaries themselves, infinities,
NaNs, denormals), result bits compared; sessions of up to 24 lights with a light plugin - create (every RW type and
other bytes, RwGlobals::lightFrame varied), destroy, colours (grey, one channel off), radius with and without frames,
cone angles (in range, the bound pi / 2 itself, beyond, negative, NaN - MSL cos of a NaN included), frames attached and
detached with syncs, lights read from streams (short structs, bad versions); after every operation every light's
fields, frame identity and the frame's pending update, callbacks, the held error; the module's close with 24 lights
alive (the tidy's destructor calls compared). `rpworld_light_native_tests` (200,025 checks): the cone angle against
acos in double over 2 million points (worst 8.8e-5 rad, bound 2e-4), set / get round trips (worst 2.4e-4 rad, bound
5e-4), defaults, the grey flag at one ulp, the radius marking the frame, a destroyed frame releasing its light, the
close destroying live lights, every native contract, RpWorldPluginAttach, counted memory. Negative controls (29,
`.build/dolrecon/light_mutations.json`): 26 fail a test; 3 are equivalent - acosf's |x| >= 1 sign test (that branch
never sees +0), its tiny-input bound (at |x| = 2^-57 the polynomial rounds to the same pi / 2, tried with the exact
boundary input) and fusing `z - df * df` (df has 12 significant bits, so the product is exact). The oracle's frame
pending-update comparison was added for the radius control.

`rwcore_texture_tests` (oracle, 18,130 operations, 4.2 million original instructions, 1.3 s): the device's raster
functions and the texture read callback scripted alike on both sides, a raster, texture and dictionary plugin;
rasters (every type / format flag, device refusals, lock levels and modes, level counts), textures (references,
names to 40 characters - the 32-byte cut and E_RW_STRING_TRUNCATION, set raster), dictionaries (create, destroy,
add, find in every letter case, current or none), RwTextureRead (616 hits; reads through the callback), the walks
with callbacks that stop, the mipmapping switches; after every operation every object's fields as identities and
strings, dictionary and module list orders, callbacks with the references a texture destructor sees, the held error.
A new texture's name bytes after its NUL are the original's leftover memory (only name[0] / mask[0] are written), so
names compare as strings. `rwcore_texture_native_tests` (165 checks): the host device's rasters (mip chains, level
locks, pixels kept, unsupported formats, camera / Z rasters), the read path a converted-asset loader uses, every
native contract, counted memory. Negative controls (30, `.build/dolrecon/texture_mutations.json`): all fail a test
(the destructor-references record was added for one).

`rwcore_image_tests` (oracle, 100 sessions, 19,591 operations, 164 million original instructions, about 45 s): the
device's pixel-to-RGB, image-from-raster, raster-from-image and raster-format functions, RwFexist (the GameCube's disc
file system 0x8047B6CC, stubbed) and the format read callbacks scripted alike on both sides, an image plugin; each
session opens the module (every fifth with one of its four allocations failing), registers up to four formats (one
without a read callback, one whose extension holds a '/'), then: images of depths 0, 1, 4, 8, 16, 24, 32 and sizes
up to 40 x 40 created, destroyed, given pixels (the same random bytes on both sides; allocations failing), freed;
masks made and applied (palettised and 32-bit, sizes that differ in width or height only, invalid depths, the
conversion's first allocation failing); copies of every depth pair the original converts (the gamma flag set on
either side) and of refused pairs (the original's ImageConvertDepth 0x8048A1AC against the native RwImageCopy: its
RwImageCopy would write through NULL); gamma tables for 1, 2.2, 0, -0, negative, huge, tiny, denormal, infinity,
NaN, random and four gammas where pow's result kept in double would change a byte; corrections; search paths
(several directories, empty elements, absolute names, past the 256-byte buffers, reallocation failing);
RwImageFindFileType and RwImageReadMaskedImage (1,259 extensions found, 716 reads, 204 images read, 29 masked), the
raster exchange, RwRGBASetFromPixel, RwRasterGetOffset. After every operation: every image's fields (the stride once
set), pixel and palette bytes, both gamma tables and the gamma, the path, the buffer sizes, every result, every
callback in order (RwFexist's and the read callbacks' full names included), the module's memory requests (size and
hint of pixels, path and scratch; the free lists' blocks differ in size by design and are not compared) and the held
error. `rwcore_image_native_tests` (2,892 checks): the module's slot in the engine, the device's gamma at
RwEngineStart, each allocation of the open failing (undone, not counted, no leak), the gamma tables against double
precision (2,000 gammas 0.1 .. 5.1: every entry within one step; gamma 1 the identity), strides and palette placement
for every depth, MakeMask / ApplyMask / Copy / GammaCorrect against their per-pixel meaning, every native contract
above, the search on a fake file system (order of the names tried) and RwFexist's fopen fallback on a real file,
counted memory. Negative controls (54, `.build/dolrecon/image_mutations.json`): 52 fail a test - 46 the oracle, 36 the
native test (the oracle misses the native contracts and leaks, the native test misses the scripted device and most
search orders: each covers the other); 5 oracle survivors of the first run led to the width-only / height-only sizes, the destination's gamma flag,
the decisive gammas, names with a leading '/' and the '/' extension. Not killed: dividing i / 255 in double
(equivalent: the same float for all 255 entries, checked exhaustively), fusing 0.5 + 255 x (not shown equivalent;
no byte changes in a search of 4 million gammas x 255 entries). pow's own controls (`pow_mutations.json`, against
`native_msl_tests`): see docs/NATIVE_MSL_MATH.md.

`rwcore_resources_tests` (oracle, 116,501 operations, 13.6 million original instructions, about 2 s): the heap -
600 heaps at start offsets 0..31 (where both layouts agree, see the contracts) and sizes 0 .. 20 KB (93 too small),
25,674 allocations (21,315 split, 13,493 failed) and 22,507 frees, the whole heap compared after each call (every
block's offset, size, flags, previous block and heap pointer, the search start); then 320 arena sessions (arena
sizes 0, too small, 96 .. 16 KB; the arena's allocation failing now and then: 46 failed opens) with 32,804 entries
(1,814 failed with the arena empty; evictions from the free list 2,428 times and from the used list 6,249 times),
5,927 frees, 1,347 entries outside the arena freed with RwFree, 6,102 uses (the release macro, the test's own on the
guest side), 3,986 purges, 1,699 emptyings, closes. After every operation: every result, the destroy callbacks in
order, every owner pointer, both lists in both directions and which is the free one, maxSize / currentSize /
reusageSize, every entry's fields and its place in the heap, the heap, the arena's memory requests and frees, the
held error and numInstances. Entry sizes are 9..16 modulo 32 (the domain where both entry headers take the same
block). `rwcore_resources_native_tests` (95,949 checks): heaps at every offset 0..31 with sizes around the minimum,
filled and emptied; heaps past 4 GiB and allocations whose rounding would wrap refused without writing; the engine's
life (the arena at RwEngineStart with its hint, every entry's callback at RwEngineStop, each allocation of the start
failing, arenas too small); the purge in RwCameraShowRaster; every native contract above; 60 random sessions at any
entry size (20,557 entries) checking the heap's tiling, no two free blocks side by side, nothing free before the search
start, owner pointers, currentSize, data integrity, and that an allocation fails only when no block holds it with the
arena empty (the random sessions' counts change with the allocator's alignment: 20,648 entries under
AddressSanitizer, where both tests and rwcore_camera_native_tests also pass). Negative controls (49,
`.build/dolrecon/resources_mutations.json`): 46 fail a test - 43 the oracle, 37 the
native test. Not killed, equivalent: scanning from the first block instead of firstFree (everything before firstFree
is used, an invariant the native test checks after every operation), and two mutants of the split's "the rest takes in
the free block after it" branch, which never runs: no two free blocks are ever side by side (free merges both
neighbours, a split's rest merges with a free successor), so the block after a free block is never free - the same
dead branch is in the original.

`rwcore_resample_tests` (oracle, 3,987 operations, 541 million original instructions, about 67 s): 2,487
RwImageResample calls on images built alike on both sides (908 enlarging, 1,448 reducing, 5 with a step of 1 - every
span 0 wide, the original's NaNs - 126 empty destinations, 744 with padded strides; sources of 1..40 pixels a side,
now and then 64..700 in one direction), every destination byte and the flags compared; 1,221 RwImageCreateResample
calls through the guest's image module (420 converted from 4 / 8 bits, 411 gamma-corrected, 279 with a pixel
allocation failing), comparing NULL or not, the new image's fields and pixels, the pixel requests, the frees and the
held error. Not generated, the deliberate differences: 13 size pairs whose float step reaches past the source (the
original reads after the buffer; predicted and skipped), steps below 1, other depths, sizes below 1, sources over
32766. `rwcore_resample_native_tests` (3,994 checks): 1,093,596 bytes within one step of the exact box filter (double,
the same 16.16 boxes; 40 resamples reaching past the source), identities (equal sizes copy, flat stays flat, halving
gives the 2 x 2 mean), the contracts above, the read past the source at the largest weight (3942 rows into 3921: the
last row unchanged by row 0 and by the memory after the pixels), and RwImageCreateResample with each allocation
failing in turn - 300 with the image free list, 600 without free lists (then the images' own allocations fail too) -
leaking nothing. Negative controls (34, `.build/dolrecon/resample_mutations.json`): 32 fail a test - 23 the oracle, 19
the native test ("step 0 allowed" by a hang: its step-0 case loops 2^32 times a pixel, stopped by the time limit). Not
killed, equivalent: the byte conversion unfused (fctiwz(255 x c + 0.5) equals the fused form for every float in
[-0.001, 1.001] - checked over all 2^32 bit patterns - and NaN; an average of bytes / 255 cannot leave that range),
and the step as 65536 x src / dst (the same float as 65536 x (src / dst): a power-of-two scale commutes with rounding).

`rwcore_palquant_tests` (oracle, 1,024 operations, 2.18 billion original instructions, about 265 s): 240 quantisers,
500 images added (300 of depth 32 - 33 random - 90 of depth 8, 110 of depth 4; 235 with noise inside or across cells;
150 of other weights, 31 of weight 0, 7 negative), resolved for maxcols 1 .. 32, 64 / 100 and in three cases 255 /
256 (62 palettes from the leaves, 217 cut into boxes, 5 for maxcols <= 0; 23 resolved twice and 21 added to after
resolving, the boxes cleared first on both sides), every image matched (100 packed, dstPacked TRUE / FALSE / 2), then
Term. After every call: the whole tree (265,367 nodes compared: which exist, every leaf's - after resolving every
node's - weight, colour sums, variance and index, floats by bits with NaN equal to NaN), the 256 boxes, variances and
box statistics, the palette with what lies past it, the indices with the padding, the result, and the node
allocations and frees with their hint. The free list is stubbed on the guest side (rwcore_memory_tests proves it);
the native side runs without free lists. `rwcore_palquant_native_tests` (224,337 checks): 300 images quantised for
maxcols 1..256 - every palette entry within one step of the mean colour of the pixels given its index, one entry a
cell when they fit, pixels of a cell sharing an index - exact colours for one colour a cell, the packed layout, and
every contract above (Init failing at its list and at its root, 13 images with nodes failing, maxcols 257 / 400 /
1000, weight 0, a second resolve stopping short), nothing leaked.

`rpworld_material_tests` (oracle, 14,966 operations, 64 million original instructions): a material plugin, a pool of
named textures in the current dictionary and a read callback that loads new ones now and then (333 loads; it records
the mipmapping switches it sees); materials created, destroyed, given textures; two material lists (2,085 appends
past the 20-entry growth, finds, gets, emptying); 3,720 materials read from streams (every struct length to 28 bytes,
bad versions, 1,903 textured: every filter, V addressing 0 or not, the automatic-mip bit, names found in any letter
case, missing, masks); texture chunks read alone; 1,061 string chunks into a caller's buffer or a new one, behind
other chunks. After every operation every material, list and texture (references, filter word, dictionary), the
mipmapping switches, callbacks, the held error. `rpworld_material_native_tests` (58 checks): every native contract
above, list growth and release, counted memory. Negative controls (28, `.build/dolrecon/material_mutations.json`): 26
fail a test (four of them only after the loading read callback was added: the switches matter only to a texture that
is loaded); 2 are equivalent (the version checks RwStreamFindChunk makes first).

`rpworld_world_tests` (oracle, 5,999 operations, 84 million original instructions, 23 s): the original
RpWorldStreamRead and the native one read the same random platform-independent world streams of the converter's shape
(tools/stage_convert: a 0x40-byte world struct or 0x20 .. 0x3F of it, material lists with read, shared and textured
materials, plane sectors with full and short structs, overlapping or not, BSP chains to 60 levels, atomic sectors
whose struct chunk holds the 0x2C header and the vertex / normal / prelit / 0 .. 8 UV sets / triangle arrays - the
pre-3.6.0.2 triangle order included - and an extension of the 0x50E mesh chunk, a sector plugin chunk and unknown
chunks) and malformed ones (versions outside the window in any chunk, truncation, container chunks of the wrong type, a
positive colSectorSize, material counts past the materials, mesh counts below the meshes written, totals above the
tree's, failing plugin reads, failing world-block / mesh-header / material-array allocations): 1,198 worlds read,
720 reads failed, every kind required to fail. Worlds stay alive (up to 4) while lights are created, framed, moved,
given radii, added and removed (positional ones tied at every frame sync: WorldLightSync), sectors walked with a
stopping callback and looked up (RpWorldSectorGetWorld, RpLightGetWorld), worlds destroyed. After every operation every
world field by field (92,755 sectors, 73,299 planes to depth 58, 92,755 mesh headers and 2 million indices compared),
materials, texture references, light lists and ties in order (4,165), plugin constructor / destructor calls in order,
the held error. The guest has the world module without the GameCube pipelines (WorldOpen's other steps by hand, the
needed steps of _rpWorldObjRegisterExtensions as their originals; each listed with its evidence address in the test's
header). Allowed and counted: 689 original plugin destructor calls on data never constructed (failed reads; the native
runs none). `rpworld_world_native_tests` (348,981 checks, under 1 s): 30 converted-shape worlds read back exactly
against the bytes written (arrays, triangles in both orders, mesh indices narrowed, materials one reference per entry,
planes, boxes from the planes, alignment, the walk and look-ups); every "Native contracts" case for worlds as a clean
failure (no leak, every plugin destructor matching a constructor); every truncation (1,464) of a world and every
allocation of its read failing in turn; a 10,000-level BSP read, lit and destroyed; ties against the definition (a
positional light reaches a sector when its radius box overlaps the sector's plane-bounded region strictly) over 2,000
lights; the light contracts; counted memory.
Found and fixed by the oracle (the native differed from the original): _rpMaterialListStreamRead allocates exactly
`count` slots at once (0x8045CA20 .. 0x8045CAB0: space = count; the native grew in steps of 20); RpWorldDestroy frees
every sector's mesh, then the material list, then the sectors' plugin data, then the world's (0x804610A0 / 0x804610D8 /
0x804610F4 / 0x80461104: sector destructors see no mesh; the native destroyed sectors with their meshes before the
materials); a failed tree read unwinds innermost plane first, each destroying only its completed left subtree
(0x80456700 .. 0x80456714; the native destroyed the whole tree in pre-order). Excluded from the inputs, with the reason
in the test's header: a texture's plugin data failing (the texture contract above), cuts inside 32-bit words of the
material list struct or a mesh chunk (a short RwStreamReadInt32 converts the whole word, and the original reads those
into its stack or a fresh block: the rest is whatever memory held). Negative controls (43,
`.build/dolrecon/world_mutations.json`): 42 fail a test (the oracle kills 33, the native test 33, "tie not checked"
only by crashing it; two survived both until the native test gained a window base of 0x10000 and a mesh
header counting 0x10000 meshes with no records, cases where only the 16-bit checks decide); 1 is equivalent: a short
prelit read accepted (a short read happens only at the end of a stream, after which the sector's plugin chunks cannot
be read, so the read fails either way).

Camera phase verification (2026-11-02): all 16 rwcore tests pass in Release (44 s) and under AddressSanitizer
(`.build/asan`, 456 s); the digest reproduces under MSVC, MSVC /arch:AVX2, MSVC + ASan and clang-cl /arch:AVX2
(-ffp-contract=off).

Image phase verification (2026-11-01): all 20 rwcore tests, the 7 rpworld tests and native_msl_tests pass in Release
(`.build/tests`, 27 tests, 203 s with two jobs); the 20 rwcore tests under AddressSanitizer (`.build/asan`, 356 s);
the new digest 0xACC0995AB18D8A3A reproduces under MSVC Release, MSVC + ASan, MSVC /arch:AVX2 and clang-cl 21.1.8
/arch:AVX2 -ffp-contract=off (`.build/xcomp/digest.cmd`); clang-cl with -ffp-contract=fast gives 0xA87C59246653813A.

`rwcore_camera_bench` (not a test; medians of 7 runs, three runs, this machine, Release): 100,000 camera syncs (10
rounds of 10,000 cameras whose frames sit under 100 rotated roots; the frames' LTM syncs included) 6.5-7.2 ms = 65-72
ns per camera; 1,000,000 sphere tests 6.5-7.5 ms = 6.5-7.5 ns per test; 1,000,000 near / far changes 2.4-2.6 ms. There
is no earlier native version to compare: this is the baseline. Nothing was changed for speed - a stage renders with a
few cameras per frame (well under a microsecond), the cost does not depend on the resolution (no per-pixel work: 5K
and 32:10 cost the same as 1080p here), and the obvious faster forms (several planes per SIMD instruction with
separate multiplies, double-precision tests) would change result bits the oracle proves - not tried; the cost to watch is the per-object frustum tests of world sectors and atomics, which
come with rpworld and will be measured there.

`rwcore_list_tests` (30,000 random operations on up to 13 lists, 0.7 million original instructions): after every
operation each list's counts, entry size and entry bytes, every returned entry as an index, the held error, and every
RwMalloc / RwRealloc / RwFree (size, hint), with failed allocations and reallocations injected. Negative controls (each
fails): growth by a half instead of a quarter, _rwSListGetNewEntries growing only on overflow (`>` for `>=`),
_rwSListToArray handing over the storage of an empty list, the entries allocated without rwMEMHINTFLAG_RESIZEABLE.

`rwcore_vector_tests` (532,198 checks, 22 million original instructions, about 4 s) compares both 4096-entry tables
word for word, 100,000 inputs each of _rwSqrt / _rwInvSqrt / RwV2dLength / RwV3dLength / RwV3dNormalize (in place
and separate, zero vectors with their E_RW_ZEROLENGTH), 20,000 transforms (in place, overlapping one
point ahead and behind, separate), 4,000 error set / get steps, every rwmalloc request (size, hint), the free order, the
module counters, and both allocation-failure paths of _rwVectorOpen. Negative controls (each fails the test): the table
exponent constants swapped, the lookup without its +0x800 rounding, RwV3dTransformPoints storing before reading the
next point, RwErrorSet overwriting the held error, the squared length fused, RwV3dNormalize dividing by the length,
a zero length without its error. Measured on the way: under the interpreter's frsqrte model (exactly
1 / sqrt) the tables' two Newton steps change no entry, so the oracle cannot tell them apart; they are kept because the
original runs them. The tables are therefore proven against an exact estimate: the GameCube's hardware estimate is
coarser before the two Newton steps, which can only differ in a table entry's last bit, if at all; the native's
tables are the mathematically exact ones (measured precision below).

`rwcore_lifecycle_tests` (native, 267 checks): 200 full Init -> Open -> Start -> Stop -> Close -> Term cycles with and
without free lists, every core module answering in between; every allocation of start-up failing in turn (each must
unwind, leave no allocation behind - a counting allocator - and let a later start-up succeed); misuse of the order;
an application plugin (offset, zeroed globals, init after the core modules, term before them, a failing init closing
the opened modules again, no registration after RwEngineOpen); each device request failing; the device defaults; an
error raised before the error module opens surviving RwEngineStart. Negative controls (each fails): close keeping
the instance, term keeping memory open, a failed registration keeping the registry, the vector table leak back, a
failed open keeping the instance, stop closing in registration order, the instance not zeroed, a pending error
dropped (the first run let three of these pass; the test now checks memory is closed, the stop order and the
carried error).

`rwcore_math_tests` (native, 280,003 checks): every matrix / vector function against double-precision references and
properties - multiply, scalar multiply, determinant, inverse * matrix = identity (general and orthonormal),
ortho-normalize (orthonormal, right-handed, near its source), rotation = Rodrigues' formula (right-handed, bound by the
11-bit axis normalization), the combine orders (pre = T * M, post = M * T) of rotate / scale / translate / transform,
point / vector transforms, lengths, normalize; in place = separate, bit for bit, wherever RenderWare allows in place
(and for Invert / Multiply natively); the square-root tables over every exponent: measured max relative error
2^-13 (1.22e-4) against the 2^-12 design. Negative controls (each fails): invert without its source copy, multiply
storing as it goes, a left-hand rotation, a table one bit coarser. Found: the original's in-place RwMatrixInvert is
wrong (see "Native contracts").

`rwcore_limits_tests` (native, 21 checks): skips of 2^31 .. 2^32 - 1 bytes reach the file layer as positive 64-bit
offsets; a real 6 GiB sparse file (no disk space used) is skipped past 4 GiB with RwStreamSkip, RwFtell reports
4.5 GiB, RwStreamFindChunk / RwStreamReadInt32 read a chunk there; the free-list parameter checks; RwSList and plugin
offsets at RwInt32's end. Negative controls (each fails): the skip through a 32-bit value, a 32-bit host tell, a 32-bit
host seek, no list overflow guard, no plugin offset guard, no free-list parameter check.

`rwcore_fp_digest_tests` (native): every floating-point function over 20,000 fixed inputs, 400 frames in random
hierarchies (every LTM) and 800 cameras on them (view matrices, frusta, boxes, Z scale / shift, sphere tests), the
image module's gamma tables for 400 gammas (since 2026-11-01), all result bits hashed (`--print` gives the digest
after the math, the frames, the cameras and all); the expected digest (0xACC0995AB18D8A3A; through the cameras still
0xD7A9AC3F7B638783, the value quoted below) is the oracle-proven MSVC build's. Reproduced by MSVC Release, MSVC + AddressSanitizer, MSVC
/arch:AVX2 and clang-cl 22 (/arch:AVX2, -ffp-contract=off); clang-cl /arch:AVX2 with -ffp-contract=on or fast gives
other digests - the measurement behind the compile flags above. Found with the cameras: the sign of a generated NaN
can differ between compilers (likely a negated FMA folded into one instruction, identical for every finite value;
the mechanism was not traced), so NaNs are hashed as one pattern and a closest-corner byte (a normal's sign bit) not
at all where that normal is NaN - with raw bytes clang-cl and MSVC disagreed on the camera part only, and a word-by-
word dump of 200 ordinary cameras was identical under both.

## Review 2026-11-01

A full read of every implementation (not the headers) against the originals and the RW 3.7 reference, then tests for
what was found. Confirmed and fixed: RwStreamSkip on files cast its offset to a 32-bit `long` (2 GiB and more seeked
backwards) and RwFtell truncated to RwInt32; the RwInt32 wrap-arounds of RwSList growth and plugin offsets; free lists
accepting parameters that divide by zero later; the vector open leak; errors written over curCamera before the error
module opens; the open-engine counter documented as "started" (RwEngineClose decrements it: plugins register between
RwEngineInit and RwEngineOpen, as RenderWare documents); the false "dst may be src" contract of RwMatrixInvert (wrong
in the original, now correct natively); rwcore had no -ffp-contract=off for GCC / Clang (bit-identity measured to
break without it); babinary's writer addresses were assigned wrongly in the header (now Real 0x8047E3A0, Int32
0x8047E4A8, Int16 0x8047E5B0, by link order and the 16-bit swap). Added: the writers, batkbin, the engine's life with
a host device, the string module. Verified: all 12 rwcore tests pass in Release and under AddressSanitizer (MSVC,
.build/asan; an injected one-byte heap overflow was reported, so the sanitizer is live); the digest reproduces across
compilers as stated. Not covered: LeakSanitizer does not exist for MSVC (the counting allocator of the lifecycle test
checks the engine's allocations instead); the big-endian paths (swapWords, the writers' chunked copies) cannot run
on this host; no threading (RenderWare's core is single-threaded; nothing here is thread-safe); memory streams past
4 GiB are size_t arithmetic, not exercised (a 4 GiB allocation in a test was not attempted).

## The oracle harness

`runtime/tests/rwcore_oracle.h`: the game's DOL in the reference interpreter with the RenderWare environment the
originals need - r2 / r13 (PAL 0x805FA780 / 0x805EC500), RwEngineInstance pointing at a guest engine block, and the
engine's rwmalloc / rwfree as host stubs over a guest heap that record every call and can fail a chosen one.
Functions taking pointers to structures compare through the guest layout (32-bit) on one side and the native layout
(64-bit) on the other.

## Scope and plan

The PC libraries' `rwcore` (44 modules) and `rpworld` (14 modules) hold 938 functions; the game links a subset, and
246 of its functions are aligned with PC names so far (`.build/dolrecon/rw/rw_names.json`). Order of work - each
module's game functions identified from their code and the PC source order, then ported and proven:

1. (bacamera, babincam, babbox: done, see Modules) balight (RpLightCreate 0x8045BD84,
   colour, cone angle, _rpLightOpen / Close), the rpworld plugin (RpWorldPluginAttach 0x8046185C, WorldOpen / Close,
   world-object extensions, WorldLightSync, RpWorldAddLight 0x804635C8)
2. baraster / batextur / baimage (the CPU side; texture formats converted from the GameCube's)
3. rpworld: geometry, material, matlist, mesh, light, clump / atomic, world / sectors (bageomet, bamateri, bamatlst,
   bamesh, balight, baclump, baworld, basector, baworobj)

The GameCube driver functions are not ported; the renderer behind RenderWare's device interface is the project's
D3D12 / Vulkan code. Reference for it: `H:\The Godfather The Game\_re\out` (user-provided decompilation of RenderWare
3.7's own Direct3D 9 driver from The Godfather PC: device, rasters, instancing, pipelines, lights) - facts only.

## Build and test

`rwcore` is a static library in `CMakeLists.txt`. All tests: build the `rwcore_*_tests` targets in `.build/tests`
(Release), then `ctest --test-dir .build/tests -C Release -R ^rwcore_` (26 tests; rwcore_palquant_tests alone runs about 265 s, the rest about 150 s with two jobs; the oracles take
`<main.dol>`, labels reference / native-contract; the native tests are native-contract only).

AddressSanitizer: `.build/asan` (configured with `-DCMAKE_CXX_FLAGS="/fsanitize=address /EHsc /DWIN32 /D_WINDOWS"`,
`-DSHADOW_DEPLOY_DIR=` so it never deploys; from Git Bash set `MSYS_NO_PATHCONV=1`, or the flag becomes a path), the
same targets; the ASan runtime DLL is in the MSVC toolset's `bin/Hostx64/x64`, which must be on PATH to run them.

Other compilers: compile `rwcore/src/*.cpp`, `runtime/lift/math/msl.cpp` and `runtime/tests/rwcore_fp_digest_tests.cpp`
directly (includes `rwcore/include`, `runtime/lift`, `runtime/include`) and run it; it must report the expected digest.
