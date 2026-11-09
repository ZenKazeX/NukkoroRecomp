// gx_texture_decode tests.
//
//   gx_texture_decode_tests [mem1-path] [png-output-dir]
//
// (a) hand-computed vectors per GX format, (b) a randomized differential test
// and (c) an ORACLE test over the real rasters of an in-stage MEM1 image, all
// against `legacy::Renderer::decode_texel` below - a literal transcription of
// runtime/src/gekko/renderer.cpp, which is the project's exactness reference.
// renderer.cpp is deliberately not linked: the oracle must not drift with it.
// Also: guard-page bounds checks, invalid-input rejection, a 512x512 CMPR
// timing gate, and PNGs of real decoded rasters for visual inspection.

#include "gekko/gx_texture_decode.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

using gekko::load_be16;
using gekko::load_be32;
using gekko::s32;
using gekko::u16;
using gekko::u32;
using gekko::u64;
using gekko::u8;

constexpr const char* kDefaultMem1 =
    "H:/ShadowTheHedgehogDiskGC_Extract/RENDERER_RE/_work/geometry/runs/B45.mem1";
constexpr const char* kDefaultPngDir =
    "H:/ShadowTheHedgehogDiskGC_Extract/GekkoForge/scratch/native-runtime-2026-09-16/"
    "gx-texture-decode";

int g_cases = 0;
int g_failures = 0;

bool report(const std::string& name, bool ok, const std::string& detail = {}) {
    ++g_cases;
    if (!ok) ++g_failures;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : " - ",
                detail.c_str());
    return ok;
}

std::string format_text(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

// ---------------------------------------------------------------------------
// ORACLE: literal transcription of runtime/src/gekko/renderer.cpp as of
// 2026-09-16 - pack_colour/expand3..6/decode_rgb565/decode_rgb5a3 (lines 60-88),
// Renderer::decode_texel (1254-1367), Renderer::decode_tlut_entry (1743-1757)
// and the tex_read8/16/32 accessors (renderer.h 2549-2559, bulk-source branch).
// Only the class scaffolding is new: tmem_ is the TLUT memory the legacy
// decoder indexes by tlut_base.
// ---------------------------------------------------------------------------
namespace legacy {

inline u32 pack_colour(int r, int g, int b, int a) {
    return (static_cast<u32>(std::clamp(a, 0, 255)) << 24) |
           (static_cast<u32>(std::clamp(r, 0, 255)) << 16) |
           (static_cast<u32>(std::clamp(g, 0, 255)) << 8) |
           static_cast<u32>(std::clamp(b, 0, 255));
}

inline u32 expand3(u32 value) { return (value << 5) | (value << 2) | (value >> 1); }
inline u32 expand4(u32 value) { return (value << 4) | value; }
inline u32 expand5(u32 value) { return (value << 3) | (value >> 2); }
inline u32 expand6(u32 value) { return (value << 2) | (value >> 4); }

u32 decode_rgb565(u16 value) {
    return pack_colour(static_cast<int>(expand5((value >> 11) & 0x1f)),
                       static_cast<int>(expand6((value >> 5) & 0x3f)),
                       static_cast<int>(expand5(value & 0x1f)), 255);
}

u32 decode_rgb5a3(u16 value) {
    if ((value & 0x8000) != 0) {
        return pack_colour(static_cast<int>(expand5((value >> 10) & 0x1f)),
                           static_cast<int>(expand5((value >> 5) & 0x1f)),
                           static_cast<int>(expand5(value & 0x1f)), 255);
    }
    return pack_colour(static_cast<int>(expand4((value >> 8) & 0x0f)),
                       static_cast<int>(expand4((value >> 4) & 0x0f)),
                       static_cast<int>(expand4(value & 0x0f)),
                       static_cast<int>(expand3((value >> 12) & 0x07)));
}

struct TextureUnit {
    u32 width = 0;
    u32 height = 0;
    u32 format = 0;
    u32 tlut_format = 0;
    u32 tlut_base = 0;
};

struct Span {
    const u8* ptr = nullptr;
    std::size_t length = 0;
    std::size_t size() const { return length; }
    const u8* data() const { return ptr; }
};

struct Renderer {
    Span tmem_;

    u8 tex_read8(const TextureUnit&, const u8* source, u32 offset) const {
        return source[offset];
    }
    u16 tex_read16(const TextureUnit&, const u8* source, u32 offset) const {
        return load_be16(source + offset);
    }
    u32 tex_read32(const TextureUnit&, const u8* source, u32 offset) const {
        return load_be32(source + offset);
    }

    u32 decode_tlut_entry(const TextureUnit& unit, u32 palette_index) const {
        const std::size_t entry_offset = static_cast<std::size_t>(unit.tlut_base) +
                                         static_cast<std::size_t>(palette_index) * 2u;
        const u16 entry =
            entry_offset + 1 < tmem_.size() ? load_be16(tmem_.data() + entry_offset) : 0u;
        switch (unit.tlut_format) {
            case 0: {  // IA8
                const int alpha = entry >> 8;
                const int level = entry & 0xff;
                return pack_colour(level, level, level, alpha);
            }
            case 1: return decode_rgb565(entry);
            default: return decode_rgb5a3(entry);
        }
    }

    u32 decode_texel(const TextureUnit& unit, u32 x, u32 y, const u8* source) {
        const u32 width = unit.width;
        switch (unit.format) {
            case 0: {  // I4, 8x8 tiles
                const u32 tiles = (width + 7u) / 8u;
                const u32 tile = (y / 8u) * tiles + (x / 8u);
                const u32 offset = tile * 32u + (y % 8u) * 4u + (x % 8u) / 2u;
                const u8 packed = tex_read8(unit, source, offset);
                const u32 nibble = ((x & 1u) == 0u) ? (packed >> 4) : (packed & 0x0fu);
                const int level = static_cast<int>(expand4(nibble));
                return pack_colour(level, level, level, level);
            }
            case 1: {  // I8, 8x4 tiles
                const u32 tiles = (width + 7u) / 8u;
                const u32 tile = (y / 4u) * tiles + (x / 8u);
                const u32 offset = tile * 32u + (y % 4u) * 8u + (x % 8u);
                const int level = tex_read8(unit, source, offset);
                return pack_colour(level, level, level, level);
            }
            case 2: {  // IA4, 8x4 tiles
                const u32 tiles = (width + 7u) / 8u;
                const u32 tile = (y / 4u) * tiles + (x / 8u);
                const u32 offset = tile * 32u + (y % 4u) * 8u + (x % 8u);
                const u8 packed = tex_read8(unit, source, offset);
                const int level = static_cast<int>(expand4(packed & 0x0fu));
                const int alpha = static_cast<int>(expand4(packed >> 4));
                return pack_colour(level, level, level, alpha);
            }
            case 3: {  // IA8, 4x4 tiles
                const u32 tiles = (width + 3u) / 4u;
                const u32 tile = (y / 4u) * tiles + (x / 4u);
                const u32 offset = tile * 32u + ((y % 4u) * 4u + (x % 4u)) * 2u;
                const u16 packed = tex_read16(unit, source, offset);
                const int alpha = packed >> 8;
                const int level = packed & 0xff;
                return pack_colour(level, level, level, alpha);
            }
            case 4: {  // RGB565, 4x4 tiles
                const u32 tiles = (width + 3u) / 4u;
                const u32 tile = (y / 4u) * tiles + (x / 4u);
                const u32 offset = tile * 32u + ((y % 4u) * 4u + (x % 4u)) * 2u;
                return decode_rgb565(tex_read16(unit, source, offset));
            }
            case 5: {  // RGB5A3, 4x4 tiles
                const u32 tiles = (width + 3u) / 4u;
                const u32 tile = (y / 4u) * tiles + (x / 4u);
                const u32 offset = tile * 32u + ((y % 4u) * 4u + (x % 4u)) * 2u;
                return decode_rgb5a3(tex_read16(unit, source, offset));
            }
            case 6: {  // RGBA8, 4x4 tiles stored as an AR half then a GB half
                const u32 tiles = (width + 3u) / 4u;
                const u32 tile = (y / 4u) * tiles + (x / 4u);
                const u32 base = tile * 64u;
                const u32 index = (y % 4u) * 4u + (x % 4u);
                const u8 alpha = tex_read8(unit, source, base + index * 2u);
                const u8 red = tex_read8(unit, source, base + index * 2u + 1u);
                const u8 green = tex_read8(unit, source, base + 32u + index * 2u);
                const u8 blue = tex_read8(unit, source, base + 32u + index * 2u + 1u);
                return pack_colour(red, green, blue, alpha);
            }
            case 8:
            case 9:
            case 10: {  // Palettised formats
                u32 palette_index = 0;
                if (unit.format == 8) {
                    const u32 tiles = (width + 7u) / 8u;
                    const u32 tile = (y / 8u) * tiles + (x / 8u);
                    const u32 offset = tile * 32u + (y % 8u) * 4u + (x % 8u) / 2u;
                    const u8 packed = tex_read8(unit, source, offset);
                    palette_index = ((x & 1u) == 0u) ? (packed >> 4) : (packed & 0x0fu);
                } else if (unit.format == 9) {
                    const u32 tiles = (width + 7u) / 8u;
                    const u32 tile = (y / 4u) * tiles + (x / 8u);
                    const u32 offset = tile * 32u + (y % 4u) * 8u + (x % 8u);
                    palette_index = tex_read8(unit, source, offset);
                } else {
                    const u32 tiles = (width + 3u) / 4u;
                    const u32 tile = (y / 4u) * tiles + (x / 4u);
                    const u32 offset = tile * 32u + ((y % 4u) * 4u + (x % 4u)) * 2u;
                    palette_index = tex_read16(unit, source, offset) & 0x3fffu;
                }
                return decode_tlut_entry(unit, palette_index);
            }
            case 14: {  // CMPR: 8x8 tiles of four DXT1 blocks
                const u32 tiles = (width + 7u) / 8u;
                const u32 tile = (y / 8u) * tiles + (x / 8u);
                const u32 sub = ((y % 8u) / 4u) * 2u + ((x % 8u) / 4u);
                const u32 base = tile * 32u + sub * 8u;
                const u16 c0 = tex_read16(unit, source, base);
                const u16 c1 = tex_read16(unit, source, base + 2u);
                const u32 bits = tex_read32(unit, source, base + 4u);
                const u32 shift = 30u - (((y % 4u) * 4u + (x % 4u)) * 2u);
                const u32 selector = (bits >> shift) & 3u;
                const u32 a = decode_rgb565(c0);
                const u32 b = decode_rgb565(c1);
                const auto lerp = [](u32 first, u32 second, int numerator, int denominator) {
                    const auto mix = [&](int shift_bits) {
                        const int p = static_cast<int>((first >> shift_bits) & 0xff);
                        const int q = static_cast<int>((second >> shift_bits) & 0xff);
                        return (p * (denominator - numerator) + q * numerator) / denominator;
                    };
                    return pack_colour(mix(16), mix(8), mix(0), 255);
                };
                if (selector == 0) return a;
                if (selector == 1) return b;
                if (c0 > c1) {
                    return selector == 2 ? lerp(a, b, 1, 3) : lerp(a, b, 2, 3);
                }
                return selector == 2 ? lerp(a, b, 1, 2) : 0x00000000u;
            }
            default:
                return 0xffffffffu;
        }
    }
};

}  // namespace legacy

// Whole-image oracle decode into the module's output layout (R,G,B,A bytes).
std::array<u8, 4> dolphin_cmpr_texel(const u8* data, u32 width, u32 x, u32 y);

void oracle_decode(u32 format, u32 width, u32 height, const u8* data, const u8* tmem,
                   std::size_t tmem_size, u32 tlut_format, u8* out) {
    if (format == 14) {
        // CMPR: the GameCube palette rule, not the legacy decoder's PC DXT1.
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const auto t = dolphin_cmpr_texel(data, width, x, y);
                std::memcpy(out + (static_cast<std::size_t>(y) * width + x) * 4u, t.data(), 4u);
            }
        }
        return;
    }
    legacy::Renderer renderer;
    renderer.tmem_ = {tmem, tmem_size};
    legacy::TextureUnit unit;
    unit.width = width;
    unit.height = height;
    unit.format = format;
    unit.tlut_format = tlut_format;
    unit.tlut_base = 0;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 argb = renderer.decode_texel(unit, x, y, data);
            u8* p = out + (static_cast<std::size_t>(y) * width + x) * 4u;
            p[0] = static_cast<u8>(argb >> 16);
            p[1] = static_cast<u8>(argb >> 8);
            p[2] = static_cast<u8>(argb);
            p[3] = static_cast<u8>(argb >> 24);
        }
    }
}

