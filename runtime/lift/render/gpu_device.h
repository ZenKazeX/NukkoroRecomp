#pragma once

// Shadow's graphics device: Direct3D 12, Shader Model 6.6, bindless (docs/lift-20261006/R_DESIGN.md 2.6-2.7).
//
// The renderer owns the device directly - no portability layer between it and D3D12. This file holds the parts
// every other renderer component builds on:
//
//   * adapter choice (the high-performance GPU, or WARP for deterministic tests) and the capability floor: Shader
//     Model 6.6 with resource binding tier 3, so shaders index the descriptor heaps directly;
//   * the queues: one direct queue for each frame's work and one copy queue for load batches, each with its own
//     fence; a queue's fence value is the only "is the GPU done with it" signal the renderer uses;
//   * D3D12 Memory Allocator for every heap placement and the video-memory budget;
//   * deferred release: an object handed to `retire` is kept alive until both queues have passed everything that
//     was submitted when it was retired.
//
// Threads (ES 2026-10-19): each queue has ONE submitting thread at a time (the direct queue: whoever renders; the copy
// queue: whoever owns the upload stream), but any thread may take a mark, retire an object or ask what the GPU has
// reached - the submitted fence values are atomic and the retire list has its own lock (never held across a wait).

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace D3D12MA {
class Allocator;
class Allocation;
}  // namespace D3D12MA

namespace shadow::render {

template <class T>
using Com = Microsoft::WRL::ComPtr<T>;

struct DeviceOptions {
    enum class Adapter {
        HighPerformance,  // the discrete GPU when there is one
        LowPower,         // the integrated GPU when there is one (comparisons across GPUs)
        Warp,             // the software rasterizer: deterministic, for tests and reference captures
    };
    Adapter adapter = Adapter::HighPerformance;
    bool debug_layer = false;     // D3D12 debug layer (development builds)
    bool gpu_validation = false;  // GPU-based validation, on top of the debug layer
};

struct DeviceCaps {
    std::string adapter_name;
    bool software = false;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    D3D_SHADER_MODEL shader_model = D3D_SHADER_MODEL_5_1;
    D3D12_RESOURCE_BINDING_TIER binding_tier = D3D12_RESOURCE_BINDING_TIER_1;
    bool enhanced_barriers = false;
    bool gpu_upload_heap = false;
    bool tearing = false;  // DXGI_FEATURE_PRESENT_ALLOW_TEARING: variable refresh rate presentation
    std::uint64_t dedicated_video_memory = 0;
};

// A command queue with its fence. Fence values only grow; value 0 is "nothing submitted".
class Queue {
public:
    Queue() = default;
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    ~Queue();

    bool create(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, const wchar_t* name, std::string* error);
    void reset();

    // Executes the lists and signals the next fence value, which is returned.
    std::uint64_t submit(ID3D12CommandList* const* lists, std::uint32_t count);
    std::uint64_t signal();
    // This queue's later work waits on the GPU until `other` reaches `value`.
    void wait_for(const Queue& other, std::uint64_t value);

    std::uint64_t last_submitted() const noexcept { return last_value_.load(std::memory_order_acquire); }
    std::uint64_t completed() const;
    bool reached(std::uint64_t value) const { return completed() >= value; }
    // Blocks the calling thread until the GPU reaches `value`.
    void wait(std::uint64_t value);
    void wait_idle() { wait(signal()); }

    ID3D12CommandQueue* get() const noexcept { return queue_.Get(); }
    ID3D12Fence* fence() const noexcept { return fence_.Get(); }
    D3D12_COMMAND_LIST_TYPE type() const noexcept { return type_; }

private:
    Com<ID3D12CommandQueue> queue_;
    Com<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    std::atomic<std::uint64_t> last_value_{0};  // written by the submitting thread, read by any (marks)
    D3D12_COMMAND_LIST_TYPE type_ = D3D12_COMMAND_LIST_TYPE_DIRECT;
};

// The point after which a retired object may be released: everything both queues had been given.
struct GpuMark {
    std::uint64_t direct = 0;
    std::uint64_t copy = 0;
};

class Device {
public:
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

    bool create(const DeviceOptions& options, std::string* error);
    void destroy();

    ID3D12Device8* d3d() const noexcept { return device_.Get(); }
    IDXGIFactory6* factory() const noexcept { return factory_.Get(); }
    IDXGIAdapter1* adapter() const noexcept { return adapter_.Get(); }
    D3D12MA::Allocator* allocator() const noexcept { return allocator_; }
    const DeviceCaps& caps() const noexcept { return caps_; }

    Queue& direct() noexcept { return direct_; }
    Queue& copy() noexcept { return copy_; }

    GpuMark mark() const noexcept { return {direct_.last_submitted(), copy_.last_submitted()}; }
    bool reached(const GpuMark& mark) const { return direct_.reached(mark.direct) && copy_.reached(mark.copy); }

    // Keeps `object` alive until the GPU has passed everything submitted so far.
    void retire(Com<IUnknown> object);
    void retire(Com<IUnknown> object, const GpuMark& mark);
    // Releases every retired object the GPU is done with. Called once per frame.
    void collect();
    std::size_t retired_count() const {
        std::lock_guard<std::mutex> lock(retired_mutex_);
        return retired_.size();
    }

    // Waits until both queues are idle and releases everything retired.
    void wait_idle();

    // Debug-layer messages of severity error or corruption since the last call (none without the debug layer).
    std::vector<std::string> take_debug_errors();

    // The video-memory budget the OS gives this process, and what it uses (local segment).
    struct Budget {
        std::uint64_t budget = 0;
        std::uint64_t usage = 0;
    };
    Budget local_budget() const;

private:
    struct Retired {
        GpuMark mark;
        Com<IUnknown> object;
    };

    Com<IDXGIFactory6> factory_;
    Com<IDXGIAdapter1> adapter_;
    Com<ID3D12Device8> device_;
    D3D12MA::Allocator* allocator_ = nullptr;
    Queue direct_;
    Queue copy_;
    DeviceCaps caps_;
    mutable std::mutex retired_mutex_;  // guards retired_ only; nothing waits on the GPU while holding it
    std::deque<Retired> retired_;
};

// HRESULT as text for error messages.
std::string hresult_text(HRESULT hr);

}  // namespace shadow::render
