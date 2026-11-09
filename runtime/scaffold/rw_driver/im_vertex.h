// SCAFFOLDING (R2): reading the immediate modes' vertex records and indices out of the address window (GameCube byte order), shared by Im2D, Im3D and the screen passes; what a primitive means is runtime/lift/rwim (NE-RW2) - deleted by R4 / R6: with immediate.cpp and screens.cpp, the files that use it
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "gekko/types.h"
#include "render/frame.h"

namespace gekko::scaffold::im {

// RwIm2DVertex on this platform (r2/SITES_DEVICE.md 2.2): x, y, z, RGBA8, u, v.
constexpr u32 k2DVertexSize = 0x18;
constexpr u32 k2DVertexColour = 0x0C;  // RGBA8, R first
constexpr u32 k2DVertexUv = 0x10;      // u, v

inline float be_float(const u8* p) {
    const u32 bits = (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
                     (static_cast<u32>(p[2]) << 8) | p[3];
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline std::uint32_t rgba8(const u8* rgba) {
    return static_cast<std::uint32_t>(rgba[0]) | (static_cast<std::uint32_t>(rgba[1]) << 8) |
           (static_cast<std::uint32_t>(rgba[2]) << 16) | (static_cast<std::uint32_t>(rgba[3]) << 24);
}

// A primitive's u16 indices (big-endian in the address window; null: none) in host order, for the native immediate
// modes' list_order (runtime/lift/rwim, NE-RW2). Returns null when there are none.
inline const std::uint16_t* host_indices(const u8* indices, u32 index_count, std::vector<std::uint16_t>* out) {
    if (indices == nullptr)
        return nullptr;
    out->resize(index_count);
    for (u32 i = 0; i < index_count; ++i)
        (*out)[i] = static_cast<std::uint16_t>((indices[i * 2u] << 8) | indices[i * 2u + 1u]);
    return out->data();
}

}  // namespace gekko::scaffold::im
