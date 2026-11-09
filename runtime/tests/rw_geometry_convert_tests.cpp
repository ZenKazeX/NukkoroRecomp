#include "gekko/rw_geometry_convert.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Allocation counter: rw_expand_dynamic_pos_nrm promises no allocation, and
// the only honest proof is to count every global operator new around the call.
// AddressSanitizer builds bring their own allocator hooks, so they skip it.
#if !defined(__SANITIZE_ADDRESS__)
static std::atomic<std::uint64_t> g_allocations{0};
void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* block = std::malloc(size != 0 ? size : 1)) return block;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* block = std::malloc(size != 0 ? size : 1)) return block;
    throw std::bad_alloc();
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }
static constexpr bool kCountAllocations = true;
#else
static std::atomic<std::uint64_t> g_allocations{0};
static constexpr bool kCountAllocations = false;
#endif

namespace {

using gekko::ConvertedGeometry;
using gekko::GuestSpan;
using gekko::GxVertexFormatDesc;
using gekko::NativeVertex;
using gekko::RwConvertOptions;
using gekko::s16;
using gekko::s32;
using gekko::s8;
using gekko::u16;
using gekko::u32;
using gekko::u64;
using gekko::u8;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

u32 fbits(float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void put8(std::vector<u8>& out, u32 value) { out.push_back(static_cast<u8>(value)); }
void put16(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value >> 8));
    out.push_back(static_cast<u8>(value));
}
void put32(std::vector<u8>& out, u32 value) {
    put16(out, value >> 16);
    put16(out, value & 0xffffu);
}
void putf(std::vector<u8>& out, float value) { put32(out, fbits(value)); }

// ---------------------------------------------------------------------------
// Synthetic RwGC instance builder: header records + data block at a guest base.
// ---------------------------------------------------------------------------
struct Synth {
    u32 base = 0x80200000u;
    std::vector<u8> data;
    struct Record {
        u32 offset;
        u8 gx_attr;
        u8 stride;
        u8 index_type;
    };
    std::vector<Record> records;
    std::vector<std::pair<u32, u32>> lists;  // offset, size
    u32 attribute_count_override = 0;

    u32 append(const std::vector<u8>& bytes) {
        // Non-zero filler between blocks: nothing may read outside an extent.
        while (data.size() % 32u) data.push_back(0xee);
        const u32 offset = static_cast<u32>(data.size());
        data.insert(data.end(), bytes.begin(), bytes.end());
        return offset;
    }
    void attribute(u8 gx_attr, u8 stride, u8 index_type, const std::vector<u8>& array) {
        records.push_back({append(array), gx_attr, stride, index_type});
    }
    void list(std::vector<u8> dl) {
        while (dl.size() % 32u) dl.push_back(0x00);
        const u32 size = static_cast<u32>(dl.size());
        lists.push_back({append(dl), size});
    }
    std::vector<u8> header() const {
        std::vector<u8> h;
        put16(h, 0x1234);
        put16(h, 0x0042);
        put32(h, 1);
        put32(h, attribute_count_override != 0 ? attribute_count_override
                                                : static_cast<u32>(records.size()));
        for (const Record& r : records) {
            put32(h, base + r.offset);
            put8(h, r.gx_attr);
            put8(h, r.stride);
            put8(h, r.index_type);
            put8(h, 0);
        }
        for (const auto& l : lists) {
            put32(h, base + l.first);
            put32(h, l.second);
        }
        return h;
    }
};

bool convert(const Synth& s, const GxVertexFormatDesc& format, ConvertedGeometry& out,
             std::string* error, const RwConvertOptions& options = {}) {
    const std::vector<u8> header = s.header();
    return gekko::rw_convert_instance(header.data(), header.size(), s.data.data(), s.data.size(),
                                      s.base, format, out, error, options);
}

std::vector<u8> positions_f32(u32 count) {
    std::vector<u8> bytes;
    for (u32 i = 0; i < count; ++i) {
        putf(bytes, static_cast<float>(i) + 0.25f);
        putf(bytes, static_cast<float>(i) * -2.0f);
        putf(bytes, 1000.0f + static_cast<float>(i));
    }
    return bytes;
}

GxVertexFormatDesc pos_only() { return gekko::rw_vertex_format(nullptr, 0, false); }

std::vector<u32> triangle_sequence(const ConvertedGeometry& g) {
    return std::vector<u32>(g.indices.begin(), g.indices.end());
}

std::string join(const std::vector<u32>& values) {
    std::string text;
    for (u32 v : values) text += std::to_string(v) + ",";
    return text;
}

// ---------------------------------------------------------------------------
// Synthetic cases
// ---------------------------------------------------------------------------

