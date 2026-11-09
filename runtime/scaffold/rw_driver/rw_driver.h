// SCAFFOLDING (R2): the RenderWare GameCube driver's draw-facing entries on the native renderer - deleted by R6: RenderWare's own lift removes the translated objects this reads (R4 the 2D layer, R5 the render tree first)
#pragma once

// docs/lift-20261006/R2_DRIVER.md 2.1 and 4.3. Translated RenderWare and game code reach the RenderWare GameCube
// driver through generator-lowered call sites (extract/lift-sites-r2.txt); each lowered site calls one function of
// this layer with the call's register values. The layer reads RenderWare objects in the address window, keeps the
// driver's render-state values natively, writes back exactly the RenderWare-visible results the driver produced, and
// describes each frame to the renderer (runtime/lift/render) as values: views, passes, draw records. It owns no GPU
// object and never identifies a resource by a guest address: native handles live in words of the RenderWare objects
// that translated code no longer reads (4.3).
//
// While RenderWare's object factories and memory management stay translated (R2_DRIVER.md 4.1, 4.3), the objects
// that own those words are freed by translated code; once per frame `reconcile` releases every handle whose word no
// longer holds it.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gekko/hud_listener.h"
#include "gekko/appear_fade.h"
#include "far_objects.h"
#include "material_probe.h"
#include "ui/hud_layout.h"
#include "game/rw_api.h"
#include "gekko/types.h"
#include "render/frame.h"
#include "render/packet_queue.h"
#include "render/renderer.h"
#include "rwim/immediate.h"

namespace gekko {
class Cpu;
}

