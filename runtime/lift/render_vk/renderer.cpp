#include "render_vk/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "render/frame_math.h"
#include "render/snapshot_reads.h"

namespace shadow::render_vk {

namespace {

// The point / clamp entry of the sampler table (render/descriptors.h sampler_index).
constexpr std::uint32_t kPointClamp =
    render::sampler_index(render::Filter::Point, render::Address::Clamp, render::Address::Clamp);

VkSampleCountFlagBits sample_bits(std::uint32_t samples) {
    switch (samples) {
    case 1: return VK_SAMPLE_COUNT_1_BIT;
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    default: return VK_SAMPLE_COUNT_4_BIT;
    }
}

void begin_rendering(VkCommandBuffer cmd, std::uint32_t width, std::uint32_t height,
                     const VkRenderingAttachmentInfo& colour, const VkRenderingAttachmentInfo* depth) {
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {width, height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &colour;
    info.pDepthAttachment = depth;
    vkCmdBeginRendering(cmd, &info);
}

}  // namespace

Renderer::~Renderer() {
    destroy();
}

const char* Renderer::skip_reason_name(SkipReason reason) noexcept {
    switch (reason) {
    case SkipReason::ParticleBatch: return "particle-batch";
    case SkipReason::ParticlePipeline: return "particle-pipeline";
    case SkipReason::ParticleUpload: return "particle-upload";
    case SkipReason::ImmediateInvalid: return "immediate-invalid";
    case SkipReason::ImmediatePipeline: return "immediate-pipeline";
    case SkipReason::ImmediateImage: return "immediate-image";
    case SkipReason::ImmediateUpload: return "immediate-upload";
    case SkipReason::MeshMissing: return "mesh-missing";
    case SkipReason::SkinMissing: return "skin-missing";
    case SkipReason::MeshPipeline: return "mesh-pipeline";
    case SkipReason::MeshUpload: return "mesh-upload";
    case SkipReason::Count: break;
    }
    return "?";
}

bool Renderer::create(const RendererOptions& options, std::string* error) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    options_ = options;
    if (!device_.create(options.device, error))
        return false;
    samples_ = sample_bits(options.msaa_samples);
    // The scene target: UNORM16 as on D3D12 when the device blends into it at the sample count; otherwise the
    // nearest format that keeps 16 bits per channel (docs/VULKAN_RENDERER.md 5.3).
    if (!colour_target_supported(device_.physical(), options_.scene_format, samples_, true)) {
        if (colour_target_supported(device_.physical(), VK_FORMAT_R16G16B16A16_SFLOAT, samples_, true)) {
            std::fprintf(stderr, "[render-vk] scene format %d cannot be a blended %u-sample target here; using "
                                 "R16G16B16A16_SFLOAT\n",
                         static_cast<int>(options_.scene_format), static_cast<unsigned>(samples_));
            options_.scene_format = VK_FORMAT_R16G16B16A16_SFLOAT;
        } else {
            return fail(error, "no 16-bit-per-channel scene format renders at the asked sample count");
        }
    }
    if (!bindless_.create(device_, options.bindless_persistent, options.bindless_transient_per_frame,
                          options.frames_in_flight, options.max_anisotropy, error))
        return false;
    // The ring keeps the widest constant block's range past every allocation (a dynamic offset names a whole block).
    if (!frames_.create(device_, options.frames_in_flight, options.upload_ring_bytes, kLightsBlockBytes, error))
        return false;
    for (std::uint32_t s = 0; s < options.frames_in_flight; ++s)
        if (!bindless_.make_constants_set(s, frames_.ring_buffer(s), error))
            return false;
    if (!pipelines_.create(device_, bindless_, error))
        return false;
    targets_.attach(device_);
    // Timestamps per rendering group: the frame's start and one per group (a split-screen frame has ~40 groups).
    if (options.gpu_timestamps && !timer_.create(device_, options.frames_in_flight, 512, error))
        return false;
    if (!reserve_skin_cache(std::max(1u, options.skinned_vertices_per_frame), error))
        return false;
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = device_.queue_family();
    if (!check(vkCreateCommandPool(device_.vk(), &pool, nullptr, &upload_pool_), "vkCreateCommandPool (uploads)",
               error))
        return false;
    created_ = true;
    return true;
}

void Renderer::destroy() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!created_ && device_.vk() == VK_NULL_HANDLE)
        return;
    device_.wait_idle();
    swapchain_.destroy();
    has_window_ = false;
    targets_.destroy();
    timer_.destroy();
    destroy_image(device_, capture_);
    destroy_buffer(device_, readback_);
    for (History& h : histories_)
        destroy_image(device_, h.image);
    histories_.clear();
    destroy_buffer(device_, skin_cache_);
    skin_cache_srv_ = kNoDescriptor;
    skin_per_frame_ = 0;
    {
        std::lock_guard<std::mutex> table_lock(table_mutex_);
        for (TextureSlot& s : textures_)
            if (s.live)
                destroy_image(device_, s.image);
        for (MeshSlot& s : meshes_)
            if (s.live)
                destroy_buffer(device_, s.streams);
        textures_.clear();
        meshes_.clear();
        free_textures_.clear();
        free_meshes_.clear();
        pending_releases_.clear();
        packets_finished_ = false;
        finished_through_ = 0;
    }
    device_.collect();  // the retired staging buffers and upload command buffers (the GPU is idle)
    if (upload_pool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(device_.vk(), upload_pool_, nullptr);
    upload_pool_ = VK_NULL_HANDLE;
    pipelines_.destroy();
    frames_.destroy();
    bindless_.destroy();
    device_.destroy();
    created_ = false;
}

