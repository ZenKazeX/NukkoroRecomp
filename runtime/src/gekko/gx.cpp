#include "gekko/gx.h"

namespace gekko {
namespace {

u32 component_size(u32 format) {
    switch (format) {
        case 0: return 1;  // u8
        case 1: return 1;  // s8
        case 2: return 2;  // u16
        case 3: return 2;  // s16
        default: return 4;  // f32
    }
}

u32 colour_size(u32 format) {
    // Flipper stores the colour by COLFMT alone. COLCNT selects RGB vs RGBA
    // for lighting; it does not change the byte count. Matching
    // ModernGekko/Dolphin ColorDirectSize {2,3,4,2,3,4,0,0}.
    static constexpr u32 kSizes[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return kSizes[format & 7u];
}

u32 indexed_size(AttributeMode mode) {
    if (mode == AttributeMode::Index8) {
        return 1;
    }
    if (mode == AttributeMode::Index16) {
        return 2;
    }
    return 0;
}

}  // namespace

VertexFormat decode_vertex_format(u32 low, u32 high, u32 vat_a, u32 vat_b, u32 vat_c) noexcept {
    VertexFormat format;
    format.position_matrix_index = (low & 1u) != 0u;
    for (u32 index = 0; index < 8; ++index) {
        format.texture_matrix_index[index] = ((low >> (1u + index)) & 1u) != 0u;
    }
    format.position = static_cast<AttributeMode>((low >> 9) & 3u);
    format.normal = static_cast<AttributeMode>((low >> 11) & 3u);
    format.color[0] = static_cast<AttributeMode>((low >> 13) & 3u);
    format.color[1] = static_cast<AttributeMode>((low >> 15) & 3u);
    for (u32 index = 0; index < 8; ++index) {
        format.texture_coordinate[index] = static_cast<AttributeMode>((high >> (index * 2u)) & 3u);
    }

    u32 stride = 0;
    if (format.position_matrix_index) {
        stride += 1;
    }
    for (bool present : format.texture_matrix_index) {
        if (present) {
            stride += 1;
        }
    }

    if (format.position == AttributeMode::Direct) {
        const u32 elements = ((vat_a >> 0) & 1u) != 0u ? 3u : 2u;
        stride += elements * component_size((vat_a >> 1) & 7u);
    } else {
        stride += indexed_size(format.position);
    }

    if (format.normal == AttributeMode::Direct) {
        const bool nbt = ((vat_a >> 9) & 1u) != 0u;
        stride += (nbt ? 9u : 3u) * component_size((vat_a >> 10) & 7u);
    } else {
        // VAT_A bit 31 (NormalIndex3): NBT sends three separate indices
        // instead of one packed-NBT index. ModernGekko GetVertexSize does the
        // same multiply.
        const bool nbt = ((vat_a >> 9) & 1u) != 0u;
        const bool index3 = ((vat_a >> 31) & 1u) != 0u;
        const u32 indices = (nbt && index3) ? 3u : 1u;
        stride += indices * indexed_size(format.normal);
    }

    for (u32 index = 0; index < 2; ++index) {
        if (format.color[index] == AttributeMode::Direct) {
            const u32 shift = index == 0 ? 13u : 17u;
            const u32 component = (vat_a >> (shift + 1u)) & 7u;
            stride += colour_size(component);
        } else {
            stride += indexed_size(format.color[index]);
        }
    }

    // Texture coordinate 0 lives in VAT_A, 1-4 in VAT_B, 5-7 in VAT_C.
    const auto coordinate_fields = [&](u32 index, u32& elements, u32& component) {
        if (index == 0) {
            elements = (vat_a >> 21) & 1u;
            component = (vat_a >> 22) & 7u;
        } else if (index <= 4) {
            const u32 shift = (index - 1u) * 9u;
            elements = (vat_b >> shift) & 1u;
            component = (vat_b >> (shift + 1u)) & 7u;
        } else {
            const u32 shift = (index - 5u) * 9u;
            elements = (vat_c >> shift) & 1u;
            component = (vat_c >> (shift + 1u)) & 7u;
        }
    };
    for (u32 index = 0; index < 8; ++index) {
        if (format.texture_coordinate[index] == AttributeMode::Direct) {
            u32 elements = 0;
            u32 component = 0;
            coordinate_fields(index, elements, component);
            stride += (elements != 0u ? 2u : 1u) * component_size(component);
        } else {
            stride += indexed_size(format.texture_coordinate[index]);
        }
    }

    format.stride = stride;
    return format;
}

}  // namespace gekko
