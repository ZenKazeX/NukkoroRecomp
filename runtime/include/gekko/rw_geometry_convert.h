#pragma once

// RwGC instance geometry -> native triangle-list vertex/index data.
//
// The native renderer's GeometryCache input (docs/native-runtime-2026-09-16/
// ARCHITECTURE.md R3/§3). One RwResEntry's GameCube instance data - the
// big-endian header at entry+24, its GX display lists and its indexed
// attribute arrays - becomes one vertex array, one u32 triangle-list index
// array and one index range per RpMesh. Pure functions: no D3D, no guest
// memory access, no global state. Everything the converter reads comes from
// caller-provided byte spans and is bounds-checked; malformed input fails with
// a message, it never reads outside the spans.
//
// Layout (RENDERER_RE/02_NATIVE_GEOMETRY_PIPELINES.md §3.1, capture map §4.4):
//
//   header (BE)  +0x00 u16 drawSyncStamp  +0x02 u16 meshSerial
//                +0x04 u32 hdrFlags (bit0: vertex colours carry alpha)
//                +0x08 u32 numAttributes N
//                +0x0C N x {u32 array, u8 gxAttr, u8 stride, u8 indexType, u8 pad}
//                +0x0C+8N numMeshes x {u32 displayList, u32 sizeBytes}
//
// The header does not store numMeshes (it lives in RpMeshHeader), so the
// header span must be EXACTLY 12 + 8N + 8 * numMeshes bytes - the Native
// Data PLG headerSize, and what the capture copies.
//
// GX binds attributes by RECORD ORDER, not by the gxAttr byte
// (RwGC_SetArrays 0x80463C80): POS, [NRM|NBT if flags&0x10], [CLR0 if
// flags&0x08], [TEX0.. for every remaining record if flags&0x84]. Every
// primitive uses GX_VTXFMT0.
//
// Topology: GX strips/fans/quads/triangles become a triangle LIST in the exact
// GX per-triangle vertex order (the order the legacy decoder in renderer.cpp
// rasterises, which is what cull winding is evaluated on):
//   strip   i >= 2: even i -> (i-2, i-1, i), odd i -> (i-1, i-2, i)
//   fan     i >= 2: (0, i-1, i)
//   quads   per quad q: (q, q+1, q+2), (q, q+2, q+3)   (GX_QUADS and GX_QUADS_2)
//   triangles: as stored
// A triangle whose three OUTPUT vertex ids are not distinct (strip joins) is
// dropped and counted. A geometrically degenerate triangle with distinct ids
// is kept: it is what the guest drew. Lines, line strips and points have no
// meaning inside an RW mesh; they are counted, never silently dropped.
//
// Vertices: an output vertex is a unique tuple of source indices. World
// sectors index every attribute independently (e.g. POS idx16, CLR0 idx8) and
// are unwelded through an open-addressing hash. When every tuple repeats one
// index for all attributes (runtime-instanced models, 0x8046D124) the output
// keeps a 1:1 mapping: output vertex i IS source vertex i.
//
// Non-primitive display-list commands are not expected in RwGC lists. The
// native path does not execute display lists (GraphicsPipe skips
// GX_CMD_CALL_DL by length), so a command that writes state - CP, XF, indexed
// XF, BP, or a nested list - is a CONVERSION FAILURE naming the mesh, offset
// and opcode: it has no other executor. The stateless vertex-cache
// invalidations 0x44/0x48 are skipped by their exact length and REPORTED
// (offset/opcode) in `foreign`.

#include "gekko/types.h"
#include "native_memory.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace gekko {

// GXCompType as RpGameCubeVtxFmt stores it (non-colour attributes).
inline constexpr u8 kGxCompU8 = 0;
inline constexpr u8 kGxCompS8 = 1;
inline constexpr u8 kGxCompU16 = 2;
inline constexpr u8 kGxCompS16 = 3;
inline constexpr u8 kGxCompF32 = 4;

// GXCompType for GX_VA_CLR0.
inline constexpr u8 kGxColorRGB565 = 0;
inline constexpr u8 kGxColorRGB8 = 1;
inline constexpr u8 kGxColorRGBX8 = 2;
inline constexpr u8 kGxColorRGBA4 = 3;
inline constexpr u8 kGxColorRGBA6 = 4;
inline constexpr u8 kGxColorRGBA8 = 5;

// RpGeometry / RpWorld flag bits that decide the attribute list.
inline constexpr u32 kRwGeometryTextured = 0x04;
inline constexpr u32 kRwGeometryPrelit = 0x08;
inline constexpr u32 kRwGeometryNormals = 0x10;
inline constexpr u32 kRwGeometryTextured2 = 0x80;

// POS + NRM + CLR0 + TEX0..TEX7.
inline constexpr std::size_t kRwMaxAttributes = 11;
// NativeVertex carries two coordinate sets (TEX0 and the DUAL/DUALUV TEX1).
// A third set cannot be represented, so conversion FAILS rather than dropping it.
inline constexpr std::size_t kNativeTexcoordSets = 2;