// Expected triangle vertex POSITIONS (0-based positions in the primitive) per GX
// primitive type for 8 vertices, written out by hand from the GX rules.
std::vector<u32> expected_local(u8 opcode) {
    switch (opcode) {
        case 0x80:
        case 0x88: return {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        case 0x90: return {0, 1, 2, 3, 4, 5};
        case 0x98: return {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5, 4, 5, 6, 6, 5, 7};
        case 0xa0: return {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 0, 5, 6, 0, 6, 7};
        default: return {};
    }
}

void test_every_primitive_and_index_width() {
    constexpr std::array<u8, 8> kOpcodes = {0x80, 0x88, 0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8};
    constexpr std::array<u32, 8> kPermutation = {7, 3, 5, 1, 0, 2, 6, 4};
    for (u8 index_type : {u8{2}, u8{3}}) {
        for (u8 opcode : kOpcodes) {
            Synth s;
            s.attribute(9, 12, index_type, positions_f32(8));
            std::vector<u8> dl;
            put8(dl, opcode);
            put16(dl, 8);
            for (u32 p : kPermutation) {
                if (index_type == 2) put8(dl, p); else put16(dl, p);
            }
            s.list(dl);
            ConvertedGeometry g;
            std::string error;
            require(convert(s, pos_only(), g, &error), "convert failed: " + error);
            std::vector<u32> expected;
            for (u32 local : expected_local(opcode)) expected.push_back(kPermutation[local]);
            char label[64];
            std::snprintf(label, sizeof(label), "opcode 0x%02X idx%s", opcode,
                          index_type == 2 ? "8" : "16");
            require(triangle_sequence(g) == expected,
                    std::string(label) + " triangle order " + join(triangle_sequence(g)) +
                        " expected " + join(expected));
            require(g.shared_index, std::string(label) + " single attribute must be shared");
            const bool triangles = opcode <= 0xa0;
            require(g.stats.unsupported_primitives == (triangles ? 0u : 1u) &&
                        g.stats.unsupported_vertices == (triangles ? 0u : 8u),
                    std::string(label) + " unsupported primitive counters");
            require(g.stats.incomplete_vertices == (opcode == 0x90 ? 2u : 0u),
                    std::string(label) + " incomplete vertices");
            require(g.vertices.size() == (triangles ? 8u : 0u), std::string(label) + " vertex count");
            require(g.meshes.size() == 1 && g.meshes[0].primitives == 1 &&
                        g.meshes[0].dl_vertices == 8 && g.meshes[0].index_offset == 0 &&
                        g.meshes[0].index_count == expected.size(),
                    std::string(label) + " mesh record");
            for (u32 v = 0; v < g.vertices.size(); ++v) {
                require(g.pos_source_index[v] == v, std::string(label) + " 1:1 mapping");
                require(g.vertices[v].pos[0] == static_cast<float>(v) + 0.25f &&
                            g.vertices[v].pos[1] == static_cast<float>(v) * -2.0f &&
                            g.vertices[v].pos[2] == 1000.0f + static_cast<float>(v),
                        std::string(label) + " position decode");
            }
        }
    }
}

// The persistent geometry store proves two instances equal by comparing
// [0, source_extent) of their blobs, so the extent must cover every byte the
// conversion reads and nothing it does not. Built the way the renderer calls
// it: header at blob byte 0, pointers relative to byte 0, one span over it all.
void test_consumed_extent_is_exact() {
    Synth layout;
    layout.base = 0;
    layout.attribute(9, 12, 2, positions_f32(8));  // only elements 0..3 are referenced
    std::vector<u8> dl;
    put8(dl, 0x98); put16(dl, 4);
    for (u32 i = 0; i < 4; ++i) put8(dl, i);
    layout.list(dl);
    // Rebase the data block behind the header so both live in one blob.
    const std::size_t header_size = layout.header().size();
    Synth s = layout;
    s.base = static_cast<u32>((header_size + 31u) & ~std::size_t{31u});
    std::vector<u8> blob = s.header();
    blob.resize(s.base, 0xee);
    blob.insert(blob.end(), s.data.begin(), s.data.end());
    const std::size_t list_end = s.base + s.lists[0].first + s.lists[0].second;
    const std::size_t array_end = s.base + s.records[0].offset + 3u * 12u + 12u;
    blob.resize(blob.size() + 64u, 0xee);  // bytes nothing references
    ConvertedGeometry g;
    std::string error;
    require(gekko::rw_convert_instance(blob.data(), header_size, blob.data(), blob.size(), 0u,
                                       pos_only(), g, &error),
            "convert: " + error);
    require(!g.source_other_spans, "one span");
    require(g.source_extent == std::max(list_end, array_end),
            "extent " + std::to_string(g.source_extent) + " expected " +
                std::to_string(std::max(list_end, array_end)));
    require(g.source_extent < blob.size(), "trailing bytes are not consumed");
    // A byte past the extent cannot change the conversion.
    std::vector<u8> changed = blob;
    changed.back() ^= 0xffu;
    changed[g.source_extent] ^= 0xffu;
    ConvertedGeometry h;
    require(gekko::rw_convert_instance(changed.data(), header_size, changed.data(), changed.size(), 0u,
                                       pos_only(), h, &error),
            "convert changed: " + error);
    require(h.source_extent == g.source_extent && triangle_sequence(h) == triangle_sequence(g) &&
                h.vertices.size() == g.vertices.size() &&
                std::memcmp(h.vertices.data(), g.vertices.data(),
                            g.vertices.size() * sizeof(g.vertices[0])) == 0,
            "bytes past the extent changed the output");
    // The per-draw token and allocation serial are not conversion inputs.
    changed = blob;
    changed[0] ^= 0x5au; changed[1] ^= 0xa5u; changed[2] ^= 0x33u; changed[3] ^= 0xccu;
    require(gekko::rw_convert_instance(changed.data(), header_size, changed.data(), changed.size(), 0u,
                                       pos_only(), h, &error),
            "convert stamped: " + error);
    require(triangle_sequence(h) == triangle_sequence(g) &&
                std::memcmp(h.vertices.data(), g.vertices.data(),
                            g.vertices.size() * sizeof(g.vertices[0])) == 0,
            "token/serial changed the output");
    // Failure clears it.
    std::vector<u8> truncated(blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(list_end - 1u));
    require(!gekko::rw_convert_instance(truncated.data(), header_size, truncated.data(), truncated.size(),
                                        0u, pos_only(), h, &error) && h.source_extent == 0u,
            "failed conversion must report no extent");
}

void test_short_strips_fans_and_zero_counts() {
    Synth s;
    s.attribute(9, 12, 2, positions_f32(4));
    std::vector<u8> dl;
    for (u8 opcode : {u8{0x98}, u8{0xa0}}) {
        put8(dl, opcode); put16(dl, 2); put8(dl, 0); put8(dl, 1);   // 2 vertices: nothing
        put8(dl, opcode); put16(dl, 1); put8(dl, 2);                // 1 vertex: nothing
        put8(dl, opcode); put16(dl, 0);                             // empty primitive
    }
    put8(dl, 0x80); put16(dl, 6);
    for (u32 i = 0; i < 6; ++i) put8(dl, i % 4);  // one quad + 2 leftover
    s.list(dl);
    ConvertedGeometry g;
    std::string error;
    require(convert(s, pos_only(), g, &error), error);
    require(g.stats.incomplete_vertices == 3u + 3u + 2u, "incomplete count " + std::to_string(g.stats.incomplete_vertices));
    require(triangle_sequence(g) == std::vector<u32>({0, 1, 2, 0, 2, 3}), "quad remainder triangles");
    require(g.stats.primitives == 7u, "primitive count");
}

void test_degenerate_joins_dropped_geometric_kept() {
    Synth s;
    std::vector<u8> pos = positions_f32(6);
    // Source vertex 4 is a geometric copy of vertex 3: index-distinct, kept.
    std::copy(pos.begin() + 36, pos.begin() + 48, pos.begin() + 48);
    s.attribute(9, 12, 2, pos);
    std::vector<u8> dl;
    put8(dl, 0x98); put16(dl, 8);
    for (u32 i : {0u, 1u, 2u, 2u, 3u, 3u, 4u, 5u}) put8(dl, i);
    put8(dl, 0x90); put16(dl, 3);
    for (u32 i : {2u, 3u, 4u}) put8(dl, i);  // collinear/coincident positions, distinct ids
    s.list(dl);
    ConvertedGeometry g;
    std::string error;
    require(convert(s, pos_only(), g, &error), error);
    require(triangle_sequence(g) == std::vector<u32>({0, 1, 2, 4, 3, 5, 2, 3, 4}),
            "strip join result " + join(triangle_sequence(g)));
    // ids 0 1 2 2 3 3 4 5: i=3 (2,1,2) i=4 (2,2,3) i=5 (3,2,3) i=6 (3,3,4) dropped.
    require(g.stats.degenerate_dropped == 4u && g.meshes[0].degenerate_dropped == 4u,
            "degenerate count " + std::to_string(g.stats.degenerate_dropped));
    require(g.stats.triangles == 3u, "triangles");
}

void test_unwelding_independent_indices() {
    Synth s;
    s.attribute(9, 12, 3, positions_f32(300));  // POS idx16
    std::vector<u8> colors;
    for (u32 i = 0; i < 4; ++i) { put8(colors, 10 * i); put8(colors, 20 * i); put8(colors, 30 * i); put8(colors, 255 - i); }
    s.attribute(11, 4, 2, colors);  // CLR0 RGBA8 idx8
    std::vector<u8> uvs;
    for (u32 i = 0; i < 3; ++i) { putf(uvs, 0.5f * static_cast<float>(i)); putf(uvs, -0.125f * static_cast<float>(i)); }
    s.attribute(13, 8, 2, uvs);  // TEX0 F32 idx8
    // tuples (pos, clr, tex): the first and fourth are identical -> one id.
    const std::vector<std::array<u32, 3>> tuples = {
        {299, 0, 0}, {299, 1, 0}, {5, 1, 2}, {299, 0, 0}, {7, 3, 1}, {5, 1, 2}};
    std::vector<u8> dl;
    put8(dl, 0x98); put16(dl, static_cast<u32>(tuples.size()));
    for (const auto& t : tuples) { put16(dl, t[0]); put8(dl, t[1]); put8(dl, t[2]); }
    s.list(dl);
    const GxVertexFormatDesc format = gekko::rw_vertex_format(nullptr, 0x0c, false);
    ConvertedGeometry g;
    std::string error;
    require(convert(s, format, g, &error), error);
    require(!g.shared_index, "independent indices must unweld");
    require(g.vertices.size() == 4u, "unique tuples " + std::to_string(g.vertices.size()));
    // ids by first occurrence: 0,1,2,0,3,2. Strip: (0,1,2) (2,1,0) (2,0,3) (3,0,2)
    require(triangle_sequence(g) == std::vector<u32>({0, 1, 2, 2, 1, 0, 2, 0, 3, 3, 0, 2}),
            "unwelded triangles " + join(triangle_sequence(g)));
    require(g.pos_source_index == std::vector<u32>({299, 299, 5, 7}), "pos source index");
    const auto& v1 = g.vertices[1];
    require(v1.pos[0] == 299.25f && v1.rgba[0] == 10 && v1.rgba[1] == 20 && v1.rgba[2] == 30 &&
                v1.rgba[3] == 254 && v1.uv[0][0] == 0.0f,
            "unwelded vertex 1 decode");
    const auto& v3 = g.vertices[3];
    require(v3.pos[0] == 7.25f && v3.rgba[3] == 252 && v3.uv[0][0] == 0.5f && v3.uv[0][1] == -0.125f,
            "unwelded vertex 3 decode");
    require(g.attributes[0].element_count == 300u && g.attributes[1].element_count == 4u &&
                g.attributes[2].element_count == 3u,
            "element counts");
    require(g.stats.gx_attr_mismatches == 0u, "gx attr bytes match record order");
}

void test_shared_model_keeps_one_to_one() {
    // Model layout from 0x8046D124: POS F32, NRM S8, CLR0 RGBA4, TEX0 F32, one index.
    static constexpr u8 kModelFmt[24] = {4, 1, 4, 4, 4, 4, 4, 4, 4, 4, 3, 1,
                                         6, 0, 6, 0, 0, 0, 0, 0, 0, 0, 2, 8};
    const GxVertexFormatDesc format = gekko::rw_vertex_format(kModelFmt, 0x3f, false);
    Synth s;
    s.attribute(9, 12, 2, positions_f32(5));
    std::vector<u8> normals;
    for (s32 i = 0; i < 5; ++i) { put8(normals, static_cast<u32>(64 - i)); put8(normals, static_cast<u32>(-64 + i)); put8(normals, static_cast<u32>(i)); }
    s.attribute(10, 3, 2, normals);
    std::vector<u8> colors;
    for (u32 i = 0; i < 5; ++i) put16(colors, 0x1234u + i);
    s.attribute(11, 2, 2, colors);
    std::vector<u8> uvs;
    for (u32 i = 0; i < 5; ++i) { putf(uvs, static_cast<float>(i)); putf(uvs, 1.0f); }
    s.attribute(13, 8, 2, uvs);
    std::vector<u8> dl;
    put8(dl, 0x98); put16(dl, 4);
    for (u32 i : {3u, 1u, 2u, 0u}) { put8(dl, i); put8(dl, i); put8(dl, i); put8(dl, i); }
    s.list(dl);
    ConvertedGeometry g;
    std::string error;
    require(convert(s, format, g, &error), error);
    require(g.shared_index, "shared-index model must keep 1:1");
    require(g.vertices.size() == 4u, "vertex count = max index + 1");
    require(triangle_sequence(g) == std::vector<u32>({3, 1, 2, 2, 1, 0}), "shared triangles");
    for (u32 v = 0; v < 4; ++v) {
        const NativeVertex& n = g.vertices[v];
        require(g.pos_source_index[v] == v && g.nrm_source_index[v] == v, "identity source index");
        require(n.nrm[0] == static_cast<float>(64 - static_cast<s32>(v)) / 64.0f &&
                    n.nrm[1] == static_cast<float>(-64 + static_cast<s32>(v)) / 64.0f,
                "S8 normal /64");
        const u32 c = 0x1234u + v;
        require(n.rgba[0] == ((c >> 12) & 15) * 17 && n.rgba[1] == ((c >> 8) & 15) * 17 &&
                    n.rgba[2] == ((c >> 4) & 15) * 17 && n.rgba[3] == (c & 15) * 17,
                "RGBA4 nibble*17");
        require(n.uv[0][0] == static_cast<float>(v) && n.uv[1][0] == 0.0f, "uv decode");
    }
    require(g.has_normals && g.has_colors && g.texcoord_sets == 1, "presence flags");
}

void test_component_decoding() {
    // POS / TEX integer types with fractions, F32 with a NaN payload.
    struct PosCase { u8 type; u8 frac; std::vector<u8> element; std::array<float, 3> expected; };
    const std::vector<PosCase> cases = {
        {gekko::kGxCompU8, 1, {0xff, 0x01, 0x80}, {127.5f, 0.5f, 64.0f}},
        {gekko::kGxCompS8, 3, {0xff, 0x80, 0x7f}, {-0.125f, -16.0f, 15.875f}},
        {gekko::kGxCompU16, 12, {0xff, 0xff, 0x10, 0x00, 0x00, 0x01}, {65535.0f / 4096.0f, 1.0f, 1.0f / 4096.0f}},
        {gekko::kGxCompS16, 8, {0x80, 0x00, 0x7f, 0xff, 0xff, 0xfe}, {-128.0f, 32767.0f / 256.0f, -2.0f / 256.0f}},
    };
    for (const PosCase& c : cases) {
        Synth s;
        s.attribute(9, static_cast<u8>(c.element.size()), 2, c.element);
        std::vector<u8> dl = {0x90, 0x00, 0x03, 0x00, 0x00, 0x00};
        s.list(dl);
        GxVertexFormatDesc format = pos_only();
        format.pos_type = c.type;
        format.pos_frac = c.frac;
        ConvertedGeometry g;
        std::string error;
        require(convert(s, format, g, &error), error);
        require(g.indices.empty() && g.stats.degenerate_dropped == 1u, "same index triangle dropped");
        require(g.vertices.size() == 1u, "one vertex");
        for (u32 a = 0; a < 3; ++a) {
            require(fbits(g.vertices[0].pos[a]) == fbits(c.expected[a]),
                    "pos type " + std::to_string(c.type) + " axis " + std::to_string(a));
        }
    }
    {
        Synth s;
        std::vector<u8> element;
        put32(element, 0x7fc00001u);  // quiet NaN with payload
        put32(element, 0x80000000u);  // -0.0
        put32(element, 0x00000001u);  // denormal
        s.attribute(9, 12, 2, element);
        s.list({0x90, 0x00, 0x03, 0x00, 0x00, 0x00});
        ConvertedGeometry g;
        std::string error;
        require(convert(s, pos_only(), g, &error), error);
        require(fbits(g.vertices[0].pos[0]) == 0x7fc00001u && fbits(g.vertices[0].pos[1]) == 0x80000000u &&
                    fbits(g.vertices[0].pos[2]) == 1u,
                "F32 is a bit copy");
    }
    // Normals: fixed GX scales, NBT takes N from the first three.
    struct NrmCase { u8 type; bool nbt; std::vector<u8> element; std::array<float, 3> expected; };
    const std::vector<NrmCase> normals = {
        {gekko::kGxCompS8, false, {0xc0, 0x7f, 0x01}, {-1.0f, 127.0f / 64.0f, 1.0f / 64.0f}},
        {gekko::kGxCompU8, false, {0x80, 0xff, 0x00}, {1.0f, 255.0f / 128.0f, 0.0f}},
        {gekko::kGxCompS16, false, {0xc0, 0x00, 0x40, 0x00, 0x00, 0x01}, {-1.0f, 1.0f, 1.0f / 16384.0f}},
        {gekko::kGxCompU16, false, {0x80, 0x00, 0xff, 0xff, 0x00, 0x02}, {1.0f, 65535.0f / 32768.0f, 2.0f / 32768.0f}},
        {gekko::kGxCompS8, true, {0x40, 0x00, 0xc0, 1, 2, 3, 4, 5, 6}, {1.0f, 0.0f, -1.0f}},
    };
    for (const NrmCase& c : normals) {
        Synth s;
        s.attribute(9, 12, 2, positions_f32(1));
        s.attribute(c.nbt ? 25 : 10, static_cast<u8>(c.element.size()), 2, c.element);
        s.list({0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        GxVertexFormatDesc format = gekko::rw_vertex_format(nullptr, 0x10, false);
        format.nrm_type = c.type;
        format.normal_nbt = c.nbt;
        ConvertedGeometry g;
        std::string error;
        require(convert(s, format, g, &error), error);
        for (u32 a = 0; a < 3; ++a) {
            require(fbits(g.vertices[0].nrm[a]) == fbits(c.expected[a]),
                    "normal type " + std::to_string(c.type) + (c.nbt ? " nbt" : "") + " axis " + std::to_string(a));
        }
        require(g.stats.gx_attr_mismatches == 0u, "NBT gx attr accepted");
    }
    {  // F32 normal
        Synth s;
        s.attribute(9, 12, 2, positions_f32(1));
        std::vector<u8> element;
        putf(element, 0.25f); putf(element, -0.5f); putf(element, 0.75f);
        s.attribute(10, 12, 2, element);
        s.list({0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        ConvertedGeometry g;
        std::string error;
        require(convert(s, gekko::rw_vertex_format(nullptr, 0x10, false), g, &error), error);
        require(g.vertices[0].nrm[0] == 0.25f && g.vertices[0].nrm[1] == -0.5f && g.vertices[0].nrm[2] == 0.75f,
                "F32 normal");
    }
    // Colours, every GX colour type.
    struct ClrCase { u8 type; std::vector<u8> element; std::array<u8, 4> expected; };
    const std::vector<ClrCase> colors = {
        {gekko::kGxColorRGB565, {0xf8, 0x00}, {255, 0, 0, 255}},
        {gekko::kGxColorRGB565, {0x07, 0xe0}, {0, 255, 0, 255}},
        {gekko::kGxColorRGB565, {0x84, 0x10}, {132, 130, 132, 255}},
        {gekko::kGxColorRGB8, {1, 2, 3}, {1, 2, 3, 255}},
        {gekko::kGxColorRGBX8, {4, 5, 6, 7}, {4, 5, 6, 255}},
        {gekko::kGxColorRGBA4, {0x12, 0x3f}, {17, 34, 51, 255}},
        {gekko::kGxColorRGBA6, {0xfc, 0x08, 0x01}, {255, 0, 130, 4}},
        {gekko::kGxColorRGBA8, {9, 8, 7, 6}, {9, 8, 7, 6}},
    };
    for (const ClrCase& c : colors) {
        Synth s;
        s.attribute(9, 12, 2, positions_f32(1));
        s.attribute(11, static_cast<u8>(c.element.size()), 2, c.element);
        s.list({0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        GxVertexFormatDesc format = gekko::rw_vertex_format(nullptr, 0x08, false);
        format.color_type = c.type;
        ConvertedGeometry g;
        std::string error;
        require(convert(s, format, g, &error), error);
        const u8* rgba = g.vertices[0].rgba;
        require(rgba[0] == c.expected[0] && rgba[1] == c.expected[1] && rgba[2] == c.expected[2] &&
                    rgba[3] == c.expected[3],
                "colour type " + std::to_string(c.type) + " got " + std::to_string(rgba[0]) + "," +
                    std::to_string(rgba[1]) + "," + std::to_string(rgba[2]) + "," + std::to_string(rgba[3]));
    }
    {  // Absent colour reads white; two texture sets with different types.
        Synth s;
        s.attribute(9, 12, 2, positions_f32(1));
        s.attribute(13, 4, 2, {0xff, 0x00, 0x01, 0x80});        // TEX0 S16 frac 8
        s.attribute(14, 2, 2, {0x07, 0xf9});                    // TEX1 S8 frac 3
        s.list({0x90, 0x00, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        GxVertexFormatDesc format = gekko::rw_vertex_format(nullptr, 0x84, false);
        format.tex_type[0] = gekko::kGxCompS16;
        format.tex_frac[0] = 8;
        format.tex_type[1] = gekko::kGxCompS8;
        format.tex_frac[1] = 3;
        ConvertedGeometry g;
        std::string error;
        require(convert(s, format, g, &error), error);
        const NativeVertex& n = g.vertices[0];
        require(n.rgba[0] == 255 && n.rgba[3] == 255, "absent colour is white");
        require(n.uv[0][0] == -1.0f && n.uv[0][1] == 384.0f / 256.0f && n.uv[1][0] == 7.0f / 8.0f &&
                    n.uv[1][1] == -7.0f / 8.0f,
                "texcoord integer decode");
        require(g.texcoord_sets == 2u, "two sets");
    }
}

void test_matrix_index_tuples() {
    Synth s;
    s.attribute(9, 12, 2, positions_f32(3));
    std::vector<u8> dl;
    put8(dl, 0x90); put16(dl, 3);
    for (u32 i = 0; i < 3; ++i) { put8(dl, 3 * i); put8(dl, i); }  // PNMTXIDX first
    s.list(dl);
    ConvertedGeometry g;
    std::string error;
    GxVertexFormatDesc format = pos_only();
    format.matrix_index = true;
    require(convert(s, format, g, &error), error);
    require(g.shared_index && g.matrix_index == std::vector<u8>({0, 3, 6}), "consistent palette stays 1:1");

    // The same source vertex with two matrix slots (split palette) unwelds.
    Synth t;
    t.attribute(9, 12, 2, positions_f32(3));
    std::vector<u8> dl2;
    put8(dl2, 0x90); put16(dl2, 6);
    for (u32 i = 0; i < 3; ++i) { put8(dl2, 0); put8(dl2, i); }
    for (u32 i = 0; i < 3; ++i) { put8(dl2, 9); put8(dl2, i); }
    t.list(dl2);
    require(convert(t, format, g, &error), error);
    require(!g.shared_index && g.vertices.size() == 6u, "conflicting palette slots unweld");
    require(triangle_sequence(g) == std::vector<u32>({0, 1, 2, 3, 4, 5}), "palette triangles");
    require(g.matrix_index == std::vector<u8>({0, 0, 0, 9, 9, 9}) &&
                g.pos_source_index == std::vector<u32>({0, 1, 2, 0, 1, 2}),
            "palette unweld sources");
}

void test_foreign_commands_skipped_by_length() {
    // The stateless vertex-cache invalidations are skipped by their length
    // and reported; the primitive after them still converts.
    Synth s;
    s.attribute(9, 12, 2, positions_f32(3));
    std::vector<u8> dl;
    put8(dl, 0x00);                                                    // NOP
    put8(dl, 0x44);
    put8(dl, 0x48);
    put8(dl, 0x90); put16(dl, 3); put8(dl, 0); put8(dl, 1); put8(dl, 2);
    s.list(dl);
    ConvertedGeometry g;
    std::string error;
    require(convert(s, pos_only(), g, &error), error);
    struct Expect { u32 offset; u8 opcode; u32 length; };
    const std::vector<Expect> expected = {{1, 0x44, 1}, {2, 0x48, 1}};
    require(g.foreign.size() == expected.size() && g.stats.foreign_commands == expected.size() &&
                g.meshes[0].foreign_commands == expected.size(),
            "foreign command count " + std::to_string(g.foreign.size()));
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(g.foreign[i].offset == expected[i].offset && g.foreign[i].opcode == expected[i].opcode &&
                    g.foreign[i].length == expected[i].length && g.foreign[i].mesh == 0,
                "foreign command " + std::to_string(i));
    }
    require(triangle_sequence(g) == std::vector<u32>({0, 1, 2}) && g.stats.primitives == 1u,
            "primitive after foreign commands");
    require(g.stats.nop_bytes == 1u + (64u - 9u % 32u) % 32u, "nop bytes " + std::to_string(g.stats.nop_bytes));
}

void test_state_commands_in_lists_refused() {
    // Display lists are not executed on the native path, so a list command
    // that writes CP/XF/BP state, or calls a nested list, has no executor and
    // must fail the conversion by name rather than be skipped.
    struct Case { const char* name; std::vector<u8> bytes; };
    std::vector<Case> cases;
    auto add = [&](const char* name, std::initializer_list<u8> bytes) { cases.push_back({name, bytes}); };
    add("cp", {0x08, 0x98, 0x98, 0x98, 0x98, 0x98});
    add("xf", {0x10, 0x00, 0x00, 0x10, 0x00, 0x98, 0x00, 0x03, 0x00});
    add("xf-indexed-20", {0x20, 0x98, 0x98, 0x98, 0x98});
    add("xf-indexed-28", {0x28, 0x98, 0x98, 0x98, 0x98});
    add("xf-indexed-30", {0x30, 0x98, 0x98, 0x98, 0x98});
    add("xf-indexed-38", {0x38, 0x98, 0x98, 0x98, 0x98});
    add("nested-list", {0x40, 0x80, 0x00, 0x12, 0x34, 0x00, 0x00, 0x00, 0x40});
    add("bp", {0x61, 0x41, 0x00, 0x00, 0x00});
    for (const Case& c : cases) {
        Synth s;
        s.attribute(9, 12, 2, positions_f32(3));
        std::vector<u8> dl = c.bytes;
        put8(dl, 0x90); put16(dl, 3); put8(dl, 0); put8(dl, 1); put8(dl, 2);
        s.list(dl);
        ConvertedGeometry g;
        std::string error;
        require(!convert(s, pos_only(), g, &error), std::string("state command accepted: ") + c.name);
        require(error.find("state command") != std::string::npos,
                std::string("refusal names the state command: ") + c.name + " -> " + error);
    }
}

void test_multi_mesh_ranges_and_empty() {
    Synth s;
    s.attribute(9, 12, 2, positions_f32(6));
    s.list({0x90, 0x00, 0x03, 0, 1, 2});
    s.lists.push_back({0, 0});  // empty display list
    s.list({0x98, 0x00, 0x04, 3, 4, 5, 3});
    ConvertedGeometry g;
    std::string error;
    require(convert(s, pos_only(), g, &error), error);
    require(g.meshes.size() == 3u, "three meshes");
    require(g.meshes[0].index_offset == 0 && g.meshes[0].index_count == 3, "mesh 0 range");
    require(g.meshes[1].index_offset == 3 && g.meshes[1].index_count == 0, "empty mesh range");
    require(g.meshes[2].index_offset == 3 && g.meshes[2].index_count == 6, "mesh 2 range");
    require(triangle_sequence(g) == std::vector<u32>({0, 1, 2, 3, 4, 5, 5, 4, 3}), "multi mesh triangles");

    Synth none;
    none.attribute(9, 12, 2, positions_f32(1));
    require(convert(none, pos_only(), g, &error), error);
    require(g.meshes.empty() && g.vertices.empty() && g.indices.empty(), "no meshes converts to nothing");
}

void expect_fail(const Synth& s, const GxVertexFormatDesc& format, const char* what) {
    ConvertedGeometry g;
    g.vertices.resize(3);
    std::string error;
    require(!convert(s, format, g, &error), std::string("malformed input accepted: ") + what);
    require(!error.empty(), std::string("no error text: ") + what);
    require(g.vertices.empty() && g.indices.empty() && g.meshes.empty() && g.attribute_count == 0,
            std::string("output not cleared: ") + what);
}

Synth valid_triangle() {
    Synth s;
    s.attribute(9, 12, 2, positions_f32(3));
    s.list({0x90, 0x00, 0x03, 0, 1, 2});
    return s;
}

void test_malformed_inputs_fail() {
    {
        ConvertedGeometry g;
        std::string error;
        const u8 tiny[8] = {};
        require(!gekko::rw_convert_instance(nullptr, 0, nullptr, 0, 0, pos_only(), g, &error), "null header");
        require(!gekko::rw_convert_instance(tiny, sizeof(tiny), nullptr, 0, 0, pos_only(), g, &error),
                "short header");
        Synth s = valid_triangle();
        std::vector<u8> header = s.header();
        header.push_back(0);
        require(!gekko::rw_convert_instance(header.data(), header.size(), s.data.data(), s.data.size(),
                                            s.base, pos_only(), g, &error),
                "misaligned header size");
        header.resize(12u + 4u);  // cuts into the attribute record
        require(!gekko::rw_convert_instance(header.data(), header.size(), s.data.data(), s.data.size(),
                                            s.base, pos_only(), g, &error),
                "truncated attribute record");
        // No spans at all.
        header = s.header();
        require(!gekko::rw_convert_instance(header.data(), header.size(), nullptr, 0, s.base, pos_only(),
                                            g, &error),
                "no data span");
    }
    { Synth s = valid_triangle(); s.attribute_count_override = 0xffffffffu; expect_fail(s, pos_only(), "N huge"); }
    { Synth s = valid_triangle(); s.records[0].index_type = 1; expect_fail(s, pos_only(), "GX_DIRECT index"); }
    { Synth s = valid_triangle(); s.records[0].index_type = 0; expect_fail(s, pos_only(), "GX_NONE index"); }
    { Synth s = valid_triangle(); s.records[0].stride = 8; expect_fail(s, pos_only(), "stride < element"); }
    { Synth s = valid_triangle(); s.records[0].stride = 0; expect_fail(s, pos_only(), "stride 0"); }
    { Synth s = valid_triangle(); s.lists[0].first = 0x10000; expect_fail(s, pos_only(), "DL beyond span"); }
    {
        // A span reaching the top of the 32-bit space and a list that runs 16
        // bytes past it: a u32 end-address check would wrap to 0x10 and pass.
        Synth s = valid_triangle();
        s.base = 0xfffff000u;
        s.data.resize(0x1000u, 0xee);
        s.lists[0].first = 0xff0u;
        s.lists[0].second = 0x20u;
        expect_fail(s, pos_only(), "DL wraps address space");
    }
    { Synth s = valid_triangle(); s.lists[0].second = 0x7fffffffu; expect_fail(s, pos_only(), "DL size overflow"); }
    { Synth s; s.attribute(9, 12, 2, positions_f32(3)); s.list({0x90, 0x00, 0x04, 0, 1, 2});
      s.lists[0].second = 6; expect_fail(s, pos_only(), "primitive payload truncated"); }
    { Synth s; s.attribute(9, 12, 2, positions_f32(3)); s.list({0x90, 0x00});
      s.lists[0].second = 2; expect_fail(s, pos_only(), "primitive header truncated"); }
    { Synth s; s.attribute(9, 12, 2, positions_f32(3)); s.list({0xc3}); expect_fail(s, pos_only(), "opcode 0xC3"); }
    { Synth s; s.attribute(9, 12, 2, positions_f32(3)); s.list({0x01}); expect_fail(s, pos_only(), "opcode 0x01"); }
    { Synth s; s.attribute(9, 12, 2, positions_f32(3)); s.list({0x62, 0, 0, 0, 0}); expect_fail(s, pos_only(), "opcode 0x62"); }
    { Synth s; s.attribute(9, 12, 2, positions_f32(3)); s.list({0x99, 0x00, 0x03, 0, 1, 2}); expect_fail(s, pos_only(), "VTXFMT1"); }
    { Synth s; s.attribute(9, 12, 2, positions_f32(3)); s.list({0x10, 0x00, 0x08, 0x10, 0x00, 0, 0, 0, 0});
      s.lists[0].second = 9; expect_fail(s, pos_only(), "XF load truncated"); }
    { Synth s; s.attribute(9, 12, 2, positions_f32(3)); s.list({0x40, 0x80, 0x00});
      s.lists[0].second = 3; expect_fail(s, pos_only(), "call DL truncated"); }
    {
        // Array element counts are not stored; the bound is the span. Put the
        // array last so index 3 reaches past the end of the data.
        Synth s;
        s.list({0x90, 0x00, 0x03, 0, 1, 3});
        s.attribute(9, 12, 2, positions_f32(3));
        expect_fail(s, pos_only(), "index past array");
    }
    { Synth s = valid_triangle(); s.records[0].offset = 0x7ffffff0u; expect_fail(s, pos_only(), "array outside span"); }
    {
        Synth s;
        s.attribute(9, 12, 2, positions_f32(3));
        for (u8 k = 0; k < 3; ++k) s.attribute(static_cast<u8>(13 + k), 8, 2, std::vector<u8>(24, 0));
        s.list({0x90, 0x00, 0x03, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2});
        expect_fail(s, gekko::rw_vertex_format(nullptr, 0x84, false), "three texture sets");
        expect_fail(s, pos_only(), "texture records without texture flags");
    }
    { Synth s = valid_triangle(); expect_fail(s, gekko::rw_vertex_format(nullptr, 0x18, false), "flags need NRM+CLR"); }
    { Synth s = valid_triangle(); GxVertexFormatDesc f = pos_only(); f.pos_type = 5; expect_fail(s, f, "pos type 5"); }
    { Synth s = valid_triangle(); GxVertexFormatDesc f = pos_only(); f.pos_type = 3; f.pos_frac = 32; expect_fail(s, f, "pos frac 32"); }
    {
        Synth s;
        s.attribute(9, 12, 2, positions_f32(3));
        s.attribute(11, 4, 2, std::vector<u8>(12, 0));
        s.list({0x90, 0x00, 0x03, 0, 0, 1, 1, 2, 2});
        GxVertexFormatDesc f = gekko::rw_vertex_format(nullptr, 0x08, false);
        f.color_type = 6;
        expect_fail(s, f, "colour type 6");
    }
}

void test_spans_dynamic_expand_no_alloc() {
    // Main span holds the header-pointed DL, CLR0, TEX0; POS and NRM live in a
    // second "output entry" span, as for a CPU-skinned geometry.
    static constexpr u8 kModelFmt[24] = {4, 1, 4, 4, 4, 4, 4, 4, 4, 4, 3, 1,
                                         6, 0, 6, 0, 0, 0, 0, 0, 0, 0, 2, 8};
    const GxVertexFormatDesc format = gekko::rw_vertex_format(kModelFmt, 0x3f, false);
    constexpr u32 kMainBase = 0x807a0000u;
    constexpr u32 kOutBase = 0x807b0000u;
    constexpr u32 kVertices = 40;
    std::vector<u8> out_span;
    for (u32 i = 0; i < 32; ++i) out_span.push_back(0xee);
    const u32 pos_offset = static_cast<u32>(out_span.size());
    for (u32 i = 0; i < kVertices; ++i) { putf(out_span, 1.5f * i); putf(out_span, -3.0f); putf(out_span, 0.125f * i); }
    const u32 nrm_offset = static_cast<u32>(out_span.size());
    for (u32 i = 0; i < kVertices; ++i) { put8(out_span, i); put8(out_span, 0x100u - i); put8(out_span, 64); }

    std::vector<u8> main_span(32, 0xee);
    std::vector<u8> dl;
    put8(dl, 0x98); put16(dl, kVertices + 2u);
    for (u32 i = 0; i < kVertices; ++i) { for (int a = 0; a < 4; ++a) put8(dl, i); }
    for (int a = 0; a < 4; ++a) put8(dl, kVertices - 1u);
    for (int a = 0; a < 4; ++a) put8(dl, 0);
    while (dl.size() % 32u) dl.push_back(0);
    const u32 dl_offset = static_cast<u32>(main_span.size());
    main_span.insert(main_span.end(), dl.begin(), dl.end());
    const u32 clr_offset = static_cast<u32>(main_span.size());
    for (u32 i = 0; i < kVertices; ++i) put16(main_span, 0xf00fu ^ i);
    const u32 tex_offset = static_cast<u32>(main_span.size());
    for (u32 i = 0; i < kVertices; ++i) { putf(main_span, 0.5f); putf(main_span, static_cast<float>(i)); }

    std::vector<u8> header;
    put16(header, 7); put16(header, 9); put32(header, 1); put32(header, 4);
    const auto record = [&](u32 address, u8 gx, u8 stride) { put32(header, address); put8(header, gx); put8(header, stride); put8(header, 2); put8(header, 0); };
    record(kOutBase + pos_offset, 9, 12);
    record(kOutBase + nrm_offset, 10, 3);
    record(kMainBase + clr_offset, 11, 2);
    record(kMainBase + tex_offset, 13, 8);
    put32(header, kMainBase + dl_offset); put32(header, static_cast<u32>(dl.size()));

    const GuestSpan both[2] = {{kMainBase, main_span.data(), main_span.size()},
                               {kOutBase, out_span.data(), out_span.size()}};
    ConvertedGeometry full;
    std::string error;
    require(gekko::rw_convert_instance_spans(header.data(), header.size(), both, 2, format, {}, full, &error),
            "two-span convert: " + error);
    require(full.draw_sync_stamp == 7 && full.mesh_serial == 9 && full.hdr_flags == 1, "header fields");
    require(full.source_other_spans, "a resolve in the second span is reported, not folded into the extent");
    require(full.shared_index && full.vertices.size() == kVertices, "skin-like entry stays 1:1");
    // Degenerate tail: (38,39,39) (39,39,0)... every join triangle dropped.
    require(full.stats.degenerate_dropped == 2u && full.stats.triangles == kVertices - 2u + 0u,
            "strip tail joins " + std::to_string(full.stats.degenerate_dropped) + " / " +
                std::to_string(full.stats.triangles));

    // Only the main span with dynamic POS/NRM must convert; POS/NRM stay 0.
    ConvertedGeometry dynamic;
    RwConvertOptions options;
    options.dynamic_pos_nrm = true;
    require(gekko::rw_convert_instance_spans(header.data(), header.size(), both, 1, format, options, dynamic, &error),
            "dynamic convert: " + error);
    require(!gekko::rw_convert_instance_spans(header.data(), header.size(), both, 1, format, {}, full, &error),
            "static convert without the output span must fail");
    require(gekko::rw_convert_instance_spans(header.data(), header.size(), both, 2, format, {}, full, &error), error);
    require(dynamic.indices == full.indices && dynamic.vertices.size() == full.vertices.size() &&
                dynamic.dynamic_pos_nrm,
            "dynamic topology equals static");
    std::vector<float> stream(dynamic.vertices.size() * 6u, -1.0f);
    const std::size_t pos_bytes = gekko::rw_attribute_source_bytes(dynamic.attributes[0]);
    const std::size_t nrm_bytes = gekko::rw_attribute_source_bytes(dynamic.attributes[1]);
    require(pos_bytes == kVertices * 12u && nrm_bytes == kVertices * 3u, "source byte extents");
    const u64 before = g_allocations.load();
    const bool expanded = gekko::rw_expand_dynamic_pos_nrm(
        dynamic, out_span.data() + pos_offset, pos_bytes, out_span.data() + nrm_offset, nrm_bytes,
        stream.data(), stream.size());
    const u64 after = g_allocations.load();
    require(expanded, "expand");
    if (kCountAllocations) require(after == before, "expand allocated " + std::to_string(after - before) + " times");
    struct FinalVertex {
        u32 head;
        float position[3];
        u8 gap[12];
        float normal[3];
        u32 tail;
    };
    std::vector<FinalVertex> final(dynamic.vertices.size());
    std::memset(final.data(), 0xA5, final.size() * sizeof(FinalVertex));
    const u64 strided_before = g_allocations.load();
    const bool strided_expanded = gekko::rw_expand_dynamic_pos_nrm_strided(
        dynamic, out_span.data() + pos_offset, pos_bytes,
        out_span.data() + nrm_offset, nrm_bytes,
        reinterpret_cast<u8*>(final.data()), final.size() * sizeof(FinalVertex),
        sizeof(FinalVertex), offsetof(FinalVertex, position),
        offsetof(FinalVertex, normal));
    const u64 strided_after = g_allocations.load();
    require(strided_expanded, "strided expand");
    if (kCountAllocations)
        require(strided_after == strided_before, "strided expand allocated");
    for (std::size_t v = 0; v < dynamic.vertices.size(); ++v) {
        const NativeVertex& ref = full.vertices[v];
        const NativeVertex& dyn = dynamic.vertices[v];
        for (int a = 0; a < 3; ++a) {
            require(fbits(stream[v * 6 + a]) == fbits(ref.pos[a]) && fbits(stream[v * 6 + 3 + a]) == fbits(ref.nrm[a]),
                    "expanded pos/nrm equals static decode");
            require(fbits(final[v].position[a]) == fbits(stream[v * 6 + a]) &&
                        fbits(final[v].normal[a]) == fbits(stream[v * 6 + 3 + a]),
                    "strided decode equals contiguous decode");
            require(dyn.pos[a] == 0.0f && dyn.nrm[a] == 0.0f, "dynamic leaves pos/nrm zero");
        }
        require(std::memcmp(dyn.rgba, ref.rgba, 4) == 0 && std::memcmp(dyn.uv, ref.uv, sizeof(ref.uv)) == 0,
                "dynamic static streams equal");
        require(final[v].head == 0xA5A5A5A5u && final[v].tail == 0xA5A5A5A5u,
                "strided decode preserves adjacent fields");
    }
    const std::vector<FinalVertex> guarded = final;
    const std::size_t required_strided_bytes =
        (final.size() - 1u) * sizeof(FinalVertex) + offsetof(FinalVertex, normal) +
        sizeof(FinalVertex::normal);
    require(!gekko::rw_expand_dynamic_pos_nrm_strided(
                dynamic, out_span.data() + pos_offset, pos_bytes,
                out_span.data() + nrm_offset, nrm_bytes,
                reinterpret_cast<u8*>(final.data()), required_strided_bytes - 1u,
                sizeof(FinalVertex), offsetof(FinalVertex, position),
                offsetof(FinalVertex, normal)),
            "short strided output rejected");
    require(std::memcmp(final.data(), guarded.data(), final.size() * sizeof(FinalVertex)) == 0,
            "rejected strided decode writes nothing");
    // Independently reproduce the former two-pass final upload, including
    // adjacent sentinels and nonnumeric UV bits. The fused writer must match
    // every byte and refuse malformed extents before any pose/attribute write.
    struct PackedUpload {
        u32 head;
        float position[3], normal[3];
        u8 color0[4], color1[4];
        float uv0[2], uv1[2];
        u32 tail;
    };
    const u32 uv_patterns[4]{0x80000000u, 0x7FC12345u, 0x7F812345u, 0x00000001u};
    std::memcpy(dynamic.vertices[0].uv, uv_patterns, sizeof(uv_patterns));
    std::vector<PackedUpload> legacy(dynamic.vertices.size()), fused(dynamic.vertices.size());
    std::memset(legacy.data(), 0xA5, legacy.size() * sizeof(PackedUpload));
    std::memset(fused.data(), 0xA5, fused.size() * sizeof(PackedUpload));
    require(gekko::rw_expand_dynamic_pos_nrm_strided(
                dynamic, out_span.data() + pos_offset, pos_bytes,
                out_span.data() + nrm_offset, nrm_bytes,
                reinterpret_cast<u8*>(legacy.data()), legacy.size() * sizeof(PackedUpload),
                sizeof(PackedUpload), offsetof(PackedUpload, position), offsetof(PackedUpload, normal)),
            "legacy upload pose reference");
    for (std::size_t v = 0; v < legacy.size(); ++v) {
        std::memcpy(legacy[v].color0, dynamic.vertices[v].rgba, 4u);
        std::memset(legacy[v].color1, 255, 4u);
        std::memcpy(legacy[v].uv0, dynamic.vertices[v].uv[0], sizeof(legacy[v].uv0));
        std::memcpy(legacy[v].uv1, dynamic.vertices[v].uv[1], sizeof(legacy[v].uv1));
    }
    gekko::RwPackedStaticFields fields{offsetof(PackedUpload, color0), offsetof(PackedUpload, color1),
                                     offsetof(PackedUpload, uv0), offsetof(PackedUpload, uv1)};
    const u64 fused_before = g_allocations.load();
    const bool fused_ok = gekko::rw_expand_dynamic_pos_nrm_strided(
        dynamic, out_span.data() + pos_offset, pos_bytes,
        out_span.data() + nrm_offset, nrm_bytes,
        reinterpret_cast<u8*>(fused.data()), fused.size() * sizeof(PackedUpload),
        sizeof(PackedUpload), offsetof(PackedUpload, position), offsetof(PackedUpload, normal), &fields);
    const u64 fused_after = g_allocations.load();
    require(fused_ok, "fused final upload");
    if (kCountAllocations) require(fused_before == fused_after, "fused upload allocated");
    require(std::memcmp(legacy.data(), fused.data(), fused.size() * sizeof(PackedUpload)) == 0,
            "fused upload byte-identical to two-pass reference, including raw UVs and sentinels");
    const auto packed_guard = fused;
    for (u32 failure = 0; failure < 4u; ++failure) {
        auto invalid_fields = fields;
        std::size_t output_bytes = fused.size() * sizeof(PackedUpload);
        std::size_t source_bytes = pos_bytes;
        if (failure == 0u) invalid_fields.uv1 = std::numeric_limits<std::size_t>::max();
        if (failure == 1u) output_bytes -= sizeof(PackedUpload::tail) + 1u;
        if (failure == 2u) --source_bytes;
        if (failure == 3u) invalid_fields.color1 = sizeof(PackedUpload);
        require(!gekko::rw_expand_dynamic_pos_nrm_strided(
                    dynamic, out_span.data() + pos_offset, source_bytes,
                    out_span.data() + nrm_offset, nrm_bytes,
                    reinterpret_cast<u8*>(fused.data()), output_bytes, sizeof(PackedUpload),
                    offsetof(PackedUpload, position), offsetof(PackedUpload, normal), &invalid_fields),
                "malformed fused source/field/output extent rejected");
        require(std::memcmp(fused.data(), packed_guard.data(), fused.size() * sizeof(PackedUpload)) == 0,
                "malformed fused upload changes no bytes");
    }
    require(!gekko::rw_expand_dynamic_pos_nrm(dynamic, out_span.data() + pos_offset, pos_bytes - 1u,
                                              out_span.data() + nrm_offset, nrm_bytes, stream.data(), stream.size()),
            "short POS rejected");
    require(!gekko::rw_expand_dynamic_pos_nrm(dynamic, out_span.data() + pos_offset, pos_bytes, nullptr, 0,
                                              stream.data(), stream.size()),
            "missing NRM rejected");
    require(!gekko::rw_expand_dynamic_pos_nrm(dynamic, out_span.data() + pos_offset, pos_bytes,
                                              out_span.data() + nrm_offset, nrm_bytes, stream.data(), stream.size() - 1u),
            "short output rejected");
}

// Deterministic mutation fuzz: nothing may crash or read out of bounds; any
// accepted result must be internally consistent.
struct Lcg {
    u64 state;
    u32 next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(state >> 33);
    }
};

void check_invariants(const ConvertedGeometry& g, const std::string& label) {
    require(g.indices.size() % 3u == 0u, label + " index count % 3");
    for (u32 index : g.indices) require(index < g.vertices.size(), label + " index in range");
    u64 covered = 0;
    for (const auto& mesh : g.meshes) {
        require(mesh.index_count % 3u == 0u && static_cast<u64>(mesh.index_offset) + mesh.index_count <= g.indices.size(),
                label + " mesh range");
        require(mesh.index_offset == covered, label + " mesh ranges contiguous");
        covered += mesh.index_count;
    }
    require(covered == g.indices.size(), label + " ranges cover indices");
    require(g.pos_source_index.size() == g.vertices.size(), label + " pos source size");
    for (u32 v = 0; v < g.vertices.size(); ++v) {
        require(g.pos_source_index[v] < g.attributes[0].element_count, label + " pos source in range");
    }
}

u64 fuzz_buffer(std::vector<u8> header, std::vector<u8> data, u32 base, const GxVertexFormatDesc& format,
                u32 iterations, u64 seed, u64& accepted) {
    Lcg rng{seed};
    ConvertedGeometry g;
    std::string error;
    u64 runs = 0;
    for (u32 it = 0; it < iterations; ++it) {
        std::vector<u8> h = header;
        std::vector<u8> d = data;
        const u32 flips = 1u + rng.next() % 6u;
        for (u32 f = 0; f < flips; ++f) {
            const u32 where = rng.next();
            const u8 value = static_cast<u8>(rng.next());
            if ((where & 1u) != 0u && !h.empty()) {
                h[(where >> 1) % h.size()] = value;
            } else if (!d.empty()) {
                d[(where >> 1) % d.size()] = value;
            }
        }
        std::size_t header_size = h.size();
        if ((rng.next() % 16u) == 0u) header_size = rng.next() % (h.size() + 1u);
        std::size_t data_size = d.size();
        if ((rng.next() % 16u) == 0u) data_size = rng.next() % (d.size() + 1u);
        GxVertexFormatDesc f = format;
        if ((rng.next() % 32u) == 0u) f.pos_type = static_cast<u8>(rng.next() % 6u);
        if ((rng.next() % 32u) == 0u) f.matrix_index = !f.matrix_index;
        RwConvertOptions options;
        options.dynamic_pos_nrm = (rng.next() % 4u) == 0u;
        ++runs;
        if (gekko::rw_convert_instance(h.data(), header_size, d.data(), data_size, base, f, g, &error, options)) {
            ++accepted;
            check_invariants(g, "fuzz iteration " + std::to_string(it));
        }
    }
    return runs;
}

void test_synthetic_fuzz() {
    Synth s;
    s.attribute(9, 12, 3, positions_f32(20));
    std::vector<u8> colors;
    for (u32 i = 0; i < 8; ++i) put32(colors, 0x11223344u * (i + 1));
    s.attribute(11, 4, 2, colors);
    std::vector<u8> uvs(16u * 8u, 0x3f);
    s.attribute(13, 8, 2, uvs);
    std::vector<u8> dl;
    put8(dl, 0x98); put16(dl, 12);
    for (u32 i = 0; i < 12; ++i) { put16(dl, (i * 7u) % 20u); put8(dl, i % 8u); put8(dl, (i * 3u) % 16u); }
    put8(dl, 0x10); put16(dl, 0); put16(dl, 0x1234); put32(dl, 0);
    put8(dl, 0xa0); put16(dl, 5);
    for (u32 i = 0; i < 5; ++i) { put16(dl, i); put8(dl, i); put8(dl, i); }
    s.list(dl);
    s.list({0x80, 0x00, 0x04, 0, 1, 1, 1, 0, 2, 2, 2, 0, 3, 3, 3, 0, 4, 4, 4});
    const GxVertexFormatDesc format = gekko::rw_vertex_format(nullptr, 0x0c, false);
    u64 accepted = 0;
    const u64 runs = fuzz_buffer(s.header(), s.data, s.base, format, 50000, 0x5eed0001ull, accepted);
    std::printf("      synthetic fuzz: %llu runs, %llu accepted\n", static_cast<unsigned long long>(runs),
                static_cast<unsigned long long>(accepted));
    require(accepted > 0 && accepted < runs, "fuzz must exercise both outcomes");
}

// ---------------------------------------------------------------------------
// Real data: MEM1 dump of Final Haunt at 45e9 (RENDERER_RE §11.2)
// ---------------------------------------------------------------------------

constexpr u32 kMem1Base = 0x80000000u;
constexpr u32 kNotifyStatic = 0x804638DCu;
constexpr u32 kNotifySkinned = 0x8044DE14u;
constexpr u32 kSectorRenderCallback = 0x80460B60u;

struct Mem1 {
    std::vector<u8> bytes;
    bool ok(u32 address, u64 size) const {
        return address >= kMem1Base && static_cast<u64>(address - kMem1Base) + size <= bytes.size();
    }
    const u8* at(u32 address) const { return bytes.data() + (address - kMem1Base); }
    u32 be32(u32 address) const {
        if (!ok(address, 4)) return 0;
        const u8* p = at(address);
        return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
    }
    u16 be16(u32 address) const {
        if (!ok(address, 2)) return 0;
        const u8* p = at(address);
        return static_cast<u16>((p[0] << 8) | p[1]);
    }
    u8 b(u32 address) const { return ok(address, 1) ? *at(address) : 0; }
};

// ---- the independent oracle: a straightforward GX re-decoder -------------
// Written from the GX rules only: walk the list byte by byte, fetch every
// vertex tuple from guest memory by address, expand each primitive in legacy
// renderer order, drop triangles with two identical tuples.
struct OracleVertex {
    std::array<u32, 3> pos{};
    std::array<u32, 3> nrm{};
    std::array<u8, 4> rgba{255, 255, 255, 255};
    std::array<u32, 4> uv{};
    u32 pos_index = 0;
    u32 nrm_index = 0;
};

struct OracleMesh {
    std::vector<std::array<OracleVertex, 3>> triangles;
    u32 dropped = 0;
    u32 dl_vertices = 0;
    u32 geometric_degenerate_kept = 0;
};

u32 oracle_bits(float value) { return fbits(value); }

float oracle_int_scaled(s32 value, u8 frac) { return std::ldexp(static_cast<float>(value), -static_cast<int>(frac)); }

float oracle_scalar(const Mem1& m, u32 address, u8 type, u8 frac) {
    switch (type) {
        case 0: return oracle_int_scaled(m.b(address), frac);
        case 1: return oracle_int_scaled(static_cast<s8>(m.b(address)), frac);
        case 2: return oracle_int_scaled(m.be16(address), frac);
        case 3: return oracle_int_scaled(static_cast<s16>(m.be16(address)), frac);
        default: {
            float value;
            const u32 bits = m.be32(address);
            std::memcpy(&value, &bits, 4);
            return value;
        }
    }
}

float oracle_normal(const Mem1& m, u32 address, u8 type) {
    switch (type) {
        case 0: return static_cast<float>(m.b(address)) * (1.0f / 128.0f);
        case 1: return static_cast<float>(static_cast<s8>(m.b(address))) * (1.0f / 64.0f);
        case 2: return static_cast<float>(m.be16(address)) * (1.0f / 32768.0f);
        case 3: return static_cast<float>(static_cast<s16>(m.be16(address))) * (1.0f / 16384.0f);
        default: return oracle_scalar(m, address, 4, 0);
    }
}

u32 oracle_size(u8 type) { return type <= 1 ? 1u : type <= 3 ? 2u : 4u; }

bool oracle_decode(const Mem1& m, u32 header, u32 mesh_count, u32 flags, const u8* fmt,
                   bool palette, std::vector<OracleMesh>& meshes, std::string& why) {
    const u32 n = m.be32(header + 8);
    struct Attr { u32 array; u8 stride; u8 width; char role; u8 set; };
    std::vector<Attr> attrs;
    const bool has_nrm = (flags & 0x10) != 0;
    const bool has_clr = (flags & 0x08) != 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 r = header + 12 + 8 * i;
        Attr a{m.be32(r), m.b(r + 5), static_cast<u8>(m.b(r + 6) == 3 ? 2 : 1), 'T', 0};
        if (i == 0) a.role = 'P';
        else if (has_nrm && i == 1) a.role = 'N';
        else if (has_clr && i == (has_nrm ? 2u : 1u)) a.role = 'C';
        else a.set = static_cast<u8>(i - 1u - (has_nrm ? 1u : 0u) - (has_clr ? 1u : 0u));
        attrs.push_back(a);
    }
    meshes.assign(mesh_count, {});
    for (u32 j = 0; j < mesh_count; ++j) {
        const u32 list = m.be32(header + 12 + 8 * n + 8 * j);
        const u32 size = m.be32(header + 12 + 8 * n + 8 * j + 4);
        u32 p = list;
        while (p < list + size) {
            const u8 op = m.b(p);
            if (op == 0) { ++p; continue; }
            if (op < 0x80 || op > 0xa7 || (op & 7) != 0) { why = "oracle: unexpected opcode"; return false; }
            const u32 count = m.be16(p + 1);
            p += 3;
            std::vector<OracleVertex> vertices(count);
            std::vector<std::vector<u32>> tuples(count);
            for (u32 v = 0; v < count; ++v) {
                if (palette) tuples[v].push_back(m.b(p++));
                std::vector<u32> indices;
                for (const Attr& a : attrs) {
                    u32 index = a.width == 1 ? m.b(p) : m.be16(p);
                    p += a.width;
                    indices.push_back(index);
                    tuples[v].push_back(index);
                }
                OracleVertex& o = vertices[v];
                for (std::size_t k = 0; k < attrs.size(); ++k) {
                    const Attr& a = attrs[k];
                    const u32 element = a.array + indices[k] * a.stride;
                    if (a.role == 'P') {
                        o.pos_index = indices[k];
                        for (u32 c = 0; c < 3; ++c)
                            o.pos[c] = oracle_bits(oracle_scalar(m, element + c * oracle_size(fmt[0]), fmt[0], fmt[12]));
                    } else if (a.role == 'N') {
                        o.nrm_index = indices[k];
                        for (u32 c = 0; c < 3; ++c)
                            o.nrm[c] = oracle_bits(oracle_normal(m, element + c * oracle_size(fmt[1]), fmt[1]));
                    } else if (a.role == 'C') {
                        const u8 type = fmt[10];
                        if (type == 3) {
                            const u32 value = m.be16(element);
                            for (u32 c = 0; c < 4; ++c) o.rgba[c] = static_cast<u8>(((value >> (12 - 4 * c)) & 15u) * 17u);
                        } else if (type == 5) {
                            for (u32 c = 0; c < 4; ++c) o.rgba[c] = m.b(element + c);
                        } else {
                            why = "oracle: colour type not in the dump";
                            return false;
                        }
                    } else if (a.set < 2) {
                        const u8 type = fmt[2 + a.set];
                        const u8 frac = fmt[14 + a.set];
                        o.uv[a.set * 2u] = oracle_bits(oracle_scalar(m, element, type, frac));
                        o.uv[a.set * 2u + 1u] = oracle_bits(oracle_scalar(m, element + oracle_size(type), type, frac));
                    }
                }
            }
            OracleMesh& mesh = meshes[j];
            mesh.dl_vertices += count;
            const auto add = [&](u32 a, u32 b, u32 c) {
                if (tuples[a] == tuples[b] || tuples[b] == tuples[c] || tuples[a] == tuples[c]) {
                    ++mesh.dropped;
                    return;
                }
                if (vertices[a].pos == vertices[b].pos || vertices[b].pos == vertices[c].pos || vertices[a].pos == vertices[c].pos) {
                    ++mesh.geometric_degenerate_kept;
                }
                mesh.triangles.push_back({vertices[a], vertices[b], vertices[c]});
            };
            const u32 kind = (op >> 3) & 7u;
            if (kind == 3) {  // strip, legacy renderer.cpp form
                for (u32 k = 0; k + 2 < count; ++k) {
                    if ((k & 1u) == 0u) add(k, k + 1, k + 2); else add(k + 1, k, k + 2);
                }
            } else if (kind == 2) {
                for (u32 k = 0; k + 2 < count; k += 3) add(k, k + 1, k + 2);
            } else if (kind == 4) {
                for (u32 k = 1; k + 1 < count; ++k) add(0, k, k + 1);
            } else {
                for (u32 k = 0; k + 3 < count; k += 4) { add(k, k + 1, k + 2); add(k, k + 2, k + 3); }
            }
        }
        if (p != list + size) { why = "oracle: list overrun"; return false; }
    }
    return true;
}

// Per mesh: same triangle count, drop count and list vertex count; per triangle
// corner: bit-identical position/normal/uv, identical colour and source indices.
void compare_with_oracle(const ConvertedGeometry& g, const std::vector<OracleMesh>& oracle,
                         std::vector<std::string>& problems) {
    if (g.meshes.size() != oracle.size()) {
        problems.push_back("mesh count differs");
        return;
    }
    for (std::size_t j = 0; j < oracle.size(); ++j) {
        const OracleMesh& om = oracle[j];
        const gekko::ConvertedMesh& cm = g.meshes[j];
        if (cm.index_count != om.triangles.size() * 3u || cm.degenerate_dropped != om.dropped ||
            cm.dl_vertices != om.dl_vertices) {
            problems.push_back("mesh " + std::to_string(j) + " counts: tris " + std::to_string(cm.index_count / 3) +
                               " vs " + std::to_string(om.triangles.size()) + ", dropped " +
                               std::to_string(cm.degenerate_dropped) + " vs " + std::to_string(om.dropped));
            continue;
        }
        u32 mismatches = 0;
        for (std::size_t t = 0; t < om.triangles.size(); ++t) {
            for (u32 c = 0; c < 3; ++c) {
                const u32 vid = g.indices[cm.index_offset + t * 3 + c];
                const NativeVertex& v = g.vertices[vid];
                const OracleVertex& o = om.triangles[t][c];
                bool same = g.pos_source_index[vid] == o.pos_index;
                for (u32 a = 0; a < 3; ++a) same = same && fbits(v.pos[a]) == o.pos[a] && fbits(v.nrm[a]) == o.nrm[a];
                for (u32 a = 0; a < 4; ++a) same = same && v.rgba[a] == o.rgba[a] && fbits(v.uv[a / 2][a % 2]) == o.uv[a];
                if (g.has_normals) same = same && g.nrm_source_index[vid] == o.nrm_index;
                if (!same) ++mismatches;
            }
        }
        if (mismatches != 0) problems.push_back("mesh " + std::to_string(j) + ": " + std::to_string(mismatches) + " vertex mismatches");
    }
}

struct RealEntry {
    u32 entry = 0;
    u32 owner = 0;
    u32 owner_ref_offset = 0;  // 0x58 geometry, 0x14 atomic, 0x34 sector
    u32 notify = 0;
    u32 size = 0;
};

const char* kind_name(const RealEntry& e) {
    if (e.owner_ref_offset == 0x34) return "sector";
    return e.notify == kNotifySkinned ? "skinned" : "static";
}

bool same_geometry(const ConvertedGeometry& a, const ConvertedGeometry& b, bool compare_pos_nrm) {
    if (a.vertices.size() != b.vertices.size() || a.indices != b.indices || a.meshes.size() != b.meshes.size() ||
        a.pos_source_index != b.pos_source_index || a.nrm_source_index != b.nrm_source_index ||
        a.shared_index != b.shared_index) {
        return false;
    }
    for (std::size_t m = 0; m < a.meshes.size(); ++m) {
        if (std::memcmp(&a.meshes[m], &b.meshes[m], sizeof(a.meshes[m])) != 0) return false;
    }
    for (std::size_t v = 0; v < a.vertices.size(); ++v) {
        const NativeVertex& x = a.vertices[v];
        const NativeVertex& y = b.vertices[v];
        if (compare_pos_nrm && std::memcmp(&x, &y, sizeof(NativeVertex)) != 0) return false;
        if (std::memcmp(x.rgba, y.rgba, 4) != 0 || std::memcmp(x.uv, y.uv, sizeof(x.uv)) != 0) return false;
    }
    return true;
}

std::string g_mem1_path = "H:/ShadowTheHedgehogDiskGC_Extract/RENDERER_RE/_work/geometry/runs/B45.mem1";

struct RealTotals {
    std::map<std::string, u32> entries;
    std::map<std::string, u64> dl_bytes;
    u64 triangles = 0;
    u64 foreign = 0;
    u64 unsupported = 0;
};

void run_real_data(int& failures, int& passes) {
    const auto report = [&](bool ok, const std::string& name, const std::string& detail) {
        std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
        (ok ? passes : failures) += 1;
    };
    Mem1 m;
    {
        std::ifstream file(g_mem1_path, std::ios::binary);
        if (!file) {
            report(false, "fh_mem1_load", "cannot open " + g_mem1_path);
            return;
        }
        m.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    report(m.bytes.size() == 0x1800000u, "fh_mem1_load", std::to_string(m.bytes.size()) + " bytes");

    // Discovery 1: scan for live RwResEntry records (notify + owner back-reference).
    std::vector<RealEntry> entries;
    for (u32 offset = 0; offset + 24u <= m.bytes.size(); offset += 4u) {
        const u32 address = kMem1Base + offset;
        const u32 notify = m.be32(address + 0x14);
        if (notify != kNotifyStatic && notify != kNotifySkinned) continue;
        const u32 owner = m.be32(address + 0x0c);
        const u32 ref = m.be32(address + 0x10);
        const u32 delta = ref - owner;
        if (!m.ok(ref, 4) || (delta != 0x58 && delta != 0x14 && delta != 0x34) || m.be32(ref) != address) continue;
        entries.push_back({address, owner, delta, notify, m.be32(address + 8)});
    }
    // Discovery 2: walk the resource arena list (capture map §4.12) and compare.
    const u32 res_globals = m.be32(0x805F265Cu) + m.be32(0x805F2620u);
    std::set<u32> arena;
    {
        const u32 head = res_globals + 0x18;
        u32 link = m.be32(head);
        for (u32 guard = 0; link != head && guard < 100000; ++guard) {
            const u32 notify = m.be32(link + 0x14);
            if (notify == kNotifyStatic || notify == kNotifySkinned) arena.insert(link);
            link = m.be32(link);
        }
    }
    std::set<u32> scanned_arena;
    u32 sectors = 0, statics = 0, skinned = 0;
    for (const RealEntry& e : entries) {
        if (e.owner_ref_offset == 0x34) {
            ++sectors;
        } else {
            scanned_arena.insert(e.entry);
            (e.notify == kNotifySkinned ? skinned : statics) += 1;
        }
    }
    char text[256];
    std::snprintf(text, sizeof(text), "scan: %u sectors, %u static, %u skinned; arena list: %zu", sectors, statics,
                  skinned, arena.size());
    report(sectors == 8 && statics == 20 && skinned == 5 && arena == scanned_arena, "fh_entry_discovery", text);

    // Worlds (render callback at +0x68) and the sectors of their BSP trees give
    // each sector entry its curWorld flags and format.
    std::map<u32, u32> sector_world;
    for (u32 offset = 0; offset + 0x74u <= m.bytes.size(); offset += 4u) {
        const u32 world = kMem1Base + offset;
        if (m.be32(world + 0x68) != kSectorRenderCallback) continue;
        std::vector<u32> stack = {m.be32(world + 0x1c)};
        for (u32 guard = 0; !stack.empty() && guard < 100000; ++guard) {
            const u32 node = stack.back();
            stack.pop_back();
            if (!m.ok(node, 16)) continue;
            if (static_cast<s32>(m.be32(node)) < 0) {
                sector_world[node] = world;
            } else {
                stack.push_back(m.be32(node + 8));
                stack.push_back(m.be32(node + 12));
            }
        }
    }

    const u32 geometry_fmt_offset = m.be32(0x805F25C8u);
    const u32 world_fmt_offset = m.be32(0x805F25CCu);
    const u32 skin_geometry_offset = m.be32(0x8056E7C8u + 8u);
    {
        // The converter's built-in default must be the driver's default format.
        const GxVertexFormatDesc a = gekko::rw_vertex_format(nullptr, 0xff, false);
        const GxVertexFormatDesc b = gekko::rw_vertex_format(m.at(0x805E2748u), 0xff, false);
        const bool same = a.pos_type == b.pos_type && a.pos_frac == b.pos_frac && a.nrm_type == b.nrm_type &&
                          a.normal_nbt == b.normal_nbt && a.color_type == b.color_type && a.tex_type == b.tex_type &&
                          a.tex_frac == b.tex_frac;
        report(same, "fh_default_vtxfmt_matches_0x805E2748", "");
    }

    std::sort(entries.begin(), entries.end(), [](const RealEntry& a, const RealEntry& b) {
        if (a.owner_ref_offset != b.owner_ref_offset) return a.owner_ref_offset < b.owner_ref_offset;
        if (a.notify != b.notify) return a.notify < b.notify;
        return a.entry < b.entry;
    });

    std::printf("      %-7s %-8s %4s %-17s %6s %6s %6s %6s %6s %4s %6s %5s %4s %4s %9s %9s\n", "kind", "entry",
                "mesh", "attrs", "dlB", "dlV", "binIdx", "srcPos", "outV", "map", "tris", "degen", "frgn",
                "unsp", "us(min)", "us(first)");
    RealTotals totals;
    ConvertedGeometry g;
    ConvertedGeometry tight;
    ConvertedGeometry dynamic;
    std::vector<float> stream;
    std::string error;
    for (const RealEntry& e : entries) {
        const std::string name = std::string("fh_") + kind_name(e) + "_" + [&] {
            char a[16];
            std::snprintf(a, sizeof(a), "%08X", e.entry);
            return std::string(a);
        }();
        u32 flags = 0;
        u32 fmt_pointer = 0;
        u32 mesh_header = 0;
        bool palette = false;
        u32 model_vertices = 0;
        if (e.owner_ref_offset == 0x34) {
            const auto it = sector_world.find(e.owner);
            if (it == sector_world.end()) {
                report(false, name, "no world owns this sector");
                continue;
            }
            flags = m.be32(it->second + 8);
            fmt_pointer = m.be32(it->second + world_fmt_offset);
            mesh_header = m.be32(e.owner + 0x78);
        } else {
            const u32 geometry = e.owner_ref_offset == 0x58 ? e.owner : m.be32(e.owner + 0x18);
            flags = m.be32(geometry + 8);
            fmt_pointer = m.be32(geometry + geometry_fmt_offset);
            mesh_header = m.be32(geometry + 0x54);
            model_vertices = m.be32(geometry + 0x14);
            const u32 skin = m.be32(geometry + skin_geometry_offset);
            palette = skin != 0 && m.be32(skin + 0x10) == 1u && m.be32(skin + 0x2c) != 3u;
        }
        const u32 mesh_count = m.be16(mesh_header + 4);
        const u8* fmt_bytes = fmt_pointer != 0 ? m.at(fmt_pointer) : m.at(0x805E2748u);
        const GxVertexFormatDesc format = gekko::rw_vertex_format(fmt_pointer != 0 ? m.at(fmt_pointer) : nullptr, flags, palette);
        const u32 header = e.entry + 24;
        const u32 attribute_count = m.be32(header + 8);
        const std::size_t header_size = 12u + 8u * attribute_count + 8u * mesh_count;

        // A: one span, all of MEM1. Timed: first call and the best of 32.
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = gekko::rw_convert_instance(m.at(header), header_size, m.bytes.data(), m.bytes.size(), kMem1Base,
                                             format, g, &error);
        const double first_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        double best_us = first_us;
        for (int run = 0; ok && run < 32; ++run) {
            const auto t = std::chrono::steady_clock::now();
            ok = gekko::rw_convert_instance(m.at(header), header_size, m.bytes.data(), m.bytes.size(), kMem1Base,
                                            format, g, &error);
            best_us = std::min(best_us, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count());
        }
        if (!ok) {
            report(false, name, "convert: " + error);
            continue;
        }
        std::vector<std::string> problems;

        // Oracle comparison: exact triangle sequence, bit-exact attributes.
        std::vector<OracleMesh> oracle;
        std::string why;
        u32 geometric_kept = 0;
        if (!oracle_decode(m, header, mesh_count, flags, fmt_bytes, palette, oracle, why)) {
            problems.push_back(why);
        } else {
            for (const OracleMesh& om : oracle) geometric_kept += om.geometric_degenerate_kept;
            compare_with_oracle(g, oracle, problems);
            // Negative control: the comparison must see a winding swap, one
            // colour bit, one UV bit, a wrong drop count and a wrong source index.
            if (problems.empty() && (e.entry == 0x812FA760u || e.entry == 0x807D4AC0u) && g.indices.size() >= 3) {
                u32 detected = 0;
                const u32 vid = g.indices[0];
                for (int perturbation = 0; perturbation < 5; ++perturbation) {
                    ConvertedGeometry bad = g;
                    switch (perturbation) {
                        case 0: std::swap(bad.indices[1], bad.indices[2]); break;
                        case 1: bad.vertices[vid].rgba[1] ^= 1u; break;
                        case 2: bad.vertices[vid].uv[0][1] = std::nextafter(bad.vertices[vid].uv[0][1], 1e30f); break;
                        case 3: bad.meshes[0].degenerate_dropped += 1u; break;
                        default: bad.pos_source_index[vid] += 1u; break;
                    }
                    std::vector<std::string> found;
                    compare_with_oracle(bad, oracle, found);
                    if (!found.empty()) ++detected;
                }
                std::printf("      negative control %08X: %u of 5 perturbations detected\n", e.entry, detected);
                if (detected != 5u) problems.push_back("oracle comparison missed a perturbation");
            }
        }
        std::vector<bool> used(g.vertices.size(), false);
        for (u32 index : g.indices) used[index] = true;
        const auto unreferenced = static_cast<u32>(std::count(used.begin(), used.end(), false));

        // B: realistic capture spans (entry payload; skins add the output arrays).
        std::vector<GuestSpan> spans;
        // Native Data PLG entries are RwMalloc(headerSize + dataSize + 55) with
        // entry.size = chunk size = 12 + headerSize + dataSize, so the payload
        // after the 24-byte RwResEntry is size + 19 bytes. Arena entries store
        // the payload size itself.
        const u32 payload = e.owner_ref_offset == 0x34 ? e.size + 19u : e.size;
        spans.push_back({header, m.at(header), payload});
        if (e.notify == kNotifySkinned) {
            const u32 pos_array = m.be32(header + 12);
            const u32 nrm_stride = (flags & 0x10) ? m.b(header + 12 + 8 + 5) : 0u;
            const auto align32 = [](u32 v) { return (v + 31u) & ~31u; };
            const u32 extent = align32(model_vertices * 12u) + align32(model_vertices * nrm_stride);
            spans.push_back({pos_array, m.at(pos_array), extent});
        }
        if (!gekko::rw_convert_instance_spans(m.at(header), header_size, spans.data(), spans.size(), format, {}, tight, &error)) {
            problems.push_back("tight spans: " + error);
        } else if (!same_geometry(g, tight, true)) {
            problems.push_back("tight-span result differs from whole-MEM1 result");
        }

        // C: dynamic POS/NRM with only the main span, re-expanded from memory.
        RwConvertOptions options;
        options.dynamic_pos_nrm = true;
        if (!gekko::rw_convert_instance_spans(m.at(header), header_size, spans.data(), 1, format, options, dynamic, &error)) {
            problems.push_back("dynamic convert: " + error);
        } else if (!same_geometry(g, dynamic, false)) {
            problems.push_back("dynamic topology/static streams differ");
        } else {
            stream.assign(dynamic.vertices.size() * 6u, 0.0f);
            const auto& pa = dynamic.attributes[0];
            const u8* nrm = g.has_normals ? m.at(dynamic.attributes[1].guest_array) : nullptr;
            const std::size_t nrm_bytes = g.has_normals ? gekko::rw_attribute_source_bytes(dynamic.attributes[1]) : 0u;
            const u64 before = g_allocations.load();
            const bool expanded = gekko::rw_expand_dynamic_pos_nrm(dynamic, m.at(pa.guest_array), gekko::rw_attribute_source_bytes(pa),
                                                                   nrm, nrm_bytes, stream.data(), stream.size());
            const u64 allocated = g_allocations.load() - before;
            if (!expanded) {
                problems.push_back("expand failed");
            } else {
                if (kCountAllocations && allocated != 0) problems.push_back("expand allocated");
                u32 bad = 0;
                for (std::size_t v = 0; v < g.vertices.size(); ++v) {
                    for (u32 a = 0; a < 3; ++a) {
                        if (fbits(stream[v * 6 + a]) != fbits(g.vertices[v].pos[a]) ||
                            fbits(stream[v * 6 + 3 + a]) != fbits(g.vertices[v].nrm[a])) {
                            ++bad;
                        }
                    }
                }
                if (bad != 0) problems.push_back(std::to_string(bad) + " expanded components differ");
            }
        }

        if (g.stats.foreign_commands != 0) problems.push_back("foreign commands present");
        if (g.stats.unsupported_primitives != 0) problems.push_back("line/point primitives present");
        if (e.owner_ref_offset != 0x34 && !g.shared_index) problems.push_back("model did not keep 1:1 mapping");
        if (e.owner_ref_offset != 0x34 && g.vertices.size() > model_vertices) problems.push_back("more output vertices than the model has");

        u64 dl_bytes = 0;
        u32 bin_indices = 0;
        for (u32 j = 0; j < mesh_count; ++j) {
            dl_bytes += m.be32(header + 12 + 8 * attribute_count + 8 * j + 4);
            bin_indices += m.be32(mesh_header + 16 + 12 * j + 4);
        }
        std::string attrs;
        for (u32 a = 0; a < g.attribute_count; ++a) {
            static const char* kRole[] = {"P", "N", "C", "T"};
            attrs += kRole[static_cast<int>(g.attributes[a].role)];
            attrs += g.attributes[a].index_bytes == 1 ? "8" : "16";
            if (a + 1 < g.attribute_count) attrs += " ";
        }
        std::printf("      %-7s %08X %4u %-17s %6llu %6u %6u %6u %6zu %4s %6u %5u %4u %4u %9.3f %9.3f\n", kind_name(e),
                    e.entry, mesh_count, attrs.c_str(), static_cast<unsigned long long>(dl_bytes), g.stats.dl_vertices,
                    bin_indices, g.attributes[0].element_count, g.vertices.size(), g.shared_index ? "1:1" : "hash",
                    g.stats.triangles, g.stats.degenerate_dropped, g.stats.foreign_commands,
                    g.stats.unsupported_primitives, best_us, first_us);
        if (geometric_kept != 0 || unreferenced != 0) {
            std::printf("              (geometrically degenerate triangles kept: %u, unreferenced output vertices: %u)\n",
                        geometric_kept, unreferenced);
        }
        if (e.owner_ref_offset == 0x34) {
            // Informational: RpWorldSector numVertices (+0x82) next to the unwelded count.
            std::printf("              (sector numVertices %u, unwelded tuples %zu)\n", m.be16(e.owner + 0x82),
                        g.vertices.size());
        }
        totals.entries[kind_name(e)] += 1;
        totals.dl_bytes[kind_name(e)] += dl_bytes;
        totals.triangles += g.stats.triangles;
        totals.foreign += g.stats.foreign_commands;
        totals.unsupported += g.stats.unsupported_primitives;

        std::string detail;
        for (const std::string& p : problems) detail += p + "; ";
        report(problems.empty(), name, detail);
    }
    // RE-02 §11.2's 8,224 static DL bytes came from probe3.py, which also walked
    // the stale look-alike 0x807E4540 (capture map §5.2: its owner 0x80AC0FE0
    // points at 0x807BFB60, so the liveness scan above rejects it). Account for
    // it explicitly instead of adjusting the documented number.
    u64 stale_dl_bytes = 0;
    {
        constexpr u32 kStale = 0x807E4540u;
        const u32 owner = m.be32(kStale + 0x0c);
        const bool stale = m.be32(kStale + 0x14) == kNotifyStatic && owner == 0x80AC0FE0u &&
                           m.be32(m.be32(kStale + 0x10)) != kStale;
        const u32 stale_header = kStale + 24;
        const u32 stale_attributes = m.be32(stale_header + 8);
        const u32 stale_meshes = m.be16(m.be32(owner + 0x54) + 4);
        for (u32 j = 0; stale && j < stale_meshes; ++j) {
            stale_dl_bytes += m.be32(stale_header + 12 + 8 * stale_attributes + 8 * j + 4);
        }
    }
    std::snprintf(text, sizeof(text), "DL bytes sector %llu (doc 35,776), static %llu + stale 0x807E4540 %llu (doc 8,224), skinned %llu (doc 19,232); %llu triangles, %llu foreign, %llu unsupported",
                  static_cast<unsigned long long>(totals.dl_bytes["sector"]), static_cast<unsigned long long>(totals.dl_bytes["static"]),
                  static_cast<unsigned long long>(stale_dl_bytes),
                  static_cast<unsigned long long>(totals.dl_bytes["skinned"]), static_cast<unsigned long long>(totals.triangles),
                  static_cast<unsigned long long>(totals.foreign), static_cast<unsigned long long>(totals.unsupported));
    report(totals.dl_bytes["sector"] == 35776u && totals.dl_bytes["static"] + stale_dl_bytes == 8224u &&
               stale_dl_bytes == 224u && totals.dl_bytes["skinned"] == 19232u && totals.foreign == 0 && totals.unsupported == 0,
           "fh_totals_match_re02_11_2", text);

    // Mutation fuzz on a real world sector (13 meshes, POS idx16) and a real model.
    for (u32 target : {0x812FA760u, 0x807B8320u}) {
        const auto it = std::find_if(entries.begin(), entries.end(), [&](const RealEntry& e) { return e.entry == target; });
        if (it == entries.end()) {
            report(false, "fh_fuzz", "entry missing");
            continue;
        }
        u32 flags;
        u32 mesh_header;
        if (it->owner_ref_offset == 0x34) {
            flags = m.be32(sector_world[it->owner] + 8);
            mesh_header = m.be32(it->owner + 0x78);
        } else {
            flags = m.be32(it->owner + 8);
            mesh_header = m.be32(it->owner + 0x54);
        }
        const u32 fmt_pointer = it->owner_ref_offset == 0x34 ? 0u : m.be32(it->owner + geometry_fmt_offset);
        const GxVertexFormatDesc format = gekko::rw_vertex_format(fmt_pointer ? m.at(fmt_pointer) : nullptr, flags, false);
        const u32 header = it->entry + 24;
        const std::size_t header_size = 12u + 8u * m.be32(header + 8) + 8u * m.be16(mesh_header + 4);
        const u32 payload = it->owner_ref_offset == 0x34 ? it->size + 19u : it->size;
        std::vector<u8> header_copy(m.at(header), m.at(header) + header_size);
        std::vector<u8> data_copy(m.at(header), m.at(header) + payload);
        u64 accepted = 0;
        u64 runs = 0;
        std::string failure;
        try {
            runs = fuzz_buffer(header_copy, data_copy, header, format, 20000, 0xf00d0000ull + target, accepted);
        } catch (const std::exception& ex) {
            failure = ex.what();
        }
        std::snprintf(text, sizeof(text), "%08X: %llu mutated runs, %llu accepted, all accepted results consistent%s", target,
                      static_cast<unsigned long long>(runs), static_cast<unsigned long long>(accepted), failure.empty() ? "" : " NO");
        report(failure.empty() && runs == 20000u, "fh_fuzz_" + std::to_string(target == 0x812FA760u ? 0 : 1), text + (failure.empty() ? "" : " " + failure));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_mem1_path = argv[1];
    struct Case {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"every_primitive_and_index_width", test_every_primitive_and_index_width},
        {"consumed_extent_is_exact", test_consumed_extent_is_exact},
        {"short_strips_fans_and_zero_counts", test_short_strips_fans_and_zero_counts},
        {"degenerate_joins_dropped_geometric_kept", test_degenerate_joins_dropped_geometric_kept},
        {"unwelding_independent_indices", test_unwelding_independent_indices},
        {"shared_model_keeps_one_to_one", test_shared_model_keeps_one_to_one},
        {"component_decoding", test_component_decoding},
        {"matrix_index_tuples", test_matrix_index_tuples},
        {"foreign_commands_skipped_by_length", test_foreign_commands_skipped_by_length},
        {"state_commands_in_lists_refused", test_state_commands_in_lists_refused},
        {"multi_mesh_ranges_and_empty", test_multi_mesh_ranges_and_empty},
        {"malformed_inputs_fail", test_malformed_inputs_fail},
        {"spans_dynamic_expand_no_alloc", test_spans_dynamic_expand_no_alloc},
        {"synthetic_fuzz", test_synthetic_fuzz},
    };
    int failures = 0;
    int passes = 0;
    for (const Case& c : cases) {
        try {
            c.run();
            std::printf("PASS %s\n", c.name);
            ++passes;
        } catch (const std::exception& ex) {
            std::printf("FAIL %s - %s\n", c.name, ex.what());
            ++failures;
        }
    }
    try {
        run_real_data(failures, passes);
    } catch (const std::exception& ex) {
        std::printf("FAIL real_data - %s\n", ex.what());
        ++failures;
    }
    std::printf("%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
