#include "assets/geometry_import.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace shadow::assets {

namespace {

bool fail(std::string* error, const std::string& what) {
    if (error != nullptr)
        *error = what;
    return false;
}

constexpr std::uint32_t kFlagPrelit = 0x08;
constexpr std::uint32_t kFlagTextured = 0x04;
constexpr std::uint32_t kFlagTextured2 = 0x80;
constexpr std::uint32_t kFlagNative = 0x01000000;

constexpr std::uint32_t kChunkMeshPlugin = 0x50E;
constexpr std::uint32_t kChunkPreparedData = 0x510;
constexpr std::uint32_t kChunkSkin = 0x116;
constexpr std::uint32_t kChunkDeltaMorph = 0x122;

std::uint32_t uv_set_count(std::uint32_t format) noexcept {
    const std::uint32_t declared = (format >> 16) & 0xFF;
    if (declared != 0)
        return declared;
    return (format & kFlagTextured2) != 0 ? 2 : ((format & kFlagTextured) != 0 ? 1 : 0);
}

// Triangles of a strip, winding kept (every other triangle swaps its first two corners); a triangle with two equal
// corners covers nothing and is dropped.
void append_strip(const std::uint32_t* strip, std::size_t count, std::vector<std::uint32_t>& out) {
    for (std::size_t i = 0; i + 2 < count; ++i) {
        std::uint32_t a = strip[i];
        std::uint32_t b = strip[i + 1];
        const std::uint32_t c = strip[i + 2];
        if ((i & 1) != 0)
            std::swap(a, b);
        if (a == b || b == c || a == c)
            continue;
        out.push_back(a);
        out.push_back(b);
        out.push_back(c);
    }
}

Sphere bounds_of(const std::vector<float>& positions) {
    Sphere s;
    if (positions.empty())
        return s;
    float lo[3] = {positions[0], positions[1], positions[2]};
    float hi[3] = {lo[0], lo[1], lo[2]};
    for (std::size_t i = 0; i < positions.size(); i += 3) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], positions[i + k]);
            hi[k] = std::max(hi[k], positions[i + k]);
        }
    }
    s.x = (lo[0] + hi[0]) * 0.5f;
    s.y = (lo[1] + hi[1]) * 0.5f;
    s.z = (lo[2] + hi[2]) * 0.5f;
    float r2 = 0;
    for (std::size_t i = 0; i < positions.size(); i += 3) {
        const float dx = positions[i] - s.x, dy = positions[i + 1] - s.y, dz = positions[i + 2] - s.z;
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    s.radius = std::sqrt(r2);
    return s;
}

bool read_floats(ByteView bytes, std::size_t& at, std::size_t end, std::size_t count, std::vector<float>& out) {
    if (at > end || count > (end - at) / 4)
        return false;
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        out[i] = load_le_float(bytes.data + at + i * 4);
    at += count * 4;
    return true;
}

bool parse_skin(ByteView bytes, const Chunk& c, std::uint32_t vertices, ModelGeometry* g, std::string* error) {
    std::size_t p = c.body();
    const std::size_t end = c.end();
    if (end - p < 4)
        return fail(error, "skin chunk too short");
    SkinData& skin = g->skin;
    skin.bone_count = bytes.data[p];
    const std::uint32_t used = bytes.data[p + 1];
    skin.max_weights = bytes.data[p + 2];
    p += 4;
    const bool old_layout = used == 0;
    const std::size_t matrix_stride = old_layout ? 68 : 64;
    const std::size_t need = used + std::size_t{vertices} * 4 + std::size_t{vertices} * 16 + skin.bone_count * matrix_stride;
    if (end - p < need)
        return fail(error, "skin chunk shorter than its vertex and bone data");
    skin.used_bones.assign(bytes.data + p, bytes.data + p + used);
    p += used;
    g->mesh.skin_indices.assign(bytes.data + p, bytes.data + p + std::size_t{vertices} * 4);
    p += std::size_t{vertices} * 4;
    read_floats(bytes, p, end, std::size_t{vertices} * 4, g->mesh.skin_weights);
    skin.inverse_bind.resize(std::size_t{skin.bone_count} * 16);
    for (std::uint32_t b = 0; b < skin.bone_count; ++b) {
        const std::size_t at = p + b * matrix_stride + (old_layout ? 4 : 0);
        for (int k = 0; k < 16; ++k)
            skin.inverse_bind[b * 16 + k] = load_le_float(bytes.data + at + k * 4);
    }
    p += skin.bone_count * matrix_stride;
    if (!old_layout && end - p >= 12) {
        skin.bone_limit = load_le32(bytes.data + p);
        skin.split_meshes = load_le32(bytes.data + p + 4);
        const std::uint32_t rle = load_le32(bytes.data + p + 8);
        p += 12;
        if (skin.split_meshes != 0)
            p += skin.bone_count + 2 * std::size_t{skin.split_meshes} + 2 * std::size_t{rle};
    }
    if (p != end)
        return fail(error, "skin chunk does not end where its data does");
    g->skinned = true;
    return true;
}

