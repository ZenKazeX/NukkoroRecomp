// render_vk::Renderer's effects (renderer.h): the renderer's own resolve, the full-screen passes, the glare chain with
// its composite folded into the resolve (B3a), and the deformation pass (skinning, delta morphs) - render::Renderer's
// rules (render/renderer.cpp record: add_resolve, add_screen_pass, add_glare, later_canvas_spans, the skin jobs) with
// the same shaders (shaders/utility.hlsl, glare.hlsl, skinning.hlsl).

#include "render_vk/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

#include "render_vk/deform.h"

namespace shadow::render_vk {

namespace {

constexpr std::uint32_t kLinearClamp =
    render::sampler_index(render::Filter::Linear, render::Address::Clamp, render::Address::Clamp);

std::uint32_t float_bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

void begin_colour(VkCommandBuffer cmd, const VkRenderingAttachmentInfo& colour, std::uint32_t width,
                  std::uint32_t height) {
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {width, height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &colour;
    vkCmdBeginRendering(cmd, &info);
}

void set_box(VkCommandBuffer cmd, const render::PixelBox& box) {
    set_scissor(cmd, static_cast<std::int32_t>(box.left), static_cast<std::int32_t>(box.top),
                static_cast<std::uint32_t>(std::max(0L, box.right - box.left)),
                static_cast<std::uint32_t>(std::max(0L, box.bottom - box.top)));
}

void full_view(VkCommandBuffer cmd, const Image& image) {
    set_viewport(cmd, 0.0f, 0.0f, static_cast<float>(image.width), static_cast<float>(image.height));
    set_scissor(cmd, 0, 0, image.width, image.height);
}

}  // namespace

// ---- resolves and full-screen passes ------------------------------------------------------------------------------

void Renderer::add_resolve(const char* name, ResourceId from, ResourceId into, const render::PixelBox* region,
                           std::function<void()> done) {
    // Every texture resolved is the scene's or the glare sources', at the scene's sample count: at 1x a copy.
    const bool copy = samples_ == VK_SAMPLE_COUNT_1_BIT;
    const bool part = region != nullptr;
    const render::PixelBox box = part ? *region : render::PixelBox{};
    graph_.add_pass(
        name,
        [&](PassBuilder& b) {
            b.read(from, copy ? Access::CopySource : Access::ShaderRead);
            // A partial resolve makes only what its readers sample: the rest is not kept either.
            b.write(into, copy ? Access::CopyDest : Access::RenderTarget, Load::Discard);
        },
        [this, from, into, copy, part, box, done = std::move(done)](PassContext& ctx, std::string* error) {
            const VkCommandBuffer cmd = ctx.cmd();
            Image& source = ctx.image(from);
            Image& target = ctx.image(into);
            if (copy) {
                VkImageCopy c{};
                c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.dstSubresource = c.srcSubresource;
                c.extent = {source.width, source.height, 1};
                if (part) {
                    c.srcOffset = {static_cast<std::int32_t>(box.left), static_cast<std::int32_t>(box.top), 0};
                    c.dstOffset = c.srcOffset;
                    c.extent = {static_cast<std::uint32_t>(box.right - box.left),
                                static_cast<std::uint32_t>(box.bottom - box.top), 1};
                }
                vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            } else {
                // The renderer's own resolve (PSResolve: the mean of the samples in a fixed order, the same bits in
                // every process - the driver's resolve was seen to differ in the last bit between processes,
                // R2_DRIVER.md 4.9).
                const VkPipeline pipeline =
                    pipelines_.screen(Screen::Resolve, target.format, VK_SAMPLE_COUNT_1_BIT, error);
                if (pipeline == VK_NULL_HANDLE)
                    return false;
                begin_colour(cmd, ctx.attachment(into), target.width, target.height);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                full_view(cmd, target);
                if (part)
                    set_box(cmd, box);
                std::uint32_t root[16] = {};
                root[4] = transient_srv(source);  // root1.x: the multisampled source
                root[5] = static_cast<std::uint32_t>(source.samples);
                const bool bound = bind_root(cmd, root, error);
                if (bound)
                    vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);
                if (!bound)
                    return false;
            }
            if (done)
                done();
            return true;
        });
}

void Renderer::add_screen_pass(const char* name, Screen screen, ResourceId out, std::vector<ResourceId> inputs,
                               const std::array<std::uint32_t, 16>& root) {
    graph_.add_pass(
        name,
        [&](PassBuilder& b) {
            for (const ResourceId input : inputs)
                b.read(input, Access::ShaderRead);
            b.write(out, Access::RenderTarget, Load::Discard);
        },
        [this, screen, out, inputs, root](PassContext& ctx, std::string* error) {
            Image& target = ctx.image(out);
            const VkPipeline pipeline = pipelines_.screen(screen, target.format, target.samples, error);
            if (pipeline == VK_NULL_HANDLE)
                return false;
            const VkCommandBuffer cmd = ctx.cmd();
            std::uint32_t r[16];
            std::copy(root.begin(), root.end(), r);
            for (std::size_t i = 0; i < inputs.size(); ++i)
                r[i] = transient_srv(ctx.image(inputs[i]));
            begin_colour(cmd, ctx.attachment(out), target.width, target.height);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            full_view(cmd, target);
            const bool bound = bind_root(cmd, r, error);
            if (bound)
                vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            return bound;
        });
}

// ---- the glare ----------------------------------------------------------------------------------------------------

void Renderer::add_glare(const FramePacket& packet, std::size_t v, std::size_t glare_pass,
                         const render::GlareParams& g, const ViewTargets& t, ResourceId sources_ms,
                         FoldedGlare& folded, FrameDraws& frame) {
    const std::uint32_t width = t.width, height = t.height;
    const auto image = [&](std::uint32_t w, std::uint32_t h, const char* name) {
        return frame_texture(name, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);
    };
    // Level A: (96 W/H, 128) texels, even; level B half of it (BLOOM_SPEC 6.4).
    const auto even = [](double x) { return static_cast<std::uint32_t>(std::max(2L, 2L * std::lround(x / 2.0))); };
    const std::uint32_t a_width = even(96.0 * width / height), a_height = 128;
    const std::uint32_t b_width = a_width / 2, b_height = a_height / 2;

    // The sources, resolved: the captures read them, the composite adds them.
    const ResourceId sources = image(width, height, "glare sources resolved");
    add_resolve("glare sources resolve", sources_ms, sources, nullptr);
    const auto capture = [&](std::uint32_t lw, std::uint32_t lh, float gain) {
        const ResourceId out = image(lw, lh, "glare level");
        std::array<std::uint32_t, 16> r{};
        r[4] = width;
        r[5] = height;
        r[6] = lw;
        r[7] = lh;
        r[8] = float_bits(gain);
        add_screen_pass("glare capture", Screen::GlareCapture, out, {sources}, r);  // r[0]: the sources
        return out;
    };
    // One half of a blur: the sum of 1 + taps taps along an axis (no taps: the half does not run).
    const auto blur = [&](ResourceId level, std::uint32_t lw, std::uint32_t lh, std::uint32_t taps, float spread,
                          std::uint32_t axis) {
        if (taps == 0)
            return level;
        const ResourceId out = image(lw, lh, "glare blur");
        std::array<std::uint32_t, 16> r{};
        r[4] = lw;
        r[5] = lh;
        r[8] = taps;
        r[9] = float_bits(spread);
        r[10] = axis;
        add_screen_pass("glare blur", Screen::GlareBlur, out, {level}, r);  // r[0]: the level
        return out;
    };
    ResourceId a = kNoResource;
    ResourceId b = kNoResource;
    if (g.level_a)
        a = blur(blur(capture(a_width, a_height, g.gain_a), a_width, a_height, g.taps_a[0], g.spread_a, 0), a_width,
                 a_height, g.taps_a[1], g.spread_a, 1);
    if (g.level_b) {
        if (a != kNoResource && !g.b_from_sources) {
            b = image(b_width, b_height, "glare level");
            std::array<std::uint32_t, 16> r{};
            r[4] = a_width;
            r[5] = a_height;
            r[8] = float_bits(g.gain_b);
            add_screen_pass("glare box", Screen::GlareBox, b, {a}, r);  // r[0]: level A
        } else {
            b = capture(b_width, b_height, g.gain_b);
        }
        b = blur(blur(b, b_width, b_height, g.taps_b[0], g.spread_b, 0), b_width, b_height, g.taps_b[1], g.spread_b,
                 1);
    }
    ResourceId first_level = a;
    ResourceId second_level = b;
    if (a != kNoResource && b != kNoResource && !g.separate) {
        first_level = image(a_width, a_height, "glare merge");
        std::array<std::uint32_t, 16> r{};
        r[2] = kLinearClamp;
        r[4] = a_width;
        r[5] = a_height;
        add_screen_pass("glare merge", Screen::GlareMerge, first_level, {a, b}, r);  // r[0], r[1]: A, B
        second_level = kNoResource;
    } else if (a == kNoResource) {
        first_level = b;
        second_level = kNoResource;
    }

    // The composite: the scene gets the sources and the level(s), saturating - folded into the view's resolve when
    // only canvas draws follow at 4x and above (B3a; render/renderer.cpp later_canvas_spans): then the plain composite
    // runs only on the 16-pixel tiles those draws can reach, before them as the game ordered it.
    std::vector<render::PixelBox> spans;
    bool fold = options_.fold_glare && samples_ >= VK_SAMPLE_COUNT_4_BIT && !folded.active;
    if (fold) {
        constexpr std::uint32_t kTile = 16;
        const std::uint32_t tiles_x = (width + kTile - 1) / kTile, tiles_y = (height + kTile - 1) / kTile;
        std::vector<std::uint8_t> marked(std::size_t{tiles_x} * tiles_y, 0);
        const render::ViewRecord& view = packet.views[v];
        const float w = static_cast<float>(width), h = static_cast<float>(height);
        for (std::size_t q = glare_pass + 1; q < packet.passes.size() && fold; ++q) {
            const render::PassRecord& later = packet.passes[q];
            if (later.view != v)
                continue;
            if (later.kind != render::PassKind::Scene) {
                fold = false;
                break;
            }
            for (std::uint32_t d = later.first_draw; d < later.first_draw + later.draw_count && fold; ++d) {
                if (d >= packet.draws.size() || packet.draws[d].immediate >= packet.immediates.size()) {
                    fold = false;
                    break;
                }
                const render::DrawRecord& draw = packet.draws[d];
                const render::ImmediateDraw& im = packet.immediates[draw.immediate];
                if (im.space != render::Space::Canvas ||
                    std::size_t{im.first_vertex} + im.vertex_count > packet.immediate_vertices.size()) {
                    fold = false;
                    break;
                }
                render::Mat4 placement;
                render::PixelBox unused;
                if (im.region != render::kNoRegion && im.region < packet.canvas_regions.size())
                    render::region_projection(view.canvas, packet.canvas_regions[im.region], im.anchor, w, h,
                                              &placement, &unused);
                else
                    placement = render::canvas_projection(view.canvas, im.anchor, w, h);
                const render::Mat4 m = render::multiply(placement, render::from3x4(draw.world));
                double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
                for (std::uint32_t k = 0; k < im.vertex_count && fold; ++k) {
                    const render::Float3& p = packet.immediate_vertices[im.first_vertex + k].position;
                    double c[4];
                    for (int r = 0; r < 4; ++r)
                        c[r] = static_cast<double>(m.m[r][0]) * p.x + static_cast<double>(m.m[r][1]) * p.y +
                               static_cast<double>(m.m[r][2]) * p.z + m.m[r][3];
                    if (!(c[3] > 0.0) || !std::isfinite(c[0]) || !std::isfinite(c[1])) {
                        fold = false;
                        break;
                    }
                    const double px = (c[0] / c[3] + 1.0) * 0.5 * width;
                    const double py = (1.0 - c[1] / c[3]) * 0.5 * height;
                    x0 = std::min(x0, px);
                    x1 = std::max(x1, px);
                    y0 = std::min(y0, py);
                    y1 = std::max(y1, py);
                }
                if (!fold || im.vertex_count == 0)
                    continue;
                // A pixel's samples lie inside it: one pixel of margin covers every sample a primitive inside these
                // bounds can reach (and a line's width).
                const long left = std::max(0L, static_cast<long>(std::floor(x0)) - 1);
                const long top = std::max(0L, static_cast<long>(std::floor(y0)) - 1);
                const long right = std::min(static_cast<long>(width), static_cast<long>(std::ceil(x1)) + 1);
                const long bottom = std::min(static_cast<long>(height), static_cast<long>(std::ceil(y1)) + 1);
                if (left >= right || top >= bottom)
                    continue;
                for (long ty = top / kTile; ty <= (bottom - 1) / static_cast<long>(kTile); ++ty)
                    for (long tx = left / kTile; tx <= (right - 1) / static_cast<long>(kTile); ++tx)
                        marked[static_cast<std::size_t>(ty) * tiles_x + static_cast<std::size_t>(tx)] = 1;
            }
        }
        if (fold)
            for (std::uint32_t ty = 0; ty < tiles_y; ++ty)
                for (std::uint32_t tx = 0; tx < tiles_x;) {
                    if (marked[std::size_t{ty} * tiles_x + tx] == 0) {
                        ++tx;
                        continue;
                    }
                    std::uint32_t end = tx;
                    while (end < tiles_x && marked[std::size_t{ty} * tiles_x + end] != 0)
                        ++end;
                    spans.push_back(render::PixelBox{static_cast<long>(tx * kTile), static_cast<long>(ty * kTile),
                                                     static_cast<long>(std::min(end * kTile, width)),
                                                     static_cast<long>(std::min((ty + 1) * kTile, height))});
                    tx = end;
                }
    }
    ++frame.glares;
    if (fold) {
        folded = FoldedGlare{true, sources, first_level, second_level, spans};
        ++frame.glares_folded;
        frame.folded_spans += spans.size();
        if (spans.empty())
            return;
    }
    graph_.add_pass(
        fold ? "glare composite spans" : "glare composite",
        [&](PassBuilder& builder) {
            builder.read(sources, Access::ShaderRead);
            if (first_level != kNoResource)
                builder.read(first_level, Access::ShaderRead);
            if (second_level != kNoResource)
                builder.read(second_level, Access::ShaderRead);
            builder.write(t.colour, Access::RenderTarget, Load::Keep);
        },
        [this, colour = t.colour, sources, first_level, second_level, width, height, fold,
         spans](PassContext& ctx, std::string* error) {
            Image& scene = ctx.image(colour);
            const VkPipeline pipeline = pipelines_.screen(Screen::GlareComposite, scene.format, samples_, error);
            if (pipeline == VK_NULL_HANDLE)
                return false;
            const VkCommandBuffer cmd = ctx.cmd();
            std::uint32_t r[16] = {};
            r[0] = transient_srv(ctx.image(sources));
            r[1] = first_level != kNoResource ? transient_srv(ctx.image(first_level)) : kNoDescriptor;
            r[2] = second_level != kNoResource ? transient_srv(ctx.image(second_level)) : kNoDescriptor;
            r[3] = kLinearClamp;
            r[4] = width;
            r[5] = height;
            begin_colour(cmd, ctx.attachment(colour), scene.width, scene.height);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            full_view(cmd, scene);
            const bool bound = bind_root(cmd, r, error);
            if (bound) {
                if (!fold) {
                    vkCmdDraw(cmd, 3, 1, 0, 0);
                } else {
                    for (const render::PixelBox& span : spans) {
                        set_box(cmd, span);
                        vkCmdDraw(cmd, 3, 1, 0, 0);
                    }
                }
            }
            vkCmdEndRendering(cmd);
            return bound;
        });
}

void Renderer::add_folded_resolve(const ViewTargets& t, const FoldedGlare& folded) {
    graph_.add_pass(
        "resolve with glare",
        [&](PassBuilder& b) {
            b.read(t.colour, Access::ShaderRead);
            b.read(folded.sources, Access::ShaderRead);
            if (folded.first != kNoResource)
                b.read(folded.first, Access::ShaderRead);
            if (folded.second != kNoResource)
                b.read(folded.second, Access::ShaderRead);
            b.write(t.resolved, Access::RenderTarget, Load::Discard);
        },
        [this, t, folded](PassContext& ctx, std::string* error) {
            Image& resolved = ctx.image(t.resolved);
            const VkPipeline fold = pipelines_.screen(Screen::ResolveGlare, resolved.format, VK_SAMPLE_COUNT_1_BIT, error);
            const VkPipeline plain = pipelines_.screen(Screen::Resolve, resolved.format, VK_SAMPLE_COUNT_1_BIT, error);
            if (fold == VK_NULL_HANDLE || plain == VK_NULL_HANDLE)
                return false;
            const VkCommandBuffer cmd = ctx.cmd();
            begin_colour(cmd, ctx.attachment(t.resolved), resolved.width, resolved.height);
            full_view(cmd, resolved);
            // PSResolveGlare: root0 the composite's inputs, root1.x the scene and .y its samples, root2.xy the view's
            // size, root3.x 0 (no diagnostics). PSResolve reads root1 the same way, so one block serves both.
            std::uint32_t r[16] = {};
            r[0] = transient_srv(ctx.image(folded.sources));
            r[1] = folded.first != kNoResource ? transient_srv(ctx.image(folded.first)) : kNoDescriptor;
            r[2] = folded.second != kNoResource ? transient_srv(ctx.image(folded.second)) : kNoDescriptor;
            r[3] = kLinearClamp;
            r[4] = transient_srv(ctx.image(t.colour));
            r[5] = static_cast<std::uint32_t>(samples_);
            r[8] = t.width;
            r[9] = t.height;
            const bool bound = bind_root(cmd, r, error);
            if (bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fold);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                // The spans got the composite before the draws after it: their samples are resolved as they are.
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, plain);
                for (const render::PixelBox& span : folded.spans) {
                    set_box(cmd, span);
                    vkCmdDraw(cmd, 3, 1, 0, 0);
                }
            }
            vkCmdEndRendering(cmd);
            return bound;
        });
}

