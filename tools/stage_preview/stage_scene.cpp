#include "stage_scene.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

#include "assets/archive.h"
#include "assets/chunk_stream.h"
#include "assets/geometry_import.h"
#include "assets/texture_dictionary.h"
#include "assets/texture_import.h"

namespace stage {

namespace {

using namespace shadow;

struct LandClass {
    const char* token;
    enum Kind { Opaque, Punch, Translucent, Additive, Glare } kind;
    bool far_scenery;
};

// The game's class table, in its order (R2_MATERIAL_CENSUS.md 4.1).
const LandClass kClasses[] = {
    {"_N_", LandClass::Opaque, true},      {"_D_", LandClass::Opaque, true},       {"_OS_", LandClass::Opaque, false},
    {"_OSL_", LandClass::Opaque, false},   {"_O_", LandClass::Opaque, false},      {"_OW_", LandClass::Opaque, false},
    {"_OL_", LandClass::Opaque, false},    {"_P_", LandClass::Punch, false},       {"_AF_", LandClass::Translucent, false},
    {"_A_", LandClass::Translucent, false}, {"_AL_", LandClass::Translucent, false}, {"_K_", LandClass::Additive, false},
    {"_M_", LandClass::Additive, false},   {"_G_", LandClass::Glare, false},       {"_GL_", LandClass::Glare, false},
};

// The level recipes of the land classes (R2_MATERIAL_CENSUS.md 4.1).
render::Recipe recipe_of(const LandClass& land, bool cull) {
    render::Recipe r;
    switch (land.kind) {
    case LandClass::Opaque:
        r.alpha_cutoff = 1.0f / 255.0f;
        r.cull = cull && std::strcmp(land.token, "_OW_") != 0 ? render::Cull::Back : render::Cull::None;
        break;
    case LandClass::Punch:
        r.alpha_cutoff = 200.0f / 255.0f;
        r.cull = cull ? render::Cull::Back : render::Cull::None;
        break;
    case LandClass::Translucent:
        r.depth_write = false;
        r.cull = render::Cull::None;
        break;
    case LandClass::Additive:
        r.dst = render::Blend::One;
        r.depth_write = false;
        r.cull = render::Cull::None;
        break;
    case LandClass::Glare:
        break;
    }
    return r;
}

std::string upper(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

render::Address address_of(std::uint32_t mode) {
    return mode == 2 ? render::Address::Mirror : (mode == 3 || mode == 4) ? render::Address::Clamp : render::Address::Wrap;
}

}  // namespace

bool load(render::Renderer& renderer, const std::filesystem::path& files, const std::string& stage_name,
          const LoadOptions& options, Scene* out, std::string* error) {
    *out = Scene{};
    std::vector<std::unique_ptr<std::vector<std::uint8_t>>> keep;  // bytes the texture records point into
    std::map<std::string, assets::TextureRecord> textures;          // by upper-case name
    struct LoadedWorld {
        assets::World world;
        const LandClass* land;
    };
    std::vector<LoadedWorld> worlds;
    std::string problem;
    if (!std::filesystem::exists(files / stage_name)) {
        if (error != nullptr)
            *error = "no stage folder " + (files / stage_name).string();
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(files / stage_name)) {
        if (upper(entry.path().extension().string()) != ".ONE")
            continue;
        auto archive_bytes = std::make_unique<std::vector<std::uint8_t>>(read_file(entry.path()));
        const assets::ByteView archive_view{archive_bytes->data(), archive_bytes->size()};
        assets::OneArchive archive;
        if (!assets::parse_one_archive(archive_view, &archive, &problem))
            continue;
        std::vector<std::vector<std::uint8_t>*> member_bytes(archive.members.size(), nullptr);
        for (std::size_t m = 0; m < archive.members.size(); ++m) {
            auto bytes = std::make_unique<std::vector<std::uint8_t>>();
            if (!assets::read_one_member(archive_view, archive, archive.members[m], bytes.get(), &problem))
                continue;
            const assets::ByteView view{bytes->data(), bytes->size()};
            std::vector<assets::Chunk> top;
            if (assets::read_stream(view, &top)) {
                for (const assets::Chunk& c : top) {
                    if (c.type != assets::chunk::kTextureDictionary)
                        continue;
                    assets::TextureDictionary dictionary;
                    if (assets::parse_texture_dictionary(view, c, &dictionary, &problem))
                        for (assets::TextureRecord& t : dictionary.textures)
                            textures.emplace(upper(t.name), std::move(t));
                }
            }
            member_bytes[m] = bytes.get();
            keep.push_back(std::move(bytes));
        }
        for (const LandClass& land : kClasses) {
            for (std::size_t m = 0; m < archive.members.size(); ++m) {
                if (upper(archive.members[m].name).find(land.token) == std::string::npos)
                    continue;
                if (member_bytes[m] != nullptr) {
                    const assets::ByteView view{member_bytes[m]->data(), member_bytes[m]->size()};
                    std::vector<assets::Chunk> top;
                    if (assets::read_stream(view, &top)) {
                        for (const assets::Chunk& c : top) {
                            if (c.type != assets::chunk::kWorld)
                                continue;
                            LoadedWorld loaded;
                            loaded.land = &land;
                            if (assets::import_world(view, c, &loaded.world, &problem))
                                worlds.push_back(std::move(loaded));
                        }
                    }
                }
                break;
            }
        }
        keep.push_back(std::move(archive_bytes));
    }
    out->worlds = worlds.size();

    std::map<std::string, render::TextureHandle> texture_of;
    for (const auto& [name, record] : textures) {
        assets::TextureImage image;
        if (!assets::import_texture(record, assets::ImportOptions{}, &image, &problem))
            continue;
        const render::TextureHandle handle = renderer.create_texture(render::Scope::Stage, image, &problem);
        if (handle)
            texture_of[name] = handle;
    }
    out->textures = texture_of.size();

    struct Pending {
        Draw draw;
        LandClass::Kind kind;
    };
    std::vector<Pending> pending;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const LoadedWorld& loaded : worlds) {
        if (loaded.land->kind == LandClass::Glare)
            continue;  // glare sources belong to the glare pass
        // World sectors are unlit (the level decoder clears LIGHT for every class) and prelit: the one-stage form
        // C0 x vertex colour x texture, C0 the material colour when the world modulates by it (census 4.4).
        const bool modulate = (loaded.world.format & 0x40) != 0;
        const bool prelit = (loaded.world.format & 0x08) != 0;
        for (const assets::WorldSector& sector : loaded.world.sectors) {
            if (sector.mesh.vertex_count == 0 || sector.mesh.indices.empty())
                continue;
            const render::MeshHandle mesh = renderer.create_mesh(render::Scope::Stage, sector.mesh, &problem);
            if (!mesh)
                continue;
            if (!loaded.land->far_scenery) {
                for (std::size_t i = 0; i < sector.mesh.positions.size(); i += 3)
                    for (int k = 0; k < 3; ++k) {
                        lo[k] = std::min(lo[k], sector.mesh.positions[i + k]);
                        hi[k] = std::max(hi[k], sector.mesh.positions[i + k]);
                    }
            }
            for (std::uint32_t s = 0; s < sector.mesh.submeshes.size(); ++s) {
                const assets::MeshData::Submesh& sub = sector.mesh.submeshes[s];
                Pending p;
                p.kind = loaded.land->kind;
                p.draw.mesh = mesh;
                p.draw.submesh = s;
                p.draw.recipe = recipe_of(*loaded.land, options.cull);
                p.draw.far_scenery = loaded.land->far_scenery;
                p.draw.material.combine = render::Combine::OneStage;
                p.draw.material.vertex_colours = prelit;
                p.draw.material.modulate = modulate;
                if (sub.material < loaded.world.materials.size()) {
                    const assets::MaterialRecord& m = loaded.world.materials[sub.material];
                    p.draw.material.colour = {m.colour[0] / 255.0f, m.colour[1] / 255.0f, m.colour[2] / 255.0f,
                                              m.colour[3] / 255.0f};
                    if (m.texture.present) {
                        const auto found = texture_of.find(upper(m.texture.name));
                        if (found != texture_of.end()) {
                            p.draw.material.texture = found->second;
                            p.draw.material.sampler = {render::Filter::Anisotropic,
                                                       address_of((m.texture.filter_address >> 8) & 0xF),
                                                       address_of((m.texture.filter_address >> 12) & 0xF)};
                        } else {
                            ++out->missing_textures;
                        }
                    }
                }
                out->triangles += sub.index_count / 3;
                pending.push_back(p);
            }
        }
    }
    // The game's pass order: opaque classes, punch-through, translucent, additive.
    for (int kind = LandClass::Opaque; kind <= LandClass::Additive; ++kind)
        for (const Pending& p : pending)
            if (p.kind == kind)
                out->draws.push_back(p.draw);
    std::copy(lo, lo + 3, out->lo);
    std::copy(hi, hi + 3, out->hi);
    return true;
}

void submit(const Scene& scene, render::FrameBuilder& builder, bool include_far_scenery) {
    for (const Draw& d : scene.draws) {
        if (d.far_scenery && !include_far_scenery)
            continue;
        builder.draw(d.mesh, d.submesh, d.material, render::Float3x4{}, d.recipe);
    }
}

}  // namespace stage
