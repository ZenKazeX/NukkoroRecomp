#include "assets/texel_decode.h"

#include <algorithm>
#include <array>

namespace shadow::assets {

namespace {

constexpr std::uint32_t expand3(std::uint32_t v) noexcept { return (v << 5) | (v << 2) | (v >> 1); }
constexpr std::uint32_t expand4(std::uint32_t v) noexcept { return (v << 4) | v; }
constexpr std::uint32_t expand5(std::uint32_t v) noexcept { return (v << 3) | (v >> 2); }
constexpr std::uint32_t expand6(std::uint32_t v) noexcept { return (v << 2) | (v >> 4); }

struct Rgba {
    std::uint8_t r, g, b, a;
};

constexpr Rgba rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) noexcept {
    return Rgba{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b),
                static_cast<std::uint8_t>(a)};
}

constexpr Rgba from_rgb565(std::uint32_t v) noexcept {
    return rgba(expand5((v >> 11) & 0x1F), expand6((v >> 5) & 0x3F), expand5(v & 0x1F), 255);
}

// Bit 15 set: RGB555, opaque. Clear: alpha in bits 12-14 (3 bits), RGB444.
constexpr Rgba from_rgb5a3(std::uint32_t v) noexcept {
    if ((v & 0x8000u) != 0)
        return rgba(expand5((v >> 10) & 0x1F), expand5((v >> 5) & 0x1F), expand5(v & 0x1F), 255);
    return rgba(expand4((v >> 8) & 0xF), expand4((v >> 4) & 0xF), expand4(v & 0xF), expand3((v >> 12) & 0x7));
}

// The first stored byte is alpha, the second intensity.
constexpr Rgba from_ia8(std::uint32_t v) noexcept {
    const std::uint32_t level = v & 0xFF;
    return rgba(level, level, level, v >> 8);
}

Rgba from_palette_entry(PaletteEncoding encoding, std::uint32_t entry) noexcept {
    switch (encoding) {
    case PaletteEncoding::IA8: return from_ia8(entry);
    case PaletteEncoding::RGB565: return from_rgb565(entry);
    case PaletteEncoding::RGB5A3: return from_rgb5a3(entry);
    }
    return from_rgb5a3(entry);
}

void put(std::uint8_t* rgba_out, std::uint32_t width, std::uint32_t x, std::uint32_t y, Rgba c) noexcept {
    std::uint8_t* p = rgba_out + (static_cast<std::size_t>(y) * width + x) * 4;
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

// Visits every texel in storage order: the tile grid row by row, each tile's texels row by row. Texels of the
// right and bottom tiles that lie outside the image are stored but do not exist.
template <class Texel>
void for_each_tile_texel(std::uint32_t width, std::uint32_t height, std::uint32_t tile_w, std::uint32_t tile_h,
                         std::size_t tile_bytes, const std::uint8_t* data, std::uint8_t* out, Texel texel) {
    const std::uint32_t tiles_x = (width + tile_w - 1) / tile_w;
    const std::uint32_t tiles_y = (height + tile_h - 1) / tile_h;
    for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
            const std::uint8_t* tile = data + (static_cast<std::size_t>(ty) * tiles_x + tx) * tile_bytes;
            for (std::uint32_t py = 0; py < tile_h; ++py) {
                const std::uint32_t y = ty * tile_h + py;
                if (y >= height)
                    break;
                for (std::uint32_t px = 0; px < tile_w; ++px) {
                    const std::uint32_t x = tx * tile_w + px;
                    if (x >= width)
                        break;
                    put(out, width, x, y, texel(tile, px, py));
                }
            }
        }
    }
}

void decode_cmpr(std::uint32_t width, std::uint32_t height, const std::uint8_t* data, std::uint8_t* out) {
    const std::uint32_t tiles_x = (width + 7) / 8;
    const std::uint32_t tiles_y = (height + 7) / 8;
    for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
            const std::uint8_t* tile = data + (static_cast<std::size_t>(ty) * tiles_x + tx) * 32;
            // Four 4x4 blocks per tile: top-left, top-right, bottom-left, bottom-right.
            for (std::uint32_t sub = 0; sub < 4; ++sub) {
                const std::uint32_t x0 = tx * 8 + (sub & 1) * 4;
                const std::uint32_t y0 = ty * 8 + (sub >> 1) * 4;
                if (x0 >= width || y0 >= height)
                    continue;
                const std::uint8_t* block = tile + sub * 8;
                const std::uint32_t c0 = load_be16(block);
                const std::uint32_t c1 = load_be16(block + 2);
                const std::uint32_t r0 = expand5((c0 >> 11) & 0x1F), g0 = expand6((c0 >> 5) & 0x3F),
                                    b0 = expand5(c0 & 0x1F);
                const std::uint32_t r1 = expand5((c1 >> 11) & 0x1F), g1 = expand6((c1 >> 5) & 0x3F),
                                    b1 = expand5(c1 & 0x1F);
                std::array<Rgba, 4> colours{rgba(r0, g0, b0, 255), rgba(r1, g1, b1, 255), Rgba{}, Rgba{}};
                if (c0 > c1) {
                    colours[2] = rgba((5 * r0 + 3 * r1) >> 3, (5 * g0 + 3 * g1) >> 3, (5 * b0 + 3 * b1) >> 3, 255);
                    colours[3] = rgba((3 * r0 + 5 * r1) >> 3, (3 * g0 + 5 * g1) >> 3, (3 * b0 + 5 * b1) >> 3, 255);
                } else {
                    colours[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
                    colours[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);
                }
                // One selector byte per row, the leftmost texel in the two highest bits.
                for (std::uint32_t py = 0; py < 4 && y0 + py < height; ++py) {
                    const std::uint32_t row = block[4 + py];
                    for (std::uint32_t px = 0; px < 4 && x0 + px < width; ++px)
                        put(out, width, x0 + px, y0 + py, colours[(row >> (6 - 2 * px)) & 3]);
                }
            }
        }
    }
}

}  // namespace

