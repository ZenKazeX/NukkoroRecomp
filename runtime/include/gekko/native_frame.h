#pragma once

// The frame packet between the recompiled game and the native renderer.
//
// The capture layer (gekko/rw_capture.h) observes the RenderWare GameCube
// driver at its call boundaries and appends one typed command per semantic
// event, in guest execution order, into a FrameSegment. A segment is closed at
// a present (or at a quiesce point) and handed to the renderer thread through
// SegmentQueue. See docs/native-runtime-2026-09-16/ARCHITECTURE.md.
//
// Contract, in one place:
//   * Everything a command needs travels inside the segment. The renderer
//     thread never reads guest memory, so nothing it does can race the guest.
//   * Commands are plain data: a header naming the opcode and the record size,
//     then a trivially copyable struct. Variable data (vertices, texels,
//     instance geometry) lives in the payload arena and is referenced by
//     offset, so one segment is two contiguous allocations that are recycled.
//   * State commands carry the VALUES the driver handed to the GX state API
//     (blend factors, depth function, matrices, colours). They are not register
//     bitfields and nothing here interprets TEV register state: the combiner a
//     draw uses is chosen from RenderWare semantics by the capture (MaterialProgram).
//   * The order of commands is the order the guest issued them. State set after
//     a draw never affects it.

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

#include "gekko/types.h"
#include "native_memory.h"
#include "native_render_vertex.h"
#include <stdexcept>

