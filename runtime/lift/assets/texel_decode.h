#pragma once

// Decoding the disc's texel encodings into plain RGBA8 (docs/lift-20261006/R_DESIGN.md 2.6).
//
// Every encoding stores the image as a grid of tiles (8x8 for 4-bit encodings and CMPR, 8x4 for 8-bit, 4x4 for
// 16- and 32-bit), big-endian. Channel widths below 8 bits expand by bit replication. CMPR is the DXT1 block
// algorithm with the format's own palette: the two interpolated colours are 5/8 - 3/8 blends, and in the
// three-colour mode the fourth entry is the averaged colour with alpha 0 - those are the colours the game's
// textures are defined by, so they are kept exactly.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/bytes.h"
#include "assets/texture_dictionary.h"

namespace shadow::assets {

// Decodes one level of `width` x `height` texels into `rgba` (width * height * 4 bytes, rows top first, bytes R, G,
// B, A). `texels` must hold at least level_bytes(encoding, width, height) bytes. Indexed encodings read `palette`
// (big-endian 16-bit entries already loaded) through `palette_encoding`; an index past the palette reads entry 0.
bool decode_level(TexelEncoding encoding, std::uint32_t width, std::uint32_t height, ByteView texels,
                  const std::vector<std::uint16_t>& palette, PaletteEncoding palette_encoding,
                  std::uint8_t* rgba, std::string* error);

}  // namespace shadow::assets
