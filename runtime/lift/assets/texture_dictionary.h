#pragma once

// Texture dictionaries: the game's textures as they are stored on the disc (docs/lift-20261006/R2_DRIVER.md 2.3,
// R2_MATERIAL_CENSUS.md section 5).
//
// A dictionary chunk holds texture-native chunks; each carries a big-endian record: sampling (filter and address
// modes), names, the raster's flags, size, bit depth, level count, texel encoding, palette encoding, a has-alpha
// word, the palette (for indexed encodings) and the texel bytes of every level in the encoding's tile layout.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/bytes.h"
#include "assets/chunk_stream.h"

namespace shadow::assets {

// The texel encodings the format defines (the disc uses C4 and C8 with an RGB565 or RGB5A3 palette, CMPR, RGB565
// and RGB5A3; census section 5).
enum class TexelEncoding : std::uint8_t {
    I4 = 0, I8 = 1, IA4 = 2, IA8 = 3, RGB565 = 4, RGB5A3 = 5, RGBA8 = 6, C4 = 8, C8 = 9, C14X2 = 10, CMPR = 14,
};
enum class PaletteEncoding : std::uint8_t { IA8 = 0, RGB565 = 1, RGB5A3 = 2 };

const char* encoding_name(TexelEncoding encoding) noexcept;
bool is_indexed(TexelEncoding encoding) noexcept;
// Bytes of one level in the encoding's tile layout (0 for an unknown encoding or an empty size).
std::size_t level_bytes(TexelEncoding encoding, std::uint32_t width, std::uint32_t height) noexcept;

struct TextureRecord {
    std::string name;
    std::string mask_name;
    std::uint32_t platform = 0;
    std::uint32_t filter_address = 0;  // filter (bits 0-7), address U (8-11), address V (12-15)
    std::uint32_t raster_flags = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t depth = 0;
    std::uint8_t levels = 0;
    TexelEncoding encoding = TexelEncoding::I4;
    PaletteEncoding palette_encoding = PaletteEncoding::IA8;
    std::uint32_t has_alpha = 0;       // the whole stored word
    std::vector<std::uint16_t> palette;
    ByteView texels;                   // every level, in the stream the record came from
};

struct TextureDictionary {
    std::uint16_t count = 0;   // as declared
    std::uint16_t device = 0;
    std::vector<TextureRecord> textures;
};

// Parses a texture-dictionary chunk of `bytes`. The records' `texels` point into `bytes`.
bool parse_texture_dictionary(ByteView bytes, const Chunk& dictionary, TextureDictionary* out, std::string* error);

}  // namespace shadow::assets