namespace gekko::native {

using f32 = float;

enum class Op : u16 {
    // ---- transform state ---------------------------------------------------
    Viewport = 1,        // Viewport
    Scissor,             // Scissor
    ScissorOffset,       // ScissorOffset
    Projection,          // Projection
    PositionMatrix,      // MatrixLoad (12 values)
    NormalMatrix,        // MatrixLoad (9 values)
    CurrentMatrix,       // Scalar
    TextureMatrix,       // MatrixLoad (8 or 12 values)
    // ---- raster state ------------------------------------------------------
    Blend,               // Blend
    DepthMode,           // DepthMode
    DepthBeforeTexture,  // Scalar
    AlphaCompare,        // AlphaCompare
    Cull,                // Scalar (GX API value: 0 none, 1 front, 2 back, 3 all)
    Fog,                 // Fog
    ColorUpdate,         // Scalar
    AlphaUpdate,         // Scalar
    DestinationAlpha,    // Scalar pair
    // ---- lighting / colour -------------------------------------------------
    ChannelCount,        // Scalar
    ChannelControl,      // ChannelControl
    MaterialColor,       // ChannelColor
    AmbientColor,        // ChannelColor
    Light,               // Light
    TexGenCount,         // Scalar
    TexGen,              // TexGen
    TevRegisterColor,    // TevColor
    TevKonstColor,       // TevColor
    // ---- textures ------------------------------------------------------------
    BindTexture,         // BindTexture
    TextureContent,      // TextureContent (+ payload)
    ReleaseTexture,      // ResourceKey
    // ---- geometry resources --------------------------------------------------
    GeometryContent,     // GeometryContent (+ payload)
    GeometryDynamic,     // GeometryDynamic (+ payload)
    ReleaseGeometry,     // ResourceKey
    // ---- draws and frame structure --------------------------------------------
    DrawImmediate,       // DrawImmediate (+ payload)
    DrawMesh,            // DrawMesh
    Clear,               // Clear
    CopyTexture,         // CopyTexture
    CameraBegin,         // Camera
    CameraEnd,           // Camera
    Present,             // Present
    ResetState,          // (empty) savestate load: every cached state is unknown
    HudGroup,            // HudGroup: the guest announced a 2D group (widescreen layout)
    HudScene,            // HudScene: the authored CSD cast now drawing
    GlareWindow,         // GlareWindow: a bloom pass boundary and its live parameters
    Count
};

const char* op_name(Op op) noexcept;

struct CommandHeader {
    Op op = Op::Count;
    u16 size = 0;  // whole record including this header, multiple of 8
    u32 sequence = 0;  // per-segment ordinal, for diagnostics and dedupe proofs
};

struct PayloadRef {
    std::size_t offset = 0;
    std::size_t size = 0;
};

// ---- state records ----------------------------------------------------------

struct Scalar {
    u32 value = 0;
    u32 value2 = 0;
};

struct Viewport {
    f32 x = 0, y = 0, width = 0, height = 0, near_z = 0, far_z = 1;
    s32 field = -1;  // -1 = GXSetViewport, 0/1 = GXSetViewportJitter field
};

struct Scissor {
    s32 x = 0, y = 0, width = 0, height = 0;
};

struct ScissorOffset {
    s32 x = 0, y = 0;
};

// GXSetProjectionv block: type (0 perspective, 1 orthographic) and the six
// coefficients exactly as the guest programs XF 0x1020.
struct Projection {
    u32 type = 0;
    std::array<f32, 6> p{};
};

struct MatrixLoad {
    u32 id = 0;     // GX matrix slot (PNMTX / TEXMTX / normal index)
    u32 type = 0;   // TextureMatrix: 0 = GX_MTX3x4, 1 = GX_MTX2x4
    std::array<f32, 12> m{};  // row-major, unused tail is zero
};

struct Blend {
    u8 type = 0;   // GX_BM_NONE 0, BLEND 1, LOGIC 2, SUBTRACT 3
    u8 source = 0;
    u8 destination = 0;
    u8 logic = 0;
};

struct DepthMode {
    u8 enable = 1;
    u8 function = 3;  // GXCompare
    u8 update = 1;
    u8 pad = 0;
};

struct AlphaCompare {
    u8 comp0 = 7, ref0 = 0, op = 0, comp1 = 7, ref1 = 0;
    u8 pad[3]{};
};

struct Fog {
    u32 type = 0;
    f32 start = 0, end = 0, near_z = 0, far_z = 0;
    std::array<u8, 4> rgba{};
};

struct ChannelControl {
    u8 channel = 0;       // GXChannelID
    u8 enable = 0;
    u8 ambient_source = 0;   // 0 REG, 1 VTX
    u8 material_source = 0;
    u8 light_mask = 0;
    u8 diffuse = 0;       // GXDiffuseFn
    u8 attenuation = 0;   // GXAttnFn
    u8 pad = 0;
};

struct ChannelColor {
    u32 channel = 0;
    std::array<u8, 4> rgba{};
};

// A GXLightObj as loaded into XF light memory.
struct Light {
    u32 mask = 0;  // GXLightID bit
    std::array<u8, 4> rgba{};
    std::array<f32, 3> cos_attenuation{};
    std::array<f32, 3> dist_attenuation{};
    std::array<f32, 3> position{};
    std::array<f32, 3> direction{};
};

struct TexGen {
    u8 coord = 0;       // GXTexCoordID
    u8 function = 0;    // GXTexGenType
    u8 source = 0;      // GXTexGenSrc
    u8 normalize = 0;
    u32 matrix = 0;     // GXTexMtx (texture matrix slot id)
    u32 post_matrix = 0;
};

struct TevColor {
    u32 index = 0;
    std::array<s16, 4> rgba{};  // TevColorS10 range; GXSetTevColor values are 0..255
};

// ---- resources --------------------------------------------------------------

// Parent raster (textures) or RwResEntry (geometry) guest address plus a host
// generation counter bumped at every content or lifetime event.
struct ResourceKey {
    u32 address = 0;
    u32 generation = 0;
};

enum TextureFlag : u32 {
    kTextureCameraTarget = 1u,   // RwRaster type 5: content is rendered, not decoded
    kTextureHasAlpha = 2u,       // raster extension alpha bit
    kTexturePalettised = 4u,
    // Payload is already linear RGBA8, decoded on the host by the native asset
    // manager from the disc's own ONE/TXD bytes (content hash verified against
    // the guest raster). gx_format is RGBA8; the renderer copies the payload
    // verbatim instead of decoding GX texels.
    kTextureNativeLinear = 8u,
};

struct BindTexture {
    u32 map = 0;              // GXTexMapID
    ResourceKey texture{};    // parent raster
    u16 width = 0, height = 0;  // the bound (sub-)raster's dimensions
    u8 wrap_s = 0, wrap_t = 0;  // GXTexWrapMode
    u8 min_filter = 0, mag_filter = 0;  // GXTexFilter
    u32 flags = 0;            // TextureFlag
    s16 offset_x = 0, offset_y = 0;  // sub-raster offset (camera targets)
};

struct TextureContent {
    ResourceKey texture{};
    u16 width = 0, height = 0;
    u32 gx_format = 0;
    u32 tlut_format = 0;
    u32 tlut_entries = 0;
    u32 flags = 0;
    PayloadRef texels{};
    PayloadRef palette{};  // big-endian u16 entries
};

// RpGameCubeVtxFmt (24 guest bytes) + the geometry flags that select attributes.
//
// `geometry` names the guest's RwResEntry - an entry in RenderWare's fixed
// 256 KiB instance arena, which evicts and re-instances the same model many
// times a second on some stages. The blob is the instance with its per-draw
// token and allocation serial zeroed and every embedded pointer made relative
// to payload byte 0, so it carries no guest address: two instances of one
// model are the same bytes. `identity` hashes those bytes and the parameters
// below; the renderer keys its persistent geometry on it and proves equality
// by comparing every byte the conversion consumed.
struct GeometryContent {
    ResourceKey geometry{};
    u64 identity = 0;       // content identity (0 = none); a lookup key, not a proof
    u32 geometry_flags = 0; // inst+0x08
    u32 world_sector = 0;   // 1 = sector (per-attribute indices, unwelded)
    u32 mesh_count = 0;     // RpMeshHeader.numMeshes: header = 12 + 8N + 8 * mesh_count bytes
    u32 dynamic = 0;        // 1 = POS/NRM arrive per draw (CPU skin / morph)
    // 1 = the display list carries a per-vertex matrix index (PNMTXIDX),
    // which makes every vertex tuple ONE BYTE wider. Independent of
    // `dynamic`: a skinned object can be both. Getting this wrong does not
    // mis-shade, it desynchronises the display-list walk and loses the
    // entire object for the rest of the session.
    u32 matrix_palette = 0;
    std::array<u8, 24> vertex_format{};
    PayloadRef blob{};
};

// Per-draw replacement of the position/normal arrays of a CPU-skinned or
// morphed resource, in guest byte order and the resource's own vertex format.
struct GeometryDynamic {
    ResourceKey geometry{};
    u32 vertex_count = 0;
    u16 position_stride = 0, normal_stride = 0;
    PayloadRef positions{};
    PayloadRef normals{};
};

// ---- draws -------------------------------------------------------------------

// Which native combiner program a draw uses. Chosen from RenderWare semantics
// (pipeline + RwGC decision-table case), never read from TEV registers.
enum class MaterialProgram : u8 {
    PassColor = 0,     // GX_PASSCLR: rasterised colour
    Modulate,          // GX_MODULATE: texture x rasterised colour
    DefaultTx2,        // RwGC 0x80466624 cases A-*: (C1 + C0*ras) then x texture
    DefaultUt1,        // RwGC cases C-*: C1 + C0*ras, untextured
    Unsupported,       // counted; rendered as a diagnostic colour
};

enum class DrawProducer : u8 {
    Unknown = 0,
    Im2D,
    Im2DTriangle,
    Im2DLine,
    Im2DIndexed,
    Im3D,
    PTank,
    RasterRender,
    DefaultMesh,
    SkinMesh,
    MatFxMesh,
    MatFxNativeShader,
};

enum ImmediateAttribute : u32 {
    kImmediatePosition = 1u,
    kImmediateNormal = 2u,
    kImmediateColor0 = 4u,
    kImmediateColor1 = 8u,
    kImmediateUv0 = 16u,
    kImmediateUv1 = 32u,
    kImmediateMatrixIndex = 64u,
};

// How a 2D draw is positioned, carried explicitly rather than inferred from
// projection state.
//
//   PureHud       authored directly in 2D; continues through HUD anchoring.
//   WorldAnchored position originates in 3D world space and has ALREADY been
//                 projected to final presentation space by the native
//                 world-anchor service. It must NOT receive a second
//                 camera-coverage transform.
enum class DrawAnchorKind : u8 { PureHud = 0, WorldAnchored = 1 };

struct DrawImmediate {
    DrawProducer producer = DrawProducer::Unknown;
    MaterialProgram program = MaterialProgram::PassColor;
    u8 primitive = 0;       // GX primitive (0 quads .. 7 points) = opcode >> 3 & 7
    // Explicit classification, stamped at capture from the guest scope. The
    // old `pad` byte: the draw always had room for this and the information
    // was simply never carried.
    DrawAnchorKind anchor = DrawAnchorKind::PureHud;
    u32 attributes = 0;     // ImmediateAttribute
    u32 vertex_count = 0;
    u32 return_address = 0; // producer LR at GXBegin
    PayloadRef vertices{};  // shadow::render::RenderVertex[vertex_count]
};

struct DrawMesh {
    DrawProducer producer = DrawProducer::Unknown;
    MaterialProgram program = MaterialProgram::PassColor;
    u16 mesh_index = 0;
    ResourceKey geometry{};
    u32 return_address = 0;
    u32 object = 0;          // RpAtomic / RpWorldSector
    u32 geometry_flags = 0;
};

struct Clear {
    s32 x = 0, y = 0, width = 0, height = 0;  // guest EFB pixels
    std::array<u8, 4> rgba{};
    u32 mode = 0;  // rwCAMERACLEARIMAGE 1 | rwCAMERACLEARZ 2
};

struct CopyTexture {
    ResourceKey target{};   // parent camera-texture raster
    u16 target_width = 0, target_height = 0;
    s16 destination_x = 0, destination_y = 0;  // texel origin inside the parent
    s32 source_x = 0, source_y = 0, source_width = 0, source_height = 0;
    u32 half_resolution = 0;
};

struct Camera {
    u32 camera = 0;
    u32 frame_buffer = 0;       // RwRaster
    s16 offset_x = 0, offset_y = 0;
    u16 width = 0, height = 0;
    u32 raster_type = 0;
};

struct HudGroup {
    u32 kind = 0;       // HudGroupKind
    u32 group_id = 0;   // draw site
    u32 instance = 0;   // guest object (CSD project) or 0
    f32 canvas_x = 0;   // glyph run alignment X
};

struct HudScene {
    u32 node = 0;
    u32 cast = 0;
};

// The guest's bloom ("glare") pass boundaries and the parameter block that
// drives them, read live at the boundary.
//
// The parameters are NOT authored stage data. The ScreenEffect constructor
// (guest 0x80431574) installs a hard-coded default set inline at
// 0x80431828-0x80431878 whenever its parameter argument is NULL, and a census
// of all 1244 .one archives on the disc finds no GLARE (paramtype 0) record at
// all - so every shipped stage runs on the executable's compiled-in constants.
// They are still read EVERY frame and never cached, because a temporal channel
// (Effect::PJS::EffectGlobalParamPJS type 0, via guest 0x80439A8C) can lerp the
// whole block per frame; it is present in code and unexercised on disc.
// See docs/effects-2026-09-21/BLOOM_SPEC.md section 3.1.
struct GlareWindow {
    u32 phase = 0;        // GlarePhase
    u32 object = 0;       // the ScreenEffect glare object G, for diagnostics
    u32 gate = 0;         // 1 = the guest will run the chain this frame
    // G+0xE0..0xE6. Master / level A / level B enables, the composite mode,
    // the level-B source select, and the two capture intensities.
    u8 enable = 0;        // +0xE0
    u8 enable_a = 0;      // +0xE1
    u8 enable_b = 0;      // +0xE2
    u8 separate = 0;      // +0xE3  1 = composite A and B separately
    u8 b_from_source = 0; // +0xE4  1 = level B captures the source, 0 = cascade from A
    u8 intensity_a = 0;   // +0xE5
    u8 intensity_b = 0;   // +0xE6
    u8 pad = 0;           // +0xE7, never written and never read by the guest
    f32 spread_a = 0.0f;  // +0xE8, tap spread in level-A texels
    f32 spread_b = 0.0f;  // +0xEC
    s32 taps_ah = 0;      // +0xF0, EXTRA taps per half: a half emits taps+1
    s32 taps_av = 0;      // +0xF4
    s32 taps_bh = 0;      // +0xF8
    s32 taps_bv = 0;      // +0xFC
};

enum GlarePhase : u32 {
    kGlarePreGlare = 0,   // guest fn_80430D64: snapshot, then clear for the source
    kGlarePostGlare = 1,  // guest fn_80430904: the ladder and the composite
};

struct Present {
    u32 xfb = 0;
    u32 frame = 0;              // guest present ordinal
    u16 xfb_width = 0, xfb_height = 0;
    u16 efb_height = 0;
    u8 vfilter[7]{};
    u8 vfilter_enabled = 0;
    u64 game_ticks = 0;  // the game clock when the frame was presented
};

// ---- the segment -------------------------------------------------------------

class FrameSegment {
public:
    FrameSegment() {
        commands_.reserve(1u << 16);
        payload_.reserve(1u << 20);
    }