// Dolphin's CMPR rule (Source/Core/VideoCommon/TextureDecoder_Generic.cpp
// DecodeDXTBlock + TextureDecoder_Util.h DXTBlend), for the informational
// difference count only.
std::array<u8, 4> dolphin_cmpr_texel(const u8* data, u32 width, u32 x, u32 y) {
    const u32 tiles = (width + 7u) / 8u;
    const u32 tile = (y / 8u) * tiles + (x / 8u);
    const u32 sub = ((y % 8u) / 4u) * 2u + ((x % 8u) / 4u);
    const u8* block = data + tile * 32u + sub * 8u;
    const u32 c1 = load_be16(block);
    const u32 c2 = load_be16(block + 2);
    const u32 bits = load_be32(block + 4);
    const u32 selector = (bits >> (30u - (((y % 4u) * 4u + (x % 4u)) * 2u))) & 3u;
    const auto e5 = [](u32 v) { return (v << 3) | (v >> 2); };
    const auto e6 = [](u32 v) { return (v << 2) | (v >> 4); };
    const u32 r1 = e5((c1 >> 11) & 31u), g1 = e6((c1 >> 5) & 63u), b1 = e5(c1 & 31u);
    const u32 r2 = e5((c2 >> 11) & 31u), g2 = e6((c2 >> 5) & 63u), b2 = e5(c2 & 31u);
    const auto blend = [](u32 v1, u32 v2) { return (v1 * 3u + v2 * 5u) >> 3; };
    std::array<std::array<u32, 4>, 4> colours{};
    colours[0] = {r1, g1, b1, 255u};
    colours[1] = {r2, g2, b2, 255u};
    if (c1 > c2) {
        colours[2] = {blend(r2, r1), blend(g2, g1), blend(b2, b1), 255u};
        colours[3] = {blend(r1, r2), blend(g1, g2), blend(b1, b2), 255u};
    } else {
        colours[2] = {(r1 + r2) / 2u, (g1 + g2) / 2u, (b1 + b2) / 2u, 255u};
        colours[3] = {(r1 + r2) / 2u, (g1 + g2) / 2u, (b1 + b2) / 2u, 0u};
    }
    const auto& c = colours[selector];
    return {static_cast<u8>(c[0]), static_cast<u8>(c[1]), static_cast<u8>(c[2]),
            static_cast<u8>(c[3])};
}

