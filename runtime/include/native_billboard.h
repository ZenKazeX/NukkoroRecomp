#pragma once

// Native renderer arithmetic; no machine registers, guest pointers or devices.
// Not yet promoted into the shipping caller: see NUMERIC_CONTEXT_RETIREMENT.md.
#include <array>
#include <cstddef>
#include <cstdint>
#include "native_render_vertex.h"

namespace shadow::render {
using Point3 = std::array<float, 3>;
struct Billboard {
    Point3 center{};
    std::array<float, 2> size{}; // full extents
    float rotation = 0;
};
struct BillboardBasis { Point3 right{}, up{}; };
struct BillboardCurve {
    std::array<float, 2> period{};
    std::array<float, 2> offset{};
    std::array<float, 2> angle_multiplier{};
    float scale = 0;
    float half_extent = 0;
};
using BillboardCorners = std::array<Point3, 4>;

// Inputs are binary32 values. Arithmetic rounds each operation to nearest-even;
// subnormal arithmetic operands/results and stored coordinates become signed
// zero. The owning native render thread must use the default nearest rounding
// environment. The supplied curve is data, not an implicit global/register bank.
// No allocation or retained state. Caller owns inputs and the returned corners.
BillboardCorners expand_billboard(const Billboard&, const BillboardBasis&,
                                 const BillboardCurve&) noexcept;

// Shader-facing attribute values, already selected by the owning producer.
// These are not serialized resource records or processor format descriptors.
struct BillboardAttributes {
    bool normal_present = false, color_present = false, uv_present = false;
    Point3 normal{};
    std::array<std::array<std::uint8_t, 4>, 4> colors{};
    std::array<std::array<float, 2>, 4> uv{}; // actual output-corner order
};
// Synchronous native batch. Sources must be valid typed arrays for count
// elements; output_capacity is in vertices. Writes exactly 4*count vertices,
// never allocates or retains pointers. The caller owns the channel/material
// selection and output until its final reader completes. A CPU packet can be
// retired after its last upload copy; mapped GPU-consumed output must remain
// immutable until the final consuming fence retires. Absent color0
// uses the canonical capture default (white); other absent channels are zero.
// No channel is inferred from its value. Invalid/null/overflowing
// extents or overlapping source/output storage fail before any output write.
bool emit_billboards(const Billboard* particles, const BillboardAttributes* attributes,
                     std::size_t count, const BillboardBasis&, const BillboardCurve&,
                     RenderVertex* output, std::size_t output_capacity) noexcept;
} // namespace shadow::render