// The VTXFMT0 description RwGC_SetArrays programs for one draw: which
// attributes are present (from the geometry or world flags) and how each is
// quantised (from the RpGameCubeVtxFmt). Raw fmt values are kept as they are;
// the converter rejects invalid ones.
struct GxVertexFormatDesc {
    u8 pos_type = kGxCompF32;
    u8 pos_frac = 0;
    bool normals = false;     // flags & 0x10
    bool normal_nbt = false;  // fmt.nbt: GX_NRM_NBT, 9 components at one index, N first
    // Integer normals use GX's fixed scale (s8/64, u8/128, s16/16384,
    // u16/32768); RwGC passes frac 0 and the VAT has no normal frac.
    u8 nrm_type = kGxCompF32;
    bool colors = false;  // flags & 0x08
    u8 color_type = kGxColorRGBA8;
    // flags & 0x84. The NUMBER of sets is not in the flags: every header
    // record after POS/NRM/CLR0 is a texture set.
    bool texcoords = false;
    std::array<u8, 8> tex_type{kGxCompF32, kGxCompF32, kGxCompF32, kGxCompF32,
                               kGxCompF32, kGxCompF32, kGxCompF32, kGxCompF32};
    std::array<u8, 8> tex_frac{};
    // Matrix-palette skins (not live on Final Haunt): GX_VA_PNMTXIDX, GX_DIRECT,
    // one u8 at the head of every vertex tuple.
    bool matrix_index = false;
};

// Builds the description from the 24 RpGameCubeVtxFmt bytes (+0 posType,
// +1 nrmType, +2..+9 texType, +10 colType, +12 posFrac, +13 nbt,
// +14..+21 texFrac) and the flags the node body passes (geometry flags for
// atomics, curWorld flags for sectors). nullptr selects the driver default
// format 0x805E2748 (all F32, RGBA8), exactly as RwGC_SetArrays does.
GxVertexFormatDesc rw_vertex_format(const u8* vtxfmt24, u32 flags,
                                    bool matrix_palette) noexcept;

// 44 bytes. Absent attributes read as: nrm 0, rgba 255 (GX treats a missing
// CLR0 as white), uv 0. Colours are exact 0..255 values so integer channel/TEV
// maths can use them unchanged.
struct NativeVertex {
    float pos[3];
    float nrm[3];
    u8 rgba[4];
    float uv[2][2];
};

enum class RwAttributeRole : u8 { Position, Normal, Color, TexCoord };

struct ConvertedAttribute {
    RwAttributeRole role = RwAttributeRole::Position;
    u8 set = 0;             // texture set for TexCoord, else 0
    u8 record_gx_attr = 0;  // the header byte; informational only
    u8 comp_type = 0;
    u8 frac = 0;
    u8 components = 0;
    u8 element_bytes = 0;  // bytes GX decodes at index * stride
    u8 stride = 0;
    u8 index_bytes = 0;    // 1 = GX_INDEX8, 2 = GX_INDEX16
    u32 guest_array = 0;   // pointer as stored in the header
    // Highest index any display-list tuple references, plus one; 0 when no
    // tuple references the attribute. Arrays are validated against this.
    u32 element_count = 0;
};

// Bytes of the source array the display lists reach:
// (element_count - 1) * stride + element_bytes. What a capture copies for a
// dynamic POS/NRM array, and what rw_expand_dynamic_pos_nrm requires.
std::size_t rw_attribute_source_bytes(const ConvertedAttribute& attribute) noexcept;

struct ConvertedMesh {
    u32 index_offset = 0;  // into ConvertedGeometry::indices
    u32 index_count = 0;   // multiple of 3
    u32 primitives = 0;
    u32 dl_vertices = 0;   // sum of primitive vertex counts (differs from BinMesh numIndices)
    u32 degenerate_dropped = 0;
    u32 foreign_commands = 0;
};

struct RwForeignCommand {
    u32 mesh = 0;
    u32 offset = 0;  // from the start of that mesh's display list
    u32 length = 0;
    u8 opcode = 0;
};

struct RwConvertStats {
    u32 primitives = 0;
    u32 dl_vertices = 0;
    u32 triangles = 0;
    u32 degenerate_dropped = 0;
    u32 foreign_commands = 0;
    u32 unsupported_primitives = 0;  // lines / line strips / points (must be 0 for RW)
    u32 unsupported_vertices = 0;
    // Vertices that complete no primitive: a strip or fan of 1-2 vertices,
    // the remainder of a triangle list not divisible by 3 or quads by 4.
    u32 incomplete_vertices = 0;
    u32 nop_bytes = 0;
    u32 gx_attr_mismatches = 0;  // header gxAttr byte disagrees with record order
};

struct ConvertedGeometry {
    u16 draw_sync_stamp = 0;
    u16 mesh_serial = 0;
    u32 hdr_flags = 0;