    void clear() noexcept {
        commands_.clear();
        payload_.clear();
        count_ = 0;
        present_ = false;
        last_copy_sequence_ = kNoCopy;
        close_ns_ = 0;
    }

    // steady_clock time (ns since its epoch) at which the guest closed the
    // segment: for a present, when the guest produced the frame.
    u64 close_ns() const noexcept { return close_ns_; }
    void set_close_ns(u64 value) noexcept { close_ns_ = value; }

    // Sequence of the segment's last CopyTexture (kNoCopy when it has none). A
    // segment ends at a present, so a draw after it lands in the presented image
    // rather than in a camera texture (widescreen 2D layout keys on this).
    static constexpr u32 kNoCopy = 0xFFFFFFFFu;
    u32 last_copy_sequence() const noexcept { return last_copy_sequence_; }

    template <typename Record>
    void append(Op op, const Record& record) {
        static_assert(std::is_trivially_copyable_v<Record>);
        const std::size_t size = align8(sizeof(CommandHeader) + sizeof(Record));
        const std::size_t at = commands_.size();
        commands_.resize(at + size);
        CommandHeader header;
        header.op = op;
        header.size = static_cast<u16>(size);
        header.sequence = count_++;
        if (op == Op::CopyTexture) last_copy_sequence_ = header.sequence;
        std::memcpy(commands_.data() + at, &header, sizeof(header));
        std::memcpy(commands_.data() + at + sizeof(header), &record, sizeof(Record));
    }

