#pragma once

// The deformation pass's data (shaders/skinning.hlsl CSSkin): the layouts of its structured buffers and palette, as
// the shader declares them (the D3D12 renderer's render/skin_cache.h mirrors the same structs; that header includes
// D3D12, so these are the Vulkan side's own declarations of the shader's layout, checked by size).

#include <cstdint>

namespace shadow::render_vk {

// One base vertex with its bone weights (SkinVertex in skinning.hlsl; 44 bytes, scalar layout).
struct SkinVertex {
    float position[3];
    float normal[3];
    float weights[4];
    std::uint32_t indices;  // four 8-bit bone indices, the first in the lowest byte
};
static_assert(sizeof(SkinVertex) == 44, "SkinVertex layout");

// One vertex's offset in one delta-morph target (MorphDelta in skinning.hlsl); target t of vertex v at t * count + v.
struct MorphDelta {
    float position[3];
    float normal[3];
};
static_assert(sizeof(MorphDelta) == 24, "MorphDelta layout");

// A palette entry: p' = M * [p, 1], rows of a 3x4 matrix (the HLSL's row_major float3x4).
struct BoneMatrix {
    float rows[3][4];
};
inline constexpr std::uint32_t kMaxPaletteBones = 256;
static_assert(sizeof(BoneMatrix) * kMaxPaletteBones == 12288, "cbuffer Palette");

}  // namespace shadow::render_vk
