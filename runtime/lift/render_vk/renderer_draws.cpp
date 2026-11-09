// render_vk::Renderer's draw passes (renderer.h): a Scene pass's draws into its view's colour and depth, or a
// GlareSources pass's onto black over the scene's depth (tested, not written) - meshes (base pass, then the
// environment or dual second pass; a deformed mesh's stream 0 from the skin cache), particle batches and immediate
// geometry sampling a texture, a snapshot or the view's history - with the D3D12 renderer's shaders, constant blocks
// and rules (render/renderer.cpp add_draw_pass; render/frame_math.h).

#include "render_vk/renderer.h"

#include <algorithm>
#include <cstring>

#include "render/frame_math.h"

namespace shadow::render_vk {

namespace {

using render::Anchor;
using render::kAnchorCount;
using render::Mat4;

VkPrimitiveTopology to_vk(render::Topology topology) {
    switch (topology) {
    case render::Topology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case render::Topology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case render::Topology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case render::Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

DrawPipelineKey key_of(Program program, const render::Recipe& recipe, VkFormat colour, VkSampleCountFlagBits samples,
                       bool lines = false) {
    DrawPipelineKey key;
    key.program = program;
    key.src = recipe.src;
    key.dst = recipe.dst;
    key.cull = recipe.cull;
    key.depth_test = recipe.depth_test;
    key.depth_write = recipe.depth_write;
    key.colour = colour;
    key.samples = samples;
    key.lines = lines;
    return key;
}

void set_box(VkCommandBuffer cmd, const render::PixelBox& box) {
    set_scissor(cmd, static_cast<std::int32_t>(box.left), static_cast<std::int32_t>(box.top),
                static_cast<std::uint32_t>(std::max(0L, box.right - box.left)),
                static_cast<std::uint32_t>(std::max(0L, box.bottom - box.top)));
}

}  // namespace

void Renderer::skip(SkipReason reason, FrameDraws& frame) {
    ++frame.skipped;
    ++render_stats_.skipped_by_reason[static_cast<std::size_t>(reason)];
}

std::uint32_t Renderer::ring_block(const void* data, std::size_t bytes) {
    const VkDeviceSize alignment = std::max<VkDeviceSize>(256, device_.caps().min_uniform_offset_alignment);
    const UploadAllocation block = frames_.allocate(bytes, alignment);
    if (!block)
        return ~0u;
    std::memcpy(block.cpu, data, bytes);
    return static_cast<std::uint32_t>(block.offset);
}

bool Renderer::record_draw_pass(PassContext& ctx, const FramePacket& packet, const std::vector<DrawStep>& steps,
                                std::size_t v, const ViewTargets& t, const DrawTarget& target, FrameDraws& frame,
                                std::string* error) {
    const VkCommandBuffer cmd = ctx.cmd();
    // The attachments as the pass declared them: colour cleared or kept (with the glare sources beside it in a pair);
    // depth cleared, kept, or - the glare sources testing the scene's depth without writing it - read-only, or none
    // after the view's last depth test. The graph begins the instance, or continues the one the draw pass before this
    // one left open (V6).
    const bool pair = target.sources != kNoResource;
    const VkRenderingAttachmentInfo colours[2] = {ctx.attachment(target.colour),
                                                  pair ? ctx.attachment(target.sources) : VkRenderingAttachmentInfo{}};
    const VkRenderingAttachmentInfo depth =
        target.depth != kNoResource ? ctx.attachment(target.depth) : VkRenderingAttachmentInfo{};
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {t.width, t.height}};
    info.layerCount = 1;
    info.colorAttachmentCount = pair ? 2u : 1u;
    info.pColorAttachments = colours;
    info.pDepthAttachment = target.depth != kNoResource ? &depth : nullptr;
    if (!ctx.begin_rendering(info, error))
        return false;
    set_viewport(cmd, 0.0f, 0.0f, static_cast<float>(t.width), static_cast<float>(t.height));
    set_box(cmd, render::PixelBox{0, 0, static_cast<long>(t.width), static_cast<long>(t.height)});
    // Each step's draws in order; in a pair, each step's output goes to its attachment (the other is unused).
    for (const DrawStep& step : steps) {
        if (step.pass == nullptr)
            continue;
        if (pair)
            device_.set_attachment_locations(cmd, 2, step.sources ? kSourcesLocations : kSceneLocations);
        record_draws(ctx, packet, *step.pass, v, t, target, step.sources, frame);
    }
    ctx.end_rendering();
    return true;
}

void Renderer::record_draws(PassContext& ctx, const FramePacket& packet, const render::PassRecord& record,
                            std::size_t v, const ViewTargets& t, const DrawTarget& target, bool sources,
                            FrameDraws& frame) {
    const render::PassRecord* pass = &record;
    const VkCommandBuffer cmd = ctx.cmd();
    const render::ViewRecord& view = packet.views[v];
    const bool pair = target.sources != kNoResource;
    const VkFormat format = ctx.image(pair && sources ? target.sources : target.colour).format;
    const VkFormat other = pair ? ctx.image(sources ? target.colour : target.sources).format : VK_FORMAT_UNDEFINED;
    const float width = static_cast<float>(t.width), height = static_cast<float>(t.height);
    const render::PixelBox full{0, 0, static_cast<long>(t.width), static_cast<long>(t.height)};
    const std::uint32_t end = static_cast<std::uint32_t>(
        std::min<std::size_t>(std::size_t{pass->first_draw} + pass->draw_count, packet.draws.size()));
    // The image an immediate draw samples besides textures: a snapshot made before this pass or the view's history
    // (when it holds a picture); the pass declared the reads, the graph made them readable.
    const auto screen_image = [&](const render::ImmediateDraw& im) -> Image* {
        ResourceId id = kNoResource;
        if (im.source == render::ImageSource::History)
            id = target.history;
        else if (im.source == render::ImageSource::Snapshot && im.snapshot < target.snapshots.size())
            id = target.snapshots[im.snapshot];
        return id != kNoResource ? &ctx.image(id) : nullptr;
    };
    // The pipeline of a draw in this instance: its attachment (and, in a pair, which one), with or without depth.
    const auto key = [&](Program program, const render::Recipe& recipe, bool lines = false) {
        DrawPipelineKey k = key_of(program, recipe, format, samples_, lines);
        k.depth_attachment = target.depth != kNoResource;
        k.pair = !pair ? Pair::None : sources ? Pair::Sources : Pair::Scene;
        k.other = other;
        return k;
    };

    // The pass's own camera, or the view's.
    const float aspect = width / height;
    const float view_canvas_aspect = render::canvas_aspect_of(view.canvas, width, height);
    const render::Camera& camera = pass->has_camera ? pass->camera : view.camera;
    const Mat4 pass_view = render::view_of(camera);
    const Mat4 pass_view_projection = render::multiply(render::projection_of(camera, aspect, view_canvas_aspect),
                                                       pass_view);
    // The view's canvas for each anchor (canvas draws) and its clip.
    std::array<Mat4, kAnchorCount> canvas_placement;
    std::array<render::PixelBox, kAnchorCount> canvas_scissor;
    for (std::size_t a = 0; a < kAnchorCount; ++a) {
        canvas_placement[a] = render::canvas_projection(view.canvas, static_cast<Anchor>(a), width, height);
        canvas_scissor[a] = render::canvas_scissor(view.canvas, static_cast<Anchor>(a), canvas_placement[a], width,
                                                   height);
    }
    const VkBuffer ring = frames_.ring_buffer(frames_.slot());
    const VkDescriptorSet constants_set = bindless_.constants_set(frames_.slot());
    const auto bind_blocks = [&](std::uint32_t lights, std::uint32_t draw_block) {
        const std::uint32_t offsets[3] = {0u, lights, draw_block};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_.pipeline_layout(), 0, 1,
                                &constants_set, 3, offsets);
    };
    VkPipeline bound = VK_NULL_HANDLE;
    const auto bind_pipeline = [&](VkPipeline pipeline) {
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
    };

