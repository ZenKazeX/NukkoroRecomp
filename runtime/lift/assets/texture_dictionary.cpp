#include "assets/texture_dictionary.h"

#include <algorithm>

namespace shadow::assets {

namespace {

bool fail(std::string* error, const std::string& what) {
    if (error != nullptr)
        *error = what;
    return false;
}

std::string fixed_string(const std::uint8_t* p, std::size_t capacity) {
    const auto* begin = reinterpret_cast<const char*>(p);
    return std::string(begin, std::find(begin, begin + capacity, '\0'));
}

struct Tile {
    std::uint32_t width, height, bits;
};

bool tile_of(TexelEncoding encoding, Tile* tile) noexcept {
    switch (encoding) {
    case TexelEncoding::I4: case TexelEncoding::C4: case TexelEncoding::CMPR: *tile = {8, 8, 4}; return true;
    case TexelEncoding::I8: case TexelEncoding::IA4: case TexelEncoding::C8: *tile = {8, 4, 8}; return true;
    case TexelEncoding::IA8: case TexelEncoding::RGB565: case TexelEncoding::RGB5A3: case TexelEncoding::C14X2:
        *tile = {4, 4, 16};
        return true;
    case TexelEncoding::RGBA8: *tile = {4, 4, 32}; return true;
    }
    return false;
}

// Record layout inside the texture-native struct chunk (big-endian).
constexpr std::size_t kHeaderBytes = 88 + 16 + 4;

}  // namespace

const char* encoding_name(TexelEncoding encoding) noexcept {
    switch (encoding) {
    case TexelEncoding::I4: return "I4";
    case TexelEncoding::I8: return "I8";
    case TexelEncoding::IA4: return "IA4";
    case TexelEncoding::IA8: return "IA8";
    case TexelEncoding::RGB565: return "RGB565";
    case TexelEncoding::RGB5A3: return "RGB5A3";
    case TexelEncoding::RGBA8: return "RGBA8";
    case TexelEncoding::C4: return "C4";
    case TexelEncoding::C8: return "C8";
    case TexelEncoding::C14X2: return "C14X2";
    case TexelEncoding::CMPR: return "CMPR";
    }
    return "?";
}

bool is_indexed(TexelEncoding encoding) noexcept {
    return encoding == TexelEncoding::C4 || encoding == TexelEncoding::C8 || encoding == TexelEncoding::C14X2;
}

std::size_t level_bytes(TexelEncoding encoding, std::uint32_t width, std::uint32_t height) noexcept {
    Tile tile{};
    if (width == 0 || height == 0 || !tile_of(encoding, &tile))
        return 0;
    const std::size_t padded_w = (width + tile.width - 1) / tile.width * tile.width;
    const std::size_t padded_h = (height + tile.height - 1) / tile.height * tile.height;
    return padded_w * padded_h * tile.bits / 8;
}

bool parse_texture_dictionary(ByteView bytes, const Chunk& dictionary, TextureDictionary* out, std::string* error) {
    *out = TextureDictionary{};
    std::vector<Chunk> kids;
    if (dictionary.type != chunk::kTextureDictionary || !children(bytes, dictionary, &kids))
        return fail(error, "not a texture dictionary");
    if (const Chunk* header = find(kids, chunk::kStruct); header != nullptr && header->size >= 4) {
        out->count = load_le16(bytes.data + header->body());
        out->device = load_le16(bytes.data + header->body() + 2);
    }
    for (const Chunk& native : kids) {
        if (native.type != chunk::kTextureNative)
            continue;
        std::vector<Chunk> parts;
        if (!children(bytes, native, &parts))
            return fail(error, "texture-native chunk with a broken child run");
        const Chunk* body = find(parts, chunk::kStruct);
        if (body == nullptr || body->size < kHeaderBytes)
            return fail(error, "texture-native chunk without its record");
        const std::uint8_t* p = bytes.data + body->body();
        const std::uint8_t* const end = bytes.data + body->end();
        TextureRecord t;
        t.platform = load_be32(p);
        t.filter_address = load_be32(p + 4);
        t.name = fixed_string(p + 24, 32);
        t.mask_name = fixed_string(p + 56, 32);
        p += 88;
        t.raster_flags = load_be32(p);
        t.width = load_be16(p + 4);
        t.height = load_be16(p + 6);
        t.depth = p[8];
        t.levels = p[9];
        t.encoding = static_cast<TexelEncoding>(p[10]);
        t.palette_encoding = static_cast<PaletteEncoding>(p[11]);
        t.has_alpha = load_be32(p + 12);
        p += 16;
        Tile tile{};
        if (!tile_of(t.encoding, &tile))
            return fail(error, "texture '" + t.name + "': unknown texel encoding " + std::to_string(p[-6]));
        if ((t.raster_flags & 0x6000u) != 0) {
            const std::size_t entries = std::size_t{1} << t.depth;
            if (t.depth > 14 || static_cast<std::size_t>(end - p) < entries * 2 + 4)
                return fail(error, "texture '" + t.name + "': palette runs past its record");
            t.palette.resize(entries);
            for (std::size_t i = 0; i < entries; ++i)
                t.palette[i] = load_be16(p + i * 2);
            p += entries * 2;
        }
        if (end - p < 4)
            return fail(error, "texture '" + t.name + "': no texel size");
        const std::uint32_t data_size = load_be32(p);
        p += 4;
        if (static_cast<std::size_t>(end - p) != data_size)
            return fail(error, "texture '" + t.name + "': texel bytes do not end the record");
        std::size_t expected = 0;
        std::uint32_t w = t.width;
        std::uint32_t h = t.height;
        for (std::uint32_t level = 0; level < std::max<std::uint32_t>(t.levels, 1); ++level) {
            expected += level_bytes(t.encoding, std::max<std::uint32_t>(w, 1), std::max<std::uint32_t>(h, 1));
            w >>= 1;
            h >>= 1;
        }
        if (expected != data_size)
            return fail(error, "texture '" + t.name + "': " + std::to_string(data_size) + " texel bytes, the size and "
                               "encoding need " + std::to_string(expected));
        t.texels = ByteView{p, data_size};
        out->textures.push_back(std::move(t));
    }
    return true;
}

}  // namespace shadow::assets
