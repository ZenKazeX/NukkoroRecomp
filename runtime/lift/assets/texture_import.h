#pragma once

// A stored texture becomes a native image (docs/lift-20261006/R_DESIGN.md 2.6).
//
// Every encoding decodes to RGBA8 with the colours the game's textures are defined by (texel_decode.h); a stage's
// textures are a few MiB that way, so block compression would save nothing that matters while costing exact
// colours. The disc stores one level per texture; the importer generates the rest of the mip chain for sampling
// at modern resolutions: each level halves the previous one, colours averaged in linear light and weighted by
// alpha (transparent texels lend no colour to the edges of cut-outs), alpha averaged as is (alpha-to-coverage
// then keeps a cut-out's coverage at every distance).

#include <cstdint>
#include <string>
#include <vector>

#include "assets/texture_dictionary.h"

namespace shadow::assets {

struct TextureImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_levels = 0;
    bool texel_alpha = false;                  // some texel of level 0 has alpha below 255
    std::vector<std::uint8_t> rgba;            // every level, level 0 first, rows packed (width * 4 bytes)
    std::vector<std::size_t> level_offsets;    // byte offset of each level in `rgba`

    std::uint32_t level_width(std::uint32_t level) const noexcept { return width >> level ? width >> level : 1; }
    std::uint32_t level_height(std::uint32_t level) const noexcept { return height >> level ? height >> level : 1; }
};

struct ImportOptions {
    bool generate_mips = true;
};

bool import_texture(const TextureRecord& record, const ImportOptions& options, TextureImage* out, std::string* error);

// Appends levels 1.. to an image that holds level 0 only (exposed for tests).
void generate_mip_chain(TextureImage& image);

}  // namespace shadow::assets
