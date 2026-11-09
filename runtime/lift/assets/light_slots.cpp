#include "assets/light_slots.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace shadow::assets {

namespace {

#include "assets/light_slots_table.inc"

struct Key {
    std::uint32_t x, y, z;
    bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct KeyHash {
    std::size_t operator()(const Key& k) const {
        return (static_cast<std::size_t>(k.x) * 0x9E3779B1u) ^ (static_cast<std::size_t>(k.y) * 0x27D4EB2Du) ^ k.z;
    }
};

Key key_of(const float* p) {
    Key k;
    std::memcpy(&k.x, &p[0], 4);
    std::memcpy(&k.y, &p[1], 4);
    std::memcpy(&k.z, &p[2], 4);
    return k;
}

template <class T>
void copy_attribute(std::vector<T>& a, std::size_t per_vertex, std::uint32_t from) {
    if (a.size() >= (std::size_t{from} + 1) * per_vertex)
        for (std::size_t c = 0; c < per_vertex; ++c)
            a.push_back(a[std::size_t{from} * per_vertex + c]);
}

}  // namespace

LightSlotResult fix_light_slots(MeshData& mesh, const std::vector<bool>& glow, const LightSlotMove* moves,
                                std::size_t move_count, const LightSlotAdd* adds, std::size_t add_count) {
    LightSlotResult result;
    // The lights' vertices, by their exact position, and the submesh each is drawn in.
    std::unordered_map<Key, std::vector<std::uint32_t>, KeyHash> at;
    std::vector<std::int32_t> submesh_of(mesh.vertex_count, -1);
    for (std::size_t s = 0; s < mesh.submeshes.size(); ++s) {
        if (s >= glow.size() || !glow[s])
            continue;
        const auto& sub = mesh.submeshes[s];
        for (std::uint32_t k = sub.first_index; k < sub.first_index + sub.index_count && k < mesh.indices.size(); ++k) {
            const std::uint32_t v = mesh.indices[k];
            if (v >= mesh.vertex_count || submesh_of[v] >= 0)
                continue;
            submesh_of[v] = static_cast<std::int32_t>(s);
            at[key_of(&mesh.positions[std::size_t{v} * 3])].push_back(v);
        }
    }
    if (at.empty())
        return result;

    for (std::size_t a = 0; a < add_count; ++a) {
        const LightSlotAdd& add = adds[a];
        std::uint32_t template_vertex[8];
        bool found = add.vertex_count > 0 && add.vertex_count <= 8;
        for (std::uint32_t i = 0; found && i < add.vertex_count; ++i) {
            const auto it = at.find(key_of(add.from[i]));
            found = it != at.end();
            if (found)
                template_vertex[i] = it->second.front();
        }
        if (!found)
            continue;
        const std::uint32_t first_new = mesh.vertex_count;
        for (std::uint32_t i = 0; i < add.vertex_count; ++i) {
            const std::uint32_t t = template_vertex[i];
            mesh.positions.insert(mesh.positions.end(), add.to[i], add.to[i] + 3);
            copy_attribute(mesh.normals, 3, t);
            copy_attribute(mesh.colours, 4, t);
            copy_attribute(mesh.uv0, 2, t);
            copy_attribute(mesh.uv1, 2, t);
        }
        mesh.vertex_count += add.vertex_count;
        // The new triangles close the template's submesh; the submeshes after it move up.
        const std::size_t s = static_cast<std::size_t>(submesh_of[template_vertex[0]]);
        const std::uint32_t at_index = mesh.submeshes[s].first_index + mesh.submeshes[s].index_count;
        std::vector<std::uint32_t> triangles;
        for (std::uint32_t t = 0; t < add.triangle_count && t < 6; ++t)
            for (int c = 0; c < 3; ++c)
                triangles.push_back(first_new + add.triangles[t][c]);
        mesh.indices.insert(mesh.indices.begin() + at_index, triangles.begin(), triangles.end());
        const std::uint32_t grown = static_cast<std::uint32_t>(triangles.size());
        mesh.submeshes[s].index_count += grown;
        for (auto& other : mesh.submeshes)
            if (&other != &mesh.submeshes[s] && other.first_index >= at_index)
                other.first_index += grown;
        ++result.added_lights;
    }

    for (std::size_t m = 0; m < move_count; ++m) {
        const auto it = at.find(key_of(moves[m].from));
        if (it == at.end())
            continue;
        for (const std::uint32_t v : it->second) {
            std::memcpy(&mesh.positions[std::size_t{v} * 3], moves[m].to, sizeof(float) * 3);
            ++result.moved_vertices;
        }
    }

    if (result.added_lights != 0 || result.moved_vertices != 0) {
        // The bounds still hold every vertex.
        float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (std::size_t i = 0; i + 2 < mesh.positions.size(); i += 3)
            for (int c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], mesh.positions[i + c]);
                hi[c] = std::max(hi[c], mesh.positions[i + c]);
            }
        const float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
        float r2 = 0.0f;
        for (std::size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
            const float dx = mesh.positions[i] - centre[0], dy = mesh.positions[i + 1] - centre[1],
                        dz = mesh.positions[i + 2] - centre[2];
            r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
        }
        mesh.bounds = Sphere{centre[0], centre[1], centre[2], std::sqrt(r2)};
    }
    return result;
}

LightSlotResult fix_light_slots(MeshData& mesh, const std::vector<bool>& glow) {
    return fix_light_slots(mesh, glow, kLightSlotMoves, std::size(kLightSlotMoves), kLightSlotAdds,
                           std::size(kLightSlotAdds));
}

}  // namespace shadow::assets