// ---- the deformation pass -----------------------------------------------------------------------------------------

bool Renderer::reserve_skin_cache(std::uint32_t vertices_per_frame, std::string* error) {
    if (vertices_per_frame <= skin_per_frame_)
        return true;
    const std::uint64_t doubled = std::uint64_t{skin_per_frame_} * 2u;
    const auto target = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(std::max<std::uint64_t>(vertices_per_frame, doubled), 0x7FFFFFFFu));
    Buffer made;
    if (!create_buffer(device_,
                       VkDeviceSize{target} * frames_.frames_in_flight() * sizeof(render::PositionVertex),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, HostAccess::None, &made,
                       error))
        return false;
    const std::uint32_t srv = bindless_.allocate();
    if (srv == kNoDescriptor) {
        destroy_buffer(device_, made);
        return fail(error, "bindless set full (skin cache)");
    }
    bindless_.write_buffer(srv, made.buffer, 0, VK_WHOLE_SIZE);
    // The previous cache stays until the GPU has passed the frames that used it.
    bindless_.free(skin_cache_srv_, device_.submitted());
    retire_buffer(device_, skin_cache_);
    skin_cache_ = made;
    skin_cache_srv_ = srv;
    skin_per_frame_ = target;
    return true;
}

namespace {

// A mesh's deformation sources as the jobs need them, copied under the table lock.
struct DeformSource {
    std::uint32_t skin_srv = kNoDescriptor, morph_srv = kNoDescriptor;
    std::uint32_t vertex_count = 0, morph_targets = 0;
    bool skinned = false;
};
// One job per distinct (mesh, palette, morph weights) - every draw of that combination reads its vertices.
using DeformKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;

}  // namespace

