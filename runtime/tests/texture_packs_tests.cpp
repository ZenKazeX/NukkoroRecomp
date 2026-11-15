// Texture packs (gekko/texture_packs.h): XXH64 against the published reference values, the Dolphin name rule
// (base-level hash, used-palette-range hash), the name parser, the DDS / PNG decoders and the index + lookup.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gekko/texture_packs.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(disable : 4505)  // unused static stb functions, reported at the end of the unit
#pragma warning(push)
#pragma warning(disable : 4996 4100 4244 4456 4457)
#endif
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace tp = gekko::texture_packs;

static int g_failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

static std::vector<std::uint8_t> pattern(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>((i * 7 + 3) & 255);
    return v;
}

static void test_xxh64() {
    // Reference values from the xxHash reference implementation (Python xxhash 3.6.0, seed 0 unless stated).
    CHECK(tp::xxh64("", 0) == 0xef46db3751d8e999ull);
    CHECK(tp::xxh64("a", 1) == 0xd24ec4f1a98c6e5bull);
    CHECK(tp::xxh64("abc", 3) == 0x44bc2cf5ad770999ull);
    const std::vector<std::uint8_t> p = pattern(1000);
    const struct { std::size_t n; std::uint64_t h; } cases[] = {
        {3, 0x31d2363f52e564c9ull},  {4, 0x9bb64b7d66ee9fdaull},  {7, 0x9a7b149959ce60d8ull},
        {8, 0xdab99d95c6f90092ull},  {31, 0xa2aa5f33cc4a6119ull}, {32, 0x23c3c17ef790fd97ull},
        {33, 0x50a7cfc7ba588784ull}, {64, 0x0eb64b3ef6eeb01full}, {100, 0xa61f8d4c170fe531ull},
        {1000, 0x5f235fa033f1a3fbull},
    };
    for (const auto& c : cases) CHECK(tp::xxh64(p.data(), c.n) == c.h);
    CHECK(tp::xxh64(p.data(), 100, 0x123456789ull) == 0x8505071c55df3ceaull);
}

static void test_base_level_bytes() {
    CHECK(tp::base_level_bytes(8, 128, 128) == 128u * 128u / 2u);   // C4
    CHECK(tp::base_level_bytes(9, 64, 64) == 64u * 64u);            // C8
    CHECK(tp::base_level_bytes(5, 512, 512) == 512u * 512u * 2u);   // RGB5A3
    CHECK(tp::base_level_bytes(6, 256, 256) == 256u * 256u * 4u);   // RGBA8
    CHECK(tp::base_level_bytes(9, 4, 4) == 8u * 4u);                // C8 pads to an 8x4 block
    CHECK(tp::base_level_bytes(8, 4, 4) == 32u);                    // C4 pads to an 8x8 block
    CHECK(tp::base_level_bytes(14, 4, 4) == 32u);                   // CMPR pads to 8x8
    CHECK(tp::base_level_bytes(7, 4, 4) == 0u);                     // no format 7
}

static void test_keys() {
    // C4 8x8: nibbles 3..9 are used, so the palette hash covers entries 3..9 (bytes 6..20).
    std::vector<std::uint8_t> c4(32), pal(32);
    for (int i = 0; i < 32; ++i) {
        c4[i] = static_cast<std::uint8_t>(((3 + (i % 7)) << 4) | (3 + ((i * 5) % 7)));
        pal[i] = static_cast<std::uint8_t>((i * 13 + 1) & 255);
    }
    auto key = tp::compute_key(8, 8, 8, c4.data(), c4.size(), pal.data(), pal.size());
    CHECK(key.has_value());
    CHECK(key->texel_hash == 0x6f294be2f6de9faaull);
    CHECK(key->palette_hash == 0x3047d0e0c4cfcfbfull);
    CHECK(tp::dolphin_name(*key, false) == "tex1_8x8_6f294be2f6de9faa_3047d0e0c4cfcfbf_8");
    CHECK(tp::dolphin_name(*key, true) == "tex1_8x8_m_6f294be2f6de9faa_3047d0e0c4cfcfbf_8");
    // Texels past the base level (a mip chain) do not change the name.
    std::vector<std::uint8_t> with_mips = c4;
    with_mips.resize(64, 0xEE);
    auto key2 = tp::compute_key(8, 8, 8, with_mips.data(), with_mips.size(), pal.data(), pal.size());
    CHECK(key2 && *key2 == *key);
    // Palette entries outside the used range do not change the name either.
    std::vector<std::uint8_t> pal2 = pal;
    pal2[0] ^= 0xFF;
    pal2[31] ^= 0xFF;
    auto key3 = tp::compute_key(8, 8, 8, c4.data(), c4.size(), pal2.data(), pal2.size());
    CHECK(key3 && *key3 == *key);
    pal2[6] ^= 1;
    auto key4 = tp::compute_key(8, 8, 8, c4.data(), c4.size(), pal2.data(), pal2.size());
    CHECK(key4 && key4->palette_hash != key->palette_hash && key4->texel_hash == key->texel_hash);

    // C8 8x4: indices 10..195.
    std::vector<std::uint8_t> c8(32), pal8(512);
    for (int i = 0; i < 32; ++i) c8[i] = static_cast<std::uint8_t>(10 + ((i * 37) % 191));
    for (int i = 0; i < 512; ++i) pal8[i] = static_cast<std::uint8_t>((i * 29 + 5) & 255);
    auto k8 = tp::compute_key(9, 8, 4, c8.data(), c8.size(), pal8.data(), pal8.size());
    CHECK(k8 && k8->texel_hash == 0xfb1c269c7d2fb0ccull && k8->palette_hash == 0x0a1ef0b87ca63abfull);

    // Direct format: no palette part.
    const std::vector<std::uint8_t> p = pattern(64);
    auto kd = tp::compute_key(5, 4, 8, p.data(), p.size(), nullptr, 0);
    CHECK(kd && !kd->has_palette && kd->texel_hash == 0x0eb64b3ef6eeb01full);
    CHECK(tp::dolphin_name(*kd, false) == "tex1_4x8_0eb64b3ef6eeb01f_5");
    CHECK(!tp::compute_key(5, 4, 8, p.data(), 63, nullptr, 0));  // short data
}

