#include "gekko/rw_capture.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "gekko/cpu.h"
#include "gekko/gx_texture_decode.h"
#include "gekko/native_assets.h"
#include "gekko/profiling.h"
#include "gekko/savestate.h"
#include "gekko/system.h"

namespace gekko {
namespace {

using native::Op;

// ---- guest addresses (RENDERER_RE/01..04, docs/native-runtime-2026-09-16/understand) ----

// GX SDK state API (RECOMP_VISUAL_MAP/seeds/gx_sdk_map.csv).
constexpr u32 kGXSetViewport = 0x8039A49Cu;
constexpr u32 kGXSetViewportJitter = 0x8039A444u;
constexpr u32 kGXSetScissor = 0x8039A4E4u;
constexpr u32 kGXSetScissorBoxOffset = 0x8039A55Cu;
// The bloom ("glare") pass entry points. PreGlare snapshots the scene and then
// clears the frame buffer so the following draws build the glare source;
// PostGlare runs the ladder and composites. Both read the same parameter
// block and both return early on the same gate.
// docs/effects-2026-09-21/BLOOM_SPEC.md sections 2.5 and 2.7.
constexpr u32 kGuestPreGlare = 0x80430D64u;
constexpr u32 kGuestPostGlare = 0x80430904u;
constexpr u32 kGXSetProjection = 0x8039A0B4u;
constexpr u32 kGXSetProjectionv = 0x8039A158u;
constexpr u32 kGXLoadPosMtxImm = 0x8039A22Cu;
constexpr u32 kGXLoadNrmMtxImm = 0x8039A27Cu;
constexpr u32 kGXSetCurrentMtx = 0x8039A2CCu;
constexpr u32 kGXLoadTexMtxImm = 0x8039A300u;
constexpr u32 kGXSetBlendMode = 0x80399D44u;
constexpr u32 kGXSetZMode = 0x80399DF0u;
constexpr u32 kGXSetZCompLoc = 0x80399E24u;
constexpr u32 kGXSetAlphaCompare = 0x80399778u;
constexpr u32 kGXSetCullMode = 0x80396B54u;
constexpr u32 kGXSetFog = 0x80399A0Cu;
constexpr u32 kGXSetColorUpdate = 0x80399D98u;
constexpr u32 kGXSetAlphaUpdate = 0x80399DC4u;
constexpr u32 kGXSetDstAlpha = 0x80399F58u;
constexpr u32 kGXSetNumChans = 0x80397C80u;
constexpr u32 kGXSetChanCtrl = 0x80397CBCu;
constexpr u32 kGXSetChanMatColor = 0x80397B98u;
constexpr u32 kGXSetChanAmbColor = 0x80397AB0u;
constexpr u32 kGXLoadLightObjImm = 0x80397A34u;
constexpr u32 kGXSetNumTexGens = 0x80395FC8u;
constexpr u32 kGXSetTexCoordGen2 = 0x80395D48u;
constexpr u32 kGXSetTevColor = 0x803994D0u;
constexpr u32 kGXSetTevKColor = 0x80399594u;
constexpr u32 kGXBegin = 0x80396934u;
constexpr u32 kGXCallDisplayList = 0x8039A044u;

// RwGC driver.
constexpr u32 kRenderDefault = 0x80463964u;
constexpr u32 kRenderMatFx = 0x804A62D0u;
constexpr u32 kRenderSkin = 0x8044EEC4u;
constexpr u32 kRenderSkinMatFx = 0x80452C1Cu;
constexpr u32 kRenderPTank = 0x80449D54u;
constexpr u32 kTextureBind = 0x8049AC88u;
constexpr u32 kCameraBeginUpdate = 0x804956F4u;   // stdFunc[1]
constexpr u32 kCameraEndUpdate = 0x80495CACu;     // stdFunc[10]
constexpr u32 kClearQuad = 0x80499E58u;
constexpr u32 kCameraTextureCopy = 0x80496208u;
constexpr u32 kShowRaster = 0x80495CBCu;          // stdFunc[20]
constexpr u32 kRasterCreate = 0x80497CA8u;        // stdFunc[4]
constexpr u32 kRasterDestroy = 0x80498058u;       // stdFunc[5]
constexpr u32 kRasterUnlock = 0x80497104u;        // stdFunc[16]
constexpr u32 kRasterUnlockPalette = 0x80497388u; // stdFunc[24]
constexpr u32 kNotifyStatic = 0x804638DCu;
constexpr u32 kNotifySkin = 0x8044DE14u;
constexpr u32 kNotifyDMorph = 0x80445908u;

// Driver globals.
constexpr u32 kEngineInstance = 0x805F265Cu;   // -> RwGlobals
constexpr u32 kRasterExtensionOffset = 0x805F2700u;
constexpr u32 kWhiteRaster = 0x805F2708u;
constexpr u32 kImTexture = 0x805F270Cu;
// RwIm3D transform context (r13 0x805EC500 + 0x6248). +0x0C holds the
// RwIm3DTransform flags; bit 0 is rwIM3D_VERTEXUV, which is what the guest
// tests (0x804A0C40 / 0x804A127C) to choose MODULATE over PASSCLR.
constexpr u32 kIm3DContext = 0x805F2748u;
constexpr u32 kNextXfb = 0x805F26A8u;
constexpr u32 kRenderMode = 0x805F26F4u;
constexpr u32 kGeometryVtxFmtOffset = 0x805F25C8u;
constexpr u32 kWorldVtxFmtOffset = 0x805F25CCu;
// RpSkin's registered RpGeometry plugin offset. RenderWare assigns plugin
// offsets at registration, so this is read from the guest rather than assumed.
// The skin block decides whether the display list carries a per-vertex matrix
// index (PNMTXIDX): with one, a vertex tuple is one byte WIDER, and a parser
// told otherwise walks short and desynchronises. See the decode in
// docs/ for the measured case - a 3,965-vertex strip that ends at +23,793
// instead of +27,758 and then reads its own index bytes as GX opcodes.
constexpr u32 kSkinGeometryOffset = 0x8056E7D0u;
constexpr u32 kDefaultVtxFmt = 0x805E2748u;

// GXBegin return addresses (call site + 4) -> producer.
struct BeginSite {
    u32 return_address;
    native::DrawProducer producer;
    bool clear_quad;
};
constexpr BeginSite kBeginSites[] = {
    {0x80491EC4u, native::DrawProducer::Im2D, false},
    {0x80491B4Cu, native::DrawProducer::Im2DTriangle, false},
    {0x80491D44u, native::DrawProducer::Im2DLine, false},
    {0x804924A8u, native::DrawProducer::Im2DIndexed, false},
    {0x80499F84u, native::DrawProducer::Unknown, true},
    {0x8044A1B8u, native::DrawProducer::PTank, false},
    {0x804A0D08u, native::DrawProducer::Im3D, false},
    {0x804A1344u, native::DrawProducer::Im3D, false},
    {0x8049947Cu, native::DrawProducer::RasterRender, false},
};

// GXCallDisplayList return addresses -> producer / pass kind.
enum class MeshPass : u8 { DefaultObject, MatFxBase, MatFxTextured, MatFxUnsupported };
struct DisplayListSite {
    u32 return_address;
    native::DrawProducer producer;
    MeshPass pass;
};
constexpr DisplayListSite kDisplayListSites[] = {
    {0x80463BB4u, native::DrawProducer::DefaultMesh, MeshPass::DefaultObject},
    {0x80463C08u, native::DrawProducer::DefaultMesh, MeshPass::DefaultObject},
    {0x8044F284u, native::DrawProducer::SkinMesh, MeshPass::DefaultObject},
    {0x8044F428u, native::DrawProducer::SkinMesh, MeshPass::DefaultObject},
    // 0x804A3F74 DUALTEXTURE base, 0x804A4240 its second pass
    {0x804A4228u, native::DrawProducer::MatFxMesh, MeshPass::MatFxBase},
    {0x804A4310u, native::DrawProducer::MatFxMesh, MeshPass::MatFxTextured},
    // 0x804A4358 DUALUVTRANSFORM pass 1, 0x804A47F4 pass 2
    {0x804A4680u, native::DrawProducer::MatFxMesh, MeshPass::MatFxBase},
    {0x804A4894u, native::DrawProducer::MatFxMesh, MeshPass::MatFxTextured},
    // 0x804A48DC ENVMAP base, 0x804A4B7C environment pass
    {0x804A4B64u, native::DrawProducer::MatFxMesh, MeshPass::MatFxBase},
    {0x804A4DB8u, native::DrawProducer::MatFxMesh, MeshPass::MatFxTextured},
    // 0x804A4E00 BUMPMAP / BUMPENVMAP (indirect TEV program)
    {0x804A55A4u, native::DrawProducer::MatFxMesh, MeshPass::MatFxUnsupported},
    // 0x804A55CC inline base passes (none / UVTRANSFORM / fallthrough)
    {0x804A5B1Cu, native::DrawProducer::MatFxMesh, MeshPass::MatFxBase},
    {0x804A5E08u, native::DrawProducer::MatFxMesh, MeshPass::MatFxBase},
    {0x804A6090u, native::DrawProducer::MatFxMesh, MeshPass::MatFxBase},
    // 0x804A7AAC GCN native-shader material (plugin 0x129; no asset carries it)
    {0x804A7B3Cu, native::DrawProducer::MatFxNativeShader, MeshPass::MatFxUnsupported},
    {0x804A7C14u, native::DrawProducer::MatFxNativeShader, MeshPass::MatFxUnsupported},
};

inline double fpr_value(const Cpu& cpu, u32 index) {
    return bits_to_double(cpu.fpr[index].bits[0]);
}

inline u64 tsc_now() noexcept {
#if defined(_MSC_VER)
    return __rdtsc();
#else
    return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

template <typename Record>
Record zeroed() noexcept {
    Record value;
    std::memset(static_cast<void*>(&value), 0, sizeof(Record));
    return value;
}

// GX texture image size for one level: the decoder's tile layout is the one
// definition (I8/IA4/C8 are 8x4 tiles, so a width that is not a multiple of 8
// carries more bytes than a 4x4-block rounding gives).
u32 gx_texture_bytes(u32 format, u32 width, u32 height) noexcept {
    return static_cast<u32>(gx_texture_data_size(format, width, height));
}

u64 fnv1a(const u8* data, std::size_t size, u64 hash = 1469598103934665603ull) noexcept {
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

u32 component_bytes(u32 type) noexcept { return type <= 1u ? 1u : (type <= 3u ? 2u : 4u); }

float read_component(const u8* at, u32 type, u32 frac) noexcept {
    const float scale = 1.0f / static_cast<float>(1u << (frac & 31u));
    switch (type) {
        case 0: return static_cast<float>(at[0]) * scale;
        case 1: return static_cast<float>(static_cast<s8>(at[0])) * scale;
        case 2: return static_cast<float>(load_be16(at)) * scale;
        case 3: return static_cast<float>(static_cast<s16>(load_be16(at))) * scale;
        default: {
            const u32 bits = load_be32(at);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
    }
}

void read_color(const u8* at, u32 type, u8 out[4]) noexcept {
    switch (type) {
        case 0: {  // RGB565
            const u32 v = load_be16(at);
            const u32 r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
            out[0] = static_cast<u8>((r << 3) | (r >> 2));
            out[1] = static_cast<u8>((g << 2) | (g >> 4));
            out[2] = static_cast<u8>((b << 3) | (b >> 2));
            out[3] = 255;
            break;
        }
        case 1:  // RGB8
            out[0] = at[0]; out[1] = at[1]; out[2] = at[2]; out[3] = 255;
            break;
        case 2:  // RGBX8
            out[0] = at[0]; out[1] = at[1]; out[2] = at[2]; out[3] = 255;
            break;
        case 3: {  // RGBA4
            const u32 v = load_be16(at);
            out[0] = static_cast<u8>(((v >> 12) & 15u) * 17u);
            out[1] = static_cast<u8>(((v >> 8) & 15u) * 17u);
            out[2] = static_cast<u8>(((v >> 4) & 15u) * 17u);
            out[3] = static_cast<u8>((v & 15u) * 17u);
            break;
        }
        case 4: {  // RGBA6
            const u32 v = (static_cast<u32>(at[0]) << 16) | (static_cast<u32>(at[1]) << 8) | at[2];
            const auto expand = [](u32 c) { return static_cast<u8>((c << 2) | (c >> 4)); };
            out[0] = expand((v >> 18) & 63u);
            out[1] = expand((v >> 12) & 63u);
            out[2] = expand((v >> 6) & 63u);
            out[3] = expand(v & 63u);
            break;
        }
        default:  // RGBA8
            out[0] = at[0]; out[1] = at[1]; out[2] = at[2]; out[3] = at[3];
            break;
    }
}

u32 color_bytes(u32 type) noexcept {
    static constexpr u32 kSizes[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return kSizes[type & 7u];
}

// Decodes direct (non-indexed) vertex attributes of one immediate primitive.
// Layout per the CP VCD/VAT words the guest programmed for this primitive.
// Returns false if any attribute is indexed (not produced by the immediate
// RwGC paths; counted by the caller).
bool decode_immediate(const VertexFormat& format, u32 vat_a, u32 vat_b, u32 vat_c,
                      const u8* payload, u32 count, std::vector<shadow::render::RenderVertex>& out,
                      u32& attributes) {
    attributes = 0;
    const auto indexed = [](AttributeMode mode) {
        return mode == AttributeMode::Index8 || mode == AttributeMode::Index16;
    };
    if (indexed(format.position) || indexed(format.normal) || indexed(format.color[0]) ||
        indexed(format.color[1])) {
        return false;
    }
    for (AttributeMode mode : format.texture_coordinate) {
        if (indexed(mode)) return false;
    }
    out.resize(count);
    const u32 pos_elements = (vat_a & 1u) != 0u ? 3u : 2u;
    const u32 pos_type = (vat_a >> 1) & 7u;
    const u32 pos_frac = (vat_a >> 4) & 31u;
    const bool nbt = ((vat_a >> 9) & 1u) != 0u;
    const u32 nrm_type = (vat_a >> 10) & 7u;
    const u32 nrm_frac = nrm_type == 1u ? 6u : (nrm_type == 3u ? 14u : 0u);
    const std::array<u32, 2> col_type{(vat_a >> 14) & 7u, (vat_a >> 18) & 7u};
    // Texture coordinate fields (Dolphin VAT layout): tex0 in VAT A, tex1-3 in
    // VAT B, tex4 split across B (count/format) and C (frac), tex5-7 in VAT C.
    struct TexField { u32 elements, type, frac; };
    std::array<TexField, 8> tex{};
    tex[0] = {(vat_a >> 21) & 1u, (vat_a >> 22) & 7u, (vat_a >> 25) & 31u};
    for (u32 i = 1; i <= 3; ++i) {
        const u32 s = (i - 1u) * 9u;
        tex[i] = {(vat_b >> s) & 1u, (vat_b >> (s + 1u)) & 7u, (vat_b >> (s + 4u)) & 31u};
    }
    tex[4] = {(vat_b >> 27) & 1u, (vat_b >> 28) & 7u, vat_c & 31u};
    for (u32 i = 5; i <= 7; ++i) {
        const u32 s = 5u + (i - 5u) * 9u;
        tex[i] = {(vat_c >> s) & 1u, (vat_c >> (s + 1u)) & 7u, (vat_c >> (s + 4u)) & 31u};
    }

    if (format.position_matrix_index) attributes |= native::kImmediateMatrixIndex;
    if (format.position == AttributeMode::Direct) attributes |= native::kImmediatePosition;
    if (format.normal == AttributeMode::Direct) attributes |= native::kImmediateNormal;
    if (format.color[0] == AttributeMode::Direct) attributes |= native::kImmediateColor0;
    if (format.color[1] == AttributeMode::Direct) attributes |= native::kImmediateColor1;
    if (format.texture_coordinate[0] == AttributeMode::Direct) attributes |= native::kImmediateUv0;
    if (format.texture_coordinate[1] == AttributeMode::Direct) attributes |= native::kImmediateUv1;

    const u8* at = payload;
    for (u32 vertex = 0; vertex < count; ++vertex) {
        shadow::render::RenderVertex& v = out[vertex];
        std::memset(&v, 0, sizeof(v));
        v.color0[0] = v.color0[1] = v.color0[2] = v.color0[3] = 255;
        if (format.position_matrix_index) ++at;
        for (bool present : format.texture_matrix_index) {
            if (present) ++at;
        }
        if (format.position == AttributeMode::Direct) {
            const u32 step = component_bytes(pos_type);
            for (u32 c = 0; c < pos_elements; ++c) {
                v.position[c] = read_component(at, pos_type, pos_frac);
                at += step;
            }
        }
        if (format.normal == AttributeMode::Direct) {
            const u32 step = component_bytes(nrm_type);
            const u32 components = nbt ? 9u : 3u;
            for (u32 c = 0; c < components; ++c) {
                if (c < 3u) v.normal[c] = read_component(at, nrm_type, nrm_frac);
                at += step;
            }
        }
        for (u32 index = 0; index < 2u; ++index) {
            if (format.color[index] != AttributeMode::Direct) continue;
            read_color(at, col_type[index], index == 0u ? v.color0 : v.color1);
            at += color_bytes(col_type[index]);
        }
        for (u32 index = 0; index < 8u; ++index) {
            if (format.texture_coordinate[index] != AttributeMode::Direct) continue;
            const TexField& field = tex[index];
            const u32 step = component_bytes(field.type);
            const u32 elements = field.elements != 0u ? 2u : 1u;
            for (u32 c = 0; c < elements; ++c) {
                const float value = read_component(at, field.type, field.frac);
                if (index < 2u) v.uv[index][c] = value;
                at += step;
            }
        }
    }
    return true;
}

}  // namespace

// ===========================================================================
// STEP-0 MEASUREMENT: the R4 / COHERE / HASH-TOKEN coherence probe. DEFAULT OFF.
// (findings-2026-09-23/HARDWARE_ASSUMPTION_AUDIT.md, R4, COHERE, HASH-TOKEN.)
//
// The question: does the guest rewrite an uploaded geometry blob IN PLACE,
// without a serial/size change or a destroy notify? The DMorph software render
// callback (0x80444AC4) locks the geometry and the reinstance (0x80464890 ->
// 0x80464624) re-emits vertex arrays into the SAME resource entry, then
// DCFlushRange's them. prepare_geometry's generation key cannot see that, so
// the host keeps drawing the upload-time copy. This probe re-hashes the current
// guest bytes at every unchanged-generation use and compares with the upload.
//
// Blob layout (tools/doldis.py on fn_80463964 / fn_8046D2FC, matches
// rw_geometry_convert.cpp): +0 u16 PE draw-sync token (re-stamped on EVERY draw
// by all four render callbacks: sth 0x18(r4) at 0x80463998 / 0x8044EEF0 /
// 0x80452C48, sth 0(r4) at 0x804A6304), +2 u16 mesh serial (part of the
// generation key), +4 u32 flags (CONTENT: the reinstance rewrites it at
// 0x80464748/0x80464774), +8 u32 numAttributes, then 8-byte attribute records
// {u32 array ptr, u8 attr, u8 stride, ...}, then 8-byte mesh records
// {u32 DL ptr, u32 DL size}, then in-blob arrays and display lists. The ONLY
// embedded pointers are attribute record +0 and mesh record +0, so exactly those
// are normalised; the payload is NOT scanned for pointer-looking words
// (-0.0f = 0x80000000 is legal vertex data).
//
// Excluded from every probe hash: [0,2) token, [2,4) serial, and the
// [size,size+32) arena tail production copies (the next heap block's header).
// "Covered" slots are exactly production's GeometryDynamic set (Skin/SkinMatFx
// slot 0, slot 1 when geometry_flags & 0x10): the host refreshes those every
// use, so they are hashed separately and the "uncovered" hash is the one that
// measures staleness the host cannot repair.
//
// Zero guest-visible effect: MEM1 is read through this class's own accessor
// (the game's memory read host-side: no guest access, no observers, no guards) and
// never through RwCapture::guest(), which counts into report_. No guest
// register, memory, retire count, dispatch decision or production record is
// touched. The optional flush observers go through System::add_hle_observer
// (chaining, declining) and NOT through RwCapture::observe, so report_ is
// unchanged. Nothing here is saved in a savestate.
// ===========================================================================

class RwCapture::CoherenceProbe {
public:
    CoherenceProbe(RwCapture& owner, const CoherenceProbeConfig& config);

    void install_flush_observers();
    void observe_use(Cpu& cpu, const ObjectContext& context, const GeometryRecord& record,
                     u32 entry, u32 header, u32 size, u32 attributes, u32 vertex_count,
                     bool sector, u32 geometry, bool upload_now);
    void on_present();
    void on_release(u32 entry);
    void on_state_loaded();
    std::string json() const;
    std::string summary() const;

private:
    static constexpr u32 kSlots = 16;
    static constexpr std::size_t kPipes = 6;  // indexed by Pipeline
    // Region bits of a change: 0..5 below, 8+i = attribute slot i.
    enum : u32 {
        kRegionFlags = 0,
        kRegionAttrCount = 1,
        kRegionAttrRecords = 2,
        kRegionMeshRecords = 3,
        kRegionDisplayList = 4,
        kRegionBodyGap = 5,
        kRegionFixedCount = 6,
        kRegionSlotBase = 8,
        kRegionNone = 0xFFu,
    };
    // Self-test region classes (review requirement: in-blob body hash control).
    enum : u32 {
        kSelfFlags,
        kSelfAttrCount,
        kSelfAttrRecord,
        kSelfMeshRecord,
        kSelfDisplayList,
        kSelfInBlobSlot,
        kSelfBodyGap,
        kSelfClasses,
    };

    // Byte source: the game's memory, optionally overlaid by a HOST copy of the
    // blob (the self-test flips bytes in host copies only, never in guest memory).
    struct Source {
        const GuestMemory* memory = nullptr;
        const u8* overlay = nullptr;
        u32 overlay_base = 0;
        u32 overlay_size = 0;
        const u8* at(u32 address, u64 bytes) const noexcept;
    };
    // Blob-relative span that was hashed (coverage invariant / self-test).
    struct Span {
        u32 begin;
        u32 end;
        u8 kind;  // 0 fixed header word, 1 attribute/mesh records, 2 body gap
    };
    struct View {
        bool ok = false;
        bool records_overhang = false;
        bool mesh_clamped = false;
        u32 blob_extent = 0;
        u32 records_end = 0;
        u32 meshes = 0;
        u32 body_begin = 0;
        u32 covered_mask = 0;
        u32 inblob_mask = 0;
        u32 ext_mask = 0;  // non-covered, external, valid
        std::array<u32, kSlots> ptr{};
        std::array<u32, kSlots> bytes{};
        std::array<u64, kSlots> slot_hash{};
        u32 covered_ranges = 0;
        std::array<std::pair<u32, u32>, 2> covered{};  // blob-relative, clipped to body, merged
        u64 uncovered = 0;
        u64 combined = 0;
        u64 bytes_hashed = 0;
        u32 inblob_overhang = 0;
        u32 extent_wrap = 0;
        u32 external_display_lists = 0;
    };
    struct FirstDiff {
        u32 region = kRegionNone;
        bool external = false;
        u32 offset = 0;  // blob-relative, or within the external slot array
        u32 old_word = 0;
        u32 new_word = 0;
    };
    struct PipeStats {
        u64 uses = 0, sector_uses = 0, uploads = 0, reuploads = 0, compares = 0,
            compares_without_baseline = 0, stale_uses = 0, stale_uncovered_uses = 0,
            change_events = 0, change_uncovered_events = 0, distinct_stale_uncovered_generations = 0,
            dmorph_sw_uses = 0, dmorph_sw_compares = 0, dmorph_sw_stale_uses = 0,
            dmorph_sw_stale_uncovered_uses = 0, dmorph_sw_change_events = 0, bytes_hashed = 0,
            external_pointer_moves = 0, change_with_flush = 0, change_without_flush = 0,
            flush_without_change = 0, flush_overlap_covered = 0,
            flush_overlap_uncovered_inblob = 0, flush_overlap_uncovered_external = 0,
            uncovered_flush_then_upload = 0, uncovered_flush_without_upload = 0,
            uncovered_flush_without_upload_changed = 0;
        std::array<u64, kRegionFixedCount> region_hits{};
        std::array<u64, kSlots> slot_hits{};
    };
    struct Rec {
        u32 generation = 0;
        u32 size = 0;
        u32 header = 0;
        u16 serial = 0;
        u8 pipeline = 0;
        bool stale_seen = false;
        bool flushed_since_use = false;
        bool uncovered_flush_pending = false;
        u64 base_combined = 0, base_uncovered = 0, last_combined = 0, last_uncovered = 0;
        u64 upload_primary = 0;  // HASH-TOKEN: this entry's previous upload hash
        u32 covered_mask = 0, inblob_mask = 0, ext_mask = 0;
        std::array<u32, kSlots> slot_ptr{};  // at the last use
        std::array<u32, kSlots> slot_len{};
        std::array<u32, kSlots> ext_ptr{};   // at the upload (pointer-move count)
        u64 lo = 0, hi = 0;                  // bounding range of the above (flush fast reject)
        u32 summary = ~0u;
        u32 events = 0;
        std::vector<u8> snap;  // raw [0,size) + external/covered-external slot arrays
        u32 snap_mask = 0;
        std::array<u32, kSlots> snap_off{}, snap_len{};
    };
    struct Summary {
        u32 entry = 0, generation = 0, header = 0, size = 0, object = 0, render_callback = 0,
            geometry = 0, morph_targets = 0, vertex_count = 0, attributes = 0, meshes = 0;
        u16 serial = 0;
        u8 pipeline = 0;
        bool sector = false;
        u64 uses_since_first_stale = 0, stale_uses = 0, stale_uncovered_uses = 0,
            change_events = 0, change_uncovered_events = 0;
        u64 first_change_present = 0, last_change_present = 0;
        u32 region_mask_union = 0;
    };
    struct Event {
        u64 present = 0;  // == native Present.frame the draw lands in (--native-screenshot-frame)
        u64 game_ticks = 0;  // the game clock at the change
        u32 entry = 0, header = 0, generation = 0, size = 0, object = 0, render_callback = 0,
            geometry = 0, morph_targets = 0, vertex_count = 0, region_mask = 0;
        u16 serial = 0;
        u8 pipeline = 0;
        bool sector = false, stale_uncovered = false, changed_uncovered = false,
             flushed_since_use = false, uncovered_flush_pending = false, has_diff = false;
        u64 diff_bytes = 0;
        FirstDiff first{};
    };
    struct LrBucket {
        u32 lr = 0;
        u64 calls = 0;
        u64 overlapping_calls = 0;
    };
    struct SelfClass {
        u64 attempts = 0, detected = 0, combined_only = 0, missed = 0, unavailable = 0;
    };

    Source live_source() const noexcept;
    static void build_view(const Source& src, u32 header, u32 size, u32 attributes,
                           u32 vertex_count, bool skin, u32 geometry_flags, u32 mesh_count,
                           View& view, std::vector<Span>* trace);
    bool coverage_ok(const View& view, u32 size, std::vector<Span>& trace) const;
    void self_test(const Source& live, const View& view, u32 header, u32 size, u32 attributes,
                   u32 vertex_count, bool skin, u32 geometry_flags, u32 mesh_count);
    void take_snapshot(Rec& rec, const Source& src, const View& view, u32 header, u32 size);
    void drop_snapshot(Rec& rec);
    void attribute(const Source& src, const View& view, const Rec& rec, u32 header, u32 size,
                   u32 attributes, u32& mask, u64& diff_bytes, FirstDiff& first) const;
    void hash_token(Rec& rec, u32 entry, const Source& src, const View& view, u32 header,
                    u32 size);
    void on_flush(Cpu& cpu, bool store);
    static bool covered_inblob_contains(const Rec& rec, u64 begin, u64 end);
    static const char* pipe_name(std::size_t index) noexcept;
    static std::string region_name(u32 region);

    RwCapture& owner_;
    CoherenceProbeConfig config_;
    std::array<PipeStats, kPipes> pipes_{};
    std::unordered_map<u32, Rec> recs_;
    std::vector<Summary> summaries_;
    u64 summaries_overflow_ = 0;
    std::vector<Event> events_;
    u64 skin_events_ = 0;
    u64 events_dropped_ = 0;
    std::vector<Span> trace_scratch_;

    // executed-path counters
    u64 valid_uses_ = 0, baselines_new_generation_ = 0, compares_ = 0,
        compares_without_baseline_ = 0, frames_ = 0, released_ = 0, state_loads_ = 0,
        live_peak_ = 0;

    // per-frame accumulators and their fold
    u64 f_compares_ = 0, f_stale_ = 0, f_stale_unc_ = 0, f_change_unc_ = 0, f_dmorph_ = 0,
        f_skin_change_ = 0;
    u64 frames_with_compares_ = 0, frames_with_stale_ = 0, frames_with_stale_unc_ = 0,
        frames_with_change_unc_ = 0, frames_with_dmorph_ = 0, frames_with_skin_change_ = 0,
        max_stale_unc_per_frame_ = 0;
    std::array<u64, 7> stale_unc_hist_{};  // 0 / 1 / 2-3 / 4-7 / 8-15 / 16-31 / 32+
    u64 first_stale_unc_present_ = 0, last_stale_unc_present_ = 0;

    // flush observers
    bool flush_installed_ = false;
    u64 flush_calls_ = 0, store_calls_ = 0, flush_bytes_ = 0, flush_overlapping_calls_ = 0,
        raster_unlock_calls_ = 0, raster_unlock_overlaps_ = 0, lr_other_calls_ = 0,
        lr_other_overlapping_ = 0;
    std::array<LrBucket, 32> lr_{};
    std::size_t lr_used_ = 0;

    // HASH-TOKEN at upload: three variants, each a capped distinct set.
    //   (i)   production method minus the token [0,2) (serial + tail kept)
    //   (ii)  minus [0,4) (tail kept)
    //   (iii) primary: minus [0,4) and minus the [size,size+32) tail
    static constexpr std::size_t kHashSetCap = 1u << 20;
    u64 ht_uploads_ = 0, ht_repeat_same_entry_ = 0, ht_repeat_other_entry_ = 0;
    std::unordered_set<u64> ht_minus_token_, ht_minus_token_serial_;
    std::unordered_map<u64, u32> ht_primary_;
    bool ht_overflow_ = false;

    // self-test + coverage invariant
    u64 coverage_checks_ = 0, coverage_violations_ = 0;
    u64 selftest_runs_ = 0, selftest_identity_mismatch_ = 0, selftest_skipped_large_ = 0;
    std::array<SelfClass, kSelfClasses> self_{};
    std::vector<u8> selftest_buffer_;

    // limits
    u64 snapshot_bytes_ = 0, snapshot_peak_ = 0, records_without_snapshot_ = 0,
        reads_out_of_range_ = 0, mesh_records_clamped_ = 0, inblob_array_overhang_ = 0,
        records_overhang_ = 0, extent_wrap_ = 0, external_display_lists_ = 0;
    u64 probe_ns_ = 0;
};

// ---------------------------------------------------------------------------

RwCapture::RwCapture(System& system) : system_(system) {
    const char* cpu_profile = std::getenv("GEKKO_NATIVE_CPU_PROFILE");
    cpu_owner_profile_ = cpu_profile != nullptr && std::strcmp(cpu_profile, "1") == 0;
    shadow_.resize(static_cast<std::size_t>(Op::Count));
    vertex_scratch_.reserve(4096);
}

RwCapture::~RwCapture() {
    if (installed_) {
        system_.immediate().set_sink(nullptr);
    }
}

void RwCapture::observe(u32 address, void (RwCapture::*handler)(Cpu&), const char* name) {
    u32 owner = static_cast<u32>(report_.hook_owner_costs.size());
    if (cpu_owner_profile_) {
        if (report_.hook_owner_count < report_.hook_owner_costs.size()) {
            owner = report_.hook_owner_count++;
            report_.hook_owner_costs[owner].address = address;
            report_.hook_owner_costs[owner].name = name;
        } else {
            ++report_.hook_owner_overflow;
        }
    }
    system_.add_hle_observer(
        address,
        [this, handler, owner](Cpu& cpu) {
            const u64 begin = tsc_now();
            ++report_.hook_calls;
            (this->*handler)(cpu);
            const u64 ticks = tsc_now() - begin;
            report_.hook_ns += ticks;
            if (cpu_owner_profile_ && owner < report_.hook_owner_costs.size()) {
                ++report_.hook_owner_costs[owner].calls;
                report_.hook_owner_costs[owner].ticks += ticks;
            }
        },
        std::string("native capture: ") + name);
}

void RwCapture::install() {
    if (installed_) return;
    installed_ = true;
    segment_ = queue_ != nullptr ? queue_->acquire() : std::make_unique<native::FrameSegment>();

    // Plot shapes are declared once, here, rather than inferred from the first
    // sample: a resource COUNT is a staircase (it holds its value between the
    // presents that sample it), while a per-frame or per-conversion QUANTITY is
    // only true at the instant it was sampled and must not be drawn as if it
    // held. The two convert/* plots are sampled in rw_geometry_convert.cpp on
    // the render thread; their shape is declared here because this is the one
    // place in the subsystem that runs exactly once.
    GEKKO_PROFILE_PLOT_CONFIG("rw/geometries-live", GEKKO_PLOT_NUMBER, true, true, 0x4fa8ff);
    GEKKO_PROFILE_PLOT_CONFIG("rw/textures-live", GEKKO_PLOT_NUMBER, true, true, 0x4fd8a8);
    GEKKO_PROFILE_PLOT_CONFIG("rw/segment-payload", GEKKO_PLOT_MEMORY, false, true, 0xffc04f);
    GEKKO_PROFILE_PLOT_CONFIG("rw/segment-commands", GEKKO_PLOT_NUMBER, false, true, 0xffc04f);
    GEKKO_PROFILE_PLOT_CONFIG("rw/segment-queue-depth", GEKKO_PLOT_NUMBER, true, true, 0xff8c4f);
    GEKKO_PROFILE_PLOT_CONFIG("rw/geometry-releases", GEKKO_PLOT_NUMBER, true, false, 0x4fa8ff);
    GEKKO_PROFILE_PLOT_CONFIG("rw/retry-pending", GEKKO_PLOT_NUMBER, true, true, 0xff5f5f);
    GEKKO_PROFILE_PLOT_CONFIG("rw/cameras-per-frame", GEKKO_PLOT_NUMBER, true, false, 0xa88cff);
    GEKKO_PROFILE_PLOT_CONFIG("rw/gx-unattributed", GEKKO_PLOT_NUMBER, true, false, 0xff5f5f);
    GEKKO_PROFILE_PLOT_CONFIG("rw/state-dedup", GEKKO_PLOT_PERCENTAGE, false, true, 0xa88cff);
    GEKKO_PROFILE_PLOT_CONFIG("rw/resources-lost", GEKKO_PLOT_NUMBER, true, false, 0xff0000);
    GEKKO_PROFILE_PLOT_CONFIG("rw/convert-foreign-commands", GEKKO_PLOT_NUMBER, false, false,
                              0xff5f5f);
    GEKKO_PROFILE_PLOT_CONFIG("rw/convert-output-bytes", GEKKO_PLOT_MEMORY, false, true, 0xffc04f);

    observe(kGXSetViewport, &RwCapture::gx_set_viewport, "GXSetViewport");
    observe(kGXSetViewportJitter, &RwCapture::gx_set_viewport_jitter, "GXSetViewportJitter");
    observe(kGXSetScissor, &RwCapture::gx_set_scissor, "GXSetScissor");
    observe(kGXSetScissorBoxOffset, &RwCapture::gx_set_scissor_offset, "GXSetScissorBoxOffset");
    observe(kGuestPreGlare, &RwCapture::glare_pre, "PreGlare");
    observe(kGuestPostGlare, &RwCapture::glare_post, "PostGlare");
    observe(kGXSetProjection, &RwCapture::gx_set_projection, "GXSetProjection");
    observe(kGXSetProjectionv, &RwCapture::gx_set_projection_v, "GXSetProjectionv");
    observe(kGXLoadPosMtxImm, &RwCapture::gx_load_position_matrix, "GXLoadPosMtxImm");
    observe(kGXLoadNrmMtxImm, &RwCapture::gx_load_normal_matrix, "GXLoadNrmMtxImm");
    observe(kGXSetCurrentMtx, &RwCapture::gx_set_current_matrix, "GXSetCurrentMtx");
    observe(kGXLoadTexMtxImm, &RwCapture::gx_load_texture_matrix, "GXLoadTexMtxImm");
    observe(kGXSetBlendMode, &RwCapture::gx_set_blend_mode, "GXSetBlendMode");
    observe(kGXSetZMode, &RwCapture::gx_set_z_mode, "GXSetZMode");
    observe(kGXSetZCompLoc, &RwCapture::gx_set_z_comp_loc, "GXSetZCompLoc");
    observe(kGXSetAlphaCompare, &RwCapture::gx_set_alpha_compare, "GXSetAlphaCompare");
    observe(kGXSetCullMode, &RwCapture::gx_set_cull_mode, "GXSetCullMode");
    observe(kGXSetFog, &RwCapture::gx_set_fog, "GXSetFog");
    observe(kGXSetColorUpdate, &RwCapture::gx_set_color_update, "GXSetColorUpdate");
    observe(kGXSetAlphaUpdate, &RwCapture::gx_set_alpha_update, "GXSetAlphaUpdate");
    observe(kGXSetDstAlpha, &RwCapture::gx_set_dst_alpha, "GXSetDstAlpha");
    observe(kGXSetNumChans, &RwCapture::gx_set_num_chans, "GXSetNumChans");
    observe(kGXSetChanCtrl, &RwCapture::gx_set_chan_ctrl, "GXSetChanCtrl");
    observe(kGXSetChanMatColor, &RwCapture::gx_set_chan_mat_color, "GXSetChanMatColor");
    observe(kGXSetChanAmbColor, &RwCapture::gx_set_chan_amb_color, "GXSetChanAmbColor");
    observe(kGXLoadLightObjImm, &RwCapture::gx_load_light, "GXLoadLightObjImm");
    observe(kGXSetNumTexGens, &RwCapture::gx_set_num_tex_gens, "GXSetNumTexGens");
    observe(kGXSetTexCoordGen2, &RwCapture::gx_set_tex_coord_gen, "GXSetTexCoordGen2");
    observe(kGXSetTevColor, &RwCapture::gx_set_tev_color, "GXSetTevColor");
    observe(kGXSetTevKColor, &RwCapture::gx_set_tev_konst_color, "GXSetTevKColor");
    observe(kGXBegin, &RwCapture::gx_begin, "GXBegin");
    observe(kGXCallDisplayList, &RwCapture::gx_call_display_list, "GXCallDisplayList");

    observe(kRenderDefault, &RwCapture::rw_render_default, "RwGC default render callback");
    observe(kRenderMatFx, &RwCapture::rw_render_matfx, "RwGC MatFX render callback");
    observe(kRenderSkin, &RwCapture::rw_render_skin, "RwGC skin render callback");
    observe(kRenderSkinMatFx, &RwCapture::rw_render_skin_matfx, "RwGC skin+MatFX render callback");
    observe(kRenderPTank, &RwCapture::rw_render_ptank, "RwGC PTank render callback");
    observe(kTextureBind, &RwCapture::rw_texture_bind, "RwGC TextureBind");
    observe(kCameraBeginUpdate, &RwCapture::rw_camera_begin, "RwGC CameraBeginUpdate");
    observe(kCameraEndUpdate, &RwCapture::rw_camera_end, "RwGC CameraEndUpdate");
    observe(kClearQuad, &RwCapture::rw_clear_quad, "RwGC ClearQuad");
    observe(kCameraTextureCopy, &RwCapture::rw_camera_texture_copy, "RwGC CameraTextureCopy");
    observe(kShowRaster, &RwCapture::rw_show_raster, "RwGC RasterShowRaster");
    observe(kRasterCreate, &RwCapture::rw_raster_create, "RwGC RasterCreate");
    observe(kRasterDestroy, &RwCapture::rw_raster_destroy, "RwGC RasterDestroy");
    observe(kRasterUnlock, &RwCapture::rw_raster_unlock, "RwGC RasterUnlock");
    observe(kRasterUnlockPalette, &RwCapture::rw_raster_unlock_palette, "RwGC RasterUnlockPalette");
    observe(kNotifyStatic, &RwCapture::rw_geometry_notify, "RwGC instance destroy notify");
    observe(kNotifySkin, &RwCapture::rw_geometry_notify, "RwGC skin instance destroy notify");
    observe(kNotifyDMorph, &RwCapture::rw_geometry_notify, "RwGC DMorph instance destroy notify");

    // STEP-0 measurement, default off: the DCFlushRange/DCStoreRange observers,
    // chained in front of whatever owns those entries (nothing today).
    if (coherence_ != nullptr) coherence_->install_flush_observers();

    system_.immediate().set_sink(this);
}

// ---- guest memory -----------------------------------------------------------

// The game's bytes at a guest address: a host-side read (nothing records it),
// refused outside mapped memory.
const u8* RwCapture::guest(u32 address, u32 size) const noexcept {
    const u8* const at = system_.guest_memory().peek(GcAddr(address), size);
    if (at == nullptr) ++report_.guest_reads_out_of_range;
    return at;
}

u32 RwCapture::read32(u32 address) const noexcept {
    const u8* at = guest(address, 4u);
    return at != nullptr ? load_be32(at) : 0u;
}
u16 RwCapture::read16(u32 address) const noexcept {
    const u8* at = guest(address, 2u);
    return at != nullptr ? load_be16(at) : 0u;
}
u8 RwCapture::read8(u32 address) const noexcept {
    const u8* at = guest(address, 1u);
    return at != nullptr ? at[0] : 0u;
}
float RwCapture::read_f32(u32 address) const noexcept {
    const u32 bits = read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// ---- segment ----------------------------------------------------------------

native::FrameSegment& RwCapture::segment() { return *segment_; }

template <typename Record>
void RwCapture::emit(Op op, const Record& record) {
    segment_->append(op, record);
    ++report_.op_counts[static_cast<std::size_t>(op)];
    ++report_.commands;
}

template <typename Record>
void RwCapture::emit_state(Op op, u32 index, const Record& record) {
    ++report_.state_calls;
    ShadowSlot& slot = shadow_[static_cast<std::size_t>(op)][index % kShadowSlots];
    if (slot.valid && slot.size == sizeof(Record) &&
        std::memcmp(slot.bytes.data(), &record, sizeof(Record)) == 0) {
        ++report_.state_deduplicated;
        return;
    }
    slot.valid = true;
    slot.size = static_cast<u8>(sizeof(Record));
    std::memcpy(slot.bytes.data(), &record, sizeof(Record));
    emit(op, record);
}

bool RwCapture::submit_native_ui(const shadow::render::RenderVertex* vertices, u32 vertex_count,
                                 float width, float height) {
    // The only path where a host module has REPLACED a guest draw, and the
    // report cannot separate these quads from the guest's own immediate draws.
    // The zone counts them and prices the save/force/restore bracket, which
    // runs inline on the guest thread.
    GEKKO_PROFILE_ZONE("rw/native-ui");
    GEKKO_PROFILE_VALUE(vertex_count);
    if (segment_ == nullptr || vertices == nullptr || vertex_count == 0u) return false;
    if (vertex_count % 4u != 0u) return false;  // quads
    if (!(width > 0.0f) || !(height > 0.0f)) return false;

    // SAVE / FORCE / RESTORE, which is exactly the bracket the guest's own CSD
    // project used (it saved six render states, forced its own, and restored
    // them at the end).
    //
    // It is not optional here. This draw lands in the MIDDLE of the frame, and
    // the guest sets its projection and matrices once per camera and then
    // relies on them persisting across every later draw. Leaving an
    // orthographic projection and an identity matrix behind would silently
    // re-transform everything drawn after the last health bar in the frame.
    struct Saved {
        Op op;
        u32 index;
        ShadowSlot slot;
    };
    const std::array<std::pair<Op, u32>, 6> touched = {{{Op::Projection, 0u},
                                                        {Op::PositionMatrix, 0u},
                                                        {Op::CurrentMatrix, 0u},
                                                        {Op::Cull, 0u},
                                                        {Op::DepthMode, 0u},
                                                        {Op::Blend, 0u}}};
    std::array<Saved, 6> saved{};
    for (std::size_t i = 0; i < touched.size(); ++i) {
        saved[i].op = touched[i].first;
        saved[i].index = touched[i].second;
        saved[i].slot = shadow_[static_cast<std::size_t>(touched[i].first)]
                               [touched[i].second % kShadowSlots];
    }

    // The replaced guest callback never ran, so NONE of the 2D pipeline state it
    // would have established is live. Everything this draw depends on has to be
    // stated, and stated through the shadow.
    //
    // An orthographic block mapping the presentation rectangle onto clip space,
    // with y down as the vertices are authored:
    //   x: [0, w] -> [-1, +1]      m00 =  2/w , m02 = -1
    //   y: [0, h] -> [+1, -1]      m11 = -2/h , m12 = +1
    native::Projection projection;
    projection.type = 1u;  // orthographic
    projection.p = {2.0f / width, -1.0f, -2.0f / height, 1.0f, -1.0f, -1.0f};
    emit_state(Op::Projection, 0u, projection);

    native::MatrixLoad identity;
    identity.id = 0u;
    identity.m = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    emit_state(Op::PositionMatrix, identity.id, identity);
    emit_state(Op::CurrentMatrix, 0u, native::Scalar{identity.id, 0u});

    // The bar never occludes and is never occluded. Note this is NOT "depth
    // off": the compare unit stays enabled with GX_ALWAYS and the write
    // disabled, which is what the guest's own ZTESTENABLE = 0 resolves to.
    emit_state(Op::Cull, 0u, native::Scalar{0u, 0u});  // GX_CULL_NONE
    native::DepthMode depth;
    depth.enable = 1u;
    depth.function = 7u;  // GX_ALWAYS
    depth.update = 0u;
    emit_state(Op::DepthMode, 0u, depth);

    // Every cast of this asset authors the same blend: its render flags low
    // byte is 0, which the draw path inverts to select normal alpha. One mode
    // for the whole layer, so one batch.
    native::Blend blend;
    blend.type = 1u;         // GX_BM_BLEND
    blend.source = 4u;       // GX_BL_SRCALPHA
    blend.destination = 5u;  // GX_BL_INVSRCALPHA
    emit_state(Op::Blend, 0u, blend);

    auto draw = zeroed<native::DrawImmediate>();
    draw.producer = native::DrawProducer::Im2D;
    draw.program = native::MaterialProgram::PassColor;
    draw.primitive = 0u;  // GX quads
    draw.anchor = native::DrawAnchorKind::WorldAnchored;
    draw.attributes = native::kImmediatePosition | native::kImmediateColor0;
    draw.vertex_count = vertex_count;
    draw.vertices =
        segment_->add_payload(vertices, vertex_count * sizeof(shadow::render::RenderVertex));
    emit(Op::DrawImmediate, draw);
    ++report_.immediate_draws;

    // Put back exactly what was there. Emitting the saved bytes through the
    // same shadow restores the renderer AND leaves the shadow agreeing with it,
    // so the guest's next set of any of these is deduplicated or emitted on its
    // real merits. A slot that was never valid is left alone: the guest had not
    // set it either, and inventing a value for it would be a state change of
    // our own.
    for (const Saved& s : saved) {
        if (!s.slot.valid) continue;
        ShadowSlot& live = shadow_[static_cast<std::size_t>(s.op)][s.index % kShadowSlots];
        if (live.valid && live.size == s.slot.size &&
            std::memcmp(live.bytes.data(), s.slot.bytes.data(), s.slot.size) == 0) {
            continue;  // untouched by this draw
        }
        switch (s.op) {
            case Op::Projection: {
                native::Projection record{};
                std::memcpy(&record, s.slot.bytes.data(), sizeof(record));
                emit_state(s.op, s.index, record);
                break;
            }
            case Op::PositionMatrix: {
                native::MatrixLoad record{};
                std::memcpy(&record, s.slot.bytes.data(), sizeof(record));
                emit_state(s.op, s.index, record);
                break;
            }
            case Op::CurrentMatrix:
            case Op::Cull: {
                native::Scalar record{};
                std::memcpy(&record, s.slot.bytes.data(), sizeof(record));
                emit_state(s.op, s.index, record);
                break;
            }
            case Op::DepthMode: {
                native::DepthMode record{};
                std::memcpy(&record, s.slot.bytes.data(), sizeof(record));
                emit_state(s.op, s.index, record);
                break;
            }
            case Op::Blend: {
                native::Blend record{};
                std::memcpy(&record, s.slot.bytes.data(), sizeof(record));
                emit_state(s.op, s.index, record);
                break;
            }
            default: break;
        }
    }
    return true;
}

void RwCapture::close_segment(bool present) {
    // The one place the producer can block: queue_->push returns the wait it
    // had to take. A long zone here next to a queue-depth plot pinned at
    // capacity IS backpressure, per frame instead of as one exit-time scalar.
    GEKKO_PROFILE_ZONE("rw/segment-close");
    segment_->set_ends_frame(present);
    segment_->set_close_ns(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count()));
    ++report_.segments;
    report_.payload_bytes += segment_->payload().size();
    // Read before the push: the segment is gone the moment it moves.
    GEKKO_PROFILE_VALUE(segment_->payload().size());
    GEKKO_PROFILE_PLOT("rw/segment-payload", static_cast<double>(segment_->payload().size()));
    GEKKO_PROFILE_PLOT("rw/segment-commands", static_cast<double>(segment_->command_count()));
    if (queue_ != nullptr) {
        report_.queue_wait_ns += queue_->push(std::move(segment_));
        // AFTER the push, so it is the depth the renderer actually faces.
        // depth() takes the queue mutex, which is why it stays behind the
        // connected guard: an unattached profiling build pays nothing.
        if (GEKKO_PROFILE_CONNECTED()) {
            GEKKO_PROFILE_PLOT("rw/segment-queue-depth", static_cast<double>(queue_->depth()));
        }
        segment_ = queue_->acquire();
    } else {
        segment_->clear();
    }
}

void RwCapture::on_state_loaded() {
    ++report_.state_loads;
    for (auto& slots : shadow_) {
        for (ShadowSlot& slot : slots) slot.valid = false;
    }
    textures_.clear();
    geometry_.clear();
    // Every record is gone with the timeline; the identities stay known.
    for (auto& [identity, live] : geometry_identity_live_) live = 0u;
    if (coherence_ != nullptr) coherence_->on_state_loaded();  // STEP-0 measurement, default off
    object_ = ObjectContext{};
    pending_ = PendingBegin{};
    if (segment_ == nullptr) {
        segment_ = queue_ != nullptr ? queue_->acquire() : std::make_unique<native::FrameSegment>();
    }
    // The unfinished frame belongs to the discarded timeline.
    segment_->clear();
    segment_->append_empty(Op::ResetState);
    ++report_.op_counts[static_cast<std::size_t>(Op::ResetState)];
}

void RwCapture::save_state(std::vector<u8>& out) const {
    GEKKO_PROFILE_ZONE("rw/state-save");
    state::put<u32>(out, static_cast<u32>(shadow_.size()));
    state::put<u32>(out, static_cast<u32>(kShadowSlots));
    for (const auto& slots : shadow_) {
        for (const ShadowSlot& slot : slots) {
            state::put<u8>(out, slot.valid ? 1u : 0u);
            if (!slot.valid) continue;
            state::put<u8>(out, slot.size);
            state::put_bytes(out, slot.bytes.data(), slot.size);
        }
    }
}

bool RwCapture::load_state(const u8*& cursor, const u8* end) {
    // This clears textures_ and geometry_, so the whole resource set re-uploads
    // afterwards. Load-time stutter has been a measured defect in this tree and
    // this is the capture's share of it.
    GEKKO_PROFILE_ZONE("rw/state-load");
    u32 ops = 0, slots_per_op = 0;
    if (!state::get<u32>(cursor, end, ops) || !state::get<u32>(cursor, end, slots_per_op)) return false;
    // A different op table (another build) cannot be interpreted: skip it and
    // fall back to invalidation rather than replay mis-numbered state.
    const bool compatible = ops == shadow_.size() && slots_per_op == kShadowSlots;
    std::vector<std::array<ShadowSlot, kShadowSlots>> restored(ops == shadow_.size() ? ops : 0u);
    for (u32 op = 0; op < ops; ++op) {
        for (u32 index = 0; index < slots_per_op; ++index) {
            u8 valid = 0;
            if (!state::get<u8>(cursor, end, valid)) return false;
            if (valid == 0u) continue;
            u8 size = 0;
            if (!state::get<u8>(cursor, end, size) || size > 72u) return false;
            std::array<u8, 72> bytes{};
            if (!state::get_bytes(cursor, end, bytes.data(), size)) return false;
            if (!compatible) continue;
            ShadowSlot& slot = restored[op][index];
            slot.valid = true;
            slot.size = size;
            slot.bytes = bytes;
        }
    }
    on_state_loaded();
    if (!compatible) return true;
    ++report_.state_loads_replayed;
    for (u32 op = 0; op < ops; ++op) {
        // Texture bindings name generations of the discarded timeline; the
        // guest rebinds before it draws and that bind uploads the content.
        if (op == static_cast<u32>(Op::BindTexture)) continue;
        for (u32 index = 0; index < slots_per_op; ++index) {
            const ShadowSlot& slot = restored[op][index];
            if (!slot.valid) continue;
            shadow_[op][index] = slot;
            segment_->append_bytes(static_cast<Op>(op), slot.bytes.data(), slot.size);
            ++report_.op_counts[op];
            ++report_.state_commands_replayed;
        }
    }
    return true;
}

// ---- GX state API -----------------------------------------------------------

void RwCapture::gx_set_viewport(Cpu& cpu) {
    auto v = zeroed<native::Viewport>();
    v.x = static_cast<float>(fpr_value(cpu, 1));
    v.y = static_cast<float>(fpr_value(cpu, 2));
    v.width = static_cast<float>(fpr_value(cpu, 3));
    v.height = static_cast<float>(fpr_value(cpu, 4));
    v.near_z = static_cast<float>(fpr_value(cpu, 5));
    v.far_z = static_cast<float>(fpr_value(cpu, 6));
    v.field = -1;
    emit_state(Op::Viewport, 0, v);
}

void RwCapture::gx_set_viewport_jitter(Cpu& cpu) {
    auto v = zeroed<native::Viewport>();
    v.x = static_cast<float>(fpr_value(cpu, 1));
    v.y = static_cast<float>(fpr_value(cpu, 2));
    v.width = static_cast<float>(fpr_value(cpu, 3));
    v.height = static_cast<float>(fpr_value(cpu, 4));
    v.near_z = static_cast<float>(fpr_value(cpu, 5));
    v.far_z = static_cast<float>(fpr_value(cpu, 6));
    v.field = static_cast<s32>(cpu.gpr[3] & 1u);
    emit_state(Op::Viewport, 0, v);
}

void RwCapture::gx_set_scissor(Cpu& cpu) {
    auto s = zeroed<native::Scissor>();
    s.x = static_cast<s32>(cpu.gpr[3]);
    s.y = static_cast<s32>(cpu.gpr[4]);
    s.width = static_cast<s32>(cpu.gpr[5]);
    s.height = static_cast<s32>(cpu.gpr[6]);
    emit_state(Op::Scissor, 0, s);
}

// The glare object is reached through the ScreenEffect singleton:
//     G = [ [0x805EF2D8] + 0x3C ]
// and the gate the guest itself tests is
//     [G's owner + 0x08] != 0  &&  E0 != 0  &&  (E1 != 0 || E2 != 0)
// Both entry points take `this` in r3, so the object is read from the argument
// rather than from the singleton - that is what the guest does, and it is also
// correct on the split-screen path where a second object exists.
void RwCapture::glare_window(Cpu& cpu, u32 phase) {
    const u32 object = cpu.gpr[3];
    if (object == 0u) return;
    auto g = zeroed<native::GlareWindow>();
    g.phase = phase;
    g.object = object;
    g.enable = read8(object + 0xE0u);
    g.enable_a = read8(object + 0xE1u);
    g.enable_b = read8(object + 0xE2u);
    g.separate = read8(object + 0xE3u);
    g.b_from_source = read8(object + 0xE4u);
    g.intensity_a = read8(object + 0xE5u);
    g.intensity_b = read8(object + 0xE6u);
    g.spread_a = read_f32(object + 0xE8u);
    g.spread_b = read_f32(object + 0xECu);
    g.taps_ah = static_cast<s32>(read32(object + 0xF0u));
    g.taps_av = static_cast<s32>(read32(object + 0xF4u));
    g.taps_bh = static_cast<s32>(read32(object + 0xF8u));
    g.taps_bv = static_cast<s32>(read32(object + 0xFCu));
    g.gate = (g.enable != 0u && (g.enable_a != 0u || g.enable_b != 0u)) ? 1u : 0u;
    emit_state(Op::GlareWindow, 0, g);
}

void RwCapture::glare_pre(Cpu& cpu) { glare_window(cpu, native::kGlarePreGlare); }
void RwCapture::glare_post(Cpu& cpu) { glare_window(cpu, native::kGlarePostGlare); }

void RwCapture::gx_set_scissor_offset(Cpu& cpu) {
    auto s = zeroed<native::ScissorOffset>();
    s.x = static_cast<s32>(cpu.gpr[3]);
    s.y = static_cast<s32>(cpu.gpr[4]);
    emit_state(Op::ScissorOffset, 0, s);
}

void RwCapture::gx_set_projection(Cpu& cpu) {
    // GXSetProjection(Mtx44 m, GXProjectionType type): the SDK stores the six
    // coefficients XF consumes, choosing column 2 terms for perspective and
    // column 3 translations for orthographic.
    const u32 m = cpu.gpr[3];
    auto p = zeroed<native::Projection>();
    p.type = cpu.gpr[4] != 0u ? 1u : 0u;
    const auto at = [&](u32 row, u32 column) { return read_f32(m + (row * 4u + column) * 4u); };
    p.p[0] = at(0, 0);
    p.p[1] = p.type == 0u ? at(0, 2) : at(0, 3);
    p.p[2] = at(1, 1);
    p.p[3] = p.type == 0u ? at(1, 2) : at(1, 3);
    p.p[4] = at(2, 2);
    p.p[5] = at(2, 3);
    emit_state(Op::Projection, 0, p);
}

void RwCapture::gx_set_projection_v(Cpu& cpu) {
    const u32 v = cpu.gpr[3];
    auto p = zeroed<native::Projection>();
    p.type = read_f32(v) == 0.0f ? 0u : 1u;
    for (u32 index = 0; index < 6u; ++index) p.p[index] = read_f32(v + 4u + index * 4u);
    emit_state(Op::Projection, 0, p);
}

void RwCapture::gx_load_position_matrix(Cpu& cpu) {
    auto m = zeroed<native::MatrixLoad>();
    m.id = cpu.gpr[4];
    for (u32 index = 0; index < 12u; ++index) m.m[index] = read_f32(cpu.gpr[3] + index * 4u);
    emit_state(Op::PositionMatrix, m.id, m);
}

void RwCapture::gx_load_normal_matrix(Cpu& cpu) {
    // GXLoadNrmMtxImm(const Mtx mt, u32 id) takes a 3x4 Mtx and loads its
    // upper-left 3x3: the translation column (elements 3, 7, 11) is skipped.
    auto m = zeroed<native::MatrixLoad>();
    m.id = cpu.gpr[4];
    static constexpr u32 kElements[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
    for (u32 index = 0; index < 9u; ++index) {
        m.m[index] = read_f32(cpu.gpr[3] + kElements[index] * 4u);
    }
    emit_state(Op::NormalMatrix, m.id, m);
}

void RwCapture::gx_set_current_matrix(Cpu& cpu) {
    auto s = zeroed<native::Scalar>();
    s.value = cpu.gpr[3];
    emit_state(Op::CurrentMatrix, 0, s);
}

void RwCapture::gx_load_texture_matrix(Cpu& cpu) {
    auto m = zeroed<native::MatrixLoad>();
    m.id = cpu.gpr[4];
    m.type = cpu.gpr[5];
    const u32 values = m.type == 1u ? 8u : 12u;
    for (u32 index = 0; index < values; ++index) m.m[index] = read_f32(cpu.gpr[3] + index * 4u);
    emit_state(Op::TextureMatrix, m.id, m);
}

void RwCapture::gx_set_blend_mode(Cpu& cpu) {
    auto b = zeroed<native::Blend>();
    b.type = static_cast<u8>(cpu.gpr[3]);
    b.source = static_cast<u8>(cpu.gpr[4]);
    b.destination = static_cast<u8>(cpu.gpr[5]);
    b.logic = static_cast<u8>(cpu.gpr[6]);
    emit_state(Op::Blend, 0, b);
}

void RwCapture::gx_set_z_mode(Cpu& cpu) {
    auto d = zeroed<native::DepthMode>();
    d.enable = static_cast<u8>(cpu.gpr[3] != 0u);
    d.function = static_cast<u8>(cpu.gpr[4] & 7u);
    d.update = static_cast<u8>(cpu.gpr[5] != 0u);
    emit_state(Op::DepthMode, 0, d);
}

void RwCapture::gx_set_z_comp_loc(Cpu& cpu) {
    auto s = zeroed<native::Scalar>();
    s.value = cpu.gpr[3] != 0u ? 1u : 0u;
    emit_state(Op::DepthBeforeTexture, 0, s);
}

void RwCapture::gx_set_alpha_compare(Cpu& cpu) {
    auto a = zeroed<native::AlphaCompare>();
    a.comp0 = static_cast<u8>(cpu.gpr[3] & 7u);
    a.ref0 = static_cast<u8>(cpu.gpr[4]);
    a.op = static_cast<u8>(cpu.gpr[5] & 3u);
    a.comp1 = static_cast<u8>(cpu.gpr[6] & 7u);
    a.ref1 = static_cast<u8>(cpu.gpr[7]);
    emit_state(Op::AlphaCompare, 0, a);
}

void RwCapture::gx_set_cull_mode(Cpu& cpu) {
    auto s = zeroed<native::Scalar>();
    s.value = cpu.gpr[3] & 3u;
    emit_state(Op::Cull, 0, s);
}

void RwCapture::gx_set_fog(Cpu& cpu) {
    auto f = zeroed<native::Fog>();
    f.type = cpu.gpr[3];
    f.start = static_cast<float>(fpr_value(cpu, 1));
    f.end = static_cast<float>(fpr_value(cpu, 2));
    f.near_z = static_cast<float>(fpr_value(cpu, 3));
    f.far_z = static_cast<float>(fpr_value(cpu, 4));
    if (const u8* color = guest(cpu.gpr[4], 4u)) {
        std::memcpy(f.rgba.data(), color, 4u);
    }
    emit_state(Op::Fog, 0, f);
}

void RwCapture::gx_set_color_update(Cpu& cpu) {
    auto s = zeroed<native::Scalar>();
    s.value = cpu.gpr[3] != 0u ? 1u : 0u;
    emit_state(Op::ColorUpdate, 0, s);
}

void RwCapture::gx_set_alpha_update(Cpu& cpu) {
    auto s = zeroed<native::Scalar>();
    s.value = cpu.gpr[3] != 0u ? 1u : 0u;
    emit_state(Op::AlphaUpdate, 0, s);
}

void RwCapture::gx_set_dst_alpha(Cpu& cpu) {
    auto s = zeroed<native::Scalar>();
    s.value = cpu.gpr[3] != 0u ? 1u : 0u;
    s.value2 = cpu.gpr[4] & 0xffu;
    emit_state(Op::DestinationAlpha, 0, s);
}

void RwCapture::gx_set_num_chans(Cpu& cpu) {
    auto s = zeroed<native::Scalar>();
    s.value = cpu.gpr[3];
    emit_state(Op::ChannelCount, 0, s);
}

// GX channel IDs alias: COLOR0A0 (4) writes COLOR0 (0) and ALPHA0 (2), COLOR1A1
// (5) writes COLOR1 (1) and ALPHA1 (3). A deduplication slot is only a valid
// record of the hardware state while no aliasing ID has written over it since.
void RwCapture::invalidate_channel_aliases(Op op, u32 channel) {
    auto& slots = shadow_[static_cast<std::size_t>(op)];
    switch (channel) {
        case 0: case 2: slots[4].valid = false; break;
        case 1: case 3: slots[5].valid = false; break;
        case 4: slots[0].valid = false; slots[2].valid = false; break;
        case 5: slots[1].valid = false; slots[3].valid = false; break;
        default: break;
    }
}

void RwCapture::gx_set_chan_ctrl(Cpu& cpu) {
    auto c = zeroed<native::ChannelControl>();
    c.channel = static_cast<u8>(cpu.gpr[3]);
    c.enable = static_cast<u8>(cpu.gpr[4] != 0u);
    c.ambient_source = static_cast<u8>(cpu.gpr[5]);
    c.material_source = static_cast<u8>(cpu.gpr[6]);
    c.light_mask = static_cast<u8>(cpu.gpr[7]);
    c.diffuse = static_cast<u8>(cpu.gpr[8]);
    c.attenuation = static_cast<u8>(cpu.gpr[9]);
    emit_state(Op::ChannelControl, c.channel, c);
    invalidate_channel_aliases(Op::ChannelControl, c.channel);
}

void RwCapture::color_state(Cpu& cpu, Op op) {
    auto c = zeroed<native::ChannelColor>();
    c.channel = cpu.gpr[3];
    if (const u8* color = guest(cpu.gpr[4], 4u)) {
        std::memcpy(c.rgba.data(), color, 4u);
    }
    emit_state(op, c.channel, c);
    invalidate_channel_aliases(op, c.channel);
}

void RwCapture::gx_set_chan_mat_color(Cpu& cpu) { color_state(cpu, Op::MaterialColor); }
void RwCapture::gx_set_chan_amb_color(Cpu& cpu) { color_state(cpu, Op::AmbientColor); }

void RwCapture::gx_load_light(Cpu& cpu) {
    const u32 object = cpu.gpr[3];
    auto l = zeroed<native::Light>();
    l.mask = cpu.gpr[4];
    if (const u8* color = guest(object + 12u, 4u)) std::memcpy(l.rgba.data(), color, 4u);
    for (u32 i = 0; i < 3u; ++i) {
        l.cos_attenuation[i] = read_f32(object + 16u + i * 4u);
        l.dist_attenuation[i] = read_f32(object + 28u + i * 4u);
        l.position[i] = read_f32(object + 40u + i * 4u);
        l.direction[i] = read_f32(object + 52u + i * 4u);
    }
    u32 index = 0;
    while (index < 8u && ((l.mask >> index) & 1u) == 0u) ++index;
    emit_state(Op::Light, index, l);
}

void RwCapture::gx_set_num_tex_gens(Cpu& cpu) {
    auto s = zeroed<native::Scalar>();
    s.value = cpu.gpr[3];
    emit_state(Op::TexGenCount, 0, s);
}

void RwCapture::gx_set_tex_coord_gen(Cpu& cpu) {
    auto t = zeroed<native::TexGen>();
    t.coord = static_cast<u8>(cpu.gpr[3]);
    t.function = static_cast<u8>(cpu.gpr[4]);
    t.source = static_cast<u8>(cpu.gpr[5]);
    t.matrix = cpu.gpr[6];
    t.normalize = static_cast<u8>(cpu.gpr[7] != 0u);
    t.post_matrix = cpu.gpr[8];
    emit_state(Op::TexGen, t.coord, t);
}

void RwCapture::gx_set_tev_color(Cpu& cpu) {
    auto t = zeroed<native::TevColor>();
    t.index = cpu.gpr[3];
    if (const u8* color = guest(cpu.gpr[4], 4u)) {
        for (u32 i = 0; i < 4u; ++i) t.rgba[i] = static_cast<s16>(color[i]);
    }
    emit_state(Op::TevRegisterColor, t.index, t);
}

void RwCapture::gx_set_tev_konst_color(Cpu& cpu) {
    auto t = zeroed<native::TevColor>();
    t.index = cpu.gpr[3];
    if (const u8* color = guest(cpu.gpr[4], 4u)) {
        for (u32 i = 0; i < 4u; ++i) t.rgba[i] = static_cast<s16>(color[i]);
    }
    emit_state(Op::TevKonstColor, t.index, t);
}

// ---- draw seals -------------------------------------------------------------

void RwCapture::gx_begin(Cpu& cpu) {
    PendingBegin pending;
    pending.valid = true;
    pending.return_address = cpu.lr;
    pending.opcode = static_cast<u8>((cpu.gpr[3] & 0xf8u) | (cpu.gpr[4] & 7u));
    pending.count = cpu.gpr[5] & 0xffffu;
    bool known = false;
    for (const BeginSite& site : kBeginSites) {
        if (site.return_address == cpu.lr) {
            pending.producer = site.producer;
            pending.clear_quad = site.clear_quad;
            known = true;
            break;
        }
    }
    if (!known) {
        if (report_.unknown_begin_sites++ == 0u) report_.first_unknown_begin_lr = cpu.lr;
    }
    switch (pending.producer) {
        case native::DrawProducer::Im2D:
        case native::DrawProducer::Im2DTriangle:
        case native::DrawProducer::Im2DLine:
        case native::DrawProducer::Im2DIndexed:
        case native::DrawProducer::RasterRender: {
            // Im2DBegin / SubmitNoLight choose MODULATE when the Im texture has a
            // raster, PASSCLR otherwise, and bind it before GXBegin
            // (0x80491538 lwz r3,0x620c(r13) / 0x8049153C lwz r0,0(r3) / beq).
            const u32 im_texture = read32(kImTexture);
            pending.program = (im_texture != 0u && read32(im_texture) != 0u)
                                  ? native::MaterialProgram::Modulate
                                  : native::MaterialProgram::PassColor;
            break;
        }
        case native::DrawProducer::Im3D: {
            // RwIm3D decides on its own vertex layout, not on the Im texture:
            // rwIM3D_VERTEXUV in the transform flags selects MODULATE, its
            // absence PASSCLR (a NULL raster still gets MODULATE when UVs are
            // declared - the program is reproduced, not the stale texture).
            // Nothing stores to the flags between the guest's test and GXBegin.
            const bool vertex_uv = (read32(read32(kIm3DContext) + 0x0Cu) & 1u) != 0u;
            const u32 im_texture = read32(kImTexture);
            const bool raster_present = im_texture != 0u && read32(im_texture) != 0u;
            if (vertex_uv != raster_present) ++report_.im3d_program_proxy_disagreements;
            pending.program = vertex_uv ? native::MaterialProgram::Modulate
                                        : native::MaterialProgram::PassColor;
            break;
        }
        case native::DrawProducer::PTank: {
            // fn_80449D54 binds a texture and programs MODULATE only when the
            // PTank's flags carry texture/UV bits (& 0x84); otherwise it
            // programs PASSCLR with no TEX0 in the vertex descriptor. The flags
            // were read at the render callback's entry (r27 is reused before
            // GXBegin).
            const bool textured = object_.pipeline == Pipeline::PTank &&
                                  (object_.geometry_flags & 0x84u) != 0u;
            if (!textured) ++report_.ptank_untextured;
            pending.program = textured ? native::MaterialProgram::Modulate
                                       : native::MaterialProgram::PassColor;
            break;
        }
        default:
            pending.program = native::MaterialProgram::PassColor;
            break;
    }
    pending_ = pending;
}

void RwCapture::on_primitive(u8 opcode, u32 vertex_count, const VertexFormat& format, u32 vat_a,
                             u32 vat_b, u32 vat_c, const u8* payload,
                             std::size_t payload_bytes) {
    const u64 begin = tsc_now();
    // Only GXBegin's immediate primitives arrive here (~9k/s), from the
    // immediate geometry builder; display lists are never walked (their
    // geometry is the content-owned store's).
    //
    // Note the function already brackets itself with tsc_now for report_.tap_ns,
    // so in a profiling build that counter includes this zone's own overhead -
    // take the self-cost baseline on build-native.
    GEKKO_PROFILE_ZONE("rw/immediate-decode");
    GEKKO_PROFILE_VALUE(vertex_count);
    if (gx_input_digest_enabled_ && pending_.valid) {
        u64 digest = report_.gx_input_digest;
        const auto mix = [&digest](u32 value, int bytes) {
            for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
                digest = (digest ^ static_cast<u8>(value >> shift)) * 0x100000001B3ull;
            }
        };
        mix(opcode, 1);
        mix(vertex_count, 2);
        mix(vat_a, 4);
        mix(vat_b, 4);
        mix(vat_c, 4);
        mix(format.stride, 4);
        for (std::size_t index = 0; payload != nullptr && index < payload_bytes; ++index) {
            digest = (digest ^ payload[index]) * 0x100000001B3ull;
        }
        report_.gx_input_digest = digest;
        ++report_.gx_input_primitives;
        report_.gx_input_payload_bytes += payload_bytes;
    }
    if (!pending_.valid) {
        if (report_.untracked_primitives++ == 0u) report_.first_untracked_opcode = opcode;
        report_.tap_ns += tsc_now() - begin;
        return;
    }
    const PendingBegin pending = pending_;
    pending_.valid = false;
    if (pending.opcode != opcode || pending.count != vertex_count) {
        ++report_.immediate_count_mismatch;
    }
    if (pending.clear_quad) {
        // RwCameraClear was already emitted as a semantic clear at its entry.
        ++report_.clear_quads_absorbed;
        report_.tap_ns += tsc_now() - begin;
        return;
    }
    if (vertex_count == 0u || payload == nullptr || payload_bytes == 0u) {
        report_.tap_ns += tsc_now() - begin;
        return;
    }
    u32 attributes = 0;
    if (!decode_immediate(format, vat_a, vat_b, vat_c, payload, vertex_count, vertex_scratch_,
                          attributes)) {
        ++report_.immediate_indexed_attributes;
        report_.tap_ns += tsc_now() - begin;
        return;
    }
    auto draw = zeroed<native::DrawImmediate>();
    draw.producer = pending.producer;
    draw.program = pending.program;
    draw.primitive = static_cast<u8>((opcode >> 3) & 7u);
    draw.attributes = attributes;
    draw.vertex_count = vertex_count;
    draw.return_address = pending.return_address;
    // Every CAPTURED immediate draw is authored in 2D by the guest and belongs
    // to the HUD, so it continues through HUD anchoring. World-anchored draws
    // do not arrive here at all: they are produced by a host module that has
    // replaced its guest path, and submit_native_ui stamps them itself.
    //
    // This used to read a global latch that a pair of scope hooks was supposed
    // to maintain. That pair never worked - the opening half was registered at
    // an address a later registration overwrote - and it could not have been
    // made to work, because HLE dispatches at function entry only, so there was
    // no exit hook to close the scope and it leaked over every later 2D draw in
    // the frame. Classification belongs on the draw record; a latch cannot
    // express it correctly.
    draw.anchor = native::DrawAnchorKind::PureHud;
    draw.vertices = segment_->add_payload(vertex_scratch_.data(),
                                          vertex_count * sizeof(shadow::render::RenderVertex));
    emit(Op::DrawImmediate, draw);
    ++report_.immediate_draws;
    report_.tap_ns += tsc_now() - begin;
}

void RwCapture::on_copy_trigger(u32) {
    // The RwGC hooks carry the semantic copy (CameraTextureCopy) and the present
    // (RasterShowRaster). The trigger is only counted, as the cross-check that
    // no copy was requested without the driver entry that owns it.
    ++report_.gx_copy_triggers;
}

void RwCapture::on_hud_group(HudGroupKind kind, u32 group_id, float canvas_x, u32 instance) {
    auto mark = zeroed<native::HudGroup>();
    mark.kind = static_cast<u32>(kind);
    mark.group_id = group_id;
    mark.instance = instance;
    mark.canvas_x = canvas_x;
    emit(Op::HudGroup, mark);
}

void RwCapture::on_hud_scene(u32 node, u32 cast) {
    auto mark = zeroed<native::HudScene>();
    mark.node = node;
    mark.cast = cast;
    emit(Op::HudScene, mark);
}

void RwCapture::gx_call_display_list(Cpu& cpu) {
    // ~140 per frame, and nothing measured it: the body is a linear scan of up
    // to mesh_count bounds-checked guest reads plus a 15-entry return-address
    // scan. Both early exits below are zero on the measured routes, so the
    // whole-body placement costs nothing they would have avoided.
    GEKKO_PROFILE_ZONE("rw/display-list-draw");
    const DisplayListSite* site = nullptr;
    for (const DisplayListSite& candidate : kDisplayListSites) {
        if (candidate.return_address == cpu.lr) {
            site = &candidate;
            break;
        }
    }
    if (site == nullptr) {
        if (report_.unknown_display_list_sites++ == 0u) {
            report_.first_unknown_display_list_lr = cpu.lr;
        }
        return;
    }
    if (!object_.valid) {
        ++report_.display_list_without_object;
        return;
    }
    // Attached here rather than at the top: the number only means something
    // once the object context is known to be live - it is the length of the
    // mesh-record scan below.
    GEKKO_PROFILE_VALUE(object_.mesh_count);
    const u32 display_list = cpu.gpr[3];
    const u32 attributes = read32(object_.header + 8u);
    const u32 mesh_records = object_.header + 12u + attributes * 8u;
    u32 mesh_index = 0xffffffffu;
    for (u32 index = 0; index < object_.mesh_count; ++index) {
        const u32 record = mesh_records + index * 8u;
        if (guest(record, 8u) == nullptr) break;
        if (read32(record) == display_list) {
            mesh_index = index;
            break;
        }
    }
    if (mesh_index == 0xffffffffu) {
        ++report_.display_list_mesh_not_found;
        return;
    }
    auto draw = zeroed<native::DrawMesh>();
    draw.producer = site->producer;
    draw.mesh_index = static_cast<u16>(mesh_index);
    draw.geometry = object_.geometry;
    draw.return_address = cpu.lr;
    draw.object = object_.object;
    draw.geometry_flags = object_.geometry_flags;
    const bool textured = (object_.geometry_flags & 0x84u) != 0u;
    switch (site->pass) {
        case MeshPass::DefaultObject: {
            // RwGC 0x80466624: A = T P X -> two-stage program; B = T, not (P X) ->
            // TX1 (texture x rasterised = MODULATE); C = untextured -> UT1.
            const bool prelit = (object_.geometry_flags & 0x08u) != 0u;
            const bool ambient = object_.has_ambient == 1u;
            if (!textured) {
                draw.program = native::MaterialProgram::DefaultUt1;
            } else if (prelit && ambient) {
                draw.program = native::MaterialProgram::DefaultTx2;
            } else {
                draw.program = native::MaterialProgram::Modulate;
            }
            break;
        }
        case MeshPass::MatFxBase:
            draw.program = textured ? native::MaterialProgram::Modulate
                                    : native::MaterialProgram::PassColor;
            break;
        case MeshPass::MatFxTextured:
            draw.program = native::MaterialProgram::Modulate;
            break;
        case MeshPass::MatFxUnsupported:
            draw.program = native::MaterialProgram::Unsupported;
            ++report_.unsupported_programs;
            break;
    }
    emit(Op::DrawMesh, draw);
    ++report_.mesh_draws;
}

// ---- RwGC semantics ---------------------------------------------------------

// THE OTHER HALF OF THE UPLOAD CONTRACT.
//
// Until this existed, `record.uploaded_generation = record.generation` was set
// the instant a blob was EMITTED, and the renderer had five ways to refuse it
// afterwards with no way to say so. Content is re-sent only when the
// generation changes, and for a static world sector that never happens - so a
// single refusal removed the object for the rest of the stage.
//
// The retry is bounded on purpose. A display list the converter cannot walk
// fails identically every time; re-sending it every frame forever would trade
// a silent loss for a silent cost. When the budget is spent the object stays
// missing and `retries_exhausted` plus the per-reason histogram say which and
// why - no silent drops AND no endless retry loops.
void RwCapture::drain_rejections() {
    if (queue_ == nullptr) return;
    // Scoped so this zone ends before the apply zone below begins - the two
    // measure different things and must not nest. The mailbox holds its own
    // mutex and this is the single point where the guest thread takes a lock
    // the render thread writes under, twice per call (drain, then stats). The
    // lock itself belongs to native_frame, which this subsystem does not own,
    // so a zone on the waiting side is the only instrument available.
    {
        GEKKO_PROFILE_ZONE("rw/rejection-drain");
        rejection_scratch_.clear();
        queue_->rejections().drain(rejection_scratch_);
        // Read back rather than left write-only: an overflow means a rejection was
        // DROPPED, and a dropped rejection is an object permanently lost.
        const auto mailbox = queue_->rejections().stats();
        report_.rejections_lost_to_overflow = mailbox.overflow;
        report_.rejections_coalesced = mailbox.coalesced;
        GEKKO_PROFILE_VALUE(rejection_scratch_.size());
    }
    if (rejection_scratch_.empty()) return;
    // After the empty check, so the timeline shows a zone only when something
    // was actually refused: a refusal storm is then a visible cluster rather
    // than a flat line of empty zones.
    GEKKO_PROFILE_ZONE("rw/rejection-apply");
    GEKKO_PROFILE_VALUE(rejection_scratch_.size());

    // Small enough that a deterministic converter refusal costs a bounded
    // number of re-parses, large enough to ride out a transient staging
    // exhaustion. Not a tuning knob: if this ever needs to be larger, the
    // reason it needs to be larger is the actual defect.
    constexpr u8 kMaxRetries = 3;
    // The budget bounds CONSECUTIVE failures. An object whose last refusal was
    // this long ago has demonstrably recovered since, so its budget is
    // restored rather than held against it for the rest of the stage. Without
    // this, three refusals spread across a whole stage permanently disabled
    // retry for an object that recovered from every one of them.
    constexpr u64 kRetryDecayPresents = 60;

    for (const native::ResourceRejection& rejection : rejection_scratch_) {
        ++report_.rejections_received;
        const auto reason_index = static_cast<std::size_t>(rejection.reason);
        if (reason_index < report_.reject_reasons.size()) ++report_.reject_reasons[reason_index];

        if (rejection.kind == native::ResourceKind::Geometry) {
            auto it = geometry_.find(rejection.address);
            // Keyed on the CONTENT's generation, not the record's current one.
            // If the record has already moved on, the refusal describes a blob
            // nobody is waiting for and re-sending it would be wrong.
            if (it == geometry_.end() || it->second.generation != rejection.generation) {
                ++report_.rejections_unmatched;
                continue;
            }
            GeometryRecord& record = it->second;
            if (report_.presents - record.last_reject_present > kRetryDecayPresents) {
                record.retries = 0;
            }
            record.last_reject = rejection.reason;
            record.last_reject_present = report_.presents;
            if (record.retries >= kMaxRetries) {
                ++report_.retries_exhausted;
                continue;
            }
            ++record.retries;
            record.needs_reupload = true;
            ++report_.reuploads_requested;
            continue;
        }

        auto it = textures_.find(rejection.address);
        if (it == textures_.end() || it->second.generation != rejection.generation) {
            ++report_.rejections_unmatched;
            continue;
        }
        TextureRecord& record = it->second;
        if (report_.presents - record.last_reject_present > kRetryDecayPresents) {
            record.retries = 0;
        }
        record.last_reject = rejection.reason;
        record.last_reject_present = report_.presents;
        if (record.retries >= kMaxRetries) {
            ++report_.retries_exhausted;
            continue;
        }
        ++record.retries;
        record.needs_reupload = true;
        ++report_.reuploads_requested;
    }
}

bool RwCapture::prepare_geometry(Cpu& cpu, ObjectContext& context) {
    drain_rejections();
    const u32 entry = context.entry;
    if (entry == 0u || guest(entry, 24u) == nullptr) {
        ++report_.geometry_invalid_entries;
        return false;
    }
    const u32 header = entry + 24u;
    const u32 size = read32(entry + 8u);
    const u16 serial = read16(header + 2u);
    const u32 attributes = read32(header + 8u);
    if (attributes > 16u || size < 12u || size > (32u << 20) || guest(header, size) == nullptr) {
        ++report_.geometry_invalid_entries;
        return false;
    }
    context.header = header;
    context.header_flags = read32(header + 4u);
    GeometryRecord& record = geometry_[entry];
    const bool first_for_entry = record.generation == 0u;
    if (record.generation == 0u || record.serial != serial || record.size != size) {
        if (record.uploaded_generation != 0u) {
            auto release = zeroed<native::ResourceKey>();
            release.address = entry;
            release.generation = record.generation;
            emit(Op::ReleaseGeometry, release);
            ++report_.geometry_releases;
            ++report_.geometry_release_regenerate;
        }
        release_identity(record);
        record.generation = next_generation();
        record.serial = serial;
        record.size = size;
        record.uploaded_generation = 0u;
        // New content deserves a fresh attempt: the spent budget belonged to a
        // blob that no longer exists.
        record.needs_reupload = false;
        record.retries = 0;
        record.last_reject = native::RejectReason::Unknown;
    }
    context.geometry.address = entry;
    context.geometry.generation = record.generation;

    // Object vertex count: attribute arrays of runtime-instanced models have one
    // entry per RpGeometry vertex. World sectors keep every array inside the
    // entry, so the count is only needed for arrays that live outside it.
    const bool sector = static_cast<s32>(read32(context.object)) == -1;
    const u32 geometry = sector ? 0u : read32(context.object + 0x18u);
    const u32 vertex_count = sector ? read16(context.object + 0x82u) : read32(geometry + 0x14u);

    // `|| record.needs_reupload` is the recovery. Without it a refused blob is
    // never sent again: the latch below has no idea the renderer threw it away.
    // (Hoisted into a local, unchanged, only so the STEP-0 coherence probe below
    // can tell an upload from the skip-upload branch.)
    const bool upload_now = record.uploaded_generation != record.generation || record.needs_reupload;
    if (upload_now) {
        // Per GEOMETRY, not per vertex. This branch is the largest static byte
        // producer in the subsystem and nothing today separates its memcpys
        // from the bounds checks that surround them.
        GEKKO_PROFILE_ZONE("rw/geometry-upload");
        // The instance blob, plus any attribute array that lives in a separate
        // allocation (CPU-skin output buffers), appended and re-pointed in the
        // host copy of the header. Guest memory is not modified.
        const u32 blob_size = guest(header, size + 32u) != nullptr
                                  ? size + 32u
                                  : size;
        std::vector<std::pair<u32, u32>> external;  // (record offset in blob, bytes)
        u32 total = blob_size;
        for (u32 index = 0; index < attributes; ++index) {
            const u32 record_at = header + 12u + index * 8u;
            const u32 array = read32(record_at);
            const u32 stride = read8(record_at + 5u);
            if (array >= header && array < header + blob_size) continue;
            const u32 bytes = stride * vertex_count;
            if (array == 0u || bytes == 0u || guest(array, bytes) == nullptr) continue;
            external.emplace_back(12u + index * 8u, bytes);
            total += (bytes + 3u) & ~3u;
        }
        // `total` is final here: instance blob plus every appended array, which
        // is exactly the payload this upload will cost.
        GEKKO_PROFILE_VALUE(total);
        auto content = zeroed<native::GeometryContent>();
        content.geometry = context.geometry;
        content.geometry_flags = context.geometry_flags;
        content.world_sector = sector ? 1u : 0u;
        content.mesh_count = context.mesh_count;
        content.dynamic = (context.pipeline == Pipeline::Skin ||
                           context.pipeline == Pipeline::SkinMatFx) ? 1u : 0u;
        // Does this geometry's display list carry a per-vertex matrix index?
        // A world sector never does. For a model, RpSkin's block answers it, and
        // the answer changes the vertex tuple WIDTH - so getting it wrong does
        // not mis-shade anything, it desynchronises the display-list walk and
        // loses the whole object. Same rule the converter's memory-dump corpus
        // test uses, read from the guest rather than assumed.
        if (!sector && geometry != 0u) {
            const u32 skin_offset = read32(kSkinGeometryOffset);
            const u32 skin = skin_offset != 0u ? read32(geometry + skin_offset) : 0u;
            if (skin != 0u && guest(skin, 0x30u) != nullptr && read32(skin + 0x10u) == 1u &&
                read32(skin + 0x2cu) != 3u) {
                content.matrix_palette = 1u;
            }
        }
        u32 format = 0;
        if (sector) {
            const u32 globals = read32(kEngineInstance);
            const u32 world = read32(globals + 4u);
            format = world != 0u ? read32(world + read32(kWorldVtxFmtOffset)) : 0u;
        } else if (geometry != 0u) {
            format = read32(geometry + read32(kGeometryVtxFmtOffset));
        }
        if (format == 0u) format = kDefaultVtxFmt;
        if (const u8* bytes = guest(format, 24u)) {
            std::memcpy(content.vertex_format.data(), bytes, 24u);
        }
        u8* out = nullptr;
        content.blob = segment_->reserve_payload(total, out);
        std::memcpy(out, guest(header, blob_size), blob_size);
        u32 cursor = blob_size;
        for (const auto& [record_offset, bytes] : external) {
            const u32 array = load_be32(out + record_offset);
            std::memcpy(out + cursor, guest(array, bytes), bytes);
            store_be32(out + record_offset, header + cursor);
            cursor += (bytes + 3u) & ~3u;
        }
        // THE PAYLOAD IS MADE ADDRESS-INDEPENDENT, so that what the renderer
        // receives is the geometry and not the place the guest's instance cache
        // happened to put it.
        //
        // The guest's RenderWare resource arena is fixed at 256 KiB at boot
        // (RwEngineInit(.., 0x40000) at 0x800516C4). When
        // RwResourcesAllocateResEntry (0x804818C8) cannot fit a new instance it
        // evicts least-recently-used entries - the not-used-since-present list
        // first, then entries used THIS frame - and the next draw of an evicted
        // object re-instances it into a new entry. On Death Ruins run-forward
        // every one of 7,351 releases is such an eviction and 7,276 of 7,392
        // uploads are byte-identical to content released earlier. Those bytes
        // differ between instances only in:
        //   +0 u16  the PE draw-sync token, re-stamped by every render callback
        //   +2 u16  the mesh serial of the allocation
        //   the embedded pointers (attribute record +0, mesh record +0), which
        //           are absolute guest addresses of this entry's own interior
        // So: the token and serial are zeroed (the converter stores them and
        // derives nothing from them), and every embedded pointer becomes an
        // offset from payload byte 0, which the converter resolves against a
        // base of 0. An in-blob pointer maps into [0, total) exactly when its
        // absolute address was inside [header, header + total), so what
        // resolves, and what fails to, is unchanged.
        {
            const u32 blob_attributes = load_be32(out + 8u);
            const u32 records_end = 12u + 8u * (blob_attributes + content.mesh_count);
            std::memset(out, 0, 4u);
            for (u32 at = 12u; at + 4u <= records_end && at + 4u <= total; at += 8u) {
                store_be32(out + at, load_be32(out + at) - header);
            }
        }
        {
            // CONTENT IDENTITY - the key of the renderer's persistent geometry
            // store. A lookup key only: the store proves equality by comparing
            // every byte the conversion consumed, so a collision here can cost a
            // hit, never produce wrong geometry. Covers the conversion
            // parameters and [4, size) of the normalised instance; neither the
            // 32-byte arena tail past `size` (the next heap block's header) nor
            // the appended per-frame skin arrays belong to the identity.
            GEKKO_PROFILE_ZONE("rw/geometry-identity");
            u64 identity = 0xcbf29ce484222325ull;
            const auto feed = [&identity](const void* data, std::size_t bytes) {
                const u8* p = static_cast<const u8*>(data);
                for (std::size_t i = 0; i < bytes; ++i) identity = (identity ^ p[i]) * 0x100000001b3ull;
            };
            const u32 parameters[5] = {content.geometry_flags, content.world_sector,
                                       content.mesh_count, content.dynamic, content.matrix_palette};
            feed(parameters, sizeof(parameters));
            feed(content.vertex_format.data(), content.vertex_format.size());
            feed(out + 4u, size - 4u);
            if (identity == 0u) identity = 1u;  // 0 means "no identity" on both sides
            content.identity = identity;
            const u32 blob_attributes = load_be32(out + 8u);
            const u32 attr_end = 12u + 8u * blob_attributes;
            for (u32 mesh = 0; mesh < content.mesh_count && attr_end + 8u * mesh + 8u <= size; ++mesh) {
                const u32 at = attr_end + 8u * mesh;
                if (static_cast<u64>(load_be32(out + at)) + load_be32(out + at + 4u) > size) {
                    ++report_.geometry_identity_extent_outside;
                    break;
                }
            }
            if (record.needs_reupload) {
                ++report_.geometry_upload_retry;
            } else if (first_for_entry) {
                ++report_.geometry_upload_new_entry;
            } else {
                ++report_.geometry_upload_regenerate;
            }
            if (record.identity != identity) {
                release_identity(record);
                auto [it, inserted] = geometry_identity_live_.try_emplace(identity, 0u);
                if (inserted) {
                    ++report_.geometry_identity_distinct;
                } else if (it->second != 0u) {
                    ++report_.geometry_identity_repeat_live;
                } else {
                    ++report_.geometry_identity_resurrected;
                }
                ++it->second;
                record.identity = identity;
            }
            report_.geometry_blob_bytes += blob_size;
            report_.geometry_appended_bytes += total - blob_size;
        }
        emit(Op::GeometryContent, content);
        record.uploaded_generation = record.generation;
        record.needs_reupload = false;
        ++report_.geometry_uploads;
        report_.geometry_upload_bytes += total;
    }

    // STEP-0 MEASUREMENT (R4 / COHERE), default off: after the upload decision,
    // before the per-frame skin refresh. No guest code runs between the upload
    // copy above and this call, so an upload baseline is exactly the uploaded
    // bytes. Reads only; changes nothing above or below.
    if (coherence_ != nullptr) {
        coherence_->observe_use(cpu, context, record, entry, header, size, attributes,
                                vertex_count, sector, geometry, upload_now);
    }

    if (context.pipeline == Pipeline::Skin || context.pipeline == Pipeline::SkinMatFx) {
        // The real byte story of the subsystem: this fires PER FRAME per skinned
        // object, not once per generation, and outweighs static geometry by
        // nearly 7x. Paired with rw/convert-expand on the render thread it
        // brackets the whole cost of skinning support across both threads.
        GEKKO_PROFILE_ZONE("rw/geometry-dynamic");
        GEKKO_PROFILE_VALUE(vertex_count);
        // CPU skinning rewrote POS (and NRM) in place for this draw.
        auto dynamic = zeroed<native::GeometryDynamic>();
        dynamic.geometry = context.geometry;
        dynamic.vertex_count = vertex_count;
        if (attributes >= 1u) {
            const u32 array = read32(header + 12u);
            const u32 stride = read8(header + 12u + 5u);
            if (const u8* bytes = guest(array, stride * vertex_count)) {
                dynamic.position_stride = static_cast<u16>(stride);
                dynamic.positions = segment_->add_payload(bytes, stride * vertex_count);
            }
        }
        if (attributes >= 2u && (context.geometry_flags & 0x10u) != 0u) {
            const u32 array = read32(header + 20u);
            const u32 stride = read8(header + 20u + 5u);
            if (const u8* bytes = guest(array, stride * vertex_count)) {
                dynamic.normal_stride = static_cast<u16>(stride);
                dynamic.normals = segment_->add_payload(bytes, stride * vertex_count);
            }
        }
        emit(Op::GeometryDynamic, dynamic);
        ++report_.geometry_dynamic_updates;
        report_.geometry_dynamic_bytes += dynamic.positions.size + dynamic.normals.size;
    }
    return true;
}

void RwCapture::begin_object(Cpu& cpu, Pipeline pipeline) {
    // Parent of prepare_geometry and everything under it, so the upload,
    // hash and dynamic zones nest inside it and their share of an object is
    // readable directly.
    GEKKO_PROFILE_ZONE("rw/object-begin");
    ObjectContext context;
    context.pipeline = pipeline;
    context.object = cpu.gpr[3];
    const u32 instance = cpu.gpr[4];
    context.entry = read32(instance);
    // RpMeshHeader.numMeshes (u16 at +4) bounds the mesh display-list records.
    context.mesh_count = std::min<u32>(read16(read32(instance + 0x04u) + 4u), 1024u);
    GEKKO_PROFILE_VALUE(context.mesh_count);
    context.geometry_flags = read32(instance + 0x08u);
    context.has_ambient = read32(instance + 0x1cu);
    context.light_mask = read32(instance + 0x20u);
    context.valid = pipeline == Pipeline::PTank ? true : prepare_geometry(cpu, context);
    object_ = context;
}

void RwCapture::rw_render_default(Cpu& cpu) { begin_object(cpu, Pipeline::Default); }
void RwCapture::rw_render_matfx(Cpu& cpu) { begin_object(cpu, Pipeline::MatFx); }
void RwCapture::rw_render_skin(Cpu& cpu) { begin_object(cpu, Pipeline::Skin); }
void RwCapture::rw_render_skin_matfx(Cpu& cpu) { begin_object(cpu, Pipeline::SkinMatFx); }
void RwCapture::rw_render_ptank(Cpu& cpu) {
    object_ = ObjectContext{};
    object_.valid = false;
    object_.pipeline = Pipeline::PTank;
    object_.object = cpu.gpr[3];
    // r4 is the caller-built per-call header (filled at 0x80464C7C..8C); +8 is
    // the flags word the guest tests for its textured branch. Read HERE, at
    // entry: the register it lives in (r27) is reused before GXBegin.
    object_.geometry_flags = read32(cpu.gpr[4] + 8u);
}

RwCapture::TextureRecord& RwCapture::texture_record(u32 parent) {
    TextureRecord& record = textures_[parent];
    if (record.generation == 0u) record.generation = next_generation();
    return record;
}

void RwCapture::bump_texture(u32 raster, bool release) {
    auto it = textures_.find(raster);
    if (it == textures_.end()) return;
    // Below the early return deliberately. All four raster hooks - create,
    // destroy, unlock, unlock-palette - land here, and a create names a raster
    // that is not tracked yet and does nothing. Past the return the zone means
    // "a tracked raster's generation actually moved", which is the event: the
    // Sofdec per-picture rewrite, and the 64 BindTexture slots invalidated
    // below with it.
    GEKKO_PROFILE_ZONE("rw/raster-lifetime");
    GEKKO_PROFILE_VALUE(textures_.size());
    // Destroy is the only time the GPU object must die. Sofdec unlocks rewrite
    // the same raster every picture; emitting ReleaseTexture here forced a
    // create/destroy of a 512x480 resource per movie frame and hitching A/V.
    if (release && it->second.uploaded_generation != 0u) {
        auto key = zeroed<native::ResourceKey>();
        key.address = raster;
        key.generation = it->second.generation;
        emit(Op::ReleaseTexture, key);
        ++report_.texture_releases;
    }
    // A binding that names the old generation must not be deduplicated away.
    for (ShadowSlot& slot : shadow_[static_cast<std::size_t>(Op::BindTexture)]) slot.valid = false;
    if (release) {
        textures_.erase(it);
    } else {
        it->second.generation = next_generation();
        it->second.uploaded_generation = 0u;
        // New content, fresh retry budget - the spent one belonged to texels
        // that no longer exist.
        it->second.needs_reupload = false;
        it->second.retries = 0;
        it->second.last_reject = native::RejectReason::Unknown;
    }
}

u64 RwCapture::hash_texture(u32 parent) const noexcept {
    // Cold by default (validate_textures_ is off). With validation on it rises
    // to the bind rate, which is acceptable precisely because what it measures
    // is a full texel-block hash: this is the only instrument that will ever
    // price that diagnostic.
    GEKKO_PROFILE_ZONE("rw/texture-hash");
    const u32 extension = parent + read32(kRasterExtensionOffset);
    const u32 format = read32(extension + 0x0cu);
    const u32 tlut_format = read32(extension + 0x10u);
    const u32 width = read32(parent + 0x0cu), height = read32(parent + 0x10u);
    const u32 depth = read32(parent + 0x14u);
    const u32 bytes = gx_texture_bytes(format, width, height);
    const u8* texels = guest(read32(extension + 0x1cu), bytes);
    if (texels == nullptr) return 0u;
    // The shared hash definition (NativeAssetManager::texture_hash) folds the
    // header and palette in, so the value is comparable against natively
    // decoded TXD rasters. Same palettised rule as rw_texture_bind.
    const bool palettised = (read8(parent + 0x23u) & 0x60u) != 0u && depth <= 14u;
    const u8* palette = nullptr;
    std::size_t palette_bytes = 0u;
    if (palettised) {
        const u32 entries = (1u << depth) * 2u;
        palette = guest(read32(extension + 0x20u), entries);
        if (palette != nullptr) palette_bytes = entries;
    }
    return NativeAssetManager::texture_hash(format, tlut_format, depth, width, height, texels,
                                            bytes, palette, palette_bytes);
}

bool RwCapture::upload_texture(u32 parent, TextureRecord& record, u32 flags) {
    // Rare, and each event is potentially megabytes. The zone covers both arms
    // - the asset-manager content-hash serve and the miss that copies texels
    // and palette out of guest memory - so it answers whether a miss costs
    // more, which two counters cannot.
    GEKKO_PROFILE_ZONE("rw/texture-upload");
    const u32 extension = parent + read32(kRasterExtensionOffset);
    const u32 format = read32(extension + 0x0cu);
    const u32 tlut_format = read32(extension + 0x10u);
    const u32 pixels = read32(extension + 0x1cu);
    const u32 palette = read32(extension + 0x20u);
    const u32 width = read32(parent + 0x0cu);
    const u32 height = read32(parent + 0x10u);
    const u32 depth = read32(parent + 0x14u);
    const u32 bytes = gx_texture_bytes(format, width, height);
    const u8* texels = guest(pixels, bytes);
    if (bytes == 0u || texels == nullptr) {
        if (report_.texture_upload_failures++ == 0u) {
            report_.first_failed_texture_format = format;
            report_.first_failed_texture_width = width;
            report_.first_failed_texture_height = height;
        }
        return false;
    }

    // Native serve path: the asset manager may already hold this exact
    // texture, decoded from the disc's own ONE/TXD bytes on its worker thread.
    // The shared content hash proves the decode inputs (header, texels,
    // palette) are identical, and both sides use gx_decode_texture, so the
    // served RGBA8 is bit-identical to what the guest-captured path would
    // produce. A miss falls through to that path unchanged.
    if (NativeAssetManager* manager = NativeAssetManager::active()) {
        const bool palettised = (flags & native::kTexturePalettised) != 0u && depth <= 14u;
        const u8* palette_bytes_ptr = nullptr;
        std::size_t palette_size = 0u;
        if (palettised && palette != 0u) {
            palette_bytes_ptr = guest(palette, (1u << depth) * 2u);
            if (palette_bytes_ptr != nullptr) palette_size = (1u << depth) * 2u;
        }
        const u64 content_hash = NativeAssetManager::texture_hash(
            format, tlut_format, depth, width, height, texels, bytes, palette_bytes_ptr,
            palette_size);
        if (const auto asset = manager->find_texture(content_hash)) {
            auto content = zeroed<native::TextureContent>();
            content.texture.address = parent;
            content.texture.generation = record.generation;
            // The asset's own extent: a pinned texture (the sharp button
            // sheets) may be larger than the guest raster; sampling is
            // normalised, so the draw maps it onto the same quads.
            content.width = static_cast<u16>(asset->width);
            content.height = static_cast<u16>(asset->height);
            content.gx_format = kGxTexFmtRGBA8;
            content.flags = flags | native::kTextureNativeLinear;
            content.texels = segment_->add_payload(asset->rgba8.data(), asset->rgba8.size());
            emit(Op::TextureContent, content);
            record.uploaded_generation = record.generation;
            record.needs_reupload = false;
            if (validate_textures_) record.content_hash = content_hash;
            ++report_.texture_uploads;
            ++report_.textures_served_native;
            report_.texture_upload_bytes += asset->rgba8.size();
            GEKKO_PROFILE_VALUE(asset->rgba8.size());
            return true;
        }
        ++report_.textures_native_miss;
    }

    auto content = zeroed<native::TextureContent>();
    content.texture.address = parent;
    content.texture.generation = record.generation;
    content.width = static_cast<u16>(width);
    content.height = static_cast<u16>(height);
    content.gx_format = format;
    content.flags = flags;
    content.texels = segment_->add_payload(texels, bytes);
    if ((flags & native::kTexturePalettised) != 0u && depth <= 14u) {
        const u32 entries = 1u << depth;
        content.tlut_format = tlut_format;
        content.tlut_entries = entries;
        if (const u8* table = guest(palette, entries * 2u)) {
            content.palette = segment_->add_payload(table, entries * 2u);
        }
    }
    emit(Op::TextureContent, content);
    record.uploaded_generation = record.generation;
    record.needs_reupload = false;
    if (validate_textures_) record.content_hash = hash_texture(parent);
    ++report_.texture_uploads;
    report_.texture_upload_bytes += bytes + content.palette.size;
    GEKKO_PROFILE_VALUE(bytes + content.palette.size);
    return true;
}

void RwCapture::rw_texture_bind(Cpu& cpu) {
    // The densest site in the set - above the 10k/s aim, well under the 100k
    // ceiling - and it is here because the work per call is real: ~20 bounds-
    // checked guest reads, a map lookup, and the upload/retry/validate branch.
    // Well over a million binds produce a couple of hundred uploads, so this is
    // what says whether pure re-binding costs anything. First site to drop if a
    // trace is ever too dense: the nested upload and hash zones carry more.
    GEKKO_PROFILE_ZONE("rw/texture-bind");
    ++report_.texture_binds;
    u32 texture = cpu.gpr[3];
    const u32 map = cpu.gpr[4];
    u32 raster = 0;
    if (texture == 0u) {
        // TextureBind(NULL) rewrites the Im texture's raster to the white 4x4
        // and binds that (RENDERER_RE/01 section 4.6).
        texture = read32(kImTexture);
        raster = read32(kWhiteRaster);
    } else {
        raster = read32(texture);
    }
    auto bind = zeroed<native::BindTexture>();
    bind.map = map;
    if (raster == 0u || texture == 0u) {
        ++report_.texture_null_raster;
        emit_state(Op::BindTexture, map, bind);
        return;
    }
    u32 parent = read32(raster);
    if (parent == 0u) parent = raster;
    const u8 type = read8(raster + 0x20u) & 7u;
    const u8 format_flags = read8(raster + 0x23u);
    const bool palettised = (format_flags & 0x60u) != 0u;
    const u32 addressing = read32(texture + 0x50u) & 0xffffu;
    u32 filter = addressing & 0xffu;
    // 0x8056FCC8: 0 -> CLAMP, 1 -> REPEAT, 2 -> MIRROR, 3 -> CLAMP, 4 -> CLAMP.
    const auto wrap = [](u32 mode) -> u8 { return mode == 1u ? 1u : (mode == 2u ? 2u : 0u); };
    bind.wrap_s = wrap((addressing >> 8) & 0xfu);
    bind.wrap_t = wrap((addressing >> 12) & 0xfu);
    if (palettised && (filter == 5u || filter == 6u)) filter = 4u;
    // 0x8056FC90 (min, mag): 0,1 NEAR; 2 LINEAR; 3 NEAR_MIP_NEAR/NEAR;
    // 4 LIN_MIP_NEAR/LINEAR; 5 NEAR_MIP_LIN/NEAR; 6 LIN_MIP_LIN/LINEAR.
    static constexpr u8 kMin[7] = {0, 0, 1, 2, 3, 4, 5};
    static constexpr u8 kMag[7] = {0, 0, 1, 0, 1, 0, 1};
    if (filter > 6u) {
        ++report_.texture_unknown_filter;
        filter = 0u;
    }
    bind.min_filter = kMin[filter];
    bind.mag_filter = kMag[filter];
    bind.width = static_cast<u16>(read32(raster + 0x0cu));
    bind.height = static_cast<u16>(read32(raster + 0x10u));
    bind.offset_x = static_cast<s16>(read16(raster + 0x1cu));
    bind.offset_y = static_cast<s16>(read16(raster + 0x1eu));
    u32 flags = 0;
    if (type == 5u) flags |= native::kTextureCameraTarget;
    if (palettised) flags |= native::kTexturePalettised;
    const u32 extension = parent + read32(kRasterExtensionOffset);
    if ((read32(extension + 0x14u) & 1u) != 0u) flags |= native::kTextureHasAlpha;
    bind.flags = flags;

    TextureRecord& record = texture_record(parent);
    if ((flags & native::kTextureCameraTarget) == 0u) {
        // Same recovery as geometry. A texture the renderer refused used to be
        // pinned to magenta for the rest of its generation.
        if (record.uploaded_generation != record.generation || record.needs_reupload) {
            if (!upload_texture(parent, record, flags)) {
                // AN UNBOUNDED RETRY LOOP, and it predates the rejection
                // channel: upload_texture returns false for an unreadable or
                // unsized raster WITHOUT latching, so the condition above
                // stays true and the upload is attempted again at EVERY bind,
                // for the rest of the generation. On a 150,000-bind route that
                // is 150,000 guest reads of an address that was unreadable the
                // first time.
                //
                // The same budget that bounds a renderer refusal bounds this.
                // When it is spent the texture stays missing and says so,
                // rather than being retried forever in silence.
                constexpr u8 kMaxRetries = 3;
                if (record.retries >= kMaxRetries) {
                    record.needs_reupload = false;
                    // Latch it so the condition above stops firing. The
                    // renderer never received content for this generation, so
                    // nothing is claimed to exist - the bind below still names
                    // the generation and the renderer will report it missing.
                    record.uploaded_generation = record.generation;
                    ++report_.retries_exhausted;
                } else {
                    ++record.retries;
                }
            }
        } else if (validate_textures_ && hash_texture(parent) != record.content_hash) {
            ++report_.texture_content_changed_without_event;
            record.generation = next_generation();
            // Third generation-change site. The other two reset the retry
            // budget and this one did not, so a texture that had already spent
            // its budget stayed unrecoverable across a content change that
            // should have given it a fresh attempt.
            record.needs_reupload = false;
            record.retries = 0;
            record.last_reject = native::RejectReason::Unknown;
            upload_texture(parent, record, flags);
        }
    }
    bind.texture.address = parent;
    bind.texture.generation = record.generation;
    emit_state(Op::BindTexture, map, bind);
}

void RwCapture::rw_camera_begin(Cpu& cpu) {
    const u32 camera = cpu.gpr[4];
    const u32 raster = read32(camera + 0x60u);
    auto c = zeroed<native::Camera>();
    c.camera = camera;
    c.frame_buffer = raster;
    if (raster != 0u) {
        c.offset_x = static_cast<s16>(read16(raster + 0x1cu));
        c.offset_y = static_cast<s16>(read16(raster + 0x1eu));
        c.width = static_cast<u16>(read32(raster + 0x0cu));
        c.height = static_cast<u16>(read32(raster + 0x10u));
        c.raster_type = read8(raster + 0x20u) & 7u;
    }
    emit(Op::CameraBegin, c);
    ++report_.cameras;
}

void RwCapture::rw_camera_end(Cpu& cpu) {
    auto c = zeroed<native::Camera>();
    c.camera = cpu.gpr[4];
    emit(Op::CameraEnd, c);
}

void RwCapture::rw_clear_quad(Cpu& cpu) {
    const u32 mode = cpu.gpr[6] & 3u;
    if (mode == 0u) return;  // the driver returns without drawing
    auto clear = zeroed<native::Clear>();
    const u32 rect = cpu.gpr[4];
    clear.x = static_cast<s32>(read32(rect));
    clear.y = static_cast<s32>(read32(rect + 4u));
    clear.width = static_cast<s32>(read32(rect + 8u));
    clear.height = static_cast<s32>(read32(rect + 12u));
    if (const u8* color = guest(cpu.gpr[5], 4u)) std::memcpy(clear.rgba.data(), color, 4u);
    clear.mode = mode;
    emit(Op::Clear, clear);
    ++report_.clears;
}

void RwCapture::rw_camera_texture_copy(Cpu& cpu) {
    // ~9 per frame and stable across routes that differ 3.6x in length. A real
    // resource event - texture_record() creates or refreshes a camera target -
    // and its position in the frame is where the effect chain takes its
    // snapshots, which is the structure the bloom/glare work reads.
    GEKKO_PROFILE_ZONE("rw/camera-copy");
    const u32 target = cpu.gpr[3];
    u32 parent = read32(target);
    if (parent == 0u) parent = target;
    TextureRecord& record = texture_record(parent);
    auto copy = zeroed<native::CopyTexture>();
    copy.target.address = parent;
    copy.target.generation = record.generation;
    copy.target_width = static_cast<u16>(read32(parent + 0x0cu));
    copy.target_height = static_cast<u16>(read32(parent + 0x10u));
    copy.destination_x = static_cast<s16>(read16(target + 0x1cu));
    copy.destination_y = static_cast<s16>(read16(target + 0x1eu));
    copy.half_resolution = cpu.gpr[4] != 0u ? 1u : 0u;
    const s32 scale = copy.half_resolution != 0u ? 2 : 1;
    copy.source_x = copy.destination_x * scale;
    copy.source_y = copy.destination_y * scale;
    copy.source_width = static_cast<s32>(read32(target + 0x0cu)) * scale;
    copy.source_height = static_cast<s32>(read32(target + 0x10u)) * scale;
    emit(Op::CopyTexture, copy);
    ++report_.copies;
}

void RwCapture::rw_show_raster(Cpu& cpu) {
    // close_segment is called from exactly one place - the end of this function
    // - so this zone genuinely brackets the drain, the Present record and the
    // handoff to the renderer, and it is the anchor every per-frame plot below
    // is sampled against.
    GEKKO_PROFILE_ZONE("rw/present");
    // SECOND DRAIN SITE, and the one that makes the contract frame-paced
    // rather than geometry-paced. prepare_geometry is the natural place to act
    // on a rejection, but a frame that prepares no geometry - a menu, a movie,
    // a pure-2D screen - never reaches it, and a TEXTURE rejection would then
    // wait on unrelated geometry work. Present always runs.
    drain_rejections();
    auto present = zeroed<native::Present>();
    present.xfb = read32(kNextXfb);
    present.frame = ++present_ordinal_;
    const u32 mode = read32(kRenderMode);
    if (mode != 0u) {
        present.xfb_width = read16(mode + 4u);
        present.efb_height = read16(mode + 6u);
        present.xfb_height = read16(mode + 8u);
        if (const u8* filter = guest(mode + 0x32u, 7u)) std::memcpy(present.vfilter, filter, 7u);
        present.vfilter_enabled = 1u;
    }
    present.game_ticks = cpu.game_clock().now();
    emit(Op::Present, present);
    ++report_.presents;
    if (coherence_ != nullptr) coherence_->on_present();  // STEP-0 measurement, default off

    // ---- the per-frame resource picture ----
    //
    // Sampled here because present is the one event that always happens, so
    // every line below is on the same time base as every other.
    GEKKO_PROFILE_PLOT("rw/geometries-live", static_cast<double>(geometry_.size()));
    // textures_ also holds camera targets, because rw_camera_texture_copy calls
    // texture_record(): this is live captured RASTERS, not uploaded textures.
    // Unlike geometry, bump_texture erases on release, so an eviction is
    // visible here as a downward step.
    GEKKO_PROFILE_PLOT("rw/textures-live", static_cast<double>(textures_.size()));
    // Cumulative, and it is NOT redundant with the live count: prepare_geometry
    // emits ReleaseGeometry on a serial or size change without erasing the map
    // entry, so thousands of evictions happen while geometry_.size() sits flat.
    // Every step in this line is one geometry thrown away, timestamped.
    GEKKO_PROFILE_PLOT("rw/geometry-releases", static_cast<double>(report_.geometry_releases));
    // A quantity, not a duration - which is why the camera pass gets a plot
    // here and not a zone at its own hook. The slope is the per-frame rate and
    // a change of slope is the finding.
    GEKKO_PROFILE_PLOT("rw/cameras-per-frame", static_cast<double>(report_.cameras));
    // Flat-zero tripwire rather than a metric: every one of these counts a
    // GXBegin or GXCallDisplayList from a return address this capture does not
    // know, i.e. draws attributed to nothing. The frame a new stage or MatFx
    // variant produces one, the line steps and the timeline says when.
    GEKKO_PROFILE_PLOT("rw/gx-unattributed",
                       static_cast<double>(report_.untracked_primitives +
                                           report_.unknown_begin_sites +
                                           report_.unknown_display_list_sites +
                                           report_.display_list_without_object +
                                           report_.display_list_mesh_not_found));
    // Each step is an object now PERMANENTLY missing: its retry budget is spent,
    // or its rejection was dropped by the mailbox. Pair with rw/rejection-apply
    // - the cluster of apply zones is the storm, a step here is what it cost.
    GEKKO_PROFILE_PLOT("rw/resources-lost", static_cast<double>(report_.retries_exhausted +
                                                                report_.rejections_lost_to_overflow));
    if (GEKKO_PROFILE_CONNECTED()) {
        // IN-FLIGHT refusal state, which no counter carries: resources waiting
        // on a re-upload or burning retry budget right now. retries_exhausted
        // only fires once an object is already lost. This walk exists solely to
        // produce profiling data, so it stays behind the connected guard - and
        // in a non-Tracy build the guard is literally `false`, so it folds away
        // while still referencing everything it names.
        std::size_t pending = 0;
        for (const auto& entry : geometry_) {
            if (entry.second.needs_reupload || entry.second.retries != 0u) ++pending;
        }
        for (const auto& entry : textures_) {
            if (entry.second.needs_reupload || entry.second.retries != 0u) ++pending;
        }
        GEKKO_PROFILE_PLOT("rw/retry-pending", static_cast<double>(pending));

        // The state-churn half of the subsystem, which can never carry a zone:
        // emit_state runs ~172k times a second and roughly two thirds of those
        // set a value the renderer already has. A DIP in this line means the
        // shadow was invalidated - bump_texture clearing the 64 BindTexture
        // slots, or a channel-alias invalidation - and that is a real cost with
        // a findable cause. The previous-frame totals live in function statics
        // rather than members because this file owns no header; the capture is
        // a single instance per process, and a second one would only make this
        // one plot meaningless, never change behaviour.
        static u64 previous_state_calls = 0;
        static u64 previous_state_deduplicated = 0;
        const u64 calls = report_.state_calls - previous_state_calls;
        const u64 deduplicated = report_.state_deduplicated - previous_state_deduplicated;
        previous_state_calls = report_.state_calls;
        previous_state_deduplicated = report_.state_deduplicated;
        if (calls != 0u) {
            GEKKO_PROFILE_PLOT("rw/state-dedup",
                               100.0 * static_cast<double>(deduplicated) /
                                   static_cast<double>(calls));
        }
    }

    close_segment(true);
    // AFTER close_segment, not at the top of the function: a frame mark at the
    // top would cut every rw/present zone in half and bill this present's own
    // work to the previous frame. Here each guest frame's zones stay inside it
    // and there is still exactly one mark per guest present.
    GEKKO_PROFILE_FRAME("rw/guest-present");
}

void RwCapture::rw_raster_create(Cpu& cpu) { bump_texture(cpu.gpr[4], false); }
void RwCapture::rw_raster_destroy(Cpu& cpu) { bump_texture(cpu.gpr[4], true); }
void RwCapture::rw_raster_unlock(Cpu& cpu) { bump_texture(cpu.gpr[4], false); }
void RwCapture::rw_raster_unlock_palette(Cpu& cpu) { bump_texture(cpu.gpr[4], false); }

void RwCapture::release_identity(GeometryRecord& record) {
    if (record.identity == 0u) return;
    auto it = geometry_identity_live_.find(record.identity);
    if (it != geometry_identity_live_.end() && it->second != 0u) --it->second;
    record.identity = 0u;
}

void RwCapture::rw_geometry_notify(Cpu& cpu) {
    const u32 entry = cpu.gpr[3];
    auto it = geometry_.find(entry);
    if (it == geometry_.end()) return;
    if (it->second.uploaded_generation != 0u) {
        auto key = zeroed<native::ResourceKey>();
        key.address = entry;
        key.generation = it->second.generation;
        emit(Op::ReleaseGeometry, key);
        ++report_.geometry_releases;
        ++report_.geometry_release_notify;
        // Who freed it. At the notify's first instruction r1 is its caller's
        // frame; that caller's own return address is in the LR save word of
        // the frame above it (EABI: back chain at +0, LR save at +4). Reads
        // only, and only of the stack the guest just built.
        const u32 frame1 = read32(cpu.gpr[1]);
        const u32 ret1 = frame1 != 0u ? read32(frame1 + 4u) : 0u;
        const u32 frame2 = frame1 != 0u ? read32(frame1) : 0u;
        const u32 ret2 = frame2 != 0u ? read32(frame2 + 4u) : 0u;
        bool found = false;
        for (u32 i = 0; i < report_.release_sites_used; ++i) {
            auto& site = report_.release_sites[i];
            if (site.ret0 == cpu.lr && site.ret1 == ret1 && site.ret2 == ret2) {
                ++site.count;
                found = true;
                break;
            }
        }
        if (!found) {
            if (report_.release_sites_used < Report::kReleaseSites) {
                auto& site = report_.release_sites[report_.release_sites_used++];
                site.ret0 = cpu.lr;
                site.ret1 = ret1;
                site.ret2 = ret2;
                site.count = 1;
            } else {
                ++report_.release_sites_overflow;
            }
        }
    }
    release_identity(it->second);
    geometry_.erase(it);
    if (coherence_ != nullptr) coherence_->on_release(entry);  // STEP-0 measurement, default off
    if (object_.entry == entry) object_.valid = false;
}

// ---- report -----------------------------------------------------------------

RwCapture::Report RwCapture::report() const { return report_; }

std::string RwCapture::report_text() const {
    const Report r = report_;
    std::ostringstream out;
    out << "[native-capture] segments " << r.segments << " presents " << r.presents << " commands "
        << r.commands << " payload " << r.payload_bytes << " B\n";
    out << "[native-capture] state calls " << r.state_calls << " deduplicated "
        << r.state_deduplicated << " immediate draws " << r.immediate_draws << " mesh draws "
        << r.mesh_draws << " clears " << r.clears << " copies " << r.copies << " cameras "
        << r.cameras << "\n";
    out << "[native-capture] coverage: untracked primitives " << r.untracked_primitives
        << " (first opcode 0x" << std::hex << r.first_untracked_opcode << std::dec
        << ") unknown GXBegin sites " << r.unknown_begin_sites << " (first lr 0x" << std::hex
        << r.first_unknown_begin_lr << std::dec << ") unknown DL sites "
        << r.unknown_display_list_sites << " (first lr 0x" << std::hex
        << r.first_unknown_display_list_lr << std::dec << ") DL without object "
        << r.display_list_without_object << " DL mesh not found " << r.display_list_mesh_not_found
        << " immediate indexed " << r.immediate_indexed_attributes << " count mismatch "
        << r.immediate_count_mismatch << " unsupported programs " << r.unsupported_programs
        << " clear quads absorbed " << r.clear_quads_absorbed << " GX copy triggers "
        << r.gx_copy_triggers << " im3d program vs raster proxy disagreements "
        << r.im3d_program_proxy_disagreements << " untextured PTank draws "
        << r.ptank_untextured << "\n";
    if (gx_input_digest_enabled_) {
        char digest[32];
        std::snprintf(digest, sizeof(digest), "0x%016llX", static_cast<unsigned long long>(r.gx_input_digest));
        out << "[gx-inputs] immediate primitives " << r.gx_input_primitives << " payload "
            << r.gx_input_payload_bytes << " B digest " << digest << " (layout from __GXData) copy triggers "
            << r.gx_copy_triggers << "\n";
    }
    out << "[native-capture] textures: binds " << r.texture_binds << " uploads "
        << r.texture_uploads << " (" << r.texture_upload_bytes << " B) served natively "
        << r.textures_served_native << " native misses " << r.textures_native_miss
        << " releases " << r.texture_releases << " null raster " << r.texture_null_raster
        << " upload failures " << r.texture_upload_failures << " (first format "
        << r.first_failed_texture_format << " " << r.first_failed_texture_width << "x"
        << r.first_failed_texture_height << ") unknown filter " << r.texture_unknown_filter
        << " changed without event " << r.texture_content_changed_without_event << "\n";
    out << "[native-capture] geometry: uploads " << r.geometry_uploads << " ("
        << r.geometry_upload_bytes << " B) dynamic " << r.geometry_dynamic_updates << " ("
        << r.geometry_dynamic_bytes << " B) releases " << r.geometry_releases << " invalid "
        << r.geometry_invalid_entries << " guest reads out of range "
        << r.guest_reads_out_of_range << " state loads " << r.state_loads << " (replayed "
        << r.state_loads_replayed << ", " << r.state_commands_replayed << " state commands)\n";
    // THE RECOVERY SIDE OF THE CONTRACT. `retries_exhausted` is the number that
    // matters: those objects are still missing, and this is the only place that
    // says so. Printed unconditionally, so "nothing was refused" and "refusals
    // went nowhere" cannot produce the same output.
    if (r.rejections_received == 0u) {
        out << "[native-capture] rejections: none received from the renderer";
        if (r.rejections_lost_to_overflow != 0u) {
            out << " -- but " << r.rejections_lost_to_overflow
                << " were LOST TO MAILBOX OVERFLOW (those objects cannot recover)";
        }
        out << "\n";
    } else {
        out << "[native-capture] rejections " << r.rejections_received << " -> re-uploads "
            << r.reuploads_requested << ", RETRIES EXHAUSTED " << r.retries_exhausted
            << " (still missing), unmatched " << r.rejections_unmatched << ", LOST TO OVERFLOW "
            << r.rejections_lost_to_overflow << ", coalesced " << r.rejections_coalesced
            << "; by reason:";
        for (std::size_t i = 0; i < r.reject_reasons.size(); ++i) {
            if (r.reject_reasons[i] == 0u) continue;
            out << " " << native::reject_reason_name(static_cast<native::RejectReason>(i)) << " "
                << r.reject_reasons[i];
        }
        out << "\n";
    }
    if (r.geometry_uploads != 0u) {
        out << "[native-capture] geometry churn: uploads new-entry " << r.geometry_upload_new_entry
            << " regenerate " << r.geometry_upload_regenerate << " retry "
            << r.geometry_upload_retry << "; releases notify " << r.geometry_release_notify
            << " regenerate " << r.geometry_release_regenerate << "\n";
        out << "[native-capture] geometry identity (token/serial/arena tail excluded): distinct "
            << r.geometry_identity_distinct << " identical-to-live "
            << r.geometry_identity_repeat_live << " resurrected-after-release "
            << r.geometry_identity_resurrected << " display lists past size "
            << r.geometry_identity_extent_outside << "; bytes: instance " << r.geometry_blob_bytes
            << " + per-frame arrays appended " << r.geometry_appended_bytes << "\n";
        if (r.release_sites_used != 0u) {
            out << "[native-capture] geometry release sites (notify lr / caller / caller's caller):";
            char text[64];
            for (u32 i = 0; i < r.release_sites_used; ++i) {
                const auto& site = r.release_sites[i];
                std::snprintf(text, sizeof(text), " %08X/%08X/%08X=", site.ret0, site.ret1,
                              site.ret2);
                out << text << site.count;
            }
            if (r.release_sites_overflow != 0u) out << " (+" << r.release_sites_overflow << " unlisted)";
            out << "\n";
        }
    }
    out << "[native-capture] host cost: hook calls " << r.hook_calls << " hook TSC ticks "
        << r.hook_ns << " tap TSC ticks " << r.tap_ns << " queue wait "
        << static_cast<double>(r.queue_wait_ns) / 1e6 << " ms\n";
    if (cpu_owner_profile_) {
        out << "[native-capture] CPU hook owners (raw TSC ticks, not nanoseconds); overflow="
            << r.hook_owner_overflow << '\n';
        for (u32 owner = 0; owner < r.hook_owner_count; ++owner) {
            const auto& cost = r.hook_owner_costs[owner];
            out << "[native-capture-cpu] " << cost.name << " address=" << cost.address
                << " calls=" << cost.calls << " ticks=" << cost.ticks << '\n';
        }
    }
    out << "[native-capture] ops:";
    for (std::size_t index = 1; index < r.op_counts.size(); ++index) {
        if (r.op_counts[index] != 0u) {
            out << " " << native::op_name(static_cast<Op>(index)) << "=" << r.op_counts[index];
        }
    }
    out << "\n";
    return out.str();
}

// ===========================================================================
// STEP-0 MEASUREMENT: CoherenceProbe (R4 / COHERE / HASH-TOKEN). DEFAULT OFF.
// Declared above RwCapture::RwCapture; see the block comment there. Nothing in
// this section runs unless RwCapture::enable_coherence_probe() was called.
// ===========================================================================

namespace {

// atomic+0x48 is RpAtomic.renderCallBack; fn_804452D4 (its only writer outside
// the RW core) installs the DMorph software render callback there.
constexpr u32 kDMorphSoftwareRender = 0x80444AC4u;
constexpr u32 kDCFlushRange = 0x80372534u;  // dcbf loop + sc
constexpr u32 kDCStoreRange = 0x80372564u;  // dcbst loop + sc
// RwRasterUnlock / RwRasterUnlockPalette extents (entry to past the blr, from
// tools/doldis.py). A flush whose LR lies inside is a texture upload.
constexpr u32 kRasterUnlockEnd = 0x804972B8u;
constexpr u32 kRasterUnlockPaletteEnd = 0x80497458u;
// fn_80464624's four DCFlushRange call sites (bl at 0x804646D0, 0x80464714,
// 0x804647D8, 0x80464858): the reinstance re-emit flushes, pre-seeded so a
// zero count is printed rather than absent.
constexpr u32 kReinstanceFlushLr[] = {0x804646D4u, 0x80464718u, 0x804647DCu, 0x8046485Cu};
// RwRasterUnlock / ...Palette flush return addresses (bl at 0x80497198 / 0x804973FC).
constexpr u32 kRasterFlushLr[] = {0x8049719Cu, 0x80497400u};

constexpr u64 kCoherenceSeed = 0x9E3779B97F4A7C15ull;

// One step is a bijection of h (xor, odd multiply, xorshift), so a change
// confined to one 8-byte word can never collide; wider changes collide with
// probability ~2^-64. Eight bytes per step: several times the byte-wise FNV.
inline u64 coherence_step(u64 h, u64 w) noexcept {
    h = (h ^ w) * 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 32);
}

inline u64 coherence_bytes(u64 h, const u8* p, std::size_t n) noexcept {
    h = coherence_step(h, n);
    for (; n >= 8u; n -= 8u, p += 8u) {
        u64 w;
        std::memcpy(&w, p, 8u);
        h = coherence_step(h, w);
    }
    if (n != 0u) {
        u64 w = 0;
        std::memcpy(&w, p, n);
        h = coherence_step(h, w ^ (static_cast<u64>(n) << 56));
    }
    return h;
}

u64 coherence_count_diff(const u8* a, const u8* b, u64 n) noexcept {
    u64 count = 0;
    for (u64 i = 0; i < n; ++i) count += a[i] != b[i] ? 1u : 0u;
    return count;
}

std::string coherence_hex(u32 value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "\"0x%08X\"", value);
    return buffer;
}

}  // namespace

RwCapture::CoherenceProbe::CoherenceProbe(RwCapture& owner, const CoherenceProbeConfig& config)
    : owner_(owner), config_(config) {
    events_.reserve(static_cast<std::size_t>(
        std::min<u64>(static_cast<u64>(config_.max_events) + config_.max_skin_events, 65536u)));
    summaries_.reserve(std::min<u32>(config_.max_record_summaries, 65536u));
    for (u32 lr : kReinstanceFlushLr) lr_[lr_used_++].lr = lr;
    for (u32 lr : kRasterFlushLr) lr_[lr_used_++].lr = lr;
}

const u8* RwCapture::CoherenceProbe::Source::at(u32 address, u64 bytes) const noexcept {
    if (overlay != nullptr && address >= overlay_base) {
        const u64 offset = static_cast<u64>(address) - overlay_base;
        if (offset + bytes <= overlay_size) return overlay + offset;
    }
    // Same read as RwCapture::guest(), without its counter.
    if (memory == nullptr || bytes > 0xFFFFFFFFull) return nullptr;
    return memory->peek(GcAddr(address), static_cast<std::size_t>(bytes));
}

RwCapture::CoherenceProbe::Source RwCapture::CoherenceProbe::live_source() const noexcept {
    // The game's memory, read host-side: no guest access, no fixture, no observer.
    Source source;
    source.memory = &owner_.system_.guest_memory();
    return source;
}

void RwCapture::CoherenceProbe::install_flush_observers() {
    if (flush_installed_ || !config_.flush_overlap) return;
    flush_installed_ = true;
    // add_hle_observer CHAINS (observer, then the previous handler) and, with no
    // previous handler, declines - the translated body runs unchanged. Not
    // RwCapture::observe: report_.hook_calls/hook_ns stay production numbers.
    owner_.system_.add_hle_observer(
        kDCFlushRange, [this](Cpu& cpu) { on_flush(cpu, false); },
        "r4 coherence: DCFlushRange observer (STEP-0, default off)");
    owner_.system_.add_hle_observer(
        kDCStoreRange, [this](Cpu& cpu) { on_flush(cpu, true); },
        "r4 coherence: DCStoreRange observer (STEP-0, default off)");
}

void RwCapture::CoherenceProbe::build_view(const Source& src, u32 header, u32 size,
                                           u32 attributes, u32 vertex_count, bool skin,
                                           u32 geometry_flags, u32 mesh_count, View& view,
                                           std::vector<Span>* trace) {
    view = View{};
    const u8* base = src.at(header, size);
    if (base == nullptr || size < 12u || attributes > kSlots) return;
    view.ok = true;
    // Production's classification extent (instance blob + arena tail).
    view.blob_extent = src.at(header, static_cast<u64>(size) + 32u) != nullptr
                           ? size + 32u
                           : size;
    const u64 blob_end = static_cast<u64>(header) + view.blob_extent;

    // [0,4) token + serial excluded. flags and numAttributes are content.
    u64 h = coherence_step(kCoherenceSeed, load_be32(base + 4u));
    h = coherence_step(h, load_be32(base + 8u));
    view.bytes_hashed = 8u;
    if (trace != nullptr) {
        trace->push_back({4u, 8u, 0u});
        trace->push_back({8u, 12u, 0u});
    }

    // Production validates only attributes <= 16 and size >= 12, so the record
    // table can overhang the blob. Then nothing past +12 has a known layout:
    // hash it raw and do not wrap `size - records_end`.
    const u64 records_end = 12ull + 8ull * attributes;
    if (records_end > size) {
        view.records_overhang = true;
        view.records_end = size;
        view.body_begin = size;
        h = coherence_bytes(h, base + 12u, size - 12u);
        view.bytes_hashed += size - 12u;
        if (trace != nullptr && size > 12u) trace->push_back({12u, size, 1u});
        view.uncovered = h;
        view.combined = h;
        return;
    }
    view.records_end = static_cast<u32>(records_end);
    const u32 max_meshes = (size - view.records_end) / 8u;
    view.meshes = mesh_count;
    if (view.meshes > max_meshes) {
        view.meshes = max_meshes;
        view.mesh_clamped = true;
    }
    view.body_begin = view.records_end + 8u * view.meshes;

    // Attribute records: word0 normalised (in-blob -> header-relative, external
    // -> a per-slot marker, since its CONTENT is hashed separately), word1 raw.
    for (u32 i = 0; i < attributes; ++i) {
        const u8* record = base + 12u + 8u * i;
        const u32 ptr = load_be32(record);
        // u64 on purpose: production's u32 product can wrap on garbage counts.
        const u64 bytes64 = static_cast<u64>(record[5]) * vertex_count;
        u32 bytes = 0;
        if (bytes64 > 0xFFFFFFFFull) {
            ++view.extent_wrap;
        } else {
            bytes = static_cast<u32>(bytes64);
        }
        const u32 bit = 1u << i;
        const bool in_blob = ptr >= header && ptr < blob_end;
        const bool valid = ptr != 0u && bytes != 0u && src.at(ptr, bytes) != nullptr;
        // Exactly production's GeometryDynamic set (the refresh after the upload).
        const bool covered =
            skin && valid && (i == 0u || (i == 1u && (geometry_flags & 0x10u) != 0u));
        view.ptr[i] = ptr;
        view.bytes[i] = bytes;
        if (in_blob) view.inblob_mask |= bit;
        if (covered) {
            view.covered_mask |= bit;
        } else if (!in_blob && valid) {
            view.ext_mask |= bit;
        }
        if (in_blob && !covered &&
            static_cast<u64>(ptr) + bytes > static_cast<u64>(header) + size) {
            ++view.inblob_overhang;
        }
        h = coherence_step(h, in_blob ? ptr - header : (ptr != 0u ? (0xE0000000u | i) : 0u));
        h = coherence_step(h, load_be32(record + 4u));
    }
    // Mesh records: display-list pointer normalised, size raw.
    for (u32 m = 0; m < view.meshes; ++m) {
        const u8* record = base + view.records_end + 8u * m;
        const u32 list = load_be32(record);
        const bool in_blob = list >= header && list < blob_end;
        if (!in_blob && list != 0u) ++view.external_display_lists;
        h = coherence_step(h, in_blob ? list - header : (list != 0u ? (0xD0000000u | m) : 0u));
        h = coherence_step(h, load_be32(record + 4u));
    }
    view.bytes_hashed += 8ull * (static_cast<u64>(attributes) + view.meshes);
    if (trace != nullptr) {
        if (attributes != 0u) trace->push_back({12u, view.records_end, 1u});
        if (view.meshes != 0u) trace->push_back({view.records_end, view.body_begin, 1u});
    }

    // Covered IN-BLOB slot ranges, clipped to the body, sorted and merged.
    std::array<std::pair<u32, u32>, 2> ranges{};
    u32 count = 0;
    for (u32 i = 0; i < attributes && i < 2u; ++i) {
        const u32 bit = 1u << i;
        if ((view.covered_mask & bit) == 0u || (view.inblob_mask & bit) == 0u) continue;
        const u64 relative = view.ptr[i] - header;
        const u64 begin = std::max<u64>(relative, view.body_begin);
        const u64 end = std::min<u64>(relative + view.bytes[i], size);
        if (begin < end) ranges[count++] = {static_cast<u32>(begin), static_cast<u32>(end)};
    }
    if (count == 2u && ranges[1].first < ranges[0].first) std::swap(ranges[0], ranges[1]);
    if (count == 2u && ranges[1].first <= ranges[0].second) {
        ranges[0].second = std::max(ranges[0].second, ranges[1].second);
        count = 1u;
    }
    view.covered = ranges;
    view.covered_ranges = count;

    // Body: [body_begin, size) minus the covered ranges; each gap is fed with its
    // offset so equal bytes moved between gaps still change the hash.
    u64 body = kCoherenceSeed ^ 0xB0D7ull;
    const auto hash_gap = [&](u32 begin, u32 end) {
        if (begin >= end) return;
        body = coherence_step(body, begin);
        body = coherence_bytes(body, base + begin, end - begin);
        view.bytes_hashed += end - begin;
        if (trace != nullptr) trace->push_back({begin, end, 2u});
    };
    u32 cursor = view.body_begin;
    for (u32 r = 0; r < count; ++r) {
        hash_gap(cursor, ranges[r].first);
        cursor = std::max(cursor, ranges[r].second);
    }
    hash_gap(cursor, size);

    u64 uncovered = coherence_step(h, body);
    uncovered = coherence_step(uncovered, static_cast<u64>(view.covered_mask) |
                                              (static_cast<u64>(view.ext_mask) << 16));
    for (u32 i = 0; i < attributes; ++i) {
        if ((view.ext_mask & (1u << i)) == 0u) continue;
        view.slot_hash[i] =
            coherence_bytes(kCoherenceSeed + i, src.at(view.ptr[i], view.bytes[i]), view.bytes[i]);
        view.bytes_hashed += view.bytes[i];
        uncovered = coherence_step(uncovered, view.slot_hash[i]);
    }
    u64 combined = uncovered;
    for (u32 i = 0; i < attributes; ++i) {
        if ((view.covered_mask & (1u << i)) == 0u) continue;
        view.slot_hash[i] = coherence_bytes(kCoherenceSeed + 0x100u + i,
                                            src.at(view.ptr[i], view.bytes[i]), view.bytes[i]);
        view.bytes_hashed += view.bytes[i];
        combined = coherence_step(combined, view.slot_hash[i]);
    }
    view.uncovered = uncovered;
    view.combined = combined;
}

bool RwCapture::CoherenceProbe::coverage_ok(const View& view, u32 size,
                                            std::vector<Span>& trace) const {
    // Review requirement (a): every byte of [4,size) is hashed exactly once or
    // lies in a covered in-blob range - the hashed spans plus the covered ranges
    // must tile [4,size) with no overlap and no hole.
    for (u32 r = 0; r < view.covered_ranges; ++r) {
        trace.push_back({view.covered[r].first, view.covered[r].second, 3u});
    }
    std::sort(trace.begin(), trace.end(),
              [](const Span& left, const Span& right) { return left.begin < right.begin; });
    u32 cursor = 4u;
    for (const Span& span : trace) {
        if (span.begin != cursor || span.end < span.begin) return false;
        cursor = span.end;
    }
    return cursor == size;
}

void RwCapture::CoherenceProbe::self_test(const Source& live, const View& view, u32 header,
                                          u32 size, u32 attributes, u32 vertex_count,
                                          bool skin, u32 geometry_flags, u32 mesh_count) {
    // Review requirement (b): a positive control for the IN-BLOB body hash - the
    // path that must see DMorph - which the Skin validation never exercises (skin
    // output buffers are external). Bytes are flipped in a HOST COPY only.
    if (size > (8u << 20)) {
        ++selftest_skipped_large_;
        return;
    }
    const u8* base = live.at(header, size);
    if (base == nullptr) return;
    ++selftest_runs_;
    selftest_buffer_.assign(base, base + size);
    Source overlay = live;
    overlay.overlay = selftest_buffer_.data();
    overlay.overlay_base = header;
    overlay.overlay_size = size;

    std::vector<Span> spans;
    View reference;
    build_view(overlay, header, size, attributes, vertex_count, skin, geometry_flags, mesh_count,
               reference, &spans);
    if (!reference.ok || reference.combined != view.combined ||
        reference.uncovered != view.uncovered) {
        ++selftest_identity_mismatch_;
    }
    std::vector<std::pair<u64, u64>> gaps;
    for (const Span& span : spans) {
        if (span.kind == 2u) gaps.emplace_back(span.begin, span.end);
    }
    const auto in_gap = [&gaps](u64 begin, u64 end) -> u32 {
        for (const auto& gap : gaps) {
            const u64 low = std::max(begin, gap.first);
            const u64 high = std::min(end, gap.second);
            if (low < high) return static_cast<u32>(low + (high - low) / 2u);
        }
        return ~0u;
    };
    const u64 blob_end = static_cast<u64>(header) + reference.blob_extent;
    std::vector<std::pair<u64, u64>> lists;
    for (u32 m = 0; m < reference.meshes; ++m) {
        const u8* record = selftest_buffer_.data() + reference.records_end + 8u * m;
        const u32 list = load_be32(record);
        if (list >= header && list < blob_end) {
            lists.emplace_back(list - header, static_cast<u64>(list - header) + load_be32(record + 4u));
        }
    }
    std::vector<std::pair<u64, u64>> slots;
    for (u32 i = 0; i < attributes; ++i) {
        const u32 bit = 1u << i;
        if ((reference.inblob_mask & bit) == 0u || (reference.covered_mask & bit) != 0u) continue;
        slots.emplace_back(reference.ptr[i] - header,
                           static_cast<u64>(reference.ptr[i] - header) + reference.bytes[i]);
    }

    const u32 k = static_cast<u32>(selftest_runs_);
    std::array<u32, kSelfClasses> offsets;
    offsets.fill(~0u);
    offsets[kSelfFlags] = 4u + k % 4u;
    offsets[kSelfAttrCount] = 8u + k % 4u;
    // A non-zero EXTERNAL pointer word is normalised to a marker by design (its
    // content is hashed separately, or - for a display list - not uploaded at
    // all), so a flip there is only aimed at the raw second word.
    if (!reference.records_overhang && attributes != 0u) {
        const u32 slot = k % attributes;
        u32 byte = (k * 3u) % 8u;
        const u32 ptr = reference.ptr[slot];
        if (byte < 4u && ptr != 0u && (reference.inblob_mask & (1u << slot)) == 0u) byte += 4u;
        offsets[kSelfAttrRecord] = 12u + 8u * slot + byte;
    }
    if (reference.meshes != 0u) {
        const u32 mesh = k % reference.meshes;
        u32 byte = (k * 5u) % 8u;
        const u32 list = load_be32(selftest_buffer_.data() + reference.records_end + 8u * mesh);
        if (byte < 4u && list != 0u && (list < header || list >= blob_end)) byte += 4u;
        offsets[kSelfMeshRecord] = reference.records_end + 8u * mesh + byte;
    }
    for (const auto& list : lists) {
        const u32 at = in_gap(list.first, list.second);
        if (at != ~0u) {
            offsets[kSelfDisplayList] = at;
            break;
        }
    }
    for (const auto& slot : slots) {
        const u32 at = in_gap(slot.first, slot.second);
        if (at != ~0u) {
            offsets[kSelfInBlobSlot] = at;
            break;
        }
    }
    // A body byte that is in no display list and no in-blob slot.
    const auto inside_any = [&lists, &slots](u64 x) {
        for (const auto& range : lists) {
            if (x >= range.first && x < range.second) return true;
        }
        for (const auto& range : slots) {
            if (x >= range.first && x < range.second) return true;
        }
        return false;
    };
    std::vector<u64> candidates;
    for (const auto& gap : gaps) candidates.push_back(gap.first);
    for (const auto& range : lists) candidates.push_back(range.second);
    for (const auto& range : slots) candidates.push_back(range.second);
    u32 tested = 0;
    for (u64 x : candidates) {
        if (++tested > 256u) break;
        if (in_gap(x, x + 1u) != ~0u && !inside_any(x)) {
            offsets[kSelfBodyGap] = static_cast<u32>(x);
            break;
        }
    }

    for (u32 c = 0; c < kSelfClasses; ++c) {
        SelfClass& result = self_[c];
        if (offsets[c] == ~0u || offsets[c] >= size) {
            ++result.unavailable;
            continue;
        }
        ++result.attempts;
        u8& byte = selftest_buffer_[offsets[c]];
        const u8 saved = byte;
        byte = static_cast<u8>(saved ^ 0xA5u);
        View flipped;
        build_view(overlay, header, size, attributes, vertex_count, skin, geometry_flags,
                   mesh_count, flipped, nullptr);
        byte = saved;
        const bool combined_changed = flipped.combined != reference.combined;
        const bool uncovered_changed = flipped.uncovered != reference.uncovered;
        if (combined_changed && uncovered_changed) {
            ++result.detected;
        } else if (combined_changed) {
            ++result.combined_only;
        } else {
            ++result.missed;
        }
    }
}

void RwCapture::CoherenceProbe::drop_snapshot(Rec& rec) {
    snapshot_bytes_ -= rec.snap.size();
    std::vector<u8>().swap(rec.snap);
    rec.snap_mask = 0;
}

void RwCapture::CoherenceProbe::take_snapshot(Rec& rec, const Source& src, const View& view,
                                              u32 header, u32 size) {
    drop_snapshot(rec);
    const u32 external = (view.covered_mask | view.ext_mask) & ~view.inblob_mask;
    u64 need = size;
    for (u32 i = 0; i < kSlots; ++i) {
        if ((external & (1u << i)) != 0u) need += view.bytes[i];
    }
    const u8* base = src.at(header, size);
    if (base == nullptr || need > 0xFFFFFFFFull ||
        snapshot_bytes_ + need > config_.snapshot_budget_bytes) {
        ++records_without_snapshot_;
        return;
    }
    rec.snap.reserve(static_cast<std::size_t>(need));
    rec.snap.assign(base, base + size);
    for (u32 i = 0; i < kSlots; ++i) {
        const u32 bit = 1u << i;
        if ((external & bit) == 0u) continue;
        const u8* bytes = src.at(view.ptr[i], view.bytes[i]);
        if (bytes == nullptr) continue;
        rec.snap_off[i] = static_cast<u32>(rec.snap.size());
        rec.snap_len[i] = view.bytes[i];
        rec.snap.insert(rec.snap.end(), bytes, bytes + view.bytes[i]);
        rec.snap_mask |= bit;
    }
    snapshot_bytes_ += rec.snap.size();
    snapshot_peak_ = std::max(snapshot_peak_, snapshot_bytes_);
}

void RwCapture::CoherenceProbe::attribute(const Source& src, const View& view, const Rec& rec,
                                          u32 header, u32 size, u32 attributes, u32& mask,
                                          u64& diff_bytes, FirstDiff& first) const {
    mask = 0;
    diff_bytes = 0;
    first = FirstDiff{};
    const u8* now = src.at(header, size);
    if (now == nullptr || rec.snap.size() < size) return;
    const u8* old = rec.snap.data();
    const u64 blob_end = static_cast<u64>(header) + view.blob_extent;

    const auto note = [&](u32 region, bool external, u32 offset, const u8* was, const u8* is,
                          u64 available) {
        mask |= 1u << region;
        if (first.region != kRegionNone) return;
        first.region = region;
        first.external = external;
        first.offset = offset;
        const u32 aligned = offset & ~3u;
        if (static_cast<u64>(aligned) + 4u <= available) {
            first.old_word = load_be32(was + aligned);
            first.new_word = load_be32(is + aligned);
        }
    };
    const auto compare_blob = [&](u32 begin, u32 end, u32 region) {
        if (begin >= end || std::memcmp(old + begin, now + begin, end - begin) == 0) return;
        u32 at = begin;
        while (old[at] == now[at]) ++at;
        diff_bytes += coherence_count_diff(old + begin, now + begin, end - begin);
        note(region, false, at, old, now, size);
    };
    compare_blob(4u, 8u, kRegionFlags);
    compare_blob(8u, 12u, kRegionAttrCount);
    if (view.records_overhang) {
        compare_blob(12u, size, kRegionAttrRecords);
        return;
    }
    const auto normalise = [&](u32 word, u32 marker) -> u32 {
        return word >= header && word < blob_end ? word - header : (word != 0u ? marker : 0u);
    };
    const auto compare_record = [&](u32 offset, u32 marker, u32 region) {
        if (normalise(load_be32(old + offset), marker) == normalise(load_be32(now + offset), marker) &&
            load_be32(old + offset + 4u) == load_be32(now + offset + 4u)) {
            return;
        }
        compare_blob(offset, offset + 8u, region);
    };
    for (u32 i = 0; i < attributes; ++i) {
        compare_record(12u + 8u * i, 0xE0000000u | i, kRegionAttrRecords);
    }
    for (u32 m = 0; m < view.meshes; ++m) {
        compare_record(view.records_end + 8u * m, 0xD0000000u | m, kRegionMeshRecords);
    }

    // Body: segment [body_begin,size) at every slot / display-list / covered
    // boundary; each atomic segment has one label (slot > display list > gap).
    struct Labelled {
        u32 begin, end, region;
    };
    std::vector<Labelled> slot_ranges;
    std::vector<std::pair<u32, u32>> lists;
    std::vector<u32> cuts{view.body_begin, size};
    const auto clip = [&](u64 begin, u64 end, u32& low, u32& high) {
        const u64 b = std::max<u64>(begin, view.body_begin);
        const u64 e = std::min<u64>(end, size);
        if (b >= e) return false;
        low = static_cast<u32>(b);
        high = static_cast<u32>(e);
        return true;
    };
    for (u32 i = 0; i < attributes; ++i) {
        const u32 bit = 1u << i;
        if ((view.inblob_mask & bit) == 0u || (view.covered_mask & bit) != 0u) continue;
        u32 low = 0, high = 0;
        const u64 relative = view.ptr[i] - header;
        if (!clip(relative, relative + view.bytes[i], low, high)) continue;
        slot_ranges.push_back({low, high, kRegionSlotBase + i});
        cuts.push_back(low);
        cuts.push_back(high);
    }
    for (u32 m = 0; m < view.meshes; ++m) {
        const u8* record = now + view.records_end + 8u * m;
        const u32 list = load_be32(record);
        if (list < header || list >= blob_end) continue;
        u32 low = 0, high = 0;
        const u64 relative = list - header;
        if (!clip(relative, relative + load_be32(record + 4u), low, high)) continue;
        lists.emplace_back(low, high);
        cuts.push_back(low);
        cuts.push_back(high);
    }
    for (u32 r = 0; r < view.covered_ranges; ++r) {
        cuts.push_back(view.covered[r].first);
        cuts.push_back(view.covered[r].second);
    }
    std::sort(lists.begin(), lists.end());
    std::vector<u32> list_max_end(lists.size());
    for (std::size_t i = 0; i < lists.size(); ++i) {
        list_max_end[i] = i == 0 ? lists[i].second : std::max(list_max_end[i - 1], lists[i].second);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (std::size_t c = 0; c + 1 < cuts.size(); ++c) {
        const u32 begin = cuts[c];
        const u32 end = cuts[c + 1];
        if (begin < view.body_begin || end > size || begin >= end) continue;
        bool excluded = false;
        for (u32 r = 0; r < view.covered_ranges; ++r) {
            if (begin >= view.covered[r].first && begin < view.covered[r].second) excluded = true;
        }
        if (excluded || std::memcmp(old + begin, now + begin, end - begin) == 0) continue;
        u32 region = kRegionBodyGap;
        bool labelled = false;
        for (const Labelled& range : slot_ranges) {
            if (begin >= range.begin && begin < range.end) {
                region = range.region;
                labelled = true;
                break;
            }
        }
        if (!labelled && !lists.empty()) {
            const auto it = std::upper_bound(
                lists.begin(), lists.end(), begin,
                [](u32 value, const std::pair<u32, u32>& range) { return value < range.first; });
            if (it != lists.begin()) {
                const std::size_t index = static_cast<std::size_t>(it - lists.begin()) - 1u;
                if (list_max_end[index] > begin) region = kRegionDisplayList;
            }
        }
        compare_blob(begin, end, region);
    }

    // Covered and external slots. In-blob covered slots are compared inside the
    // blob copy; external ones against their own snapshot range (a moved
    // pointer compares the NEW bytes with the OLD content, which is what the
    // host has).
    for (u32 i = 0; i < attributes; ++i) {
        const u32 bit = 1u << i;
        if (((view.covered_mask | view.ext_mask) & bit) == 0u) continue;
        if ((view.inblob_mask & bit) != 0u) {
            const u64 relative = view.ptr[i] - header;
            const u64 end = std::min<u64>(relative + view.bytes[i], size);
            if (relative < end) {
                compare_blob(static_cast<u32>(relative), static_cast<u32>(end), kRegionSlotBase + i);
            }
            continue;
        }
        if ((rec.snap_mask & bit) == 0u) continue;
        const u8* was = rec.snap.data() + rec.snap_off[i];
        const u8* is = src.at(view.ptr[i], view.bytes[i]);
        if (is == nullptr) continue;
        const u32 common = std::min(view.bytes[i], rec.snap_len[i]);
        const bool length_changed = view.bytes[i] != rec.snap_len[i];
        if (!length_changed && std::memcmp(was, is, common) == 0) continue;
        u32 at = 0;
        while (at < common && was[at] == is[at]) ++at;
        diff_bytes += coherence_count_diff(was, is, common) +
                      (length_changed ? (std::max(view.bytes[i], rec.snap_len[i]) - common) : 0u);
        note(kRegionSlotBase + i, true, at, was, is, common);
    }
}

void RwCapture::CoherenceProbe::hash_token(Rec& rec, u32 entry, const Source& src,
                                           const View& view, u32 header, u32 size) {
    // HASH-TOKEN attribution. Production's distinct-content hash includes the
    // token [0,2), the serial [2,4) AND the [size,size+32) arena tail; the probe
    // drops all three. Three capped distinct counts separate the causes:
    //   minus_token        = serial + probe content + tail
    //   minus_token_serial = probe content + tail
    //   primary            = probe content (the coherence hash itself)
    ++ht_uploads_;
    const u64 primary = view.combined;
    u64 tail = kCoherenceSeed ^ 0x7A11ull;
    if (view.blob_extent > size) {
        if (const u8* bytes = src.at(header + size, view.blob_extent - size)) {
            tail = coherence_bytes(tail, bytes, view.blob_extent - size);
        }
    }
    const u64 minus_token_serial = coherence_step(primary, tail);
    u64 minus_token = minus_token_serial;
    if (const u8* bytes = src.at(header + 2u, 2u)) {
        minus_token = coherence_step(minus_token_serial, 0x5E00000000000000ull | load_be16(bytes));
    }
    const auto insert = [this](std::unordered_set<u64>& set, u64 value) {
        if (set.size() < kHashSetCap || set.count(value) != 0u) {
            set.insert(value);
        } else {
            ht_overflow_ = true;
        }
    };
    insert(ht_minus_token_, minus_token);
    insert(ht_minus_token_serial_, minus_token_serial);
    const auto found = ht_primary_.find(primary);
    if (found == ht_primary_.end()) {
        if (ht_primary_.size() < kHashSetCap) {
            ht_primary_.emplace(primary, entry);
        } else {
            ht_overflow_ = true;
        }
    } else if (rec.upload_primary == primary) {
        ++ht_repeat_same_entry_;
    } else {
        ++ht_repeat_other_entry_;
    }
    rec.upload_primary = primary;
}

void RwCapture::CoherenceProbe::observe_use(Cpu& cpu, const ObjectContext& context,
                                            const GeometryRecord& record, u32 entry, u32 header,
                                            u32 size, u32 attributes, u32 vertex_count,
                                            bool sector, u32 geometry, bool upload_now) {
    const auto started = std::chrono::steady_clock::now();
    ++valid_uses_;
    const Source src = live_source();
    const std::size_t pipe = static_cast<std::size_t>(context.pipeline) < kPipes
                                 ? static_cast<std::size_t>(context.pipeline)
                                 : 0u;
    PipeStats& p = pipes_[pipe];
    const bool skin =
        context.pipeline == Pipeline::Skin || context.pipeline == Pipeline::SkinMatFx;
    u32 callback = 0;
    if (!sector) {
        if (const u8* at = src.at(context.object + 0x48u, 4u)) {
            callback = load_be32(at);
        } else {
            ++reads_out_of_range_;
        }
    }
    const bool dmorph = callback == kDMorphSoftwareRender;
    ++p.uses;
    if (sector) ++p.sector_uses;
    if (dmorph) {
        ++p.dmorph_sw_uses;
        ++f_dmorph_;
    }

    trace_scratch_.clear();
    View view;
    build_view(src, header, size, attributes, vertex_count, skin, context.geometry_flags,
               context.mesh_count, view, upload_now ? &trace_scratch_ : nullptr);
    if (!view.ok) {
        ++reads_out_of_range_;
        probe_ns_ += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - started)
                                          .count());
        return;
    }
    p.bytes_hashed += view.bytes_hashed;

    Rec* current = nullptr;
    if (upload_now) {
        Rec& rec = recs_[entry];
        if (rec.generation == record.generation) {
            ++p.reuploads;  // needs_reupload after a renderer refusal: same generation
        } else {
            ++p.uploads;
            ++baselines_new_generation_;
            rec.summary = ~0u;
            rec.stale_seen = false;
            rec.events = 0;
        }
        if (view.mesh_clamped) ++mesh_records_clamped_;
        if (view.records_overhang) ++records_overhang_;
        inblob_array_overhang_ += view.inblob_overhang;
        extent_wrap_ += view.extent_wrap;
        external_display_lists_ += view.external_display_lists;
        ++coverage_checks_;
        if (!coverage_ok(view, size, trace_scratch_)) ++coverage_violations_;
        hash_token(rec, entry, src, view, header, size);

        rec.generation = record.generation;
        rec.size = size;
        rec.header = header;
        rec.serial = record.serial;
        rec.pipeline = static_cast<u8>(pipe);
        rec.base_combined = view.combined;
        rec.last_combined = view.combined;
        rec.base_uncovered = view.uncovered;
        rec.last_uncovered = view.uncovered;
        rec.ext_ptr = view.ptr;
        take_snapshot(rec, src, view, header, size);
        if (rec.uncovered_flush_pending) ++p.uncovered_flush_then_upload;
        rec.flushed_since_use = false;
        rec.uncovered_flush_pending = false;
        current = &rec;

        bool want = selftest_runs_ < config_.selftest_uploads;
        if (!want && selftest_runs_ < 4ull * config_.selftest_uploads) {
            for (const SelfClass& result : self_) {
                if (result.attempts == 0u) want = true;
            }
        }
        if (want) {
            self_test(src, view, header, size, attributes, vertex_count, skin,
                      context.geometry_flags, context.mesh_count);
        }
    } else {
        const auto found = recs_.find(entry);
        if (found == recs_.end() || found->second.generation != record.generation) {
            // Probe enabled mid-generation, or the baseline was dropped.
            ++p.compares_without_baseline;
            ++compares_without_baseline_;
        } else {
            Rec& rec = found->second;
            current = &rec;
            ++p.compares;
            ++compares_;
            ++f_compares_;
            if (dmorph) ++p.dmorph_sw_compares;
            const bool stale = view.combined != rec.base_combined;
            const bool stale_uncovered = view.uncovered != rec.base_uncovered;
            const bool changed = view.combined != rec.last_combined;
            const bool changed_uncovered = view.uncovered != rec.last_uncovered;
            bool moved = false;
            for (u32 i = 0; i < attributes; ++i) {
                if ((view.ext_mask & (1u << i)) != 0u && view.ptr[i] != rec.ext_ptr[i]) moved = true;
            }
            if (moved) ++p.external_pointer_moves;
            if (stale) {
                ++p.stale_uses;
                ++f_stale_;
                if (dmorph) ++p.dmorph_sw_stale_uses;
            }
            if (stale_uncovered) {
                ++p.stale_uncovered_uses;
                ++f_stale_unc_;
                if (dmorph) ++p.dmorph_sw_stale_uncovered_uses;
                if (!rec.stale_seen) {
                    rec.stale_seen = true;
                    ++p.distinct_stale_uncovered_generations;
                }
            }
            // COHERE: the guest flushed uncovered bytes of this record and the
            // capture did not re-upload.
            if (rec.uncovered_flush_pending) {
                ++p.uncovered_flush_without_upload;
                if (changed_uncovered) ++p.uncovered_flush_without_upload_changed;
            }
            if (changed) {
                if (rec.flushed_since_use) {
                    ++p.change_with_flush;
                } else {
                    ++p.change_without_flush;
                }
            } else if (rec.flushed_since_use) {
                ++p.flush_without_change;
            }

            // The Present.frame this draw lands in: the NEXT present closes the
            // segment it was emitted into (native_gpu matches
            // --native-screenshot-frame against that same ordinal).
            const u64 present = static_cast<u64>(owner_.present_ordinal_) + 1u;
            u32 mask = 0;
            u64 diff_bytes = 0;
            FirstDiff first{};
            const bool has_diff = changed && !rec.snap.empty();
            if (changed) {
                ++p.change_events;
                if (changed_uncovered) {
                    ++p.change_uncovered_events;
                    ++f_change_unc_;
                }
                if (dmorph) ++p.dmorph_sw_change_events;
                if (skin) ++f_skin_change_;
                if (has_diff) {
                    attribute(src, view, rec, header, size, attributes, mask, diff_bytes, first);
                }
                for (u32 b = 0; b < kRegionFixedCount; ++b) {
                    if ((mask & (1u << b)) != 0u) ++p.region_hits[b];
                }
                for (u32 i = 0; i < kSlots; ++i) {
                    if ((mask & (1u << (kRegionSlotBase + i))) != 0u) ++p.slot_hits[i];
                }
            }
            const u32 geometry_targets = geometry != 0u && src.at(geometry + 0x18u, 4u) != nullptr
                                             ? load_be32(src.at(geometry + 0x18u, 4u))
                                             : 0u;
            if (stale_uncovered && rec.summary == ~0u) {
                if (summaries_.size() < config_.max_record_summaries) {
                    rec.summary = static_cast<u32>(summaries_.size());
                    Summary s;
                    s.entry = entry;
                    s.generation = record.generation;
                    s.header = header;
                    s.size = size;
                    s.object = context.object;
                    s.render_callback = callback;
                    s.geometry = geometry;
                    s.morph_targets = geometry_targets;
                    s.vertex_count = vertex_count;
                    s.attributes = attributes;
                    s.meshes = view.meshes;
                    s.serial = record.serial;
                    s.pipeline = static_cast<u8>(pipe);
                    s.sector = sector;
                    summaries_.push_back(s);
                } else {
                    ++summaries_overflow_;
                    rec.summary = ~0u - 1u;  // counted once, never retried
                }
            }
            if (rec.summary < summaries_.size()) {
                Summary& s = summaries_[rec.summary];
                ++s.uses_since_first_stale;
                if (stale) ++s.stale_uses;
                if (stale_uncovered) ++s.stale_uncovered_uses;
                if (changed) {
                    ++s.change_events;
                    if (s.first_change_present == 0u) s.first_change_present = present;
                    s.last_change_present = present;
                }
                if (changed_uncovered) ++s.change_uncovered_events;
                s.region_mask_union |= mask;
            }
            if (changed) {
                const bool room = skin ? skin_events_ < config_.max_skin_events
                                       : events_.size() - skin_events_ < config_.max_events;
                if (room && rec.events < config_.max_events_per_record) {
                    ++rec.events;
                    if (skin) ++skin_events_;
                    Event e;
                    e.present = present;
                    e.game_ticks = cpu.game_clock().now();
                    e.entry = entry;
                    e.header = header;
                    e.generation = record.generation;
                    e.size = size;
                    e.object = context.object;
                    e.render_callback = callback;
                    e.geometry = geometry;
                    e.morph_targets = geometry_targets;
                    e.vertex_count = vertex_count;
                    e.region_mask = mask;
                    e.serial = record.serial;
                    e.pipeline = static_cast<u8>(pipe);
                    e.sector = sector;
                    e.stale_uncovered = stale_uncovered;
                    e.changed_uncovered = changed_uncovered;
                    e.flushed_since_use = rec.flushed_since_use;
                    e.uncovered_flush_pending = rec.uncovered_flush_pending;
                    e.has_diff = has_diff;
                    e.diff_bytes = diff_bytes;
                    e.first = first;
                    events_.push_back(e);
                } else {
                    ++events_dropped_;
                }
            }
            rec.last_combined = view.combined;
            rec.last_uncovered = view.uncovered;
            rec.flushed_since_use = false;
            rec.uncovered_flush_pending = false;
        }
    }

    // Ranges at this use, for the flush-overlap classification (physical).
    if (current != nullptr) {
        current->covered_mask = view.covered_mask;
        current->inblob_mask = view.inblob_mask;
        current->ext_mask = view.ext_mask;
        current->slot_ptr = view.ptr;
        current->slot_len = view.bytes;
        u64 low = header;
        u64 high = low + size;
        const u32 external = (view.covered_mask | view.ext_mask) & ~view.inblob_mask;
        for (u32 i = 0; i < kSlots; ++i) {
            if ((external & (1u << i)) == 0u) continue;
            const u64 begin = view.ptr[i];
            low = std::min(low, begin);
            high = std::max(high, begin + view.bytes[i]);
        }
        current->lo = low;
        current->hi = high;
    }
    live_peak_ = std::max<u64>(live_peak_, recs_.size());
    probe_ns_ += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count());
}

bool RwCapture::CoherenceProbe::covered_inblob_contains(const Rec& rec, u64 begin, u64 end) {
    std::array<std::pair<u64, u64>, 2> ranges{};
    u32 count = 0;
    for (u32 i = 0; i < 2u; ++i) {
        const u32 bit = 1u << i;
        if ((rec.covered_mask & bit) == 0u || (rec.inblob_mask & bit) == 0u) continue;
        const u64 low = rec.slot_ptr[i];
        ranges[count++] = {low, low + rec.slot_len[i]};
    }
    if (count == 2u && ranges[1].first < ranges[0].first) std::swap(ranges[0], ranges[1]);
    u64 cursor = begin;
    for (u32 r = 0; r < count; ++r) {
        if (ranges[r].first <= cursor && ranges[r].second > cursor) cursor = ranges[r].second;
    }
    return cursor >= end;
}

void RwCapture::CoherenceProbe::on_flush(Cpu& cpu, bool store) {
    const auto started = std::chrono::steady_clock::now();
    // Arguments at entry: r3 = address, r4 = length. Read only.
    const u32 address = cpu.gpr[3];
    const u32 length = cpu.gpr[4];
    const u32 lr = cpu.lr;
    if (store) {
        ++store_calls_;
    } else {
        ++flush_calls_;
    }
    flush_bytes_ += length;
    LrBucket* bucket = nullptr;
    for (std::size_t i = 0; i < lr_used_; ++i) {
        if (lr_[i].lr == lr) {
            bucket = &lr_[i];
            break;
        }
    }
    if (bucket == nullptr && lr_used_ < lr_.size()) {
        bucket = &lr_[lr_used_++];
        bucket->lr = lr;
    }
    if (bucket != nullptr) {
        ++bucket->calls;
    } else {
        ++lr_other_calls_;
    }
    const bool raster = (lr >= kRasterUnlock && lr < kRasterUnlockEnd) ||
                        (lr >= kRasterUnlockPalette && lr < kRasterUnlockPaletteEnd);
    if (raster) ++raster_unlock_calls_;

    bool overlapped = false;
    if (length != 0u) {
        const u64 begin = address;
        const u64 end = begin + length;
        for (auto& item : recs_) {
            Rec& rec = item.second;
            if (end <= rec.lo || begin >= rec.hi) continue;
            bool covered = false;
            bool uncovered_inblob = false;
            bool uncovered_external = false;
            const u64 blob_begin = rec.header;
            const u64 blob_end = blob_begin + rec.size;
            if (begin < blob_end && end > blob_begin) {
                if (covered_inblob_contains(rec, std::max(begin, blob_begin),
                                            std::min(end, blob_end))) {
                    covered = true;
                } else {
                    uncovered_inblob = true;
                }
            }
            const u32 external = (rec.covered_mask | rec.ext_mask) & ~rec.inblob_mask;
            for (u32 i = 0; i < kSlots; ++i) {
                const u32 bit = 1u << i;
                if ((external & bit) == 0u) continue;
                const u64 slot_begin = rec.slot_ptr[i];
                const u64 slot_end = slot_begin + rec.slot_len[i];
                if (begin < slot_end && end > slot_begin) {
                    if ((rec.covered_mask & bit) != 0u) {
                        covered = true;
                    } else {
                        uncovered_external = true;
                    }
                }
            }
            if (!covered && !uncovered_inblob && !uncovered_external) continue;
            overlapped = true;
            if (raster) {
                ++raster_unlock_overlaps_;
                continue;
            }
            PipeStats& p = pipes_[rec.pipeline < kPipes ? rec.pipeline : 0u];
            if (covered) ++p.flush_overlap_covered;
            if (uncovered_inblob) ++p.flush_overlap_uncovered_inblob;
            if (uncovered_external) ++p.flush_overlap_uncovered_external;
            rec.flushed_since_use = true;
            if (uncovered_inblob || uncovered_external) rec.uncovered_flush_pending = true;
        }
    }
    if (overlapped) {
        ++flush_overlapping_calls_;
        if (bucket != nullptr) {
            ++bucket->overlapping_calls;
        } else {
            ++lr_other_overlapping_;
        }
    }
    probe_ns_ += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count());
}