// Delta-morph plugin: u32 target count; per target u32 name length, the name, u32 flags (geometry-flag bits of what
// it carries: 0x02 positions, 0x10 normals, 0x08 prelit colours, 0x04 UVs), u32 (unused here), u32 run-length byte
// count, u32 morphed-vertex count, the run bytes (below), the arrays for the morphed vertices, then the target's
// bounding sphere.
bool parse_delta_morph(ByteView bytes, const Chunk& c, std::uint32_t vertices, ModelGeometry* g, std::string* error) {
    std::size_t p = c.body();
    const std::size_t end = c.end();
    auto need = [&](std::size_t n) { return p <= end && end - p >= n; };
    if (!need(4))
        return fail(error, "delta-morph chunk too short");
    const std::uint32_t count = load_le32(bytes.data + p);
    p += 4;
    for (std::uint32_t t = 0; t < count; ++t) {
        if (!need(4))
            return fail(error, "delta-morph target name length past the chunk");
        const std::uint32_t name_length = load_le32(bytes.data + p);
        p += 4;
        if (!need(name_length + 16))
            return fail(error, "delta-morph target header past the chunk");
        DeltaMorphTarget target;
        const auto* name = reinterpret_cast<const char*>(bytes.data + p);
        target.name.assign(name, std::find(name, name + name_length, '\0'));
        p += name_length;
        target.flags = load_le32(bytes.data + p);
        const std::uint32_t runs = load_le32(bytes.data + p + 8);
        const std::uint32_t morphed = load_le32(bytes.data + p + 12);
        p += 16;
        if (!need(runs))
            return fail(error, "delta-morph run lengths past the chunk");
        const std::uint8_t* run = bytes.data + p;
        p += runs;
        // Which vertices the morphed entries belong to. Each run byte covers a stretch of vertices in order: with
        // the high bit set its low seven bits count morphed vertices, otherwise it counts vertices left alone
        // (verified on all 2,239 targets of the disc: the morphed counts add up to the stored count and the runs to
        // the geometry's vertices).
        std::vector<std::uint32_t> targets_of;
        std::uint32_t vertex = 0;
        for (std::uint32_t r = 0; r < runs; ++r) {
            if ((run[r] & 0x80u) != 0) {
                for (std::uint32_t k = 0; k < (run[r] & 0x7Fu); ++k)
                    targets_of.push_back(vertex++);
            } else {
                vertex += run[r];
            }
        }
        if (targets_of.size() != morphed || vertex > vertices)
            return fail(error, "delta-morph run lengths do not cover the morphed vertices");
        auto read_deltas = [&](std::vector<float>& dense) -> bool {
            if (!need(std::size_t{morphed} * 12))
                return false;
            dense.assign(std::size_t{vertices} * 3, 0.0f);
            for (std::uint32_t i = 0; i < morphed; ++i)
                for (int k = 0; k < 3; ++k)
                    dense[std::size_t{targets_of[i]} * 3 + k] = load_le_float(bytes.data + p + i * 12 + k * 4);
            p += std::size_t{morphed} * 12;
            return true;
        };
        if ((target.flags & 0x02u) != 0 && !read_deltas(target.positions))
            return fail(error, "delta-morph positions past the chunk");
        if ((target.flags & 0x10u) != 0 && !read_deltas(target.normals))
            return fail(error, "delta-morph normals past the chunk");
        if ((target.flags & 0x08u) != 0) {
            if (!need(std::size_t{morphed} * 4))
                return fail(error, "delta-morph colours past the chunk");
            p += std::size_t{morphed} * 4;
        }
        if ((target.flags & 0x04u) != 0) {
            if (!need(std::size_t{morphed} * 8))
                return fail(error, "delta-morph UVs past the chunk");
            p += std::size_t{morphed} * 8;
        }
        if (!need(16))
            return fail(error, "delta-morph bounds past the chunk");
        target.bounds = Sphere{load_le_float(bytes.data + p), load_le_float(bytes.data + p + 4),
                               load_le_float(bytes.data + p + 8), load_le_float(bytes.data + p + 12)};
        p += 16;
        g->delta_targets.push_back(std::move(target));
    }
    if (p != end)
        return fail(error, "delta-morph chunk does not end where its targets do");
    g->delta_morph = true;
    return true;
}

}  // namespace