static void test_parse() {
    auto a = tp::parse_name("tex1_128x128_0a670a23d69f0145_d1f065a85855fcc6_8.png");
    CHECK(a && a->key.width == 128 && a->key.height == 128 && a->key.format == 8 && !a->mipmapped && !a->dds);
    CHECK(a && a->key.texel_hash == 0x0a670a23d69f0145ull && a->key.palette_hash == 0xd1f065a85855fcc6ull &&
          a->key.has_palette);
    auto b = tp::parse_name("TEX1_512x512_M_84724037DE4AE3DF_5.DDS");
    CHECK(b && b->mipmapped && b->dds && !b->key.has_palette && b->key.format == 5 &&
          b->key.texel_hash == 0x84724037de4ae3dfull);
    auto c = tp::parse_name("tex1_64x64_m_9d0e3f36a81f6f67_$_8.png");
    CHECK(c && c->palette_wildcard && c->key.has_palette && c->mipmapped);
    auto d = tp::parse_name("tex1_64x64_9d0e3f36a81f6f67_394c6c7c8073a63c_8_mip2.png");
    CHECK(d && d->mip_level == 2);
    auto e = tp::parse_name("tex1_64x64_9d0e3f36a81f6f67_14_arb.dds");
    CHECK(e && e->arbitrary_mipmaps && e->key.format == 14);
    CHECK(!tp::parse_name("tex1_64x64_9d0e3f36a81f6f6_8.png"));   // 15 digits
    CHECK(!tp::parse_name("tex1_64x64_9d0e3f36a81f6f67_8.tga"));  // extension
    CHECK(!tp::parse_name("efb1_64x64_9d0e3f36a81f6f67_8.png"));  // EFB copies are not served
    CHECK(!tp::parse_name("tex1_64x64_9d0e3f36a81f6f67_8_zzz.png"));
    CHECK(!tp::parse_name("tex1_0x64_9d0e3f36a81f6f67_8.png"));
    // Round trip.
    tp::Key k{256, 128, 9, 0x0123456789abcdefull, 0xfedcba9876543210ull, true};
    auto r = tp::parse_name(tp::dolphin_name(k, true) + ".png");
    CHECK(r && r->key == k && r->mipmapped);
}

static void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
}

static std::vector<std::uint8_t> dds_header(std::uint32_t w, std::uint32_t h, std::uint32_t pf_flags,
                                            const char* fourcc, std::uint32_t bits, std::uint32_t r, std::uint32_t g,
                                            std::uint32_t b, std::uint32_t a) {
    std::vector<std::uint8_t> v(128, 0);
    std::memcpy(v.data(), "DDS ", 4);
    put32(v, 4, 124);
    put32(v, 12, h);
    put32(v, 16, w);
    put32(v, 76, 32);
    put32(v, 80, pf_flags);
    if (fourcc) std::memcpy(&v[84], fourcc, 4);
    put32(v, 88, bits);
    put32(v, 92, r);
    put32(v, 96, g);
    put32(v, 100, b);
    put32(v, 104, a);
    return v;
}

