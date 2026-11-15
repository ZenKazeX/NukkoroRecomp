// Dolphin-style custom texture packs (gekko/texture_packs.h; the rule and its proof: docs/mods/TEXTURE_PACKS.md).
// Written from the file-name convention and the XXH64 specification; no code from other projects is used.

#include "gekko/texture_packs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <system_error>
#include <unordered_map>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#if defined(_MSC_VER)
#pragma warning(disable : 4505)  // stb_image's unused static functions (reported at the end of the unit)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4244 4245 4456 4457 4701 4703 4996 6001 6011 6262 6385 6386)
#endif
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace gekko::texture_packs {

namespace detail {
std::atomic<bool> g_enabled{false};
}

// ---- XXH64 (the published xxHash specification, little-endian lanes) ----------------------------------------------

namespace {

constexpr std::uint64_t kP1 = 0x9E3779B185EBCA87ull;
constexpr std::uint64_t kP2 = 0xC2B2AE3D27D4EB4Full;
constexpr std::uint64_t kP3 = 0x165667B19E3779F9ull;
constexpr std::uint64_t kP4 = 0x85EBCA77C2B2AE63ull;
constexpr std::uint64_t kP5 = 0x27D4EB2F165667C5ull;

inline std::uint64_t rotl(std::uint64_t v, int r) noexcept { return (v << r) | (v >> (64 - r)); }
inline std::uint64_t read64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline std::uint32_t read32le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t xround(std::uint64_t acc, std::uint64_t input) noexcept {
    acc += input * kP2;
    acc = rotl(acc, 31);
    return acc * kP1;
}
inline std::uint64_t xmerge(std::uint64_t acc, std::uint64_t val) noexcept {
    acc ^= xround(0, val);
    return acc * kP1 + kP4;
}

}  // namespace

std::uint64_t xxh64(const void* data, std::size_t size, std::uint64_t seed) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    const std::uint8_t* const end = p + size;
    std::uint64_t h;
    if (size >= 32u) {
        std::uint64_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
        const std::uint8_t* const limit = end - 32;
        do {
            v1 = xround(v1, read64(p));
            v2 = xround(v2, read64(p + 8));
            v3 = xround(v3, read64(p + 16));
            v4 = xround(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xmerge(h, v1);
        h = xmerge(h, v2);
        h = xmerge(h, v3);
        h = xmerge(h, v4);
    } else {
        h = seed + kP5;
    }
    h += static_cast<std::uint64_t>(size);
    while (p + 8 <= end) {
        h ^= xround(0, read64(p));
        h = rotl(h, 27) * kP1 + kP4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<std::uint64_t>(read32le(p)) * kP1;
        h = rotl(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<std::uint64_t>(*p) * kP5;
        h = rotl(h, 11) * kP1;
        ++p;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

// ---- the naming rule ------------------------------------------------------------------------------------------------

std::size_t base_level_bytes(std::uint32_t format, std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t bw = 0, bh = 0, bpp = 0;
    switch (format) {
        case 0: case 8: case 14: bw = 8; bh = 8; bpp = 4; break;    // I4, C4, CMPR
        case 1: case 2: case 9: bw = 8; bh = 4; bpp = 8; break;     // I8, IA4, C8
        case 3: case 4: case 5: case 10: bw = 4; bh = 4; bpp = 16; break;  // IA8, RGB565, RGB5A3, C14X2
        case 6: bw = 4; bh = 4; bpp = 32; break;                    // RGBA8
        default: return 0;
    }
    if (width == 0u || height == 0u) return 0;
    const std::size_t w = (static_cast<std::size_t>(width) + bw - 1u) / bw * bw;
    const std::size_t h = (static_cast<std::size_t>(height) + bh - 1u) / bh * bh;
    return w * h * bpp / 8u;
}

std::optional<Key> compute_key(std::uint32_t format, std::uint32_t width, std::uint32_t height,
                               const std::uint8_t* texels, std::size_t texel_bytes, const std::uint8_t* palette,
                               std::size_t palette_bytes) noexcept {
    const std::size_t base = base_level_bytes(format, width, height);
    if (base == 0u || texels == nullptr || texel_bytes < base) return std::nullopt;
    Key key;
    key.width = width;
    key.height = height;
    key.format = format;
    key.texel_hash = xxh64(texels, base);
    if (format == 8u || format == 9u || format == 10u) {
        // The palette range the base level actually indexes.
        std::uint32_t lo = 0xFFFFu, hi = 0u;
        if (format == 8u) {
            for (std::size_t i = 0; i < base; ++i) {
                const std::uint32_t a = texels[i] >> 4, b = texels[i] & 0xFu;
                lo = std::min(lo, std::min(a, b));
                hi = std::max(hi, std::max(a, b));
            }
        } else if (format == 9u) {
            for (std::size_t i = 0; i < base; ++i) {
                lo = std::min<std::uint32_t>(lo, texels[i]);
                hi = std::max<std::uint32_t>(hi, texels[i]);
            }
        } else {
            for (std::size_t i = 0; i + 1u < base; i += 2u) {
                const std::uint32_t v = ((static_cast<std::uint32_t>(texels[i]) << 8) | texels[i + 1u]) & 0x3FFFu;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        std::size_t from = static_cast<std::size_t>(lo) * 2u, to = (static_cast<std::size_t>(hi) + 1u) * 2u;
        // A palette shorter than the indices used: hash what exists (no Dolphin pack can name this raster anyway).
        if (palette == nullptr) from = to = 0u;
        to = std::min(to, palette_bytes);
        from = std::min(from, to);
        key.has_palette = true;
        key.palette_hash = xxh64(palette != nullptr ? palette + from : texels, to - from);
    }
    return key;
}

std::string dolphin_name(const Key& key, bool mipmapped) {
    char buffer[128];
    if (key.has_palette) {
        std::snprintf(buffer, sizeof(buffer), "tex1_%ux%u%s_%016llx_%016llx_%u", key.width, key.height,
                      mipmapped ? "_m" : "", static_cast<unsigned long long>(key.texel_hash),
                      static_cast<unsigned long long>(key.palette_hash), key.format);
    } else {
        std::snprintf(buffer, sizeof(buffer), "tex1_%ux%u%s_%016llx_%u", key.width, key.height,
                      mipmapped ? "_m" : "", static_cast<unsigned long long>(key.texel_hash), key.format);
    }
    return buffer;
}

namespace {

bool parse_u32(std::string_view text, std::uint32_t& out) {
    if (text.empty() || text.size() > 9u) return false;
    std::uint32_t v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        v = v * 10u + static_cast<std::uint32_t>(c - '0');
    }
    out = v;
    return true;
}

bool parse_hex16(std::string_view text, std::uint64_t& out) {
    if (text.size() != 16u) return false;
    std::uint64_t v = 0;
    for (char c : text) {
        std::uint64_t d;
        if (c >= '0' && c <= '9') d = static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<std::uint64_t>(c - 'a' + 10);
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

}  // namespace

std::optional<ParsedName> parse_name(std::string_view filename) {
    std::string name(filename);
    for (char& c : name) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    ParsedName out;
    if (name.size() > 4u && name.compare(name.size() - 4u, 4u, ".png") == 0) {
        out.dds = false;
    } else if (name.size() > 4u && name.compare(name.size() - 4u, 4u, ".dds") == 0) {
        out.dds = true;
    } else {
        return std::nullopt;
    }
    name.resize(name.size() - 4u);
    std::vector<std::string_view> tokens;
    {
        std::string_view rest(name);
        while (true) {
            const std::size_t at = rest.find('_');
            tokens.push_back(rest.substr(0, at));
            if (at == std::string_view::npos) break;
            rest.remove_prefix(at + 1u);
        }
    }
    std::size_t t = 0;
    if (tokens.size() < 4u || tokens[t++] != "tex1") return std::nullopt;
    {
        const std::string_view size = tokens[t++];
        const std::size_t x = size.find('x');
        if (x == std::string_view::npos || !parse_u32(size.substr(0, x), out.key.width) ||
            !parse_u32(size.substr(x + 1u), out.key.height) || out.key.width == 0u || out.key.height == 0u)
            return std::nullopt;
    }
    if (t < tokens.size() && tokens[t] == "m") {
        out.mipmapped = true;
        ++t;
    }
    if (t >= tokens.size() || !parse_hex16(tokens[t++], out.key.texel_hash)) return std::nullopt;
    if (t < tokens.size() && tokens[t] == "$") {
        out.key.has_palette = true;
        out.palette_wildcard = true;
        ++t;
    } else if (t < tokens.size() && tokens[t].size() == 16u) {
        if (!parse_hex16(tokens[t++], out.key.palette_hash)) return std::nullopt;
        out.key.has_palette = true;
    }
    if (t >= tokens.size() || !parse_u32(tokens[t++], out.key.format)) return std::nullopt;
    if (t < tokens.size()) {
        const std::string_view suffix = tokens[t++];
        std::uint32_t level = 0;
        if (suffix == "arb") {
            out.arbitrary_mipmaps = true;
        } else if (suffix.size() > 3u && suffix.substr(0, 3) == "mip" && parse_u32(suffix.substr(3), level)) {
            out.mip_level = static_cast<int>(level);
        } else {
            return std::nullopt;
        }
    }
    if (t != tokens.size()) return std::nullopt;
    return out;
}

// ---- decoders -------------------------------------------------------------------------------------------------------

bool decode_png(const std::uint8_t* data, std::size_t size, Image& out, std::string* error) {
    if (size > 0x7FFFFFFFu) {
        if (error) *error = "file too large";
        return false;
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        if (error) *error = std::string("png: ") + (stbi_failure_reason() ? stbi_failure_reason() : "decode failed");
        return false;
    }
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.rgba8.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4u);
    stbi_image_free(pixels);
    return true;
}

namespace {

void rgb565(std::uint32_t v, std::uint8_t* rgb) {
    const std::uint32_t r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
    rgb[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
    rgb[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
    rgb[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
}

// One BC1 colour block into a 4x4 RGBA tile (16 * 4 bytes). `three_colour` allows the c0 <= c1 mode (BC1 only).
void bc1_colour(const std::uint8_t* block, std::uint8_t* tile, bool three_colour) {
    const std::uint32_t c0 = block[0] | (block[1] << 8), c1 = block[2] | (block[3] << 8);
    std::uint8_t palette[4][4];
    rgb565(c0, palette[0]);
    rgb565(c1, palette[1]);
    palette[0][3] = palette[1][3] = 255;
    if (c0 > c1 || !three_colour) {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = static_cast<std::uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
            palette[3][i] = static_cast<std::uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = static_cast<std::uint8_t>((palette[0][i] + palette[1][i]) / 2);
            palette[3][i] = 0;
        }
        palette[2][3] = 255;
        palette[3][3] = 0;
    }
    const std::uint32_t bits = read32le(block + 4);
    for (int i = 0; i < 16; ++i) std::memcpy(tile + i * 4, palette[(bits >> (2 * i)) & 3u], 4);
}

void bc3_alpha(const std::uint8_t* block, std::uint8_t* tile) {
    const std::uint32_t a0 = block[0], a1 = block[1];
    std::uint32_t alpha[8];
    alpha[0] = a0;
    alpha[1] = a1;
    if (a0 > a1) {
        for (std::uint32_t i = 1; i < 7; ++i) alpha[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (std::uint32_t i = 1; i < 5; ++i) alpha[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        alpha[6] = 0;
        alpha[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 5; i >= 0; --i) bits = (bits << 8) | block[2 + i];
    for (int i = 0; i < 16; ++i) tile[i * 4 + 3] = static_cast<std::uint8_t>(alpha[(bits >> (3 * i)) & 7u]);
}

int mask_shift(std::uint32_t mask) {
    if (mask == 0u) return 0;
    int s = 0;
    while (((mask >> s) & 1u) == 0u) ++s;
    return s;
}
std::uint8_t mask_value(std::uint32_t pixel, std::uint32_t mask) {
    if (mask == 0u) return 255;
    const int shift = mask_shift(mask);
    const std::uint32_t max = mask >> shift;
    const std::uint32_t v = (pixel & mask) >> shift;
    return static_cast<std::uint8_t>(max == 255u ? v : (v * 255u + max / 2u) / max);
}

}  // namespace

bool decode_dds(const std::uint8_t* data, std::size_t size, Image& out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = std::string("dds: ") + why;
        return false;
    };
    if (size < 128u || std::memcmp(data, "DDS ", 4) != 0) return fail("bad magic");
    const std::uint8_t* h = data + 4;
    if (read32le(h) != 124u) return fail("bad header size");
    const std::uint32_t height = read32le(h + 8), width = read32le(h + 12);
    if (width == 0u || height == 0u || width > 16384u || height > 16384u) return fail("bad extent");
    const std::uint8_t* pf = h + 72;
    const std::uint32_t pf_flags = read32le(pf + 4);
    const std::uint32_t fourcc = read32le(pf + 8);
    std::uint32_t bits = read32le(pf + 12);
    std::uint32_t masks[4] = {read32le(pf + 16), read32le(pf + 20), read32le(pf + 24), read32le(pf + 28)};
    std::size_t offset = 128u;
    enum class Kind { None, Bc1, Bc2, Bc3, Masked } kind = Kind::None;
    auto fcc = [](const char* s) { return read32le(reinterpret_cast<const std::uint8_t*>(s)); };
    if ((pf_flags & 0x4u) != 0u) {
        if (fourcc == fcc("DXT1")) kind = Kind::Bc1;
        else if (fourcc == fcc("DXT2") || fourcc == fcc("DXT3")) kind = Kind::Bc2;
        else if (fourcc == fcc("DXT4") || fourcc == fcc("DXT5")) kind = Kind::Bc3;
        else if (fourcc == fcc("DX10")) {
            if (size < 148u) return fail("short DX10 header");
            const std::uint32_t dxgi = read32le(data + 128);
            offset = 148u;
            switch (dxgi) {
                case 70: case 71: case 72: kind = Kind::Bc1; break;
                case 73: case 74: case 75: kind = Kind::Bc2; break;
                case 76: case 77: case 78: kind = Kind::Bc3; break;
                case 27: case 28: case 29:  // R8G8B8A8
                    kind = Kind::Masked; bits = 32u;
                    masks[0] = 0xFFu; masks[1] = 0xFF00u; masks[2] = 0xFF0000u; masks[3] = 0xFF000000u;
                    break;
                case 87: case 90: case 91:  // B8G8R8A8
                    kind = Kind::Masked; bits = 32u;
                    masks[0] = 0xFF0000u; masks[1] = 0xFF00u; masks[2] = 0xFFu; masks[3] = 0xFF000000u;
                    break;
                case 88: case 92: case 93:  // B8G8R8X8
                    kind = Kind::Masked; bits = 32u;
                    masks[0] = 0xFF0000u; masks[1] = 0xFF00u; masks[2] = 0xFFu; masks[3] = 0u;
                    break;
                case 97: case 98: case 99: return fail("BC7 is not supported (re-save the file as PNG, BC1-BC3 or RGBA)");
                default: return fail("unsupported DXGI format");
            }
        } else {
            return fail("unsupported FourCC (supported: DXT1-DXT5, DX10 BC1-BC3 / RGBA8 / BGRA8)");
        }
    } else if ((pf_flags & 0x40u) != 0u && (bits == 32u || bits == 24u)) {
        kind = Kind::Masked;
        if ((pf_flags & 0x1u) == 0u) masks[3] = 0u;
    } else {
        return fail("unsupported pixel format (luminance / 16-bit / palettised)");
    }
    out.width = width;
    out.height = height;
    out.rgba8.assign(static_cast<std::size_t>(width) * height * 4u, 0);
    if (kind == Kind::Masked) {
        const std::size_t bpp = bits / 8u;
        const std::size_t pitch = width * bpp;
        if (size < offset + pitch * height) return fail("truncated pixel data");
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* row = data + offset + pitch * y;
            for (std::uint32_t x = 0; x < width; ++x) {
                std::uint32_t pixel = 0;
                for (std::size_t b = 0; b < bpp; ++b) pixel |= static_cast<std::uint32_t>(row[x * bpp + b]) << (8u * b);
                std::uint8_t* o = &out.rgba8[(static_cast<std::size_t>(y) * width + x) * 4u];
                for (int c = 0; c < 4; ++c) o[c] = mask_value(pixel, masks[c]);
            }
        }
        return true;
    }
    const std::size_t block_bytes = kind == Kind::Bc1 ? 8u : 16u;
    const std::uint32_t bx = (width + 3u) / 4u, by = (height + 3u) / 4u;
    if (size < offset + block_bytes * bx * by) return fail("truncated block data");
    std::uint8_t tile[64];
    for (std::uint32_t ty = 0; ty < by; ++ty) {
        for (std::uint32_t tx = 0; tx < bx; ++tx) {
            const std::uint8_t* block = data + offset + block_bytes * (static_cast<std::size_t>(ty) * bx + tx);
            if (kind == Kind::Bc1) {
                bc1_colour(block, tile, true);
            } else if (kind == Kind::Bc2) {
                bc1_colour(block + 8, tile, false);
                for (int i = 0; i < 16; ++i) {
                    const std::uint32_t a = (block[i / 2] >> ((i & 1) * 4)) & 0xFu;
                    tile[i * 4 + 3] = static_cast<std::uint8_t>(a * 17u);
                }
            } else {
                bc1_colour(block + 8, tile, false);
                bc3_alpha(block, tile);
            }
            for (std::uint32_t py = 0; py < 4u; ++py) {
                const std::uint32_t y = ty * 4u + py;
                if (y >= height) break;
                for (std::uint32_t px = 0; px < 4u; ++px) {
                    const std::uint32_t x = tx * 4u + px;
                    if (x >= width) break;
                    std::memcpy(&out.rgba8[(static_cast<std::size_t>(y) * width + x) * 4u], tile + (py * 4u + px) * 4u, 4);
                }
            }
        }
    }
    return true;
}

// ---- the index and the cache ----------------------------------------------------------------------------------------

namespace {

struct IndexKey {
    Key key;
    bool wildcard = false;
    bool mipmapped = false;
    bool operator==(const IndexKey&) const = default;
};
struct IndexKeyHash {
    std::size_t operator()(const IndexKey& k) const noexcept {
        std::uint64_t h = k.key.texel_hash ^ (k.key.palette_hash * 0x9E3779B97F4A7C15ull);
        h ^= (static_cast<std::uint64_t>(k.key.width) << 40) ^ (static_cast<std::uint64_t>(k.key.height) << 20) ^
             (static_cast<std::uint64_t>(k.key.format) << 4) ^ (k.key.has_palette ? 1u : 0u) ^
             (k.wildcard ? 2u : 0u) ^ (k.mipmapped ? 8u : 0u);
        return static_cast<std::size_t>(h * 0xFF51AFD7ED558CCDull);
    }
};

struct FileEntry {
    std::filesystem::path path;
    bool dds = false;
    bool failed = false;
    std::shared_ptr<const texture_overrides::Override> cached;
};

constexpr std::uint64_t kCacheBudgetBytes = 1024ull * 1024ull * 1024ull;

struct State {
    std::mutex mutex;
    std::vector<std::filesystem::path> dirs;
    std::vector<FileEntry> files;
    std::unordered_map<IndexKey, std::size_t, IndexKeyHash> index;
    Stats stats;
    bool exit_report_registered = false;
    bool dirty = false;  // directories added by add_texture_pack, indexed at the first lookup
};

State& state() {
    static State s;
    return s;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    return text;
}

void rebuild_locked(State& s) {
    s.files.clear();
    s.index.clear();
    // Lookup and load counters are cumulative for the process; the index counters and residency describe the new
    // index (the cache went with the old file list).
    s.stats.directories = s.dirs.size();
    s.stats.files_indexed = s.stats.files_shadowed = s.stats.files_ignored = 0u;
    s.stats.resident_bytes = 0u;
    for (const std::filesystem::path& dir : s.dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            std::cerr << "[texture-packs] not a directory, skipped: " << dir.string() << "\n";
            continue;
        }
        // Sorted, so which of two same-named files in one pack wins does not depend on the file system's order.
        std::vector<std::filesystem::path> found;
        for (std::filesystem::recursive_directory_iterator it(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string name = lower(it->path().filename().string());
            if (name.rfind("tex1_", 0) != 0) continue;
            found.push_back(it->path());
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
            return lower(a.generic_string()) < lower(b.generic_string());
        });
        for (const std::filesystem::path& path : found) {
            const std::optional<ParsedName> parsed = parse_name(path.filename().string());
            if (!parsed || parsed->mip_level >= 0) {
                ++s.stats.files_ignored;
                continue;
            }
            IndexKey key{parsed->key, parsed->palette_wildcard, parsed->mipmapped};
            if (parsed->palette_wildcard) key.key.palette_hash = 0u;
            if (s.index.count(key) != 0u) {
                ++s.stats.files_shadowed;
                continue;
            }
            s.index.emplace(key, s.files.size());
            s.files.push_back(FileEntry{path, parsed->dds, false, nullptr});
            ++s.stats.files_indexed;
        }
    }
    detail::g_enabled.store(!s.files.empty(), std::memory_order_relaxed);
}

void exit_report() {
    if (texture_pack_stats().directories != 0u) print_texture_pack_stats(std::cout);
}

void register_exit_report_locked(State& s) {
    if (!s.exit_report_registered && !s.dirs.empty()) {
        s.exit_report_registered = true;
        std::atexit(exit_report);
    }
}

std::shared_ptr<const texture_overrides::Override> load_locked(State& s, FileEntry& file) {
    if (file.cached) return file.cached;
    if (file.failed) return nullptr;
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(file.path, std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            const std::streamoff length = in.tellg();
            in.seekg(0, std::ios::beg);
            if (length > 0 && length < (std::streamoff(1) << 31)) {
                bytes.resize(static_cast<std::size_t>(length));
                in.read(reinterpret_cast<char*>(bytes.data()), length);
                if (!in) bytes.clear();
            }
        }
    }
    Image image;
    std::string error = "unreadable file";
    const bool ok = !bytes.empty() && (file.dds ? decode_dds(bytes.data(), bytes.size(), image, &error)
                                                : decode_png(bytes.data(), bytes.size(), image, &error));
    if (!ok || image.width == 0u || image.height == 0u || image.width > 8192u || image.height > 8192u) {
        if (ok) error = "extent outside 1..8192";
        file.failed = true;
        ++s.stats.load_failures;
        std::cerr << "[texture-packs] cannot use " << file.path.string() << ": " << error << "\n";
        return nullptr;
    }
    // The GX RGBA8 tile layout needs whole 4x4 blocks: an odd extent is padded by repeating the edge texels.
    const std::uint32_t w = (image.width + 3u) & ~3u, h = (image.height + 3u) & ~3u;
    std::vector<std::uint8_t> padded;
    const std::uint8_t* rgba = image.rgba8.data();
    if (w != image.width || h != image.height) {
        padded.resize(static_cast<std::size_t>(w) * h * 4u);
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::uint32_t sy = std::min(y, image.height - 1u);
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint32_t sx = std::min(x, image.width - 1u);
                std::memcpy(&padded[(static_cast<std::size_t>(y) * w + x) * 4u],
                            &image.rgba8[(static_cast<std::size_t>(sy) * image.width + sx) * 4u], 4);
            }
        }
        rgba = padded.data();
    }
    auto replacement = std::make_shared<texture_overrides::Override>();
    replacement->width = w;
    replacement->height = h;
    replacement->gx_rgba8 = texture_overrides::encode_gx_rgba8(rgba, w, h);
    if (s.stats.resident_bytes + replacement->gx_rgba8.size() > kCacheBudgetBytes) {
        for (FileEntry& other : s.files) other.cached.reset();
        s.stats.resident_bytes = 0u;
        ++s.stats.cache_drops;
    }
    s.stats.resident_bytes += replacement->gx_rgba8.size();
    ++s.stats.files_loaded;
    std::cout << "[texture-packs] loaded " << file.path.filename().string() << " (" << image.width << "x"
              << image.height << ")\n";
    s.stats.decode_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    file.cached = std::move(replacement);
    return file.cached;
}

void index_locked(State& s) {
    s.dirty = false;
    rebuild_locked(s);
    register_exit_report_locked(s);
    if (!s.dirs.empty()) {
        std::cout << "[texture-packs] " << s.stats.files_indexed << " textures indexed from " << s.dirs.size()
                  << " director" << (s.dirs.size() == 1u ? "y" : "ies") << " (" << s.stats.files_shadowed
                  << " shadowed, " << s.stats.files_ignored << " ignored)\n";
    }
}

void ensure_indexed_locked(State& s) {
    if (s.dirty) index_locked(s);
}

}  // namespace

void configure_texture_packs(std::vector<std::filesystem::path> dirs_highest_priority_first) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.dirs = std::move(dirs_highest_priority_first);
    index_locked(s);
}

void add_texture_pack(const std::filesystem::path& dir) {
    // One scan for all the flags: the index is built at the first lookup (or stats query), not per directory.
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.dirs.push_back(dir);
    s.dirty = true;
    detail::g_enabled.store(true, std::memory_order_relaxed);
}

std::vector<std::filesystem::path> texture_pack_directories() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.dirs;
}

std::shared_ptr<const texture_overrides::Override> find_replacement(std::uint32_t format, std::uint32_t width,
                                                                     std::uint32_t height, const std::uint8_t* texels,
                                                                     std::size_t texel_bytes,
                                                                     const std::uint8_t* palette,
                                                                     std::size_t palette_bytes, bool mipmapped) {
    if (!texture_packs_enabled()) return nullptr;
    const std::optional<Key> key = compute_key(format, width, height, texels, texel_bytes, palette, palette_bytes);
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ensure_indexed_locked(s);
    if (s.files.empty()) return nullptr;
    ++s.stats.lookups;
    if (!key) {
        ++s.stats.misses;
        return nullptr;
    }
    const IndexKey exact{*key, false, mipmapped};
    Key wild_key = *key;
    wild_key.palette_hash = 0u;
    const IndexKey tries[4] = {exact, IndexKey{*key, false, !mipmapped}, IndexKey{wild_key, true, mipmapped},
                               IndexKey{wild_key, true, !mipmapped}};
    for (int i = 0; i < 4; ++i) {
        if (i >= 2 && !key->has_palette) break;
        const auto found = s.index.find(tries[i]);
        if (found == s.index.end()) continue;
        auto replacement = load_locked(s, s.files[found->second]);
        if (!replacement) continue;
        ++s.stats.hits;
        if (i == 1 || i == 3) ++s.stats.mip_flag_fallbacks;
        if (i >= 2) ++s.stats.wildcard_hits;
        return replacement;
    }
    ++s.stats.misses;
    return nullptr;
}

Stats texture_pack_stats() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ensure_indexed_locked(s);
    return s.stats;
}

void print_texture_pack_stats(std::ostream& out) {
    const Stats st = texture_pack_stats();
    out << "[texture-packs] directories " << st.directories << ", indexed " << st.files_indexed << " (shadowed "
        << st.files_shadowed << ", ignored " << st.files_ignored << "); lookups " << st.lookups << ": hits "
        << st.hits << " (mip-flag fallback " << st.mip_flag_fallbacks << ", palette wildcard " << st.wildcard_hits
        << "), misses " << st.misses << "; files loaded " << st.files_loaded << " in " << st.decode_ms
        << " ms, load failures " << st.load_failures << ", resident " << (st.resident_bytes >> 10) << " KiB\n";
}

}  // namespace gekko::texture_packs