bool import_geometry(ByteView bytes, const Chunk& geometry, ModelGeometry* out, std::string* error) {
    *out = ModelGeometry{};
    std::vector<Chunk> parts;
    if (geometry.type != chunk::kGeometry || !children(bytes, geometry, &parts))
        return fail(error, "not a geometry chunk");
    const Chunk* body = find(parts, chunk::kStruct);
    if (body == nullptr || body->size < 16)
        return fail(error, "geometry without its struct");
    std::size_t p = body->body();
    const std::size_t end = body->end();
    out->format = load_le32(bytes.data + p);
    const auto triangles = static_cast<std::int32_t>(load_le32(bytes.data + p + 4));
    const auto vertices = static_cast<std::int32_t>(load_le32(bytes.data + p + 8));
    const auto morphs = static_cast<std::int32_t>(load_le32(bytes.data + p + 12));
    p += 16;
    if (triangles < 0 || vertices < 0 || morphs < 1)
        return fail(error, "geometry with negative counts or no morph target");
    if ((out->format & kFlagNative) != 0)
        return fail(error, "geometry prepared for a console pipeline (not expected on this disc)");
    if (library_version(body->stamp) < 0x34000)
        p += 12;  // surface properties of old streams
    MeshData& mesh = out->mesh;
    mesh.vertex_count = static_cast<std::uint32_t>(vertices);
    const std::size_t nv = mesh.vertex_count;
    if ((out->format & kFlagPrelit) != 0) {
        if (p > end || end - p < nv * 4)
            return fail(error, "prelit colours run past the struct");
        mesh.colours.assign(bytes.data + p, bytes.data + p + nv * 4);
        p += nv * 4;
    }
    const std::uint32_t uv_sets = uv_set_count(out->format);
    if (uv_sets > 2)
        return fail(error, "geometry with more than two UV sets");
    for (std::uint32_t s = 0; s < uv_sets; ++s) {
        if (!read_floats(bytes, p, end, nv * 2, s == 0 ? mesh.uv0 : mesh.uv1))
            return fail(error, "UV set runs past the struct");
    }
    p += static_cast<std::size_t>(triangles) * 8;  // the triangle list; the mesh plugin's strips are drawn instead
    for (std::int32_t m = 0; m < morphs; ++m) {
        if (p > end || end - p < 24)
            return fail(error, "morph target header runs past the struct");
        MeshData::MorphTarget target;
        target.bounds = Sphere{load_le_float(bytes.data + p), load_le_float(bytes.data + p + 4),
                               load_le_float(bytes.data + p + 8), load_le_float(bytes.data + p + 12)};
        const bool has_positions = load_le32(bytes.data + p + 16) != 0;
        const bool has_normals = load_le32(bytes.data + p + 20) != 0;
        p += 24;
        if (has_positions && !read_floats(bytes, p, end, nv * 3, target.positions))
            return fail(error, "morph target positions run past the struct");
        if (has_normals && !read_floats(bytes, p, end, nv * 3, target.normals))
            return fail(error, "morph target normals run past the struct");
        if (m == 0) {
            mesh.positions = std::move(target.positions);
            mesh.normals = std::move(target.normals);
            mesh.bounds = target.bounds;
        } else {
            mesh.morph_targets.push_back(std::move(target));
        }
    }
    if (p != end)
        return fail(error, "geometry struct does not end where its data does");
    if (const Chunk* materials = find(parts, chunk::kMaterialList)) {
        if (!import_material_list(bytes, *materials, &out->materials, error))
            return false;
        out->material_count = static_cast<std::uint32_t>(out->materials.size());
    }
    const Chunk* extension = find(parts, chunk::kExtension);
    std::vector<Chunk> plugins;
    if (extension == nullptr || !children(bytes, *extension, &plugins))
        return fail(error, "geometry without its extension");
    bool meshes_seen = false;
    for (const Chunk& plugin : plugins) {
        if (plugin.type == kChunkMeshPlugin) {
            std::size_t q = plugin.body();
            const std::size_t q_end = plugin.end();
            if (q_end - q < 12)
                return fail(error, "mesh plugin too short");
            const bool strips = (load_le32(bytes.data + q) & 1u) != 0;
            const std::uint32_t count = load_le32(bytes.data + q + 4);
            q += 12;
            std::vector<std::uint32_t> run;
            for (std::uint32_t k = 0; k < count; ++k) {
                if (q_end - q < 8)
                    return fail(error, "mesh plugin entry runs past the chunk");
                const std::uint32_t n = load_le32(bytes.data + q);
                const std::uint32_t material = load_le32(bytes.data + q + 4);
                q += 8;
                if ((q_end - q) / 4 < n)
                    return fail(error, "mesh plugin indices run past the chunk");
                run.resize(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    run[i] = load_le32(bytes.data + q + i * 4);
                    if (run[i] >= mesh.vertex_count)
                        return fail(error, "mesh plugin index beyond the vertices");
                }
                q += std::size_t{n} * 4;
                MeshData::Submesh sub;
                sub.first_index = static_cast<std::uint32_t>(mesh.indices.size());
                sub.material = material;
                if (strips) {
                    append_strip(run.data(), run.size(), mesh.indices);
                    ++out->source_strips;
                } else {
                    mesh.indices.insert(mesh.indices.end(), run.begin(), run.end() - run.size() % 3);
                }
                sub.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - sub.first_index;
                mesh.submeshes.push_back(sub);
            }
            if (q != q_end)
                return fail(error, "mesh plugin does not end where its data does");
            meshes_seen = true;
        } else if (plugin.type == kChunkSkin) {
            if (!parse_skin(bytes, plugin, mesh.vertex_count, out, error))
                return false;
        } else if (plugin.type == kChunkDeltaMorph) {
            if (!parse_delta_morph(bytes, plugin, mesh.vertex_count, out, error))
                return false;
        }
    }
    if (!meshes_seen)
        return fail(error, "geometry without its mesh plugin");
    return true;
}

