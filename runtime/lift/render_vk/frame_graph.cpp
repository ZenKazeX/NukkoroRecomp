#include "render_vk/frame_graph.h"

#include <algorithm>
#include <unordered_map>

#include "render_vk/gpu_timer.h"

namespace shadow::render_vk {

namespace {

constexpr VkAccessFlags2 kWriteAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

// A buffer's stage and access for a use (no layout).
ImageUse buffer_use(Access access) noexcept {
    switch (access) {
    case Access::UnorderedAccess:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
    case Access::VertexRead:
        return {VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
    case Access::ShaderRead:
        return {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
    case Access::CopySource:
        return {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
    case Access::CopyDest:
        return {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
    default:
        return {};
    }
}

bool is_buffer_access(Access access) noexcept {
    return access == Access::UnorderedAccess || access == Access::VertexRead || access == Access::ShaderRead ||
           access == Access::CopySource || access == Access::CopyDest;
}

bool is_image_access(Access access) noexcept {
    return access != Access::VertexRead;
}

bool is_attachment(Access access) noexcept {
    return access == Access::RenderTarget || access == Access::DepthWrite || access == Access::DepthRead;
}

// The stage a barrier waits for or blocks: an image not used yet this run (a new image, a swapchain image after its
// present) waits for everything before it - for a swapchain image that includes the acquire semaphore's wait.
VkPipelineStageFlags2 or_all(VkPipelineStageFlags2 stage) noexcept {
    return stage == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : stage;
}

std::uint32_t texel_bytes(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    default:
        return 4;
    }
}

}  // namespace

const char* access_name(Access access) noexcept {
    switch (access) {
    case Access::RenderTarget: return "RenderTarget";
    case Access::DepthWrite: return "DepthWrite";
    case Access::DepthRead: return "DepthRead";
    case Access::ShaderRead: return "ShaderRead";
    case Access::UnorderedAccess: return "UnorderedAccess";
    case Access::CopySource: return "CopySource";
    case Access::CopyDest: return "CopyDest";
    case Access::VertexRead: return "VertexRead";
    }
    return "?";
}

bool is_write(Access access) noexcept {
    return access == Access::RenderTarget || access == Access::DepthWrite || access == Access::UnorderedAccess ||
           access == Access::CopyDest;
}

ImageUse image_use(Access access) noexcept {
    switch (access) {
    case Access::RenderTarget: return use::kColourWrite;
    case Access::DepthWrite: return use::kDepthWrite;
    case Access::DepthRead: return use::kDepthRead;
    case Access::ShaderRead: return use::kShaderRead;
    case Access::UnorderedAccess:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL};
    case Access::CopySource: return use::kCopySource;
    case Access::CopyDest: return use::kCopyDest;
    case Access::VertexRead: break;
    }
    return {};
}

VkImageUsageFlags pooled_usage(const TextureDesc& desc) noexcept {
    if (is_depth_format(desc.format))
        return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               (desc.transient ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT : 0u);
    if (desc.transient)
        return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc.storage)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    return usage;
}

// ---- TargetPool -------------------------------------------------------------------------------------------------

TargetPool::~TargetPool() {
    destroy();
}

Image* TargetPool::acquire(const TextureDesc& desc, std::uint64_t frame, const char* name, std::string* error) {
    for (const std::unique_ptr<Entry>& entry : entries_) {
        if (!entry->in_use && compatible(entry->desc, desc)) {
            entry->in_use = true;
            entry->last_frame = frame;
            return &entry->image;
        }
    }
    if (device_ == nullptr) {
        fail(error, "target pool not attached to a device");
        return nullptr;
    }
    auto entry = std::make_unique<Entry>();
    ImageInfo info;
    info.width = desc.width;
    info.height = desc.height;
    info.format = desc.format;
    info.samples = desc.samples;
    info.usage = pooled_usage(desc) & ~VkImageUsageFlags{VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT};
    info.transient = desc.transient;  // create_image adds the usage and asks for lazily allocated memory
    info.name = name;
    if (!create_image(*device_, info, &entry->image, error))
        return nullptr;
    entry->desc = desc;
    entry->in_use = true;
    entry->last_frame = frame;
    entries_.push_back(std::move(entry));
    ++created_;
    return &entries_.back()->image;
}

void TargetPool::end_frame(std::uint64_t frame, std::uint64_t keep_frames) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        Entry& entry = **it;
        entry.in_use = false;
        if (frame > entry.last_frame + keep_frames) {
            retire_image(*device_, entry.image);
            it = entries_.erase(it);
            ++released_;
        } else {
            ++it;
        }
    }
}