    for (std::uint32_t d = pass->first_draw; d < end; ++d) {
        const render::DrawRecord& draw = packet.draws[d];
        render::Recipe base_recipe = draw.recipe;
        if (sources)
            base_recipe.depth_write = false;

        if (draw.particles != render::kNoParticles) {
            // A particle batch: its records as a per-instance stream, six vertices per particle.
            const render::ParticleBatch* batch =
                draw.particles < packet.particle_batches.size() ? &packet.particle_batches[draw.particles] : nullptr;
            const VkPipeline pipeline =
                pipelines_.draw(key(Program::Particles, base_recipe), nullptr);
            if (batch == nullptr || batch->count == 0 ||
                std::size_t{batch->first} + batch->count > packet.particles.size() || pipeline == VK_NULL_HANDLE) {
                skip(pipeline == VK_NULL_HANDLE ? SkipReason::ParticlePipeline : SkipReason::ParticleBatch, frame);
                continue;
            }
            const VkDeviceSize bytes = VkDeviceSize{batch->count} * sizeof(render::Particle);
            const UploadAllocation records = frames_.allocate(bytes, 16);
            if (!records) {
                skip(SkipReason::ParticleUpload, frame);
                continue;
            }
            std::memcpy(records.cpu, &packet.particles[batch->first], bytes);
            render::ParticleConstants constants{};
            render::store_column_major(pass_view_projection, constants.view_projection);
            constants.right[0] = batch->right.x;
            constants.right[1] = batch->right.y;
            constants.right[2] = batch->right.z;
            constants.up[0] = batch->up.x;
            constants.up[1] = batch->up.y;
            constants.up[2] = batch->up.z;
            constants.colour[0] = batch->colour.x;
            constants.colour[1] = batch->colour.y;
            constants.colour[2] = batch->colour.z;
            constants.colour[3] = batch->colour.w;
            constants.texture = texture_srv(batch->texture);
            constants.sampler = render::sampler_index(batch->sampler.filter, batch->sampler.u, batch->sampler.v);
            constants.flags = batch->particle_colours ? render::kParticleColours : 0u;
            constants.cutoff = draw.recipe.alpha_cutoff > 0.0f ? draw.recipe.alpha_cutoff : -1.0f;
            constants.fog_enabled = draw.recipe.fog ? 1u : 0u;
            constants.fog_start = draw.recipe.fog_start;
            constants.fog_end = draw.recipe.fog_end;
            constants.fog_colour = render::packed(draw.recipe.fog_colour);
            const std::uint32_t block = ring_block(&constants, sizeof(constants));
            if (block == ~0u) {
                skip(SkipReason::ParticleUpload, frame);
                continue;
            }
            bind_pipeline(pipeline);
            vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            vkCmdBindVertexBuffers(cmd, 0, 1, &ring, &records.offset);
            bind_blocks(frame.no_lights, block);
            vkCmdDraw(cmd, 6, batch->count, 0, 0);
            ++frame.draws;
            frame.particles += batch->count;
            continue;
        }

        if (draw.immediate != render::kNoImmediate) {
            // Immediate geometry: its vertices (and indices) streamed through the frame ring.
            const render::ImmediateDraw* im =
                draw.immediate < packet.immediates.size() ? &packet.immediates[draw.immediate] : nullptr;
            const bool lines = im != nullptr && (im->topology == render::Topology::Lines ||
                                                 im->topology == render::Topology::LineStrip);
            const VkPipeline pipeline =
                pipelines_.draw(key(Program::Immediate, base_recipe, lines), nullptr);
            if (im == nullptr || pipeline == VK_NULL_HANDLE || im->vertex_count == 0 ||
                std::size_t{im->first_vertex} + im->vertex_count > packet.immediate_vertices.size() ||
                std::size_t{im->first_index} + im->index_count > packet.immediate_indices.size()) {
                skip(im != nullptr && pipeline == VK_NULL_HANDLE ? SkipReason::ImmediatePipeline
                                                                 : SkipReason::ImmediateInvalid,
                     frame);
                continue;
            }
            // The image it samples: a texture, a snapshot taken earlier in the frame, the view's history; a
            // snapshot or history that does not exist leaves the draw out (counted).
            std::uint32_t image = kNoDescriptor;
            if (im->source == render::ImageSource::Texture) {
                image = texture_srv(im->texture);
            } else {
                Image* source = screen_image(*im);
                if (source == nullptr) {
                    skip(SkipReason::ImmediateImage, frame);
                    ++render_stats_.image_misses;
                    continue;
                }
                image = transient_srv(*source);
            }
            const VkDeviceSize vertex_bytes = VkDeviceSize{im->vertex_count} * sizeof(render::ImmediateVertex);
            const VkDeviceSize index_bytes = VkDeviceSize{im->index_count} * sizeof(std::uint32_t);
            const UploadAllocation vertices = frames_.allocate(vertex_bytes, 16);
            const UploadAllocation indices = im->index_count != 0 ? frames_.allocate(index_bytes, 16)
                                                                  : UploadAllocation{};
            if (!vertices || (im->index_count != 0 && !indices)) {
                skip(SkipReason::ImmediateUpload, frame);
                continue;
            }
            std::memcpy(vertices.cpu, &packet.immediate_vertices[im->first_vertex], vertex_bytes);
            if (im->index_count != 0)
                std::memcpy(indices.cpu, &packet.immediate_indices[im->first_index], index_bytes);
            render::ImmediateConstants constants{};
            // A canvas draw laid out in a region of the canvas (a player's viewport in split screen) is placed and
            // clipped in that region; any other by its anchor alone.
            Mat4 region_placement;
            render::PixelBox region_scissor;
            const bool in_region = im->space == render::Space::Canvas && im->region != render::kNoRegion &&
                                   im->region < packet.canvas_regions.size();
            if (in_region)
                render::region_projection(view.canvas, packet.canvas_regions[im->region], im->anchor, width, height,
                                          &region_placement, &region_scissor);
            const Mat4& placement = in_region                          ? region_placement
                                    : im->space == render::Space::Canvas ? canvas_placement[static_cast<std::size_t>(im->anchor)]
                                                                         : pass_view_projection;
            render::store_column_major(render::multiply(placement, render::from3x4(draw.world)),
                                       constants.world_view_projection);
            // Texture coordinates: as given, or - a canvas draw of a screen image - canvas points placed like the
            // positions.
            constants.uv_row[0][0] = 1.0f;
            constants.uv_row[1][1] = 1.0f;
            if (im->space == render::Space::Canvas && im->source != render::ImageSource::Texture) {
                const auto rows = render::canvas_image_rows(placement);
                for (int r = 0; r < 2; ++r)
                    for (int k = 0; k < 4; ++k)
                        constants.uv_row[r][k] = rows[r][k];
            }
            constants.texture = image;
            constants.sampler = render::sampler_index(im->sampler.filter, im->sampler.u, im->sampler.v);
            constants.cutoff = base_recipe.alpha_cutoff > 0.0f ? base_recipe.alpha_cutoff : -1.0f;
            constants.fog_enabled = base_recipe.fog ? 1u : 0u;
            constants.fog_start = base_recipe.fog_start;
            constants.fog_end = base_recipe.fog_end;
            constants.fog_colour = render::packed(base_recipe.fog_colour);
            const std::uint32_t block = ring_block(&constants, sizeof(constants));
            if (block == ~0u) {
                skip(SkipReason::ImmediateUpload, frame);
                continue;
            }
            bind_pipeline(pipeline);
            vkCmdSetPrimitiveTopology(cmd, to_vk(im->topology));
            vkCmdBindVertexBuffers(cmd, 0, 1, &ring, &vertices.offset);
            bind_blocks(frame.no_lights, block);
            const bool on_canvas = im->space == render::Space::Canvas;
            if (on_canvas)
                set_box(cmd, in_region ? region_scissor : canvas_scissor[static_cast<std::size_t>(im->anchor)]);
            if (im->index_count != 0) {
                vkCmdBindIndexBuffer(cmd, ring, indices.offset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, im->index_count, 1, 0, 0, 0);
            } else {
                vkCmdDraw(cmd, im->vertex_count, 1, 0, 0);
            }
            if (on_canvas)
                set_box(cmd, full);
            ++frame.draws;
            ++frame.immediates;
            continue;
        }

        // A mesh's submesh: the streams and the submesh copied under the table lock (the slot table may grow).
        VkBuffer streams = VK_NULL_HANDLE;
        VkDeviceSize attributes_offset = 0, indices_offset = 0;
        assets::MeshData::Submesh sub;
        {
            std::lock_guard<std::mutex> lock(table_mutex_);
            const MeshSlot* mesh = mesh_slot(draw.mesh);
            if (mesh != nullptr && draw.submesh < mesh->submeshes.size()) {
                streams = mesh->streams.buffer;
                attributes_offset = mesh->attributes_offset;
                indices_offset = mesh->indices_offset;
                sub = mesh->submeshes[draw.submesh];
            }
        }
        if (streams == VK_NULL_HANDLE) {
            skip(SkipReason::MeshMissing, frame);
            continue;
        }
        if (sub.index_count == 0)
            continue;
        // A deformed draw reads this frame's vertices from the skin cache; a skinned draw without them (no room, no
        // usable palette) is left out - its bind pose at the origin is not the picture.
        const std::uint32_t deformed = d < frame.deformed.size() ? frame.deformed[d] : kNotDeformed;
        if (draw.bones.count != 0u && deformed == kNotDeformed) {
            skip(SkipReason::SkinMissing, frame);
            continue;
        }
        const render::Material& material = draw.material;
        render::DrawConstants constants{};
        // Skinned positions are world space less the pose's origin, which the world matrix puts back; a morphed shape
        // alone is still in model space.
        render::Float3x4 world3 = draw.world;
        if (deformed != kNotDeformed && draw.bones.count != 0u) {
            world3 = render::Float3x4{};
            for (int row = 0; row < 3; ++row)
                world3.m[row][3] = frame.deformed_origin[d][row];
        }
        const Mat4 world = render::from3x4(world3);
        const Mat4 wvp = render::multiply(pass_view_projection, world);
        const Mat4 world_view = render::multiply(pass_view, world);
        render::store_column_major(wvp, constants.world_view_projection);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col) {
                constants.world_row[row][col] = world3.m[row][col];
                constants.view_row[row][col] = col < 3 ? world_view.m[row][col] : 0.0f;
            }
        constants.colour[0] = material.colour.x;
        constants.colour[1] = material.colour.y;
        constants.colour[2] = material.colour.z;
        constants.colour[3] = material.colour.w;
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col)
                constants.uv_row[row][col] = material.uv_transform[row][col];
        constants.texture = texture_srv(material.texture);
        constants.sampler = render::sampler_index(material.sampler.filter, material.sampler.u, material.sampler.v);
        constants.flags = render::flags_of(material);
        constants.uv_set = 0;
        constants.cutoff = draw.recipe.alpha_cutoff > 0.0f ? draw.recipe.alpha_cutoff : -1.0f;
        constants.ka = material.ka;
        constants.coefficient = 0.0f;
        constants.fog_enabled = draw.recipe.fog ? 1u : 0u;
        constants.fog_start = draw.recipe.fog_start;
        constants.fog_end = draw.recipe.fog_end;
        constants.fog_colour = render::packed(draw.recipe.fog_colour);
        constants.dissolve = material.dissolve;  // the fade-in (mesh.hlsl dissolve_test)
        const std::uint32_t lights = draw.lights < frame.light_blocks.size() ? frame.light_blocks[draw.lights]
                                                                             : frame.no_lights;
        const VkBuffer buffers[2] = {deformed != kNotDeformed ? skin_cache_.buffer : streams, streams};
        const VkDeviceSize offsets[2] = {
            deformed != kNotDeformed ? VkDeviceSize{deformed} * sizeof(render::PositionVertex) : 0, attributes_offset};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, streams, indices_offset, VK_INDEX_TYPE_UINT32);

        const auto issue = [&](Program program, const render::Recipe& recipe, const render::DrawConstants& data) {
            const VkPipeline pipeline = pipelines_.draw(key(program, recipe), nullptr);
            const std::uint32_t block = pipeline != VK_NULL_HANDLE ? ring_block(&data, sizeof(data)) : ~0u;
            if (pipeline == VK_NULL_HANDLE || block == ~0u) {
                skip(pipeline == VK_NULL_HANDLE ? SkipReason::MeshPipeline : SkipReason::MeshUpload, frame);
                return;
            }
            bind_pipeline(pipeline);
            vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            bind_blocks(lights, block);
            vkCmdDrawIndexed(cmd, sub.index_count, 1, sub.first_index, 0, 0);
            ++frame.draws;
        };
        const render::ExtraPass& extra = material.extra;
        const std::uint32_t extra_texture = texture_srv(extra.texture);
        if (extra.kind == render::ExtraPass::Kind::BumpEnvironment && extra_texture != kNoDescriptor)
            render::fill_bump_environment(
                extra, extra_texture, render::sampler_index(extra.sampler.filter, extra.sampler.u, extra.sampler.v),
                &constants);
        issue(Program::Base, base_recipe, constants);

        // The second draw of the same mesh: same depth test, no depth writes, its own blend.
        if ((extra.kind == render::ExtraPass::Kind::Environment || extra.kind == render::ExtraPass::Kind::Dual) &&
            extra_texture != kNoDescriptor) {
            render::Recipe recipe = base_recipe;
            recipe.depth_write = false;
            render::DrawConstants second = constants;
            second.texture = extra_texture;
            second.sampler = render::sampler_index(extra.sampler.filter, extra.sampler.u, extra.sampler.v);
            if (extra.kind == render::ExtraPass::Kind::Environment) {
                recipe.src = render::Blend::One;
                recipe.dst = render::Blend::One;
                second.coefficient = extra.coefficient;
                second.cutoff = -1.0f;
                issue(Program::Environment, recipe, second);
            } else {
                recipe.src = extra.src;
                recipe.dst = extra.dst;
                second.uv_set = extra.second_uv_set ? 1u : 0u;
                for (int row = 0; row < 2; ++row)
                    for (int col = 0; col < 3; ++col)
                        second.uv_row[row][col] = extra.uv_transform[row][col];
                second.cutoff = extra.alpha_cutoff > 0.0f ? extra.alpha_cutoff : -1.0f;
                issue(Program::Dual, recipe, second);
            }
        }
    }
}

}  // namespace shadow::render_vk
