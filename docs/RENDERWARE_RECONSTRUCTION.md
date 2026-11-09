# RenderWare reconstruction roadmap (2026-11-01)

User directive, 2026-11-01: "Do not forge game specific engine files. first you reconstruct a modern version of the
renderware engine and then build the game on it by mass reconstruction".

Project reminder from the user (2026-11-01), the governing principle: RenderWare reconstructed as a fully native,
modern, 64-bit PC engine on DirectX 12 - not a GameCube emulator, not a preservation of the original hardware
architecture, not wrapped legacy middleware. Reverse engineer the original (functions, structures, algorithms,
dependencies, behaviour); reimplement on a modern 64-bit architecture (native memory management, multithreading where
appropriate, Direct3D 12); replace GameCube hardware abstractions, PowerPC execution requirements and obsolete memory
models; keep the original as a BEHAVIOURAL reference, not an architectural constraint - internal improvements are
welcome; optimise for modern hardware; no emulation layers or PowerPC compatibility in the engine; no component is
called reconstructed until meaningful tests demonstrate its required behaviour. Final destination: Shadow the Hedgehog
running natively on Windows x64 on this engine, without the GameCube execution environment.

So the order is: the whole RenderWare 3.7 engine the game uses, rebuilt as modern native x64 code (rwcore/, rpworld/,
then the toolkits and plugins), every module proven against the original functions of PAL main.dol in the reference
interpreter (rwcore/README.md says how); then the game reconstructed on top of it. Asset files are real RenderWare
streams; no invented platforms, formats or game-specific engine files.

## What the library is in PAL main.dol