namespace gekko::scaffold {

// The guest's layout (RW 3.7, GameCube build; r2/SITES_DEVICE.md 1.1, 4.5; r2/SITES_PIPELINES.md 2.9).
namespace layout {
// Words the guest's small-data area holds (r13 = 0x805EC500).
constexpr u32 kRwGlobals = 0x805F265C;         // RwGlobals*
constexpr u32 kInStageCameraHolder = 0x8056FF1C;  // +0x00 the in-stage RwCamera* (projection.cpp kCameraHolder)
constexpr u32 kImTexture = 0x805F270C;         // the driver's Im texture (RwTexture*), made by FINALIZESTART
constexpr u32 kWhiteRaster = 0x805F2708;       // the driver's 4x4 white raster
constexpr u32 kRasterExtOffset = 0x805F2700;   // raster plugin offset (0x34)
constexpr u32 kTextureExtOffset = 0x805F2718;  // texture plugin offset (0x58)

// RwGlobals
constexpr u32 kGlobalsCurrentCamera = 0x00;
constexpr u32 kGlobalsCurrentWorld = 0x04;
constexpr u32 kGlobalsLightFrame = 0x0A;  // u16

// RwCamera
constexpr u32 kCameraFrame = 0x04;
constexpr u32 kCameraProjection = 0x14;  // 1 perspective, 2 parallel
constexpr u32 kCameraFrameBuffer = 0x60;
constexpr u32 kCameraViewWindow = 0x68;  // f32 x, y: tan of the half angles
constexpr u32 kCameraViewOffset = 0x78;
constexpr u32 kCameraNear = 0x80;
constexpr u32 kCameraFar = 0x84;
constexpr u32 kCameraFogPlane = 0x88;
// RwFrame: the local-to-world matrix (right, up, at, pos rows of 16 bytes).
constexpr u32 kFrameLtm = 0x50;

// RwRaster
constexpr u32 kRasterParent = 0x00;
constexpr u32 kRasterWidth = 0x0C;
constexpr u32 kRasterHeight = 0x10;
constexpr u32 kRasterDepth = 0x14;
constexpr u32 kRasterOffsetX = 0x1C;  // s16
constexpr u32 kRasterOffsetY = 0x1E;  // s16
constexpr u32 kRasterType = 0x20;     // u8, low 3 bits: 0 normal, 1 Z, 2 camera, 4 texture, 5 camera texture
// The raster extension as the translated factories write it, and the one word this layer owns.
constexpr u32 kRasterExtGxFormat = 0x0C;
constexpr u32 kRasterExtTlutFormat = 0x10;
constexpr u32 kRasterExtFlags = 0x14;     // bit 0: has alpha
constexpr u32 kRasterExtPixels = 0x1C;    // GX-tiled texels, every level
constexpr u32 kRasterExtPalette = 0x20;
constexpr u32 kRasterExtTexture = 0x2C;   // the native texture handle (the driver's TMEM-region word, read only by
                                          // the lowered bind and copy; the translated create writes 0)
constexpr u32 kRasterExtLevelsMinusOne = 0x32;  // u8, 0xFF: derive

// RwTexture
constexpr u32 kTextureRaster = 0x00;
constexpr u32 kTextureFilterAddressing = 0x50;
// The texture extension: the driver's GX texture object (read only by the lowered bind and LOD functions) holds the
// LOD values natively set; +0x20 keeps the driver's state word.
constexpr u32 kTextureExtLodBias = 0x00;  // f32
constexpr u32 kTextureExtBiasClamp = 0x04;
constexpr u32 kTextureExtEdgeLod = 0x08;
constexpr u32 kTextureExtMaxAniso = 0x0C;
constexpr u32 kTextureExtState = 0x20;
constexpr u32 kTextureStateStale = 0x01000000;

// Plugin offsets and pipelines the guest keeps in its small-data area.
constexpr u32 kMaterialEffectsOffset = 0x805F2750;  // MatFX material data (rpMatFXMaterialData*) offset
constexpr u32 kMatFxAtomicPipeline = 0x805F2764;
constexpr u32 kMatFxSectorPipeline = 0x805F2768;  // renderer-re 04_GC_DRIVER.md (0x804A64E4)
constexpr u32 kSkinPipeline = 0x8056E7F0;
constexpr u32 kSkinMatFxPipeline = 0x8056E7F4;
constexpr u32 kSkinAtomicOffset = 0x8056E7CC;    // RpHAnimHierarchy* at atomic + [this] (r2/SITES_PIPELINES.md 2.5)
constexpr u32 kSkinGeometryOffset = 0x8056E7D0;  // RpSkin* at geometry + [this]
// RpDMorph's plugin offsets (the render callback 0x80444AC4 reads them as [r13+0x5FD0] / [r13+0x5FD8];
// docs/mesh-re-20261007/M4_DEFORMATION.md 2.2-2.3): rpDMorphGeometryData* at geometry + [this].
constexpr u32 kDMorphGeometryOffset = 0x805F24D0;
constexpr u32 kPtankPipeline = 0x805F24F8;
constexpr u32 kPtankExtOffset = 0x805F24F0;      // the PTank extension at atomic + [this] (r2/SITES_PIPELINES.md 2.9)

// The PTank extension: counts, the cluster table ({data, stride} pairs), the axes the render callback hands the
// emitter, the data / platform flags, the constant colour.
constexpr u32 kPtankActiveCount = 0x04;
constexpr u32 kPtankPlatFlags = 0x44;
constexpr u32 kPtankPositions = 0x4C;
constexpr u32 kPtankSizes = 0x64;
constexpr u32 kPtankColours = 0x6C;
constexpr u32 kPtankRotations = 0x7C;
constexpr u32 kPtankUvs = 0x84;           // two corners per particle
constexpr u32 kPtankAxes = 0x94;          // userData: {right, up} for this draw
constexpr u32 kPtankDataFlags = 0xA0;
constexpr u32 kPtankConstantColour = 0xC4;  // RGBA8
// Data flags: per-particle clusters, and the constants that replace them.
constexpr u32 kPtankPosition = 0x01, kPtankColour = 0x02, kPtankSize = 0x04, kPtankMatrix = 0x08,
              kPtankNormal = 0x10, kPtankRotation = 0x20, kPtankVertexColours = 0x40, kPtankUv2 = 0x80,
              kPtankUv4 = 0x100, kPtankUseCenter = 0x01000000;
constexpr u32 kPtankPlatConstantColour = 0x80000;  // the render's channel: the constant colour

// Im3D (RwIm3DTransform 0x8049B87C, RwIm3DRenderPrimitive 0x8049BB48 / Indexed 0x8049B9AC): the pool the transform
// fills and the stash the render fills right after it (stash = pool + 0x0C), and the 36-byte RwIm3DVertex.
constexpr u32 kIm3DStashInPool = 0x0C;
constexpr u32 kIm3DCount = 0x00;          // pool: u16 vertex count
constexpr u32 kIm3DVertices = 0x04;       // pool: the client's vertices
constexpr u32 kIm3DStride = 0x08;         // pool: 36
constexpr u32 kIm3DFlags = 0x00;          // stash: bit 0 VERTEXUV (textured)
constexpr u32 kIm3DMatrix = 0x04;         // stash: RwMatrix* or 0 (world space)
constexpr u32 kIm3DPrimitive = 0x24;      // stash: 1 lines, 2 polyline, 3 triangles, 4 strip, 5 fan
constexpr u32 kIm3DIndices = 0x28;        // stash: u16*, 0 when not indexed
constexpr u32 kIm3DIndexCount = 0x2C;     // stash
constexpr u32 kIm3DVertexColour = 0x18;   // RGBA8, R first
constexpr u32 kIm3DVertexUv = 0x1C;       // u, v

// The glare (r2/SITES_PRESENT_EFFECTS.md 3.1; docs/effects-2026-09-21/BLOOM_SPEC.md 3.1): the effect manager E, its
// glare object G = E+0x3C with the screen rasters it copies into, its camera and the parameter block E0..FF.
constexpr u32 kEffectManager = 0x805EF2D8;
// An in-engine event (a real-time cutscene) is playing: EventManager's constructor (0x80173EF4) takes the frame interval
// over and sets this small-data byte (r13 + 0x3E5D) to 1; its destructor (the code at 0x80173E44) gives the interval back and
// clears it. The cutscenes were composed for a 4:3 TV and are shown 4:3 (user's choice 2026-10-26).
constexpr u32 kSdaEventHoldsInterval = 0x3E5Du;
// The game's system object (also the frame limiter's): +0xB2 is set while two players play. A two-player match's intro
// events are drawn split for both players and keep the output's shape; only story events are shown 4:3 (user, 2026-10-26).
constexpr u32 kSystemObject = 0x8057E760u;
constexpr u32 kSystemTwoPlayers = 0xB2u;
// The stage's StageAction manager (cached at r13 + 0x2EDC by its getter 0x80074598; docs/tools/STAGE_CONTROL.md): +0x8
// is the action running. A stage's goal (action 6, GoalAction) plays its goal scene and the result screen under an
// EventManager too - the byte above is set from about 4 s after the goal until the save (measured on Westopolis,
// 2026-10-30) - but they are the stage's own screens, not a story cutscene, and keep the output's shape (user,
// 2026-10-30: the result screen was boxed 4:3 at every wider ratio). A played story event runs under action 12.
constexpr u32 kSdaStageActionManager = 0x2EDCu;
constexpr u32 kStageActionCurrent = 0x8u;
constexpr u32 kStageActionGoal = 6u;
constexpr u32 kEffectGlare = 0x3C;
constexpr u32 kGlareSourceRaster = 0x08;  // copy #2's destination (the glare-source image)
constexpr u32 kGlareSceneRaster = 0x0C;   // copy #1's destination (the scene saved before the sources)
constexpr u32 kGlareCamera = 0xDC;
constexpr u32 kGlareEnable = 0xE0;        // u8 E0 master, E1 level A, E2 level B, E3 separate composite,
                                          //    E4 B from the sources, E5 A gain, E6 B gain
constexpr u32 kGlareSpreadA = 0xE8;       // f32
constexpr u32 kGlareSpreadB = 0xEC;
constexpr u32 kGlareTapsA = 0xF0;         // s32 horizontal, +4 vertical
constexpr u32 kGlareTapsB = 0xF8;

// RpSkin (r2/SITES_PIPELINES.md 2.5) and RpHAnimHierarchy (3.6)
constexpr u32 kSkinBoneCount = 0x00;
constexpr u32 kSkinUsedBoneCount = 0x04;
constexpr u32 kSkinUsedBones = 0x08;      // u8[numUsedBones]*
constexpr u32 kSkinInverseBind = 0x0C;    // RwMatrix[numBones]*
constexpr u32 kSkinMaxWeights = 0x10;
constexpr u32 kSkinVertexIndices = 0x14;  // u8[4] per vertex*
constexpr u32 kSkinVertexWeights = 0x18;  // f32[4] per vertex*
// rpDMorphGeometryData (0x1C) and its RpDMorphTarget records (0x40 each; M4_DEFORMATION.md 2.2).
constexpr u32 kDMorphTargetCount = 0x00;
constexpr u32 kDMorphTargets = 0x04;      // RpDMorphTarget[count]*
constexpr u32 kDMorphCurrentValues = 0x08;  // f32[count]*: the weights the callback last applied (per geometry)
constexpr u32 kDMorphTargetSize = 0x40;
constexpr u32 kDMorphTargetFlags = 0x08;  // geometry-flag bits it moves: 0x02 positions, 0x10 normals, 0x08 / 0x04
constexpr u32 kDMorphTargetRleCode = 0x10;      // u8[numCodeElements]*: bit 7 set = morph the next c & 0x7F vertices
constexpr u32 kDMorphTargetRleCodeCount = 0x14;
constexpr u32 kDMorphTargetRleDataCount = 0x18;
constexpr u32 kDMorphTargetVertices = 0x20;  // RwV3d[numDataElements]*
constexpr u32 kDMorphTargetNormals = 0x24;
constexpr u32 kHierarchyFlags = 0x00;
constexpr u32 kHierarchyNodeCount = 0x04;
constexpr u32 kHierarchyMatrices = 0x08;  // RwMatrix[numNodes]*
constexpr u32 kHierarchyNodeInfo = 0x10;  // RpHAnimNodeInfo[numNodes]*: 16 bytes, the node's RwFrame* at +0x0C
constexpr u32 kNodeInfoSize = 16;
constexpr u32 kNodeInfoFrame = 0x0C;
constexpr u32 kHierarchyNoMatrices = 0x02;
constexpr u32 kHierarchyLocalSpace = 0x4000;

// RpWorldSector / RpGeometry / RpAtomic / RpMaterial / mesh header
constexpr u32 kSectorRepEntry = 0x34;
constexpr u32 kSectorMesh = 0x78;
constexpr u32 kSectorVertexCount = 0x82;  // u16
constexpr u32 kGeometryFlags = 0x08;
constexpr u32 kGeometryLocked = 0x0C;     // u16 lockedSinceLastInst
constexpr u32 kGeometryVertexCount = 0x14;
constexpr u32 kGeometryMorphTargets = 0x18;
constexpr u32 kGeometryUvSets = 0x1C;
constexpr u32 kGeometryPrelit = 0x30;     // RwRGBA[numVertices]
constexpr u32 kGeometryTexCoords = 0x34;  // RwTexCoords*[8]
constexpr u32 kGeometryMesh = 0x54;
constexpr u32 kGeometryRepEntry = 0x58;   // the instance: the native mesh handle in the lifted build
constexpr u32 kGeometryMorphTarget = 0x5C;
constexpr u32 kMorphTargetVertices = 0x14;
constexpr u32 kMorphTargetNormals = 0x18;
constexpr u32 kAtomicFrame = 0x04;
constexpr u32 kAtomicGeometry = 0x18;
constexpr u32 kAtomicInterpolatorFlags = 0x4C;
constexpr u32 kAtomicPipeline = 0x6C;
// RpWorld / RpLight / RwFrame
constexpr u32 kWorldDirectionalLights = 0x3C;  // RwLinkList of RpLight (link at light + 0x34)
constexpr u32 kLightInWorldLink = 0x34;
constexpr u32 kLightSubType = 0x01;            // u8: 1 directional, 2 ambient
constexpr u32 kLightFlags = 0x02;              // u8: 1 lights atomics
constexpr u32 kLightFrame = 0x04;
constexpr u32 kLightColour = 0x18;             // RwRGBAReal
constexpr u32 kFrameModelling = 0x10;          // RwMatrix
constexpr u32 kFrameRoot = 0xA0;
constexpr u32 kFrameParent = 0x04;             // object.parent: the parent frame (0 at the root)
constexpr u32 kFramePrivateFlags = 0x03;       // u8; bit 0 on the root: the hierarchy's LTMs are stale
// rpMatFXMaterialData: two 24-byte effect slots, flags at +0x30 (r2/SITES_PIPELINES.md 2.4).
constexpr u32 kMatFxFlags = 0x30;
constexpr u32 kMeshHeaderCount = 0x04;    // u16
constexpr u32 kMeshFirst = 0x10;          // 12-byte meshes: indices*, numIndices, material*
constexpr u32 kMeshStride = 12;
constexpr u32 kMeshMaterial = 0x08;
constexpr u32 kMaterialTexture = 0x00;
constexpr u32 kMaterialColour = 0x04;     // RwRGBA
constexpr u32 kMaterialAmbient = 0x0C;    // f32 ka
// The RG1 prepared-data block a sector's repEntry points at: a 24-byte resource entry, then the big-endian header
// {u16 stamp, u16 mesh serial, u32 flags, u32 attribute count, 8-byte attributes, 8-byte draw lists} whose offsets
// the translated reader relocated to guest addresses (r2/SITES_PIPELINES.md 2.9; NativeDataReadCommon 0x80465AE4).
constexpr u32 kResEntrySize = 0x08;       // the chunk size the reader stored
constexpr u32 kResEntryHeader = 0x18;
constexpr u32 kPreparedFlags = 0x04;      // the word this layer owns once the sector is drawn (handle)
constexpr u32 kPreparedAttributeCount = 0x08;
constexpr u32 kPreparedAttributes = 0x0C;
}  // namespace layout

// The driver's render-state values (r2/SITES_DEVICE.md 1.1), as RenderWare sees them through Get, with the driver's
// defaults (FINALIZESTART's reset 0x804981E8). The GX-facing derivations (z compare, alpha compare) follow below.
struct RenderStates {
    u32 z_write = 1;
    u32 z_test = 1;
    u32 cull = 2;            // RW: 1 none, 2 back, 3 front
    u32 fog_enable = 0;
    u32 fog_type = 1;        // linear
    u32 fog_colour = 0;      // 0xAARRGGBB
    float fog_start = 5.0f, fog_end = 10.0f, fog_near = 0.05f, fog_far = 10.0f;
    u32 src_blend = 5;       // RW blend: SRCALPHA
    u32 dst_blend = 6;       // INVSRCALPHA
    u32 alpha_test_off = 1;  // 1: compare disabled (early depth); 0: the cached compare applies
    u32 cmp0 = 4;            // GX compare: 4 GREATER
    u32 cmp1 = 7;            // 7 ALWAYS
    u32 alpha_op = 0;        // AND
    u8 ref0 = 0;
    u8 ref1 = 0;
    bool full_compare = false;
    // GX fog as the driver last programmed it: FOGENABLE arms and disarms it, FOGCOLOR / FOGTYPE re-arm it with the
    // current camera's planes even while RenderWare fog is off (r2/SITES_DEVICE.md 1.2, cases 14-16); the planes it
    // was armed with.
    bool gx_fog = false;
    float gx_fog_start = 5.0f, gx_fog_end = 10.0f;
};

// NE-RW2: the immediate modes are runtime/lift/rwim (shadow::rwim::Immediate); this layer is their camera / frame host
// and their 2D layout policy (both private: only `imm_` calls them).
class RwDriver final : public HudListener, public game::RwApi, private shadow::rwim::Host,
                       private shadow::rwim::LayoutPolicy {
public:
    RwDriver(shadow::render::Renderer& renderer, shadow::render::PacketQueue& frames);
    ~RwDriver();
    RwDriver(const RwDriver&) = delete;
    RwDriver& operator=(const RwDriver&) = delete;

    // The driver the lowered sites reach: one per process (the game's System), installed before guest code runs.
    void install();
    void uninstall();
    static RwDriver* installed() noexcept;
    // Sees every finished frame on the game thread before it is handed over (headless captures: the exact frame,
    // whatever the render thread keeps up with). Set before the game runs.
    void set_frame_observer(std::function<void(const shadow::render::FramePacket&)> observer) {
        frame_observer_ = std::move(observer);
    }
    // VISetBlack: true while the game wants the display black (NativeVideo::black_requested). Read on the game thread
    // when a frame is finished; that frame is then shown as one black frame. Set before the game runs.
    void set_display_blank(std::function<bool()> source) { display_blank_ = std::move(source); }
    // The fade-in of models that appear inside the view (gekko/appear_fade.h): length in game ticks, 0 = off.
    void set_fade_in_ticks(u64 ticks) noexcept { appear_.set_duration(ticks); }
    u64 fade_ins() const noexcept { return appear_.stats().fades; }
    // The far-object proxies (far_objects.h): the game's gadget library, on/off, the farthest proxy, and the render
    // height the projected-size rule measures in.
    FarObjects& far_objects() noexcept { return far_; }
    // Diagnostics: GEKKO_MATERIAL_PROBE (material_probe.h).
    MaterialProbe& material_probe() noexcept { return material_probe_; }
    void set_far_pixel_height(float pixels) noexcept { far_pixel_height_ = pixels; }

    // ---- render states (render_states.cpp) ----
    u32 render_state_set(Cpu& c, u32 state, u32 value);
    u32 render_state_get(Cpu& c, u32 state, u32 out);
    void render_state_reset();
    void alpha_test_mode(u32 off);
    void alpha_compare(u32 cmp0, u32 ref0, u32 op, u32 cmp1, u32 ref1);
    void texture_set_lod(Cpu& c, u32 texture, u32 bias_clamp, u32 edge_lod, u32 max_aniso, double lod_bias);
    void texture_get_lod(Cpu& c, u32 texture, u32 bias, u32 bias_clamp, u32 edge_lod, u32 max_aniso);

    // ---- textures (textures.cpp) ----
    void native_texture_loaded(Cpu& c, u32 texture, u32 bias_clamp, u32 edge_lod, u32 max_aniso, double lod_bias);

    // ---- cameras and frames (frames.cpp) ----
    u32 camera_begin(Cpu& c, u32 camera);
    u32 camera_end(Cpu& c, u32 camera);
    u32 camera_clear(Cpu& c, u32 camera, u32 colour, u32 mode);
    // ClearQuad 0x80499E58 (a raster-level clear of the current camera's raster): a quad over the rectangle (raster
    // pixels) drawn into the open bracket's view that replaces the colour (`colour`: rgba, R in the low byte) and / or
    // writes the farthest depth (`depth`), whatever is there. Returns 1.
    u32 raster_clear(Cpu& c, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, u32 rgba, bool colour, bool depth);
    void submit_frame(Cpu& c);
    // Samples whether an in-engine event is playing (kSdaEventHoldsInterval) outside a stage's goal (kStageActionGoal):
    // while one is, the output the game sees is 4:3 (output_aspect) and the frame is shown in a centred 4:3 box
    // (FramePacket::content_aspect).
    void sample_event_framing(Cpu& c) noexcept;

    // ---- immediate geometry and copies (immediate.cpp) ----
    u32 im2d_primitive(Cpu& c, u32 type, u32 vertices, u32 count);
    u32 im2d_indexed(Cpu& c, u32 type, u32 vertices, u32 count, u32 indices, u32 index_count);
    // The movie player's frame blit (0x80036570): the raster the decoder rewrites every picture, refreshed when its
    // texels changed, drawn keeping the movie's 4:3 picture whole on any output.
    u32 movie_frame(Cpu& c, u32 type, u32 vertices, u32 count);
    // RwRasterRender's draw (raster_render_install.cpp, os-other): `raster` as a textured quad at `rect` {x, y, w, h} of the
    // current target; the render states it sets are restored as the original does.
    void raster_quad(Cpu& c, u32 raster, const u32 rect[4], bool scaled, bool fast);
    // System's 2D identity announcements (game thread, draw order): the anchors of the Im2D draws that follow.
    void on_hud_group(HudGroupKind kind, u32 group_id, float canvas_x, u32 instance) override;
    void on_hud_scene(u32 node, u32 cast) override;
    u32 im3d_transform(Cpu& c, u32 pipeline, u32 pool);
    u32 im3d_render(Cpu& c, u32 pipeline, u32 stash);
    void camera_texture_copy(Cpu& c, u32 raster, u32 half_resolution);

    // ---- the screen passes (screens.cpp; r2/SITES_PRESENT_EFFECTS.md 3.2-3.7, R2_DRIVER.md 4.9) ----
    // A copy of the screen that a distortion samples next (the full-screen heat haze, a refraction element): a
    // snapshot, here, of every view the copy's rectangle covers.
    void screen_snapshot(Cpu& c, u32 raster, u32 half_resolution);
    // SaveScreen's copy of the screen: the history of every view the copy covers becomes its scene here.
    void trail_save(Cpu& c, u32 raster, u32 half_resolution);
    // DrawBlur's quad: the view's history over the scene, zoomed as the quad is.
    u32 trail_draw(Cpu& c, u32 type, u32 vertices, u32 count);
    // The heat haze's distortion meshes sampling the snapshot taken just before: the full-screen grid (laid out over
    // the view) and a refraction element's patch (at its place in the world).
    u32 shimmer_draw(Cpu& c, u32 type, u32 vertices, u32 count);
    u32 refraction_draw(Cpu& c, u32 type, u32 vertices, u32 count);
    // DrawScreenColor's multiply: the scene times a colour (the console sampled a copy of the screen for it).
    u32 screen_multiply(Cpu& c, u32 type, u32 vertices, u32 count);

    // ---- the 2D <-> 3D boundary (projection.cpp; docs/boundary-20261007/DESIGN.md) ----
    // The framing of every view's picture (render/view.h): square pixels unless set otherwise. Before the game runs.
    void set_framing(shadow::render::Framing framing) noexcept { framing_ = framing; }
    shadow::render::Framing framing() const noexcept override { return framing_; }
    // The game's world-to-screen projection for the 2D it attaches to world points (fn_8000F654's call sites: the
    // lock-on mark, the enemy gauge, Sprite3D, the 3D-anchored CSD scenes): the point at `world` (three big-endian
    // floats) on player `player`'s view, written to `out` as the game's screen coordinates - x, y in its 640 x 480
    // canvas with the viewport's origin, the forward distance - placed by render/view.h canvas_point(). Returns `out`.
    u32 world_to_screen(Cpu& c, u32 out, u32 world, u32 player);
    // An Im2D primitive the game placed at a projected point (the lock-on mark's quads): Anchor::Projected.
    u32 im2d_primitive_projected(Cpu& c, u32 type, u32 vertices, u32 count);
    // The output's width / height (the window's client area, else the captures'): what the output shows of the
    // game's screen. Any thread; 4:3 until set.
    void set_output_aspect(float aspect) noexcept;
    // The aspect the game LOGIC's on-screen test uses (the enemy AI, displayed_screen_point; gekko/gameplay_aspect.h):
    // 0 = the output's own (live play, the default); a replay passes the recording's so the AI decides as it did,
    // whatever the picture is. Any thread.
    void set_logic_aspect(float aspect) noexcept;
    float logic_aspect() const noexcept;
    // C3: a rendered packet back to the frame builder (any thread).
    void recycle_packet(shadow::render::FramePacket&& used) { builder_.recycle(std::move(used)); }
    float output_aspect() const noexcept override;
    // One edge of the screen the output shows, for a viewport rectangle at `rect` (the screen tests of the gauge's
    // gate and the 3D-anchored CSD helpers, lowered from their rect accessors): whole canvas units, outward.
    enum class ScreenEdge : u32 { Right, Left, Bottom, Top };
    u32 visible_screen_edge(Cpu& c, u32 rect, ScreenEdge edge);
    // The game's screen position of a world point for its "is it on the screen" decisions (fn_8000F57C(out, camera
    // state, world, viewport) at the sites of the Black Arms' shared on-screen test 0x8019DA1C,
    // docs/ASPECT_INVENTORY.md): x, y in the view's normalised device coordinates of the DISPLAYED picture (+-1 its
    // edges, y down as the game's), then the view-space z, written to `out`. Returns `out`.
    u32 displayed_screen_point(Cpu& c, u32 out, u32 camera_state, u32 world, u32 viewport);
    // ---- game::RwApi (NE-RW1): what the game-level owners (runtime/game: Weapon::LockonMark's draw, the lens flare's
    // builder) reach RenderWare through - this layer's render states, Im2D, projection and shown screen -------------
    SavedStates saved_states() const override;
    void state_set(Cpu& c, u32 state, u32 value) override;
    void im2d_draw(Cpu& c, u32 type, const shadow::render::ImmediateVertex* vertices, u32 count,
                   shadow::render::Anchor anchor) override;
    bool project(Cpu& c, u32 world, u32 player, Projected* out) override;
    game::RwApi::ScreenRect viewport(Cpu& c, s32 index) override;
    shadow::render::CanvasRect shown_screen(const game::RwApi::ScreenRect& r) const override;
    float presented_aspect() const override { return output_aspect(); }
    u64 frame_number() const override { return stats_.frames; }
    // The camera sync's window step (fn_8000F860(out, fov) in fn_80271F9C, lowered): the renderer camera's window is
    // the authored picture's (render/view.h authored_window) of the FOV pair at `fov`, written to `out`. Returns `out`.
    u32 camera_picture_window(Cpu& c, u32 out, u32 fov);
    // Whether a Chao::CSD widget (System's group id: the return address of its Project::Render call) is drawn where
    // the game projected a world point.
    static bool is_world_anchored_widget(u32 group_id) noexcept;

    // ---- pipelines (pipelines.cpp) ----
    u32 pipeline_atomic(Cpu& c, u32 pipeline, u32 atomic);
    u32 pipeline_sector(Cpu& c, u32 pipeline, u32 sector);
    void matfx_layers(Cpu& c, u32 fx, u32 fx_kind, u32 geometry_flags, float cutoff, shadow::render::Material* material);
    // ---- the effect layer switches (layers.cpp): the layer the next draws belong to ----
    // `known`: the site's own layer (a screen pass); else the guest call chain's. None while no switch is set.
    // `land`: a world sector (a land class's draw names its land layer).
    void tag_draws(Cpu& c, shadow::render::EffectLayer known = shadow::render::EffectLayer::None, bool land = false);
    shadow::render::EffectLayer layer_of_caller(Cpu& c);
    // A render state set by the level recipe 0x80046D80: its render-class flags (r31 there) name the land layer.
    void note_land_class(Cpu& c) noexcept;
    // ---- the glare (glare.cpp): PreGlare's and PostGlare's copies ----
    void glare_sources_begin(Cpu& c, u32 raster);
    void glare_composite(Cpu& c, u32 raster);
    // RwResourcesFreeResEntry at a geometry's destruction: the instance is the native mesh.
    u32 geometry_instance_release(Cpu& c, u32 entry);

    struct Stats {
        u64 frames = 0;
        u64 frames_dropped = 0;       // the render thread had not taken the previous ones
        u64 state_sets = 0, state_gets = 0;
        u64 cameras = 0, clears = 0, clears_ignored = 0, raster_clears = 0, raster_clears_ignored = 0;
        u64 im2d_primitives = 0, im3d_primitives = 0, copies = 0;
        u64 atomics = 0, sectors = 0, sector_draws = 0, sector_meshes_built = 0, sectors_skipped = 0;
        u64 dissolving_draws = 0;  // atomics drawn mid fade-in
        u64 far_proxy_draws = 0;   // far-object proxy meshes drawn
        u64 model_draws = 0, models_built = 0, models_rebuilt = 0, model_failures = 0, extra_passes = 0;
        u64 skinned_draws = 0, skins_unsupported = 0, particles_skipped = 0, geometry_releases = 0;
        // Particle systems: batches drawn, their particles, executions of a configuration the native path does not
        // draw (counted in particles_skipped too).
        u64 particle_batches = 0, particles = 0, particles_unsupported = 0;
        // The glare: windows opened (PreGlare's copy), composites (PostGlare's), copies that did not match the glare
        // object or came without the other, windows still open at the end of a frame.
        u64 glare_windows = 0, glares = 0, glare_mismatches = 0, glares_unclosed = 0;
        // Im3D: draws described, renders not drawn (no pass, no pipeline, an index out of range).
        u64 im3d_draws = 0, im3d_skipped = 0;
        // Im2D: canvas draws described (by anchor: left, centre, right, fill), draws outside a screen view (the
        // screen passes' own quads into camera textures), composites textured with a camera texture (the screen
        // passes' results, not native yet), refused primitives (points, none), unreadable vertices or indices, draws
        // the layout found off the canvas, draws made while GX fog was armed (canvas draws are not fogged).
        u64 im2d_draws = 0, im2d_outside_views = 0, im2d_effect_composites = 0, im2d_refused = 0, im2d_unreadable = 0;
        u64 im2d_off_canvas = 0, im2d_fog_armed = 0;
        u64 im2d_anchors[shadow::render::kAnchorCount] = {0, 0, 0, 0, 0, 0, 0};
        // The 2D layout's tiled strips (runtime/lift/ui/tiled_strip.h): strips continued, the copies of their end
        // tiles drawn, and runs of tiles that did not qualify (gaps) or whose pass had closed before they ended.
        u64 strips_continued = 0, strip_copies = 0, strips_rejected = 0, strips_unfinished = 0;
        // Split screen: overlay views that draw over the players' viewports (render/frame.h ViewRecord::over_views),
        // and canvas draws laid out in a player's viewport region (render/view.h CanvasRegion).
        u64 views_over_views = 0, im2d_in_regions = 0;
        // The world-to-screen projection: calls, calls for a player without a camera, points on the eye's plane, and
        // calls whose window differed from the renderer camera's of the same viewport (they must be the same camera).
        u64 projections = 0, projections_without_camera = 0, projections_degenerate = 0;
        u64 projection_window_mismatches = 0;
        // The screen tests answered with the displayed screen (the lock-on marks: game::GameOwners), and the on-screen
        // points of the game's own on-screen decisions (displayed_screen_point).
        u64 screen_edge_reads = 0;
        // ... of them on the displayed screen, and those whose verdict differs from the game's own 4:3 window's (on the
        // screen only on the displayed picture: gained; only in the game's window: lost).
        u64 displayed_screen_points = 0, displayed_screen_points_on = 0;
        u64 displayed_screen_points_gained = 0, displayed_screen_points_lost = 0;
        u64 camera_windows = 0;  // the camera sync's windows set (camera_picture_window)
        // The screen passes: snapshots taken, distortion draws (heat haze, refraction), history saves and trail
        // draws, screen multiplies, and their draws not described (no screen view, no snapshot of the view yet,
        // unreadable vertices, a refused primitive).
        u64 screen_snapshots = 0, distortion_draws = 0, trail_saves = 0, trail_draws = 0, screen_multiplies = 0;
        u64 screen_pass_skipped = 0;
        // Center-placed grids spanning their view's canvas width laid out over the view (the full-screen refraction).
        u64 screen_covers_widened = 0;
        // ... by reason (ScreenSkip), and the frames the first and the last of them were in.
        u64 screen_skips[6] = {0, 0, 0, 0, 0, 0};
        u64 screen_skip_frames[2] = {~u64{0}, 0};
        // Screen copies made while no camera bracket of a screen raster was in force (the frame buffer copied as it
        // is - e.g. the heat haze in the frames after a respawn, before any bracket of the frame).
        u64 screen_copies_unbracketed = 0;
        // The frames they were first and last drawn in (the frame count before the first: none).
        u64 distortion_frames[2] = {~u64{0}, 0}, trail_frames[2] = {~u64{0}, 0}, multiply_frames[2] = {~u64{0}, 0};
        // Movies: frame blits drawn, pictures (the raster's texels changed: its texture rebuilt), and the frames.
        u64 movie_draws = 0, movie_pictures = 0;
        u64 mode_switch_blanks = 0;  // frames shown black at a video-mode switch (the raster changed size)
        u64 display_blanks = 0;      // frames shown black because the game had VISetBlack(TRUE) in force
        u64 event_framed = 0;        // frames of an in-engine event, shown in a 4:3 box
        u64 movie_frames[2] = {~u64{0}, 0};
        // Why skins were not drawn: no RpSkin, no hierarchy, a hierarchy without matrices or node frames, bones out
        // of range.
        u64 skins_without_skin = 0, skins_without_hierarchy = 0, skins_without_matrices = 0, skins_bad_bones = 0;
        u32 last_hierarchy_flags = 0, last_hierarchy_nodes = 0, last_skin_bones = 0;
        // Bone frames whose hierarchy was still dirty at the draw (their LTMs computed read-only from the modelling
        // matrices; RenderWare writes the same LTMs at its next synchronisation).
        u64 dirty_bone_frames = 0, bone_frames = 0;
        // The deforming-mesh audit (2026-10-07): skins built with float weights whose sum is not 1 (the console
        // quantises them to u8 / 128 summing to exactly 1 at load, 0x8044F458), and their vertices; a geometry's
        // mesh rebuilt (a lock) after it was already drawn in the same frame - the earlier draw's handle is gone;
        // draws of geometry with more than one morph target (the RpMorph interpolator is not applied); skins posed
        // from global / local-space hierarchy matrices instead of node frames.
        u64 skins_unnormalised = 0, skin_vertices_unnormalised = 0;
        u64 meshes_replaced_in_frame = 0, morph_target_draws = 0, skins_from_matrices = 0;
        // Delta morphs drawn natively: meshes built with their targets, draws carrying weights, and geometry whose
        // targets the native path does not take (prelit / UV deltas, more than kMaxMorphTargets: rebuilt per lock).
        u64 dmorph_meshes = 0, dmorph_draws = 0, dmorph_unsupported = 0;
        u64 sector_import_failures = 0, draws_without_view = 0, draws_without_texture = 0;
        u64 light_slot_vertices_moved = 0, light_slots_added = 0;  // assets/light_slots.h
        u64 textures_built = 0, texture_failures = 0, textures_released = 0, meshes_released = 0;
        u64 textures_overridden = 0;  // drawn from gekko/texture_overrides.h (the sharp button sheets)
        u64 camera_mismatches = 0;    // a later begin of the same view moved the camera
        u64 passes = 0;               // passes opened (a bracket's first draw, unless it continues the open pass)
        u64 view_offsets = 0;         // cameras with a non-zero view offset (not applied yet)
        float handedness = 0.0f;      // (right x up) . at of the last camera: > 0 means right points to the left
    };
    // NE-RW2: the immediate modes' counters come from `imm_` (same fields, same meaning).
    Stats stats() const noexcept;
    // Frames handed over so far: the next frame's packet tick (game thread).
    u64 frames() const noexcept { return stats_.frames; }
    // A savestate was restored (game thread, at a boundary - no lowered site is running): every native resource this
    // layer made is given back and the frame being described is dropped. The restored RenderWare objects' words name
    // handles of the timeline that was replaced; none of them is owned any more, so each is rebuilt from the restored
    // objects at its first use - what a process that started from the savestate does.
    void on_state_loaded();
    // The driver's part of a savestate (the lifted renderer's block, gekko/savestate.h): RenderWare's render-state
    // values as the lowered driver keeps them natively, and the 2D layout's remembered anchors. What RenderWare can
    // observe of the driver outside guest memory; the frame being described is not part of it (a savestate is taken
    // between traversals - `frame_open` says whether one was, for the record).
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);
    bool frame_open() const noexcept { return frame_open_; }
    // The guest words this layer keeps its native handles in (saved as 0: a savestate holds no process's handles).
    void own_words(std::vector<u32>& out) const;
    // Diagnostics: the screen passes' Im2D composites and the camera-texture copies not drawn yet, by call site (the
    // site's return address).
    const std::map<u32, u64>& effect_composite_sites() const noexcept { return effect_composite_sites_; }
    const std::map<u32, u64>& copy_sites() const noexcept { return copy_sites_; }
    // Why a screen pass's copy or draw was not described: a draw with no screen-raster camera bracket in force (none
    // since the frame began, or the latest one targets a camera texture), a draw whose view has no snapshot this
    // frame, unreadable vertices or a refused primitive, no pass could open, the distortions switched off
    // (SHADOW_R2_NO_DISTORTION), a copy whose rectangle covers no view of the frame. Counted by reason and by call
    // site (the site's return address, keyed with the reason above bit 32).
    enum class ScreenSkip : u32 { NoBracket = 0, NoSnapshot = 1, Unreadable = 2, NoPass = 3, Disabled = 4, NoView = 5 };
    const std::map<u64, u64>& screen_skip_sites() const noexcept { return screen_skip_sites_; }

private:
    // The current frame as it is being described.
    struct View {
        float x = 0, y = 0, width = 0, height = 0;  // the camera raster rectangle (authoring pixels)
        float screen_width = 0, screen_height = 0;  // its top-level raster: the whole picture
        shadow::render::ViewId id = 0;
        shadow::render::Camera camera;
        // The view's latest snapshot this frame (a screen copy covered it): what its distortions sample.
        bool has_snapshot = false;
        shadow::render::SnapshotId snapshot = 0;
        // It draws over the views opened before it this frame (the full-screen overlay of split screen), and the
        // players' viewports it covers - the regions its HUD is laid out in (canvas units of this view, and the
        // frame packet's region ids).
        bool over_views = false;
        std::vector<shadow::ui::HudLayout::Region> hud_regions;
        std::vector<std::uint32_t> region_ids;
    };
    struct Owned {
        u32 handle = 0;
        u32 word = 0;    // the guest word that holds it
        u32 serial = 0;  // a geometry's mesh-header serial at build
        u32 skin = 0;    // a geometry's skin at build (its weights are in the mesh)
        u64 drawn_frame = ~u64{0};  // the frame the mesh was last handed to a draw
        u32 dmorph = 0;  // a geometry's delta-morph data at build (its targets are in the mesh; its locks are morphs)
    };
    // The deforming-mesh audit's first reports, one per (issue, geometry).
    std::set<u64> deform_reported_;
    std::set<u32> bump_reported_;  // MatFX bump-environment data already logged (one line each)
    void deform_report(u32 issue, u32 geometry, const char* text);
    // True when deform_report(issue, geometry, ...) would print: callers build the text only then. The skin and
    // morph sites run per draw, and formatting every draw's text only for the set to discard it was the game
    // thread's one per-frame printf (2026-11-04: ~0.013 ms/frame).
    bool deform_unreported(u32 issue, u32 geometry) const {
        return deform_reported_.size() < 256u && !deform_reported_.contains((u64{issue} << 32) | geometry);
    }

