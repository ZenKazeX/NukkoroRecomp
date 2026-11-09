#include "gekko/gx_texture_decode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gekko/profiling.h"

// Texels are assembled as a host u32 whose memory order is R,G,B,A and stored
// with one fixed-size memcpy (a single mov). That order is the little-endian
// packing r | g<<8 | b<<16 | a<<24; the runtime is x64-only (types.h already
// lowers every guest load to a host byte swap). The runtime builds as C++17,
// so this is a preprocessor check rather than std::endian.
#if !defined(_MSC_VER) && \
    !(defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#error "gx_texture_decode packs R,G,B,A as a little-endian u32"
#endif

namespace gekko {
namespace {

// Channel expansion exactly as renderer.cpp expand3/4/5/6: bit replication,
// not rounded scaling v*255/max (which differs for 4 of the 5-bit values and
// 10 of the 6-bit values).
constexpr u32 expand3(u32 value) noexcept { return (value << 5) | (value << 2) | (value >> 1); }
constexpr u32 expand4(u32 value) noexcept { return (value << 4) | value; }
constexpr u32 expand5(u32 value) noexcept { return (value << 3) | (value >> 2); }
constexpr u32 expand6(u32 value) noexcept { return (value << 2) | (value >> 4); }

constexpr u32 pack_rgba(u32 r, u32 g, u32 b, u32 a) noexcept {
    return r | (g << 8) | (b << 16) | (a << 24);
}

inline void store_texel(u8* destination, u32 texel) noexcept {
    std::memcpy(destination, &texel, sizeof(texel));
}

constexpr u32 decode_rgb565(u32 value) noexcept {
    return pack_rgba(expand5((value >> 11) & 0x1fu), expand6((value >> 5) & 0x3fu),
                     expand5(value & 0x1fu), 255u);
}

constexpr u32 decode_rgb5a3(u32 value) noexcept {
    if ((value & 0x8000u) != 0u) {
        return pack_rgba(expand5((value >> 10) & 0x1fu), expand5((value >> 5) & 0x1fu),
                         expand5(value & 0x1fu), 255u);
    }
    return pack_rgba(expand4((value >> 8) & 0x0fu), expand4((value >> 4) & 0x0fu),
                     expand4(value & 0x0fu), expand3((value >> 12) & 0x07u));
}

// IA8 texels and IA8 TLUT entries: the FIRST guest byte is alpha.
constexpr u32 decode_ia8(u32 value) noexcept {
    const u32 level = value & 0xffu;
    return pack_rgba(level, level, level, value >> 8);
}

constexpr u32 decode_tlut_value(u32 tlut_format, u32 entry) noexcept {
    switch (tlut_format) {
        case kGxTlutFmtIA8: return decode_ia8(entry);
        case kGxTlutFmtRGB565: return decode_rgb565(entry);
        default: return decode_rgb5a3(entry);
    }
}

// Walks the GX tile grid in guest storage order and hands the texel callback
// the tile's first byte plus the texel's position inside the tile. Right and
// bottom tiles are clipped to the image: the guest stores whole tiles, but
// the texels past width/height do not exist. The callback is a lambda, so
// every format gets its own fully inlined loop (no per-texel indirect call).
template <typename TexelFn>
void decode_tiles(u32 width, u32 height, u32 tile_width, u32 tile_height, u32 tile_bytes,
                  const u8* data, u8* out, TexelFn texel) noexcept {
    const u32 tiles_x = (width + tile_width - 1u) / tile_width;
    const u32 tiles_y = (height + tile_height - 1u) / tile_height;
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    for (u32 ty = 0; ty < tiles_y; ++ty) {
        const u32 y0 = ty * tile_height;
        const u32 rows = std::min(tile_height, height - y0);
        const std::size_t tile_row = static_cast<std::size_t>(ty) * tiles_x;
        for (u32 tx = 0; tx < tiles_x; ++tx) {
            const u8* const tile = data + (tile_row + tx) * tile_bytes;
            const u32 x0 = tx * tile_width;
            const u32 columns = std::min(tile_width, width - x0);
            for (u32 py = 0; py < rows; ++py) {
                u8* destination = out + static_cast<std::size_t>(y0 + py) * stride +
                                  static_cast<std::size_t>(x0) * 4u;
                for (u32 px = 0; px < columns; ++px, destination += 4) {
                    store_texel(destination, texel(tile, px, py));
                }
            }
        }
    }
}

// CMPR: 8x8 tiles of four 4x4 DXT1 blocks in TL, TR, BL, BR order. The block's
// four colours are resolved once and its 16 selectors index them, where the
// legacy per-texel decoder re-derives both endpoints for every texel. The
// values are identical; only the work per texel differs.
void decode_cmpr(u32 width, u32 height, const u8* data, u8* out) noexcept {
    const u32 tiles_x = (width + 7u) / 8u;
    const u32 tiles_y = (height + 7u) / 8u;
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    for (u32 ty = 0; ty < tiles_y; ++ty) {
        const std::size_t tile_row = static_cast<std::size_t>(ty) * tiles_x;
        for (u32 tx = 0; tx < tiles_x; ++tx) {
            const u8* const tile = data + (tile_row + tx) * 32u;
            for (u32 sub = 0; sub < 4u; ++sub) {
                const u32 x0 = tx * 8u + (sub & 1u) * 4u;
                const u32 y0 = ty * 8u + (sub >> 1) * 4u;
                // A texture narrower or shorter than 5 texels still stores all
                // four sub-blocks of its tile; the unused ones decode nothing.
                if (x0 >= width || y0 >= height) continue;
                const u32 columns = std::min(4u, width - x0);
                const u32 rows = std::min(4u, height - y0);
                const u8* const block = tile + sub * 8u;
                const u32 c0 = load_be16(block);
                const u32 c1 = load_be16(block + 2);
                const u32 bits = load_be32(block + 4);

                const u32 r0 = expand5((c0 >> 11) & 0x1fu);
                const u32 g0 = expand6((c0 >> 5) & 0x3fu);
                const u32 b0 = expand5(c0 & 0x1fu);
                const u32 r1 = expand5((c1 >> 11) & 0x1fu);
                const u32 g1 = expand6((c1 >> 5) & 0x3fu);
                const u32 b1 = expand5(c1 & 0x1fu);
                u32 palette[4];
                palette[0] = pack_rgba(r0, g0, b0, 255u);
                palette[1] = pack_rgba(r1, g1, b1, 255u);
                // The GameCube CMPR palette, which is NOT PC DXT1: the two
                // interpolants are 5/8-3/8 blends, truncated, and in the
                // c0 <= c1 mode entry 3 keeps the averaged colour with alpha 0
                // rather than being transparent black (Dolphin
                // TextureDecoder_Util.h DXTBlend and TextureDecoder_Generic.cpp,
                // identical in its x64 and GPU decoders). The legacy decoder's
                // exact thirds and transparent black were the PC format.
                if (c0 > c1) {
                    palette[2] = pack_rgba((5u * r0 + 3u * r1) >> 3, (5u * g0 + 3u * g1) >> 3,
                                           (5u * b0 + 3u * b1) >> 3, 255u);
                    palette[3] = pack_rgba((3u * r0 + 5u * r1) >> 3, (3u * g0 + 5u * g1) >> 3,
                                           (3u * b0 + 5u * b1) >> 3, 255u);
                } else {
                    palette[2] = pack_rgba((r0 + r1) / 2u, (g0 + g1) / 2u, (b0 + b1) / 2u, 255u);
                    palette[3] = pack_rgba((r0 + r1) / 2u, (g0 + g1) / 2u, (b0 + b1) / 2u, 0u);
                }

                for (u32 py = 0; py < rows; ++py) {
                    u8* destination = out + static_cast<std::size_t>(y0 + py) * stride +
                                      static_cast<std::size_t>(x0) * 4u;
                    const u32 row_bits = bits << (py * 8u);
                    for (u32 px = 0; px < columns; ++px, destination += 4) {
                        store_texel(destination, palette[(row_bits >> (30u - px * 2u)) & 3u]);
                    }
                }
            }
        }
    }
}

// C4/C8: at most 256 distinct texel values, so the palette is resolved into a
// stack table once and each texel is one indexed load.
template <typename IndexFn>
void decode_indexed_small(u32 width, u32 height, u32 tile_width, u32 tile_height,
                          u32 index_count, const u8* data, const u8* tlut_be, u32 tlut_format,
                          u32 tlut_entries, u8* out, IndexFn index_of) noexcept {
    u32 table[256];
    for (u32 index = 0; index < index_count; ++index) {
        // An index past the caller's palette reads as entry 0x0000 (header).
        const u32 entry = index < tlut_entries ? load_be16(tlut_be + index * 2u) : 0u;
        table[index] = decode_tlut_value(tlut_format, entry);
    }
    decode_tiles(width, height, tile_width, tile_height, 32u, data, out,
                 [&](const u8* tile, u32 px, u32 py) { return table[index_of(tile, px, py)]; });
}

// C14X2: up to 16384 indices, too many to pre-resolve on the stack; the TLUT
// format is fixed per instantiation so the per-texel work is a bounds compare,
// a load and the branch-free channel expansion.
template <typename DecodeFn>
void decode_c14x2(u32 width, u32 height, const u8* data, const u8* tlut_be, u32 tlut_entries,
                  u8* out, DecodeFn decode) noexcept {
    decode_tiles(width, height, 4u, 4u, 32u, data, out, [&](const u8* tile, u32 px, u32 py) {
        const u32 index = load_be16(tile + (py * 4u + px) * 2u) & 0x3fffu;
        const u32 entry = index < tlut_entries ? load_be16(tlut_be + index * 2u) : 0u;
        return decode(entry);
    });
}

}  // namespace

bool gx_texture_format_is_palettised(u32 gx_format) noexcept {
    return gx_format == kGxTexFmtC4 || gx_format == kGxTexFmtC8 || gx_format == kGxTexFmtC14X2;
}

std::size_t gx_texture_data_size(u32 gx_format, u32 width, u32 height) noexcept {
    if (width == 0u || height == 0u || width > kGxMaxTextureDimension ||
        height > kGxMaxTextureDimension) {
        return 0u;
    }
    const std::size_t tiles8_x = (width + 7u) / 8u;
    const std::size_t tiles4_x = (width + 3u) / 4u;
    const std::size_t tiles8_y = (height + 7u) / 8u;
    const std::size_t tiles4_y = (height + 3u) / 4u;
    switch (gx_format) {
        case kGxTexFmtI4:
        case kGxTexFmtC4:
        case kGxTexFmtCMPR:
            return tiles8_x * tiles8_y * 32u;
        case kGxTexFmtI8:
        case kGxTexFmtIA4:
        case kGxTexFmtC8:
            return tiles8_x * tiles4_y * 32u;
        case kGxTexFmtIA8:
        case kGxTexFmtRGB565:
        case kGxTexFmtRGB5A3:
        case kGxTexFmtC14X2:
            return tiles4_x * tiles4_y * 32u;
        case kGxTexFmtRGBA8:
            return tiles4_x * tiles4_y * 64u;
        default:
            return 0u;
    }
}

bool gx_decode_texture(u32 gx_format, u32 width, u32 height, const u8* data,
                       std::size_t data_size, const u8* tlut_be, u32 tlut_format,
                       u32 tlut_entries, u8* rgba8_out) noexcept {
    // Opened BEFORE the two guards, not after them. Both refusals below are
    // silent - a short or null buffer returns false, and so does a palettised
    // format with no TLUT - and refusal is the only thing this function can get
    // wrong. A zone that opened after the guards would show nothing at all for
    // exactly the case anyone would be looking for. There are two callers (a
    // new-generation GX texture, and one task per texture per TXD at load), so
    // this is order 10^3 events a second during a stage load and near zero in
    // gameplay.
    GEKKO_PROFILE_ZONE("gpu/texture-decode");
    const std::size_t needed = gx_texture_data_size(gx_format, width, height);
    GEKKO_PROFILE_VALUE(needed);
    if (GEKKO_PROFILE_CONNECTED()) {
        // Formatting costs more than the decode of a small texture, so it only
        // happens while a profiler is actually attached.
        char description[48];
        const int length = std::snprintf(description, sizeof(description), "fmt %u  %ux%u",
                                         gx_format, width, height);
        if (length > 0) {
            GEKKO_PROFILE_TEXT(description, static_cast<std::size_t>(length));
        }
    }
    if (needed == 0u || data == nullptr || rgba8_out == nullptr || data_size < needed) {
        return false;
    }
    if (gx_texture_format_is_palettised(gx_format) &&
        (tlut_be == nullptr || tlut_entries == 0u || tlut_format > kGxTlutFmtRGB5A3)) {
        return false;
    }

    // Deliberately NOT co-located with the zone: the zone opens above the
    // guards so a refusal is counted, and this stays below them so a refusal
    // does not inflate the byte total. The quantity is the decoded OUTPUT, not
    // `needed` - plotting the source size would make CMPR look eight times
    // cheaper than the memory it actually produces.
    GEKKO_PROFILE_ONLY(static const bool bytes_plot_configured = [] {
        GEKKO_PROFILE_PLOT_CONFIG("gpu/texture bytes decoded", GEKKO_PLOT_MEMORY, true, true,
                                  0x4FA8FFu);
        return true;
    }();
                       (void)bytes_plot_configured;)
    GEKKO_PROFILE_PLOT("gpu/texture bytes decoded",
                       static_cast<double>(width) * static_cast<double>(height) * 4.0);

    switch (gx_format) {
        case kGxTexFmtI4:
            decode_tiles(width, height, 8u, 8u, 32u, data, rgba8_out,
                         [](const u8* tile, u32 px, u32 py) {
                             const u32 packed = tile[py * 4u + px / 2u];
                             const u32 level =
                                 expand4((px & 1u) == 0u ? (packed >> 4) : (packed & 0x0fu));
                             return pack_rgba(level, level, level, level);
                         });
            return true;
        case kGxTexFmtI8:
            decode_tiles(width, height, 8u, 4u, 32u, data, rgba8_out,
                         [](const u8* tile, u32 px, u32 py) {
                             const u32 level = tile[py * 8u + px];
                             return pack_rgba(level, level, level, level);
                         });
            return true;
        case kGxTexFmtIA4:
            decode_tiles(width, height, 8u, 4u, 32u, data, rgba8_out,
                         [](const u8* tile, u32 px, u32 py) {
                             const u32 packed = tile[py * 8u + px];
                             const u32 level = expand4(packed & 0x0fu);
                             return pack_rgba(level, level, level, expand4(packed >> 4));
                         });
            return true;
        case kGxTexFmtIA8:
            decode_tiles(width, height, 4u, 4u, 32u, data, rgba8_out,
                         [](const u8* tile, u32 px, u32 py) {
                             return decode_ia8(load_be16(tile + (py * 4u + px) * 2u));
                         });
            return true;
        case kGxTexFmtRGB565:
            decode_tiles(width, height, 4u, 4u, 32u, data, rgba8_out,
                         [](const u8* tile, u32 px, u32 py) {
                             return decode_rgb565(load_be16(tile + (py * 4u + px) * 2u));
                         });
            return true;
        case kGxTexFmtRGB5A3:
            decode_tiles(width, height, 4u, 4u, 32u, data, rgba8_out,
                         [](const u8* tile, u32 px, u32 py) {
                             return decode_rgb5a3(load_be16(tile + (py * 4u + px) * 2u));
                         });
            return true;
        case kGxTexFmtRGBA8:
            // 64-byte tile: sixteen (A,R) pairs, then sixteen (G,B) pairs.
            decode_tiles(width, height, 4u, 4u, 64u, data, rgba8_out,
                         [](const u8* tile, u32 px, u32 py) {
                             const u32 index = (py * 4u + px) * 2u;
                             return pack_rgba(tile[index + 1u], tile[32u + index],
                                              tile[33u + index], tile[index]);
                         });
            return true;
        case kGxTexFmtC4:
            decode_indexed_small(width, height, 8u, 8u, 16u, data, tlut_be, tlut_format,
                                 tlut_entries, rgba8_out, [](const u8* tile, u32 px, u32 py) {
                                     const u32 packed = tile[py * 4u + px / 2u];
                                     return (px & 1u) == 0u ? (packed >> 4) : (packed & 0x0fu);
                                 });
            return true;
        case kGxTexFmtC8:
            decode_indexed_small(width, height, 8u, 4u, 256u, data, tlut_be, tlut_format,
                                 tlut_entries, rgba8_out, [](const u8* tile, u32 px, u32 py) {
                                     return static_cast<u32>(tile[py * 8u + px]);
                                 });
            return true;
        case kGxTexFmtC14X2:
            switch (tlut_format) {
                case kGxTlutFmtIA8:
                    decode_c14x2(width, height, data, tlut_be, tlut_entries, rgba8_out,
                                 [](u32 entry) { return decode_ia8(entry); });
                    break;
                case kGxTlutFmtRGB565:
                    decode_c14x2(width, height, data, tlut_be, tlut_entries, rgba8_out,
                                 [](u32 entry) { return decode_rgb565(entry); });
                    break;
                default:
                    decode_c14x2(width, height, data, tlut_be, tlut_entries, rgba8_out,
                                 [](u32 entry) { return decode_rgb5a3(entry); });
                    break;
            }
            return true;
        case kGxTexFmtCMPR:
            decode_cmpr(width, height, data, rgba8_out);
            return true;
        default:
            return false;  // unreachable: gx_texture_data_size rejected the format
    }
}

}  // namespace gekko