Measured from .build/dolrecon/pal-v30/config/symbols.txt (function sizes) against the addresses rwcore/README.md
quotes as ported. 911 functions, 307,484 bytes between 0x80455000 and 0x804AB000; 238 functions / 50,540 bytes
(16 %) ported and proven on 2026-11-01. Recounted 2026-11-02 the same way over every function symbol in the range:
1,042 functions / 351,304 bytes, 333 / 89,732 bytes (26 %) ported (palquant's 21 included); 158 functions / 66,352
bytes are the GameCube driver and pipelines (0x80465000, 0x80491000..0x8049D000), replaced, not ported.

| block (16 KB) | contents (symbol names) | ported |
|---|---|---|
| 0x80455000 | rpworld objects: clump, UV animation plugin | 0 % |
| 0x80459000 | geometry, light, atomic, material, material list | 32 % |
| 0x8045D000 | world plugin attach, mesh build, tristripper, world sector | 0 % |
| 0x80461000 | world, world pipelines, world objects | 3 % |
| 0x80465000 | GameCube world-sector pipeline nodes, GameCube native geometry streams | 0 % (platform: replace) |
| 0x80469000 / 0x8046D000 | unnamed (30 functions) | 0 % - identify first |
| 0x80471000 / 0x80475000 | Rt2d (fonts, device, CTM), RtAnim | 0 % |
| 0x80479000 | RtCharset, RtAnim interpolator | 3 % |
| 0x8047D000 | lists, plugin registry, free lists, errors | 99 % |
| 0x80481000 | plugin registry, resources arena, strings | 70 % |
| 0x80485000 | camera, frame, engine, image open | 82 % |
| 0x80489000 | raster, image, gamma, image formats, texture dictionaries | 22 % |
| 0x8048D000 | textures, dictionaries, mipmap naming / generation | 23 % |
| 0x80491000 / 0x80495000 / 0x80499000 | the GameCube driver (Gcn raster, image, Im2D, camera, native texture) | 0 % (platform: replace) |
| 0x8049D000 | pipeline nodes (ImmInstance, SubmitNoLight), node definitions | 0 % |
| 0x804A1000 / 0x804A5000 | RpMatFX | 0 % |
| 0x804A9000 | RpMultiTexture / MTEffect | 0 % |

## Order

1. **rwcore, platform-independent**: images (baimage / baimras 0x80488964..0x8048AB90, 24 functions / 8,836 bytes
   with _rwpathisabsolute: DONE 2026-11-01, rwcore/README.md), resources arena (baresour 0x8048158C..0x80481CBC +
   resmem 0x804839D4..0x80483C10 and RwRasterShowRaster's purge: DONE 2026-11-02, rwcore/README.md), resampling
   (baresamp 0x8048B1C8..0x8048BE30, 4 functions: DONE 2026-11-02), palette quantisation (palquant
   0x8048E8AC..0x8049148C, 21 functions: oracle and native tests pass 2026-11-02, rwcore/README.md),
   texture remainder (batextur's default read path 0x8048C6FC..0x8048D96C - TextureDefaultMipmapName,
   PalettizeImage, PalettizeMipmaps, TextureImageReadAndSize, TextureDefaultNormalRead / MipmapRead / Read,
   TextureRasterDefaultBuildMipmaps - and RwTextureGenerateMipmapName 0x8048E13C, 0x8048E310: read, next to port),
   the pipeline core (RxPipeline, nodes, clusters) and
   immediate mode (Im2D / Im3D) where the GameCube build has them.
2. **the PC driver, modern backend**: the device, rasters, native textures and immediate mode of the RenderWare 3.7 PC
   driver on Direct3D 12. Texture dictionaries read the real RW PC native texture format. This replaces the GameCube
   driver and retires the stand-in host raster plugin and the invented 'HOST' texture platform in
   rwcore/src/raster.cpp and texture.cpp.
   Evidence and oracle: the ORIGINAL RenderWare 3.7.0.2 libraries are on disk - RW37SDK\Library D3D9 lib Files\release
   (66 x86 COFF libs: rwcore, rpworld, rpmatfx, rpskin, rphanim, rpuvanim, rtanim, rt2d, rtcharse, rtdict, ...), the
   same for NULL / OpenGL / D3D8 (RW37SDK\_mined\buildability_notes.md: linkable from VS2022 x86 with
   legacy_stdio_definitions). A 32-bit harness linking them is the PC oracle, as the reference interpreter is for the
   GameCube code: native texture reads, raster formats, the driver's standard functions compared against the original
   PC driver. The decompiled D3D9 driver (H:\The Godfather The Game\_re, clmpview_d3d9.exe) is the reading aid.
   Proven feasible 2026-11-01: tools/rw_pc_oracle/probe.c, built 32-bit by tools/rw_pc_oracle/build_probe.cmd (VS 18
   x86, /MT, original rpworld.lib + rwcore.lib + rtfsyst.lib, d3d9 / legacy_stdio_definitions / advapi32 / user32 /
   gdi32 / winmm / uuid) into .build/rw_pc_oracle/rw_pc_probe.exe: RwEngineInit / RpWorldPluginAttach / RwEngineOpen on
   a hidden window / RwEngineStart ran; library version 0x37002, sub system 0 = the RTX 4080, 61 video modes; a 64x32
   rwRASTERFORMAT8888 texture raster created (depth 32, format 0x0500) and locked (stride 256); stop / close / term.

   The PC native texture format is specified from the original driver's code (d3d9texdic.obj, with symbols) in
   docs/renderware-pc-driver/NATIVE_TEXTURE_FORMAT.md, checked byte for byte against the corpus (148 textures, 509
   levels, 0 mismatches; .build/rw_pc_oracle/txd_check.py). Design consequences for the Direct3D 12 reader: each
   level's stored bytes are the D3D9 surface pitch x rows, so the reader derives the row pitch from size / rows and
   repacks; DXT levels whose height >= 4 is not a multiple of 4 are stored one block row short; the original reads
   only what the D3D9 device supports (P8, DXT, auto-mipmap caps change what is written and read) - the native reader
   accepts every format the stream can hold, matching the original wherever the original succeeds, and documents
   the original's device-dependent failures (and its unreadable uncompressed cube textures) as deliberate differences.

   Boundary: the engine itself is native 64-bit C++ written here. The 32-bit original libraries and the GameCube code
   in the interpreter exist only as test oracles; no console code (the GameCube driver, GameCube pipelines, native
   GameCube geometry / sector formats) is ported, and no original library is linked into the engine.
3. **rpworld**: geometry, atomic, clump and their streams; world sectors, mesh build / tristripper; the world and
   atomic pipelines instanced on the PC driver.
4. **plugins and toolkits the game links**: RpMatFX, RpMultiTexture, RpUVAnim, RtAnim, Rt2d (+ fonts), RtCharset,
   RtDict, and the unnamed 0x80469000 block once identified; hierarchical animation and skinning wherever they are.
5. **then the game**, reconstructed in bulk on that engine.

## Not engine work (stopped)

tools/stage_convert (custom .light / .worlds files and HOST textures), tools/native_stage_clip (trace replay) and the
SHADOW_CAMERA_TRACE observer in runtime/src/gekko/system.cpp were a game-specific shortcut to a demo clip; they are not
built on any further. The game-camera disassembly in docs/camera-re-20261102 is kept as reconstruction input for step 5.