static void test_dds() {
    // Uncompressed BGRA 2x1.
    {
        auto v = dds_header(2, 1, 0x41, nullptr, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u);
        const std::uint8_t px[8] = {1, 2, 3, 4, 10, 20, 30, 40};  // B G R A
        v.insert(v.end(), px, px + 8);
        tp::Image img;
        std::string err;
        CHECK(tp::decode_dds(v.data(), v.size(), img, &err));
        CHECK(img.width == 2 && img.height == 1 && img.rgba8.size() == 8);
        CHECK(img.rgba8[0] == 3 && img.rgba8[1] == 2 && img.rgba8[2] == 1 && img.rgba8[3] == 4);
        CHECK(img.rgba8[4] == 30 && img.rgba8[7] == 40);
    }
    // BC1 4x4: c0 = pure red (0xF800), c1 = pure blue (0x001F); indices 0,1,2,3 on row 0.
    {
        auto v = dds_header(4, 4, 0x4, "DXT1", 0, 0, 0, 0, 0);
        const std::uint8_t block[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00};
        v.insert(v.end(), block, block + 8);
        tp::Image img;
        std::string err;
        CHECK(tp::decode_dds(v.data(), v.size(), img, &err));
        CHECK(img.rgba8[0] == 255 && img.rgba8[2] == 0 && img.rgba8[3] == 255);            // c0
        CHECK(img.rgba8[4] == 0 && img.rgba8[6] == 255);                                   // c1
        CHECK(img.rgba8[8] == 170 && img.rgba8[10] == 85);                                 // 2/3 c0 + 1/3 c1
        CHECK(img.rgba8[12] == 85 && img.rgba8[14] == 170);                                // 1/3 c0 + 2/3 c1
        CHECK(img.rgba8[16] == 255 && img.rgba8[18] == 0);                                 // row 1: index 0
    }
    // BC1 punch-through: c0 <= c1, index 3 is transparent black.
    {
        auto v = dds_header(4, 4, 0x4, "DXT1", 0, 0, 0, 0, 0);
        const std::uint8_t block[8] = {0x1F, 0x00, 0x00, 0xF8, 0x03, 0x00, 0x00, 0x00};
        v.insert(v.end(), block, block + 8);
        tp::Image img;
        CHECK(tp::decode_dds(v.data(), v.size(), img, nullptr));
        CHECK(img.rgba8[3] == 0 && img.rgba8[0] == 0);  // pixel 0: index 3
        CHECK(img.rgba8[7] == 255);                     // pixel 1: index 0
    }
    // BC3 4x4: alpha endpoints 255 / 0 (8-step mode), pixel 0 index 0 (255), pixel 1 index 1 (0); colour white.
    {
        auto v = dds_header(4, 4, 0x4, "DXT5", 0, 0, 0, 0, 0);
        const std::uint8_t block[16] = {255, 0, 0x08, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
        v.insert(v.end(), block, block + 16);
        tp::Image img;
        CHECK(tp::decode_dds(v.data(), v.size(), img, nullptr));
        CHECK(img.rgba8[3] == 255 && img.rgba8[7] == 0 && img.rgba8[0] == 255 && img.rgba8[1] == 255);
    }
    // BC7 is refused with a message, never misdecoded.
    {
        auto v = dds_header(4, 4, 0x4, "DX10", 0, 0, 0, 0, 0);
        std::vector<std::uint8_t> dx10(20, 0);
        put32(dx10, 0, 98);
        v.insert(v.end(), dx10.begin(), dx10.end());
        v.resize(v.size() + 16, 0);
        tp::Image img;
        std::string err;
        CHECK(!tp::decode_dds(v.data(), v.size(), img, &err));
        CHECK(err.find("BC7") != std::string::npos);
    }
    // Truncated data is refused.
    {
        auto v = dds_header(8, 8, 0x4, "DXT1", 0, 0, 0, 0, 0);
        v.resize(v.size() + 8, 0);
        tp::Image img;
        CHECK(!tp::decode_dds(v.data(), v.size(), img, nullptr));
    }
}

static void write_png(const std::filesystem::path& path, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = 255;
    }
    stbi_write_png(path.string().c_str(), w, h, 4, rgba.data(), w * 4);
}