// ---- clumps -----------------------------------------------------------------------------------------------------------

bool import_clump(ByteView bytes, const Chunk& clump, Clump* out, std::string* error) {
    *out = Clump{};
    std::vector<Chunk> parts;
    if (clump.type != chunk::kClump || !children(bytes, clump, &parts))
        return fail(error, "not a clump chunk");
    // Frame list: struct {count; per frame: rotation (9 floats: right, up, at), position (3 floats), parent index,
    // matrix flags}, then one extension per frame.
    if (const Chunk* frames = find(parts, chunk::kFrameList)) {
        std::vector<Chunk> list;
        if (!children(bytes, *frames, &list))
            return fail(error, "frame list with a broken child run");
        const Chunk* body = find(list, chunk::kStruct);
        if (body == nullptr || body->size < 4)
            return fail(error, "frame list without its struct");
        const std::uint32_t count = load_le32(bytes.data + body->body());
        if ((body->size - 4) / 56 < count)
            return fail(error, "frame list struct shorter than its frames");
        for (std::uint32_t f = 0; f < count; ++f) {
            const std::uint8_t* p = bytes.data + body->body() + 4 + std::size_t{f} * 56;
            Clump::Frame frame;
            for (int k = 0; k < 9; ++k)
                frame.rotation[k] = load_le_float(p + k * 4);
            for (int k = 0; k < 3; ++k)
                frame.position[k] = load_le_float(p + 36 + k * 4);
            frame.parent = static_cast<std::int32_t>(load_le32(p + 48));
            if (frame.parent >= static_cast<std::int32_t>(count))
                return fail(error, "frame parent beyond the frame list");
            out->frames.push_back(frame);
        }
    }
    if (const Chunk* geometries = find(parts, chunk::kGeometryList)) {
        std::vector<Chunk> list;
        if (!children(bytes, *geometries, &list))
            return fail(error, "geometry list with a broken child run");
        for (const Chunk& g : list) {
            if (g.type != chunk::kGeometry)
                continue;
            ModelGeometry geometry;
            if (!import_geometry(bytes, g, &geometry, error))
                return false;
            out->geometries.push_back(std::move(geometry));
        }
    }
    for (const Chunk& part : parts) {
        if (part.type != chunk::kAtomic)
            continue;
        std::vector<Chunk> atomic_parts;
        if (!children(bytes, part, &atomic_parts))
            return fail(error, "atomic with a broken child run");
        const Chunk* body = find(atomic_parts, chunk::kStruct);
        if (body == nullptr || body->size < 12)
            return fail(error, "atomic without its struct");
        Clump::Atomic atomic;
        atomic.frame = load_le32(bytes.data + body->body());
        atomic.geometry = load_le32(bytes.data + body->body() + 4);
        atomic.flags = load_le32(bytes.data + body->body() + 8);
        if (atomic.frame >= out->frames.size() || atomic.geometry >= out->geometries.size())
            return fail(error, "atomic names a frame or geometry the clump does not have");
        out->atomics.push_back(atomic);
    }
    return true;
}