void TargetPool::destroy() {
    if (device_ != nullptr)
        for (const std::unique_ptr<Entry>& entry : entries_)
            destroy_image(*device_, entry->image);
    entries_.clear();
}

VkDeviceSize TargetPool::bytes() const noexcept {
    VkDeviceSize total = 0;
    for (const std::unique_ptr<Entry>& entry : entries_)
        total += VkDeviceSize{entry->desc.width} * entry->desc.height * static_cast<std::uint32_t>(entry->desc.samples) *
                 texel_bytes(entry->desc.format);
    return total;
}

// ---- PassBuilder / PassContext ------------------------------------------------------------------------------------

void PassBuilder::read(ResourceId resource, Access access) {
    graph_.add_use(pass_, resource, access, Load::Keep, false, VkClearValue{});
}

void PassBuilder::write(ResourceId resource, Access access, Load load, const VkClearValue& clear) {
    graph_.add_use(pass_, resource, access, load, true, clear);
}

void PassBuilder::side_effect() {
    graph_.passes_[pass_].side_effect = true;
}

void PassBuilder::mergeable() {
    graph_.passes_[pass_].mergeable = true;
}

Image& PassContext::image(ResourceId id) const {
    return *graph_.resources_[id].image;
}

VkBuffer PassContext::buffer(ResourceId id) const {
    return graph_.resources_[id].buffer;
}

VkRenderingAttachmentInfo PassContext::attachment(ResourceId id) const {
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    const FrameGraph::Pass& pass = graph_.passes_[pass_];
    // The store op is the instance's: the last pass merged into it decides (the same attachments, same accesses).
    const FrameGraph::Pass& last = graph_.passes_[pass.group_last];
    for (std::size_t u = 0; u < pass.uses.size(); ++u) {
        const FrameGraph::Use& use = pass.uses[u];
        if (use.resource != id)
            continue;
        a.imageView = graph_.resources_[id].image->view;
        a.imageLayout = image_use(use.access).layout;
        a.loadOp = use.load == Load::Clear     ? VK_ATTACHMENT_LOAD_OP_CLEAR
                   : use.load == Load::Discard ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                               : VK_ATTACHMENT_LOAD_OP_LOAD;
        bool stored = pass.stored[u] != 0u;
        for (std::size_t l = 0; l < last.uses.size(); ++l)
            if (last.uses[l].resource == id)
                stored = last.stored[l] != 0u;
        a.storeOp = use.access == Access::DepthRead ? VK_ATTACHMENT_STORE_OP_NONE
                    : stored                        ? VK_ATTACHMENT_STORE_OP_STORE
                                                    : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.clearValue = use.clear;
        break;
    }
    return a;
}