bool decode_level(TexelEncoding encoding, std::uint32_t width, std::uint32_t height, ByteView texels,
                  const std::vector<std::uint16_t>& palette, PaletteEncoding palette_encoding,
                  std::uint8_t* out, std::string* error) {
    const std::size_t needed = level_bytes(encoding, width, height);
    if (needed == 0 || texels.size < needed || out == nullptr) {
        if (error != nullptr)
            *error = std::string("cannot decode a ") + encoding_name(encoding) + " level of " + std::to_string(width) +
                     " x " + std::to_string(height) + " from " + std::to_string(texels.size) + " bytes";
        return false;
    }
    const std::uint8_t* d = texels.data;
    auto entry = [&](std::uint32_t index) -> Rgba {
        return from_palette_entry(palette_encoding, index < palette.size() ? palette[index] : 0u);
    };
    switch (encoding) {
    case TexelEncoding::I4:
        for_each_tile_texel(width, height, 8, 8, 32, d, out, [](const std::uint8_t* t, std::uint32_t x, std::uint32_t y) {
            const std::uint32_t byte = t[y * 4 + x / 2];
            const std::uint32_t v = expand4((x & 1) != 0 ? byte & 0xF : byte >> 4);
            return rgba(v, v, v, v);
        });
        break;
    case TexelEncoding::I8:
        for_each_tile_texel(width, height, 8, 4, 32, d, out, [](const std::uint8_t* t, std::uint32_t x, std::uint32_t y) {
            const std::uint32_t v = t[y * 8 + x];
            return rgba(v, v, v, v);
        });
        break;
    case TexelEncoding::IA4:
        for_each_tile_texel(width, height, 8, 4, 32, d, out, [](const std::uint8_t* t, std::uint32_t x, std::uint32_t y) {
            const std::uint32_t byte = t[y * 8 + x];
            const std::uint32_t level = expand4(byte & 0xF);
            return rgba(level, level, level, expand4(byte >> 4));
        });
        break;
    case TexelEncoding::IA8:
        for_each_tile_texel(width, height, 4, 4, 32, d, out, [](const std::uint8_t* t, std::uint32_t x, std::uint32_t y) {
            return from_ia8(load_be16(t + (y * 4 + x) * 2));
        });
        break;
    case TexelEncoding::RGB565:
        for_each_tile_texel(width, height, 4, 4, 32, d, out, [](const std::uint8_t* t, std::uint32_t x, std::uint32_t y) {
            return from_rgb565(load_be16(t + (y * 4 + x) * 2));
        });
        break;
    case TexelEncoding::RGB5A3:
        for_each_tile_texel(width, height, 4, 4, 32, d, out, [](const std::uint8_t* t, std::uint32_t x, std::uint32_t y) {
            return from_rgb5a3(load_be16(t + (y * 4 + x) * 2));
        });
        break;
    case TexelEncoding::RGBA8:
        // Each 4x4 tile is 64 bytes: 16 (alpha, red) pairs, then 16 (green, blue) pairs.
        for_each_tile_texel(width, height, 4, 4, 64, d, out, [](const std::uint8_t* t, std::uint32_t x, std::uint32_t y) {
            const std::uint32_t i = (y * 4 + x) * 2;
            return rgba(t[i + 1], t[32 + i], t[32 + i + 1], t[i]);
        });
        break;
    case TexelEncoding::C4:
        for_each_tile_texel(width, height, 8, 8, 32, d, out, [&](const std::uint8_t* t, std::uint32_t x, std::uint32_t y) {
            const std::uint32_t byte = t[y * 4 + x / 2];
            return entry((x & 1) != 0 ? byte & 0xF : byte >> 4);
        });
        break;
    case TexelEncoding::C8:
        for_each_tile_texel(width, height, 8, 4, 32, d, out, [&](const std::uint8_t* t, std::uint32_t x, std::uint32_t y) {
            return entry(t[y * 8 + x]);
        });
        break;
    case TexelEncoding::C14X2:
        for_each_tile_texel(width, height, 4, 4, 32, d, out, [&](const std::uint8_t* t, std::uint32_t x, std::uint32_t y) {
            return entry(load_be16(t + (y * 4 + x) * 2) & 0x3FFFu);
        });
        break;
    case TexelEncoding::CMPR:
        decode_cmpr(width, height, d, out);
        break;
    }
    return true;
}

}  // namespace shadow::assets
