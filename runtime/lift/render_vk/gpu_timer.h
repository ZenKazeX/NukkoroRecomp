#pragma once

// GPU time per pass (docs/VULKAN_RENDERER.md 5.2, stage V6): timestamp queries the frame graph writes after each
// rendering group it records - the counterpart of render/gpu_timer.h. Each frame slot has its own query pool; a slot's
// results are read when the slot comes round again (its fence has been waited for) or right after a capture waited
// for the GPU, so reading never stalls.
//
// A span is the time from the end of the previous group (or the frame's start) to the end of this one: it includes the
// barriers recorded before the group. On a tiler the GPU defers and bins work, so spans measure where the GPU's
// timeline moved, not what each pass cost in isolation; the frame total is exact either way.

#include <cstdint>
#include <string>
#include <vector>

#include "render_vk/device.h"

namespace shadow::render_vk {

class GpuTimer {
public:
    GpuTimer() = default;
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;
    ~GpuTimer();

    // `capacity`: timestamps one frame can write (the start and one per group). Without timestamps on the device's
    // queue the timer stays off and every call does nothing.
    bool create(Device& device, std::uint32_t slots, std::uint32_t capacity, std::string* error);
    void destroy();
    bool enabled() const noexcept { return !slots_.empty(); }

    // Starts measuring a frame in `slot` (whose previous frame the GPU has finished): reads that frame's results,
    // resets the slot's queries and writes the start timestamp.
    void begin(VkCommandBuffer cmd, std::uint32_t slot);
    // A timestamp after everything recorded so far, ending the span `name`. Outside a rendering instance.
    void mark(VkCommandBuffer cmd, std::string name);
    // Reads `slot`'s results when the GPU has written them all (no wait); true when it read a frame.
    bool collect(std::uint32_t slot);

    struct Span {
        std::string name;
        double ms = 0.0;
    };
    // The latest frame read: its spans in recording order and their sum.
    const std::vector<Span>& spans() const noexcept { return spans_; }
    double frame_ms() const noexcept { return frame_ms_; }
    std::uint64_t frames_read() const noexcept { return frames_read_; }
    std::uint64_t dropped() const noexcept { return dropped_; }  // marks past the capacity, all frames

private:
    struct Slot {
        VkQueryPool pool = VK_NULL_HANDLE;
        std::vector<std::string> names;  // of the marks written (the start has none)
        bool pending = false;            // written, not read yet
    };
    Device* device_ = nullptr;
    std::vector<Slot> slots_;
    std::uint32_t capacity_ = 0;
    std::uint32_t recording_ = 0;  // the slot begin() opened
    std::uint64_t mask_ = 0;       // the queue's valid timestamp bits
    double ns_per_tick_ = 0.0;
    std::vector<Span> spans_;
    double frame_ms_ = 0.0;
    std::uint64_t frames_read_ = 0;
    std::uint64_t dropped_ = 0;
};

}  // namespace shadow::render_vk