void Renderer::size_skin_cache(const FramePacket& packet) {
    // The frame's distinct jobs fit in the cache before it is used (it grows, at least doubling).
    std::set<DeformKey> keys;
    std::uint64_t needed = 0;
    for (const render::DrawRecord& draw : packet.draws) {
        if (draw.bones.count == 0u && draw.morph.count == 0u)
            continue;
        std::uint32_t vertex_count = 0, morph_targets = 0;
        {
            std::lock_guard<std::mutex> lock(table_mutex_);
            const MeshSlot* mesh = mesh_slot(draw.mesh);
            if (mesh == nullptr || mesh->skin_srv == kNoDescriptor)
                continue;
            vertex_count = mesh->vertex_count;
            morph_targets = mesh->morph_targets;
        }
        const bool morph = morph_targets != 0u && draw.morph.count != 0u;
        if (draw.bones.count == 0u && !morph)
            continue;
        if (keys.emplace(draw.mesh.value, draw.bones.count != 0u ? draw.bones.first : ~0u,
                         morph ? draw.morph.first : ~0u).second)
            needed += vertex_count;
    }
    std::string grow_error;
    if (needed > skin_per_frame_ &&
        !reserve_skin_cache(static_cast<std::uint32_t>(std::min<std::uint64_t>(needed, 0x7FFFFFFFu)), &grow_error))
        std::fprintf(stderr, "[render-vk] skin cache could not grow to %llu vertices: %s\n",
                     static_cast<unsigned long long>(needed), grow_error.c_str());
}