void RwCapture::CoherenceProbe::on_present() {
    // Called right after present_ordinal_ was advanced: everything accumulated
    // since the previous present was drawn into THIS Present.frame.
    ++frames_;
    const u64 present = owner_.present_ordinal_;
    if (f_compares_ != 0u) ++frames_with_compares_;
    if (f_stale_ != 0u) ++frames_with_stale_;
    if (f_stale_unc_ != 0u) {
        ++frames_with_stale_unc_;
        if (first_stale_unc_present_ == 0u) first_stale_unc_present_ = present;
        last_stale_unc_present_ = present;
    }
    if (f_change_unc_ != 0u) ++frames_with_change_unc_;
    if (f_dmorph_ != 0u) ++frames_with_dmorph_;
    if (f_skin_change_ != 0u) ++frames_with_skin_change_;
    max_stale_unc_per_frame_ = std::max(max_stale_unc_per_frame_, f_stale_unc_);
    const u64 v = f_stale_unc_;
    const std::size_t bucket = v == 0u ? 0u : v == 1u ? 1u : v < 4u ? 2u : v < 8u ? 3u
                             : v < 16u ? 4u : v < 32u ? 5u : 6u;
    ++stale_unc_hist_[bucket];
    f_compares_ = f_stale_ = f_stale_unc_ = f_change_unc_ = f_dmorph_ = f_skin_change_ = 0;
}