bool PassContext::begin_rendering(const VkRenderingInfo& info, std::string* error) {
    const FrameGraph::Pass& pass = graph_.passes_[pass_];
    std::vector<VkImageView> views;
    for (std::uint32_t c = 0; c < info.colorAttachmentCount; ++c)
        views.push_back(info.pColorAttachments[c].imageView);
    views.push_back(info.pDepthAttachment != nullptr ? info.pDepthAttachment->imageView : VK_NULL_HANDLE);
    if (pass.continues) {
        // The instance the previous pass left open: the same attachments over the same area, or the merge was wrong.
        const bool same = graph_.open_ && views == graph_.open_views_ &&
                          info.renderArea.offset.x == graph_.open_area_.offset.x &&
                          info.renderArea.offset.y == graph_.open_area_.offset.y &&
                          info.renderArea.extent.width == graph_.open_area_.extent.width &&
                          info.renderArea.extent.height == graph_.open_area_.extent.height;
        if (!same)
            return graph_.fail(error, "pass '" + pass.name + "' was merged but renders to other attachments or area");
        return true;
    }
    if (graph_.open_)
        return graph_.fail(error, "pass '" + pass.name + "' begins rendering inside an open instance");
    vkCmdBeginRendering(cmd_, &info);
    graph_.open_ = true;
    graph_.open_views_ = std::move(views);
    graph_.open_area_ = info.renderArea;
    return true;
}

void PassContext::end_rendering() {
    if (graph_.passes_[pass_].leaves_open || !graph_.open_)
        return;
    vkCmdEndRendering(cmd_);
    graph_.open_ = false;
}

// ---- FrameGraph ---------------------------------------------------------------------------------------------------

void FrameGraph::reset(std::uint64_t frame) {
    frame_ = frame;
    resources_.clear();
    passes_.clear();
    barriers_.clear();
    closing_ = Planned{};
    end_states_.clear();
    stats_ = Stats{};
    compiled_ = false;
    build_error_.clear();
    open_ = false;
}