// ---- worlds -----------------------------------------------------------------------------------------------------------


bool import_sector(ByteView bytes, const Chunk& sector, WorldSector* out, std::string* error) {
    std::vector<Chunk> parts;
    if (!children(bytes, sector, &parts))
        return fail(error, "atomic sector with a broken child run");
    const Chunk* extension = find(parts, chunk::kExtension);
    std::vector<Chunk> plugins;
    if (extension == nullptr || !children(bytes, *extension, &plugins))
        return fail(error, "atomic sector without its extension");
    const Chunk* prepared = find(plugins, kChunkPreparedData);
    if (prepared == nullptr)
        return true;  // an empty sector
    std::vector<std::uint32_t> materials;
    if (const Chunk* mesh_plugin = find(plugins, kChunkMeshPlugin)) {
        const std::size_t q = mesh_plugin->body();
        if (mesh_plugin->size < 12)
            return fail(error, "sector mesh plugin too short");
        const std::uint32_t count = load_le32(bytes.data + q + 4);
        if ((mesh_plugin->size - 12) / 8 < count)
            return fail(error, "sector mesh plugin entries run past the chunk");
        for (std::uint32_t k = 0; k < count; ++k)
            materials.push_back(load_le32(bytes.data + q + 12 + k * 8 + 4));
    }
    std::vector<Chunk> prepared_parts;
    if (!children(bytes, *prepared, &prepared_parts))
        return fail(error, "prepared sector data with a broken child run");
    const Chunk* body = find(prepared_parts, chunk::kStruct);
    if (body == nullptr || body->size < 12)
        return fail(error, "prepared sector data without its struct");
    const std::size_t s = body->body();
    const std::uint32_t platform = load_le32(bytes.data + s);
    const std::uint32_t header_size = load_le32(bytes.data + s + 4);
    const std::uint32_t data_size = load_le32(bytes.data + s + 8);
    if (platform != 6)
        return fail(error, "prepared sector data for platform " + std::to_string(platform));
    const std::size_t h = s + 12;
    const std::size_t data = h + header_size;
    if (header_size < 12 || data > body->end() || body->end() - data != data_size)
        return fail(error, "prepared sector data sizes do not add up");
    const std::uint32_t attribute_count = load_be32(bytes.data + h + 8);
    if (12 + std::size_t{attribute_count} * 8 > header_size)
        return fail(error, "prepared sector attributes run past the header");
    PreparedArrays arrays;
    arrays.attributes.resize(attribute_count);
    for (std::uint32_t i = 0; i < attribute_count; ++i) {
        const std::uint8_t* a = bytes.data + h + 12 + i * 8;
        arrays.attributes[i].offset = load_be32(a);
        arrays.attributes[i].kind = a[4];
        arrays.attributes[i].stride = a[5];
        const std::uint8_t index_type = a[6];
        if (index_type != 2 && index_type != 3)
            return fail(error, "prepared sector attribute without an index array");
        arrays.attributes[i].index_bytes = index_type == 2 ? 1 : 2;
    }
    const std::size_t mesh_count = (header_size - 12 - std::size_t{attribute_count} * 8) / 8;
    arrays.lists.resize(mesh_count);
    for (std::size_t j = 0; j < mesh_count; ++j) {
        const std::uint8_t* m = bytes.data + h + 12 + attribute_count * 8 + j * 8;
        arrays.lists[j] = {load_be32(m), load_be32(m + 4)};
    }
    if (materials.size() != mesh_count)
        return fail(error, "sector has " + std::to_string(mesh_count) + " draw lists for " +
                               std::to_string(materials.size()) + " materials");
    arrays.data = bytes.slice(data, data_size);
    return import_prepared_mesh(arrays, materials, out, error);
}


