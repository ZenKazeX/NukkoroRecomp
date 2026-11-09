#pragma once

// Presentation (docs/lift-20261006/R_DESIGN.md 2.1): a flip-model swap chain at the window's own resolution,
// with a frame-latency waitable object (latency 1 by default) and tearing support for variable-refresh-rate
// displays. Back buffers are imported into each frame's graph; the frame graph leaves them in the PRESENT state.

#include <cstdint>
#include <string>

#include "render/descriptors.h"
#include "render/gpu_device.h"

namespace shadow::render {

struct SwapChainOptions {
    std::uint32_t buffers = 3;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::uint32_t max_latency = 1;
    bool allow_tearing = true;  // used only when the system supports it
};

enum class PresentMode {
    VSync,     // one refresh per present
    Immediate, // no wait for vertical blank; tears on displays without variable refresh rate
};

class SwapChain {
public:
    SwapChain() = default;
    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;
    ~SwapChain();

    bool create(Device& device, HWND window, std::uint32_t width, std::uint32_t height,
                const SwapChainOptions& options, std::string* error);
    void destroy();

    // Blocks until the swap chain can take another frame (bounded by `timeout_ms`).
    bool wait_for_frame(std::uint32_t timeout_ms);
    std::uint32_t current() const;
    ID3D12Resource* back_buffer(std::uint32_t index) const { return buffers_[index].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(std::uint32_t index) const { return rtvs_[index]; }
    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    DXGI_FORMAT format() const noexcept { return options_.format; }

    // False only on a lost device or a failed present; an occluded window is not an error.
    bool present(PresentMode mode, std::string* error);
    // Waits for the GPU, then resizes the back buffers.
    bool resize(std::uint32_t width, std::uint32_t height, std::string* error);

private:
    bool acquire_buffers(std::string* error);
    void release_buffers();

    static constexpr std::uint32_t kMaxBuffers = 4;
    Device* device_ = nullptr;
    Com<IDXGISwapChain3> swap_chain_;
    HANDLE waitable_ = nullptr;
    CpuDescriptorPool rtv_pool_;
    Com<ID3D12Resource> buffers_[kMaxBuffers];
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs_[kMaxBuffers]{};
    SwapChainOptions options_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool tearing_ = false;
};

}  // namespace shadow::render