static void test_index_and_lookup() {
    CHECK(!tp::texture_packs_enabled());
    CHECK(!tp::find_replacement(5, 4, 8, pattern(64).data(), 64, nullptr, 0, false));
    CHECK(tp::texture_pack_stats().lookups == 0u);  // off: nothing is hashed or looked up

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "gekko_texture_packs_tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "high" / "Sub");
    std::filesystem::create_directories(root / "low");
    const std::vector<std::uint8_t> p = pattern(64);
    const tp::Key kd = *tp::compute_key(5, 4, 8, p.data(), p.size(), nullptr, 0);
    // The same name in both packs: the first directory wins. Upper-case name in a subdirectory.
    std::string name = tp::dolphin_name(kd, true) + ".png";
    std::string upper = name;
    for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    write_png(root / "high" / "Sub" / upper, 8, 16, 255, 0, 0);
    write_png(root / "low" / name, 8, 16, 0, 255, 0);
    write_png(root / "low" / (tp::dolphin_name(kd, true) + "_mip1.png"), 4, 8, 0, 0, 255);
    // A paletted wildcard entry.
    std::vector<std::uint8_t> c8(32), pal8(512, 7);
    for (int i = 0; i < 32; ++i) c8[i] = static_cast<std::uint8_t>(i);
    const tp::Key k8 = *tp::compute_key(9, 8, 4, c8.data(), c8.size(), pal8.data(), pal8.size());
    char wild[128];
    std::snprintf(wild, sizeof(wild), "tex1_8x4_%016llx_$_9.png", static_cast<unsigned long long>(k8.texel_hash));
    write_png(root / "low" / wild, 6, 6, 1, 2, 3);  // 6x6: padded to 8x8
    // A broken file.
    const tp::Key kbad{4, 4, 5, 0x1111222233334444ull, 0, false};
    { std::ofstream(root / "low" / (tp::dolphin_name(kbad, false) + ".png"), std::ios::binary) << "not a png"; }

    tp::configure_texture_packs({root / "high", root / "low"});
    CHECK(tp::texture_packs_enabled());
    tp::Stats s = tp::texture_pack_stats();
    CHECK(s.files_indexed == 3u && s.files_shadowed == 1u && s.files_ignored == 1u);

    // Mip flag mismatch still hits (fallback); the high-priority (red) file is served.
    auto r = tp::find_replacement(5, 4, 8, p.data(), p.size(), nullptr, 0, false);
    CHECK(r && r->width == 8 && r->height == 16);
    // GX RGBA8 tile: first 32 bytes are (A, R) pairs.
    CHECK(r && r->gx_rgba8[0] == 255 && r->gx_rgba8[1] == 255 && r->gx_rgba8[33] == 0);
    auto w = tp::find_replacement(9, 8, 4, c8.data(), c8.size(), pal8.data(), pal8.size(), false);
    CHECK(w && w->width == 8 && w->height == 8);
    CHECK(!tp::find_replacement(5, 4, 4, p.data(), 32, nullptr, 0, false));  // a miss
    std::vector<std::uint8_t> bad_texels(32);
    // kbad's hash is invented, so no texels produce it: the broken file is never reached by a lookup, and stays
    // unloaded. Prove the failure path directly instead through a file whose name a real raster produces.
    const tp::Key kreal = *tp::compute_key(5, 4, 4, bad_texels.data(), bad_texels.size(), nullptr, 0);
    { std::ofstream(root / "low" / (tp::dolphin_name(kreal, false) + ".dds"), std::ios::binary) << "DDS broken"; }
    tp::configure_texture_packs({root / "high", root / "low"});
    CHECK(!tp::find_replacement(5, 4, 4, bad_texels.data(), bad_texels.size(), nullptr, 0, false));
    CHECK(!tp::find_replacement(5, 4, 4, bad_texels.data(), bad_texels.size(), nullptr, 0, false));
    s = tp::texture_pack_stats();
    CHECK(s.load_failures == 1u);  // counted once, then skipped
    CHECK(s.hits == 2u && s.mip_flag_fallbacks == 1u && s.wildcard_hits == 1u);

    tp::configure_texture_packs({});
    CHECK(!tp::texture_packs_enabled());

    // The CLI path: add_texture_pack defers the scan to the first lookup; the first directory added wins.
    tp::add_texture_pack(root / "low");
    tp::add_texture_pack(root / "high");
    CHECK(tp::texture_packs_enabled());
    auto lazy = tp::find_replacement(5, 4, 8, p.data(), p.size(), nullptr, 0, true);
    CHECK(lazy && lazy->gx_rgba8[1] == 0 && lazy->gx_rgba8[32] == 255);  // green: the "low" pack was added first
    CHECK(tp::texture_pack_directories().size() == 2u);
    tp::configure_texture_packs({});
    // An added directory without any tex1_ file switches the feature back off at the first lookup.
    std::filesystem::create_directories(root / "empty");
    tp::add_texture_pack(root / "empty");
    CHECK(!tp::find_replacement(5, 4, 8, p.data(), p.size(), nullptr, 0, true));
    CHECK(!tp::texture_packs_enabled());
    tp::configure_texture_packs({});
    std::filesystem::remove_all(root);
}

int main() {
    test_xxh64();
    test_base_level_bytes();
    test_keys();
    test_parse();
    test_dds();
    test_index_and_lookup();
    if (g_failures != 0) {
        std::fprintf(stderr, "texture_packs_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("texture_packs_tests: all passed\n");
    return 0;
}
