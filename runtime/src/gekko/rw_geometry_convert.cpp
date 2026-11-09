#include "gekko/rw_geometry_convert.h"

#include <cstdarg>
#include <cstdio>
#include <limits>

#include "gekko/profiling.h"

namespace gekko {
namespace {

constexpr std::size_t kHeaderFixedBytes = 12;
constexpr std::size_t kRecordBytes = 8;

// The driver default RpGameCubeVtxFmt at 0x805E2748 (initialised by
// 0x80463FFC; dump bytes 040404040404040404040501000000000000000000000001).
// RwGC_SetArrays substitutes it when a geometry or world carries no format.
constexpr u8 kDefaultVtxFmt[24] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 1,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

// GX primitive type = (opcode >> 3) & 7 for opcodes 0x80-0xBF.
constexpr u8 kPrimQuads = 0;
constexpr u8 kPrimQuads2 = 1;
constexpr u8 kPrimTriangles = 2;
constexpr u8 kPrimStrip = 3;
constexpr u8 kPrimFan = 4;

// GX_VA_* ids the instancers write into the informational gxAttr byte.
constexpr u8 kGxVaPos = 9;
constexpr u8 kGxVaNrm = 10;
constexpr u8 kGxVaClr0 = 11;
constexpr u8 kGxVaTex0 = 13;
constexpr u8 kGxVaNbt = 25;

constexpr u32 kEmptySlot = 0;  // hash table stores vertex id + 1

bool fail(ConvertedGeometry& out, std::string* error, const char* format, ...) {
    out.clear();
    if (error != nullptr) {
        char message[256];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        *error = message;
        // Read back from *error rather than re-formatting: the va_list is
        // already consumed and `message` only exists on this branch. Every
        // refusal leaves an object missing until its retry budget recovers it,
        // so this puts the desync in TIME, beside the rw/convert zone it
        // aborted, instead of only in the exit report.
        GEKKO_PROFILE_MESSAGE(error->c_str(), error->size());
    }
    return false;
}

u32 comp_bytes(u8 type) noexcept {
    switch (type) {
        case kGxCompU8:
        case kGxCompS8: return 1;
        case kGxCompU16:
        case kGxCompS16: return 2;
        default: return 4;
    }
}

u32 color_bytes(u8 type) noexcept {
    static constexpr u8 kBytes[6] = {2, 3, 4, 2, 3, 4};
    return type < 6u ? kBytes[type] : 0u;
}

// The integer value converted to float and divided by 2^frac - the same
// operation as the legacy decoder (renderer.cpp read_component), so both paths
// produce bit-identical floats. F32 is a bit copy (NaN payloads included).
float read_scaled(const u8* p, u8 type, u8 frac) noexcept {
    const float scale = static_cast<float>(1u << frac);
    switch (type) {
        case kGxCompU8: return static_cast<float>(p[0]) / scale;
        case kGxCompS8: return static_cast<float>(static_cast<s8>(p[0])) / scale;
        case kGxCompU16: return static_cast<float>(load_be16(p)) / scale;
        case kGxCompS16: return static_cast<float>(static_cast<s16>(load_be16(p))) / scale;
        default: return bits_to_float(load_be32(p));
    }
}

// GX normals ignore the VAT frac: the scale is fixed by the component type
// (renderer.cpp read_normal_component). RwGC's model format is S8 -> v/64.
float read_normal(const u8* p, u8 type) noexcept {
    switch (type) {
        case kGxCompU8: return static_cast<float>(p[0]) / 128.0f;
        case kGxCompS8: return static_cast<float>(static_cast<s8>(p[0])) / 64.0f;
        case kGxCompU16: return static_cast<float>(load_be16(p)) / 32768.0f;
        case kGxCompS16: return static_cast<float>(static_cast<s16>(load_be16(p))) / 16384.0f;
        default: return bits_to_float(load_be32(p));
    }
}

u8 expand5(u32 value) noexcept { return static_cast<u8>((value << 3) | (value >> 2)); }
u8 expand6(u32 value) noexcept { return static_cast<u8>((value << 2) | (value >> 4)); }
u8 expand4(u32 value) noexcept { return static_cast<u8>((value << 4) | value); }

// Bit replication per channel, as Flipper and the legacy read_direct_colour
// expand them. RGB formats have no alpha: 255.
void read_color(const u8* p, u8 type, u8* rgba) noexcept {
    switch (type) {
        case kGxColorRGB565: {
            const u32 value = load_be16(p);
            rgba[0] = expand5((value >> 11) & 0x1fu);
            rgba[1] = expand6((value >> 5) & 0x3fu);
            rgba[2] = expand5(value & 0x1fu);
            rgba[3] = 255;
            return;
        }
        case kGxColorRGB8:
        case kGxColorRGBX8:
            rgba[0] = p[0];
            rgba[1] = p[1];
            rgba[2] = p[2];
            rgba[3] = 255;
            return;
        case kGxColorRGBA4: {
            const u32 value = load_be16(p);
            rgba[0] = expand4((value >> 12) & 0xfu);
            rgba[1] = expand4((value >> 8) & 0xfu);
            rgba[2] = expand4((value >> 4) & 0xfu);
            rgba[3] = expand4(value & 0xfu);
            return;
        }
        case kGxColorRGBA6: {
            const u32 value = (static_cast<u32>(p[0]) << 16) | (static_cast<u32>(p[1]) << 8) | p[2];
            rgba[0] = expand6((value >> 18) & 0x3fu);
            rgba[1] = expand6((value >> 12) & 0x3fu);
            rgba[2] = expand6((value >> 6) & 0x3fu);
            rgba[3] = expand6(value & 0x3fu);
            return;
        }
        default:
            rgba[0] = p[0];
            rgba[1] = p[1];
            rgba[2] = p[2];
            rgba[3] = p[3];
            return;
    }
}

// The span that holds [address, address + bytes) entirely, or nullptr.
const u8* resolve(const GuestSpan* spans, std::size_t span_count, u32 address,
                  u64 bytes) noexcept {
    for (std::size_t index = 0; index < span_count; ++index) {
        const GuestSpan& span = spans[index];
        if (span.bytes == nullptr || address < span.guest_address) continue;
        const u64 offset = static_cast<u64>(address) - span.guest_address;
        const u64 size = static_cast<u64>(span.size);
        if (offset <= size && bytes <= size - offset) {
            return span.bytes + offset;
        }
    }
    return nullptr;
}

// Exact FIFO length of a non-primitive command at dl[pos], or 0 for an opcode
// whose length is unknown (the list cannot be walked past it). `available` is
// the bytes left from pos; the XF load needs its count field to be present.
u32 foreign_command_length(u8 opcode, const u8* at, std::size_t available) noexcept {
    switch (opcode) {
        case 0x08: return 1u + 1u + 4u;  // CP register load
        case 0x10: {                     // XF register load: u16 count-1, u16 address, values
            if (available < 5u) return 5u;  // truncated; the caller's bound check rejects it
            return 1u + 2u + 2u + 4u * (static_cast<u32>(load_be16(at + 1)) + 1u);
        }
        case 0x20:  // XF indexed loads: position / normal / texture matrix, light
        case 0x28:
        case 0x30:
        case 0x38: return 1u + 4u;
        case 0x40: return 1u + 4u + 4u;  // call display list
        case 0x44:
        case 0x48: return 1u;  // invalidate vertex cache and its neighbour
        case 0x61: return 1u + 4u;  // BP register load
        default: return 0u;
    }
}

u32 hash_tuple(const u16* tuple, u32 width) noexcept {
    u32 hash = 0x811c9dc5u;
    for (u32 index = 0; index < width; ++index) {
        hash = (hash ^ tuple[index]) * 0x01000193u;
    }
    return hash ^ (hash >> 16);
}

bool is_triangle_primitive(u8 type) noexcept { return type <= kPrimFan; }

struct Primitive {
    u32 first = 0;  // vertex offset into the tuple list
    u32 count = 0;
    u8 type = 0;
};

}  // namespace

GxVertexFormatDesc rw_vertex_format(const u8* vtxfmt24, u32 flags, bool matrix_palette) noexcept {
    const u8* fmt = vtxfmt24 != nullptr ? vtxfmt24 : kDefaultVtxFmt;
    GxVertexFormatDesc desc;
    desc.pos_type = fmt[0];
    desc.nrm_type = fmt[1];
    for (std::size_t set = 0; set < 8; ++set) {
        desc.tex_type[set] = fmt[2 + set];
        desc.tex_frac[set] = fmt[14 + set];
    }
    desc.color_type = fmt[10];
    desc.pos_frac = fmt[12];
    desc.normal_nbt = fmt[13] != 0;
    desc.normals = (flags & kRwGeometryNormals) != 0;
    desc.colors = (flags & kRwGeometryPrelit) != 0;
    desc.texcoords = (flags & (kRwGeometryTextured | kRwGeometryTextured2)) != 0;
    desc.matrix_index = matrix_palette;
    return desc;
}

std::size_t rw_attribute_source_bytes(const ConvertedAttribute& attribute) noexcept {
    if (attribute.element_count == 0) return 0;
    return static_cast<std::size_t>(attribute.element_count - 1u) * attribute.stride +
           attribute.element_bytes;
}

void ConvertedGeometry::clear() noexcept {
    draw_sync_stamp = 0;
    mesh_serial = 0;
    hdr_flags = 0;
    attribute_count = 0;
    attributes = {};
    has_normals = false;
    has_colors = false;
    texcoord_sets = 0;
    has_matrix_index = false;
    shared_index = false;
    dynamic_pos_nrm = false;
    vertices.clear();
    indices.clear();
    meshes.clear();
    pos_source_index.clear();
    nrm_source_index.clear();
    matrix_index.clear();
    foreign.clear();
    stats = {};
    source_extent = 0;
    source_other_spans = false;
}

bool rw_convert_instance(const u8* header_be, std::size_t header_size, const u8* data,
                         std::size_t data_size, u32 data_base_guest_address,
                         const GxVertexFormatDesc& format, ConvertedGeometry& out,
                         std::string* error, const RwConvertOptions& options) {
    const GuestSpan span{data_base_guest_address, data, data_size};
    return rw_convert_instance_spans(header_be, header_size, &span, 1, format, options, out,
                                     error);
}

bool rw_convert_instance_spans(const u8* header_be, std::size_t header_size,
                               const GuestSpan* spans, std::size_t span_count,
                               const GxVertexFormatDesc& format,
                               const RwConvertOptions& options, ConvertedGeometry& out,
                               std::string* error) {
    // PER GEOMETRY, on the render thread. Only the spans form is zoned:
    // rw_convert_instance is a one-line wrapper around it and the renderer
    // calls this one directly, so zoning both would produce two perfectly
    // nested zones with identical timing and a nesting depth that lies.
    //
    // This is the most expensive operation in the subsystem and the only one
    // that can REFUSE, which is what makes an object disappear.
    GEKKO_PROFILE_ZONE("rw/convert");
    out.clear();
    if (header_be == nullptr || header_size < kHeaderFixedBytes) {
        return fail(out, error, "header of %zu bytes is shorter than the 12-byte fixed part",
                    header_size);
    }
    if (spans == nullptr) span_count = 0;
    // Records the consumed extent of every resolve (ConvertedGeometry::source_extent).
    const auto note_extent = [&out, spans, span_count](const u8* at, u64 bytes) {
        if (at == nullptr) return;
        for (std::size_t index = 0; index < span_count; ++index) {
            const GuestSpan& span = spans[index];
            if (span.bytes == nullptr || at < span.bytes || at >= span.bytes + span.size) continue;
            if (index != 0) {
                out.source_other_spans = true;
                return;
            }
            const std::size_t end = static_cast<std::size_t>(at - span.bytes) + static_cast<std::size_t>(bytes);
            out.source_extent = std::max(out.source_extent, end);
            return;
        }
    };
    if (span_count != 0 && spans != nullptr && spans[0].bytes == header_be) {
        out.source_extent = header_size;
    }

    // ---- header and attribute layout -------------------------------------
    const u32 attribute_count = load_be32(header_be + 8);
    if (attribute_count == 0 || attribute_count > kRwMaxAttributes) {
        return fail(out, error, "numAttributes %u is outside 1..%zu", attribute_count,
                    kRwMaxAttributes);
    }
    const std::size_t records_end = kHeaderFixedBytes + kRecordBytes * attribute_count;
    if (header_size < records_end || (header_size - records_end) % kRecordBytes != 0) {
        return fail(out, error,
                    "header size %zu is not 12 + 8*%u attribute records + 8*numMeshes",
                    header_size, attribute_count);
    }
    const std::size_t mesh_count = (header_size - records_end) / kRecordBytes;

    if (format.pos_type > kGxCompF32 || format.pos_frac > 31u ||
        (format.normals && format.nrm_type > kGxCompF32) ||
        (format.colors && format.color_type > kGxColorRGBA8)) {
        return fail(out, error, "invalid vertex format (pos %u/%u nrm %u clr %u)",
                    format.pos_type, format.pos_frac, format.nrm_type, format.color_type);
    }
    const u32 fixed_attributes = 1u + (format.normals ? 1u : 0u) + (format.colors ? 1u : 0u);
    if (attribute_count < fixed_attributes) {
        return fail(out, error, "%u attribute records but the flags require %u", attribute_count,
                    fixed_attributes);
    }
    const u32 texcoord_sets = attribute_count - fixed_attributes;
    if (texcoord_sets != 0 && !format.texcoords) {
        return fail(out, error, "%u texture records on geometry without texture flags",
                    texcoord_sets);
    }
    if (texcoord_sets > kNativeTexcoordSets) {
        return fail(out, error, "%u texture coordinate sets; NativeVertex represents %zu",
                    texcoord_sets, kNativeTexcoordSets);
    }

    out.draw_sync_stamp = load_be16(header_be);
    out.mesh_serial = load_be16(header_be + 2);
    out.hdr_flags = load_be32(header_be + 4);
    out.attribute_count = attribute_count;
    out.has_normals = format.normals;
    out.has_colors = format.colors;
    out.texcoord_sets = static_cast<u8>(texcoord_sets);
    out.has_matrix_index = format.matrix_index;
    out.dynamic_pos_nrm = options.dynamic_pos_nrm;

    s32 normal_slot = -1;
    s32 color_slot = -1;
    s32 texcoord_slot[kNativeTexcoordSets] = {-1, -1};
    u32 tuple_bytes = format.matrix_index ? 1u : 0u;
    for (u32 slot = 0; slot < attribute_count; ++slot) {
        const u8* record = header_be + kHeaderFixedBytes + kRecordBytes * slot;
        ConvertedAttribute& attribute = out.attributes[slot];
        attribute.guest_array = load_be32(record);
        attribute.record_gx_attr = record[4];
        attribute.stride = record[5];
        const u8 index_type = record[6];
        if (index_type != 2u && index_type != 3u) {
            return fail(out, error, "attribute %u index type %u is not GX_INDEX8/GX_INDEX16", slot,
                        index_type);
        }
        attribute.index_bytes = index_type == 2u ? 1u : 2u;
        tuple_bytes += attribute.index_bytes;

        u8 expected_gx_attr = kGxVaPos;
        u32 element_bytes = 0;
        if (slot == 0) {
            attribute.role = RwAttributeRole::Position;
            attribute.comp_type = format.pos_type;
            attribute.frac = format.pos_frac;
            attribute.components = 3;
            element_bytes = 3u * comp_bytes(format.pos_type);
        } else if (format.normals && slot == 1) {
            attribute.role = RwAttributeRole::Normal;
            attribute.comp_type = format.nrm_type;
            attribute.components = format.normal_nbt ? 9 : 3;
            element_bytes = attribute.components * comp_bytes(format.nrm_type);
            expected_gx_attr = format.normal_nbt ? kGxVaNbt : kGxVaNrm;
            normal_slot = static_cast<s32>(slot);
        } else if (format.colors && slot == (format.normals ? 2u : 1u)) {
            attribute.role = RwAttributeRole::Color;
            attribute.comp_type = format.color_type;
            attribute.components = format.color_type > kGxColorRGBX8 ? 4 : 3;
            element_bytes = color_bytes(format.color_type);
            expected_gx_attr = kGxVaClr0;
            color_slot = static_cast<s32>(slot);
        } else {
            const u32 set = slot - fixed_attributes;
            if (format.tex_type[set] > kGxCompF32 || format.tex_frac[set] > 31u) {
                return fail(out, error, "invalid TEX%u format %u/%u", set, format.tex_type[set],
                            format.tex_frac[set]);
            }
            attribute.role = RwAttributeRole::TexCoord;
            attribute.set = static_cast<u8>(set);
            attribute.comp_type = format.tex_type[set];
            attribute.frac = format.tex_frac[set];
            attribute.components = 2;
            element_bytes = 2u * comp_bytes(format.tex_type[set]);
            expected_gx_attr = static_cast<u8>(kGxVaTex0 + set);
            texcoord_slot[set] = static_cast<s32>(slot);
        }
        attribute.element_bytes = static_cast<u8>(element_bytes);
        if (attribute.stride == 0 || element_bytes > attribute.stride) {
            return fail(out, error, "attribute %u stride %u cannot hold a %u-byte element", slot,
                        attribute.stride, element_bytes);
        }
        if (attribute.record_gx_attr != expected_gx_attr &&
            !(attribute.role == RwAttributeRole::Normal && attribute.record_gx_attr == kGxVaNrm)) {
            ++out.stats.gx_attr_mismatches;
        }
    }
    const u32 tuple_width = attribute_count + (format.matrix_index ? 1u : 0u);

    // ---- pass 1: walk every display list, collect vertex tuples ------------
    // Tuples are stored as u16: [attribute indices..., matrix index].
    std::vector<u16> tuples;
    std::vector<Primitive> primitives;
    std::vector<u32> mesh_first_primitive(mesh_count + 1u, 0u);
    std::array<u32, kRwMaxAttributes> max_index{};
    std::array<bool, kRwMaxAttributes> referenced{};
    u64 triangle_vertices = 0;
    u64 index_upper_bound = 0;
    out.meshes.resize(mesh_count);
    // Scoped so the whole display-list walk is ONE zone with the element count
    // attached. The inner `while (pos < list_size)` steps a single GX command
    // at a time over millions of commands a route - that population must never
    // carry a zone of its own.
    {
        GEKKO_PROFILE_ZONE("rw/convert-walk");
        for (std::size_t mesh = 0; mesh < mesh_count; ++mesh) {
            mesh_first_primitive[mesh] = static_cast<u32>(primitives.size());
            const u8* record = header_be + records_end + kRecordBytes * mesh;
            const u32 list_address = load_be32(record);
            const u32 list_size = load_be32(record + 4);
            ConvertedMesh& converted = out.meshes[mesh];
            if (list_size == 0) continue;
            const u8* list = resolve(spans, span_count, list_address, list_size);
            note_extent(list, list_size);
            if (list == nullptr) {
                return fail(out, error, "mesh %zu display list 0x%08X+%u is outside the spans", mesh,
                            list_address, list_size);
            }
            std::size_t pos = 0;
            while (pos < list_size) {
                const u8 opcode = list[pos];
                if (opcode == 0x00) {  // GX NOP: the 32-byte padding
                    ++out.stats.nop_bytes;
                    ++pos;
                    continue;
                }
                if ((opcode & 0xc0u) == 0x80u) {
                    if ((opcode & 0x07u) != 0u) {
                        // RwGC programs only VTXFMT0; another format's VAT is unknown.
                        return fail(out, error, "mesh %zu +%zu primitive 0x%02X uses VTXFMT%u", mesh,
                                    pos, opcode, opcode & 7u);
                    }
                    if (list_size - pos < 3u) {
                        return fail(out, error, "mesh %zu +%zu primitive header truncated", mesh, pos);
                    }
                    const u32 count = load_be16(list + pos + 1);
                    const u64 payload = static_cast<u64>(count) * tuple_bytes;
                    if (payload > list_size - pos - 3u) {
                        return fail(out, error,
                                    "mesh %zu +%zu primitive 0x%02X of %u vertices overruns the %u-byte list",
                                    mesh, pos, opcode, count, list_size);
                    }
                    const u8 type = static_cast<u8>((opcode >> 3) & 7u);
                    Primitive primitive;
                    primitive.first = static_cast<u32>(tuples.size() / tuple_width);
                    primitive.count = count;
                    primitive.type = type;
                    const u8* cursor = list + pos + 3;
                    const std::size_t base = tuples.size();
                    tuples.resize(base + static_cast<std::size_t>(count) * tuple_width);
                    u16* tuple = tuples.data() + base;
                    for (u32 vertex = 0; vertex < count; ++vertex, tuple += tuple_width) {
                        // GX order: PNMTXIDX (direct) first, then attributes in record order.
                        if (format.matrix_index) tuple[attribute_count] = *cursor++;
                        for (u32 slot = 0; slot < attribute_count; ++slot) {
                            u32 index;
                            if (out.attributes[slot].index_bytes == 1u) {
                                index = *cursor++;
                            } else {
                                index = load_be16(cursor);
                                cursor += 2;
                            }
                            tuple[slot] = static_cast<u16>(index);
                            if (!referenced[slot] || index > max_index[slot]) max_index[slot] = index;
                            referenced[slot] = true;
                        }
                    }
                    ++converted.primitives;
                    converted.dl_vertices += count;
                    if (is_triangle_primitive(type)) {
                        triangle_vertices += count;
                        index_upper_bound += 6u * static_cast<u64>(count);
                    } else {
                        ++out.stats.unsupported_primitives;
                        out.stats.unsupported_vertices += count;
                    }
                    primitives.push_back(primitive);
                    pos += 3u + static_cast<std::size_t>(payload);
                    continue;
                }
                const u32 length = foreign_command_length(opcode, list + pos, list_size - pos);
                if (length == 0u) {
                    return fail(out, error, "mesh %zu +%zu unknown display-list opcode 0x%02X", mesh, pos,
                                opcode);
                }
                if (length > list_size - pos) {
                    return fail(out, error, "mesh %zu +%zu command 0x%02X of %u bytes is truncated", mesh,
                                pos, opcode, length);
                }
                // The native path never executes a display list: GraphicsPipe
                // skips GX_CMD_CALL_DL by its length (M-F F1) and this
                // conversion is the only reader of the list. A command that
                // WRITES state - CP (0x08), XF (0x10), indexed XF (0x20-0x38),
                // BP (0x61), or a nested list (0x40) whose contents would be
                // skipped with it - therefore has no executor, and skipping
                // it would silently change what the guest programmed. It is
                // refused with its reason, so the object is reported missing
                // (convert failures, RETRIES EXHAUSTED) instead of drawn wrong.
                // 0x44/0x48 (vertex-cache invalidation) carry no state and are
                // accepted.
                if (opcode != 0x44u && opcode != 0x48u) {
                    return fail(out, error,
                                "mesh %zu +%zu display list carries state command 0x%02X (%u bytes); "
                                "lists are not executed on the native path",
                                mesh, pos, opcode, length);
                }
                RwForeignCommand command;
                command.mesh = static_cast<u32>(mesh);
                command.offset = static_cast<u32>(pos);
                command.length = length;
                command.opcode = opcode;
                out.foreign.push_back(command);
                ++converted.foreign_commands;
                ++out.stats.foreign_commands;
                pos += length;
            }
            out.stats.primitives += converted.primitives;
            out.stats.dl_vertices += converted.dl_vertices;
        }
        GEKKO_PROFILE_VALUE(out.stats.dl_vertices);
    }
    mesh_first_primitive[mesh_count] = static_cast<u32>(primitives.size());
    if (index_upper_bound > std::numeric_limits<u32>::max()) {
        return fail(out, error, "display lists expand to more than 2^32 indices");
    }

    // ---- arrays: bound every referenced extent ------------------------------
    std::array<const u8*, kRwMaxAttributes> arrays{};
    for (u32 slot = 0; slot < attribute_count; ++slot) {
        ConvertedAttribute& attribute = out.attributes[slot];
        attribute.element_count = referenced[slot] ? max_index[slot] + 1u : 0u;
        const bool dynamic = options.dynamic_pos_nrm && (attribute.role == RwAttributeRole::Position ||
                                                         attribute.role == RwAttributeRole::Normal);
        if (dynamic || attribute.element_count == 0) continue;
        const u64 bytes = rw_attribute_source_bytes(attribute);
        arrays[slot] = resolve(spans, span_count, attribute.guest_array, bytes);
        note_extent(arrays[slot], bytes);
        if (arrays[slot] == nullptr) {
            return fail(out, error, "attribute %u array 0x%08X (%u elements x %u) is outside the spans",
                        slot, attribute.guest_array, attribute.element_count, attribute.stride);
        }
    }

    // ---- vertex identity: 1:1 when every tuple shares one index -------------
    // Only primitives that produce triangles contribute output vertices. The
    // shared form additionally needs one matrix index per source vertex (a
    // split palette skin may give one vertex different slots in two meshes).
    bool shared = true;
    u32 shared_vertex_count = 0;
    std::vector<u16> shared_matrix;
    if (format.matrix_index) {
        shared_matrix.assign(static_cast<std::size_t>(max_index[0]) + 1u, 0xffffu);
    }
    for (const Primitive& primitive : primitives) {
        if (!shared) break;
        if (!is_triangle_primitive(primitive.type)) continue;
        const u16* tuple = tuples.data() + static_cast<std::size_t>(primitive.first) * tuple_width;
        for (u32 vertex = 0; vertex < primitive.count && shared; ++vertex, tuple += tuple_width) {
            const u16 index = tuple[0];
            for (u32 slot = 1; slot < attribute_count; ++slot) {
                if (tuple[slot] != index) {
                    shared = false;
                    break;
                }
            }
            if (!shared) break;
            if (format.matrix_index) {
                u16& matrix = shared_matrix[index];
                if (matrix != 0xffffu && matrix != tuple[attribute_count]) {
                    shared = false;
                    break;
                }
                matrix = tuple[attribute_count];
            }
            if (static_cast<u32>(index) + 1u > shared_vertex_count) shared_vertex_count = index + 1u;
        }
    }
    out.shared_index = shared;

    // ---- pass 2: ids and triangles ------------------------------------------
    std::vector<u32> table;
    std::vector<u16> unique;  // hashed form: tuple of each output vertex
    u32 table_mask = 0;
    if (!shared) {
        std::size_t capacity = 16;
        while (capacity < triangle_vertices * 2u) capacity <<= 1u;
        table.assign(capacity, kEmptySlot);
        table_mask = static_cast<u32>(capacity - 1u);
        unique.reserve(static_cast<std::size_t>(triangle_vertices) * tuple_width);
    }
    u32 vertex_count = shared ? shared_vertex_count : 0u;
    out.indices.reserve(static_cast<std::size_t>(index_upper_bound));
    std::vector<u32> ids;

    const auto emit = [&](ConvertedMesh& mesh, u32 a, u32 b, u32 c) {
        if (a == b || b == c || a == c) {
            ++mesh.degenerate_dropped;
            return;
        }
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    };

    // Scoped on its own because this is the part of conversion that is
    // superlinear in the bad case: on the non-shared path it runs open
    // addressing with linear probing and a full tuple compare per probe,
    // for every triangle vertex. Averaged into rw/convert that is invisible.
    // The per-vertex probe loop inside gets nothing.
    {
        GEKKO_PROFILE_ZONE("rw/convert-index");
        for (std::size_t mesh = 0; mesh < mesh_count; ++mesh) {
            ConvertedMesh& converted = out.meshes[mesh];
            converted.index_offset = static_cast<u32>(out.indices.size());
            for (u32 p = mesh_first_primitive[mesh]; p < mesh_first_primitive[mesh + 1u]; ++p) {
                const Primitive& primitive = primitives[p];
                if (!is_triangle_primitive(primitive.type)) continue;
                const u32 count = primitive.count;
                ids.resize(count);
                const u16* tuple = tuples.data() + static_cast<std::size_t>(primitive.first) * tuple_width;
                for (u32 vertex = 0; vertex < count; ++vertex, tuple += tuple_width) {
                    if (shared) {
                        ids[vertex] = tuple[0];
                        continue;
                    }
                    u32 slot_index = hash_tuple(tuple, tuple_width) & table_mask;
                    for (;;) {
                        const u32 entry = table[slot_index];
                        if (entry == kEmptySlot) {
                            table[slot_index] = vertex_count + 1u;
                            unique.insert(unique.end(), tuple, tuple + tuple_width);
                            ids[vertex] = vertex_count++;
                            break;
                        }
                        const u16* existing = unique.data() + static_cast<std::size_t>(entry - 1u) * tuple_width;
                        bool equal = true;
                        for (u32 k = 0; k < tuple_width; ++k) {
                            if (existing[k] != tuple[k]) {
                                equal = false;
                                break;
                            }
                        }
                        if (equal) {
                            ids[vertex] = entry - 1u;
                            break;
                        }
                        slot_index = (slot_index + 1u) & table_mask;
                    }
                }

                switch (primitive.type) {
                    case kPrimQuads:
                    case kPrimQuads2:
                        for (u32 q = 0; q + 3u < count; q += 4u) {
                            emit(converted, ids[q], ids[q + 1u], ids[q + 2u]);
                            emit(converted, ids[q], ids[q + 2u], ids[q + 3u]);
                        }
                        out.stats.incomplete_vertices += count % 4u;
                        break;
                    case kPrimTriangles:
                        for (u32 t = 0; t + 2u < count; t += 3u) {
                            emit(converted, ids[t], ids[t + 1u], ids[t + 2u]);
                        }
                        out.stats.incomplete_vertices += count % 3u;
                        break;
                    case kPrimStrip:
                        // Winding alternates per triangle, parity restarts at each strip.
                        for (u32 i = 2; i < count; ++i) {
                            if ((i & 1u) == 0u) {
                                emit(converted, ids[i - 2u], ids[i - 1u], ids[i]);
                            } else {
                                emit(converted, ids[i - 1u], ids[i - 2u], ids[i]);
                            }
                        }
                        if (count < 3u) out.stats.incomplete_vertices += count;
                        break;
                    default:  // kPrimFan
                        for (u32 i = 2; i < count; ++i) {
                            emit(converted, ids[0], ids[i - 1u], ids[i]);
                        }
                        if (count < 3u) out.stats.incomplete_vertices += count;
                        break;
                }
            }
            converted.index_count = static_cast<u32>(out.indices.size()) - converted.index_offset;
            out.stats.degenerate_dropped += converted.degenerate_dropped;
        }
        GEKKO_PROFILE_VALUE(out.indices.size() / 3u);
    }
    out.stats.triangles = static_cast<u32>(out.indices.size() / 3u);

    // ---- decode vertices ------------------------------------------------------
    // The scope starts at the four resize() calls, not at the loop: they are
    // the conversion's OUTPUT ALLOCATION and the only allocation in this
    // converter that is in scope to measure. What follows is the
    // cache-hostile strided gather across up to five separate guest arrays.
    {
        GEKKO_PROFILE_ZONE("rw/convert-vertices");
        GEKKO_PROFILE_VALUE(vertex_count);
        out.vertices.resize(vertex_count);
        out.pos_source_index.resize(vertex_count);
        if (format.normals) out.nrm_source_index.resize(vertex_count);
        if (format.matrix_index) out.matrix_index.resize(vertex_count);
        u16 shared_tuple[kRwMaxAttributes + 1];
        for (u32 vertex = 0; vertex < vertex_count; ++vertex) {
            const u16* tuple;
            if (shared) {
                for (u32 slot = 0; slot < attribute_count; ++slot) {
                    shared_tuple[slot] = static_cast<u16>(vertex);
                }
                if (format.matrix_index) {
                    const u16 matrix = shared_matrix[vertex];
                    shared_tuple[attribute_count] = matrix == 0xffffu ? 0u : matrix;
                }
                tuple = shared_tuple;
            } else {
                tuple = unique.data() + static_cast<std::size_t>(vertex) * tuple_width;
            }
            NativeVertex& native = out.vertices[vertex];
            native = NativeVertex{};
            native.rgba[0] = native.rgba[1] = native.rgba[2] = native.rgba[3] = 255;
            out.pos_source_index[vertex] = tuple[0];
            if (arrays[0] != nullptr) {
                const ConvertedAttribute& attribute = out.attributes[0];
                const u8* element = arrays[0] + static_cast<std::size_t>(tuple[0]) * attribute.stride;
                const u32 step = comp_bytes(attribute.comp_type);
                for (u32 axis = 0; axis < 3; ++axis) {
                    native.pos[axis] = read_scaled(element + axis * step, attribute.comp_type, attribute.frac);
                }
            }
            if (normal_slot >= 0) {
                const ConvertedAttribute& attribute = out.attributes[normal_slot];
                out.nrm_source_index[vertex] = tuple[normal_slot];
                if (arrays[normal_slot] != nullptr) {
                    const u8* element = arrays[normal_slot] +
                                        static_cast<std::size_t>(tuple[normal_slot]) * attribute.stride;
                    const u32 step = comp_bytes(attribute.comp_type);
                    for (u32 axis = 0; axis < 3; ++axis) {
                        native.nrm[axis] = read_normal(element + axis * step, attribute.comp_type);
                    }
                }
            }
            if (color_slot >= 0) {
                const ConvertedAttribute& attribute = out.attributes[color_slot];
                read_color(arrays[color_slot] + static_cast<std::size_t>(tuple[color_slot]) * attribute.stride,
                           attribute.comp_type, native.rgba);
            }
            for (std::size_t set = 0; set < kNativeTexcoordSets; ++set) {
                const s32 slot = texcoord_slot[set];
                if (slot < 0) continue;
                const ConvertedAttribute& attribute = out.attributes[slot];
                const u8* element = arrays[slot] + static_cast<std::size_t>(tuple[slot]) * attribute.stride;
                const u32 step = comp_bytes(attribute.comp_type);
                native.uv[set][0] = read_scaled(element, attribute.comp_type, attribute.frac);
                native.uv[set][1] = read_scaled(element + step, attribute.comp_type, attribute.frac);
            }
            if (format.matrix_index) out.matrix_index[vertex] = static_cast<u8>(tuple[attribute_count]);
        }
    }
    GEKKO_PROFILE_VALUE(out.vertices.size());
    // PER-CONVERSION samples, which is why neither plot is a staircase: between
    // conversions there is no value, and drawing one would assert something
    // that was never measured.
    //
    // foreign_commands is a tripwire - it is zero everywhere today, and the
    // frame a stage or material starts programming CP/XF/BP state inside its
    // display lists this line leaves the floor. The byte plot is the host-heap
    // product of the conversion and the denominator for geometry residency on
    // the renderer's side.
    GEKKO_PROFILE_PLOT("rw/convert-foreign-commands",
                       static_cast<double>(out.stats.foreign_commands));
    GEKKO_PROFILE_PLOT("rw/convert-output-bytes",
                       static_cast<double>(out.vertices.size() * sizeof(NativeVertex) +
                                           out.indices.size() * sizeof(u32)));
    return true;
}

bool rw_expand_dynamic_pos_nrm(const ConvertedGeometry& geometry, const u8* pos_be,
                               std::size_t pos_bytes, const u8* nrm_be,
                               std::size_t nrm_bytes, float* out_pos_nrm,
                               std::size_t out_float_count) noexcept {
    if (out_float_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) return false;
    return rw_expand_dynamic_pos_nrm_strided(
        geometry, pos_be, pos_bytes, nrm_be, nrm_bytes,
        reinterpret_cast<u8*>(out_pos_nrm), out_float_count * sizeof(float),
        6u * sizeof(float), 0u, 3u * sizeof(float));
}

bool rw_expand_dynamic_pos_nrm_strided(
    const ConvertedGeometry& geometry, const u8* pos_be, std::size_t pos_bytes,
    const u8* nrm_be, std::size_t nrm_bytes, u8* out_vertices,
    std::size_t out_bytes, std::size_t vertex_stride,
    std::size_t position_offset, std::size_t normal_offset,
    const RwPackedStaticFields* static_fields) noexcept {
    // Once per GeometryDynamic record, i.e. per skinned object per frame. Paired
    // with rw/geometry-dynamic on the capture side these two bracket the whole
    // cost of skinning support across both threads.
    GEKKO_PROFILE_ZONE("rw/convert-expand");
    const std::size_t vertex_count = geometry.vertices.size();
    GEKKO_PROFILE_VALUE(vertex_count);
    if (vertex_count == 0) return true;
    if (geometry.attribute_count == 0 || geometry.pos_source_index.size() != vertex_count ||
        out_vertices == nullptr || vertex_stride == 0u) {
        return false;
    }
    if (vertex_count - 1u > std::numeric_limits<std::size_t>::max() / vertex_stride) {
        return false;
    }
    const std::size_t last_base = (vertex_count - 1u) * vertex_stride;
    if (last_base > out_bytes || position_offset > out_bytes - last_base ||
        sizeof(float) * 3u > out_bytes - last_base - position_offset ||
        normal_offset > out_bytes - last_base ||
        sizeof(float) * 3u > out_bytes - last_base - normal_offset) {
        return false;
    }
    if (static_fields != nullptr) {
        const auto fits = [&](std::size_t offset, std::size_t bytes) {
            return offset <= out_bytes - last_base && bytes <= out_bytes - last_base - offset;
        };
        if (!fits(static_fields->color0, 4u) || !fits(static_fields->color1, 4u) ||
            !fits(static_fields->uv0, 2u * sizeof(float)) ||
            !fits(static_fields->uv1, 2u * sizeof(float))) return false;
    }
    // element_count bounds every index the display lists hold, so one size
    // check per array replaces a check per vertex.
    const ConvertedAttribute& position = geometry.attributes[0];
    if (pos_be == nullptr || pos_bytes < rw_attribute_source_bytes(position)) return false;
    const ConvertedAttribute* normal = nullptr;
    if (geometry.has_normals) {
        normal = &geometry.attributes[1];
        if (geometry.nrm_source_index.size() != vertex_count || nrm_be == nullptr ||
            nrm_bytes < rw_attribute_source_bytes(*normal)) {
            return false;
        }
    }
    const u32 pos_step = comp_bytes(position.comp_type);
    const u32 nrm_step = normal != nullptr ? comp_bytes(normal->comp_type) : 0u;
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        float decoded_position[3];
        float decoded_normal[3];
        const u8* element = pos_be + static_cast<std::size_t>(geometry.pos_source_index[vertex]) * position.stride;
        decoded_position[0] = read_scaled(element, position.comp_type, position.frac);
        decoded_position[1] = read_scaled(element + pos_step, position.comp_type, position.frac);
        decoded_position[2] = read_scaled(element + 2u * pos_step, position.comp_type, position.frac);
        if (normal != nullptr) {
            const u8* n = nrm_be + static_cast<std::size_t>(geometry.nrm_source_index[vertex]) * normal->stride;
            decoded_normal[0] = read_normal(n, normal->comp_type);
            decoded_normal[1] = read_normal(n + nrm_step, normal->comp_type);
            decoded_normal[2] = read_normal(n + 2u * nrm_step, normal->comp_type);
        } else {
            decoded_normal[0] = decoded_normal[1] = decoded_normal[2] = 0.0f;
        }
        u8* output = out_vertices + vertex * vertex_stride;
        std::memcpy(output + position_offset, decoded_position, sizeof(decoded_position));
        std::memcpy(output + normal_offset, decoded_normal, sizeof(decoded_normal));
        if (static_fields != nullptr) {
            const NativeVertex& immutable = geometry.vertices[vertex];
            std::memcpy(output + static_fields->color0, immutable.rgba, 4u);
            std::memset(output + static_fields->color1, 255, 4u);
            std::memcpy(output + static_fields->uv0, immutable.uv[0], 2u * sizeof(float));
            std::memcpy(output + static_fields->uv1, immutable.uv[1], 2u * sizeof(float));
        }
    }
    return true;
}

}  // namespace gekko