bool Renderer::record_deformations(VkCommandBuffer cmd, const FramePacket& packet, FrameDraws& frame,
                                   std::string* error) {
    using Source = DeformSource;
    using Key = DeformKey;
    const auto source_of = [&](render::MeshHandle handle, Source* out) {
        std::lock_guard<std::mutex> lock(table_mutex_);
        const MeshSlot* mesh = mesh_slot(handle);
        if (mesh == nullptr || mesh->skin_srv == kNoDescriptor)
            return false;
        *out = Source{mesh->skin_srv, mesh->morph_srv, mesh->vertex_count, mesh->morph_targets, mesh->skinned};
        return true;
    };
    struct Job {
        Source source;
        std::uint32_t palette = 0;  // ring offset of the palette block
        std::uint32_t element = 0;
        float weights[render::kMaxMorphTargets] = {};
        bool morph = false;
        bool skin = false;
    };
    const std::uint32_t region = frames_.slot() * skin_per_frame_;
    std::uint32_t used = 0;
    std::vector<Job> jobs;
    std::map<Key, std::uint32_t> job_of;  // -> element
    std::map<Key, std::array<float, 3>> origin_of;
    std::uint32_t no_palette = ~0u;
    for (std::size_t d = 0; d < packet.draws.size(); ++d) {
        const render::DrawRecord& draw = packet.draws[d];
        if (draw.bones.count == 0u && draw.morph.count == 0u)
            continue;
        Source s;
        const bool known = source_of(draw.mesh, &s);
        const bool morph = known && s.morph_targets != 0u && draw.morph.count != 0u &&
                           std::size_t{draw.morph.first} + draw.morph.count <= packet.morph_weights.size();
        if (draw.bones.count == 0u && !morph)
            continue;
        const Key key{draw.mesh.value, draw.bones.count != 0u ? draw.bones.first : ~0u, morph ? draw.morph.first : ~0u};
        if (const auto found = job_of.find(key); found != job_of.end()) {
            frame.deformed[d] = found->second;
            frame.deformed_origin[d] = origin_of[key];
            continue;
        }
        std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
        std::uint32_t element = kNotDeformed;
        const bool skin = draw.bones.count != 0u;
        const bool palette_ok = known && s.skinned && draw.bones.count <= kMaxPaletteBones &&
                                std::size_t{draw.bones.first} + draw.bones.count <= packet.bones.size();
        if (known && (skin ? palette_ok : morph)) {
            if (s.vertex_count > skin_per_frame_ - used) {
                ++render_stats_.skin_overflows;
            } else {
                element = region + used;
                used += s.vertex_count;
                Job job;
                job.source = s;
                job.element = element;
                job.skin = skin;
                job.morph = morph;
                if (skin) {
                    // A pose is written relative to its own origin (the first posed bone's translation): the draw puts
                    // the origin back through its world matrix (render/renderer.cpp, float precision far out).
                    BoneMatrix palette[kMaxPaletteBones] = {};
                    std::memcpy(palette, &packet.bones[draw.bones.first], draw.bones.count * sizeof(BoneMatrix));
                    const auto posed = [](const BoneMatrix& b) {
                        for (const auto& row : b.rows)
                            for (const float value : row)
                                if (value != 0.0f)
                                    return true;
                        return false;
                    };
                    for (std::uint32_t i = 0; i < draw.bones.count; ++i)
                        if (posed(palette[i])) {
                            origin = {palette[i].rows[0][3], palette[i].rows[1][3], palette[i].rows[2][3]};
                            break;
                        }
                    for (std::uint32_t i = 0; i < draw.bones.count; ++i)
                        if (posed(palette[i]))
                            for (int row = 0; row < 3; ++row)
                                palette[i].rows[row][3] -= origin[row];
                    job.palette = ring_block(palette, sizeof(palette));
                    if (job.palette == ~0u)
                        return fail(error, "frame ring full (skin palettes)");
                    ++frame.skin_jobs;
                    frame.skinned_vertices += s.vertex_count;
                } else {
                    if (no_palette == ~0u) {
                        static const BoneMatrix zero[kMaxPaletteBones] = {};
                        no_palette = ring_block(zero, sizeof(zero));
                        if (no_palette == ~0u)
                            return fail(error, "frame ring full (skin palettes)");
                    }
                    job.palette = no_palette;
                }
                if (morph) {
                    // Targets past the draw's weights blend with weight 0.
                    for (std::uint32_t k = 0; k < s.morph_targets && k < render::kMaxMorphTargets; ++k)
                        job.weights[k] = k < draw.morph.count ? packet.morph_weights[draw.morph.first + k] : 0.0f;
                    ++frame.morph_jobs;
                }
                jobs.push_back(job);
            }
        }
        job_of.emplace(key, element);
        origin_of.emplace(key, origin);
        frame.deformed[d] = element;
        frame.deformed_origin[d] = origin;
    }
    if (jobs.empty())
        return true;

    const VkPipeline pipeline = pipelines_.skin(error);
    if (pipeline == VK_NULL_HANDLE)
        return false;
    // The graph made the cache writable (earlier frames' draws read other regions of it; the writes wait for them all
    // the same - cheap) and makes it readable as a vertex stream after this pass.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    const VkDescriptorSet heap = bindless_.heap_set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bindless_.pipeline_layout(), 1, 1, &heap, 0, nullptr);
    const VkDescriptorSet constants_set = bindless_.constants_set(frames_.slot());
    for (const Job& job : jobs) {
        // root0: source, destination, vertex count, first element; root1: the deltas, the target count (0: no morph),
        // skinning on / off; root2 / root3: the weights of targets 0..7 (shaders/skinning.hlsl).
        std::uint32_t root[16] = {job.source.skin_srv, skin_cache_srv_, job.source.vertex_count, job.element};
        root[4] = job.morph ? job.source.morph_srv : kNoDescriptor;
        root[5] = job.morph ? job.source.morph_targets : 0u;
        root[6] = job.skin ? 1u : 0u;
        if (job.morph)
            for (std::uint32_t k = 0; k < job.source.morph_targets && k < 8u; ++k)
                std::memcpy(&root[8 + k], &job.weights[k], sizeof(float));
        const VkDeviceSize alignment = std::max<VkDeviceSize>(16, device_.caps().min_uniform_offset_alignment);
        const UploadAllocation block = frames_.allocate(kRootBlockBytes, alignment);
        if (!block)
            return fail(error, "frame ring full (skin constants)");
        std::memcpy(block.cpu, root, sizeof(root));
        const std::uint32_t offsets[3] = {static_cast<std::uint32_t>(block.offset), job.palette, 0u};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bindless_.pipeline_layout(), 0, 1,
                                &constants_set, 3, offsets);
        vkCmdDispatch(cmd, (job.source.vertex_count + 63) / 64, 1, 1);
    }
    return true;
}

}  // namespace shadow::render_vk