// ---- histories (stage V4) ----------------------------------------------------------------------------------------

void Renderer::invalidate_histories() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    for (History& h : histories_)
        h.valid = false;
}

// ---- frames ------------------------------------------------------------------------------------------------------

ResourceId Renderer::frame_texture(const char* name, std::uint32_t width, std::uint32_t height, VkFormat format,
                                   VkSampleCountFlagBits samples) {
    return graph_.create_texture(name, TextureDesc{width, height, format, samples, false});
}

std::uint32_t Renderer::transient_srv(const Image& image) {
    const std::uint32_t index = bindless_.allocate_transient();
    bindless_.write_image(index, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return index;
}

bool Renderer::bind_root(VkCommandBuffer cmd, const std::uint32_t (&root)[16], std::string* error) {
    const UploadAllocation block =
        frames_.allocate(kRootBlockBytes, std::max<VkDeviceSize>(device_.caps().min_uniform_offset_alignment, 16));
    if (!block)
        return fail(error, "upload ring full (root constants)");
    std::memcpy(block.cpu, root, sizeof(root));
    const std::uint32_t offsets[3] = {static_cast<std::uint32_t>(block.offset), 0u, 0u};
    const VkDescriptorSet set = bindless_.constants_set(frames_.slot());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_.pipeline_layout(), 0, 1, &set, 3,
                            offsets);
    return true;
}

