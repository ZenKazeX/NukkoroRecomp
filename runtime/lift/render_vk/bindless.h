#pragma once

// Bindless descriptors in Vulkan (docs/VULKAN_RENDERER.md 3.4): the counterpart of render/descriptors.h, laid out so
// the D3D12 renderer's HLSL compiles to SPIR-V unchanged (DXC -fvk-bind-resource-heap / -fvk-bind-sampler-heap).
//
// Set 1 - the "heap", bound once per command buffer:
//   binding 0  SAMPLED_IMAGE[capacity]   ResourceDescriptorHeap of the graphics shaders (Texture2D, Texture2DMS)
//   binding 1  SAMPLER[27]               SamplerDescriptorHeap: the fixed table, as immutable samplers
//   binding 2  STORAGE_BUFFER[capacity]  ResourceDescriptorHeap of the compute shaders (skinning's structured buffers)
// ONE index space: an index names a slot in binding 0 and binding 2 alike, so a draw record carries the same plain
// integer a D3D12 draw carries; an image is written into binding 0, a buffer into binding 2. Binding 0 and 2 are
// separate arrays because a sampled image and a storage buffer are different descriptor types, and aliasing them in
// one binding needs VK_EXT_mutable_descriptor_type, which mobile drivers do not all have.
//
// Set 0 - the constant blocks, one set per frame slot over that slot's upload ring, with dynamic offsets per draw:
//   binding 0  UNIFORM_BUFFER_DYNAMIC  cbuffer Root (b0): D3D12's 16 root constants, as a 64-byte block
//   binding 1  UNIFORM_BUFFER_DYNAMIC  cbuffer Lights / Palette (b1)
//   binding 2  UNIFORM_BUFFER_DYNAMIC  cbuffer Draw / Particles / Immediate (b2)
//
// Persistent indices are reused only after the GPU has passed the frame that freed them; transient indices are handed
// out linearly per frame in flight, exactly as BindlessHeap does.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "render_vk/device.h"
#include "render_vk/resources.h"

namespace shadow::render_vk {

// The sampler table: filter (point, linear, anisotropic) x the address mode of u and v (wrap, clamp, mirror), indexed
// exactly as render/descriptors.h sampler_index() indexes it.
inline constexpr std::uint32_t kSamplerCount = 27;
inline constexpr std::uint32_t kRootBlockBytes = 64;      // 16 x 32-bit root constants
inline constexpr std::uint32_t kLightsBlockBytes = 12288; // max(cbuffer Lights, cbuffer Palette: 256 x 48 bytes)
inline constexpr std::uint32_t kDrawBlockBytes = 256;

class Bindless {
public:
    Bindless() = default;
    Bindless(const Bindless&) = delete;
    Bindless& operator=(const Bindless&) = delete;
    ~Bindless();

    // `max_anisotropy`: the anisotropic entries' limit (render::RendererOptions::max_anisotropy; 1: trilinear).
    bool create(Device& device, std::uint32_t persistent_capacity, std::uint32_t transient_per_frame,
                std::uint32_t frames_in_flight, std::uint32_t max_anisotropy, std::string* error);
    void destroy();

    VkDescriptorSetLayout constants_layout() const noexcept { return constants_layout_; }
    VkDescriptorSetLayout heap_layout() const noexcept { return heap_layout_; }
    VkDescriptorSet heap_set() const noexcept { return heap_set_; }
    VkPipelineLayout pipeline_layout() const noexcept { return pipeline_layout_; }

    // The constant-block set of a frame slot over `ring` (made once per slot; the ring buffer never changes).
    bool make_constants_set(std::uint32_t slot, VkBuffer ring, std::string* error);
    VkDescriptorSet constants_set(std::uint32_t slot) const noexcept { return constant_sets_[slot]; }

    // ---- persistent indices (any thread) ----
    std::uint32_t allocate();
    void free(std::uint32_t index, std::uint64_t after_mark);
    void reclaim(std::uint64_t completed);

    // ---- transient indices (the render thread) ----
    void begin_frame(std::uint32_t slot);
    std::uint32_t allocate_transient();

    // ---- writes ----
    void write_image(std::uint32_t index, VkImageView view, VkImageLayout layout);
    void write_buffer(std::uint32_t index, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);

    std::uint64_t exhausted_count() const noexcept { return exhausted_; }

private:
    Device* device_ = nullptr;
    VkSampler samplers_[kSamplerCount] = {};
    VkDescriptorSetLayout constants_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout heap_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet heap_set_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> constant_sets_;

    std::mutex mutex_;
    std::uint32_t capacity_ = 0;            // persistent + transient
    std::uint32_t persistent_capacity_ = 0;
    std::uint32_t persistent_next_ = 0;
    std::vector<std::uint32_t> free_list_;
    struct Freed {
        std::uint64_t after;
        std::uint32_t index;
    };
    std::deque<Freed> pending_;
    std::uint32_t transient_per_frame_ = 0;
    std::uint32_t transient_base_ = 0;
    std::uint32_t transient_used_ = 0;
    std::uint64_t exhausted_ = 0;
};

}  // namespace shadow::render_vk