    // A record whose bytes were captured earlier (savestate replay of state).
    void append_bytes(Op op, const u8* record, std::size_t record_size) {
        const std::size_t size = align8(sizeof(CommandHeader) + record_size);
        const std::size_t at = commands_.size();
        commands_.resize(at + size);
        CommandHeader header;
        header.op = op;
        header.size = static_cast<u16>(size);
        header.sequence = count_++;
        std::memcpy(commands_.data() + at, &header, sizeof(header));
        std::memcpy(commands_.data() + at + sizeof(header), record, record_size);
    }

    void append_empty(Op op) {
        const std::size_t at = commands_.size();
        commands_.resize(at + sizeof(CommandHeader));
        CommandHeader header;
        header.op = op;
        header.size = sizeof(CommandHeader);
        header.sequence = count_++;
        std::memcpy(commands_.data() + at, &header, sizeof(header));
    }

    // Reserves payload bytes and returns where they live. The pointer is valid
    // until the next reserve.
    PayloadRef reserve_payload(std::size_t bytes, u8*& out) {
        const std::size_t at = align8(payload_.size());
        if (at > payload_.max_size() || bytes > payload_.max_size() - at)
            throw std::length_error("native frame payload exceeds host allocation range");
        payload_.resize(at + bytes);
        out = payload_.empty() ? nullptr : payload_.data() + at;
        return PayloadRef{at, bytes};
    }
    PayloadRef add_payload(const void* data, std::size_t bytes) {
        u8* out = nullptr;
        const PayloadRef ref = reserve_payload(bytes, out);
        if (bytes != 0u) std::memcpy(out, data, bytes);
        return ref;
    }