bool Renderer::record(VkCommandBuffer cmd, const FramePacket& packet, const Output& output, bool commit_history,
                      std::string* error) {
    bindless_.begin_frame(frames_.slot());
    const VkDescriptorSet heap = bindless_.heap_set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_.pipeline_layout(), 1, 1, &heap, 0,
                            nullptr);
    const VkFormat scene_format = options_.scene_format;
    graph_.reset(frame_number_);
    graph_.set_transient(options_.transient_targets);
    graph_.set_merging(options_.merge_passes);

    // The box the views are shown in: the whole output, or a centred box of the packet's content aspect, whole pixels
    // (render/renderer.cpp record, the same arithmetic).
    float box_x = 0.0f, box_y = 0.0f;
    float box_w = static_cast<float>(output.width), box_h = static_cast<float>(output.height);
    if (packet.content_aspect > 0.0f) {
        if (box_w > std::round(box_h * packet.content_aspect)) {
            box_w = std::round(box_h * packet.content_aspect);
            box_x = std::floor((static_cast<float>(output.width) - box_w) * 0.5f);
        } else if (box_h > std::round(box_w / packet.content_aspect)) {
            box_h = std::round(box_w / packet.content_aspect);
            box_y = std::floor((static_cast<float>(output.height) - box_h) * 0.5f);
        }
    }

    std::vector<ViewTargets> views(packet.views.size());
    const auto view_size = [&](std::size_t v, std::uint32_t* w, std::uint32_t* h) {
        const render::ViewRecord& view = packet.views[v];
        const float x0 = std::round(view.rect.x * box_w), y0 = std::round(view.rect.y * box_h);
        const float x1 = std::round((view.rect.x + view.rect.width) * box_w);
        const float y1 = std::round((view.rect.y + view.rect.height) * box_h);
        *w = static_cast<std::uint32_t>(std::max(1.0f, x1 - x0));
        *h = static_cast<std::uint32_t>(std::max(1.0f, y1 - y0));
    };

    // Scene snapshots (numbered in pass order) and the views whose history this frame saves or draws.
    std::vector<std::uint32_t> pass_snapshot(packet.passes.size(), ~0u);
    std::vector<std::uint8_t> uses_history(packet.views.size(), 0);
    std::uint32_t snapshot_count = 0;
    for (std::size_t p = 0; p < packet.passes.size(); ++p) {
        const render::PassRecord& pass = packet.passes[p];
        if (pass.kind == render::PassKind::Snapshot)
            pass_snapshot[p] = snapshot_count++;
        if (pass.view >= packet.views.size())
            continue;
        if (pass.kind == render::PassKind::SaveHistory)
            uses_history[pass.view] = 1;
        for (std::uint32_t d = pass.first_draw; d < pass.first_draw + pass.draw_count && d < packet.draws.size(); ++d) {
            const std::uint32_t im = packet.draws[d].immediate;
            if (im < packet.immediates.size() && packet.immediates[im].source == render::ImageSource::History)
                uses_history[pass.view] = 1;
        }
    }
    // What each snapshot's readers sample of it (render/snapshot_reads.h): its resolve makes only that. A snapshot
    // whose reads cannot be bounded - a draw outside the canvas, one in another view, lines - resolves its whole view.
    std::vector<render::TexelRect> snapshot_region(snapshot_count);
    std::vector<std::uint8_t> snapshot_whole(snapshot_count, 0);
    {
        std::vector<std::size_t> snapshot_view(snapshot_count, packet.views.size());
        for (std::size_t p = 0; p < packet.passes.size(); ++p)
            if (pass_snapshot[p] < snapshot_count)
                snapshot_view[pass_snapshot[p]] = packet.passes[p].view;
        for (const render::PassRecord& pass : packet.passes) {
            if (pass.view >= packet.views.size())
                continue;
            std::uint32_t width = 0, height = 0;
            view_size(pass.view, &width, &height);
            const render::Canvas& canvas = packet.views[pass.view].canvas;
            for (std::uint32_t d = pass.first_draw; d < pass.first_draw + pass.draw_count && d < packet.draws.size();
                 ++d) {
                const std::uint32_t i = packet.draws[d].immediate;
                if (i >= packet.immediates.size())
                    continue;
                const render::ImmediateDraw& im = packet.immediates[i];
                if (im.source != render::ImageSource::Snapshot || im.snapshot >= snapshot_count ||
                    snapshot_whole[im.snapshot] != 0u)
                    continue;
                bool bounded = im.space == render::Space::Canvas && snapshot_view[im.snapshot] == pass.view &&
                               std::size_t{im.first_vertex} + im.vertex_count <= packet.immediate_vertices.size() &&
                               std::size_t{im.first_index} + im.index_count <= packet.immediate_indices.size();
                if (bounded) {
                    // The draw's placement exactly as its pass records it.
                    render::Mat4 placement;
                    const float w = static_cast<float>(width), h = static_cast<float>(height);
                    if (im.region != render::kNoRegion && im.region < packet.canvas_regions.size()) {
                        render::PixelBox unused;
                        render::region_projection(canvas, packet.canvas_regions[im.region], im.anchor, w, h,
                                                  &placement, &unused);
                    } else {
                        placement = render::canvas_projection(canvas, im.anchor, w, h);
                    }
                    const render::Mat4 clip = render::multiply(placement, render::from3x4(packet.draws[d].world));
                    const float position[3][4] = {{clip.m[0][0], clip.m[0][1], clip.m[0][2], clip.m[0][3]},
                                                  {clip.m[1][0], clip.m[1][1], clip.m[1][2], clip.m[1][3]},
                                                  {clip.m[3][0], clip.m[3][1], clip.m[3][2], clip.m[3][3]}};
                    const auto rows = render::canvas_image_rows(placement);
                    const float uv[2][4] = {{rows[0][0], rows[0][1], rows[0][2], rows[0][3]},
                                            {rows[1][0], rows[1][1], rows[1][2], rows[1][3]}};
                    bounded = render::grow_sampled_texels(
                        im.topology, &packet.immediate_vertices[im.first_vertex], im.vertex_count,
                        im.index_count != 0u ? &packet.immediate_indices[im.first_index] : nullptr, im.index_count,
                        position, uv, width, height, &snapshot_region[im.snapshot]);
                }
                if (!bounded)
                    snapshot_whole[im.snapshot] = 1;
            }
        }
    }
    if (histories_.size() < packet.views.size())
        histories_.resize(packet.views.size());

    // Keep depth on tile: per view, the last pass whose draws test or write the depth (a glare sources pass only tests
    // it); per glare sources pass, whether a later Glare pass of its view reads them (unread ones stay alone, culled).
    constexpr std::size_t kNoPass = ~std::size_t{0};
    std::vector<std::size_t> last_depth(packet.views.size(), kNoPass);
    std::vector<std::uint8_t> sources_read(packet.passes.size(), 0);
    {
        std::vector<std::uint8_t> glare_ahead(packet.views.size(), 0);
        for (std::size_t p = packet.passes.size(); p-- > 0;) {
            const render::PassRecord& pass = packet.passes[p];
            if (pass.view >= packet.views.size())
                continue;
            if (pass.kind == render::PassKind::Glare)
                glare_ahead[pass.view] = 1;
            if (pass.kind == render::PassKind::GlareSources)
                sources_read[p] = glare_ahead[pass.view];
            if (pass.kind != render::PassKind::Scene && pass.kind != render::PassKind::GlareSources)
                continue;
            bool depth = false;
            for (std::uint32_t d = pass.first_draw; d < pass.first_draw + pass.draw_count && d < packet.draws.size();
                 ++d) {
                const render::Recipe& r = packet.draws[d].recipe;
                depth = depth || r.depth_test || (r.depth_write && pass.kind == render::PassKind::Scene);
            }
            if (depth && last_depth[pass.view] == kNoPass)
                last_depth[pass.view] = p;
        }
    }
    const bool pairing = options_.keep_depth_on_tile && device_.caps().local_read;

    // The frame's light sets, written once each into the frame ring; draws without a set read the empty one.
    FrameDraws frame;
    frame.light_blocks.resize(packet.light_sets.size(), 0);
    for (std::size_t i = 0; i < packet.light_sets.size(); ++i) {
        render::LightConstants constants;
        render::fill_light_constants(packet.light_sets[i], &constants);
        frame.light_blocks[i] = ring_block(&constants, sizeof(constants));
        if (frame.light_blocks[i] == ~0u)
            return fail(error, "frame ring full (light sets)");
    }
    {
        const render::LightConstants empty{};
        frame.no_lights = ring_block(&empty, sizeof(empty));
        if (frame.no_lights == ~0u)
            return fail(error, "frame ring full (light sets)");
    }
    frame.deformed.assign(packet.draws.size(), kNotDeformed);
    frame.deformed_origin.assign(packet.draws.size(), std::array<float, 3>{0.0f, 0.0f, 0.0f});

    // The deformed meshes (R_DESIGN.md 2.4): every job into this frame's region of the skin cache, before the views;
    // the draws read it as a vertex stream. Without scalarBlockLayout the pass's 44-byte stream cannot be read
    // (docs/VULKAN_RENDERER.md 6): no jobs, and the posed draws are left out as having no posed vertices.
    ResourceId skin_cache = kNoResource;
    const bool deforming =
        device_.caps().scalar_block_layout &&
        std::any_of(packet.draws.begin(), packet.draws.end(),
                    [](const render::DrawRecord& d) { return d.bones.count != 0u || d.morph.count != 0u; });
    if (deforming) {
        size_skin_cache(packet);
        skin_cache = graph_.import_buffer("skin cache", skin_cache_.buffer, Access::VertexRead, Access::VertexRead);
        graph_.add_pass(
            "deformations", [&](PassBuilder& b) { b.write(skin_cache, Access::UnorderedAccess); },
            [this, &packet, &frame](PassContext& ctx, std::string* e) {
                return record_deformations(ctx.cmd(), packet, frame, e);
            });
    }

    VkClearValue background{};
    background.color = {{packet.background.x, packet.background.y, packet.background.z, 1.0f}};
    VkClearValue black{};  // the glare sources: onto black, alpha 0
    VkClearValue far_depth{};
    far_depth.depthStencil = {render::kFarDepth, 0};
    std::vector<ResourceId> snapshots(snapshot_count, kNoResource);  // per snapshot id, once declared

    for (std::size_t v = 0; v < packet.views.size(); ++v) {
        const render::ViewRecord& view = packet.views[v];
        ViewTargets& t = views[v];
        t.x0 = box_x + std::round(view.rect.x * box_w);
        t.y0 = box_y + std::round(view.rect.y * box_h);
        t.x1 = box_x + std::round((view.rect.x + view.rect.width) * box_w);
        t.y1 = box_y + std::round((view.rect.y + view.rect.height) * box_h);
        t.width = static_cast<std::uint32_t>(std::max(1.0f, t.x1 - t.x0));
        t.height = static_cast<std::uint32_t>(std::max(1.0f, t.y1 - t.y0));
        t.colour = frame_texture("view colour", t.width, t.height, scene_format, samples_);
        t.depth = frame_texture("view depth", t.width, t.height, VK_FORMAT_D32_SFLOAT, samples_);
        t.resolved = frame_texture("view", t.width, t.height, scene_format, VK_SAMPLE_COUNT_1_BIT);

        // A view drawn over the views before it (split screen's overlay) starts from the picture those views made
        // where it covers them, on the background elsewhere (colour only, as on D3D12); its depth is cleared by its
        // first draw pass.
        std::vector<std::size_t> under;
        if (view.over_views && v > 0) {
            for (std::size_t u = 0; u < v; ++u) {
                const ViewTargets& w = views[u];
                if (w.x0 < t.x1 && w.x1 > t.x0 && w.y0 < t.y1 && w.y1 > t.y0)
                    under.push_back(u);
            }
            ++render_stats_.views_over_views;
        }
        bool colour_ready = false;
        if (!under.empty()) {
            graph_.add_pass(
                "views under",
                [&](PassBuilder& b) {
                    b.write(t.colour, Access::RenderTarget, Load::Clear, background);
                    for (const std::size_t u : under)
                        b.read(views[u].resolved, Access::ShaderRead);
                },
                [this, &views, v, under, scene_format](PassContext& ctx, std::string* e) {
                    const ViewTargets& vt = views[v];
                    const VkPipeline pipeline = pipelines_.screen(Screen::Under, scene_format, samples_, e);
                    if (pipeline == VK_NULL_HANDLE)
                        return false;
                    const VkCommandBuffer cmd = ctx.cmd();
                    const VkRenderingAttachmentInfo colour = ctx.attachment(vt.colour);
                    begin_rendering(cmd, vt.width, vt.height, colour, nullptr);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    set_scissor(cmd, 0, 0, vt.width, vt.height);
                    bool bound = true;
                    for (const std::size_t u : under) {
                        const ViewTargets& w = views[u];
                        set_viewport(cmd, w.x0 - vt.x0, w.y0 - vt.y0, w.x1 - w.x0, w.y1 - w.y0);
                        std::uint32_t root[16] = {};
                        root[4] = transient_srv(ctx.image(w.resolved));
                        root[5] = kPointClamp;
                        if (!(bound = bind_root(cmd, root, e)))
                            break;
                        vkCmdDraw(cmd, 3, 1, 0, 0);
                    }
                    vkCmdEndRendering(cmd);
                    return bound;
                });
            colour_ready = true;
        }

        // The view's history, when this frame saves or draws it (B1): a colour attachment kept across frames at the
        // view's size (a new size makes a new, invalid one) that a save resolves straight into; imported, so the
        // graph keeps its last use across frames.
        ResourceId history = kNoResource;
        bool history_valid = false;
        if (uses_history[v] != 0u) {
            History& h = histories_[v];
            if (!h.image || h.image.width != t.width || h.image.height != t.height) {
                retire_image(device_, h.image);
                h = History{};
                ImageInfo info;
                info.width = t.width;
                info.height = t.height;
                info.format = scene_format;
                info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                info.name = "view history";
                if (!create_image(device_, info, &h.image, error))
                    return false;
            }
            history = graph_.import_image("view history", h.image);
            history_valid = h.valid;
        }

        // A draw pass: its colour cleared or kept (and, in a pair, the glare sources beside it), the view's depth
        // cleared, kept, only tested or - after the view's last depth test - not taken; it reads the snapshots and the
        // history its immediate draws sample, and the skin cache its deformed draws stream.
        const auto add_draw = [&](std::vector<DrawStep> steps, const char* name, const DrawTarget& target,
                                  Load colour_load, const VkClearValue& colour_clear, Load depth_load,
                                  Load sources_load) {
            graph_.add_pass(
                name,
                [&](PassBuilder& b) {
                    b.mergeable();
                    b.write(target.colour, Access::RenderTarget, colour_load, colour_clear);
                    if (target.sources != kNoResource)
                        b.write(target.sources, Access::RenderTarget, sources_load, black);
                    if (target.depth != kNoResource) {
                        if (target.glare_sources)
                            b.read(target.depth, Access::DepthRead);
                        else
                            b.write(target.depth, Access::DepthWrite, depth_load, far_depth);
                    }
                    for (const DrawStep& step : steps) {
                        const render::PassRecord* pass = step.pass;
                        if (pass == nullptr)
                            continue;
                        for (std::uint32_t d = pass->first_draw;
                             d < pass->first_draw + pass->draw_count && d < packet.draws.size(); ++d) {
                            const std::uint32_t i = packet.draws[d].immediate;
                            if (i >= packet.immediates.size())
                                continue;
                            const render::ImmediateDraw& im = packet.immediates[i];
                            ResourceId read = kNoResource;
                            if (im.source == render::ImageSource::History)
                                read = target.history;
                            else if (im.source == render::ImageSource::Snapshot && im.snapshot < target.snapshots.size())
                                read = target.snapshots[im.snapshot];
                            if (read != kNoResource)
                                b.read(read, Access::ShaderRead);
                        }
                        if (skin_cache != kNoResource && pass->draw_count != 0u)
                            b.read(skin_cache, Access::VertexRead);
                    }
                },
                [this, &packet, &frame, &views, steps, v, target](PassContext& ctx, std::string* e) {
                    return record_draw_pass(ctx, packet, steps, v, views[v], target, frame, e);
                });
        };

        // The view's passes in the game's order. The first draw pass clears the depth (and the colour, unless the
        // views under it made it); a view without passes still shows the background. Glare sources go to a black
        // target over the scene's depth until the view's next Glare pass adds their glare to the scene (sources no
        // Glare pass reads are culled). A pass after the view's last depth test does not take the depth (keep depth on
        // tile) - unless it follows a scene pass that took it, so that the two still share one rendering instance (the
        // depth is dropped at that instance's end all the same); with local read, the view's run of scene and read
        // glare sources passes up to its next glare, snapshot or history pass is ONE pair pass, so the depth's last
        // use is inside one rendering instance.
        bool first = true;
        bool depth_open = false;  // the pass declared last is a scene pass that took the depth
        ResourceId glare_sources = kNoResource;
        FoldedGlare folded;
        const auto depth_at = [&](std::size_t p) {
            return !options_.keep_depth_on_tile || depth_open || (last_depth[v] != kNoPass && p <= last_depth[v]);
        };
        const auto scene_pass = [&](const render::PassRecord* pass, std::size_t p) {
            DrawTarget target;
            target.colour = t.colour;
            target.depth = depth_at(p) ? t.depth : kNoResource;
            target.history = history_valid ? history : kNoResource;
            target.snapshots = snapshots;
            add_draw({DrawStep{pass, false}}, pass != nullptr ? "scene" : "view clear", target,
                     first && !colour_ready ? Load::Clear : Load::Keep, background, first ? Load::Clear : Load::Keep,
                     Load::Keep);
            first = false;
            depth_open = target.depth != kNoResource;
        };
        const auto drawn = [&](std::size_t p) {
            const render::PassKind kind = packet.passes[p].kind;
            return kind == render::PassKind::Scene || (kind == render::PassKind::GlareSources && sources_read[p] != 0u);
        };
        for (std::size_t p = 0; p < packet.passes.size(); ++p) {
            const render::PassRecord& pass = packet.passes[p];
            if (pass.view != v)
                continue;
            if (pairing && drawn(p)) {
                // The run of this view's scene and read glare sources passes from here; a pair when it has sources.
                std::vector<DrawStep> steps;
                std::size_t last = p;
                bool has_sources = false;
                for (std::size_t q = p; q < packet.passes.size(); ++q) {
                    if (packet.passes[q].view != v)
                        continue;
                    if (!drawn(q))
                        break;
                    const bool sources = packet.passes[q].kind == render::PassKind::GlareSources;
                    steps.push_back(DrawStep{&packet.passes[q], sources});
                    has_sources = has_sources || sources;
                    last = q;
                }
                if (has_sources) {
                    const bool fresh = glare_sources == kNoResource;
                    if (fresh)
                        glare_sources =
                            frame_texture("glare sources", t.width, t.height, VK_FORMAT_R8G8B8A8_UNORM, samples_);
                    DrawTarget target;
                    target.colour = t.colour;
                    target.sources = glare_sources;
                    target.depth = depth_at(p) ? t.depth : kNoResource;
                    target.history = history_valid ? history : kNoResource;
                    target.snapshots = snapshots;
                    add_draw(std::move(steps), "scene + glare sources", target,
                             first && !colour_ready ? Load::Clear : Load::Keep, background,
                             first ? Load::Clear : Load::Keep, fresh ? Load::Clear : Load::Keep);
                    ++frame.pairs;
                    first = false;
                    depth_open = false;  // two colour attachments: no single scene pass continues it
                    p = last;
                    continue;
                }
            }
            switch (pass.kind) {
            case render::PassKind::Scene:
                scene_pass(&pass, p);
                break;
            case render::PassKind::GlareSources: {
                if (first)
                    scene_pass(nullptr, p);  // the scene's depth exists before it is tested
                const bool fresh = glare_sources == kNoResource;
                if (fresh)
                    glare_sources = frame_texture("glare sources", t.width, t.height, VK_FORMAT_R8G8B8A8_UNORM, samples_);
                DrawTarget target;
                target.colour = glare_sources;
                target.depth = depth_at(p) ? t.depth : kNoResource;
                target.glare_sources = true;
                target.history = history_valid ? history : kNoResource;
                target.snapshots = snapshots;
                add_draw({DrawStep{&pass, true}}, "glare sources", target, fresh ? Load::Clear : Load::Keep, black,
                         Load::Keep, Load::Keep);
                depth_open = false;
                break;
            }
            case render::PassKind::Glare:
                if (glare_sources != kNoResource) {
                    add_glare(packet, v, p, pass.glare, t, glare_sources, folded, frame);
                    glare_sources = kNoResource;
                    depth_open = false;
                }
                break;
            case render::PassKind::Snapshot:
            case render::PassKind::SaveHistory: {
                // The scene colour as it is here, resolved into a snapshot texture or straight into the view's
                // history (a capture that does not commit resolves into a frame texture: the kept one stays).
                const bool save = pass.kind == render::PassKind::SaveHistory;
                const std::uint32_t id = pass_snapshot[p];
                if (save ? history == kNoResource : id >= snapshot_count)
                    break;
                if (first)
                    scene_pass(nullptr, p);  // the scene exists before it is copied
                ResourceId into = kNoResource;
                if (save && commit_history)
                    into = history;
                else
                    into = frame_texture(save ? "history resolve" : "scene snapshot", t.width, t.height, scene_format,
                                         VK_SAMPLE_COUNT_1_BIT);
                // A snapshot makes what its readers sample (none: a corner texel).
                render::PixelBox region;
                const bool part = !save && snapshot_whole[id] == 0u;
                if (part) {
                    const render::TexelRect r =
                        snapshot_region[id].empty() ? render::TexelRect{0, 0, 1, 1} : snapshot_region[id];
                    region = render::PixelBox{r.left, r.top, r.right, r.bottom};
                }
                std::function<void()> done;
                if (save && !commit_history) {
                    // The frame's later draws read the picture it saves; the kept history is left as it is.
                    history = into;
                    history_valid = true;
                } else if (save) {
                    history_valid = true;
                    done = [this, v, &frame] {
                        histories_[v].valid = true;
                        ++frame.history_saves;
                    };
                } else {
                    snapshots[id] = into;
                    done = [&frame] { ++frame.snapshots_made; };
                }
                add_resolve(save ? "save history" : "snapshot", t.colour, into, part ? &region : nullptr,
                            std::move(done));
                depth_open = false;
                break;
            }
            }
        }
        if (first)
            scene_pass(nullptr, packet.passes.size());

        // The view's resolve (PSResolve), with a folded glare composite added per sample (B3a).
        if (folded.active)
            add_folded_resolve(t, folded);
        else
            add_resolve("resolve", t.colour, t.resolved, nullptr);
    }

    // The compose: every view's picture into its rectangle of the output, over the background - turned into the
    // image when the surface is pre-rotated (Screen::Present; no turn: the same picture as the D3D12 compose).
    const ImageUse present = use::kPresent;
    const ResourceId out = graph_.import_image("output", *output.image, output.present ? &present : nullptr);
    graph_.add_pass(
        "compose",
        [&](PassBuilder& b) {
            b.write(out, Access::RenderTarget, Load::Clear, background);
            for (const ViewTargets& t : views)
                b.read(t.resolved, Access::ShaderRead);
        },
        [this, &views, &output, out](PassContext& ctx, std::string* e) {
            Image& image = ctx.image(out);
            const VkPipeline compose = pipelines_.screen(Screen::Present, image.format, VK_SAMPLE_COUNT_1_BIT, e);
            if (compose == VK_NULL_HANDLE)
                return false;
            const VkCommandBuffer cmd = ctx.cmd();
            const VkRenderingAttachmentInfo attachment = ctx.attachment(out);
            begin_rendering(cmd, image.width, image.height, attachment, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose);
            bool bound = true;
            for (const ViewTargets& t : views) {
                const PixelRect at = turned_rect({t.x0, t.y0, t.x1 - t.x0, t.y1 - t.y0}, output.quarter_turns,
                                                 static_cast<float>(output.width), static_cast<float>(output.height));
                set_viewport(cmd, at.x, at.y, at.width, at.height);
                set_scissor(cmd, static_cast<std::int32_t>(at.x), static_cast<std::int32_t>(at.y),
                            static_cast<std::uint32_t>(at.width), static_cast<std::uint32_t>(at.height));
                std::uint32_t root[16] = {};
                root[4] = transient_srv(ctx.image(t.resolved));
                root[5] = kPointClamp;
                root[8] = output.quarter_turns;  // root2.x
                if (!(bound = bind_root(cmd, root, e)))
                    break;
                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
            vkCmdEndRendering(cmd);
            return bound;
        });
    // A capture: the picture copied into the readback buffer and made visible to the CPU (an outside effect).
    if (output.readback != nullptr) {
        graph_.add_pass(
            "readback",
            [&](PassBuilder& b) {
                b.read(out, Access::CopySource);
                b.side_effect();
            },
            [&output, out](PassContext& ctx, std::string*) {
                const Image& image = ctx.image(out);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {image.width, image.height, 1};
                vkCmdCopyImageToBuffer(ctx.cmd(), image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       output.readback->buffer, 1, &region);
                VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &to_host;
                vkCmdPipelineBarrier2(ctx.cmd(), &dependency);
                return true;
            });
    }

    bool recorded = graph_.compile(targets_, error);
    if (recorded) {
        // The GPU time per rendering group: the slot's previous frame is read (its fence was waited for), then this
        // one is measured.
        timer_.begin(cmd, frames_.slot());
        recorded = graph_.execute(cmd, error, timer_.enabled() ? &timer_ : nullptr);
    }
    targets_.end_frame(frame_number_++);
    if (!recorded)
        return false;

    render_stats_.draws = frame.draws;
    render_stats_.particles = frame.particles;
    render_stats_.immediates = frame.immediates;
    render_stats_.skipped_draws = frame.skipped;
    render_stats_.skipped_draws_total += frame.skipped;
    render_stats_.glares = frame.glares;
    render_stats_.glares_folded = frame.glares_folded;
    render_stats_.folded_spans = frame.folded_spans;
    render_stats_.snapshots = frame.snapshots_made;
    render_stats_.history_saves = frame.history_saves;
    render_stats_.skin_jobs = frame.skin_jobs;
    render_stats_.pairs = frame.pairs;
    render_stats_.morph_jobs = frame.morph_jobs;
    render_stats_.skinned_vertices = frame.skinned_vertices;
    render_stats_.pool_images = targets_.size();
    render_stats_.pool_bytes = targets_.bytes();
    render_stats_.pool_created = targets_.created();
    render_stats_.graph = graph_.stats();
    render_stats_.gpu_passes = timer_.spans();
    render_stats_.gpu_ms = timer_.frame_ms();
    render_stats_.gpu_frames = timer_.frames_read();
    render_stats_.pipelines = pipelines_.count();
    ++render_stats_.frames;
    return true;
}

