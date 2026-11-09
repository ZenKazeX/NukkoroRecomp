# Vulkan renderer (Android port) - design and staged plan

Started 2026-10-26. Code: `runtime/lift/render_vk/` (its own CMake project, build tree `.build/vk`). Governing
documents: docs/NORTH_STAR_COMPACT.md, docs/DIRECTIVE_2026-10-26_NATIVE_ENGINE.md. The D3D12 renderer
(`runtime/lift/render/`, "R2") stays the desktop renderer and the reference for every behaviour described here.

## 0. Status (update on every step)

| Stage | What | State |
|---|---|---|
| V0 | Toolchain, shader path proven (all 21 HLSL entry points -> valid SPIR-V, unchanged) | done 2026-10-26 |
| V1 | Device, swapchain (Win32 + Android code paths), frame loop, bindless set, screen pipelines, frame skeleton: per-view MSAA UNORM16 scene, over-views start, own resolve, compose, captures | done 2026-10-26 on Windows / RTX 4080 (section 10); Android: see V5 |
| V2 | Frame graph port (passes declare reads/writes; barriers derived; pass culling; pooled targets) | done 2026-10-27 (GPU timestamps per pass came with V6): `render_vk/frame_graph.*` (declarations, culling, read-before-write refusal, pooled images with in-frame aliasing, sync2 barriers per pass boundary, Load -> load ops, derived store ops); every pass of the renderer declared through it; checks PASS (1x / 2x / 4x, validation) with the earlier checks' pictures byte-identical, the Westopolis replay byte-identical to the pre-graph captures at 4x and 1x (section 10) |
| V3 | Resources (textures, meshes, handle tables, resource worker) + mesh / particle / immediate draws, canvas placement | done 2026-10-26 except the asynchronous resource worker (resources are made on the asking thread): textures, meshes, handles / scopes / deferred releases, the mesh base / environment / dual passes, particles, immediates (world + canvas, anchors, regions, scissors), light sets, fog, cutoff; draw checks PASS on Windows (4x, 1x, validation) and the Android emulator (section 10) |
| V4 | Glare chain, snapshots (partial resolves), histories, skinning / morph compute | done 2026-10-26: glare chain with the B3a fold, partial snapshots (G11 shared image), histories resolved straight into (B1), commit_history, invalidate_histories, skinning + delta morphs (CSSkin, skin cache with per-frame regions, pose rebasing); effect checks PASS on Windows (1x / 2x / 4x, validation) and the emulator (section 10) |
| V5 | Android app shell (GameActivity / NativeActivity), surface lifecycle, pre-rotation, frame pacing | in progress: NativeActivity (`render_vk/android/main.cpp`) drives `render_vk::Renderer` with FramePackets; builds x86_64 + arm64, packaged without Gradle; on the emulator: off-screen checks PASS, presenting PASS, lifecycle, pre-rotation, config changes without relaunch (section 10); arm64 never run (no device); frame pacing not started |
| V6 | Mobile optimisation: tile-friendly passes, transient attachments, compressed textures | started 2026-10-27: transient (tile-only) attachment images, merging adjacent passes into one rendering instance, GPU timestamps per rendering group; the view's depth kept on tile (scene + glare sources in one instance with local read, depth-free passes after the last depth test) - done, validated (checks + Westopolis replay byte-identical, section 10 "V6"); open: the resolve decision (3.9), snapshots inside an instance (local read of the colour), ASTC / ETC2, A2B10G10R10, push constants, every measurement on a phone |
| V7 | Integration into the game build behind a backend choice (hooks section 8) | not started |

Nothing of this is in the game's build. No claim here is about gameplay, Android devices or GPU correctness beyond what
section 10 records as run.

## 1. Principle

**User decision (2026-10-26): the Android port is Vulkan-based, for maximum optimisation.** No OpenGL ES path and no
portability layer (ANGLE, MoltenVK-style translation, a generic RHI) - the renderer uses Vulkan directly and is free to
use what makes mobile GPUs fast: load/store ops and transient lazily-allocated attachments, render-pass merging and
`VK_KHR_dynamic_rendering_local_read`, pre-rotated swapchains, ASTC textures, persisted pipeline caches. Every such
choice is still measured on a device before it is kept (directive section 9).

The FramePacket (`render/frame.h`) is the engine's own description of a frame: views, cameras, passes in the game's
authored order, draws with plain-data materials and recipes, handles the renderer issued. It contains no GX, no
RenderWare internals and no D3D12 object. A second backend therefore consumes the SAME packet; it is not a
translation layer between the console and a GPU API, and it does not wrap the D3D12 renderer. Test 2 of the north star
("could it have been written from scratch in 2026?") applies: this is how a 2026 engine supports D3D12 on Windows and
Vulkan on Android.

Rules carried over from the D3D12 renderer, unchanged:

* the packet's order is the picture's order: passes are never reordered (translucent and additive draws compose in the
  authored order);
* every view renders at its real share of the output's physical resolution (no 640 x 480 base, no multipliers);
* the scene target has 16 bits per channel (no 8-bit banding), multisampled, resolved by the renderer's OWN resolve in
  a fixed sample order (`PSResolve`, `precise`) so the same frame gives the same bits in every process;
* validation is behaviour equality + visual intent + byte-exact self-determinism - not pixel parity with D3D12.

One source for each piece of meaning: the HLSL in `render/shaders/` is compiled for both backends (section 3.7); the
camera, canvas and projection arithmetic must become one shared header (hook H2) so both backends compute identical
constants.

## 2. What the packet must be drawn with (from the D3D12 renderer, 2026-10-26)

Read from `render/renderer.cpp` `Renderer::record`, `frame_graph.*`, `skin_cache.*`, `shaders/*.hlsl`:

1. **Light sets** - one constant block each per frame (cbuffer `Lights`, b1), plus an empty one.
2. **Skinning / morph jobs** - one compute job per distinct (mesh, palette, morph weights); palettes rebased to their
   first posed bone's translation (float precision far from the origin), the draw puts the origin back through its
   world matrix composed in double. Output: the frame slot's region of the skin cache, read as vertex stream 0.
