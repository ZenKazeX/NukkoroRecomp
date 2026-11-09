#pragma once

// The renderer's sampler table (docs/lift-20261006/R_DESIGN.md 2.6): every combination a material asks for - the
// filter (point, linear, anisotropic) times the address mode of each axis (wrap, clamp, mirror) - at a fixed index.
// Plain values with no graphics API, so the packet contract (frame.h) names samplers the same way for every backend:
// D3D12 builds the table as its shader-visible sampler heap (descriptors.h SamplerHeap), Vulkan as immutable samplers
// (render_vk/bindless.h); a shader reaches entry i of either as SamplerDescriptorHeap[i].

#include <cstdint>

namespace shadow::render {

enum class Filter : std::uint32_t { Point = 0, Linear = 1, Anisotropic = 2 };
enum class Address : std::uint32_t { Wrap = 0, Clamp = 1, Mirror = 2 };

// The sampler table's index of a combination: what a shader passes to SamplerDescriptorHeap[].
constexpr std::uint32_t sampler_index(Filter filter, Address u, Address v) noexcept {
    return static_cast<std::uint32_t>(filter) * 9u + static_cast<std::uint32_t>(u) * 3u + static_cast<std::uint32_t>(v);
}
inline constexpr std::uint32_t kSamplerCount = 27;

}  // namespace shadow::render