// ---- output ------------------------------------------------------------------------------------------------------

bool Renderer::attach_window(const NativeWindow& window, std::uint32_t width, std::uint32_t height,
                             std::string* error) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    swapchain_.destroy();
    has_window_ = swapchain_.create(device_, window, width, height, PresentMode::VSync, frames_.frames_in_flight(),
                                    error);
    return has_window_;
}

void Renderer::detach_window() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    swapchain_.destroy();
    has_window_ = false;
}

bool Renderer::resize_window(std::uint32_t width, std::uint32_t height, std::string* error) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!has_window_)
        return fail(error, "no window attached");
    return swapchain_.resize(width, height, error);
}

bool Renderer::render(const FramePacket& packet, PresentMode mode, std::string* error) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!has_window_)
        return fail(error, "no window attached");
    (void)mode;  // the swapchain's present mode is chosen at attach_window (stage V5: switch modes by recreating it)
    reading_.store(&packet, std::memory_order_release);
    struct Done {
        std::atomic<const FramePacket*>& r;
        ~Done() { r.store(nullptr, std::memory_order_release); }
    } done{reading_};
    device_.collect();
    bindless_.reclaim(device_.completed());
    if (!frames_.begin(error))
        return false;
    const std::uint32_t slot = frames_.slot();
    std::uint32_t index = 0;
    bool out_of_date = false;
    if (!swapchain_.acquire(slot, &index, &out_of_date, error))
        return false;
    VkCommandBuffer cmd = frames_.cmd();
    if (out_of_date) {
        // Nothing acquired: submit the empty list (the slot's wait stays balanced) and rebuild the swapchain.
        if (!frames_.submit(VK_NULL_HANDLE, VK_NULL_HANDLE, error))
            return false;
        return swapchain_.resize(swapchain_.width(), swapchain_.height(), error);
    }
    Image& target = swapchain_.image(index);
    Output output{&target, swapchain_.width(), swapchain_.height(), swapchain_.quarter_turns(), true, nullptr};
    if (!record(cmd, packet, output, true, error))
        return false;
    if (!frames_.submit(swapchain_.acquired(slot), swapchain_.ready(index), error))
        return false;
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_ = render_stats_;
    }
    if (!swapchain_.present(index, &out_of_date, error))
        return false;
    if (out_of_date)
        return swapchain_.resize(swapchain_.width(), swapchain_.height(), error);
    return true;
}