void append_strip_triangles(const std::uint32_t* strip, std::size_t count, std::vector<std::uint32_t>& out) {
    append_strip(strip, count, out);
}

Sphere bounding_sphere(const std::vector<float>& positions) {
    return bounds_of(positions);
}

bool import_prepared_mesh(const PreparedArrays& arrays, const std::vector<std::uint32_t>& materials,
                          WorldSector* out, std::string* error) {
    struct Attribute {
        std::uint32_t offset = 0;
        std::uint8_t kind = 0;
        std::uint8_t stride = 0;
        std::uint8_t index_bytes = 0;
        std::size_t extent = 0;  // bytes of the array
    };
    const std::size_t data_size = arrays.data.size;
    const std::size_t mesh_count = arrays.lists.size();
    if (arrays.attributes.size() > 4)
        return fail(error, "prepared data with more than four attributes");
    if (materials.size() != mesh_count)
        return fail(error, "prepared data with " + std::to_string(mesh_count) + " draw lists for " +
                               std::to_string(materials.size()) + " materials");
    std::vector<Attribute> attributes(arrays.attributes.size());
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        const PreparedArrays::Attribute& a = arrays.attributes[i];
        if (a.index_bytes != 1 && a.index_bytes != 2)
            return fail(error, "prepared attribute index of " + std::to_string(a.index_bytes) + " bytes");
        if (a.offset > data_size)
            return fail(error, "prepared attribute array outside the data");
        attributes[i] = {a.offset, a.kind, a.stride, a.index_bytes, 0};
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> lists(mesh_count);
    for (std::size_t j = 0; j < mesh_count; ++j)
        lists[j] = {arrays.lists[j].offset, arrays.lists[j].size};
    // Array extents: up to the next array, draw list or the end of the data.
    std::vector<std::size_t> boundaries{data_size};
    for (const Attribute& a : attributes)
        boundaries.push_back(a.offset);
    for (const auto& l : lists)
        boundaries.push_back(l.first);
    std::sort(boundaries.begin(), boundaries.end());
    int slot[4] = {-1, -1, -1, -1};  // position, colour, UV0, UV1
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        Attribute& a = attributes[i];
        const auto next = std::upper_bound(boundaries.begin(), boundaries.end(), std::size_t{a.offset});
        a.extent = (next == boundaries.end() ? data_size : *next) - a.offset;
        int which = -1;
        std::uint8_t stride = 0;
        switch (a.kind) {
        case 9: which = 0; stride = 12; break;
        case 11: which = 1; stride = 4; break;
        case 13: which = 2; stride = 8; break;
        case 14: which = 3; stride = 8; break;
        default: return fail(error, "prepared sector attribute of kind " + std::to_string(a.kind));
        }
        if (a.stride != stride)
            return fail(error, "prepared sector attribute with stride " + std::to_string(a.stride));
        slot[which] = static_cast<int>(i);
        out->attributes |= 1u << which;
    }
    if (slot[0] < 0)
        return fail(error, "prepared sector without positions");

    MeshData& mesh = out->mesh;
    std::unordered_map<std::uint64_t, std::uint32_t> vertex_of;
    std::vector<std::uint32_t> strip;
    const std::uint8_t* const base = arrays.data.data;
    auto add_vertex = [&](const std::uint32_t* index, std::uint32_t* vertex) -> bool {
        std::uint64_t key = 0;
        for (std::size_t i = 0; i < attributes.size(); ++i)
            key |= static_cast<std::uint64_t>(index[i]) << (16 * i);
        const auto found = vertex_of.find(key);
        if (found != vertex_of.end()) {
            *vertex = found->second;
            return true;
        }
        for (int which = 0; which < 4; ++which) {
            if (slot[which] < 0)
                continue;
            const Attribute& a = attributes[static_cast<std::size_t>(slot[which])];
            const std::size_t at = std::size_t{index[slot[which]]} * a.stride;
            if (at + a.stride > a.extent)
                return false;
            const std::uint8_t* src = base + a.offset + at;
            switch (which) {
            case 0:
                for (int k = 0; k < 3; ++k)
                    mesh.positions.push_back(load_be_float(src + k * 4));
                break;
            case 1: mesh.colours.insert(mesh.colours.end(), src, src + 4); break;
            case 2: mesh.uv0.push_back(load_be_float(src)); mesh.uv0.push_back(load_be_float(src + 4)); break;
            case 3: mesh.uv1.push_back(load_be_float(src)); mesh.uv1.push_back(load_be_float(src + 4)); break;
            }
        }
        *vertex = mesh.vertex_count++;
        vertex_of.emplace(key, *vertex);
        return true;
    };
    for (std::size_t j = 0; j < mesh_count; ++j) {
        MeshData::Submesh sub;
        sub.first_index = static_cast<std::uint32_t>(mesh.indices.size());
        sub.material = materials[j];
        std::size_t p = lists[j].first;
        const std::size_t list_end = std::size_t{lists[j].first} + lists[j].second;
        if (list_end > data_size)
            return fail(error, "draw list runs past the sector data");
        while (p < list_end) {
            const std::uint8_t op = base[p];
            if (op == 0) {
                ++p;
                continue;
            }
            if (op != 0x98)
                return fail(error, "draw list primitive other than a triangle strip");
            if (list_end - p < 3)
                return fail(error, "draw list header runs past the list");
            const std::uint32_t count = load_be16(base + p + 1);
            p += 3;
            strip.clear();
            for (std::uint32_t v = 0; v < count; ++v) {
                std::uint32_t index[4] = {0, 0, 0, 0};
                for (std::size_t i = 0; i < attributes.size(); ++i) {
                    if (list_end - p < attributes[i].index_bytes)
                        return fail(error, "draw list vertex runs past the list");
                    index[i] = attributes[i].index_bytes == 1 ? base[p] : load_be16(base + p);
                    p += attributes[i].index_bytes;
                }
                std::uint32_t vertex = 0;
                if (!add_vertex(index, &vertex))
                    return fail(error, "draw list index beyond its attribute array");
                strip.push_back(vertex);
            }
            append_strip(strip.data(), strip.size(), mesh.indices);
            ++out->strips;
            out->strip_vertices += count;
        }
        sub.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - sub.first_index;
        mesh.submeshes.push_back(sub);
    }
    mesh.bounds = bounds_of(mesh.positions);
    return true;
}

