#pragma once

// Bindless descriptors (docs/lift-20261006/R_DESIGN.md 2.6).
//
// One shader-visible CBV/SRV/UAV heap and one shader-visible sampler heap are bound for the whole frame. A
// shader reaches a texture or buffer as ResourceDescriptorHeap[index] and a sampler as SamplerDescriptorHeap[index]
// (Shader Model 6.6), so a material or draw record carries plain integers instead of descriptor tables.
//
//   * Persistent indices belong to resources that live for a scope (asset textures, pooled render targets).
//     A freed index is reused only after the GPU has passed the frame that freed it.
//   * Transient indices are handed out linearly per frame in flight and recycled when that frame slot comes
//     round again.
//   * Render-target and depth views live in CPU-only heaps; shaders never see them.
//   * Samplers form a fixed table built at start-up: the filter (point, linear, anisotropic) times the address
//     mode of each axis (wrap, clamp, mirror) - every combination a material asks for.

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "render/gpu_device.h"
#include "render/sampler_table.h"

namespace shadow::render {

inline constexpr std::uint32_t kNoDescriptor = 0xFFFFFFFFu;

class BindlessHeap {
public:
    bool create(Device& device, std::uint32_t persistent_capacity, std::uint32_t transient_per_frame,
                std::uint32_t frames_in_flight, std::string* error);

    // ---- persistent --------------------------------------------------------------------------------------
    // Any thread: asset views are made where the asset is created (the resource worker), render targets and
    // histories on the render thread. These four calls share one lock, never held across a wait.
    // kNoDescriptor when the heap is full (a reported failure: capacities are sized for the whole game).
    std::uint32_t allocate();
    // The index becomes reusable once the GPU has passed `mark`.
    void free(std::uint32_t index, const GpuMark& mark);
    // Makes indices whose frames the GPU has finished reusable again.
    void reclaim(const Device& device);

    // ---- transient (per frame in flight): the render thread only -------------------------------------------
    void begin_frame(std::uint32_t frame_slot);
    std::uint32_t allocate_transient();

    // ---- views ------------------------------------------------------------------------------------------
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(std::uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(std::uint32_t index) const;
    ID3D12DescriptorHeap* heap() const noexcept { return heap_.Get(); }

    std::uint32_t persistent_in_use() const {
        std::lock_guard<std::mutex> lock(persistent_mutex_);
        return persistent_in_use_;
    }
    std::uint32_t persistent_capacity() const noexcept { return persistent_capacity_; }
    // The current frame's transient descriptors so far (the render thread only; GPU trace).
    std::uint32_t transient_used() const noexcept { return transient_used_; }
    std::uint64_t exhausted_count() const noexcept { return exhausted_.load(std::memory_order_relaxed); }

private:
    struct Freed {
        GpuMark mark;
        std::uint32_t index;
    };
    Com<ID3D12DescriptorHeap> heap_;
    std::uint32_t increment_ = 0;
    mutable std::mutex persistent_mutex_;  // persistent_next_, persistent_in_use_, free_list_, pending_
    std::uint32_t persistent_capacity_ = 0;
    std::uint32_t persistent_next_ = 0;   // never-used indices start here
    std::uint32_t persistent_in_use_ = 0;
    std::vector<std::uint32_t> free_list_;
    std::deque<Freed> pending_;
    std::uint32_t transient_per_frame_ = 0;
    std::uint32_t transient_base_ = 0;    // first index of the current frame's transient range
    std::uint32_t transient_used_ = 0;
    std::atomic<std::uint64_t> exhausted_{0};
};

// CPU-only render-target or depth-stencil views, with immediate reuse (the GPU reads these only while the
// command list records them, never later).
class CpuDescriptorPool {
public:
    bool create(Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t capacity, std::string* error);
    // A null handle (ptr == 0) when full.
    D3D12_CPU_DESCRIPTOR_HANDLE allocate();
    void free(D3D12_CPU_DESCRIPTOR_HANDLE handle);
    std::uint32_t in_use() const noexcept { return in_use_; }

private:
    Com<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE base_{};
    std::uint32_t increment_ = 0;
    std::uint32_t capacity_ = 0;
    std::uint32_t next_ = 0;
    std::uint32_t in_use_ = 0;
    std::vector<std::uint32_t> free_list_;
};

// Filter, Address, sampler_index and kSamplerCount: render/sampler_table.h (API-free, shared with the packet contract).

class SamplerHeap {
public:
    bool create(Device& device, std::uint32_t max_anisotropy, std::string* error);
    ID3D12DescriptorHeap* heap() const noexcept { return heap_.Get(); }

private:
    Com<ID3D12DescriptorHeap> heap_;
};

}  // namespace shadow::render