    // FOGCOLOR / FOGTYPE: GX fog armed with the current camera's planes.
    void rearm_fog(Cpu& c);
    // An Im2D primitive (`indices` 0: the vertices in order) on the current view's canvas; `anchor` (null: the 2D
    // layout policy decides) places it. `native_vertices`: the vertices as values, made by a game owner (NE-RW1)
    // instead of read at `vertices`.
    u32 im2d(Cpu& c, u32 type, u32 vertices, u32 count, u32 indices, u32 index_count,
             const shadow::render::Anchor* anchor = nullptr,
             const shadow::render::ImmediateVertex* native_vertices = nullptr);
    // The boundary's projection (projection.cpp): a world point on a player's view, in the game's screen coordinates.
    struct ProjectedPoint {
        float x = 0.0f, y = 0.0f, depth = 0.0f;
        s32 viewport = 0;
    };
    bool project_point(Cpu& c, u32 world, u32 player, ProjectedPoint* out);
    // A viewport's rectangle of the game's 640 x 480 screen, and the part of the screen the output shows of it.
    struct ScreenRect {
        float x = 0.0f, y = 0.0f, w = 640.0f, h = 480.0f;
    };
    ScreenRect viewport_rect(Cpu& c, s32 viewport);
    // The viewport's FOV scale (fn_802731A4): its table pair times the global factors, times 1.25 off the whole screen.
    void viewport_fov_scale(Cpu& c, s32 viewport, float* scale_x, float* scale_y);
    shadow::render::CanvasRect visible_screen(const ScreenRect& r) const;
    // The Im texture's bind for an Im2D primitive: its raster (0: untextured), whether that is a camera texture; the
    // alpha mode re-selected from the raster as the driver's bind does.
    u32 im2d_bind(Cpu& c, bool* camera_texture);
    // A screen pass's Im2D primitive as a canvas draw sampling `source` (none: untextured), placed by `anchor`; a
    // screen image's texture coordinates become the canvas points they address. `blend` overrides the recipe's
    // (multiply: the scene times the vertex colour).
    u32 screen_pass_draw(Cpu& c, u32 type, u32 vertices, u32 count, shadow::render::ImageSource source,
                         shadow::render::Anchor anchor, bool multiply, const char* what);
    // A screen pass's copy or draw left out (counted by reason and site).
    void skip_screen_pass(Cpu& c, ScreenSkip why);
    // The frame's views a copy into `raster` reads: the copy's source is the raster's own rectangle of the frame
    // buffer (its offset and size - a parent raster is the whole picture), r2/SITES_PRESENT_EFFECTS.md 2.1.
    template <typename F>
    std::size_t for_each_view_copied(Cpu& c, u32 raster, u32 half_resolution, F&& f);

