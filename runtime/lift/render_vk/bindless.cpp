#include "render_vk/bindless.h"

#include <algorithm>

namespace shadow::render_vk {

Bindless::~Bindless() {
    destroy();
}

bool Bindless::create(Device& device, std::uint32_t persistent_capacity, std::uint32_t transient_per_frame,
                      std::uint32_t frames_in_flight, std::uint32_t max_anisotropy, std::string* error) {
    device_ = &device;
    VkDevice vk = device.vk();
    const Capabilities& caps = device.caps();
    capacity_ = persistent_capacity + transient_per_frame * frames_in_flight;
    const std::uint32_t limit = std::min(caps.max_sampled_images, caps.max_storage_buffers);
    if (capacity_ > limit)
        return fail(error, "bindless capacity " + std::to_string(capacity_) + " exceeds the device's " +
                               std::to_string(limit) + " update-after-bind descriptors");
    persistent_capacity_ = persistent_capacity;
    transient_per_frame_ = transient_per_frame;

    // The sampler table (render/descriptors.cpp SamplerHeap::create): point / linear / anisotropic, each axis wrap /
    // clamp / mirror, w clamped, every mip level.
    const VkSamplerAddressMode modes[3] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                           VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    for (std::uint32_t f = 0; f < 3; ++f)
        for (std::uint32_t u = 0; u < 3; ++u)
            for (std::uint32_t v = 0; v < 3; ++v) {
                VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                s.magFilter = f == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                s.minFilter = s.magFilter;
                s.mipmapMode = f == 0 ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
                s.addressModeU = modes[u];
                s.addressModeV = modes[v];
                s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                s.anisotropyEnable = f == 2 && caps.sampler_anisotropy && max_anisotropy > 1 ? VK_TRUE : VK_FALSE;
                s.maxAnisotropy = std::min(static_cast<float>(std::max(1u, max_anisotropy)), caps.max_anisotropy);
                s.minLod = 0.0f;
                s.maxLod = VK_LOD_CLAMP_NONE;
                if (!check(vkCreateSampler(vk, &s, nullptr, &samplers_[f * 9 + u * 3 + v]), "vkCreateSampler", error))
                    return false;
            }

    // Set 1: the heap.
    VkDescriptorSetLayoutBinding heap[3]{};
    heap[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_, VK_SHADER_STAGE_ALL, nullptr};
    heap[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, kSamplerCount, VK_SHADER_STAGE_ALL, samplers_};
    heap[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, capacity_, VK_SHADER_STAGE_ALL, nullptr};
    const VkDescriptorBindingFlags indexed = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                             VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                             VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const VkDescriptorBindingFlags heap_flags[3] = {indexed, 0, indexed};
    VkDescriptorSetLayoutBindingFlagsCreateInfo heap_binding_flags{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    heap_binding_flags.bindingCount = 3;
    heap_binding_flags.pBindingFlags = heap_flags;
    VkDescriptorSetLayoutCreateInfo heap_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    heap_info.pNext = &heap_binding_flags;
    heap_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    heap_info.bindingCount = 3;
    heap_info.pBindings = heap;
    if (!check(vkCreateDescriptorSetLayout(vk, &heap_info, nullptr, &heap_layout_), "vkCreateDescriptorSetLayout (heap)",
               error))
        return false;

    // Set 0: the constant blocks.
    VkDescriptorSetLayoutBinding constants[3]{};
    for (std::uint32_t b = 0; b < 3; ++b)
        constants[b] = {b, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL, nullptr};
    VkDescriptorSetLayoutCreateInfo constants_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    constants_info.bindingCount = 3;
    constants_info.pBindings = constants;
    if (!check(vkCreateDescriptorSetLayout(vk, &constants_info, nullptr, &constants_layout_),
               "vkCreateDescriptorSetLayout (constants)", error))
        return false;

    const VkDescriptorSetLayout layouts[2] = {constants_layout_, heap_layout_};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = layouts;
    if (!check(vkCreatePipelineLayout(vk, &layout_info, nullptr, &pipeline_layout_), "vkCreatePipelineLayout", error))
        return false;

    const VkDescriptorPoolSize sizes[4] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kSamplerCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, capacity_},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * frames_in_flight},
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1 + frames_in_flight;
    pool_info.poolSizeCount = 4;
    pool_info.pPoolSizes = sizes;
    if (!check(vkCreateDescriptorPool(vk, &pool_info, nullptr, &pool_), "vkCreateDescriptorPool", error))
        return false;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &heap_layout_;
    if (!check(vkAllocateDescriptorSets(vk, &alloc, &heap_set_), "vkAllocateDescriptorSets (heap)", error))
        return false;
    constant_sets_.assign(frames_in_flight, VK_NULL_HANDLE);
    return true;
}

