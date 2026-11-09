#pragma once

// One native shader-facing vertex layout for mesh, immediate and billboard
// production. Coordinate space and channel presence belong to the draw, not
// to this storage type. No processor, device, serialized-format or ABI state.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace shadow::render {
struct RenderVertex {
    float position[3];
    float normal[3];
    std::uint8_t color0[4];
    std::uint8_t color1[4];
    float uv[2][2];
};

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(std::is_standard_layout_v<RenderVertex> && std::is_trivially_copyable_v<RenderVertex>);
static_assert(sizeof(RenderVertex) == 48 && alignof(RenderVertex) == alignof(float));
static_assert(offsetof(RenderVertex, position) == 0 && offsetof(RenderVertex, normal) == 12);
static_assert(offsetof(RenderVertex, color0) == 24 && offsetof(RenderVertex, color1) == 28);
static_assert(offsetof(RenderVertex, uv) == 32);
} // namespace shadow::render