void RwCapture::CoherenceProbe::on_release(u32 entry) {
    const auto found = recs_.find(entry);
    if (found == recs_.end()) return;
    drop_snapshot(found->second);
    recs_.erase(found);
    ++released_;
}

void RwCapture::CoherenceProbe::on_state_loaded() {
    // Production cleared geometry_: every record restarts with a fresh upload.
    recs_.clear();
    snapshot_bytes_ = 0;
    ++state_loads_;
}

const char* RwCapture::CoherenceProbe::pipe_name(std::size_t index) noexcept {
    static constexpr const char* kNames[kPipes] = {"None",  "Default",   "MatFx",
                                                  "Skin",  "SkinMatFx", "PTank"};
    return index < kPipes ? kNames[index] : "?";
}

std::string RwCapture::CoherenceProbe::region_name(u32 region) {
    static constexpr const char* kNames[kRegionFixedCount] = {
        "flags", "attr_count", "attr_records", "mesh_records", "display_list", "body_gap"};
    if (region < kRegionFixedCount) return kNames[region];
    if (region >= kRegionSlotBase && region < kRegionSlotBase + kSlots) {
        return "slot" + std::to_string(region - kRegionSlotBase);
    }
    return "none";
}

std::string RwCapture::CoherenceProbe::json() const {
    std::ostringstream o;
    const auto b = [](bool value) { return value ? "true" : "false"; };
    const Report& r = owner_.report_;

    o << "{\"enabled\":true,\"capture_active\":" << b(owner_.installed_);
    o << ",\"method\":{\"hash\":\"u64 word-step, bijective per 8-byte word\","
         "\"excluded\":[\"header+0 u16 PE draw-sync token\",\"header+2 u16 mesh serial\","
         "\"[size,size+32) arena tail\"],"
         "\"normalised\":[\"attribute record +0 (in-blob: header-relative; external: slot "
         "marker, content hashed separately)\",\"mesh record +0\"],"
         "\"covered_slots\":\"Skin/SkinMatFx slot0, slot1 if geometry_flags&0x10 (production "
         "GeometryDynamic set)\","
         "\"present_convention\":\"event.present and *_present = native Present.frame "
         "(present_ordinal) of the frame the draw lands in; use with "
         "--native-screenshot-frame\"}";
    o << ",\"config\":{\"max_events\":" << config_.max_events
      << ",\"max_skin_events\":" << config_.max_skin_events
      << ",\"max_events_per_record\":" << config_.max_events_per_record
      << ",\"max_record_summaries\":" << config_.max_record_summaries
      << ",\"snapshot_budget_bytes\":" << config_.snapshot_budget_bytes
      << ",\"selftest_uploads\":" << config_.selftest_uploads
      << ",\"flush_overlap\":" << b(config_.flush_overlap) << "}";

    u64 dmorph_uses = 0;
    for (const PipeStats& p : pipes_) dmorph_uses += p.dmorph_sw_uses;
    o << ",\"executed\":{\"valid_uses\":" << valid_uses_
      << ",\"invalid_entries_production\":" << r.geometry_invalid_entries
      << ",\"baselines_new_generation\":" << baselines_new_generation_
      << ",\"compares\":" << compares_ << ",\"compares_without_baseline\":"
      << compares_without_baseline_ << ",\"dmorph_sw_uses\":" << dmorph_uses
      << ",\"frames\":" << frames_ << ",\"flush_calls\":" << flush_calls_
      << ",\"store_calls\":" << store_calls_ << "}";

    o << ",\"pipelines\":{";
    for (std::size_t index = 1; index <= 4u; ++index) {
        const PipeStats& p = pipes_[index];
        if (index != 1u) o << ",";
        o << "\"" << pipe_name(index) << "\":{\"uses\":" << p.uses
          << ",\"sector_uses\":" << p.sector_uses << ",\"uploads\":" << p.uploads
          << ",\"reuploads\":" << p.reuploads << ",\"compares\":" << p.compares
          << ",\"compares_without_baseline\":" << p.compares_without_baseline
          << ",\"stale_uses\":" << p.stale_uses
          << ",\"stale_uncovered_uses\":" << p.stale_uncovered_uses
          << ",\"change_events\":" << p.change_events
          << ",\"change_uncovered_events\":" << p.change_uncovered_events
          << ",\"distinct_stale_uncovered_generations\":"
          << p.distinct_stale_uncovered_generations << ",\"dmorph_sw_uses\":" << p.dmorph_sw_uses
          << ",\"dmorph_sw_compares\":" << p.dmorph_sw_compares
          << ",\"dmorph_sw_stale_uses\":" << p.dmorph_sw_stale_uses
          << ",\"dmorph_sw_stale_uncovered_uses\":" << p.dmorph_sw_stale_uncovered_uses
          << ",\"dmorph_sw_change_events\":" << p.dmorph_sw_change_events
          << ",\"bytes_hashed\":" << p.bytes_hashed
          << ",\"external_pointer_moves\":" << p.external_pointer_moves
          << ",\"change_with_flush\":" << p.change_with_flush
          << ",\"change_without_flush\":" << p.change_without_flush
          << ",\"flush_without_change\":" << p.flush_without_change
          << ",\"flush_overlap_covered\":" << p.flush_overlap_covered
          << ",\"flush_overlap_uncovered_inblob\":" << p.flush_overlap_uncovered_inblob
          << ",\"flush_overlap_uncovered_external\":" << p.flush_overlap_uncovered_external
          << ",\"uncovered_flush_then_upload\":" << p.uncovered_flush_then_upload
          << ",\"uncovered_flush_without_upload\":" << p.uncovered_flush_without_upload
          << ",\"uncovered_flush_without_upload_changed\":"
          << p.uncovered_flush_without_upload_changed << ",\"region_hits\":{";
        for (u32 region = 0; region < kRegionFixedCount; ++region) {
            if (region != 0u) o << ",";
            o << "\"" << region_name(region) << "\":" << p.region_hits[region];
        }
        o << "},\"slot_hits\":[";
        for (u32 i = 0; i < kSlots; ++i) o << (i != 0u ? "," : "") << p.slot_hits[i];
        o << "]}";
    }
    o << "}";

    const auto optional_present = [](u64 value) {
        return value == 0u ? std::string("null") : std::to_string(value);
    };
    o << ",\"frames\":{\"presents\":" << frames_ << ",\"with_compares\":" << frames_with_compares_
      << ",\"with_stale\":" << frames_with_stale_
      << ",\"with_stale_uncovered\":" << frames_with_stale_unc_
      << ",\"with_change_uncovered\":" << frames_with_change_unc_
      << ",\"with_dmorph_sw\":" << frames_with_dmorph_
      << ",\"with_skin_change\":" << frames_with_skin_change_
      << ",\"max_stale_uncovered\":" << max_stale_unc_per_frame_
      << ",\"stale_uncovered_hist\":{\"0\":" << stale_unc_hist_[0] << ",\"1\":"
      << stale_unc_hist_[1] << ",\"2-3\":" << stale_unc_hist_[2] << ",\"4-7\":"
      << stale_unc_hist_[3] << ",\"8-15\":" << stale_unc_hist_[4] << ",\"16-31\":"
      << stale_unc_hist_[5] << ",\"32+\":" << stale_unc_hist_[6] << "}"
      << ",\"first_stale_uncovered_present\":" << optional_present(first_stale_unc_present_)
      << ",\"last_stale_uncovered_present\":" << optional_present(last_stale_unc_present_) << "}";

    std::vector<std::size_t> order(summaries_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    const std::size_t top = std::min<std::size_t>(order.size(), 64u);
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(top), order.end(),
                      [this](std::size_t left, std::size_t right) {
                          return summaries_[left].stale_uncovered_uses >
                                 summaries_[right].stale_uncovered_uses;
                      });
    o << ",\"records\":{\"live\":" << recs_.size() << ",\"live_peak\":" << live_peak_
      << ",\"released\":" << released_ << ",\"state_loads\":" << state_loads_
      << ",\"summaries\":" << summaries_.size() << ",\"summaries_overflow\":" << summaries_overflow_
      << ",\"top\":[";
    for (std::size_t n = 0; n < top; ++n) {
        const Summary& s = summaries_[order[n]];
        if (n != 0u) o << ",";
        o << "{\"entry\":" << coherence_hex(s.entry) << ",\"header\":" << coherence_hex(s.header)
          << ",\"generation\":" << s.generation << ",\"serial\":" << s.serial
          << ",\"size\":" << s.size << ",\"pipeline\":\"" << pipe_name(s.pipeline)
          << "\",\"sector\":" << b(s.sector) << ",\"object\":" << coherence_hex(s.object)
          << ",\"render_callback\":" << coherence_hex(s.render_callback)
          << ",\"dmorph_sw\":" << b(s.render_callback == kDMorphSoftwareRender)
          << ",\"geometry\":" << coherence_hex(s.geometry)
          << ",\"morph_targets\":" << s.morph_targets << ",\"vertex_count\":" << s.vertex_count
          << ",\"attributes\":" << s.attributes << ",\"meshes\":" << s.meshes
          << ",\"uses_since_first_stale\":" << s.uses_since_first_stale
          << ",\"stale_uses\":" << s.stale_uses
          << ",\"stale_uncovered_uses\":" << s.stale_uncovered_uses
          << ",\"change_events\":" << s.change_events
          << ",\"change_uncovered_events\":" << s.change_uncovered_events
          << ",\"first_change_present\":" << optional_present(s.first_change_present)
          << ",\"last_change_present\":" << optional_present(s.last_change_present)
          << ",\"region_mask_union\":" << coherence_hex(s.region_mask_union) << "}";
    }
    o << "]}";

    o << ",\"events\":{\"recorded\":" << events_.size() << ",\"skin_recorded\":" << skin_events_
      << ",\"dropped\":" << events_dropped_ << ",\"list\":[";
    for (std::size_t n = 0; n < events_.size(); ++n) {
        const Event& e = events_[n];
        if (n != 0u) o << ",";
        o << "{\"kind\":\"change\",\"present\":" << e.present << ",\"game_ticks\":" << e.game_ticks
          << ",\"entry\":" << coherence_hex(e.entry) << ",\"header\":" << coherence_hex(e.header)
          << ",\"generation\":" << e.generation << ",\"serial\":" << e.serial
          << ",\"size\":" << e.size << ",\"pipeline\":\"" << pipe_name(e.pipeline)
          << "\",\"sector\":" << b(e.sector) << ",\"object\":" << coherence_hex(e.object)
          << ",\"render_callback\":" << coherence_hex(e.render_callback)
          << ",\"dmorph_sw\":" << b(e.render_callback == kDMorphSoftwareRender)
          << ",\"geometry\":" << coherence_hex(e.geometry)
          << ",\"morph_targets\":" << e.morph_targets << ",\"vertex_count\":" << e.vertex_count
          << ",\"stale_uncovered\":" << b(e.stale_uncovered)
          << ",\"changed_uncovered\":" << b(e.changed_uncovered)
          << ",\"flushed_since_use\":" << b(e.flushed_since_use)
          << ",\"uncovered_flush_pending\":" << b(e.uncovered_flush_pending)
          << ",\"region_mask\":" << coherence_hex(e.region_mask) << ",\"diff_bytes\":"
          << e.diff_bytes << ",\"first_diff\":";
        if (!e.has_diff || e.first.region == kRegionNone) {
            o << "null";
        } else {
            o << "{\"region\":\"" << region_name(e.first.region)
              << "\",\"external\":" << b(e.first.external) << ",\"offset\":" << e.first.offset
              << ",\"old_be32\":" << coherence_hex(e.first.old_word)
              << ",\"new_be32\":" << coherence_hex(e.first.new_word) << "}";
        }
        o << "}";
    }
    o << "]}";

    // Flush observers: installed, still ours, and actually called.
    const auto hook_state = [this, &b](u32 address) {
        bool present = false;
        bool intact = false;
        for (const auto& entry : owner_.system_.cpu().hle_entries()) {
            if (entry.address != address) continue;
            present = true;
            intact = entry.name.find("r4 coherence") != std::string::npos;
        }
        return std::string("{\"address\":") + coherence_hex(address) + ",\"present\":" +
               b(present) + ",\"intact\":" + b(intact) + "}";
    };
    o << ",\"flush\":{\"enabled\":" << b(config_.flush_overlap)
      << ",\"installed\":" << b(flush_installed_);
    if (flush_installed_) {
        o << ",\"hooks\":[" << hook_state(kDCFlushRange) << "," << hook_state(kDCStoreRange) << "]";
    }
    o << ",\"calls\":" << flush_calls_ << ",\"store_calls\":" << store_calls_
      << ",\"bytes\":" << flush_bytes_ << ",\"overlapping_calls\":" << flush_overlapping_calls_
      << ",\"raster_unlock_calls\":" << raster_unlock_calls_
      << ",\"raster_unlock_overlaps\":" << raster_unlock_overlaps_ << ",\"by_lr\":[";
    for (std::size_t i = 0; i < lr_used_; ++i) {
        const LrBucket& bucket = lr_[i];
        const char* site = "";
        for (u32 lr : kReinstanceFlushLr) {
            if (lr == bucket.lr) site = "reinstance fn_80464624";
        }
        for (u32 lr : kRasterFlushLr) {
            if (lr == bucket.lr) site = "RwRasterUnlock";
        }
        if (i != 0u) o << ",";
        o << "{\"lr\":" << coherence_hex(bucket.lr) << ",\"site\":\"" << site
          << "\",\"calls\":" << bucket.calls << ",\"overlapping_calls\":"
          << bucket.overlapping_calls << "}";
    }
    o << "],\"lr_other_calls\":" << lr_other_calls_
      << ",\"lr_other_overlapping_calls\":" << lr_other_overlapping_ << "}";

    o << ",\"hash_token\":{\"uploads\":" << ht_uploads_
      << ",\"distinct_minus_token\":" << ht_minus_token_.size()
      << ",\"distinct_minus_token_serial\":" << ht_minus_token_serial_.size()
      << ",\"distinct_primary\":" << ht_primary_.size()
      << ",\"primary_repeat_same_entry\":" << ht_repeat_same_entry_
      << ",\"primary_repeat_other_entry\":" << ht_repeat_other_entry_
      << ",\"set_overflow\":" << b(ht_overflow_)
      << ",\"production_uploads\":" << r.geometry_uploads
      << ",\"production_identity_distinct\":" << r.geometry_identity_distinct
      << ",\"production_identity_repeat_live\":" << r.geometry_identity_repeat_live
      << ",\"production_identity_resurrected\":" << r.geometry_identity_resurrected << "}";

    static constexpr const char* kSelfNames[kSelfClasses] = {
        "flags", "attr_count", "attr_record", "mesh_record", "display_list",
        "inblob_uncovered_slot", "body_gap"};
    bool clean = selftest_runs_ != 0u && selftest_identity_mismatch_ == 0u &&
                 coverage_violations_ == 0u;
    bool exercised = true;
    for (const SelfClass& result : self_) {
        if (result.missed != 0u || result.combined_only != 0u) clean = false;
        if (result.attempts == 0u) exercised = false;
    }
    o << ",\"selftest\":{\"runs\":" << selftest_runs_
      << ",\"identity_mismatch\":" << selftest_identity_mismatch_
      << ",\"skipped_large\":" << selftest_skipped_large_
      << ",\"coverage_checks\":" << coverage_checks_
      << ",\"coverage_violations\":" << coverage_violations_ << ",\"classes\":{";
    for (u32 c = 0; c < kSelfClasses; ++c) {
        const SelfClass& result = self_[c];
        if (c != 0u) o << ",";
        o << "\"" << kSelfNames[c] << "\":{\"attempts\":" << result.attempts
          << ",\"detected\":" << result.detected << ",\"combined_only\":" << result.combined_only
          << ",\"missed\":" << result.missed << ",\"unavailable\":" << result.unavailable << "}";
    }
    o << "},\"passed\":" << b(clean) << ",\"all_classes_exercised\":" << b(exercised) << "}";

    o << ",\"limits\":{\"snapshot_bytes\":" << snapshot_bytes_
      << ",\"snapshot_peak\":" << snapshot_peak_
      << ",\"snapshot_budget\":" << config_.snapshot_budget_bytes
      << ",\"records_without_snapshot\":" << records_without_snapshot_
      << ",\"reads_out_of_range\":" << reads_out_of_range_
      << ",\"mesh_records_clamped\":" << mesh_records_clamped_
      << ",\"records_overhang\":" << records_overhang_
      << ",\"inblob_array_overhang\":" << inblob_array_overhang_
      << ",\"extent_u32_wrap\":" << extent_wrap_
      << ",\"external_display_lists\":" << external_display_lists_ << "}";
    o << ",\"cost\":{\"probe_ns\":" << probe_ns_ << "}}";
    return o.str();
}