    const auto& commands() const noexcept { return commands_; }
    const auto& payload() const noexcept { return payload_; }
    const u8* payload_at(const PayloadRef& ref) const noexcept {
        if (ref.offset > payload_.size() || ref.size > payload_.size() - ref.offset) return nullptr;
        return payload_.empty() ? nullptr : payload_.data() + ref.offset;
    }
    u32 command_count() const noexcept { return count_; }
    bool ends_frame() const noexcept { return present_; }
    void set_ends_frame(bool value) noexcept { present_ = value; }

    // Walks the command stream: fn(const CommandHeader&, const u8* record).
    template <typename Fn>
    void for_each(Fn&& fn) const {
        std::size_t at = 0;
        while (at + sizeof(CommandHeader) <= commands_.size()) {
            CommandHeader header;
            std::memcpy(&header, commands_.data() + at, sizeof(header));
            if (header.size < sizeof(CommandHeader)) break;
            fn(header, commands_.data() + at + sizeof(CommandHeader));
            at += header.size;
        }
    }

    template <typename Record>
    static Record read(const u8* record) noexcept {
        Record value;
        std::memcpy(&value, record, sizeof(Record));
        return value;
    }

private:
    static constexpr std::size_t align8(std::size_t value) noexcept {
        return (value + 7u) & ~static_cast<std::size_t>(7u);
    }
    shadow::memory::Vector<u8, shadow::memory::Domain::Renderer> commands_;
    shadow::memory::Vector<u8, shadow::memory::Domain::Renderer> payload_;
    u32 count_ = 0;
    u32 last_copy_sequence_ = kNoCopy;
    u64 close_ns_ = 0;
    bool present_ = false;
};

// Bounded single-producer (guest thread) / single-consumer (renderer thread)
// handoff over a fixed ring of segments (SegmentQueue, below).
//
// The capacity is the number of frames allowed in flight. When it is full the
// producer waits: that wait is the renderer's back-pressure, and it is measured
// and reported, never hidden behind a larger bound. Segments are never
// dropped - they carry resource lifetime commands.
// WHY THIS EXISTS.
//
// The capture latched a resource as "uploaded" the instant it EMITTED it, with
// no knowledge of whether the renderer accepted it - and the renderer has
// several ways to refuse. Content is re-sent only when the generation changes,
// and for static world geometry that is never. So ONE refusal deleted the
// object for the rest of the session. That is the mechanism that turned 17
// conversion failures into 1,538 dropped draws on Gun Fortress.
//
// This is the missing half of the contract: the renderer says what it did not
// accept, and why. The capture clears its latch and re-sends.
//
// It is deliberately a mutex, not a lock-free ring. It is drained once per
// object preparation on a path that only does work when something already
// failed, so the lock is free in practice - and docs/PIXEL_GATE.md records
// that the last cross-thread ring in this tree rewound a staging buffer under
// a live GPU read and had to be reverted. Correctness first; there is no
// measurement saying this is hot.
enum class ResourceKind : u8 { Geometry, Texture };

// Every refusal is reason-coded. "It did not arrive" is not an answer a report
// can act on; these names are the vocabulary of the DROPPED OBJECTS block.
enum class RejectReason : u8 {
    Unknown,
    ConvertFailed,    // rw_convert_instance refused the display list
    BufferCreate,     // the device refused a vertex or index buffer
    VertexStaging,    // the upload ring could not stage the vertices
    IndexStaging,     // ... or the indices
    TextureCreate,    // the device refused the texture
    TextureUpload,    // the texels could not be uploaded
    Count,
};

const char* reject_reason_name(RejectReason reason) noexcept;

struct ResourceRejection {
    u32 address = 0;
    u32 generation = 0;  // the CONTENT's generation, which is what the capture latched
    ResourceKind kind = ResourceKind::Geometry;
    RejectReason reason = RejectReason::Unknown;
};

class RejectionMailbox {
public:
    // Renderer side. Bounded: an unbounded queue here would convert a storm of
    // refusals into unbounded memory. Overflow is COUNTED, never silent - a
    // lost rejection is a permanently lost object, which is the exact failure
    // this class exists to end.
    void post(const ResourceRejection& rejection) {
        std::lock_guard lock(mutex_);
        ++posted_;
        if (pending_.size() < kMaxPending) {
            pending_.push_back(rejection);
            return;
        }
        // FULL. Do NOT drop - a dropped rejection is an object that can never
        // recover, which is the exact failure this class exists to end.
        //
        // Coalesce instead. A rejection is a REQUEST TO RE-SEND, and two
        // requests for the same resource are worth exactly as much as one, so
        // folding a storm onto the entry already queued for that resource
        // loses nothing. The consumer acts on (address, kind) and the newest
        // generation is the one worth asking for.
        //
        // This is reachable in a real scenario: the guest thread parks in
        // SegmentQueue::push while the render thread walks a stage-load
        // segment carrying thousands of content ops, so nothing drains while
        // a VRAM or ring exhaustion refuses all of them.
        for (ResourceRejection& queued : pending_) {
            if (queued.address != rejection.address || queued.kind != rejection.kind) continue;
            queued.generation = rejection.generation;
            queued.reason = rejection.reason;
            ++coalesced_;
            return;
        }
        // Full AND this resource is not already queued. Only now is anything
        // actually lost, and with kMaxPending distinct resources pending it
        // means the run is already failing catastrophically.
        ++overflow_;
    }

