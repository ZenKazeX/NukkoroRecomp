// SCAFFOLDING (R2): material/texture flicker probe over the translated RenderWare draw stream - deleted by R6: the scene graph's lift owns materials and their animation clocks
// Diagnostics: draws whose material inputs flicker. Contract: material_probe.h.

#include "material_probe.h"

#include "gekko/cpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gekko::scaffold {

namespace render = shadow::render;

namespace {

const char* const kFieldNames[MaterialProbe::kFieldCount] = {
    "texture", "sampler", "uv", "colour", "program", "extra-kind", "extra-texture", "extra-uv", "extra-blend",
    "blend", "depth", "cull", "fog", "cutoff", "pass-kind"};

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

std::uint64_t q(float f, float step) {
    if (!std::isfinite(f))
        return 0xFFFFFFFFFFFFFFFFull;
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(static_cast<double>(f) / step)));
}

std::uint64_t matrix_hash(const float m[2][3]) {
    std::uint64_t h = 0;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            h = mix(h, q(m[r][c], 1.0e-4f));
    return h;
}

std::uint64_t sampler_bits(const render::Sampler& s) {
    return (static_cast<std::uint64_t>(s.filter) << 16) | (static_cast<std::uint64_t>(s.u) << 8) |
           static_cast<std::uint64_t>(s.v);
}

}  // namespace

MaterialProbe::MaterialProbe() {
    if (const char* path = std::getenv("GEKKO_MATERIAL_PROBE"))
        path_ = path;
}

MaterialProbe::~MaterialProbe() {
    if (enabled())
        write_report();
    if (texanim_log_ != nullptr)
        std::fclose(texanim_log_);
    if (series_ != nullptr)
        std::fclose(series_);
}

void MaterialProbe::install(Cpu& cpu) {
    if (!enabled())
        return;
    if (const char* trace = std::getenv("GEKKO_TEXANIM_TRACE"))
        texanim_trace_ = std::string(",") + trace + ",";
    texanim_log_ = std::fopen((path_ + ".texanim.txt").c_str(), "w");
    // 0x8004F1C4(set, i): one texture-pattern animation's frame choice (0x8004F0FC) and RpMaterialSetTexture.
    cpu.install_hle(0x8004F1C4u, [this](Cpu& c) {
        texanim(c);
        return false;
    }, "material probe: texture pattern animation (observer)");
}

void MaterialProbe::texanim(Cpu& c) {
    if (texanim_log_ == nullptr)
        return;
    const auto f32 = [&c](u32 a) {
        const u32 bits = c.mem_read32(a);
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    };
    const u32 set = c.gpr[3];
    const u32 anim = set + 0x44u + c.gpr[4] * 0x14u;
    const u32 def = c.mem_read32(anim);
    if (def < 0x80000000u || def >= 0x81800000u)
        return;
    char name[33]{};
    for (u32 i = 0; i < 32u; ++i) {
        name[i] = static_cast<char>(c.mem_read8(def + 0x24u + i));
        if (name[i] == '\0')
            break;
    }
    const s32 period = static_cast<s32>(c.mem_read32(def));
    const s32 keys = static_cast<s32>(c.mem_read32(def + 0x48u));
    auto [seen, first] = texanim_seen_.try_emplace(def, false);
    if (first) {
        std::fprintf(texanim_log_, "def %08X %s period %d keys %d:", def, name, period, keys);
        for (s32 k = 0; k < keys && k < 64; ++k)
            std::fprintf(texanim_log_, " %d->%d", static_cast<s32>(c.mem_read32(def + 0x4Cu + 8u * k)),
                         static_cast<s32>(c.mem_read32(def + 0x50u + 8u * k)));
        std::fprintf(texanim_log_, "\n");
    }
    if (texanim_trace_.find(std::string(",") + name + ",") != std::string::npos) {
        const float global = f32(c.gpr[13] - 0x7AA0u);
        const float own = f32(set + 0x34u);
        std::fprintf(texanim_log_, "t %llu %s global %.6f own %.6f current %d set %08X material %08X\n",
                     static_cast<unsigned long long>(c.game_clock().now()), name, global, own,
                     static_cast<s32>(c.mem_read32(anim + 0x10u)), set, c.mem_read32(anim + 4u));
    }
}