// Deterministic xorshift64* so every run tests the same inputs.
struct Rng {
    u64 state;
    u64 next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ull;
    }
    u32 below(u32 bound) { return static_cast<u32>(next() % bound); }
    void fill(u8* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<u8>(next() >> 56);
    }
};

const char* format_name(u32 format) {
    switch (format) {
        case 0: return "I4";
        case 1: return "I8";
        case 2: return "IA4";
        case 3: return "IA8";
        case 4: return "RGB565";
        case 5: return "RGB5A3";
        case 6: return "RGBA8";
        case 8: return "C4";
        case 9: return "C8";
        case 10: return "C14X2";
        case 14: return "CMPR";
        default: return "?";
    }
}

const char* tlut_name(u32 tlut_format) {
    switch (tlut_format) {
        case 0: return "IA8";
        case 1: return "RGB565";
        case 2: return "RGB5A3";
        default: return "?";
    }
}

constexpr std::array<u32, 11> kAllFormats = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 14};

std::string first_mismatch(const std::vector<u8>& got, const std::vector<u8>& want, u32 width) {
    for (std::size_t i = 0; i < got.size(); i += 4) {
        if (std::memcmp(got.data() + i, want.data() + i, 4) != 0) {
            const std::size_t texel = i / 4;
            return format_text("texel (%u,%u) got %02X%02X%02X%02X want %02X%02X%02X%02X",
                               static_cast<unsigned>(texel % width),
                               static_cast<unsigned>(texel / width), got[i], got[i + 1],
                               got[i + 2], got[i + 3], want[i], want[i + 1], want[i + 2],
                               want[i + 3]);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// (a) hand-computed vectors
// ---------------------------------------------------------------------------

struct Expect {
    u32 x, y;
    u8 r, g, b, a;
};

bool check_texels(const std::string& name, const std::vector<u8>& out, u32 width,
                  std::initializer_list<Expect> expects, bool decoded) {
    if (!decoded) return report(name, false, "decoder returned false");
    for (const Expect& e : expects) {
        const u8* p = out.data() + (static_cast<std::size_t>(e.y) * width + e.x) * 4u;
        if (p[0] != e.r || p[1] != e.g || p[2] != e.b || p[3] != e.a) {
            return report(name, false,
                          format_text("texel (%u,%u) got %02X%02X%02X%02X want %02X%02X%02X%02X",
                                      e.x, e.y, p[0], p[1], p[2], p[3], e.r, e.g, e.b, e.a));
        }
    }
    return report(name, true, format_text("%zu texels checked", expects.size()));
}

void put16(std::vector<u8>& v, std::size_t offset, u16 value) {
    v[offset] = static_cast<u8>(value >> 8);
    v[offset + 1] = static_cast<u8>(value);
}

void test_sizes() {
    struct Row {
        u32 format, width, height;
        std::size_t bytes;
    };
    // Hand-computed from the guest allocator rule (RENDERER_RE/01 section 4.4):
    // depth 4 A8(w)*A8(h)/2, depth 8 A8(w)*A4(h), depth 16 A4(w)*A4(h)*2,
    // depth 32 A4(w)*A4(h)*4.
    const Row rows[] = {
        {0, 1, 1, 32},       {8, 9, 1, 64},        {14, 5, 3, 32},      {14, 512, 512, 131072},
        {1, 4, 4, 32},       {2, 9, 5, 128},       {9, 2, 2, 32},       {9, 64, 64, 4096},
        {3, 5, 5, 128},      {4, 4, 4, 32},        {5, 128, 128, 32768}, {10, 3, 7, 64},
        {6, 5, 5, 256},      {6, 640, 480, 1228800}, {8, 1024, 1024, 524288},
        {7, 4, 4, 0},        {11, 4, 4, 0},        {15, 4, 4, 0},       {4, 0, 4, 0},
        {4, 4, 0, 0},        {4, 1025, 4, 0},      {4, 4, 1025, 0},
    };
    bool ok = true;
    std::string detail;
    for (const Row& row : rows) {
        const std::size_t got = gekko::gx_texture_data_size(row.format, row.width, row.height);
        if (got != row.bytes) {
            ok = false;
            detail = format_text("fmt %u %ux%u got %zu want %zu", row.format, row.width,
                                 row.height, got, row.bytes);
            break;
        }
    }
    report("sizes: gx_texture_data_size table (22 rows)", ok, detail);
}

void test_vectors() {
    // I4 9x2: tile 0 bytes 0..31, tile 1 bytes 32..63; high nibble = even x.
    {
        std::vector<u8> d(64, 0), out(9 * 2 * 4, 0xEE);
        d[0] = 0x3C;
        d[4] = 0x9F;
        d[32] = 0x70;
        d[36] = 0x10;
        const bool ok = gekko::gx_decode_texture(0, 9, 2, d.data(), d.size(), nullptr, 0, 0,
                                                 out.data());
        check_texels("vector I4 9x2", out, 9,
                     {{0, 0, 0x33, 0x33, 0x33, 0x33}, {1, 0, 0xCC, 0xCC, 0xCC, 0xCC},
                      {2, 0, 0, 0, 0, 0},              {8, 0, 0x77, 0x77, 0x77, 0x77},
                      {0, 1, 0x99, 0x99, 0x99, 0x99}, {1, 1, 0xFF, 0xFF, 0xFF, 0xFF},
                      {8, 1, 0x11, 0x11, 0x11, 0x11}},
                     ok);
    }
    // I8 16x8: 8-wide x 4-tall tiles (not 4x4 blocks). (9,5) is tile 3 byte 1*8+1.
    {
        std::vector<u8> d(128, 0), out(16 * 8 * 4, 0xEE);
        d[105] = 0xAB;
        d[8] = 0x12;
        d[32] = 0x34;
        d[64] = 0x56;
        const bool ok = gekko::gx_decode_texture(1, 16, 8, d.data(), d.size(), nullptr, 0, 0,
                                                 out.data());
        check_texels("vector I8 16x8 (8x4 tiles)", out, 16,
                     {{9, 5, 0xAB, 0xAB, 0xAB, 0xAB}, {0, 1, 0x12, 0x12, 0x12, 0x12},
                      {8, 0, 0x34, 0x34, 0x34, 0x34}, {0, 4, 0x56, 0x56, 0x56, 0x56},
                      {4, 0, 0, 0, 0, 0}},
                     ok);
    }
    // IA4 8x4: high nibble alpha, low nibble intensity.
    {
        std::vector<u8> d(32, 0), out(8 * 4 * 4, 0xEE);
        d[0] = 0x5A;
        d[9] = 0xF0;
        const bool ok = gekko::gx_decode_texture(2, 8, 4, d.data(), d.size(), nullptr, 0, 0,
                                                 out.data());
        check_texels("vector IA4 8x4", out, 8,
                     {{0, 0, 0xAA, 0xAA, 0xAA, 0x55}, {1, 1, 0x00, 0x00, 0x00, 0xFF}}, ok);
    }
    // IA8 4x4: first byte alpha, second intensity.
    {
        std::vector<u8> d(32, 0), out(4 * 4 * 4, 0xEE);
        d[2] = 0x80;
        d[3] = 0x40;
        d[30] = 0xFF;
        d[31] = 0x00;
        const bool ok = gekko::gx_decode_texture(3, 4, 4, d.data(), d.size(), nullptr, 0, 0,
                                                 out.data());
        check_texels("vector IA8 4x4", out, 4,
                     {{1, 0, 0x40, 0x40, 0x40, 0x80}, {3, 3, 0x00, 0x00, 0x00, 0xFF},
                      {0, 0, 0, 0, 0, 0}},
                     ok);
    }
    // RGB565 5x5: 0x8410 -> r 16 -> 0x84, g 32 -> 0x82 (6-bit replication), b 0x84.
    {
        std::vector<u8> d(128, 0), out(5 * 5 * 4, 0xEE);
        put16(d, 0, 0xF800);
        put16(d, 2, 0x07E0);
        put16(d, 4, 0x001F);
        put16(d, 6, 0x8410);
        put16(d, 96, 0xFFFF);
        const bool ok = gekko::gx_decode_texture(4, 5, 5, d.data(), d.size(), nullptr, 0, 0,
                                                 out.data());
        check_texels("vector RGB565 5x5", out, 5,
                     {{0, 0, 0xFF, 0x00, 0x00, 0xFF}, {1, 0, 0x00, 0xFF, 0x00, 0xFF},
                      {2, 0, 0x00, 0x00, 0xFF, 0xFF}, {3, 0, 0x84, 0x82, 0x84, 0xFF},
                      {4, 4, 0xFF, 0xFF, 0xFF, 0xFF}, {4, 0, 0x00, 0x00, 0x00, 0xFF}},
                     ok);
    }
    // RGB5A3 4x1: opaque 555, A3 444 (a 3 -> 0x6D), opaque 0x8421 -> 08, zero.
    {
        std::vector<u8> d(32, 0), out(4 * 1 * 4, 0xEE);
        put16(d, 0, 0xFFFF);
        put16(d, 2, 0x3A5C);
        put16(d, 4, 0x8421);
        put16(d, 6, 0x0000);
        const bool ok = gekko::gx_decode_texture(5, 4, 1, d.data(), d.size(), nullptr, 0, 0,
                                                 out.data());
        check_texels("vector RGB5A3 4x1", out, 4,
                     {{0, 0, 0xFF, 0xFF, 0xFF, 0xFF}, {1, 0, 0xAA, 0x55, 0xCC, 0x6D},
                      {2, 0, 0x08, 0x08, 0x08, 0xFF}, {3, 0, 0x00, 0x00, 0x00, 0x00}},
                     ok);
    }
    // RGBA8 5x5: tile = 16 (A,R) pairs then 16 (G,B) pairs.
    {
        std::vector<u8> d(256, 0), out(5 * 5 * 4, 0xEE);
        d[10] = 0x11;  // (1,1) A
        d[11] = 0x22;  // R
        d[42] = 0x33;  // G
        d[43] = 0x44;  // B
        d[192] = 0xA0;  // (4,4): tile 3, index 0
        d[193] = 0xB0;
        d[224] = 0xC0;
        d[225] = 0xD0;
        const bool ok = gekko::gx_decode_texture(6, 5, 5, d.data(), d.size(), nullptr, 0, 0,
                                                 out.data());
        check_texels("vector RGBA8 5x5 (AR/GB halves)", out, 5,
                     {{1, 1, 0x22, 0x33, 0x44, 0x11}, {4, 4, 0xB0, 0xC0, 0xD0, 0xA0}}, ok);
    }
    // C4 3x9 + RGB5A3 TLUT: entry 1 = 0x3A5C, entry 14 = 0xFC00.
    {
        std::vector<u8> d(64, 0), out(3 * 9 * 4, 0xEE), tlut(32, 0);
        put16(tlut, 2, 0x3A5C);
        put16(tlut, 28, 0xFC00);
        d[0] = 0x1E;
        d[33] = 0x10;  // (2,8): tile 1, byte 0*4 + 2/2
        const bool ok = gekko::gx_decode_texture(8, 3, 9, d.data(), d.size(), tlut.data(), 2, 16,
                                                 out.data());
        check_texels("vector C4 3x9 + RGB5A3", out, 3,
                     {{0, 0, 0xAA, 0x55, 0xCC, 0x6D}, {1, 0, 0xFF, 0x00, 0x00, 0xFF},
                      {2, 8, 0xAA, 0x55, 0xCC, 0x6D}, {2, 0, 0x00, 0x00, 0x00, 0x00}},
                     ok);
    }
    // C4 short palette: index 15 >= 2 entries decodes entry 0x0000 per TLUT format.
    {
        std::vector<u8> d(32, 0), out(2 * 1 * 4, 0xEE), tlut(4, 0);
        put16(tlut, 0, 0x001F);
        put16(tlut, 2, 0xF800);
        d[0] = 0x0F;
        bool ok = gekko::gx_decode_texture(8, 2, 1, d.data(), d.size(), tlut.data(), 1, 2,
                                           out.data());
        check_texels("vector C4 out-of-range index, RGB565 TLUT", out, 2,
                     {{0, 0, 0x00, 0x00, 0xFF, 0xFF}, {1, 0, 0x00, 0x00, 0x00, 0xFF}}, ok);
        ok = gekko::gx_decode_texture(8, 2, 1, d.data(), d.size(), tlut.data(), 0, 2, out.data());
        check_texels("vector C4 out-of-range index, IA8 TLUT", out, 2,
                     {{0, 0, 0x1F, 0x1F, 0x1F, 0x00}, {1, 0, 0x00, 0x00, 0x00, 0x00}}, ok);
        ok = gekko::gx_decode_texture(8, 2, 1, d.data(), d.size(), tlut.data(), 2, 2, out.data());
        check_texels("vector C4 out-of-range index, RGB5A3 TLUT", out, 2,
                     {{0, 0, 0x00, 0x11, 0xFF, 0x00}, {1, 0, 0x00, 0x00, 0x00, 0x00}}, ok);
    }
    // C8 2x2 + IA8 TLUT: entry 0xC3 = bytes C0 30 -> alpha C0, intensity 30.
    {
        std::vector<u8> d(32, 0), out(2 * 2 * 4, 0xEE), tlut(512, 0);
        tlut[0xC3 * 2] = 0xC0;
        tlut[0xC3 * 2 + 1] = 0x30;
        d[1] = 0xC3;
        d[8] = 0xC3;
        const bool ok = gekko::gx_decode_texture(9, 2, 2, d.data(), d.size(), tlut.data(), 0, 256,
                                                 out.data());
        check_texels("vector C8 2x2 + IA8", out, 2,
                     {{1, 0, 0x30, 0x30, 0x30, 0xC0}, {0, 1, 0x30, 0x30, 0x30, 0xC0},
                      {0, 0, 0, 0, 0, 0}},
                     ok);
    }
    // C14X2 4x4 + RGB565: top two bits ignored; index 9 >= 8 entries -> (0,0,0,FF).
    {
        std::vector<u8> d(32, 0), out(4 * 4 * 4, 0xEE), tlut(16, 0);
        put16(tlut, 10, 0x07E0);
        put16(d, 0, 0xC005);
        put16(d, 2, 0x0009);
        put16(d, 4, 0x4005);
        const bool ok = gekko::gx_decode_texture(10, 4, 4, d.data(), d.size(), tlut.data(), 1, 8,
                                                 out.data());
        check_texels("vector C14X2 4x4 + RGB565", out, 4,
                     {{0, 0, 0x00, 0xFF, 0x00, 0xFF}, {1, 0, 0x00, 0x00, 0x00, 0xFF},
                      {2, 0, 0x00, 0xFF, 0x00, 0xFF}},
                     ok);
    }
    // CMPR 8x8, one tile: TL c0>c1 5/8-3/8 blends; TR c0<c1 half + half with
    // alpha 0; BL c0==c1 (the alpha branch); BR c0=FFFF c1=0 selector 2
    // everywhere. GameCube rule: (5*a+3*b)>>3, not PC DXT1's exact thirds.
    std::vector<u8> cmpr(32, 0);
    put16(cmpr, 0, 0xF800);
    put16(cmpr, 2, 0x001F);
    cmpr[4] = 0x1B;  // row 0 selectors 0,1,2,3
    put16(cmpr, 8, 0x001F);
    put16(cmpr, 10, 0xF800);
    cmpr[12] = 0x1B;
    put16(cmpr, 16, 0x07E0);
    put16(cmpr, 18, 0x07E0);
    cmpr[23] = 0xFF;  // row 3 selectors 3,3,3,3
    put16(cmpr, 24, 0xFFFF);
    put16(cmpr, 26, 0x0000);
    std::memset(cmpr.data() + 28, 0xAA, 4);
    {
        std::vector<u8> out(8 * 8 * 4, 0xEE);
        const bool ok = gekko::gx_decode_texture(14, 8, 8, cmpr.data(), cmpr.size(), nullptr, 0,
                                                 0, out.data());
        check_texels("vector CMPR 8x8 (blend, transparent, sub-block order)", out, 8,
                     {{0, 0, 0xFF, 0x00, 0x00, 0xFF}, {1, 0, 0x00, 0x00, 0xFF, 0xFF},
                      {2, 0, 0x9F, 0x00, 0x5F, 0xFF}, {3, 0, 0x5F, 0x00, 0x9F, 0xFF},
                      {0, 3, 0xFF, 0x00, 0x00, 0xFF}, {4, 0, 0x00, 0x00, 0xFF, 0xFF},
                      {5, 0, 0xFF, 0x00, 0x00, 0xFF}, {6, 0, 0x7F, 0x00, 0x7F, 0xFF},
                      {7, 0, 0x7F, 0x00, 0x7F, 0x00}, {0, 4, 0x00, 0xFF, 0x00, 0xFF},
                      {0, 7, 0x00, 0xFF, 0x00, 0x00}, {3, 7, 0x00, 0xFF, 0x00, 0x00},
                      {4, 4, 0x9F, 0x9F, 0x9F, 0xFF}, {7, 7, 0x9F, 0x9F, 0x9F, 0xFF}},
                     ok);
    }
    // CMPR 5x3: the same tile clipped; only TL (4x3) and TR's first column exist.
    {
        std::vector<u8> out(5 * 3 * 4, 0xEE);
        const bool ok = gekko::gx_decode_texture(14, 5, 3, cmpr.data(), cmpr.size(), nullptr, 0,
                                                 0, out.data());
        check_texels("vector CMPR 5x3 clipped tile", out, 5,
                     {{3, 0, 0x5F, 0x00, 0x9F, 0xFF}, {4, 0, 0x00, 0x00, 0xFF, 0xFF},
                      {3, 2, 0xFF, 0x00, 0x00, 0xFF}, {4, 2, 0x00, 0x00, 0xFF, 0xFF}},
                     ok);
    }
}

void test_invalid_inputs() {
    std::vector<u8> data(4096, 0x5A), tlut(512, 0x11), out(64 * 4, 0xEE);
    const std::vector<u8> pristine = out;
    struct Case {
        const char* name;
        u32 format, width, height;
        std::size_t data_size;
        bool null_data, null_tlut, null_out;
        u32 tlut_format, tlut_entries;
    };
    const Case cases[] = {
        {"format 7", 7, 4, 4, 4096, false, false, false, 0, 16},
        {"format 11", 11, 4, 4, 4096, false, false, false, 0, 16},
        {"format 15", 15, 4, 4, 4096, false, false, false, 0, 16},
        {"width 0", 4, 0, 4, 4096, false, false, false, 0, 16},
        {"height 0", 4, 4, 0, 4096, false, false, false, 0, 16},
        {"width 1025", 4, 1025, 1, 1u << 20, false, false, false, 0, 16},
        {"data one byte short", 4, 8, 8, 63, false, false, false, 0, 16},
        {"CMPR data one byte short", 14, 8, 8, 31, false, false, false, 0, 16},
        {"null data", 4, 4, 4, 4096, true, false, false, 0, 16},
        {"null output", 4, 4, 4, 4096, false, false, true, 0, 16},
        {"C4 null tlut", 8, 4, 4, 4096, false, true, false, 1, 16},
        {"C8 zero tlut entries", 9, 4, 4, 4096, false, false, false, 1, 0},
        {"C14X2 tlut format 3", 10, 4, 4, 4096, false, false, false, 3, 16},
    };
    for (const Case& c : cases) {
        out = pristine;
        const bool result = gekko::gx_decode_texture(
            c.format, c.width, c.height, c.null_data ? nullptr : data.data(), c.data_size,
            c.null_tlut ? nullptr : tlut.data(), c.tlut_format, c.tlut_entries,
            c.null_out ? nullptr : out.data());
        report(std::string("invalid input rejected, output untouched: ") + c.name,
               !result && out == pristine);
    }
    // Non-palettised formats ignore the TLUT arguments entirely.
    out = pristine;
    const bool ignored =
        gekko::gx_decode_texture(4, 4, 4, data.data(), 32, nullptr, 99, 0, out.data());
    report("non-palettised format ignores null tlut / bad tlut format", ignored);
}

// ---------------------------------------------------------------------------
// (b) randomized differential test against the transcription
// ---------------------------------------------------------------------------

void test_differential() {
    Rng rng{0x9E3779B97F4A7C15ull};
    constexpr u32 kSizes[] = {1, 2, 3, 4, 5, 7, 8, 9, 12, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100, 128};
    constexpr u32 kEntries[] = {1, 3, 15, 16, 17, 200, 255, 256, 1000, 4096, 16384};
    for (u32 format : kAllFormats) {
        const bool palettised = gekko::gx_texture_format_is_palettised(format);
        u64 texels = 0;
        u32 images = 0;
        std::string failure;
        for (u32 trial = 0; trial < 160 && failure.empty(); ++trial) {
            const u32 width = trial < 20 ? kSizes[trial] : kSizes[rng.below(20)];
            const u32 height = trial < 20 ? kSizes[19 - trial] : kSizes[rng.below(20)];
            const std::size_t size = gekko::gx_texture_data_size(format, width, height);
            std::vector<u8> data(size);
            rng.fill(data.data(), size);
            u32 tlut_format = 0;
            u32 entries = 0;
            std::vector<u8> tlut;
            if (palettised) {
                tlut_format = trial % 3u;
                entries = kEntries[rng.below(static_cast<u32>(std::size(kEntries)))];
                tlut.resize(static_cast<std::size_t>(entries) * 2u);
                rng.fill(tlut.data(), tlut.size());
            }
            std::vector<u8> got(static_cast<std::size_t>(width) * height * 4u, 0xEE);
            std::vector<u8> want(got.size(), 0x11);
            const bool ok = gekko::gx_decode_texture(format, width, height, data.data(), size,
                                                     palettised ? tlut.data() : nullptr,
                                                     tlut_format, entries, got.data());
            oracle_decode(format, width, height, data.data(), tlut.data(), tlut.size(),
                          tlut_format, want.data());
            if (!ok) {
                failure = format_text("%ux%u decoder returned false", width, height);
            } else if (got != want) {
                failure = format_text("%ux%u tlut %s/%u: ", width, height, tlut_name(tlut_format),
                                      entries) +
                          first_mismatch(got, want, width);
            }
            texels += static_cast<u64>(width) * height;
            ++images;
        }
        report(format_text("differential vs legacy transcription: %s", format_name(format)),
               failure.empty(),
               failure.empty() ? format_text("%u images, %llu texels identical", images,
                                             static_cast<unsigned long long>(texels))
                               : failure);
    }
}

// ---------------------------------------------------------------------------
// Guard pages: the image and TLUT end exactly at a PAGE_NOACCESS page, so any
// read past either extent faults. SEH turns the fault into a FAIL.
// ---------------------------------------------------------------------------

struct GuardedBuffer {
    u8* base = nullptr;
    std::size_t usable = 0;
    explicit GuardedBuffer(std::size_t bytes) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        const std::size_t page = info.dwPageSize;
        usable = ((bytes + page - 1) / page) * page;
        base = static_cast<u8*>(
            VirtualAlloc(nullptr, usable + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old = 0;
        if (base != nullptr) VirtualProtect(base + usable, page, PAGE_NOACCESS, &old);
    }
    ~GuardedBuffer() {
        if (base != nullptr) VirtualFree(base, 0, MEM_RELEASE);
    }
    GuardedBuffer(const GuardedBuffer&) = delete;
    GuardedBuffer& operator=(const GuardedBuffer&) = delete;
    u8* tail(std::size_t bytes) const { return base + usable - bytes; }
};

// No C++ objects with destructors in this frame: __try requires it.
int guarded_decode(u32 format, u32 width, u32 height, const u8* data, std::size_t size,
                   const u8* tlut, u32 tlut_format, u32 entries, u8* out) {
    __try {
        return gekko::gx_decode_texture(format, width, height, data, size, tlut, tlut_format,
                                        entries, out)
                   ? 1
                   : 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return -1;
    }
}

void test_guard_pages() {
    Rng rng{0xC0FFEEull};
    constexpr u32 kSizes[] = {1, 3, 5, 8, 9, 17, 33, 64, 127, 128};
    GuardedBuffer image(128u * 128u * 4u);  // largest: RGBA8 128x128 = 64,000 bytes rounded
    GuardedBuffer palette(2u * 16384u);
    bool ok = image.base != nullptr && palette.base != nullptr;
    std::string detail = ok ? std::string() : "VirtualAlloc failed";
    u32 decodes = 0;
    for (u32 format : kAllFormats) {
        for (u32 wi = 0; ok && wi < std::size(kSizes); ++wi) {
            for (u32 hi = 0; ok && hi < std::size(kSizes); ++hi) {
                const u32 width = kSizes[wi];
                const u32 height = kSizes[(hi + wi) % std::size(kSizes)];
                const std::size_t size = gekko::gx_texture_data_size(format, width, height);
                const u8* data = image.tail(size);
                rng.fill(image.tail(size), size);
                // Small palettes against 16-bit C14X2 indices: every out-of-range
                // index must be bounds-checked, never dereferenced.
                const u32 entries = 1u + rng.below(40);
                rng.fill(palette.tail(entries * 2u), entries * 2u);
                std::vector<u8> out(static_cast<std::size_t>(width) * height * 4u);
                const int result = guarded_decode(format, width, height, data, size,
                                                  palette.tail(entries * 2u), rng.below(3),
                                                  entries, out.data());
                ++decodes;
                if (result != 1) {
                    ok = false;
                    detail = format_text("%s %ux%u %s", format_name(format), width, height,
                                         result < 0 ? "READ PAST THE EXTENT (access violation)"
                                                    : "returned false");
                }
            }
        }
    }
    report("guard pages: no read past image or TLUT extent", ok,
           ok ? format_text("%u decodes, image and palette flush against PAGE_NOACCESS", decodes)
              : detail);
}

// ---------------------------------------------------------------------------
// PNG (stored deflate) for visual inspection.
// ---------------------------------------------------------------------------

u32 crc32_update(u32 crc, const u8* data, std::size_t n) {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) != 0u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

bool write_png(const std::string& path, u32 width, u32 height, const u8* rgba) {
    std::vector<u8> raw;
    raw.reserve((static_cast<std::size_t>(width) * 4u + 1u) * height);
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);
        const u8* row = rgba + static_cast<std::size_t>(y) * width * 4u;
        raw.insert(raw.end(), row, row + static_cast<std::size_t>(width) * 4u);
    }
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (u8 v : raw) {
        a = (a + v) % 65521u;
        b = (b + a) % 65521u;
    }
    for (std::size_t offset = 0; offset < raw.size() || offset == 0;) {
        const std::size_t n = std::min<std::size_t>(65535u, raw.size() - offset);
        const bool last = offset + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<u8>(n));
        z.push_back(static_cast<u8>(n >> 8));
        z.push_back(static_cast<u8>(~n));
        z.push_back(static_cast<u8>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                 raw.begin() + static_cast<std::ptrdiff_t>(offset + n));
        offset += n;
        if (last) break;
    }
    const u32 adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(static_cast<u8>(adler >> s));

    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    const auto be32 = [&](u32 v) {
        const u8 bytes[4] = {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16),
                             static_cast<u8>(v >> 8), static_cast<u8>(v)};
        file.write(reinterpret_cast<const char*>(bytes), 4);
    };
    const auto chunk = [&](const char* type, const u8* payload, std::size_t n) {
        be32(static_cast<u32>(n));
        std::vector<u8> body(type, type + 4);
        body.insert(body.end(), payload, payload + n);
        file.write(reinterpret_cast<const char*>(body.data()),
                   static_cast<std::streamsize>(body.size()));
        be32(crc32_update(0, body.data(), body.size()));
    };
    static const u8 signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    file.write(reinterpret_cast<const char*>(signature), 8);
    const u8 ihdr[13] = {static_cast<u8>(width >> 24), static_cast<u8>(width >> 16),
                         static_cast<u8>(width >> 8),  static_cast<u8>(width),
                         static_cast<u8>(height >> 24), static_cast<u8>(height >> 16),
                         static_cast<u8>(height >> 8), static_cast<u8>(height),
                         8, 6, 0, 0, 0};
    chunk("IHDR", ihdr, sizeof(ihdr));
    chunk("IDAT", z.data(), z.size());
    chunk("IEND", nullptr, 0);
    return static_cast<bool>(file);
}

