#pragma once

// Frames in flight (docs/lift-20261006/R_DESIGN.md 2.1, 2.6, 2.10).
//
// Each of the (two) frame slots owns a command allocator, a segment of the per-frame upload ring and a range of
// transient bindless descriptors. A slot is reused only after the GPU has finished the frame that last used it;
// that wait - bounded by the number of frames in flight - is the only place the CPU waits for the GPU in steady
// state. Beginning a frame also releases whatever the GPU has finished with (retired resources, freed bindless
// indices). The frame loop belongs to the rendering thread: the renderer calls it under its render lock only, never
// under the lock the game thread's resource calls take, so this wait never reaches the game (D11, ES 2026-10-19).

#include <cstdint>
#include <string>
#include <vector>

#include "render/descriptors.h"
#include "render/gpu_device.h"
#include "render/resources.h"

namespace shadow::render {

class FrameLoop {
public:
    FrameLoop() = default;
    FrameLoop(const FrameLoop&) = delete;
    FrameLoop& operator=(const FrameLoop&) = delete;
    ~FrameLoop();

    bool create(Device& device, BindlessHeap& heap, std::uint32_t frames_in_flight, std::uint64_t ring_segment_bytes,
                std::string* error);
    void destroy();

    struct Frame {
        ID3D12GraphicsCommandList* list = nullptr;
        std::uint32_t slot = 0;
        std::uint64_t number = 0;  // 1, 2, 3, ...
    };

    // Waits for the slot's previous frame, then readies its allocator, list, ring segment and transient range.
    Frame begin();
    // Closes and executes the frame's list on the direct queue; returns the fence value that ends the frame.
    std::uint64_t submit();

    UploadRing& ring() noexcept { return ring_; }
    std::uint32_t frames_in_flight() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }
    std::uint64_t frame_number() const noexcept { return number_; }

private:
    struct Slot {
        Com<ID3D12CommandAllocator> allocator;
        std::uint64_t fence = 0;
    };

    Device* device_ = nullptr;
    BindlessHeap* heap_ = nullptr;
    std::vector<Slot> slots_;
    Com<ID3D12GraphicsCommandList> list_;
    UploadRing ring_;
    std::uint64_t number_ = 0;
    std::uint32_t slot_ = 0;
    bool open_ = false;
};

}  // namespace shadow::render