void MaterialProbe::frame(const render::FramePacket& packet) {
    if (!enabled())
        return;
    ++frame_;
    // The particle series (<report>.particles.txt): every frame, each particle texture's batches and particles in the
    // scene and glare-source passes - an effect whose emission races after a load and decays shows as a burst.
    if (series_ == nullptr)
        series_ = std::fopen((path_ + ".particles.txt").c_str(), "w");
    if (series_ != nullptr) {
        std::map<std::pair<std::uint32_t, int>, std::pair<std::uint32_t, std::uint32_t>> per;  // (texture, glare)
        for (const render::PassRecord& pass : packet.passes) {
            const int glare = pass.kind == render::PassKind::GlareSources ? 1 : 0;
            for (std::uint32_t d = pass.first_draw; d < pass.first_draw + pass.draw_count && d < packet.draws.size();
                 ++d) {
                const render::DrawRecord& draw = packet.draws[d];
                if (draw.particles >= packet.particle_batches.size())
                    continue;
                const render::ParticleBatch& b = packet.particle_batches[draw.particles];
                auto& slot = per[{b.texture.value, glare}];
                ++slot.first;
                slot.second += b.count;
            }
        }
        std::fprintf(series_, "%llu", static_cast<unsigned long long>(frame_));
        // The cameras of the first view's scene and glare-source passes: their far plane and a hash of every bit of
        // the camera (a glare-source draw tests against the depth a scene pass wrote with its camera).
        for (const render::PassRecord& pass : packet.passes) {
            if (pass.view != 0u || (pass.kind != render::PassKind::Scene && pass.kind != render::PassKind::GlareSources))
                continue;
            const render::Camera& cam = pass.has_camera ? pass.camera
                                        : (!packet.views.empty() ? packet.views.front().camera : render::Camera{});
            std::uint64_t h = 1469598103934665603ull;
            const auto* bytes = reinterpret_cast<const unsigned char*>(&cam.frame);
            for (std::size_t i = 0; i < sizeof(cam.frame); ++i)
                h = (h ^ bytes[i]) * 1099511628211ull;
            std::uint32_t far_bits, near_bits;
            std::memcpy(&far_bits, &cam.far_plane, 4);
            std::memcpy(&near_bits, &cam.near_plane, 4);
            h = (h ^ far_bits) * 1099511628211ull;
            h = (h ^ near_bits) * 1099511628211ull;
            std::fprintf(series_, " %s[%u]far=%.4f/%08llX", pass.kind == render::PassKind::Scene ? "SCENE" : "GLARE",
                         pass.draw_count, static_cast<double>(cam.far_plane), static_cast<unsigned long long>(h & 0xFFFFFFFFull));
        }
        for (const auto& [key, v] : per) {
            // Handles follow creation order and differ between builds: the texture's name is the stable key.
            const auto name = names_.find(key.first);
            std::fprintf(series_, " %s%s:%u/%u", key.second != 0 ? "g" : "s",
                         name != names_.end() ? name->second.c_str() : "?", v.first, v.second);
        }
        std::fprintf(series_, "\n");
    }
    // Shells off their model: an overlay (depth tested, no depth write) placed with the rotation of a depth-writing
    // draw but a translation a little apart - an effect following its model a frame behind. Exactly on it, the two
    // depths are bit-identical; a little apart, the faces they share tie by rounding (RENDER_FLICKER.md "Fourth bug").
    {
        std::vector<const render::DrawRecord*> writers;
        for (const render::DrawRecord& draw : packet.draws)
            if (draw.mesh && draw.particles == render::kNoParticles && draw.immediate == render::kNoImmediate &&
                draw.recipe.depth_write)
                writers.push_back(&draw);
        for (const render::DrawRecord& draw : packet.draws) {
            if (!draw.mesh || draw.particles != render::kNoParticles || draw.immediate != render::kNoImmediate ||
                !draw.recipe.depth_test || draw.recipe.depth_write)
                continue;
            for (const render::DrawRecord* w : writers) {
                bool same_rotation = true;
                for (int r = 0; r < 3 && same_rotation; ++r)
                    for (int c = 0; c < 3 && same_rotation; ++c)
                        same_rotation = std::fabs(draw.world.m[r][c] - w->world.m[r][c]) < 1.0e-4f;
                if (!same_rotation)
                    continue;
                const float dx = draw.world.m[0][3] - w->world.m[0][3], dy = draw.world.m[1][3] - w->world.m[1][3],
                            dz = draw.world.m[2][3] - w->world.m[2][3];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (!(d < 2.0f))
                    continue;
                Shell& s = shells_[{draw.material.texture.value, w->material.texture.value}];
                if (d == 0.0f) {
                    ++s.exact;
                } else {
                    if (s.off == 0u)
                        s.first_frame = frame_;
                    ++s.off;
                    s.max_offset = std::max(s.max_offset, d);
                    s.last_frame = frame_;
                    s.position[0] = draw.world.m[0][3];
                    s.position[1] = draw.world.m[1][3];
                    s.position[2] = draw.world.m[2][3];
                }
                break;
            }
        }
    }
    // The n-th draw of the same mesh, submesh and place in this frame is its own draw (an object drawing itself
    // twice - a glare source, an effect layer - must not read as one draw changing).
    std::unordered_map<std::uint64_t, std::uint32_t> occurrence;
    for (const render::PassRecord& pass : packet.passes) {
        for (std::uint32_t d = pass.first_draw; d < pass.first_draw + pass.draw_count && d < packet.draws.size(); ++d) {
            const render::DrawRecord& draw = packet.draws[d];
            if (!draw.mesh || draw.particles != render::kNoParticles || draw.immediate != render::kNoImmediate)
                continue;
            const float pos[3] = {draw.world.m[0][3], draw.world.m[1][3], draw.world.m[2][3]};
            std::uint64_t key = mix(mix(draw.mesh.value, draw.submesh), q(pos[0], 1.0f));
            key = mix(mix(key, q(pos[1], 1.0f)), q(pos[2], 1.0f));
            key = mix(key, occurrence[key]++);
            Track& t = tracks_[key];
            if (t.frames != 0u && t.last_frame == frame_)
                continue;
            const render::Material& m = draw.material;
            const render::Recipe& r = draw.recipe;
            Values now;
            now.v[kTexture] = m.texture.value;
            now.v[kSampler] = sampler_bits(m.sampler);
            now.v[kUv] = matrix_hash(m.uv_transform);
            now.v[kColour] = mix(mix(q(m.colour.x, 1.0f / 255), q(m.colour.y, 1.0f / 255)),
                                 mix(q(m.colour.z, 1.0f / 255), q(m.colour.w, 1.0f / 255)));
            now.v[kProgram] = (static_cast<std::uint64_t>(m.combine) << 8) | (m.vertex_colours ? 1u : 0u) |
                              (m.modulate ? 2u : 0u) | (m.lit ? 4u : 0u) | (m.ambient ? 16u : 0u) |
                              (q(m.ka, 1.0e-3f) << 16);
            now.v[kExtraKind] = static_cast<std::uint64_t>(m.extra.kind) | (q(m.extra.coefficient, 1.0e-3f) << 8);
            now.v[kExtraTexture] = (static_cast<std::uint64_t>(m.extra.texture.value) << 32) | sampler_bits(m.extra.sampler);
            now.v[kExtraUv] = matrix_hash(m.extra.uv_transform) ^ (m.extra.second_uv_set ? 1u : 0u);
            now.v[kExtraBlend] = (static_cast<std::uint64_t>(m.extra.src) << 8) | static_cast<std::uint64_t>(m.extra.dst) |
                                 (q(m.extra.alpha_cutoff, 1.0e-3f) << 16);
            now.v[kBlend] = (static_cast<std::uint64_t>(r.src) << 8) | static_cast<std::uint64_t>(r.dst);
            now.v[kDepth] = (r.depth_test ? 1u : 0u) | (r.depth_write ? 2u : 0u);
            now.v[kCull] = static_cast<std::uint64_t>(r.cull);
            now.v[kFog] = r.fog ? mix(mix(q(r.fog_start, 1.0f), q(r.fog_end, 1.0f)),
                                      mix(q(r.fog_colour.x, 1.0f / 255), q(r.fog_colour.y, 1.0f / 255)))
                                : 0u;
            now.v[kCutoff] = q(r.alpha_cutoff, 1.0e-3f);
            now.v[kPassKind] = static_cast<std::uint64_t>(pass.kind);
            if (t.frames == 0u) {
                t.first_frame = frame_;
                std::memcpy(t.position, pos, sizeof(pos));
                t.mesh = draw.mesh.value;
                t.submesh = draw.submesh;
            }
            const bool consecutive = t.have_last && t.last_frame + 1u == frame_;
            for (int f = 0; f < kFieldCount; ++f) {
                if (!consecutive)
                    continue;
                if (now.v[f] != t.last.v[f]) {
                    ++t.changes[f];
                    if (t.have_prev && now.v[f] == t.prev.v[f]) {
                        if (t.toggles[f]++ == 0u) {
                            t.toggle_a[f] = now.v[f];
                            t.toggle_b[f] = t.last.v[f];
                        }
                    }
                }
            }
            t.prev = t.last;
            t.have_prev = consecutive && t.have_last;
            t.last = now;
            t.have_last = true;
            t.last_frame = frame_;
            ++t.frames;
        }
    }
}