    u32 attribute_count = 0;
    std::array<ConvertedAttribute, kRwMaxAttributes> attributes{};
    bool has_normals = false;  // attributes[1] is the normal when set
    bool has_colors = false;
    u8 texcoord_sets = 0;
    bool has_matrix_index = false;
    // Every tuple repeats one index: vertices[i] is source vertex i.
    bool shared_index = false;
    // Converted with RwConvertOptions::dynamic_pos_nrm: pos/nrm are 0 here.
    bool dynamic_pos_nrm = false;

    shadow::memory::Vector<NativeVertex, shadow::memory::Domain::Assets> vertices;
    shadow::memory::Vector<u32, shadow::memory::Domain::Assets> indices;  // triangle list, GX vertex order per triangle
    shadow::memory::Vector<ConvertedMesh, shadow::memory::Domain::Assets> meshes;
    // Source POS / NRM array index per output vertex, for per-draw dynamic
    // re-expansion of CPU-skinned (or morphed) arrays. nrm_source_index is
    // empty without normals; matrix_index is empty without a palette.
    std::vector<u32> pos_source_index;
    std::vector<u32> nrm_source_index;
    std::vector<u8> matrix_index;
    std::vector<RwForeignCommand> foreign;
    RwConvertStats stats;
    // EXACTLY what the conversion read from spans[0]: every byte of every
    // display list and array it resolved lies in [0, source_extent) of that
    // span. The header is read through header_be, which callers pass as the
    // start of the same span, so the extent covers it too when they alias.
    // Two inputs equal over [0, source_extent) (and equal format/options)
    // convert to equal output: nothing else is read. `source_other_spans`
    // says a resolve landed in another span, which this extent does not cover.
    std::size_t source_extent = 0;
    bool source_other_spans = false;

    // Resets every field; vectors keep their capacity so a reused object
    // converts without allocating once it has grown.
    void clear() noexcept;
};

// A guest-address range the caller copied. Header pointers are translated to
// offsets inside whichever span contains the whole referenced extent.
struct GuestSpan {
    u32 guest_address = 0;
    const u8* bytes = nullptr;
    std::size_t size = 0;
};

struct RwConvertOptions {
    // CPU-skinned / morphed geometry: the POS and NRM records point at arrays
    // rewritten every draw (a separate output entry for skins). Do not resolve
    // or decode them; topology, colours and UVs are still built and the index
    // ranges still recorded, so rw_expand_dynamic_pos_nrm can fill them per draw.
    bool dynamic_pos_nrm = false;
};

// Converts one instance. header_be/header_size: the header at entry+24, exactly
// 12 + 8N + 8 * numMeshes bytes. data/data_size: a copy of guest memory that
// starts at data_base_guest_address and holds every display list and array the
// header points at (for a Native Data PLG blob whose pointers are relative, pass
// the data block with base 0). On failure returns false, writes a reason to
// *error (when non-null) and leaves `out` cleared.
bool rw_convert_instance(const u8* header_be, std::size_t header_size, const u8* data,
                         std::size_t data_size, u32 data_base_guest_address,
                         const GxVertexFormatDesc& format, ConvertedGeometry& out,
                         std::string* error, const RwConvertOptions& options = {});

// Same, with the pointed-at memory split over several spans (a CPU-skinned
// entry's POS/NRM live in a separate output entry). Each display list and each
// array extent must lie entirely within one span.
bool rw_convert_instance_spans(const u8* header_be, std::size_t header_size,
                               const GuestSpan* spans, std::size_t span_count,
                               const GxVertexFormatDesc& format,
                               const RwConvertOptions& options, ConvertedGeometry& out,
                               std::string* error);

// Per-draw POS/NRM re-expansion for CPU-skinned geometry. pos_be/nrm_be are the
// current big-endian arrays starting at the records' array pointers (at least
// rw_attribute_source_bytes of the POS / NRM attribute; nrm may be null when the
// geometry has no normals). Writes 6 floats per output vertex, aligned with
// geometry.vertices: px py pz nx ny nz (n = 0 without normals), decoded with the
// same rules as the static conversion. No allocation, no locks; returns false
// (writing nothing) when a span is too short or out_float_count < 6 * vertices.
bool rw_expand_dynamic_pos_nrm(const ConvertedGeometry& geometry, const u8* pos_be,
                               std::size_t pos_bytes, const u8* nrm_be,
                               std::size_t nrm_bytes, float* out_pos_nrm,
                               std::size_t out_float_count) noexcept;

// The same canonical decode written into fields of a strided final vertex
// buffer. All bounds are validated before the first byte is written.
// Optional immutable attributes join that same forward pass, so mapped upload
// storage need not be revisited after positions/normals have been written.
struct RwPackedStaticFields {
    std::size_t color0, color1, uv0, uv1;
};
bool rw_expand_dynamic_pos_nrm_strided(
    const ConvertedGeometry& geometry, const u8* pos_be, std::size_t pos_bytes,
    const u8* nrm_be, std::size_t nrm_bytes, u8* out_vertices,
    std::size_t out_bytes, std::size_t vertex_stride,
    std::size_t position_offset, std::size_t normal_offset,
    const RwPackedStaticFields* static_fields = nullptr) noexcept;

}  // namespace gekko
