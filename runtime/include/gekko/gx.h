#pragma once

// What remains of the GX command language: the immediate vertex layout and the
// 2D group kinds.
//
// Phase 4 (docs/native-time-20261005/GX_DATA_PLANE.md) deleted the write-gather
// pipe, its command decoder and every BP / CP / XF register file: the
// generator lowers each store of the image to the pipe by what it meant
// (gekko/gx_sites.h). Two things of the GameCube's vocabulary are still the
// game's own data rather than transport:
//
//   - the immediate vertex layout - the vertex descriptor (VCD) and the vertex
//     attribute format (VAT) the GX library keeps in __GXData. RenderWare's
//     immediate producers write vertices in that layout and the immediate
//     geometry builder (gekko/native_immediate.h) reads it from those shadows
//     to know where a primitive ends; the capture decodes the payload with it;
//   - the 2D group kinds the recovered layout hooks announce (NE-RW2c: defined by the 2D layout policy,
//     runtime/lift/ui/hud_layout.h; this name stays for the legacy renderer and System).

#include <array>

#include "gekko/types.h"
#include "ui/hud_layout.h"

namespace gekko {

// Vertex attribute presence, two bits per attribute.
enum class AttributeMode : u32 {
    None = 0,
    Direct = 1,
    Index8 = 2,
    Index16 = 3,
};

struct VertexFormat {
    // Decoded from the vertex descriptor and the selected attribute table.
    bool position_matrix_index = false;
    std::array<bool, 8> texture_matrix_index{};
    AttributeMode position = AttributeMode::None;
    AttributeMode normal = AttributeMode::None;
    std::array<AttributeMode, 2> color{};
    std::array<AttributeMode, 8> texture_coordinate{};
    u32 stride = 0;

    bool operator==(const VertexFormat& other) const noexcept {
        return position_matrix_index == other.position_matrix_index &&
               texture_matrix_index == other.texture_matrix_index && position == other.position &&
               normal == other.normal && color == other.color &&
               texture_coordinate == other.texture_coordinate && stride == other.stride;
    }
    bool operator!=(const VertexFormat& other) const noexcept { return !(*this == other); }
};

// The layout of one vertex: VCD low / high (the GX library's gx+0x14 / +0x18)
// and the VAT A / B / C words of the primitive's format (gx+0x1C / +0x3C /
// +0x5C + 4 * format). The stride is the payload bytes per vertex: direct
// attributes by their component types and counts, indexed ones by their
// index width, colours by their format alone (COLCNT does not change the
// byte count), NBT normals with NormalIndex3 as three indices.
VertexFormat decode_vertex_format(u32 vcd_low, u32 vcd_high, u32 vat_a, u32 vat_b, u32 vat_c) noexcept;

// Which logical 2D group the game announced: the 2D layout policy's type (NE-RW2c).
using HudGroupKind = shadow::ui::HudGroupKind;

}  // namespace gekko
