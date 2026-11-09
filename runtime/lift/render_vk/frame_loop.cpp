#include "render_vk/frame_loop.h"

namespace shadow::render_vk {

FrameLoop::~FrameLoop() {
    destroy();
}

bool FrameLoop::create(Device& device, std::uint32_t frames_in_flight, VkDeviceSize ring_bytes,
                       VkDeviceSize ring_slack, std::string* error) {
    device_ = &device;
    ring_capacity_ = ring_bytes;
    slots_.resize(frames_in_flight);
    for (Slot& s : slots_) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = device.queue_family();
        if (!check(vkCreateCommandPool(device.vk(), &pool, nullptr, &s.pool), "vkCreateCommandPool", error))
            return false;
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = s.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(device.vk(), &alloc, &s.cmd), "vkAllocateCommandBuffers", error))
            return false;
        if (!create_buffer(device, ring_bytes + ring_slack,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           HostAccess::Write, &s.ring, error))
            return false;
        if (s.ring.mapped == nullptr)
            return fail(error, "the upload ring is not host-visible");
    }
    slot_ = 0;
    return true;
}

bool FrameLoop::begin(std::string* error) {
    Slot& s = slots_[slot_];
    if (s.done_at != 0 && !device_->wait(s.done_at, UINT64_MAX, error))
        return false;
    if (!check(vkResetCommandPool(device_->vk(), s.pool, 0), "vkResetCommandPool", error))
        return false;
    s.used = 0;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ++frame_number_;
    return check(vkBeginCommandBuffer(s.cmd, &begin), "vkBeginCommandBuffer", error);
}

UploadAllocation FrameLoop::allocate(VkDeviceSize bytes, VkDeviceSize alignment) {
    Slot& s = slots_[slot_];
    const VkDeviceSize offset = (s.used + alignment - 1) / alignment * alignment;
    if (offset + bytes > ring_capacity_)
        return {};
    s.used = offset + bytes;
    return {static_cast<std::uint8_t*>(s.ring.mapped) + offset, offset};
}

bool FrameLoop::submit(VkSemaphore wait, VkSemaphore signal, std::string* error) {
    Slot& s = slots_[slot_];
    if (!check(vkEndCommandBuffer(s.cmd), "vkEndCommandBuffer", error))
        return false;
    std::uint64_t value = 0;
    if (!device_->submit(s.cmd, wait, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, signal, &value, error))
        return false;
    s.done_at = value;
    slot_ = (slot_ + 1) % static_cast<std::uint32_t>(slots_.size());
    return true;
}

void FrameLoop::destroy() {
    if (device_ == nullptr)
        return;
    device_->wait_idle();
    for (Slot& s : slots_) {
        destroy_buffer(*device_, s.ring);
        if (s.pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device_->vk(), s.pool, nullptr);
    }
    slots_.clear();
    device_ = nullptr;
}

}  // namespace shadow::render_vk
