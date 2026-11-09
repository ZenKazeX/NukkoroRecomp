// render_vk::Renderer's resources (renderer.h): textures and meshes made from the importer's data, named by
// generational handles, released by scope or after the last packet that names them - render::Renderer's rules
// (render/renderer.cpp "resources"), on Vulkan.
//
// A resource is made on the asking thread: a host-visible staging buffer, a one-time command buffer that copies it into
// the device-local image / buffer and makes it readable by every later frame (the barrier's second scope covers the
// commands submitted after it on the queue), one submission. The staging buffer and the command buffer are released
// once the GPU has passed that submission.

#include "render_vk/renderer.h"

#include <cstring>

#include "render/frame_math.h"
#include "render_vk/deform.h"

namespace shadow::render_vk {

namespace {

std::uint32_t handle_value(std::uint32_t slot, std::uint8_t generation) {
    return (static_cast<std::uint32_t>(generation) << 24) | (slot + 1);
}

std::uint8_t next_generation(std::uint8_t g) {
    return static_cast<std::uint8_t>(g == 255 ? 1 : g + 1);
}

}  // namespace

bool Renderer::upload(VkDeviceSize staging_bytes, const std::function<void(void* staging)>& write,
                      const std::function<void(VkCommandBuffer cmd, VkBuffer staging)>& fill, std::string* error) {
    Buffer staging;
    if (!create_buffer(device_, staging_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostAccess::Write, &staging, error))
        return false;
    write(staging.mapped);
    std::lock_guard<std::mutex> lock(upload_mutex_);
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = upload_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!check(vkAllocateCommandBuffers(device_.vk(), &alloc, &cmd), "vkAllocateCommandBuffers (upload)", error)) {
        destroy_buffer(device_, staging);
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    fill(cmd, staging.buffer);
    const bool submitted = check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer (upload)", error) &&
                           device_.submit(cmd, VK_NULL_HANDLE, VK_PIPELINE_STAGE_2_NONE, VK_NULL_HANDLE, nullptr,
                                          error);
    if (!submitted) {
        vkFreeCommandBuffers(device_.vk(), upload_pool_, 1, &cmd);
        destroy_buffer(device_, staging);
        return false;
    }
    // After the submission: the retirement's mark covers it.
    retire_buffer(device_, staging);
    VkDevice vk = device_.vk();
    VkCommandPool pool = upload_pool_;
    std::mutex* pool_lock = &upload_mutex_;
    device_.retire([vk, pool, cmd, pool_lock] {
        std::lock_guard<std::mutex> l(*pool_lock);
        vkFreeCommandBuffers(vk, pool, 1, &cmd);
    });
    return true;
}

TextureHandle Renderer::create_texture(Scope scope, const assets::TextureImage& image, std::string* error) {
    if (image.width == 0 || image.height == 0 || image.mip_levels == 0 || image.level_offsets.size() < image.mip_levels)
        return fail(error, "empty texture"), TextureHandle{};
    ImageInfo info;
    info.width = image.width;
    info.height = image.height;
    info.mip_levels = image.mip_levels;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.name = "asset texture";
    Image made;
    if (!create_image(device_, info, &made, error))
        return {};
    const bool uploaded = upload(
        image.rgba.size(), [&](void* staging) { std::memcpy(staging, image.rgba.data(), image.rgba.size()); },
        [&](VkCommandBuffer cmd, VkBuffer staging) {
            use_image(cmd, made, use::kCopyDest, true);
            std::vector<VkBufferImageCopy> levels(image.mip_levels);
            for (std::uint32_t l = 0; l < image.mip_levels; ++l) {
                VkBufferImageCopy& r = levels[l];
                r = VkBufferImageCopy{};
                r.bufferOffset = image.level_offsets[l];
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                r.imageExtent = {image.level_width(l), image.level_height(l), 1};
            }
            vkCmdCopyBufferToImage(cmd, staging, made.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image.mip_levels,
                                   levels.data());
            // Readable by every later frame's shaders.
            const ImageUse sampled{VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            use_image(cmd, made, sampled);
        },
        error);
    if (!uploaded) {
        destroy_image(device_, made);
        return {};
    }
    made.srv = bindless_.allocate();
    if (made.srv == kNoDescriptor) {
        retire_image(device_, made);
        return fail(error, "bindless set full (textures)"), TextureHandle{};
    }
    bindless_.write_image(made.srv, made.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    std::lock_guard<std::mutex> lock(table_mutex_);
    std::uint32_t slot = 0;
    if (!free_textures_.empty()) {
        slot = free_textures_.back();
        free_textures_.pop_back();
    } else if (textures_.size() < 0xFFFFFFu) {
        slot = static_cast<std::uint32_t>(textures_.size());
        textures_.emplace_back();
    } else {
        bindless_.free(made.srv, device_.submitted());
        retire_image(device_, made);
        return fail(error, "texture handles exhausted"), TextureHandle{};
    }
    TextureSlot& s = textures_[slot];
    s.image = made;
    s.scope = scope;
    s.live = true;
    return TextureHandle{handle_value(slot, s.generation)};
}

MeshHandle Renderer::create_mesh(Scope scope, const assets::MeshData& mesh, std::string* error) {
    if (mesh.vertex_count == 0 || mesh.indices.empty())
        return fail(error, "empty mesh"), MeshHandle{};
    // The streams exactly as render::Renderer lays them out (render/renderer.cpp make_mesh).
    std::vector<render::PositionVertex> positions(mesh.vertex_count);
    std::vector<render::AttributeVertex> attributes(mesh.vertex_count);
    for (std::uint32_t v = 0; v < mesh.vertex_count; ++v) {
        render::PositionVertex& p = positions[v];
        std::memcpy(p.position, &mesh.positions[std::size_t{v} * 3], sizeof(p.position));
        for (int k = 0; k < 3; ++k)
            p.normal[k] = mesh.normals.empty() ? 0.0f : mesh.normals[std::size_t{v} * 3 + k];
        render::AttributeVertex& a = attributes[v];
        // No colour array: the vertex colour channel is black (the console's channel without a colour source;
        // R2_MATERIAL_CENSUS.md 4.4).
        a.colour = mesh.colours.empty()
                       ? 0xFF000000u
                       : (mesh.colours[std::size_t{v} * 4] | (mesh.colours[std::size_t{v} * 4 + 1] << 8) |
                          (mesh.colours[std::size_t{v} * 4 + 2] << 16) |
                          (static_cast<std::uint32_t>(mesh.colours[std::size_t{v} * 4 + 3]) << 24));
        for (int k = 0; k < 2; ++k) {
            a.uv0[k] = mesh.uv0.empty() ? 0.0f : mesh.uv0[std::size_t{v} * 2 + k];
            a.uv1[k] = mesh.uv1.empty() ? 0.0f : mesh.uv1[std::size_t{v} * 2 + k];
        }
    }
    // A skinned mesh also keeps its bind pose with the weights: the deformation pass poses it each frame. A delta-morphed
    // mesh keeps its base vertices the same way plus its targets (render/renderer.cpp make_mesh).
    const std::size_t vc = mesh.vertex_count;
    const bool skinned = mesh.skin_indices.size() >= vc * 4 && mesh.skin_weights.size() >= vc * 4;
    bool morphed = !mesh.delta_targets.empty() && mesh.delta_targets.size() <= render::kMaxMorphTargets;
    for (const assets::MeshData::DeltaTarget& t : mesh.delta_targets)
        if ((!t.positions.empty() && t.positions.size() != vc * 3) || (!t.normals.empty() && t.normals.size() != vc * 3))
            morphed = false;
    std::vector<SkinVertex> skin;
    if (skinned || morphed) {
        skin.resize(vc);
        for (std::size_t v = 0; v < vc; ++v) {
            SkinVertex& sv = skin[v];
            std::memcpy(sv.position, positions[v].position, sizeof(sv.position));
            std::memcpy(sv.normal, positions[v].normal, sizeof(sv.normal));
            std::memset(sv.weights, 0, sizeof(sv.weights));
            sv.indices = 0;
            if (!skinned)
                continue;
            std::memcpy(sv.weights, &mesh.skin_weights[v * 4], sizeof(sv.weights));
            for (int k = 0; k < 4; ++k)
                sv.indices |= static_cast<std::uint32_t>(mesh.skin_indices[v * 4 + k]) << (8 * k);
        }
    }
    std::vector<MorphDelta> deltas;
    if (morphed) {
        deltas.resize(mesh.delta_targets.size() * vc);
        for (std::size_t t = 0; t < mesh.delta_targets.size(); ++t) {
            const assets::MeshData::DeltaTarget& target = mesh.delta_targets[t];
            for (std::size_t v = 0; v < vc; ++v) {
                MorphDelta& d = deltas[t * vc + v];
                for (int k = 0; k < 3; ++k) {
                    d.position[k] = target.positions.empty() ? 0.0f : target.positions[v * 3 + k];
                    d.normal[k] = target.normals.empty() ? 0.0f : target.normals[v * 3 + k];
                }
            }
        }
    }
    const auto align = [](VkDeviceSize x) { return (x + 255) / 256 * 256; };
    const VkDeviceSize pbytes = positions.size() * sizeof(render::PositionVertex);
    const VkDeviceSize abytes = attributes.size() * sizeof(render::AttributeVertex);
    const VkDeviceSize ibytes = mesh.indices.size() * sizeof(std::uint32_t);
    MeshSlot made;
    made.attributes_offset = align(pbytes);
    made.indices_offset = align(made.attributes_offset + abytes);
    const VkDeviceSize total = made.indices_offset + ibytes;
    if (!create_buffer(device_, total,
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       HostAccess::None, &made.streams, error))
        return {};
    const VkDeviceSize skin_bytes = skin.size() * sizeof(SkinVertex);
    const VkDeviceSize delta_bytes = deltas.size() * sizeof(MorphDelta);
    const auto give_back = [&] {
        destroy_buffer(device_, made.streams);
        destroy_buffer(device_, made.skin);
        destroy_buffer(device_, made.morph);
    };
    const VkBufferUsageFlags source_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((skin_bytes != 0 && !create_buffer(device_, skin_bytes, source_usage, HostAccess::None, &made.skin, error)) ||
        (delta_bytes != 0 && !create_buffer(device_, delta_bytes, source_usage, HostAccess::None, &made.morph, error))) {
        give_back();
        return {};
    }
    const VkDeviceSize skin_at = align(total), delta_at = align(skin_at + skin_bytes);
    const bool uploaded = upload(
        delta_at + delta_bytes,
        [&](void* staging) {
            auto* bytes = static_cast<std::uint8_t*>(staging);
            std::memcpy(bytes, positions.data(), pbytes);
            std::memcpy(bytes + made.attributes_offset, attributes.data(), abytes);
            std::memcpy(bytes + made.indices_offset, mesh.indices.data(), ibytes);
            if (skin_bytes != 0)
                std::memcpy(bytes + skin_at, skin.data(), skin_bytes);
            if (delta_bytes != 0)
                std::memcpy(bytes + delta_at, deltas.data(), delta_bytes);
        },
        [&](VkCommandBuffer cmd, VkBuffer staging) {
            const VkBufferCopy whole{0, 0, total};
            vkCmdCopyBuffer(cmd, staging, made.streams.buffer, 1, &whole);
            if (skin_bytes != 0) {
                const VkBufferCopy part{skin_at, 0, skin_bytes};
                vkCmdCopyBuffer(cmd, staging, made.skin.buffer, 1, &part);
            }
            if (delta_bytes != 0) {
                const VkBufferCopy part{delta_at, 0, delta_bytes};
                vkCmdCopyBuffer(cmd, staging, made.morph.buffer, 1, &part);
            }
            // Readable as vertex and index streams (and by the V4 deformation pass) by every later frame.
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = VK_NULL_HANDLE;
            barrier.size = VK_WHOLE_SIZE;
            VkBufferMemoryBarrier2 barriers[3];
            std::uint32_t count = 0;
            for (VkBuffer b : {made.streams.buffer, made.skin.buffer, made.morph.buffer}) {
                if (b == VK_NULL_HANDLE)
                    continue;
                barriers[count] = barrier;
                barriers[count++].buffer = b;
            }
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = count;
            dependency.pBufferMemoryBarriers = barriers;
            vkCmdPipelineBarrier2(cmd, &dependency);
        },
        error);
    if (!uploaded) {
        give_back();
        return {};
    }
    made.vertex_count = mesh.vertex_count;
    made.submeshes = mesh.submeshes;
    made.skinned = skinned;
    made.morph_targets = morphed ? static_cast<std::uint32_t>(mesh.delta_targets.size()) : 0u;
    // The deformation pass reads the sources through the bindless set's storage-buffer binding.
    if (made.skin) {
        made.skin_srv = bindless_.allocate();
        bindless_.write_buffer(made.skin_srv, made.skin.buffer, 0, VK_WHOLE_SIZE);
    }
    if (made.morph) {
        made.morph_srv = bindless_.allocate();
        bindless_.write_buffer(made.morph_srv, made.morph.buffer, 0, VK_WHOLE_SIZE);
    }
    if ((made.skin && made.skin_srv == kNoDescriptor) || (made.morph && made.morph_srv == kNoDescriptor)) {
        bindless_.free(made.skin_srv, device_.submitted());
        bindless_.free(made.morph_srv, device_.submitted());
        retire_buffer(device_, made.streams);
        retire_buffer(device_, made.skin);
        retire_buffer(device_, made.morph);
        return fail(error, "bindless set full (mesh sources)"), MeshHandle{};
    }

    std::lock_guard<std::mutex> lock(table_mutex_);
    std::uint32_t slot = 0;
    if (!free_meshes_.empty()) {
        slot = free_meshes_.back();
        free_meshes_.pop_back();
    } else if (meshes_.size() < 0xFFFFFFu) {
        slot = static_cast<std::uint32_t>(meshes_.size());
        meshes_.emplace_back();
    } else {
        bindless_.free(made.skin_srv, device_.submitted());
        bindless_.free(made.morph_srv, device_.submitted());
        retire_buffer(device_, made.streams);
        retire_buffer(device_, made.skin);
        retire_buffer(device_, made.morph);
        return fail(error, "mesh handles exhausted"), MeshHandle{};
    }
    MeshSlot& s = meshes_[slot];
    const std::uint8_t generation = s.generation;
    s = std::move(made);
    s.generation = generation;
    s.scope = scope;
    s.live = true;
    return MeshHandle{handle_value(slot, generation)};
}

Renderer::TextureSlot* Renderer::texture_slot(TextureHandle handle) {
    if (!handle)
        return nullptr;
    const std::uint32_t slot = (handle.value & 0xFFFFFFu) - 1;
    if (slot >= textures_.size())
        return nullptr;
    TextureSlot& s = textures_[slot];
    return s.live && s.generation == (handle.value >> 24) ? &s : nullptr;
}

Renderer::MeshSlot* Renderer::mesh_slot(MeshHandle handle) {
    if (!handle)
        return nullptr;
    const std::uint32_t slot = (handle.value & 0xFFFFFFu) - 1;
    if (slot >= meshes_.size())
        return nullptr;
    MeshSlot& s = meshes_[slot];
    return s.live && s.generation == (handle.value >> 24) ? &s : nullptr;
}

std::uint32_t Renderer::texture_srv(TextureHandle handle) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    const TextureSlot* s = texture_slot(handle);
    return s != nullptr ? s->image.srv : kNoDescriptor;
}

void Renderer::release(TextureHandle handle, std::uint64_t named_through) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    if (named_through != kNoPacket && !(packets_finished_ && named_through <= finished_through_) &&
        texture_slot(handle) != nullptr) {
        pending_releases_.push_back(PendingRelease{true, handle.value, named_through});
        ++deferred_releases_;
        return;
    }
    release_locked(handle);
}

void Renderer::release(MeshHandle handle, std::uint64_t named_through) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    if (named_through != kNoPacket && !(packets_finished_ && named_through <= finished_through_) &&
        mesh_slot(handle) != nullptr) {
        pending_releases_.push_back(PendingRelease{false, handle.value, named_through});
        ++deferred_releases_;
        return;
    }
    release_locked(handle);
}

