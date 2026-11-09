// model_preview - draws a model (a clump) with the new renderer's material program, straight from the disc
// (docs/lift-20261006/R2_DRIVER.md 2.2; r2/SITES_PIPELINES.md 4.3-4.5: the decision table, the effects base pass,
// environment and dual passes, lights). Skinned models are drawn in their bind pose.
//
//   model_preview <files directory> <archive, e.g. character/shadow.one> <out.png> [--member NAME]... [--warp]
//                 [--width W] [--height H] [--yaw DEGREES] [--pitch DEGREES] [--no-lights]

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/archive.h"
#include "assets/chunk_stream.h"
#include "assets/geometry_import.h"
#include "assets/texture_dictionary.h"
#include "assets/texture_import.h"
#include "render/frame.h"
#include "render/renderer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace shadow;

namespace {

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

render::Sampler sampler_of(const assets::TextureReference& t) {
    return {render::Filter::Anisotropic, address_of((t.filter_address >> 8) & 0xF), address_of((t.filter_address >> 12) & 0xF)};
}

// RW 3.7 frame (rows right, up, at, position; row vectors) to the column-vector 3x4.
render::Float3x4 local_of(const assets::Clump::Frame& f) {
    render::Float3x4 m;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            m.m[r][c] = f.rotation[c * 3 + r];
    for (int r = 0; r < 3; ++r)
        m.m[r][3] = f.position[r];
    return m;
}

render::Float3x4 compose(const render::Float3x4& a, const render::Float3x4& b) {
    render::Float3x4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = j == 3 ? a.m[i][3] : 0.0f;
            for (int k = 0; k < 3; ++k)
                s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: model_preview <files dir> <archive.one> <out.png> [--member NAME]... [--warp] "
                             "[--width W] [--height H] [--yaw D] [--pitch D] [--no-lights]\n");
        return 2;
    }
    const std::filesystem::path files = std::filesystem::u8path(argv[1]);
    const std::string archive_name = argv[2];
    const std::string out_png = argv[3];
    std::vector<std::string> wanted;
    bool warp = false, lights_on = true;
    std::uint32_t width = 1280, height = 1280;
    float yaw = 30.0f, pitch = 10.0f;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--member" && i + 1 < argc) wanted.push_back(upper(argv[++i]));
        else if (a == "--warp") warp = true;
        else if (a == "--no-lights") lights_on = false;
        else if (a == "--width" && i + 1 < argc) width = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        else if (a == "--height" && i + 1 < argc) height = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        else if (a == "--yaw" && i + 1 < argc) yaw = std::stof(argv[++i]);
        else if (a == "--pitch" && i + 1 < argc) pitch = std::stof(argv[++i]);
    }

    std::vector<std::uint8_t> archive_bytes = read_file(files / archive_name);
    const assets::ByteView archive_view{archive_bytes.data(), archive_bytes.size()};
    assets::OneArchive archive;
    std::string error;
    if (!assets::parse_one_archive(archive_view, &archive, &error)) {
        std::fprintf(stderr, "%s: %s\n", archive_name.c_str(), error.c_str());
        return 1;
    }
    std::vector<std::unique_ptr<std::vector<std::uint8_t>>> keep;
    std::map<std::string, assets::TextureRecord> textures;
    std::vector<std::pair<std::string, assets::Clump>> clumps;
    for (const assets::OneMember& member : archive.members) {
        auto bytes = std::make_unique<std::vector<std::uint8_t>>();
        if (!assets::read_one_member(archive_view, archive, member, bytes.get(), &error))
            continue;
        const assets::ByteView view{bytes->data(), bytes->size()};
        std::vector<assets::Chunk> top;
        if (!assets::read_stream(view, &top))
            continue;
        const std::string name = upper(member.name);
        const bool chosen = wanted.empty() ? clumps.empty() : std::find(wanted.begin(), wanted.end(), name) != wanted.end();
        for (const assets::Chunk& c : top) {
            if (c.type == assets::chunk::kTextureDictionary) {
                assets::TextureDictionary dictionary;
                if (assets::parse_texture_dictionary(view, c, &dictionary, &error))
                    for (assets::TextureRecord& t : dictionary.textures)
                        textures.emplace(upper(t.name), std::move(t));
            } else if (c.type == assets::chunk::kClump && chosen) {
                assets::Clump clump;
                if (assets::import_clump(view, c, &clump, &error))
                    clumps.emplace_back(name, std::move(clump));
                else
                    std::printf("  %s: %s\n", name.c_str(), error.c_str());
            }
        }
        keep.push_back(std::move(bytes));
    }
    if (clumps.empty()) {
        std::fprintf(stderr, "no clump chosen in %s\n", archive_name.c_str());
        return 1;
    }

    render::Renderer renderer;
    render::RendererOptions options;
    options.device.adapter = warp ? render::DeviceOptions::Adapter::Warp : render::DeviceOptions::Adapter::HighPerformance;
    options.device.debug_layer = warp;
    if (!renderer.create(options, &error)) {
        std::fprintf(stderr, "renderer: %s\n", error.c_str());
        return 1;
    }
    std::map<std::string, render::TextureHandle> texture_of;
    for (const auto& [name, record] : textures) {
        assets::TextureImage image;
        if (assets::import_texture(record, assets::ImportOptions{}, &image, &error)) {
            const render::TextureHandle handle = renderer.create_texture(render::Scope::Stage, image, &error);
            if (handle)
                texture_of[name] = handle;
        }
    }
    auto texture = [&](const assets::TextureReference& t) {
        if (!t.present)
            return render::TextureHandle{};
        const auto found = texture_of.find(upper(t.name));
        return found != texture_of.end() ? found->second : render::TextureHandle{};
    };

    // The light set: one ambient and one directional light, as a stage light record gives them.
    render::LightSet lights;
    lights.ambient = {0.35f, 0.35f, 0.38f};
    lights.directional_count = 1;
    lights.directional_colour[0] = {0.85f, 0.85f, 0.8f};
    lights.directional_vector[0] = {-0.4f, -0.6f, -0.7f};

    render::FrameBuilder builder;
    builder.begin_frame(1, render::Float4{0.25f, 0.27f, 0.32f, 1.0f});
    struct Pending {
        render::MeshHandle mesh;
        std::uint32_t submesh;
        render::Material material;
        render::Float3x4 world;
    };
    std::vector<Pending> pending;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    std::size_t effects = 0, environment = 0, dual = 0, two_stage = 0, lit = 0;
    for (const auto& [name, clump] : clumps) {
        std::vector<render::Float3x4> ltm(clump.frames.size());
        for (std::size_t f = 0; f < clump.frames.size(); ++f) {
            const render::Float3x4 local = local_of(clump.frames[f]);
            ltm[f] = clump.frames[f].parent >= 0 ? compose(ltm[static_cast<std::size_t>(clump.frames[f].parent)], local)
                                                 : local;
        }
        for (const assets::Clump::Atomic& atomic : clump.atomics) {
            const assets::ModelGeometry& g = clump.geometries[atomic.geometry];
            const render::MeshHandle mesh = renderer.create_mesh(render::Scope::Stage, g.mesh, &error);
            if (!mesh)
                continue;
            const render::Float3x4& world = ltm[atomic.frame];
            for (std::size_t i = 0; i < g.mesh.positions.size(); i += 3) {
                for (int r = 0; r < 3; ++r) {
                    const float p = world.m[r][0] * g.mesh.positions[i] + world.m[r][1] * g.mesh.positions[i + 1] +
                                    world.m[r][2] * g.mesh.positions[i + 2] + world.m[r][3];
                    lo[r] = std::min(lo[r], p);
                    hi[r] = std::max(hi[r], p);
                }
            }
            const std::uint32_t flags = g.format & 0xFF;
            bool effects_pipeline = false;
            for (const assets::MaterialRecord& m : g.materials)
                effects_pipeline = effects_pipeline || m.effect != assets::EffectKind::None;
            for (std::uint32_t s = 0; s < g.mesh.submeshes.size(); ++s) {
                const std::uint32_t material_index = g.mesh.submeshes[s].material;
                if (material_index >= g.materials.size())
                    continue;
                const assets::MaterialRecord& m = g.materials[material_index];
                render::Material material;
                material.colour = {m.colour[0] / 255.0f, m.colour[1] / 255.0f, m.colour[2] / 255.0f, m.colour[3] / 255.0f};
                material.texture = (flags & 0x84) != 0 ? texture(m.texture) : render::TextureHandle{};
                material.sampler = sampler_of(m.texture);
                material.vertex_colours = (flags & 0x08) != 0;  // P
                material.modulate = (flags & 0x40) != 0;        // M
                material.lit = lights_on && (flags & 0x20) != 0; // L
                material.ambient = lights_on;                    // X
                material.ka = m.ambient;
                const bool textured = static_cast<bool>(material.texture);
                // The decision table (SITES_PIPELINES.md 4.3): A (T, P, X) and C (!T) are two-stage, B one-stage;
                // the effects pipeline's base pass is one-stage (4.4).
                material.combine = effects_pipeline ? render::Combine::OneStage
                                   : (!textured || (material.vertex_colours && material.ambient)) ? render::Combine::TwoStage
                                                                                                    : render::Combine::OneStage;
                for (std::uint32_t k = 0; k < m.slot_count; ++k) {
                    const assets::EffectSlot& slot = m.slots[k];
                    if (slot.type == 2 && slot.texture.present) {  // environment: the loaders scale it by 0.45
                        material.extra.kind = render::ExtraPass::Kind::Environment;
                        material.extra.texture = texture(slot.texture);
                        material.extra.sampler = sampler_of(slot.texture);
                        material.extra.coefficient = std::min(1.0f, slot.coefficient * 0.45f);
                        ++environment;
                    } else if (slot.type == 4 && slot.texture.present) {
                        material.extra.kind = render::ExtraPass::Kind::Dual;
                        material.extra.texture = texture(slot.texture);
                        material.extra.sampler = sampler_of(slot.texture);
                        material.extra.src = static_cast<render::Blend>(std::max<std::uint32_t>(slot.src_blend, 1) - 1);
                        material.extra.dst = static_cast<render::Blend>(std::max<std::uint32_t>(slot.dst_blend, 1) - 1);
                        material.extra.second_uv_set = !g.mesh.uv1.empty();
                        ++dual;
                    }
                }
                if (m.effect != assets::EffectKind::None)
                    ++effects;
                if (material.combine == render::Combine::TwoStage)
                    ++two_stage;
                if (material.lit)
                    ++lit;
                pending.push_back(Pending{mesh, s, material, world});
            }
        }
    }

    const float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    const float radius = 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                          (hi[2] - lo[2]) * (hi[2] - lo[2]));
    const float yaw_r = yaw * 3.14159265f / 180.0f, pitch_r = pitch * 3.14159265f / 180.0f;
    const float distance = radius * 2.6f;
    render::Camera camera;
    camera.frame = render::look_at(render::Float3{centre[0] + distance * std::sin(yaw_r) * std::cos(pitch_r),
                                                  centre[1] + distance * std::sin(pitch_r),
                                                  centre[2] + distance * std::cos(yaw_r) * std::cos(pitch_r)},
                                   render::Float3{centre[0], centre[1], centre[2]}, render::Float3{0, 1, 0});
    camera.view_window_y = std::tan(0.35f);
    camera.near_plane = radius * 0.05f;
    camera.far_plane = radius * 20.0f;
    const render::ViewId view = builder.view(camera, render::ViewRect{});
    const render::LightSetId light_set = builder.lights(lights);
    render::Clear clear;
    clear.colour = true;
    clear.depth = true;
    builder.begin_pass(view, render::PassKind::Scene, clear);
    render::Recipe recipe;  // the Opeq level: blended with a cut-off at 1, Z test and write, back faces culled
    recipe.alpha_cutoff = 1.0f / 255.0f;
    for (const Pending& p : pending)
        builder.draw(p.mesh, p.submesh, p.material, p.world, recipe, light_set);
    builder.end_pass();
    std::vector<std::uint8_t> pixels;
    if (!renderer.render_to_image(builder.finish(), width, height, &pixels, &error)) {
        std::fprintf(stderr, "render: %s\n", error.c_str());
        return 1;
    }
    std::printf("%zu clump(s), %zu draws (%zu two-stage, %zu lit, %zu with effects: %zu environment, %zu dual), "
                "%llu GPU draws, %zu textures\n", clumps.size(), pending.size(), two_stage, lit, effects, environment,
                dual, static_cast<unsigned long long>(renderer.stats().draws), texture_of.size());
    for (const std::string& message : renderer.take_debug_errors())
        std::printf("debug layer: %s\n", message.c_str());
    if (stbi_write_png(out_png.c_str(), static_cast<int>(width), static_cast<int>(height), 4, pixels.data(),
                       static_cast<int>(width * 4)) == 0)
        return 1;
    std::printf("wrote %s\n", out_png.c_str());
    return 0;
}