    // Capture side. Takes everything pending in one swap.
    void drain(std::vector<ResourceRejection>& out) {
        std::lock_guard lock(mutex_);
        if (pending_.empty()) return;
        out.insert(out.end(), pending_.begin(), pending_.end());
        drained_ += pending_.size();
        pending_.clear();
    }

    struct Stats {
        u64 posted = 0;
        u64 drained = 0;
        u64 coalesced = 0;  // folded onto an entry already queued for that resource: LOSSLESS
        u64 overflow = 0;   // rejections LOST; each one is an object gone for the session
    };
    Stats stats() const {
        std::lock_guard lock(mutex_);
        return Stats{posted_, drained_, coalesced_, overflow_};
    }

private:
    static constexpr std::size_t kMaxPending = 4096;
    mutable std::mutex mutex_;
    std::vector<ResourceRejection> pending_;
    u64 posted_ = 0, drained_ = 0, coalesced_ = 0, overflow_ = 0;
};

// The ring is the handoff's entire working set: the segment the producer is
// filling, `capacity` queued, and the one the renderer is processing. The first
// acquire allocates all of it; recycle appends and acquire takes the oldest, so
// segment k % ring always carries frame k. The frame loop therefore never
// allocates, and neither the allocation count nor which segment's arena grows
// depends on how the two threads interleave. The on-demand pool this replaced
// allocated whenever the renderer had not recycled yet - on the movie route the
// third present lands ~0.15 ms either side of that, so renderer-host memory was
// the outcome of a thread race (scratch/segment-race-probe-20260928).
class SegmentQueue {
public:
    explicit SegmentQueue(std::size_t capacity = 2) : capacity_(capacity == 0 ? 1 : capacity) {}