void Renderer::finish_packets(std::uint64_t tick) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    if (!packets_finished_ || tick > finished_through_)
        finished_through_ = tick;
    packets_finished_ = true;
    std::size_t kept = 0;
    for (const PendingRelease& p : pending_releases_) {
        if (p.named_through > finished_through_) {
            pending_releases_[kept++] = p;
            continue;
        }
        if (p.texture)
            release_locked(TextureHandle{p.handle});
        else
            release_locked(MeshHandle{p.handle});
    }
    pending_releases_.resize(kept);
}

void Renderer::release_locked(TextureHandle handle) {
    TextureSlot* s = texture_slot(handle);
    if (s == nullptr)
        return;
    // The image and its index go once the GPU has passed every frame submitted so far.
    bindless_.free(s->image.srv, device_.submitted());
    retire_image(device_, s->image);
    s->live = false;
    s->generation = next_generation(s->generation);
    free_textures_.push_back((handle.value & 0xFFFFFFu) - 1);
}

void Renderer::release_locked(MeshHandle handle) {
    MeshSlot* s = mesh_slot(handle);
    if (s == nullptr)
        return;
    retire_buffer(device_, s->streams);
    bindless_.free(s->skin_srv, device_.submitted());
    bindless_.free(s->morph_srv, device_.submitted());
    retire_buffer(device_, s->skin);
    retire_buffer(device_, s->morph);
    s->skin_srv = s->morph_srv = kNoDescriptor;
    s->skinned = false;
    s->morph_targets = 0;
    s->submeshes.clear();
    s->live = false;
    s->generation = next_generation(s->generation);
    free_meshes_.push_back((handle.value & 0xFFFFFFu) - 1);
}

void Renderer::release_scope(Scope scope) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    for (std::uint32_t i = 0; i < textures_.size(); ++i)
        if (textures_[i].live && textures_[i].scope == scope)
            release_locked(TextureHandle{handle_value(i, textures_[i].generation)});
    for (std::uint32_t i = 0; i < meshes_.size(); ++i)
        if (meshes_[i].live && meshes_[i].scope == scope)
            release_locked(MeshHandle{handle_value(i, meshes_[i].generation)});
}

Renderer::Stats Renderer::stats() const {
    Stats out;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        out = stats_;
    }
    std::lock_guard<std::mutex> lock(table_mutex_);
    out.textures = 0;
    for (const TextureSlot& s : textures_)
        out.textures += s.live ? 1u : 0u;
    out.meshes = 0;
    for (const MeshSlot& s : meshes_)
        out.meshes += s.live ? 1u : 0u;
    out.deferred_releases = deferred_releases_;
    out.pending_releases = pending_releases_.size();
    return out;
}

}  // namespace shadow::render_vk