3. **Per view** (in packet order): a box of `content_aspect` centred in the output (whole pixels); the view's rectangle
   rounded inside it; scene colour (UNORM16, MSAA) + depth (D32) at that size.
   * a view with `over_views` starts from the earlier views' resolved pictures where it covers them (`Under`, point
     clamp), on the background elsewhere;
   * passes of the view in order: `Scene` (draws), `GlareSources` (draws onto a black RGBA8 target over the scene's
     depth, tested not written), `Glare` (capture -> blur x/y -> box -> merge -> additive composite), `Snapshot`
     (partial resolve of exactly the texels its readers sample, `snapshot_reads.h`; one texture per view reused),
     `SaveHistory` (resolve into the view's kept history; a capture that is also presented does not commit it);
   * the view's resolve (`PSResolve`) into its single-sampled picture.
4. **Compose** - every view's picture into its rectangle of the output (cleared to the background), point sampled.
5. **Present scale** (G10) - when the render size differs from the back buffer: 2 x 2 box at exactly 2:1, else a
   Catmull-Rom bicubic, fitted with bars.
6. **Capture** - optional copy of the output to a readback buffer.

Draw records: mesh submesh (base pass + optional environment / dual second pass, depth write off), particle batch
(44-byte per-instance stream, six vertices per particle), immediate geometry (24-byte vertices, lines / strips /
triangles, world or canvas space with an anchor and an optional canvas region, sampling a texture, a snapshot or the
history). Every skip is counted by reason.

Observation: `PassRecord::clear` is not read by the D3D12 renderer (the scene clears to the packet background on its
first pass). The Vulkan renderer does the same; whether the field should be removed or honoured is a question for the
D3D12 owner, not for this port.

## 3. Mapping to Vulkan

### 3.1 Instance, device, queues (`render_vk/device.*`)

* Vulkan 1.3 instance and device. Capability floor (checked, reported by name when missing): dynamicRendering,
  synchronization2, shaderDemoteToHelperInvocation (PSBase's alpha cutoff), timelineSemaphore, descriptorIndexing,
  runtimeDescriptorArray, descriptorBindingPartiallyBound, sampled-image and storage-buffer update-after-bind,
  updateUnusedWhilePending, sampled-image / storage-buffer array dynamic indexing.
* Optional, adapted to: scalarBlockLayout (CSSkin's 44-byte structured stride - without it skinning needs a 48-byte
  padded stream variant), samplerAnisotropy, shaderSignedZeroInfNanPreserveFloat32 (section 6).
* ONE queue family with graphics + compute takes every frame's work and (V3) the uploads. A dedicated transfer queue is
  an optimisation for desktop only; mobile GPUs have one universal queue in practice.
* One timeline semaphore is the only "is the GPU done" signal: each submission signals the next value; deferred
  release keeps an object until the value current at retirement is reached (the D3D12 `retire` / `GpuMark` model).
* Validation layer + debug-utils messenger on request (`DeviceOptions::validation`); messages are collected and the
  smoke test fails on any.

Why 1.3 rather than 1.1 + extensions: Android devices since Android 13/14 ship 1.3 drivers (the Android 15 Vulkan
profile requires 1.3); a 2005 game's port targets 2022+ phones. A 1.1 + extension fallback (VK_KHR_dynamic_rendering,
VK_KHR_synchronization2, VK_KHR_timeline_semaphore, VK_EXT_descriptor_indexing) is mechanical if a target device needs
it - UNKNOWN whether any does.

### 3.2 Swapchain (`render_vk/swapchain.*`)

* Surface from `NativeWindow`: Win32 HWND (desktop testing) or `ANativeWindow*` (Android).
* Format: B8G8R8A8_UNORM / R8G8B8A8_UNORM, sRGB-nonlinear colour space, written without sRGB conversion - the D3D12
  back buffer is R8G8B8A8_UNORM, so the picture's values are the same.
* Present modes: VSync = FIFO (always available; the Android compositor's mode); Immediate = IMMEDIATE, else MAILBOX.
* Semaphores: "acquired" per frame slot, "ready" per swapchain image (a per-slot one could be re-signalled while a
  present still holds it). Out-of-date / suboptimal -> recreate at the surface's size and skip the frame.
* Android: `preTransform = currentTransform`; the final pass (compose or present scale) will rotate (V5) so the
  compositor does not spend a full-screen rotation per frame. Surface lifecycle (APP_CMD_TERM_WINDOW / INIT_WINDOW)
  destroys and recreates the surface and swapchain, keeping every other resource.

### 3.3 Memory (`render_vk/resources.*`, VMA)

* VulkanMemoryAllocator 3.2.1, already vendored by plume (`thirdparty/plume/contrib/VulkanMemoryAllocator`), compiled
  once in `render_vk/vma.cpp`.
* Render targets: `AUTO_PREFER_DEVICE`; multisampled colour/depth that never leave the tile: `TRANSIENT_ATTACHMENT` +
  `GPU_LAZILY_ALLOCATED` (falls back to device memory where the GPU has no lazy memory - every desktop GPU).
* Upload rings: host-visible sequential-write, persistently mapped, one per frame slot; captures: host-visible random
  access (cached) + invalidate.
* Pooled render targets matched by what they ARE (size, format, samples, usage), kept 3600 unused frames, as
  `TexturePool` does (the D3D12 measurement that motivated 3600 was about the NVIDIA Windows driver's commit; on Android
  the right retention must be measured - section 9).

### 3.4 Descriptors (`render_vk/bindless.*`)

The D3D12 renderer is bindless with Shader Model 6.6 heap indexing (`ResourceDescriptorHeap[i]`,
`SamplerDescriptorHeap[i]`); draw constants carry plain integers. The Vulkan layout keeps those integers:

| Set / binding | Type | D3D12 counterpart |
|---|---|---|
| set 1, b0 | SAMPLED_IMAGE[capacity], partially bound, update-after-bind | ResourceDescriptorHeap in graphics shaders (Texture2D, Texture2DMS) |
| set 1, b1 | SAMPLER[27], immutable | the sampler table (`sampler_index`: filter x u x v) |
| set 1, b2 | STORAGE_BUFFER[capacity], partially bound, update-after-bind | ResourceDescriptorHeap in compute (skinning's structured buffers) |
| set 0, b0 | UNIFORM_BUFFER_DYNAMIC, 64 B | the 16 root constants (cbuffer Root, b0) |
| set 0, b1 | UNIFORM_BUFFER_DYNAMIC, 12 KiB | cbuffer Lights / Palette (b1, root CBV) |
| set 0, b2 | UNIFORM_BUFFER_DYNAMIC, 256 B | cbuffer Draw / Particles / Immediate (b2, root CBV) |

* One index space: an index names slot i of b0 and of b2; an image is written to b0, a buffer to b2. Persistent and
  per-frame transient ranges exactly as `BindlessHeap`.
* DXC maps the heaps with `-fvk-bind-resource-heap <binding> 1 -fvk-bind-sampler-heap 1 1`; the graphics compile uses
  binding 0, the compute compile binding 2. Every heap access of one shader is then of one descriptor type, so no
  aliasing of different types in one binding - that would need VK_EXT_mutable_descriptor_type (present on the RTX 4080
  here; on Android drivers UNKNOWN per device), which this layout avoids.
* Root constants are a 64-byte block in the frame's ring, bound by dynamic offset per draw. Push constants would save
  the ring write and the descriptor-set rebind; DXC does not turn a `cbuffer` into push constants without a
  `[[vk::push_constant]]` attribute in the HLSL, so that is a later, measured change (either an `#ifdef __spirv__`
  attribute in the shared HLSL or a SPIR-V storage-class rewrite at build time).
* An alternative to all of this exists: DXC's `-fspv-use-descriptor-heap` (VK_EXT_descriptor_heap). It is new; driver
  coverage on Android is UNKNOWN; not used.

### 3.5 Frame loop and synchronisation (`render_vk/frame_loop.*`)

* N frames in flight (2), each with a command pool + primary command buffer and an upload ring. `begin` waits on the
  timeline for the slot's previous submission.
* One submission per frame: waits "acquired" at colour output, signals the timeline value and "ready".
* Single submitting thread, as on D3D12 (the render thread). Secondary command buffers / parallel recording are not
  planned: the D3D12 measurements showed recording is not the bottleneck at this draw count.

### 3.6 Frame graph -> synchronization2 barriers (V2, `render_vk/frame_graph.*`)

`render/frame_graph.*` is API-specific (D3D12 states, CPU descriptor handles), so render_vk has its counterpart with
the same model, rules and code shape rather than a shared core (H3 stays optional): passes declare reads / writes with
`Access` and `Load` in the game's order; `compile` culls the passes nothing uses (a pass that writes an imported
resource or has a side effect is kept), refuses a read or an add before a graph texture's first write, gives every
graph texture a pooled image (a texture whose first kept pass comes after another's last takes over that image - the
D3D12 graph's rule), and plans the barriers; `execute` records each kept pass after its barriers. Vulkan specifics:

| Frame-graph Access | stage / access / layout |
|---|---|
| RenderTarget | COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_READ+WRITE / COLOR_ATTACHMENT_OPTIMAL |
| DepthWrite | EARLY+LATE_FRAGMENT_TESTS / DS_READ+WRITE / DEPTH_ATTACHMENT_OPTIMAL |
| DepthRead | EARLY+LATE_FRAGMENT_TESTS / DS_READ / DEPTH_READ_ONLY_OPTIMAL |
| ShaderRead | FRAGMENT_SHADER (+COMPUTE) / SHADER_SAMPLED_READ / SHADER_READ_ONLY_OPTIMAL |
| UnorderedAccess | COMPUTE_SHADER / SHADER_STORAGE_READ+WRITE / GENERAL |
| CopySource / CopyDest | COPY / TRANSFER_READ / TRANSFER_WRITE, TRANSFER_SRC / DST_OPTIMAL |
| ResolveSource / Dest | unused (the renderer resolves in a shader) |
| VertexRead / IndexRead | VERTEX_ATTRIBUTE_INPUT / VERTEX_ATTRIBUTE_READ, INDEX_INPUT / INDEX_READ (buffers) |

`Load::Clear / Discard / Keep` become the attachment load ops CLEAR / DONT_CARE / LOAD (and UNDEFINED as the old
layout for Clear and Discard) instead of separate clear commands - on a tiler that is the difference between a clear
that costs nothing and a full-screen memory read. The store op is derived: STORE when a later kept pass uses the texture
or the graph does not own it, DONT_CARE otherwise (the depth and the multisampled scene colour after their last pass are
never written out on a tiler), NONE for the read-only depth. Barriers are tracked per physical image (an aliased texture
starts from the use the one before left) and per imported buffer, from the use each was last given - across frames
through `Image::state` - and batched into one `vkCmdPipelineBarrier2` per pass boundary; a read after reads in the same
layout needs none. An image not used yet (new, or a swapchain image after its present) is waited on from ALL_COMMANDS,
which chains with the acquire semaphore. Imported: the output (swapchain image, left in PRESENT_SRC; or the capture,
read back by a side-effect pass), the views' histories, the skin cache (a buffer: compute writes, vertex reads).
Pooled usage is one set per kind (colour: attachment + sampled + copy source / destination; depth: attachment), so one
size and format pool and alias whatever passes use them. V6: a texture every kept pass uses only as an attachment gets
a transient kind instead (the attachment usage alone + TRANSIENT_ATTACHMENT, in lazily allocated memory where the GPU
has it: on a tiler it never reaches DRAM unless a pass stores it); the graph decides this per frame from the
declarations, and the pool keys on it. `use_image` (resources.h) remains only for the one-time upload command buffers.

### 3.7 Shaders: the D3D12 HLSL, compiled to SPIR-V by DXC (no duplicates)

Proven 2026-10-26 (section 10): every entry point the D3D12 build compiles (21: utility x6, mesh x4, particles x2,
immediate x2, glare x6 incl. ResolveGlare, skinning x1) compiles with the Vulkan SDK's DXC 1.9.0.5347
(`-spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout -fvk-bind-resource-heap ... -T *_6_6 -O3 -WX -HV 2021 -Gis`) and
passes `spirv-val --target-env vulkan1.3` (CSSkin with `--scalar-block-layout`). No shader source was changed.

* `-fvk-use-dx-layout`: constant buffers keep D3D packing, so the C++ blocks (`DrawConstants`, ...) are shared byte for
  byte (hook H2).
* Clip space: the shaders write D3D clip space (y up, z 0..1). Vulkan viewports are set with a negative height
  (core since 1.1), which maps it onto the same pixels; front-face winding then matches D3D's (`FrontCounterClockwise =
  FALSE` -> `VK_FRONT_FACE_CLOCKWISE`) - to be confirmed by the first mesh capture in V3 (STRUCTURAL MATCH until then).
  `-fvk-invert-y` is the alternative and is not used (it would change the shaders' output, not the viewport).
* `SV_Position` in pixel shaders (PSResolve, PSScale's `Load`) is the framebuffer's pixel centre in both APIs.
* `precise` -> `NoContraction`; `-Gis` -> `SignedZeroInfNanPreserve` execution mode (section 6).
* Build: `render_vk/CMakeLists.txt` compiles each entry to a header (`spv_<Entry>.h`, `kSpv<Entry>`); the D3D12 build is
  untouched. When integrated (H4), both compiles come from one entry list.

### 3.8 Passes: dynamic rendering

`vkCmdBeginRendering` per graph pass (no VkRenderPass / VkFramebuffer objects; pipelines name their formats via
`VkPipelineRenderingCreateInfo`). Pipelines are created on first use and cached by key (program, recipe blend / cull /
depth, colour format, samples, lines), as the D3D12 PSO cache; a `VkPipelineCache` persisted to disk on Android avoids
first-run hitches (V3; the D3D12 side plans build-time enumeration (R2c) - the same list feeds both).

Tile-based GPUs reward keeping a view's consecutive Scene / GlareSources / Scene passes in as few render-pass instances
as possible. Dynamic rendering expresses that by the graph merging adjacent passes with the same attachments (and
`VK_KHR_dynamic_rendering_local_read` where a pass reads the attachment it writes).

Done in V6 (2026-10-27): a pass declared `mergeable()` (the draw passes: view clear, scene, glare sources) records one
rendering instance through `PassContext::begin_rendering / end_rendering`; the graph continues the previous kept pass's
instance when both are mergeable, use the same attachments in the same layouts, the later one keeps their contents (no
Clear / Discard) and needs no barrier other than between attachment writes in an unchanged layout (rasterization order
covers those inside one instance; they are dropped). The instance's store ops are its last pass's. A first read of a
snapshot, a layout change (the glare sources' read-only depth), a clear or a buffer barrier ends the instance.
Westopolis merges only 6 passes in 91 frames that way: its scene passes alternate with glare sources, glare and
snapshot passes.

Done in V6 (2026-10-27, second part) - the view's depth kept on tile (`RendererOptions::keep_depth_on_tile`):
* Scene + glare sources in ONE instance where the device has `VK_KHR_dynamic_rendering_local_read` (core 1.4): a
  view's run of Scene and read GlareSources passes up to its next Glare / Snapshot / SaveHistory pass becomes one graph
  pass "scene + glare sources" with two colour attachments (0: scene colour, 1: glare sources) and the depth. Each
  pass's draws are recorded in order; before them `vkCmdSetRenderingAttachmentLocationsKHR` maps fragment output 0 to
  that pass's attachment and the other to none (`kSceneLocations` / `kSourcesLocations`); the pair pipelines carry the
  same mapping (`VkRenderingAttachmentLocationInfo`), both attachments the same blend state (no independentBlend
  needed - the unmapped one is never written). The shaders are unchanged. The sources test the depth in the
  instance's DEPTH_ATTACHMENT layout with depth writes off, as their pipelines always had. The structs and values of
  the extension are declared in `vk_common.h` (the NDK's headers predate it).
* Depth-free passes: a pass after the view's last pass whose draws test (or, for a scene pass, write) the depth does
  not take the depth (pipelines with no depth attachment: `DrawPipelineKey::depth_attachment`), unless it directly
  follows a scene pass that took it (then the two stay one instance). So the canvas pass after the glare no longer
  keeps the depth alive, and the depth's last use is the pair.
* Without local read (the emulator; `DeviceOptions::local_read = false`, `--no-local-read`) the depth-free passes
  still apply and the sources keep their own instance (the depth stored once for their test).

### 3.9 MSAA and the deterministic resolve

Kept exactly: the scene is multisampled (4x - guaranteed by Vulkan for colour attachment formats), resolved by
`PSResolve` (mean of the samples in a fixed order, `precise`), not by the driver. On desktop this matches D3D12. On a
tiler the shader resolve forces the multisampled image out to memory (4 x the bytes of the view, written once and read
once per resolve). The alternative - `resolveMode = AVERAGE` on the colour attachment, resolved on tile for free - is
the mobile-native way, but the driver's resolve was measured on D3D12 to differ in the last bit between processes
(R2_DRIVER 4.9), i.e. it may break self-determinism. Decision deferred to V6 with a measurement on a real device:
determinism of the hardware resolve run to run on that GPU, and the bandwidth cost of the shader resolve. Until then the
shader resolve is the only path (V1 implements it).

### 3.10 Glare chain

The six full-screen passes run unchanged (same shaders, same constants, RGBA8 levels of (96 W/H, 128) and half). The
level textures are tiny; the costs on mobile are the sources target (RGBA8, multisampled, view-sized) and its resolve.
The sources pass tests the scene's depth read-only (DEPTH_READ_ONLY_OPTIMAL). Composite: additive blend ONE/ONE
colour, ZERO/ONE alpha, as the D3D12 PSO. The B3a attribution variants (`GEKKO_GLARE_COMPOSITE`) are diagnostics of
the D3D12 renderer and are not ported.

### 3.11 Snapshots and histories

Snapshots: one graph texture per snapshot, partial resolve of the readers' texel rectangle (`snapshot_reads.h` is
API-free since H1) - the scissor of the resolve pass; snapshots that are not read at the same time share one image
through the graph's aliasing (V4's hand-made G11 sharing is gone), and a snapshot nobody samples is culled. Histories: kept images per view, recreated (and invalid) on a size change;
`invalidate_histories` marks them invalid. Both are SHADER_READ_ONLY between frames.

### 3.12 Skinning / morph compute

`CSSkin` (compute) writes the skin cache (a storage buffer); the mesh pass reads it as vertex stream 0. Barrier:
COMPUTE_SHADER/SHADER_STORAGE_WRITE -> VERTEX_ATTRIBUTE_INPUT/VERTEX_ATTRIBUTE_READ. Bindless buffers through set 1
binding 2. Needs scalarBlockLayout (44-byte stride) - present on current Adreno / Mali / Xclipse drivers per their
published feature tables (STRONGLY SUPPORTED, verify per device with vulkaninfo); otherwise a 48-byte padded stream.

### 3.13 Canvas placement, compose, present scale

Pure arithmetic shared with D3D12 (`view.h` already API-free; the clip-space matrices in `renderer.cpp`'s anonymous
namespace need hook H2). Scissors become `VkRect2D`. Present scale: same shader; on Android, render size < surface size
is the normal power-saving configuration (e.g. 1600 x 720 on a 2400 x 1080 panel), so the Catmull-Rom path matters
there.

### 3.14 Captures and determinism

`render_to_image` renders into an RGBA8 image and copies it to a host-cached buffer (rows top first, as D3D12).
Self-determinism gate: the same packet rendered twice in two processes must give identical bytes (V1 smoke checks the
exact expected value of a cleared + resolved + composed frame).

## 4. Shader / binding evidence (V0)

`spirv-dis` of the SPIR-V the CMake rule produces:

* graphics: `ResourceDescriptorHeap` -> set 1 binding 0 (`OpTypeRuntimeArray` of 2D and 2D-MS sampled images, several
  aliased variables of the SAME descriptor type), `SamplerDescriptorHeap` -> set 1 binding 1, cbuffers b0/b1/b2 ->
  set 0 bindings 0/1/2;
* compute (CSSkin): three structured-buffer variables aliased on set 1 binding 2 (all STORAGE_BUFFER), Root b0,
  Palette b1;
* capabilities: Shader, RuntimeDescriptorArray (SPV_EXT_descriptor_indexing), DemoteToHelperInvocation (mesh pixel
  shaders), SignedZeroInfNanPreserve (all, from -Gis).

## 5. Mobile constraints

### 5.1 Tile-based GPUs and bandwidth

Mali, Adreno (in its binning mode), PowerVR and Xclipse are tilers: bandwidth, not ALU, decides frame time and heat.

* Load / store ops instead of clears: every graph pass's Clear / Discard becomes CLEAR / DONT_CARE; depth is
  DONT_CARE-stored at the end of the view's last depth use.
* Multisampled targets: transient + lazily allocated when nothing reads them after the pass... which, with the shader
  resolve (3.9), is false for the scene colour; it is true for depth once the view's last depth-testing pass is done.
  The decision in 3.9 is the main bandwidth lever. Done in V6: the depth transient, never stored in a frame without
  snapshots (scene + glare sources in one instance, 3.8).
* Render-pass merging (3.8), and no mid-pass reads of the scene (snapshots end a render-pass instance by necessity).
* Upload rings in host-visible device-local memory (unified memory: there is no separate copy on mobile).

### 5.2 Power, heat, pacing

Measure (directive section 9) on a device: GPU time per pass (timestamp queries: `render_vk/gpu_timer.*`, one span per
rendering group, read when the frame slot comes round - V6; on a tiler a span shows where the GPU's timeline moved,
not an isolated pass cost, and the frame total is exact), frame pacing with
`VK_GOOGLE_display_timing` or Android's Swappy (Frame Pacing library), thermal headroom
(`AThermal_getThermalHeadroom`). 30 Hz vs 60 Hz presentation: the game's tick is the game's; the renderer presents what
the packet queue hands it (FRAME_PACING.md).

### 5.3 Formats

* Scene colour R16G16B16A16_UNORM as a blended, 4x multisampled colour attachment: NOT a mandatory format feature in
  Vulkan (the spec mandates COLOR_ATTACHMENT + BLEND for R16G16B16A16_SFLOAT, not UNORM). On the RTX 4080 it is
  supported. On Android GPUs: UNKNOWN per device - check with `vulkaninfo` / gpuinfo.org. The renderer checks it at
  start (`colour_target_supported`, resources.h) and falls back to R16G16B16A16_SFLOAT, reported on stderr. SFLOAT keeps 11 significant bits
  in [0.5, 1) (step 2^-11) against UNORM16's 2^-16: both far below the 8-bit output step, so no visible banding; the
  quantisation differs from D3D12, which is allowed (validation is self-determinism, not cross-backend pixel parity).
  A2B10G10R10_UNORM is the bandwidth-saving option (half the bytes) to evaluate in V6 against banding in the glare and
  fog gradients.
* Depth D32_SFLOAT: Vulkan guarantees D16 and one of X8_D24 / D32_SFLOAT; check, fall back to D24 (X8_D24_UNORM_PACK32).
* Textures: the asset pipeline decodes the game's textures to RGBA8 with mips (`assets::TextureImage`). For mobile,
  transcode offline (an install-time or build-time asset step, never at draw time) to ASTC LDR (4x4 for UI / fonts /
  alpha-tested, 6x6 or 8x8 for world textures) with ETC2 as the fallback; BC1/BC3/BC7 on desktop. The game's CMPR
  textures are DXT1 blocks: BC1 on desktop is a lossless re-pack; ASTC is a lossy re-encode of already-quantised data
  (visual intent kept; evidence by side-by-side capture). `TextureImage` grows a format + block data; the renderer's
  `create_texture` takes either.

### 5.4 Other Android specifics

32-bit indices (supported everywhere), lines of width 1 only (no wideLines on many mobile GPUs - immediate lines are
width 1 already), no geometry shaders used, pipeline cache persisted in the app's cache directory, surface rotation
(3.2), the app being paused / resumed (surface lost; device kept).

## 6. Capability floor and adaptation (V1 code: `Device::create`, `Capabilities`)

Required (fail with the feature's name): listed in 3.1. Adapted:

| Capability | Without it |
|---|---|
| scalarBlockLayout | skinning uses a 48-byte padded stream variant (V4) |
| R16G16B16A16_UNORM blended MSAA attachment | R16G16B16A16_SFLOAT (implemented) |
| shaderSignedZeroInfNanPreserveFloat32 | the -Gis SPIR-V declares an execution mode the device does not promise; options: compile the Vulkan shaders without -Gis and rely on `precise` (NoContraction) for the determinism the D3D12 build gets from -Gis, then verify run-to-run captures on that device. RTX 4080: supported. Android: UNKNOWN per device. |
| samplerAnisotropy | the anisotropic sampler entries are trilinear |
| update-after-bind limits | the bindless capacity is checked against `maxDescriptorSetUpdateAfterBind*` at start |

## 7. Staged plan

Each stage ends with: build, the smoke test (headless, exact values), validation layer clean, a capture compared with
the D3D12 renderer's for visual intent (not pixels), the docs updated.

* **V1 - skeleton (this session).** Done criteria: compiles; headless smoke test renders split-screen views through
  clear -> over-views start -> resolve -> compose with the exact expected bytes, no validation messages; the windowed
  test presents through a Win32 swapchain including a resize.
* **V2 - frame graph.** Port `frame_graph.*` to Vulkan (or split it into an API-free core + a backend part, see H3):
  Access -> sync2 table (3.6), Load -> load ops, pass culling, pooled targets, imported swapchain / history images,
  GPU timestamps per pass. Done: V1's frame expressed as graph passes, identical bytes.
* **V3 - resources and draws.** Handle tables and resource worker (texture upload with mips through a staging ring;
  mesh streams in one buffer like G13), pipeline cache keyed like `PipelineKey`, mesh base / environment / dual
  passes, particles (instance stream), immediate geometry (world + canvas, anchors, regions, scissors), light sets,
  fog, alpha cutoff, skip reasons. Done: a recorded Westopolis packet stream (stg0100 tape replay, desktop) renders
  every draw with zero skips; capture reviewed against D3D12 for visual intent; run-to-run byte identity.
* **V4 - effects.** Glare chain, snapshots (partial), histories, skinning / morph compute, invalidate_histories,
  commit_history semantics. Done: the motion trail, heat haze and glare scenes reviewed; byte identity run to run.
  Carry over the D3D12 renderer's two GPU optimisations (main session, 2026-10-26; docs/OPTIMIZATION_RESULTS.md
  increment 30; render/renderer.cpp history_rtvs_, later_canvas_spans, FoldedGlare; shaders/glare.hlsl):
  - **History saves (B1):** the view history is a colour-attachment-capable image and a committed SaveHistory resolves
    the 4x scene STRAIGHT into it with PSResolve - no transient resolve target, no full-view copy (D3D12: 0.090 ->
    0.049 ms per save at 2560x1440). A capture that does not commit resolves into a frame image instead.
  - **Glare composite folded into the resolve (B3a):** when only canvas draws follow the view's Glare pass and the
    scene has >= 4 samples, the ONE/ONE composite runs only on the 16-px tiles those canvas draws can reach (their
    placed bounds + 1 px, merged into disjoint row spans, scissored), and the view's final resolve (PSResolveGlare)
    adds the glare per sample with the blend's arithmetic - k = round(saturate(sample + glare) * 65535), the float
    from a 256 x 256 R16_UNORM table of every step (the GPU's own UNORM -> float conversion) - then PSResolve's
    fixed-order mean; the spans are re-resolved plainly. Plain composite otherwise (3D, snapshots, history or another
    glare after it; 2x; the glare debug options). D3D12 4x: composite + resolve 3.81 -> 2.48 ms. On a tiler the form to
    measure first is the glare added inside the resolve while the samples are still in tile memory (one render-pass
    instance: scene, then a resolve subpass / dynamic-rendering local read), so the 4x scene never goes to DRAM;
    the table-based per-sample arithmetic keeps the result the D3D12 fold's.
* **V5 - Android shell.** NDK build (CMake toolchain `android.toolchain.cmake`, arm64-v8a, API 33+), GameActivity or
  NativeActivity, ANativeWindow surface lifecycle, pre-rotation in the final pass, input / audio are other subsystems'
  ports. First run in the emulator (system images android-34 / android-36.1 are installed; the emulator's Vulkan is a
  host-translated driver, fine for bring-up, not for performance or format evidence), then a real device.
* **V6 - mobile optimisation (measured).** Render-pass merging, transient attachments, the resolve decision (3.9),
  ASTC / ETC2 textures, scene format A2B10G10R10 evaluation, push constants for root blocks.
* **V7 - integration.** The hooks below; the game chooses the backend at build time (Android: Vulkan only) and,
  on Windows, optionally at run time for testing.

## 8. Integration hooks needed in existing code (not done: other sessions own these files)

| # | Where | Change | Why |
|---|---|---|---|
| H1 (DONE 2026-10-26, by this work at the user's request) | `render/frame.h`, `render/descriptors.h` | move `Filter`, `Address`, `sampler_index`, `kSamplerCount` into an API-free header (e.g. `render/sampler_table.h`); `frame.h` includes only that and `view.h` | `frame.h` -> `descriptors.h` -> `gpu_device.h` -> `<d3d12.h>`: the packet contract could not compile for Android. Done: `render/sampler_table.h`; `descriptors.h` includes it; `frame.h` includes it instead of `descriptors.h`. Verified: all 21 files of the tree that reach `frame.h` pass `cl /Zs` with their own vcxproj flags (release/tests trees; renderer.cpp with the diag tree, which has its newer generated shader headers). |
| H2 (DONE 2026-10-26: `render/frame_math.h` is the single definition - render_vk uses it; the main session token-compared its 21 copies (identical except PixelBox vs D3D12_RECT, adapted with to_rect()), switched renderer.cpp to the header and deleted its copies; its captures before / after pixel-identical within MSAA noise) | `render/renderer.cpp` anonymous namespace | move the API-free arithmetic and the shader-block layouts to shared headers: `Mat4`, `multiply` (double accumulation), `from3x4`, `view_of`, `projection_of`, `canvas_aspect_of`, `canvas_projection`, `region_projection` (returning a plain rect instead of `D3D12_RECT`), `canvas_image_rows`, `packed`, `flags_of`, `fill_light_constants`, `DrawConstants`, `LightConstants`, `ParticleConstants`, `ImmediateConstants`, `kPrelit...` | one definition of every number both backends feed the same shaders; duplicating it would let the backends drift (and is the kind of copy the north star forbids). |
| H3 | `render/frame_graph.*` | optional: split the graph's API-free core (pass list, culling, read-before-write check, lifetimes, pooling keys) from the D3D12 recording | lets V2 reuse the core instead of a parallel copy |
| H4 | root `CMakeLists.txt` | `option(SHADOW_RENDER_VULKAN)`, `add_subdirectory(runtime/lift/render_vk)`; one shader entry list feeding DXIL and SPIR-V compiles (the SPIR-V compile needs the Vulkan SDK's dxc.exe - the Windows SDK's has no SPIR-V back end) | build integration |
| H5 | `runtime/scaffold/rw_driver/rw_driver.h` (`RwDriver(render::Renderer&, ...)`), `rw_driver.cpp`, `textures.cpp` | depend on a small resource interface (request_texture / request_mesh / release / finish_packets / release_scope) instead of the concrete D3D12 class | the driver must create resources on either backend |
| H6 | `runtime/src/gekko/lifted_render.cpp` | owns `render::Renderer` and passes an `HWND`; needs a backend choice and a platform window type (`NativeWindow`), stats printing per backend | the owner of the renderer |
| H7 | `assets/texture_import.*` | a compressed-format path (block data + format) next to RGBA8 | mobile texture bandwidth (5.3) |
| H8 | `render/display_modes.*`, `present_trace.*`, `gpu_timer.*`, `gpu_memory.*` | DXGI / D3D12-specific diagnostics: Android needs its own display-mode list (Display API / `ANativeWindow` sizes) and timers (timestamp queries) | diagnostics parity |

## 9. Open questions and risks (evidence levels per the north star)

* R16G16B16A16_UNORM blendable MSAA attachment on target phones - UNKNOWN; fallback implemented.
* Hardware MSAA resolve determinism on tilers - UNKNOWN; decides the largest bandwidth cost (3.9).
* -Gis / SignedZeroInfNanPreserve on mobile drivers - UNKNOWN; affects determinism strategy (6).
* Front-face convention with negative-height viewports - STRUCTURAL MATCH; confirm with the first mesh capture (V3).
* Render-target retention (3600 frames) on Android - the D3D12 reason was a Windows-driver commit effect; measure.
* The Android emulator's Vulkan (gfxstream to the host GPU) is good for bring-up only; feature lists there are the
  host's, not a phone's.

## 10. Toolchain report and what ran (2026-10-26)

Machine: Windows 11 Pro, RTX 4080 (driver 610.88, Vulkan 1.4.341) + Intel UHD 770 (Vulkan 1.4.323), VS 2026
Community 18.10, CMake 4.2.3, Clang (LLVM) present, no Ninja on PATH.

| Tool | Found |
|---|---|
| Vulkan SDK | `C:\VulkanSDK\1.4.350.0` (`VULKAN_SDK` set): headers incl. `vulkan_android.h`, `vulkan-1.lib`, validation layer, `dxc.exe` 1.9.0.5347 (SPIR-V back end), `glslangValidator`/`glslc`, `spirv-val`/`spirv-opt`/`spirv-dis`, SPIRV-Cross, Slang, `vulkaninfo` |
| VMA | 3.2.1 in `thirdparty/plume/contrib/VulkanMemoryAllocator` (not in the SDK) |
| Android SDK | `C:\AndroidSDK` (also reachable as `%LOCALAPPDATA%\Android\Sdk`; `ANDROID_HOME` / `ANDROID_SDK_ROOT` NOT set): platforms android-33/34/36.1, build-tools 34.0.0/36.1.0, cmdline-tools, emulator, system images android-30 (google_apis), android-34 (android-wear), android-36.1 (google_apis_playstore), SDK CMake 3.22.1, platform-tools (`adb` on PATH; no device attached) |
| Android NDK | 25.2.9519653 and 27.2.12479018 (`ANDROID_NDK_HOME` not set) |
| JDK | Eclipse Adoptium 21.0.10 |
| Android Studio | `G:\Program Files\Android\Android Studio\bin\studio64.exe`, build AI-253.30387.90.2532.14935130, with its own JBR (`jbr\bin\java.exe`); for V5 (app shell, Gradle/AGP project, emulator / AVD manager, GPU inspector) |

Device features of note on the RTX 4080: VK_EXT_mutable_descriptor_type, VK_KHR_dynamic_rendering_local_read,
descriptorBindingPartiallyBound, runtimeDescriptorArray.

Shader evidence: section 4 (DXC compile + spirv-val of all 21 entry points, run 2026-10-26 by hand into the session's
scratch directory; the CMake rule repeats it at build time).

### What ran (2026-10-26, Windows, RTX 4080, no game process running)

* Configure + build: `cmake -S runtime/lift/render_vk -B .build/vk -G "Visual Studio 18 2026" -A x64`, then
  `cmake --build .build/vk --config Release -j 2` (MSBUILDDISABLENODEREUSE=1). All 21 SPIR-V headers generated; the
  library and both test executables built with /W4 /permissive- and no warnings.
* `.build/vk/Release/shadow_render_vk_smoke.exe --validation` -> PASS, and again with `--samples 1` -> PASS. Four
  frames (64x32 twice - pooled targets reused -, 33x17, 640x360), each: three views (two halves + an over-views
  overlay) in a 1:1 content box, every pixel the background through UNORM16 -> resolve -> compose -> RGBA8, rendered a
  second time to identical bytes; 3 skipped draws and 1 not-run history pass per frame as the packet says; validation
  layer silent. Scene format R16G16B16A16_UNORM, 4x, supported natively here.
* `.build/vk/Release/shadow_render_vk_window.exe --validation --frames 120` -> PASS: 120 frames presented to a Win32
  swapchain (FIFO), a resize half-way, validation silent.
* NDK 27.2 clang 18 (`--target=aarch64-linux-android33 -std=c++17 -c`): device, resources, bindless, frame_loop,
  swapchain (Android surface path), pipelines, vma compile; renderer.cpp fails at `render/gpu_device.h: 'd3d12.h' file
  not found` - exactly hook H1. Warnings: only -Wmissing-field-initializers from the `VkX x{VK_STRUCTURE_TYPE_...}`
  idiom, and VMA's own.

Found on the way (fixed): the over-views pass was first recorded inside a rendering instance with a depth attachment
its pipeline does not declare (validation VUID-vkCmdDraw-dynamicRenderingUnusedAttachments-08914); it is colour-only
now, as on D3D12. And the first smoke version expected round-to-nearest for float -> UNORM; Vulkan allows either
neighbour ("Fixed-Point Data Conversions"), and this driver's clear takes the lower one at exact .5 ties (0.5 -> 127,
0.1 -> 25, 0.3 -> 76 in RGBA8). The test accepts both neighbours and requires run-to-run identity instead; the same
packet on D3D12 may give the other neighbour - one more reason the gate is self-determinism, not cross-backend parity.

NOT verified: anything drawn by the game (no draws are drawn yet), Android at run time (no emulator or device run),
the Intel GPU, performance. The smoke test's picture is uniform, so it cannot detect a misplaced view rectangle; the
first test that can is V3's (draws).

### Android emulator run (2026-10-26)

Bring-up activity (`render_vk/android/bringup_main.cpp`, README there): NDK 27.2, `android-33`, x86_64, APK made by
`android/package_apk.py` (aapt2 / zipalign / apksigner, throw-away key), AVD `shadow_vk_api36` (Pixel 7 profile,
Android 36.1 x86_64 Google Play image, `-gpu host`, headless). Logcat (`adb logcat -s shadow_vk`):

* device "Goldfish GFXStream (NVIDIA GeForce RTX 4080)", Vulkan 1.3.0 - the capability floor passed;
* scalarBlockLayout yes, signedZeroInfNanPreserve f32 yes, anisotropy 16, update-after-bind 1048576 images / buffers,
  min UBO offset alignment 64, max UBO range 65536;
* R16G16B16A16_UNORM, R16G16B16A16_SFLOAT, A2B10G10R10_UNORM, R8G8B8A8_UNORM all blendable at 1x and 4x; D32_SFLOAT,
  X8_D24, D24S8 depth; ASTC LDR, ETC2, BC; no lazily allocated memory (it is the desktop GPU behind gfxstream - none of
  this is a phone's answer);
* the scene path (4x UNORM16 scene, PSResolve, compose) ran: `BRINGUP PASS: 300 frames presented` in ~5.4 s (vsync);
  HOME then relaunch: "surface released after 1279 frames", a new swapchain on the new surface, drawing resumed.

Found: the surface reports `currentTransform = ROTATE_90` (a landscape app on a portrait-native panel), and the first
swapchain took preTransform = ROTATE_90 without drawing turned - the screenshot showed the 4:3 box stretched and
misplaced. Fixed by real pre-rotation (`swapchain.h`): images in the native orientation (extent sides swapped),
preTransform = currentTransform, the final pass draws turned (`Screen::Present` = render_vk/shaders/present.hlsl
VSFullscreenRotated + PSTexture, `turned_rect`); used by both the bring-up and `render_vk::Renderer`'s compose.
Re-run (same day, after the game was closed): swapchain "picture 2264x1080, images 1080x2264, preTransform 0x2 (1
quarter turns)"; screenshot shows the asymmetric test picture correctly placed (4:3 box 1440x1080 at the right edge,
marker square in the top-left corner next to the 136 px cutout), also after HOME + relaunch; BRINGUP PASS. Windows
smoke + window tests re-run after the compose change: PASS, validation silent.

Also seen: on the first launch after a cold boot the surface reported 1x1 at APP_CMD_INIT_WINDOW; the first present
reported the swapchain out of date and it was rebuilt at the real size (every rebuild is logged now, with its reason).
So a 1x1 first surface is normal on Android, and resize-on-out-of-date is required.

### Packets on Android (2026-10-26, after H1)

* `render_vk::Renderer` and `render/frame.cpp` compile for Android (x86_64 and arm64, NDK 27.2, no warnings); the
  packet-free bring-up file was deleted as its scaffolding note said. Targets: `shadow_render_vk_core`,
  `shadow_render_vk` (renderer + FrameBuilder), `shadow_render_vk_checks` (the headless checks, shared), and on
  Android `libshadow_vk.so` (`android/main.cpp`, package `com.shadow.vk`).
* Emulator: capability report as before; `CHECKS PASS` - the same four off-screen split-screen frames as the Windows
  test give the same bytes (64 127 191 / 255 0 51 / 25 229 102 / 153 76 13; the emulator's Vulkan is the same NVIDIA
  GPU, so this is not phone evidence) and re-render identically; `PRESENT PASS: 300 frames` through the pre-rotated
  swapchain (the packet's 3 draws per frame counted as skipped - V3); HOME + relaunch keeps the device.
* Found: the activity was RECREATED about 3 s after launch - the emulator's SIM registered (mcc 310 / mnc 260) and
  `mcc|mnc` were not in `configChanges`. Real phones see the same on SIM changes and roaming. The manifest now
  declares every configuration change (a recreated activity would rebuild the GPU device); a uiMode (night) change
  then arrived as APP_CMD_CONFIG_CHANGED with no relaunch.
* Windows: smoke (validation) + window tests PASS after the restructuring.

### V3: draws (2026-10-26)

* New: `render/frame_math.h` (H2's shared math and constant blocks, a verbatim copy of renderer.cpp's), render_vk
  `renderer_resources.cpp` (textures / meshes: staging + one-time upload command buffer on the shared queue, barriers
  to every later frame; handles, scopes, deferred releases as render::Renderer), `renderer_draws.cpp` (the Scene
  pass's draws, line for line after render::Renderer add_draw_pass), `Pipelines::draw` (vertex inputs by the
  locations DXC assigned - POSITION, NORMAL, COLOR0, TEXCOORD0/1 in declaration order -, D3D12's blend / cull / depth,
  dynamic viewport, scissor and primitive topology), `Device::submit` / `present` (one queue lock: uploads may come
  from the game thread), `tests/draw_checks.*`.
* Draw checks (`tests/draw_checks.h`, a packet whose right pixels follow from the packet alone; sampled at interior
  points): a counter-clockwise world quad shown and a clockwise one culled (so D3D12's FrontCounterClockwise = TRUE is
  `VK_FRONT_FACE_COUNTER_CLOCKWISE` through the negative-height viewport - PROVEN for this renderer), three overlapping
  quads ordered by depth, a particle, an opaque canvas quad, a half-transparent one (0 0 128 over black), a 2 x 2
  texture sampled texel by texel, the background; 0 skipped draws; re-render identical. PASS on Windows (4x and 1x,
  validation silent; `--png` writes the picture) and on the emulator (same values). The world appears mirrored
  left-right in the picture (world -x on the right): that is the game's camera convention (`look_at`, view_of) in the
  shared math, the same for both backends.
* One test error found on the way, in the test: with perspective the near quad covered the "far only" sample point.
* Not drawn yet (V4, counted): glare sources and glare, snapshots, histories, skinned / delta-morphed draws, immediate
  draws that sample a snapshot or the history. Not ported: the asynchronous resource worker (V3b), GPU timers.
* Not verified: any game content (no game packet stream has been rendered by render_vk), phones (arm64 never run).

### V4: effects (2026-10-26)

* `renderer_effects.cpp` (resolve_into, screen_pass, record_glare with the B3a fold and its tile spans,
  resolve_folded, record_deformations, reserve_skin_cache), the per-view pass walk in `renderer.cpp` record() in the
  D3D12 order (Scene, GlareSources over read-only depth, Glare, Snapshot with the readers' texel region
  (`render/snapshot_reads.h`), SaveHistory straight into the kept history - B1 - or into a frame image for an
  uncommitted capture), `deform.h` (the skinning shader's layouts), `Pipelines::skin` (CSSkin), mesh skin / morph
  sources as bindless storage buffers.
* Effect checks (`tests/effect_checks.h`): a snapshot copy, a history copy across frames, a skinned and a morphed quad
  moved by their bone / target (and absent from their rest place), a glare source (saturated; halo symmetric, falling
  off 223 165 101 52 23 6 0 within 6 px of the saturated rim - the blur sums its taps unweighted, BLOOM_SPEC 6.4 - and
  nothing beyond), a canvas quad after the glare (the fold: 2 tile spans), the picture with the folded and the plain
  composite IDENTICAL (0 values differ; D3D12 measured ~0.1% of pixels by 1/65535 on NVIDIA's MSAA blend collapse -
  not seen in this scene), re-render identical, invalidate_histories and an uncommitted capture leaving the history
  draw out. PASS on Windows at 1x, 2x and 4x with validation, and on the emulator with the same values.
* Two test errors found on the way, both in the test: the halo point (the blur saturates next to a white source by
  design) and the fold count at 1x / 2x (the fold is 4x+ only, by design).
* Not done in V4 (next): the asynchronous resource worker, GPU timestamps per pass, the frame graph (V2) - passes are
  recorded in order with per-image barriers - and the mobile optimisations of V6 (the glare added inside the resolve in
  tile memory, transient MSAA targets, ASTC). Not verified: game content, phones.

### Real game packets (2026-10-26)

* Recorder: `render/packet_stream.{h,cpp}` (new; API-free; the packet's records as their bytes with a layout check in
  the header) and 11 one-line hooks in `render/renderer.cpp` (the SHADOW_RECORD_PACKETS macro: nothing under
  GEKKO_SHIPPING, one null check otherwise; recorder made only when GEKKO_PACKET_RECORD=<file> is set) - agreed with
  and handed back to the D3D12 owner. Replay: `render_vk/tests/vk_replay.cpp` (`shadow_render_vk_replay <stream> --out
  <dir> --reference <d3d-%llu.png> [--samples N] [--anisotropy N]`): resources remade (recorded textures decoded by the
  game's importer), handles remapped, every recorded frame rendered in order (history-only frames too), captures
  compared with the D3D12 captures of the same frames.
* Run: `.build/diag` game (non-shipping), headless cold boot into Westopolis on the westopolis-cpu-spike tape (isolated
  copies of its initial/ files), 25 s game time, captures at 1280x720 from frame 300 every 80 frames (15), 4x MSAA:
  stream 12.6 MB (2,040 events, 91 rendered frames, 280 textures, 79 meshes); 10 s wall.
* Result: render_vk drew every draw of every frame (0 skipped), glare (with the fold), snapshots, skinning; validation
  silent. Against D3D12 over 15 captures: 4.73% of pixels differ at all, 0.68% by more than 2 steps, 0.008% by more than
  16, mean 0.08 steps, largest 42. The same scene, composition, HUD and effects: visual intent equal.
* Attribution (each by a recording + replay of the same tape, deterministic - the unstripped run repeated exactly):
  - SHADOW_R2_STRIP=meshes: 4.73% -> 1.42% (> 16: 0.000%): most of it is in mesh draws; glare 0.8 points
    (strip glare: 3.94%); particles, immediates little;
  - SHADOW_R2_MSAA=1 on both: 4.63% (no change): not MSAA coverage, not the resolve;
  - render_vk anisotropy 16 / 8 / 1 (D3D12 uses 16): 4.73% / 5.06% / 17.19%: the filter settings match at 16;
  - by region in frame 1420: near (magnified) surfaces 0.025% and 0.007% of pixels differ, the distant band 34.0%
    (heavily minified, anisotropically filtered window textures; nearly all by <= 2 steps).
  So the residual is the minification / anisotropic filtering of the two drivers (NVIDIA D3D12 vs Vulkan on the same
  RTX 4080) - STRONGLY SUPPORTED (same texel data, same sampler parameters, same precise shaders), not PROVEN (that
  needs a sampler probe on both APIs). It is a GPU-filtering difference, not a renderer difference; on phones each
  driver filters its own way anyway, so the gate stays self-determinism plus visual intent.
* CTest Release preset with the hooks (cmake --build --preset tests -j 2; ctest --preset tests -j 2): 118/118 passed,
  743 s (run after the recordings; the recordings only read the game).

### V2: frame graph (2026-10-27)

* Code: `render_vk/frame_graph.{h,cpp}` (Access / Load / TextureDesc, TargetPool, PassBuilder, PassContext,
  FrameGraph: compile, execute, introspection: kept / culled passes, planned barriers, the image each texture got,
  stats); `renderer.cpp` record() declares every pass (deformations, views under, view clear, scene, glare sources,
  the glare chain's resolve / capture / blur / box / merge / composite, snapshot, save history, resolve or resolve with
  glare, compose, readback) and runs compile + execute; the pass bodies (`renderer_draws.cpp`, `renderer_effects.cpp`)
  take their images and attachments from the PassContext and record no barriers of their own. Renderer::Stats gains
  `graph` (passes, kept, culled, textures, images, aliased, barriers, barrier calls), `pool_bytes`, `pool_created`.
* Checks (`tests/graph_checks.{h,cpp}`, run by `shadow_render_vk_smoke` and the Android activity): the graph alone on
  the GPU - 7 passes of clears and copies: the unread clear culled, z taking over x's image (3 textures in 2 images),
  x's barriers (one discarding from UNDEFINED to COLOR_ATTACHMENT, one COLOR_ATTACHMENT -> TRANSFER_SRC, none before its
  second read), load / store ops (x CLEAR / STORE, the unread depth CLEAR / DONT_CARE), the executed picture (red left,
  green right: the aliased clear did not reach the earlier copy), a second frame making 0 images with the same pixels;
  five invalid graphs refused with the reason; in the renderer - an unread snapshot and glare sources without a glare
  culled (2 passes) with the picture identical, 0 targets made for a frame seen before, split screen: 6 textures in 4
  images (the second view's colour and depth alias the first's).
* Windows, RTX 4080, validation on, `shadow_render_vk_smoke --samples 1 / 2 / 4`: PASS (14 graph checks plus all the
  earlier ones); the draw and effect checks' pictures byte-identical to the pre-graph build at all three sample counts;
  the smoke frames' pools smaller (e.g. 27 -> 23 images).
* Westopolis replay (the 2026-10-26 recordings, validation on): every capture byte-identical to the pre-graph
  render_vk captures - 15 / 15 at 4x, 15 / 15 at 1x; 0 skipped draws; validation silent. Per frame 16-18 passes (none
  culled in this stretch - the game's frames have no dead passes here), 12 textures in 9 images, one barrier call per
  pass; over 91 frames 1,111 textures in 819 images (292 aliased); the target pool holds 9 images (67.0 MiB at 4x,
  24.8 MiB at 1x, 1280x720).
* Android emulator (shadow_vk_api36, x86_64, gfxstream on the RTX 4080 - not phone evidence): the same 14 graph checks
  ok, `CHECKS PASS` (all checks), `PRESENT PASS: 300 frames`; arm64 builds, not run (no device).
* Not done in V2: GPU timestamps per pass (planned with V6's measurements), merging adjacent passes into one rendering
  instance (V6), the D3D12 graph's target trace (`set_target_trace`) - not needed until the resolution audit covers
  Vulkan.

### V6 (first part): transient attachments, pass merging, GPU timestamps (2026-10-27)

* Code: `frame_graph.*` - the transient kind (`TextureDesc::transient`, decided in compile: every kept use an
  attachment; `pooled_usage` gives attachment + TRANSIENT_ATTACHMENT; `create_image` asks VMA for lazily allocated
  memory and falls back to device memory; `Image::transient` / `Image::lazy` say what it got), mergeable passes
  (`PassBuilder::mergeable`, `PassContext::begin_rendering / end_rendering`, merge planning after the barriers,
  `Stats::merged`, `Stats::transient`, `FrameGraph::groups()`, `set_transient / set_merging` for A/B), timestamps
  (`gpu_timer.{h,cpp}`: a query pool per frame slot, one timestamp at the frame's start and one after each rendering
  group, read without waiting when the slot comes round or right after a capture's wait). `Capabilities` gains the
  queue's `timestamp_bits` and `timestamp_period` (timestamps off when the queue writes none). `RendererOptions`
  gains `transient_targets`, `merge_passes`, `gpu_timestamps` (all on); `Renderer::Stats` gains `gpu_passes` (name,
  ms per group), `gpu_ms`, `gpu_frames`. The draw passes are mergeable; `record_draw_pass` returns bool.
* Checks (`tests/graph_checks.cpp`, `graph_v6`): the graph alone - three mergeable passes on one colour and depth
  recorded as one instance ("draw a + draw b + draw c", no barrier before "draw b"), the instance's store ops its last
  pass's (colour STORE: copied later; depth DONT_CARE: nothing after the instance, although "draw b" uses it), the
  depth transient and the copied colour not, the executed picture (red left, green right, drawn inside the merged
  instance by vkCmdClearAttachments), timestamps (3 spans, one per group, named after them, summing to the frame),
  merging off (5 groups, same picture), a read-only depth and a clear each ending the instance (same picture); in the
  renderer - two Scene passes in a row merged ("scene + scene"), the view's depth transient, the GPU times of that
  frame one per group. 24 graph checks in all.
* Windows, RTX 4080 (no lazily allocated memory: the transient images get device memory), validation on,
  `shadow_render_vk_smoke --samples 1 / 2 / 4`: PASS; the draw and effect checks' pictures byte-identical to the V2
  build's at all three sample counts (baselines rendered by the V2 binaries just before the change).
* Westopolis replay (validation on): 15 / 15 captures byte-identical to the V2 build's at 4x and 15 / 15 at 1x;
  validation silent; `--v2-plan` (transient and merging off) also byte-identical. Per frame 1 transient texture (the
  view's depth: the scene colour and the glare sources are resolved by a shader, so they are sampled, not
  attachment-only); 6 passes merged in 91 frames ("scene + scene", frames with snapshots); target pool unchanged (9
  images, 67.0 MiB at 4x). GPU time per frame, 4x, 1280x720, one frame at a time (render_to_image waits): mean 0.44
  ms; the capture's readback copy into host memory is 65% of it (a replay artefact, not game rendering), the scene
  passes 18%, every glare pass under 0.015 ms. A/B, back to back, two runs each, no validation: V6 0.437 / 0.444 ms,
  V2 plan 0.441 / 0.445 ms - no difference on a desktop GPU, as expected (no tiler, no lazily allocated memory).
* Android emulator (shadow_vk_api36, x86_64, gfxstream - not phone evidence): timestamps 64 bits at 1 ns, no lazily
  allocated memory; all graph checks ok, `CHECKS PASS`, `PRESENT PASS: 300 frames` (same figures as V2). arm64 builds,
  not run (no device).
* What this means for a phone (UNVERIFIED until a device run): the depth is transient but still stored between
  instances (scene -> glare sources, read-only, in its own instance -> scene), so a tiler commits its memory anyway;
  the multisampled scene colour leaves the tile for the shader resolve (3.9). The bandwidth levers are therefore the
  ones not done yet: the scene and the glare sources in one instance (then the depth never leaves the tile), and the
  resolve decision. Measure each with the timestamps on a device first.
* Not done in this part: those two, ASTC / ETC2, A2B10G10R10, push constants for root blocks.

### V6 (second part): the depth kept on tile (2026-10-27)

* Code: `vk_common.h` (the local-read declarations), `device.*` (`Capabilities::local_read`, the extension and feature
  enabled when offered, `Device::set_attachment_locations`, `DeviceOptions::local_read`), `pipelines.*` (`Pair`,
  `DrawPipelineKey::depth_attachment / pair / other`, the pair pipelines with their attachment locations),
  `renderer.cpp` record() (per view the last depth pass, per glare sources pass whether a Glare pass reads it, the
  runs, the pair pass, depth-free passes), `renderer_draws.cpp` (`record_draw_pass` over a list of `DrawStep`s in one
  instance, `record_draws` per pass), `frame_graph.*` (`Stats::stores`, `Stats::depth_stores`: attachments each
  instance stores at its end, the depth ones apart). `RendererOptions::keep_depth_on_tile` (on), `Stats::pairs`.
  Smoke and replay: `--v2-plan` (all V6 off), replay `--no-tile-depth`, both `--no-local-read`, replay `--groups`
  (each capture's rendering groups with their GPU times).
* Checks (`graph_checks.cpp`, in the renderer): a scene pass, glare sources, their glare and a canvas pass without a
  depth test - with local read 1 pair and the depth stored 0 times (it never leaves the tile), the picture as drawn;
  without (`--no-local-read`, the emulator) 0 pairs and the depth stored once, for the sources' test. The V6 renderer
  checks skip themselves when their options are off.
* Windows, RTX 4080 (local read yes), validation on: `shadow_render_vk_smoke` PASS at 1x / 2x / 4x in three modes (on,
  `--no-local-read`, `--v2-plan`); the draw and effect checks' pictures byte-identical across the three modes at every
  sample count (the effect checks' glare scene forms a pair).
* Westopolis replay, validation on, silent: the 15 captures byte-identical across on, `--no-local-read`,
  `--no-tile-depth` and `--v2-plan` at 4x, and on vs `--v2-plan` at 1x; every capture's statistics against the D3D12
  captures identical to the V2 log's (4x against the 4x D3D12 captures, 1x against `strip-none-x1`) - the V2 pictures
  themselves were not kept, so this, with the on / off identity, is the link to the earlier builds. One pair per frame
  (91 in 91 frames); depth stores over 91 frames: 184 with the V2 plan, 178 with V6's first part, 104 without local
  read, 19 with the pair - 0 in a frame without snapshots (frame 300: 14 attachment stores, none depth; V2 plan 15, 1
  depth). The 19 are the snapshot frames: a snapshot resolve reads the scene colour mid-scene, which ends the instance
  while later scene passes still test the depth.
* What it saves on a tiler (arithmetic, UNVERIFIED on a device): at 4x, 1280x720, the depth (D32, 4 samples) is 14.1
  MiB; a normal Westopolis frame no longer stores it once and loads it twice (for the sources' test and for the canvas
  pass) - about 42 MiB of memory traffic per frame, 2.5 GB/s at 60 Hz. GPU time on the RTX 4080 (not a tiler), no
  validation, three runs each, alternated: on 0.439 / 0.448 / 0.465 ms, V2 plan 0.445 / 0.448 / 0.446 ms - no
  difference, as expected.
* Android emulator (shadow_vk_api36: no local read): the fallback - `CHECKS PASS` (0 pairs, depth stored once),
  `PRESENT PASS: 300 frames`; the pair path ran only on the RTX 4080. arm64 builds, not run.
* Not done: snapshots inside an instance (the snapshot resolve as a local read of the colour - the 19 remaining depth
  stores), the resolve decision (3.9), and every measurement on a phone (which phones offer local read: to be listed
  from gpuinfo.org / a device run).

### Hand-over (next step: V5 frame pacing / an arm64 device run, or the rest of V6)

* Files: `runtime/lift/render_vk/` - `vk_common.h`, `device.*`, `resources.*` (images, buffers, `use_image` sync2
  tracking for uploads), `frame_graph.*` (V2, V6), `gpu_timer.*` (V6), `bindless.*`, `frame_loop.*`, `swapchain.*`, `pipelines.*` (screen pipelines; SPIR-V headers from the
  build), `renderer.*` (`render_vk::Renderer`, same public shape as `render::Renderer`), `vma.cpp`,
  `tests/vk_smoke.cpp`, `tests/vk_window.cpp`, `CMakeLists.txt`.
* Scaffolding marked in code (deleted by V3/V4): `SkipReason::NotImplemented`, the not-run glare / snapshot / history
  passes, the empty depth-only pass, the resource stubs.
* For the Android emulator: set `ANDROID_HOME=C:\AndroidSDK`, use `ndk/27.2.12479018/build/cmake/android.toolchain.cmake`
  (`-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33` for phones). Installed emulator images: android-30
  google_apis (arm64-v8a and x86_64), android-34 android-wear (x86_64), android-36.1 google_apis_playstore (x86_64) -
  on this x64 host the emulator wants an `-DANDROID_ABI=x86_64` build. (H1 has since landed: everything builds for Android - android/README.md.)