std::string RwCapture::CoherenceProbe::summary() const {
    std::ostringstream o;
    const PipeStats& d = pipes_[1];
    const PipeStats& m = pipes_[2];
    const PipeStats& s = pipes_[3];
    const PipeStats& sm = pipes_[4];
    o << "[r4-cohere] valid uses " << valid_uses_ << " compares " << compares_ << " (no-baseline "
      << compares_without_baseline_ << ") | Default stale-uncovered " << d.stale_uncovered_uses
      << " change " << d.change_uncovered_events << " | MatFx stale-uncovered "
      << m.stale_uncovered_uses << " change " << m.change_uncovered_events
      << " | Skin stale-uncovered " << s.stale_uncovered_uses << " | SkinMatFx stale-uncovered "
      << sm.stale_uncovered_uses << "\n";
    u64 dmorph_uses = 0, dmorph_compares = 0, dmorph_stale = 0, dmorph_changes = 0;
    for (const PipeStats& p : pipes_) {
        dmorph_uses += p.dmorph_sw_uses;
        dmorph_compares += p.dmorph_sw_compares;
        dmorph_stale += p.dmorph_sw_stale_uncovered_uses;
        dmorph_changes += p.dmorph_sw_change_events;
    }
    o << "[r4-cohere] dmorph-sw uses " << dmorph_uses << " compares " << dmorph_compares
      << " stale-uncovered " << dmorph_stale << " change " << dmorph_changes << " | frames "
      << frames_ << " with-stale-uncovered " << frames_with_stale_unc_ << " max/frame "
      << max_stale_unc_per_frame_;
    if (dmorph_uses == 0u) o << " (DMorph path UNEXERCISED on this route)";
    o << "\n";
    const u64 skin_compares = s.compares + sm.compares;
    const u64 skin_changes = s.change_events + sm.change_events;
    o << "[r4-cohere] Skin validation (external covered slots): compares " << skin_compares
      << " change-events " << skin_changes << " (uncovered "
      << s.change_uncovered_events + sm.change_uncovered_events
      << ") -> hash sees changing data: "
      << (skin_compares == 0u ? "NOT ESTABLISHED (no skin compares)"
                              : (skin_changes != 0u ? "yes" : "no"))
      << "\n";
    bool clean = selftest_runs_ != 0u && selftest_identity_mismatch_ == 0u &&
                 coverage_violations_ == 0u;
    u64 attempts = 0, detected = 0;
    for (const SelfClass& result : self_) {
        attempts += result.attempts;
        detected += result.detected;
        if (result.missed != 0u || result.combined_only != 0u) clean = false;
    }
    o << "[r4-cohere] in-blob self-test runs " << selftest_runs_ << " flips " << attempts
      << " detected " << detected << " coverage violations " << coverage_violations_ << "/"
      << coverage_checks_ << " -> " << (clean ? "PASSED" : "NOT PASSED") << "\n";
    u64 cohere = 0, cohere_changed = 0;
    for (const PipeStats& p : pipes_) {
        cohere += p.uncovered_flush_without_upload;
        cohere_changed += p.uncovered_flush_without_upload_changed;
    }
    o << "[r4-cohere] flush " << flush_calls_ << " store " << store_calls_ << " overlapping "
      << flush_overlapping_calls_ << " (raster-unlock " << raster_unlock_calls_
      << ") uncovered-flush-without-upload " << cohere << " (hash changed " << cohere_changed
      << ") | events " << events_.size() << " dropped " << events_dropped_ << " | snapshots "
      << static_cast<double>(snapshot_peak_) / (1024.0 * 1024.0) << " MiB peak | probe "
      << static_cast<double>(probe_ns_) / 1e6 << " ms\n";
    o << "[r4-cohere] hash-token: uploads " << ht_uploads_ << " distinct primary "
      << ht_primary_.size() << " minus-token+serial " << ht_minus_token_serial_.size()
      << " minus-token " << ht_minus_token_.size() << " vs production identity distinct "
      << owner_.report_.geometry_identity_distinct << " of " << owner_.report_.geometry_uploads
      << (ht_overflow_ ? " (sets overflowed: lower bounds)" : "") << "\n";
    return o.str();
}

// ---- RwCapture side of the STEP-0 probe --------------------------------------

void RwCapture::enable_coherence_probe(const CoherenceProbeConfig& config) {
    if (coherence_ != nullptr) return;
    coherence_ = std::make_unique<CoherenceProbe>(*this, config);
    // Before System::start the observers install inside install(), after every
    // production hook; after it, immediately.
    if (installed_) coherence_->install_flush_observers();
}

std::string RwCapture::coherence_json() const {
    return coherence_ != nullptr ? coherence_->json() : std::string("{\"enabled\":false}");
}

std::string RwCapture::coherence_summary() const {
    return coherence_ != nullptr ? coherence_->summary() : std::string();
}

}  // namespace gekko