    // Guest access, values only.
    u32 im_texture(Cpu& c);
    u32 raster_ext(Cpu& c, u32 raster);
    u32 texture_ext(Cpu& c, u32 texture);
    float read_f32(Cpu& c, u32 address);
    void write_f32(Cpu& c, u32 address, float value);

    void open_frame();
    View* view_for(Cpu& c, u32 camera);
    // The pass a draw of the current camera bracket goes to: opened at the bracket's first draw with the bracket's
    // camera, continued while later brackets keep the view and the camera. False outside any screen bracket.
    bool ensure_pass() override;
    // The strip being recognised has ended: continue it to the view's edges if it is one (immediate.cpp).
    void strip_flush();
    // ---- shadow::rwim::Host / LayoutPolicy (NE-RW2) ----
    bool pass_open() const override { return pass_open_; }
    std::size_t current_view() const override { return bracket_view_; }
    bool view(std::size_t index, shadow::rwim::Host::View* out) const override;
    std::uint64_t frame() const override { return stats_.frames; }
    shadow::rwim::Placement place(const shadow::rwim::Bounds& bounds, bool textured, bool shows) override;
    shadow::render::Camera camera_of(Cpu& c, u32 camera);
    shadow::render::TextureHandle texture_of(Cpu& c, u32 rw_texture, bool* has_alpha);
    shadow::render::TextureHandle texture_lookup(Cpu& c, u32 rw_texture, bool* has_alpha);
    shadow::render::Recipe recipe() const;
    float alpha_cutoff() const;
    shadow::render::Sampler sampler_of(u32 filter_addressing) const;
    // `mips`: generate the mip chain (not for a movie picture, which is only ever magnified).
    bool build_texture(Cpu& c, u32 raster, u32 ext, shadow::render::TextureHandle* out, bool mips = true);
    shadow::render::MeshHandle sector_mesh(Cpu& c, u32 sector);
    // `skin`: the geometry's RpSkin when a skin pipeline draws it (its weights go into the mesh), else 0.
    // `dmorph`: receives the geometry's delta-morph data (rpDMorphGeometryData) when the mesh carries its targets -
    // its draws then carry the weights (dmorph_weights) - else 0.
    shadow::render::MeshHandle geometry_mesh(Cpu& c, u32 geometry, u32 flags, u32 mesh_header, u32 mesh_count,
                                             bool locked, u32 skin, u32* dmorph);
    // The geometry's delta-morph data when the native path draws its targets (0: none, or targets it does not take).
    u32 dmorph_data(Cpu& c, u32 geometry);
    // Builds the mesh's delta targets from the geometry's DMorph data and takes the applied weights back out of its
    // arrays (the game blends the targets into them in place): `mesh` then holds the authored base shape.
    bool dmorph_targets(Cpu& c, u32 dmorph, shadow::assets::MeshData* mesh);
    // A skinned atomic's bone palette for this frame (world space); false when the hierarchy is not one the native
    // path draws (counted).
    bool skin_palette(Cpu& c, u32 atomic, u32 skin, shadow::render::BoneRange* out);
    // A PTank atomic's particles as one batch of the open pass, drawn with `material` and `recipe` (counted).
    void particle_batch(Cpu& c, u32 atomic, const shadow::render::Material& material,
                        const shadow::render::Recipe& recipe);
    // The lights the current world gives an atomic now (ambient sum, directional lights with flags & 1).
    shadow::render::LightSetId light_set(Cpu& c, bool* lit, bool* ambient, shadow::render::LightSet* out = nullptr);
    // The lights a far proxy's part gets where the game draws it: its display layer's (none unless the layer is
    // lit) with the light manager's two lights set to its light group (pipelines.cpp).
    shadow::render::LightSetId layer_lights(Cpu& c, s32 layer, s32 light_group, bool* lit, bool* ambient,
                                            shadow::render::LightSet* out = nullptr);
    // The render states the game sets for a display layer's pass (0x80046D80, pipelines.cpp), over the in-stage
    // camera's fog: what a far proxy's part draws with. `cutoff` the layer's alpha compare as a cut-off.
    shadow::render::Recipe layer_recipe(Cpu& c, s32 layer, float* cutoff) const;
    // Measuring (SHADOW_FAR_VALIDATE): every game draw inside a layer's world against layer_recipe.
    u64 layer_state_checks_ = 0, layer_state_mismatches_ = 0;
    std::unordered_set<u32> opaque_level_geometries_;  // the geometries of FarObjects::kOpaqueLevel proxies
    // The last frame a FOGCOLOR / FOGTYPE change re-armed GX fog inside a CameraRender level, and its planes.
    u64 level_fog_frame_ = ~u64{0};
    float level_fog_start_ = 0.0f, level_fog_end_ = 1.0f;
    // Measuring (SHADOW_FAR_VALIDATE): the lights each geometry the game drew this frame got, against the proxies'.
    struct GameLights {
        shadow::render::LightSet set;
        bool lit = false, ambient = false;
    };
    std::unordered_map<u32, GameLights> game_lights_;
    u64 light_checks_ = 0, light_mismatches_ = 0, light_checks_lit_ = 0;
    // Measuring: the lights the game gave a skinned geometry this frame (world, light group) - an enemy stand-in's.
    struct SkinLights {
        GameLights lights;
        u32 world = 0, group = 0;
    };
    std::unordered_map<u32, SkinLights> game_skin_lights_;
    u64 skin_light_checks_ = 0, skin_light_mismatches_ = 0;
    // Shared UV animations (docs/DISTANT_RENDERING.md 6b): a model holder registers a part's .UVA with
    // TextureUVAnimManager, which advances the library material's interpolators only while an instance is registered
    // (a spawn resumes them where they stood). Those materials - proxies' and the game's own draws alike - are drawn
    // at one native clock per animation instead, render only: the game's interpolators run untouched.
    enum class UvTime { Shared, Game };  // the shared native clock / the interpolator's own time (measuring)
    // A material's uv matrices at `when`; false when it has no RpUVAnim this evaluates.
    bool uv_animation(Cpu& c, u32 material, u32 geometry, UvTime when, float (&base)[2][3], float (&dual)[2][3],
                      bool* has_dual);
    // Whether `geometry` belongs to a registered model: its clump is (or was this stage) in TextureUVAnimManager's map.
    bool uv_shared_geometry(Cpu& c, u32 geometry);
    // The geometries of the clumps registered right now (the map walk).
    void uv_registered_now(Cpu& c, std::set<u32>* geometries);
    // Once a frame: a registered clock moves by its interpolator's own step (the game's speed, and any time the game
    // sets itself); an unregistered one (only proxies show it) by the time TextureUVAnimManagerTask was given since
    // its last step (gekko/uv_anim_time.h) - as it would run if registered, standing whenever the game stops that task,
    // also over frames that drew none of it. Under --legacy-scheduler (no manager time): the frame's game time, unless
    // registered animations stood this frame (paused).
    void advance_uv_clocks(Cpu& c);
    // Once a frame (submit_frame): the stage's archive at 0x805F03C0 changed (a stage loaded, or left) - every clock and
    // kept geometry is dropped, so a reload at the same addresses never revives the last stage's clocks. A pause-menu
    // restart keeps the archive and the libraries with it (measured 2026-11-05), so their clocks carry on.
    void uv_stage_check(Cpu& c);
    // The shared clock for a material whose geometry is shared: overrides the uv matrices `m` from matfx_layers.
    void shared_uv(Cpu& c, u32 material, u32 geometry, u32 fx_kind, shadow::render::Material* m, bool proxy);
    struct UvClock {
        u32 interpolator = 0, geometry = 0;
        float duration = 0.0f;
        double time = 0.0;        // the shared time, in [0, duration)
        float last_game = 0.0f;   // the interpolator's time at the last step, and the ticks then
        u64 last_ticks = 0;
        double last_manager = 0.0;  // TextureUVAnimManagerTask's time at the last step
        double offset = 0.0;      // measuring: shared time - the game's while registered
        bool offset_known = false;
    };
    std::unordered_map<u64, UvClock> uv_clocks_;  // by animation << 32 | interpolator
    std::set<u32> uv_shared_geometries_, uv_registered_;
    u64 uv_epoch_ = ~0ull, uv_advanced_frame_ = ~0ull;
    u32 uv_stage_archive_ = 0;  // the stage archive last seen (uv_stage_check)
    u64 uv_stage_resets_ = 0;
    double uv_manager_seen_ = 0.0;  // TextureUVAnimManagerTask's time at the last advance_uv_clocks
    bool uv_shared_enabled_ = std::getenv("SHADOW_UV_SHARED_CLOCK") == nullptr ||
                              std::getenv("SHADOW_UV_SHARED_CLOCK")[0] != '0';
    // Measuring: evaluation against the game's matrix at the game's time; while registered, the shared time's offset
    // from the game's (constant = same speed, no restart); the shared time given to proxies and to the game's own draws
    // within one frame; frames the game paused the registered animations.
    u64 uv_checks_ = 0, uv_mismatches_ = 0, uv_evaluations_ = 0, uv_eval_ns_ = 0;
    u64 uv_registered_steps_ = 0, uv_offset_breaks_ = 0, uv_paused_frames_ = 0;
    std::unordered_map<u32, float> uv_frame_time_;  // by interpolator: the shared time this frame
    u64 uv_frame_ = ~0ull, uv_both_frames_ = 0, uv_split_frames_ = 0;
    // Measuring (SHADOW_SKIN_HASH=1): a hash of every skin palette a frame builds, printed per frame.
    bool skin_hash_ = std::getenv("SHADOW_SKIN_HASH") != nullptr;
    bool skin_hash_detail_ = std::getenv("SHADOW_SKIN_HASH") != nullptr && std::getenv("SHADOW_SKIN_HASH")[0] == '2';
    u64 frame_skin_hash_ = 0xCBF29CE484222325ull;
    u32 frame_skins_ = 0;
    shadow::render::Float3x4 frame_matrix(Cpu& c, u32 matrix);
    // A frame's LTM as RwFrameGetLTM returns it, without its synchronisation's writes: the stored LTM when the
    // hierarchy is in sync, else the product of the modelling matrices up to the root.
    shadow::render::Float3x4 frame_ltm(Cpu& c, u32 frame);
    void reconcile(Cpu& c);