ResourceId FrameGraph::create_texture(const char* name, const TextureDesc& desc) {
    Resource resource;
    resource.name = name;
    resource.kind = Kind::Created;
    resource.desc = desc;
    resources_.push_back(std::move(resource));
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId FrameGraph::import_image(const char* name, Image& image, const ImageUse* final_use) {
    Resource resource;
    resource.name = name;
    resource.kind = Kind::ImportedImage;
    resource.desc = TextureDesc{image.width, image.height, image.format, image.samples, false};
    resource.image = &image;
    if (final_use != nullptr) {
        resource.has_final = true;
        resource.final_use = *final_use;
    }
    resources_.push_back(std::move(resource));
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId FrameGraph::import_buffer(const char* name, VkBuffer buffer, Access initial, Access final) {
    Resource resource;
    resource.name = name;
    resource.kind = Kind::ImportedBuffer;
    resource.buffer = buffer;
    resource.buffer_state = buffer_use(initial);
    resource.buffer_final = buffer_use(final);
    resource.has_final = true;
    resources_.push_back(std::move(resource));
    return static_cast<ResourceId>(resources_.size() - 1);
}

void FrameGraph::add_use(std::uint32_t pass, ResourceId resource, Access access, Load load, bool write,
                         const VkClearValue& clear) {
    Pass& p = passes_[pass];
    const auto refuse = [&](const std::string& why) {
        if (build_error_.empty())
            build_error_ = "pass '" + p.name + "': " + why;
    };
    if (resource >= resources_.size())
        return refuse("an unknown resource");
    const Resource& r = resources_[resource];
    if (is_write(access) != write)
        return refuse(std::string("declares ") + access_name(access) + " of '" + r.name + "' as a " +
                      (write ? "write" : "read"));
    if (r.kind == Kind::ImportedBuffer ? !is_buffer_access(access) : !is_image_access(access))
        return refuse(std::string(access_name(access)) + " cannot be a use of '" + r.name + "'");
    if (r.kind != Kind::ImportedBuffer) {
        const bool depth = is_depth_format(r.desc.format);
        const bool depth_access = access == Access::DepthWrite || access == Access::DepthRead;
        if (depth != depth_access)
            return refuse(std::string(access_name(access)) + " of the " + (depth ? "depth" : "colour") + " texture '" +
                          r.name + "'");
        if (access == Access::UnorderedAccess && r.kind == Kind::Created && !r.desc.storage)
            return refuse("'" + r.name + "' is not a storage texture");
    }
    if (load == Load::Clear && access != Access::RenderTarget && access != Access::DepthWrite)
        return refuse("only render-target and depth writes can clear");
    // A pass uses a resource one way: the same read twice is one read; anything else is refused (one layout at a
    // time - the read a Load::Keep write implies is part of the write).
    for (const Use& u : p.uses) {
        if (u.resource != resource)
            continue;
        if (!u.write && !write && u.access == access)
            return;
        return refuse("uses '" + r.name + "' both as " + access_name(u.access) + " and as " + access_name(access));
    }
    p.uses.push_back(Use{resource, access, load, write, clear});
}

bool FrameGraph::fail(std::string* error, const std::string& message) {
    if (error != nullptr)
        *error = message;
    compiled_ = false;
    return false;
}

bool FrameGraph::compile(TargetPool& pool, std::string* error) {
    barriers_.clear();
    closing_ = Planned{};
    end_states_.clear();
    stats_ = Stats{};
    compiled_ = false;
    if (!build_error_.empty())
        return fail(error, build_error_);
    stats_.passes = static_cast<std::uint32_t>(passes_.size());

    // Cull, walking backwards: a pass is kept when it has an outside effect, writes an imported resource, or writes
    // something a kept later pass reads (render/frame_graph.cpp, the same rule).
    std::vector<bool> needed(resources_.size(), false);
    for (std::size_t n = passes_.size(); n-- > 0;) {
        Pass& pass = passes_[n];
        bool keep = pass.side_effect;
        for (const Use& use : pass.uses)
            if (use.write && (resources_[use.resource].kind != Kind::Created || needed[use.resource]))
                keep = true;
        pass.kept = keep;
        if (!keep)
            continue;
        for (const Use& use : pass.uses)
            if (use.write && use.load != Load::Keep)
                needed[use.resource] = false;  // overwritten: earlier contents are not observed
        for (const Use& use : pass.uses)
            if (!use.write || use.load == Load::Keep)
                needed[use.resource] = true;
    }

    // A graph-owned texture must be written (cleared or overwritten) before anything reads or adds to it.
    std::vector<bool> written(resources_.size(), false);
    for (Pass& pass : passes_) {
        if (!pass.kept)
            continue;
        ++stats_.kept;
        for (const Use& use : pass.uses) {
            Resource& resource = resources_[use.resource];
            resource.used = true;
            if (resource.kind != Kind::Created || written[use.resource])
                continue;
            if (!use.write)
                return fail(error, "pass '" + pass.name + "' reads '" + resource.name + "' before any pass wrote it");
            if (use.load == Load::Keep)
                return fail(error, "pass '" + pass.name + "' adds to '" + resource.name +
                                       "' before any pass wrote it (clear or overwrite it first)");
        }
        for (const Use& use : pass.uses)
            if (use.write)
                written[use.resource] = true;
    }
    stats_.culled = stats_.passes - stats_.kept;

    // Transient textures: every kept use an attachment (nothing samples or copies them), so their images need no
    // memory beyond the tile where the GPU has lazily allocated memory (V6).
    for (std::size_t r = 0; r < resources_.size(); ++r) {
        Resource& resource = resources_[r];
        if (resource.kind != Kind::Created)
            continue;
        resource.desc.transient = false;
        if (!transient_ || !resource.used || resource.desc.storage)
            continue;
        bool attachment_only = true;
        for (const Pass& pass : passes_)
            if (pass.kept)
                for (const Use& use : pass.uses)
                    if (use.resource == r && !is_attachment(use.access))
                        attachment_only = false;
        resource.desc.transient = attachment_only;
        if (attachment_only)
            ++stats_.transient;
    }

    // Pooled images for the graph-owned textures the kept passes use. A texture whose first kept pass comes after
    // another's last takes over its image: its first use writes it whole (checked above), so nothing it reads was
    // left by the one before.
    std::vector<std::uint32_t> first_use(resources_.size(), ~0u), last_use(resources_.size(), 0);
    for (std::size_t n = 0; n < passes_.size(); ++n) {
        if (!passes_[n].kept)
            continue;
        for (const Use& use : passes_[n].uses) {
            first_use[use.resource] = std::min(first_use[use.resource], static_cast<std::uint32_t>(n));
            last_use[use.resource] = std::max(last_use[use.resource], static_cast<std::uint32_t>(n));
        }
    }
    std::vector<std::size_t> order;
    for (std::size_t r = 0; r < resources_.size(); ++r)
        if (resources_[r].kind == Kind::Created && resources_[r].used)
            order.push_back(r);
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return first_use[a] < first_use[b]; });
    struct Held {
        Image* image;
        TextureDesc desc;
        std::uint32_t until;  // the holder's last kept pass
    };
    std::vector<Held> held;
    for (const std::size_t r : order) {
        Resource& resource = resources_[r];
        ++stats_.textures;
        resource.image = nullptr;
        for (Held& h : held) {
            if (h.until < first_use[r] && TargetPool::compatible(h.desc, resource.desc)) {
                resource.image = h.image;
                resource.aliased = true;
                h.until = last_use[r];
                ++stats_.aliased;
                break;
            }
        }
        if (resource.image == nullptr) {
            std::string reason;
            resource.image = pool.acquire(resource.desc, frame_, resource.name.c_str(), &reason);
            if (resource.image == nullptr)
                return fail(error, "texture '" + resource.name + "': " + reason);
            held.push_back(Held{resource.image, resource.desc, last_use[r]});
            ++stats_.images;
        }
    }

    // Barriers, pass by pass in the authored order, tracked per physical image (a texture that took over an image
    // starts from the use the one before left it in) and per imported buffer.
    std::unordered_map<Image*, ImageUse> image_state;
    std::vector<ImageUse> buffer_state(resources_.size());
    for (std::size_t r = 0; r < resources_.size(); ++r)
        if (resources_[r].kind == Kind::ImportedBuffer)
            buffer_state[r] = resources_[r].buffer_state;
    const auto state_of = [&](Image* image) -> ImageUse& {
        auto found = image_state.find(image);
        if (found == image_state.end())
            found = image_state.emplace(image, image->state).first;
        return found->second;
    };
    const auto plan_image = [&](Planned& planned, std::uint32_t pass, ResourceId id, const ImageUse& next,
                                bool discard) {
        Image& image = *resources_[id].image;
        ImageUse& last = state_of(&image);
        const bool last_wrote = (last.access & kWriteAccess) != 0;
        const bool next_writes = (next.access & kWriteAccess) != 0;
        if (!discard && !last_wrote && !next_writes && last.layout == next.layout) {
            // A read after reads in the same layout: no barrier, the readers accumulate (a later write waits for all).
            last.stage |= next.stage;
            last.access |= next.access;
            return;
        }
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = or_all(last.stage);
        b.srcAccessMask = last.access;
        b.dstStageMask = or_all(next.stage);
        b.dstAccessMask = next.access;
        b.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : last.layout;
        b.newLayout = next.layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.image;
        b.subresourceRange = {image.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
        planned.images.push_back(b);
        barriers_.push_back(Barrier{pass, id, last, next, discard});
        last = next;
    };
    const auto plan_buffer = [&](Planned& planned, std::uint32_t pass, ResourceId id, const ImageUse& next) {
        ImageUse& last = buffer_state[id];
        const bool last_wrote = (last.access & kWriteAccess) != 0;
        const bool next_writes = (next.access & kWriteAccess) != 0;
        if (!last_wrote && !next_writes) {
            last.stage |= next.stage;
            last.access |= next.access;
            return;
        }
        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask = or_all(last.stage);
        b.srcAccessMask = last.access;
        b.dstStageMask = or_all(next.stage);
        b.dstAccessMask = next.access;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = resources_[id].buffer;
        b.size = VK_WHOLE_SIZE;
        planned.buffers.push_back(b);
        barriers_.push_back(Barrier{pass, id, last, next, false});
        last = next;
    };
    for (std::size_t n = 0; n < passes_.size(); ++n) {
        Pass& pass = passes_[n];
        pass.planned = Planned{};
        pass.stored.assign(pass.uses.size(), 1);
        if (!pass.kept)
            continue;
        for (std::size_t u = 0; u < pass.uses.size(); ++u) {
            const Use& use = pass.uses[u];
            const Resource& resource = resources_[use.resource];
            if (resource.kind == Kind::ImportedBuffer) {
                plan_buffer(pass.planned, static_cast<std::uint32_t>(n), use.resource, buffer_use(use.access));
                continue;
            }
            plan_image(pass.planned, static_cast<std::uint32_t>(n), use.resource, image_use(use.access),
                       use.write && use.load != Load::Keep);
            // An attachment is stored when a later kept pass uses the texture, or the graph does not own it.
            pass.stored[u] = resource.kind != Kind::Created || last_use[use.resource] > n ? 1u : 0u;
        }
        if (!pass.planned.images.empty() || !pass.planned.buffers.empty())
            ++stats_.barrier_calls;
    }
    // Merges (V6): a mergeable pass continues the previous kept pass's rendering instance when both are mergeable, use
    // the same attachments the same way, the later one keeps their contents (no Clear / Discard) and its only
    // barriers are between attachment writes in an unchanged layout - inside one instance rasterization order
    // already orders those, so they are dropped. Anything else (a first read of a snapshot, a layout change, a
    // buffer) ends the instance.
    const auto attachments_of = [&](const Pass& pass) {
        std::vector<std::pair<ResourceId, Access>> list;
        for (const Use& use : pass.uses)
            if (is_attachment(use.access))
                list.emplace_back(use.resource, use.access);
        std::sort(list.begin(), list.end());
        return list;
    };
    std::size_t previous = passes_.size();
    for (std::size_t n = 0; n < passes_.size(); ++n) {
        Pass& pass = passes_[n];
        pass.continues = false;
        pass.leaves_open = false;
        pass.group_last = static_cast<std::uint32_t>(n);
        if (!pass.kept)
            continue;
        bool merge = merging_ && pass.mergeable && previous < passes_.size() && passes_[previous].mergeable &&
                     pass.planned.buffers.empty();
        std::vector<std::pair<ResourceId, Access>> attachments;
        if (merge) {
            attachments = attachments_of(pass);
            merge = !attachments.empty() && attachments == attachments_of(passes_[previous]);
        }
        if (merge)
            for (const Use& use : pass.uses)
                if (is_attachment(use.access) && use.load != Load::Keep)
                    merge = false;
        if (merge)
            for (const Barrier& b : barriers_)
                if (b.pass == n && (b.discard || b.before.layout != b.after.layout ||
                                    std::none_of(attachments.begin(), attachments.end(),
                                                 [&](const auto& a) { return a.first == b.resource; })))
                    merge = false;
        if (merge) {
            pass.continues = true;
            passes_[previous].leaves_open = true;
            if (!pass.planned.images.empty())
                --stats_.barrier_calls;
            pass.planned = Planned{};
            barriers_.erase(std::remove_if(barriers_.begin(), barriers_.end(),
                                           [&](const Barrier& b) { return b.pass == n; }),
                            barriers_.end());
            ++stats_.merged;
        }
        previous = n;
    }
    for (std::size_t n = passes_.size(); n-- > 0;) {
        if (!passes_[n].kept || !passes_[n].leaves_open)
            continue;
        for (std::size_t next = n + 1; next < passes_.size(); ++next)
            if (passes_[next].kept) {
                passes_[n].group_last = passes_[next].group_last;
                break;
            }
    }

    // The attachments each rendering instance stores (decided by its last pass), the depth ones apart.
    for (const Pass& pass : passes_) {
        if (!pass.kept || pass.continues)
            continue;
        const Pass& last = passes_[pass.group_last];
        for (std::size_t u = 0; u < last.uses.size(); ++u) {
            const Use& use = last.uses[u];
            if (!is_attachment(use.access) || use.access == Access::DepthRead || last.stored[u] == 0u)
                continue;
            ++stats_.stores;
            if (use.access == Access::DepthWrite)
                ++stats_.depth_stores;
        }
    }

    // The imported resources left in their final use.
    const auto closing = static_cast<std::uint32_t>(passes_.size());
    for (std::size_t r = 0; r < resources_.size(); ++r) {
        const Resource& resource = resources_[r];
        if (!resource.has_final)
            continue;
        if (resource.kind == Kind::ImportedBuffer) {
            plan_buffer(closing_, closing, static_cast<ResourceId>(r), resource.buffer_final);
        } else {
            ImageUse& last = state_of(resource.image);
            if (last.layout != resource.final_use.layout || (last.access & kWriteAccess) != 0)
                plan_image(closing_, closing, static_cast<ResourceId>(r), resource.final_use, false);
        }
    }
    if (!closing_.images.empty() || !closing_.buffers.empty())
        ++stats_.barrier_calls;
    stats_.barriers = static_cast<std::uint32_t>(barriers_.size());
    for (const auto& [image, state] : image_state)
        end_states_.emplace_back(image, state);
    compiled_ = true;
    return true;
}

bool FrameGraph::execute(VkCommandBuffer cmd, std::string* error, GpuTimer* timer) {
    if (!compiled_)
        return fail(error, "frame graph executed before a successful compile");
    const auto barrier = [&](const Planned& planned) {
        if (planned.images.empty() && planned.buffers.empty())
            return;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(planned.images.size());
        dependency.pImageMemoryBarriers = planned.images.data();
        dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(planned.buffers.size());
        dependency.pBufferMemoryBarriers = planned.buffers.data();
        vkCmdPipelineBarrier2(cmd, &dependency);
    };
    open_ = false;
    std::string group;
    for (std::size_t n = 0; n < passes_.size(); ++n) {
        Pass& pass = passes_[n];
        if (!pass.kept)
            continue;
        barrier(pass.planned);
        if (pass.execute) {
            PassContext context(*this, cmd, static_cast<std::uint32_t>(n));
            if (!pass.execute(context, error))
                return false;
        }
        if (open_ != pass.leaves_open)
            return fail(error, "pass '" + pass.name + "' left " +
                                   (open_ ? "a rendering instance open" : "no instance open for the next pass"));
        group += (group.empty() ? "" : " + ") + pass.name;
        if (!pass.leaves_open) {
            if (timer != nullptr)
                timer->mark(cmd, std::move(group));
            group.clear();
        }
    }
    barrier(closing_);
    for (const auto& [image, state] : end_states_)
        image->state = state;
    return true;
}

std::vector<std::string> FrameGraph::kept_passes() const {
    std::vector<std::string> names;
    for (const Pass& pass : passes_)
        if (pass.kept)
            names.push_back(pass.name);
    return names;
}

std::vector<std::string> FrameGraph::culled_passes() const {
    std::vector<std::string> names;
    for (const Pass& pass : passes_)
        if (!pass.kept)
            names.push_back(pass.name);
    return names;
}

std::vector<std::string> FrameGraph::groups() const {
    std::vector<std::string> names;
    for (const Pass& pass : passes_) {
        if (!pass.kept)
            continue;
        if (pass.continues && !names.empty())
            names.back() += " + " + pass.name;
        else
            names.push_back(pass.name);
    }
    return names;
}

const Image* FrameGraph::image_of(ResourceId id) const noexcept {
    return id < resources_.size() ? resources_[id].image : nullptr;
}

}  // namespace shadow::render_vk