namespace {

// A platform-independent sector (the RP2 worlds): a 44-byte header {material window base, triangle count, vertex
// count, bounding box, collision flag, pad}, then positions, packed normals (when the world has them), prelit
// colours, UV sets and triangles; the mesh plugin lists each material's vertex indices.
bool import_independent_sector(ByteView bytes, const Chunk& sector, std::uint32_t format, WorldSector* out,
                               std::string* error) {
    std::vector<Chunk> parts;
    if (!children(bytes, sector, &parts))
        return fail(error, "atomic sector with a broken child run");
    const Chunk* body = find(parts, chunk::kStruct);
    if (body == nullptr || body->size < 44)
        return fail(error, "platform-independent sector without its header");
    std::size_t p = body->body();
    const std::size_t end = body->end();
    const auto triangles = static_cast<std::int32_t>(load_le32(bytes.data + p + 4));
    const auto vertices = static_cast<std::int32_t>(load_le32(bytes.data + p + 8));
    p += 44;
    if (triangles < 0 || vertices < 0)
        return fail(error, "platform-independent sector with negative counts");
    MeshData& mesh = out->mesh;
    const std::size_t nv = static_cast<std::size_t>(vertices);
    if (!read_floats(bytes, p, end, nv * 3, mesh.positions))
        return fail(error, "sector positions run past the header");
    out->attributes |= 1u;
    if ((format & 0x10u) != 0) {  // packed normals: three signed bytes and a pad
        if (end - p < nv * 4)
            return fail(error, "sector normals run past the header");
        mesh.normals.resize(nv * 3);
        for (std::size_t v = 0; v < nv; ++v)
            for (int k = 0; k < 3; ++k)
                mesh.normals[v * 3 + k] = static_cast<std::int8_t>(bytes.data[p + v * 4 + k]) / 128.0f;
        p += nv * 4;
    }
    if ((format & kFlagPrelit) != 0) {
        if (end - p < nv * 4)
            return fail(error, "sector colours run past the header");
        mesh.colours.assign(bytes.data + p, bytes.data + p + nv * 4);
        p += nv * 4;
        out->attributes |= 2u;
    }
    const std::uint32_t uv_sets = uv_set_count(format);
    for (std::uint32_t s = 0; s < uv_sets && s < 2; ++s) {
        if (!read_floats(bytes, p, end, nv * 2, s == 0 ? mesh.uv0 : mesh.uv1))
            return fail(error, "sector UV set runs past the header");
        out->attributes |= s == 0 ? 4u : 8u;
    }
    p += std::size_t{static_cast<std::size_t>(triangles)} * 8;
    if (p != end)
        return fail(error, "platform-independent sector does not end where its arrays do");
    mesh.vertex_count = static_cast<std::uint32_t>(nv);
    const Chunk* extension = find(parts, chunk::kExtension);
    std::vector<Chunk> plugins;
    if (extension == nullptr || !children(bytes, *extension, &plugins))
        return fail(error, "platform-independent sector without its extension");
    const Chunk* mesh_plugin = find(plugins, kChunkMeshPlugin);
    if (mesh_plugin == nullptr)
        return fail(error, "platform-independent sector without its mesh plugin");
    std::size_t q = mesh_plugin->body();
    const std::size_t q_end = mesh_plugin->end();
    if (q_end - q < 12)
        return fail(error, "sector mesh plugin too short");
    const bool strips = (load_le32(bytes.data + q) & 1u) != 0;
    const std::uint32_t count = load_le32(bytes.data + q + 4);
    q += 12;
    std::vector<std::uint32_t> run;
    for (std::uint32_t k = 0; k < count; ++k) {
        if (q_end - q < 8)
            return fail(error, "sector mesh plugin entry runs past the chunk");
        const std::uint32_t n = load_le32(bytes.data + q);
        MeshData::Submesh sub;
        sub.material = load_le32(bytes.data + q + 4);
        q += 8;
        if ((q_end - q) / 4 < n)
            return fail(error, "sector mesh plugin indices run past the chunk");
        run.resize(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            run[i] = load_le32(bytes.data + q + i * 4);
            if (run[i] >= mesh.vertex_count)
                return fail(error, "sector mesh plugin index beyond the vertices");
        }
        q += std::size_t{n} * 4;
        sub.first_index = static_cast<std::uint32_t>(mesh.indices.size());
        if (strips) {
            append_strip(run.data(), run.size(), mesh.indices);
            ++out->strips;
            out->strip_vertices += n;
        } else {
            mesh.indices.insert(mesh.indices.end(), run.begin(), run.end() - run.size() % 3);
        }
        sub.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - sub.first_index;
        mesh.submeshes.push_back(sub);
    }
    if (q != q_end)
        return fail(error, "sector mesh plugin does not end where its data does");
    mesh.bounds = bounds_of(mesh.positions);
    return true;
}

bool walk_sectors(ByteView bytes, const Chunk& node, World* out, std::string* error, int depth) {
    if (depth > 64)
        return fail(error, "sector tree too deep");
    if (node.type == chunk::kAtomicSector) {
        WorldSector sector;
        const bool prepared = (out->format & kFlagNative) != 0;
        if (prepared ? !import_sector(bytes, node, &sector, error)
                     : !import_independent_sector(bytes, node, out->format, &sector, error))
            return false;
        out->sectors.push_back(std::move(sector));
        return true;
    }
    if (node.type == chunk::kPlaneSector) {
        ++out->plane_sectors;
        std::vector<Chunk> kids;
        if (!children(bytes, node, &kids))
            return fail(error, "plane sector with a broken child run");
        for (const Chunk& kid : kids) {
            if ((kid.type == chunk::kAtomicSector || kid.type == chunk::kPlaneSector) &&
                !walk_sectors(bytes, kid, out, error, depth + 1))
                return false;
        }
    }
    return true;
}

}  // namespace

bool import_world(ByteView bytes, const Chunk& world, World* out, std::string* error) {
    *out = World{};
    std::vector<Chunk> parts;
    if (world.type != chunk::kWorld || !children(bytes, world, &parts))
        return fail(error, "not a world chunk");
    const Chunk* body = find(parts, chunk::kStruct);
    if (body == nullptr || body->size < 40)
        return fail(error, "world without its struct");
    out->format = load_le32(bytes.data + body->body() + 36);
    if (const Chunk* materials = find(parts, chunk::kMaterialList)) {
        if (!import_material_list(bytes, *materials, &out->materials, error))
            return false;
        out->material_count = static_cast<std::uint32_t>(out->materials.size());
    }
    for (const Chunk& part : parts) {
        if ((part.type == chunk::kAtomicSector || part.type == chunk::kPlaneSector) &&
            !walk_sectors(bytes, part, out, error, 0))
            return false;
    }
    return true;
}

}  // namespace shadow::assets
