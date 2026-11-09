#pragma once

// Frames in flight (docs/VULKAN_RENDERER.md 3.5): the counterpart of render/frame_loop.h. Each slot owns a command
// pool and buffer and an upload ring (host-visible, persistently mapped: constant blocks, immediate vertices,
// particles); `begin` waits until the GPU has finished the slot's previous frame on the device timeline, then resets
// both. One submission per frame signals the next timeline value.

#include <cstdint>
#include <string>
#include <vector>

#include "render_vk/device.h"
#include "render_vk/resources.h"

namespace shadow::render_vk {

struct UploadAllocation {
    void* cpu = nullptr;
    VkDeviceSize offset = 0;  // in the slot's ring buffer
    explicit operator bool() const noexcept { return cpu != nullptr; }
};

class FrameLoop {
public:
    FrameLoop() = default;
    FrameLoop(const FrameLoop&) = delete;
    FrameLoop& operator=(const FrameLoop&) = delete;
    ~FrameLoop();

    // `ring_slack`: bytes past every allocation that stay inside the buffer (the widest constant block's range, so a
    // dynamic offset near the end still names a whole block).
    bool create(Device& device, std::uint32_t frames_in_flight, VkDeviceSize ring_bytes, VkDeviceSize ring_slack,
                std::string* error);
    void destroy();

    // Waits for the slot's previous frame and opens its command buffer.
    bool begin(std::string* error);
    std::uint32_t slot() const noexcept { return slot_; }
    std::uint64_t frame_number() const noexcept { return frame_number_; }
    VkCommandBuffer cmd() const noexcept { return slots_[slot_].cmd; }
    VkBuffer ring_buffer(std::uint32_t slot) const noexcept { return slots_[slot].ring.buffer; }
    UploadAllocation allocate(VkDeviceSize bytes, VkDeviceSize alignment);

    // Closes and submits the command buffer, waiting for `wait` (at colour output, VK_NULL_HANDLE: none) and signalling
    // `signal` (binary, VK_NULL_HANDLE: none) besides the timeline. The slot advances.
    bool submit(VkSemaphore wait, VkSemaphore signal, std::string* error);
    std::uint32_t frames_in_flight() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }

private:
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        Buffer ring;
        VkDeviceSize used = 0;
        std::uint64_t done_at = 0;  // the timeline value of its last submission
    };
    Device* device_ = nullptr;
    std::vector<Slot> slots_;
    VkDeviceSize ring_capacity_ = 0;  // allocatable bytes (the buffer is this + slack)
    std::uint32_t slot_ = 0;
    std::uint64_t frame_number_ = 0;
};

}  // namespace shadow::render_vk
