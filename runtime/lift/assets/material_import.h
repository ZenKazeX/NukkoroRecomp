#pragma once

// Materials as the assets define them (docs/lift-20261006/R2_DRIVER.md 2.2-2.3, R2_MATERIAL_CENSUS.md 2.4-2.5).
//
// A material is a colour, an optional texture reference (by name, with its sampling: filter and the address mode
// of each axis), lighting coefficients, and optionally an effect: an environment reflection (strength, texture),
// a second texture blended over the first (blend pair, texture), a UV transform driven by a UV animation, or a
// bump-mapped environment. Names are resolved against the texture dictionaries in scope when a stage or model is
// loaded, not here.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/bytes.h"
#include "assets/chunk_stream.h"

namespace shadow::assets {

struct TextureReference {
    bool present = false;
    std::string name;
    std::string mask_name;
    std::uint32_t filter_address = 0;  // filter (bits 0-7), address U (8-11), address V (12-15)
};

enum class EffectKind : std::uint32_t {
    None = 0, Bump = 1, Environment = 2, BumpEnvironment = 3, Dual = 4, UvTransform = 5, DualUvTransform = 6,
};

struct EffectSlot {
    std::uint32_t type = 0;            // 1 bump, 2 environment, 4 dual, 5 UV transform, 0 none
    float coefficient = 0;             // bump / environment strength
    bool framebuffer_alpha = false;    // environment: blend by the target's alpha
    std::uint32_t src_blend = 0;       // dual: the blend pair, as the asset's blend enumeration
    std::uint32_t dst_blend = 0;
    TextureReference texture;          // environment / dual texture, or the bumped texture
    TextureReference texture2;         // the bump texture
};

struct MaterialRecord {
    std::uint32_t flags = 0;
    std::uint8_t colour[4] = {255, 255, 255, 255};
    bool textured = false;
    TextureReference texture;
    float ambient = 1.0f, specular = 1.0f, diffuse = 1.0f;
    EffectKind effect = EffectKind::None;
    std::uint32_t slot_count = 0;
    EffectSlot slots[2];
    std::vector<std::string> uv_animations;  // UV animation names, when the material has them
    bool platform_material = false;          // a console-specific material plugin is attached
};

// Parses a material-list chunk (type 0x08) into one record per list entry (instanced entries copied).
bool import_material_list(ByteView bytes, const Chunk& list, std::vector<MaterialRecord>* out, std::string* error);

}  // namespace shadow::assets