bool Renderer::render_to_image(const FramePacket& packet, std::uint32_t width, std::uint32_t height,
                               std::vector<std::uint8_t>* rgba, std::string* error, bool commit_history) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (width == 0 || height == 0)
        return fail(error, "render_to_image: empty size");
    reading_.store(&packet, std::memory_order_release);
    struct Done {
        std::atomic<const FramePacket*>& r;
        ~Done() { r.store(nullptr, std::memory_order_release); }
    } done{reading_};
    device_.collect();
    bindless_.reclaim(device_.completed());
    if (capture_.width != width || capture_.height != height) {
        retire_image(device_, capture_);
        retire_buffer(device_, readback_);
        ImageInfo info;
        info.width = width;
        info.height = height;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.name = "capture";
        if (!create_image(device_, info, &capture_, error) ||
            !create_buffer(device_, VkDeviceSize{width} * height * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           HostAccess::Read, &readback_, error))
            return false;
    }
    if (!frames_.begin(error))
        return false;
    VkCommandBuffer cmd = frames_.cmd();
    const std::uint32_t slot = frames_.slot();
    Output output{&capture_, width, height, 0, false, &readback_};
    if (!record(cmd, packet, output, commit_history, error))
        return false;
    if (!frames_.submit(VK_NULL_HANDLE, VK_NULL_HANDLE, error))
        return false;
    if (!device_.wait(device_.submitted(), UINT64_MAX, error))
        return false;
    // This frame's GPU times, now that the GPU has finished it.
    if (timer_.collect(slot)) {
        render_stats_.gpu_passes = timer_.spans();
        render_stats_.gpu_ms = timer_.frame_ms();
        render_stats_.gpu_frames = timer_.frames_read();
    }
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_ = render_stats_;
    }
    if (rgba != nullptr) {
        invalidate_buffer(device_, readback_);
        const auto* bytes = static_cast<const std::uint8_t*>(readback_.mapped);
        rgba->assign(bytes, bytes + std::size_t{width} * height * 4u);
    }
    return true;
}

}  // namespace shadow::render_vk