bool Bindless::make_constants_set(std::uint32_t slot, VkBuffer ring, std::string* error) {
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &constants_layout_;
    if (!check(vkAllocateDescriptorSets(device_->vk(), &alloc, &constant_sets_[slot]),
               "vkAllocateDescriptorSets (constants)", error))
        return false;
    const VkDescriptorBufferInfo blocks[3] = {
        {ring, 0, kRootBlockBytes},
        {ring, 0, kLightsBlockBytes},
        {ring, 0, kDrawBlockBytes},
    };
    VkWriteDescriptorSet writes[3]{};
    for (std::uint32_t b = 0; b < 3; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = constant_sets_[slot];
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[b].pBufferInfo = &blocks[b];
    }
    vkUpdateDescriptorSets(device_->vk(), 3, writes, 0, nullptr);
    return true;
}

std::uint32_t Bindless::allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!free_list_.empty()) {
        const std::uint32_t index = free_list_.back();
        free_list_.pop_back();
        return index;
    }
    if (persistent_next_ < persistent_capacity_)
        return persistent_next_++;
    ++exhausted_;
    return kNoDescriptor;
}

void Bindless::free(std::uint32_t index, std::uint64_t after_mark) {
    if (index == kNoDescriptor)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(Freed{after_mark, index});
}

void Bindless::reclaim(std::uint64_t completed) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pending_.empty() && pending_.front().after <= completed) {
        free_list_.push_back(pending_.front().index);
        pending_.pop_front();
    }
}

void Bindless::begin_frame(std::uint32_t slot) {
    transient_base_ = persistent_capacity_ + slot * transient_per_frame_;
    transient_used_ = 0;
}

std::uint32_t Bindless::allocate_transient() {
    if (transient_used_ >= transient_per_frame_) {
        ++exhausted_;
        return kNoDescriptor;
    }
    return transient_base_ + transient_used_++;
}

void Bindless::write_image(std::uint32_t index, VkImageView view, VkImageLayout layout) {
    if (index == kNoDescriptor)
        return;
    VkDescriptorImageInfo image{VK_NULL_HANDLE, view, layout};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = heap_set_;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_->vk(), 1, &write, 0, nullptr);
}

void Bindless::write_buffer(std::uint32_t index, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range) {
    if (index == kNoDescriptor)
        return;
    VkDescriptorBufferInfo info{buffer, offset, range};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = heap_set_;
    write.dstBinding = 2;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_->vk(), 1, &write, 0, nullptr);
}

void Bindless::destroy() {
    if (device_ == nullptr)
        return;
    VkDevice vk = device_->vk();
    if (pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vk, pool_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vk, pipeline_layout_, nullptr);
    if (heap_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vk, heap_layout_, nullptr);
    if (constants_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vk, constants_layout_, nullptr);
    for (VkSampler& s : samplers_) {
        if (s != VK_NULL_HANDLE)
            vkDestroySampler(vk, s, nullptr);
        s = VK_NULL_HANDLE;
    }
    pool_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    heap_layout_ = constants_layout_ = VK_NULL_HANDLE;
    heap_set_ = VK_NULL_HANDLE;
    constant_sets_.clear();
    free_list_.clear();
    pending_.clear();
    persistent_next_ = 0;
    device_ = nullptr;
}

}  // namespace shadow::render_vk