void MaterialProbe::write_report() const {
    std::FILE* out = std::fopen(path_.c_str(), "w");
    if (out == nullptr)
        return;
    std::array<std::uint64_t, kFieldCount> toggles_by_field{}, draws_by_field{};
    std::vector<std::pair<std::uint64_t, const Track*>> ranked;
    for (const auto& [key, t] : tracks_) {
        std::uint64_t total = 0;
        for (int f = 0; f < kFieldCount; ++f) {
            total += t.toggles[f];
            toggles_by_field[f] += t.toggles[f];
            if (t.toggles[f] != 0u)
                ++draws_by_field[f];
        }
        if (total != 0u)
            ranked.emplace_back(total, &t);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(out, "material probe: %llu frames, %zu draws tracked, %zu with A-B-A toggles\n",
                 static_cast<unsigned long long>(frame_), tracks_.size(), ranked.size());
    std::fprintf(out, "toggles by field (draws affected):");
    for (int f = 0; f < kFieldCount; ++f)
        if (toggles_by_field[f] != 0u)
            std::fprintf(out, " %s %llu (%llu)", kFieldNames[f], static_cast<unsigned long long>(toggles_by_field[f]),
                         static_cast<unsigned long long>(draws_by_field[f]));
    std::fprintf(out, "\n\n");
    for (std::size_t i = 0; i < ranked.size() && i < 80u; ++i) {
        const Track& t = *ranked[i].second;
        std::fprintf(out, "%llu toggles  mesh %08X sub %u at (%.0f %.0f %.0f) frames %llu..%llu (%llu drawn):",
                     static_cast<unsigned long long>(ranked[i].first), t.mesh, t.submesh, t.position[0], t.position[1],
                     t.position[2], static_cast<unsigned long long>(t.first_frame),
                     static_cast<unsigned long long>(t.last_frame), static_cast<unsigned long long>(t.frames));
        for (int f = 0; f < kFieldCount; ++f) {
            if (t.toggles[f] == 0u)
                continue;
            std::fprintf(out, " %s %u/%u [%llX <-> %llX]", kFieldNames[f], t.toggles[f], t.changes[f],
                         static_cast<unsigned long long>(t.toggle_a[f]),
                         static_cast<unsigned long long>(t.toggle_b[f]));
            if (f == kTexture) {
                const auto a = names_.find(static_cast<std::uint32_t>(t.toggle_a[f]));
                const auto b = names_.find(static_cast<std::uint32_t>(t.toggle_b[f]));
                std::fprintf(out, " (%s <-> %s)", a != names_.end() ? a->second.c_str() : "?",
                             b != names_.end() ? b->second.c_str() : "?");
            }
        }
        std::fprintf(out, "\n");
    }
    // Shells: overlays drawn on a model of the same rotation. "apart" counts frames the overlay was placed off the
    // model (0 < offset < 2 units): there the faces they share tie by rounding - the shell must be placed where its
    // model is, not compared loosely.
    std::fprintf(out, "\nshells (overlay texture over model texture: frames exact / apart, largest offset, last place):\n");
    for (const auto& [key, s] : shells_) {
        const auto a = names_.find(key.first);
        const auto b = names_.find(key.second);
        std::fprintf(out, "%s over %s: %llu exact / %llu apart, max %.3f units, frames %llu..%llu at (%.0f %.0f %.0f)\n",
                     a != names_.end() ? a->second.c_str() : "?", b != names_.end() ? b->second.c_str() : "?",
                     static_cast<unsigned long long>(s.exact), static_cast<unsigned long long>(s.off),
                     static_cast<double>(s.max_offset), static_cast<unsigned long long>(s.first_frame),
                     static_cast<unsigned long long>(s.last_frame), s.position[0], s.position[1], s.position[2]);
    }
    // Texture handles follow creation order, so a trace's handles are read through this table.
    std::fprintf(out, "\ntextures:\n");
    std::vector<std::pair<std::uint32_t, std::string>> names(names_.begin(), names_.end());
    std::sort(names.begin(), names.end());
    for (const auto& [handle, name] : names)
        std::fprintf(out, "%08X %s\n", handle, name.c_str());
    std::fclose(out);
}

}  // namespace gekko::scaffold
