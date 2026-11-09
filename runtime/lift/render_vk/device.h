#pragma once

// Shadow's Vulkan device (docs/VULKAN_RENDERER.md 3): the Vulkan counterpart of render/gpu_device.h.
//
//   * instance and adapter choice (the discrete GPU unless asked otherwise) and the capability floor: Vulkan 1.3 with
//     dynamic rendering, synchronization2, timeline semaphores and the descriptor-indexing features the bindless set
//     needs (bindless.h);
//   * ONE queue family that does graphics and compute (every desktop and mobile GPU has one); its queue takes every
//     frame's work and, for now, the uploads;
//   * VMA for every buffer and image placement;
//   * a timeline semaphore as the only "is the GPU done with it" signal: every submission signals the next value,
//     a GpuMark is a value, and deferred release keeps an object until the queue has passed the mark it was retired at.
//
// Threads: one submitting thread at a time (whoever renders); `retire` and `completed` from any thread.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "render_vk/vk_common.h"

struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;

namespace shadow::render_vk {

struct DeviceOptions {
    bool validation = false;     // the Khronos validation layer and debug-utils messages (when installed)
    bool prefer_integrated = false;
    std::string adapter;         // a substring of the device name to prefer (empty: the first discrete GPU)
    bool local_read = true;      // false: VK_KHR_dynamic_rendering_local_read left off even where offered (A/B)
};

// What the chosen device can do beyond the floor (the renderer adapts; docs/VULKAN_RENDERER.md 6).
struct Capabilities {
    std::string name;
    std::uint32_t api_version = 0;
    bool scalar_block_layout = false;      // skinning.hlsl's 44-byte stream (CSSkin) needs it
    bool signed_zero_inf_nan_f32 = false;  // -Gis shaders declare SignedZeroInfNanPreserve
    bool sampler_anisotropy = false;
    float max_anisotropy = 1.0f;
    bool timestamps = false;                // timestampComputeAndGraphics and the queue writes timestamps
    std::uint32_t timestamp_bits = 0;       // the queue's timestampValidBits
    float timestamp_period = 0.0f;          // nanoseconds per timestamp tick
    bool local_read = false;  // VK_KHR_dynamic_rendering_local_read: attachment locations remapped per draw
    std::uint32_t max_sampled_images = 0;  // update-after-bind limit of one set
    std::uint32_t max_storage_buffers = 0;
    VkDeviceSize min_uniform_offset_alignment = 256;
    VkDeviceSize max_uniform_range = 16384;
};

class Device {
public:
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

    bool create(const DeviceOptions& options, std::string* error);
    void destroy();

    VkInstance instance() const noexcept { return instance_; }
    VkPhysicalDevice physical() const noexcept { return physical_; }
    VkDevice vk() const noexcept { return device_; }
    VkQueue queue() const noexcept { return queue_; }
    std::uint32_t queue_family() const noexcept { return queue_family_; }
    VmaAllocator allocator() const noexcept { return allocator_; }
    const Capabilities& caps() const noexcept { return caps_; }
    // Which colour attachment each fragment output location writes from the next draw on (caps().local_read only):
    // locations[i] for attachment i, kUnusedLocation for none.
    void set_attachment_locations(VkCommandBuffer cmd, std::uint32_t count, const std::uint32_t* locations) const {
        AttachmentLocationInfo info;
        info.colorAttachmentCount = count;
        info.pColorAttachmentLocations = locations;
        set_locations_(cmd, &info);
    }

    // ---- the queue and the timeline ------------------------------------------------------------------------------
    // Submits one command buffer, waiting for `wait` at `wait_stage` (VK_NULL_HANDLE: none) and signalling `signal`
    // (binary, VK_NULL_HANDLE: none) and the timeline's next value (`*value`). Any thread: the queue is locked, so the
    // timeline values rise in submission order (frames from the render thread, uploads from the game thread).
    bool submit(VkCommandBuffer cmd, VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal,
                std::uint64_t* value, std::string* error);
    // vkQueuePresentKHR under the same lock.
    VkResult present(const VkPresentInfoKHR& info);
    VkSemaphore timeline() const noexcept { return timeline_; }
    // The last value handed to a submission: everything submitted so far is done when the GPU reaches it.
    std::uint64_t submitted() const noexcept { return submitted_.load(std::memory_order_acquire); }
    std::uint64_t completed() const;
    bool wait(std::uint64_t value, std::uint64_t timeout_ns, std::string* error) const;
    void wait_idle();

    // ---- deferred release ---------------------------------------------------------------------------------------
    // `destroy` runs once the GPU has passed everything submitted before this call.
    void retire(std::function<void()> destroy);
    // Runs the releases the GPU has passed (the renderer calls it once per frame).
    void collect();

    // Validation messages since the last call (errors and warnings; empty without validation).
    std::vector<std::string> take_validation_messages();
    void note_validation_message(std::string message);

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VmaAllocator allocator_ = nullptr;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::mutex queue_mutex_;  // vkQueueSubmit2 / vkQueuePresentKHR and the timeline value they take
    std::atomic<std::uint64_t> submitted_{0};
    Capabilities caps_;
    PfnSetAttachmentLocations set_locations_ = nullptr;

    struct Retired {
        std::uint64_t after = 0;
        std::function<void()> destroy;
    };
    std::mutex retire_mutex_;
    std::deque<Retired> retired_;

    std::mutex messages_mutex_;
    std::vector<std::string> messages_;
};

}  // namespace shadow::render_vk