    // The reverse channel. Lives here because this class is already the one
    // thing both the capture (producer) and the renderer (consumer) hold.
    RejectionMailbox& rejections() noexcept { return rejections_; }

    // Producer: the next cleared segment of the ring. The producer holds at most
    // one segment and pushes it before acquiring the next; throws if that
    // contract is broken rather than growing the ring.
    std::unique_ptr<FrameSegment> acquire();

    // Producer: hand a closed segment over. Blocks while `capacity` segments
    // are already queued; returns the nanoseconds spent blocked.
    u64 push(std::unique_ptr<FrameSegment> segment);

    // Consumer: next segment, waiting up to `timeout_ms` (0 = poll).
    std::unique_ptr<FrameSegment> pop(u32 timeout_ms);

    // Consumer: give a processed segment back for reuse.
    void recycle(std::unique_ptr<FrameSegment> segment) {
        segment->clear();
        std::lock_guard lock(mutex_);
        pool_.push_back(std::move(segment));
    }

    // Wakes a consumer blocked in pop and makes every later pop return null.
    void close();
    // Terminal native renderer failure. Wake both sides and rethrow on any
    // subsequent producer/consumer operation; never acknowledge dropped work.
    void fail(std::exception_ptr failure);
    bool closed() const noexcept { return closed_.load(std::memory_order_relaxed); }

    struct Stats {
        u64 pushed = 0;
        u64 popped = 0;
        u64 producer_waits = 0;
        u64 producer_wait_ns = 0;
        u64 producer_wait_ns_max = 0;
        u64 segments_allocated = 0;
        u64 depth_max = 0;
    };
    Stats stats() const;
    std::size_t depth() const;

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::vector<std::unique_ptr<FrameSegment>> queue_;  // FIFO, small
    std::deque<std::unique_ptr<FrameSegment>> pool_;    // FIFO: the ring's free segments, oldest first
    bool primed_ = false;
    std::atomic<bool> closed_{false};
    std::exception_ptr failure_;
    u64 pushed_ = 0, popped_ = 0, waits_ = 0, wait_ns_ = 0, wait_ns_max_ = 0;
    u64 segments_allocated_ = 0, depth_max_ = 0;
    RejectionMailbox rejections_;
};

}  // namespace gekko::native