    shadow::render::Renderer& renderer_;
    shadow::render::PacketQueue& frames_;
    RenderStates states_;
    u32 current_camera_ = 0;  // the camera between begin and end (RenderWare's own pointer, a value)
    shadow::render::FrameBuilder builder_;
    bool frame_open_ = false;
    bool pass_open_ = false;
    std::size_t pass_view_ = 0;
    shadow::render::PassKind pass_kind_ = shadow::render::PassKind::Scene;
    shadow::render::Camera pass_camera_;
    // Between PreGlare's and PostGlare's copies the draws are the glare sources of `glare_view_`.
    bool glare_window_ = false;
    std::size_t glare_view_ = 0;
    // The latest camera bracket on a screen raster (it stays in force after its end, as the GX state did).
    bool bracket_valid_ = false;
    std::size_t bracket_view_ = 0;
    shadow::render::Camera bracket_camera_;
    std::vector<View> views_;
    // The first use of each new 2D path, reported once (stderr): proof that the running build takes it.
    bool reported_over_views_ = false, reported_regions_ = false;
    shadow::render::Float4 background_{0, 0, 0, 1};
    bool background_set_ = false;
    std::vector<Owned> textures_;
    // C2 (HW_REPORT S): textures_ indexed by word (the handle it owns), rebuilt when textures_ changed - texture_of
    // ran a linear scan of every owned texture per draw (about 0.13 s of the game thread on the Lava replay).
    std::unordered_map<u32, u32> texture_index_;
    bool texture_index_dirty_ = true;
    std::vector<Owned> meshes_;
    std::vector<shadow::render::Particle> particle_records_;  // one batch's records, reused
    std::vector<shadow::render::ImmediateVertex> immediate_vertices_;  // one Im3D render's geometry, reused
    std::vector<std::uint32_t> immediate_indices_;
    std::vector<std::uint16_t> immediate_index_values_;  // an Im2D / Im3D draw's indices in host order, reused
    // The 2D layout policy (runtime/src/gekko/native_hud.cpp; docs/2D_LAYOUT_ARCHITECTURE.md): System's
    // announcements open and close groups and casts, each Im2D draw gets its anchor; one frame per present.
    shadow::ui::HudLayout hud_;
    // NE-RW2: RenderWare's immediate modes (runtime/lift/rwim), drawing into `builder_`.
    shadow::rwim::Immediate imm_{builder_, *this, *this};
    std::map<u32, u64> effect_composite_sites_;
    std::map<u32, u64> copy_sites_;
    std::map<u64, u64> screen_skip_sites_;
    // A movie raster's texel fingerprint at its last texture build (the decoder rewrites the raster in place).
    std::map<u32, u64> movie_texels_;
    // Diagnostics of the screen passes left out: this frame's camera brackets on screen rasters and on other rasters,
    // and the raster type of the latest bracket (0xFF: none this frame, 0xFE: no raster).
    u32 frame_screen_brackets_ = 0, frame_other_brackets_ = 0, last_bracket_type_ = 0xFFu;
    std::function<void(const shadow::render::FramePacket&)> frame_observer_;
    AppearFade appear_;  // the fade-in of models that appear inside the view
    FarObjects far_;     // the far-object proxies
    MaterialProbe material_probe_;  // diagnostics: GEKKO_MATERIAL_PROBE
    float far_pixel_height_ = 2160.0f;
    bool far_drawn_this_frame_ = false;
    // The render recipe of the last plain world model the game drew through the in-stage camera this frame (its fog,
    // depth and blend states): the proxies draw like it.
    bool world_recipe_valid_ = false;
    shadow::render::Recipe world_recipe_;
    float world_cutoff_ = 0.0f;
    void draw_far_proxies(Cpu& c);
    // A skinned atomic (an enemy's stand-in) takes `bones` palette matrices; any other `world`.
    void draw_proxy_atomic(Cpu& c, u32 atomic, const shadow::render::Float3x4& world, shadow::render::LightSetId lights,
                           bool lit, bool ambient, const shadow::render::Recipe& recipe, float cutoff,
                           const shadow::render::Float3x4* palette = nullptr, u32 bones = 0, bool uva = false);
    std::vector<FarObjects::Proxy> far_proxies_;
    Stats stats_;
    std::uint64_t trace_frame_ = ~std::uint64_t{0};
    // The previous frame's top-level raster (0: none yet): a change is a video-mode switch (submit_frame).
    float last_screen_width_ = 0.0f, last_screen_height_ = 0.0f;
    std::function<bool()> display_blank_;
    bool no_distortion_ = false;  // diagnostics: SHADOW_R2_NO_DISTORTION
    bool tag_layers_ = false;     // this frame's draws carry their effect layer (an effect layer switch is set)
    u32 land_flags_ = 0;          // the render-class flags the level recipe last applied (note_land_class)
    bool game_window_screen_test_ = false;  // diagnostics: SHADOW_GAME_WINDOW_SCREEN_TEST
    shadow::render::Framing framing_ = shadow::render::Framing::SquarePixels;
    // The open Chao::CSD widget is world-anchored: its Im2D draws are Projected.
    bool projected_widget_ = false;
    // Diagnostics: SHADOW_BOUNDARY_TRACE=<n> prints the first n projections' values (stderr).
    std::uint64_t trace_projections_ = 0;
    std::atomic<u32> output_aspect_bits_{0x3FAAAAABu};  // binary32 width / height
    std::atomic<u32> logic_aspect_bits_{0u};            // binary32; 0: the output's (set_logic_aspect)
    std::atomic<bool> event_framed_{false};             // sample_event_framing
};

// The device surface's native owners (rwR1; extract/native-owners-r2.txt): the lowered scaffold functions, entered by address.
void install_device_surface_owners(Cpu& cpu);
// The display swap (ShowRaster, the retrace callbacks, GcnSystem; docs/agents/vi.md "Step 12") with the scaffold's lowered frame submit / state reset.
void install_display_owners(Cpu& cpu);

// The raster-level clear as a function the native raster owners call (rw_raster::set_raster_clear): rw_driver.cpp.
u32 rw_raster_clear(Cpu& c, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint32_t rgba, bool colour,
                    bool depth);

}  // namespace gekko::scaffold