// ---------------------------------------------------------------------------
// (c) ORACLE over the real rasters of an in-stage MEM1 image.
// ---------------------------------------------------------------------------

struct Raster {
    u32 address, width, height, depth, format, tlut_format, pixels, palette;
    u8 type;
};

void test_mem1_oracle(const std::string& mem1_path, const std::string& png_dir) {
    std::ifstream file(mem1_path, std::ios::binary);
    std::vector<u8> mem((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!report("mem1: dump loaded", mem.size() == 0x1800000u,
                format_text("%s, %zu bytes", mem1_path.c_str(), mem.size()))) {
        return;
    }
    const auto u32_at = [&](std::size_t o) { return load_be32(mem.data() + o); };
    const auto in_mem1 = [&](u32 address, std::size_t bytes) {
        if (address < 0x80000000u) return false;
        const std::size_t offset = address - 0x80000000u;
        return offset <= mem.size() && bytes <= mem.size() - offset;
    };

    // RwRaster (RW 3.7 base) + RwGC extension, docs/native-runtime-2026-09-16/
    // understand/01 section 2.3: +0x00 parent (== self for a top-level raster),
    // +0x0C width, +0x10 height, +0x14 depth, +0x20 cType, +0x40 GXTexFmt,
    // +0x44 GXTlutFmt, +0x50 pixels, +0x54 palette (1 << depth entries).
    std::vector<Raster> rasters;
    for (std::size_t o = 0; o + 0x68 <= mem.size(); o += 4) {
        if (u32_at(o) != 0x80000000u + o) continue;
        const u32 width = u32_at(o + 0x0C);
        const u32 height = u32_at(o + 0x10);
        if (width == 0 || height == 0 || width > 1024 || height > 1024) continue;
        const u8 type = mem[o + 0x20];
        if (type != 0 && type != 4 && type != 5) continue;
        const u32 format = u32_at(o + 0x40);
        const std::size_t size = gekko::gx_texture_data_size(format, width, height);
        if (size == 0) continue;
        Raster r{static_cast<u32>(0x80000000u + o), width, height, u32_at(o + 0x14), format,
                 u32_at(o + 0x44), u32_at(o + 0x50), u32_at(o + 0x54), type};
        if (!in_mem1(r.pixels, size)) continue;
        if (gekko::gx_texture_format_is_palettised(format)) {
            if (r.tlut_format > 2 || (r.depth != 4 && r.depth != 8) ||
                !in_mem1(r.palette, (std::size_t{1} << r.depth) * 2u)) {
                continue;
            }
        }
        rasters.push_back(r);
    }
    report("mem1: RwRaster scan", !rasters.empty(),
           format_text("%zu top-level rasters with a valid GX format and in-range pixels",
                       rasters.size()));

    struct Category {
        const char* name;
        u32 rasters = 0;
        u64 texels = 0;
        u32 mismatched = 0;
        const Raster* largest = nullptr;
    };
    // Index: 0 C4+RGB5A3, 1 C4+RGB565, 2 C4+IA8, 3 C8+RGB5A3, 4 C8+RGB565, 5 C8+IA8,
    // 6 CMPR with transparent texels, 7 CMPR without, 8 RGB565, 9 RGB5A3, 10 RGBA8, 11 other
    std::array<Category, 12> categories = {{{"C4+RGB5A3"}, {"C4+RGB565"}, {"C4+IA8"},
                                            {"C8+RGB5A3"}, {"C8+RGB565"}, {"C8+IA8"},
                                            {"CMPR with transparent texels"},
                                            {"CMPR without transparent texels"}, {"RGB565"},
                                            {"RGB5A3"}, {"RGBA8"}, {"other"}}};
    u64 cmpr_texels = 0, cmpr_dolphin_diff = 0, cmpr_dolphin_transparent_diff = 0;
    u32 cmpr_dolphin_max_delta = 0, cmpr_dolphin_transparent_max = 0;
    std::string first_failure;
    std::vector<u8> got, want;
    for (const Raster& r : rasters) {
        const std::size_t size = gekko::gx_texture_data_size(r.format, r.width, r.height);
        const u8* data = mem.data() + (r.pixels - 0x80000000u);
        const bool palettised = gekko::gx_texture_format_is_palettised(r.format);
        const u8* tlut = palettised ? mem.data() + (r.palette - 0x80000000u) : nullptr;
        const u32 entries = palettised ? (1u << r.depth) : 0u;
        got.assign(static_cast<std::size_t>(r.width) * r.height * 4u, 0xEE);
        want.assign(got.size(), 0x11);
        const bool ok = gekko::gx_decode_texture(r.format, r.width, r.height, data, size, tlut,
                                                 r.tlut_format, entries, got.data());
        // The legacy decoder indexes all of texture memory from the TLUT base;
        // hand it everything from the palette to the end of MEM1.
        const std::size_t tmem_size =
            palettised ? mem.size() - (r.palette - 0x80000000u) : 0u;
        oracle_decode(r.format, r.width, r.height, data, tlut, tmem_size, r.tlut_format,
                      want.data());

        std::size_t index = 11;
        if (r.format == 8 || r.format == 9) {
            index = (r.format == 8 ? 0u : 3u) + (r.tlut_format == 2 ? 0u : r.tlut_format == 1 ? 1u : 2u);
        } else if (r.format == 14) {
            bool transparent = false;
            for (std::size_t i = 3; i < want.size(); i += 4) transparent |= want[i] == 0;
            index = transparent ? 6u : 7u;
            for (u32 y = 0; y < r.height; ++y) {
                for (u32 x = 0; x < r.width; ++x) {
                    const auto d = dolphin_cmpr_texel(data, r.width, x, y);
                    const u8* p = got.data() + (static_cast<std::size_t>(y) * r.width + x) * 4u;
                    u32 delta = 0;
                    for (int c = 0; c < 4; ++c) {
                        delta = std::max(delta, static_cast<u32>(std::abs(int{p[c]} - int{d[c]})));
                    }
                    if (delta == 0) continue;
                    // Alpha 0 on both sides: the c3 entry of a c0 <= c1 block, where
                    // only the (invisible unless filtered) RGB differs.
                    if (p[3] == 0) {
                        ++cmpr_dolphin_transparent_diff;
                        cmpr_dolphin_transparent_max = std::max(cmpr_dolphin_transparent_max, delta);
                    } else {
                        ++cmpr_dolphin_diff;
                        cmpr_dolphin_max_delta = std::max(cmpr_dolphin_max_delta, delta);
                    }
                }
            }
            cmpr_texels += static_cast<u64>(r.width) * r.height;
        } else if (r.format == 4) {
            index = 8;
        } else if (r.format == 5) {
            index = 9;
        } else if (r.format == 6) {
            index = 10;
        }
        Category& category = categories[index];
        ++category.rasters;
        category.texels += static_cast<u64>(r.width) * r.height;
        if (category.largest == nullptr ||
            r.width * r.height > category.largest->width * category.largest->height) {
            category.largest = &r;
        }
        if (!ok || got != want) {
            ++category.mismatched;
            if (first_failure.empty()) {
                first_failure = format_text("raster 0x%08X %s %ux%u: ", r.address,
                                            format_name(r.format), r.width, r.height) +
                                (ok ? first_mismatch(got, want, r.width) : "decoder returned false");
            }
        }
    }

    for (std::size_t i = 0; i < categories.size(); ++i) {
        const Category& c = categories[i];
        // Required by the task: C4+RGB5A3, C4+RGB565, C8, CMPR with and without
        // transparent blocks, RGB565. IA8 TLUTs and "other" are optional (the
        // game data has none); everything present must match.
        const bool required = i == 0 || i == 1 || i == 6 || i == 7 || i == 8;
        if (c.rasters == 0) {
            if (required) report(format_text("mem1 oracle: %s", c.name), false, "none in the dump");
            continue;
        }
        report(format_text("mem1 oracle: %s", c.name), c.mismatched == 0,
               format_text("%u rasters, %llu texels, %u mismatched%s", c.rasters,
                           static_cast<unsigned long long>(c.texels), c.mismatched,
                           c.mismatched != 0 ? (" - " + first_failure).c_str() : ""));
    }
    const u32 c8_rasters = categories[3].rasters + categories[4].rasters + categories[5].rasters;
    report("mem1 oracle: C8 present (any TLUT)", c8_rasters > 0,
           format_text("%u C8 rasters", c8_rasters));
    std::printf("INFO CMPR vs Dolphin's DecodeDXTBlock rule on real data (%llu texels): "
                "%llu opaque texels differ (%.2f%%, c2/c3 thirds vs 3/8 blend, max channel "
                "delta %u); %llu transparent texels differ in RGB only (max delta %u)\n",
                static_cast<unsigned long long>(cmpr_texels),
                static_cast<unsigned long long>(cmpr_dolphin_diff),
                cmpr_texels ? 100.0 * static_cast<double>(cmpr_dolphin_diff) /
                                  static_cast<double>(cmpr_texels)
                            : 0.0,
                cmpr_dolphin_max_delta,
                static_cast<unsigned long long>(cmpr_dolphin_transparent_diff),
                cmpr_dolphin_transparent_max);

    // PNGs of the largest raster per category, straight RGBA and alpha-forced.
    u32 written = 0;
    for (const Category& c : categories) {
        if (c.largest == nullptr) continue;
        const Raster& r = *c.largest;
        std::vector<u8> out(static_cast<std::size_t>(r.width) * r.height * 4u);
        const bool palettised = gekko::gx_texture_format_is_palettised(r.format);
        gekko::gx_decode_texture(
            r.format, r.width, r.height, mem.data() + (r.pixels - 0x80000000u),
            gekko::gx_texture_data_size(r.format, r.width, r.height),
            palettised ? mem.data() + (r.palette - 0x80000000u) : nullptr, r.tlut_format,
            palettised ? (1u << r.depth) : 0u, out.data());
        std::string stem = format_text("%s/mem1_%08X_%s%s%s_%ux%u", png_dir.c_str(), r.address,
                                       format_name(r.format), palettised ? "_" : "",
                                       palettised ? tlut_name(r.tlut_format) : "", r.width,
                                       r.height);
        written += write_png(stem + "_rgba.png", r.width, r.height, out.data()) ? 1u : 0u;
        for (std::size_t i = 3; i < out.size(); i += 4) out[i] = 255;
        written += write_png(stem + "_opaque.png", r.width, r.height, out.data()) ? 1u : 0u;
    }
    report("mem1: PNGs written for visual inspection", written > 0,
           format_text("%u files in %s", written, png_dir.c_str()));
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

struct Timing {
    double min_ms, median_ms;
};

template <typename Fn>
Timing time_runs(int runs, Fn fn) {
    for (int i = 0; i < 3; ++i) fn();
    std::vector<double> samples;
    for (int i = 0; i < runs; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return {samples.front(), samples[samples.size() / 2]};
}

void test_timing() {
    Rng rng{0x5EEDull};
    constexpr u32 kSide = 512;
    std::vector<u8> out(static_cast<std::size_t>(kSide) * kSide * 4u);
    std::vector<u8> cmpr(gekko::gx_texture_data_size(14, kSide, kSide));
    rng.fill(cmpr.data(), cmpr.size());  // ~half the blocks take each colour branch
    const Timing t_cmpr = time_runs(64, [&] {
        gekko::gx_decode_texture(14, kSide, kSide, cmpr.data(), cmpr.size(), nullptr, 0, 0,
                                 out.data());
    });
    report("timing: 512x512 CMPR single-threaded < 2 ms (min of 64)", t_cmpr.min_ms < 2.0,
           format_text("min %.3f ms, median %.3f ms", t_cmpr.min_ms, t_cmpr.median_ms));

    std::vector<u8> c4(gekko::gx_texture_data_size(8, kSide, kSide)), tlut(32);
    rng.fill(c4.data(), c4.size());
    rng.fill(tlut.data(), tlut.size());
    const Timing t_c4 = time_runs(64, [&] {
        gekko::gx_decode_texture(8, kSide, kSide, c4.data(), c4.size(), tlut.data(), 2, 16,
                                 out.data());
    });
    std::vector<u8> rgba8(gekko::gx_texture_data_size(6, kSide, kSide));
    rng.fill(rgba8.data(), rgba8.size());
    const Timing t_rgba8 = time_runs(64, [&] {
        gekko::gx_decode_texture(6, kSide, kSide, rgba8.data(), rgba8.size(), nullptr, 0, 0,
                                 out.data());
    });
    const Timing t_legacy = time_runs(8, [&] {
        oracle_decode(14, kSide, kSide, cmpr.data(), nullptr, 0, 0, out.data());
    });
    std::printf("INFO timing 512x512: C4+RGB5A3 min %.3f / median %.3f ms; RGBA8 min %.3f / "
                "median %.3f ms; legacy per-texel CMPR transcription min %.3f / median %.3f ms\n",
                t_c4.min_ms, t_c4.median_ms, t_rgba8.min_ms, t_rgba8.median_ms, t_legacy.min_ms,
                t_legacy.median_ms);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mem1 = argc > 1 ? argv[1] : kDefaultMem1;
    const std::string png_dir = argc > 2 ? argv[2] : kDefaultPngDir;
    test_sizes();
    test_vectors();
    test_invalid_inputs();
    test_differential();
    test_guard_pages();
    test_mem1_oracle(mem1, png_dir);
    test_timing();
    std::printf("%s: %d cases, %d failed\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_cases,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
