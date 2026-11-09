// The prompts' modern controller buttons (gekko/button_glyphs.h): the pad.cfg key, the family a set of connected
// controllers calls for and, given the game data (argv[1] = <disc>/sys/main.dol), the patch the game reads.

#include <cmath>
#include "shadow/backends/storage_backends.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "gekko/asset_mount.h"
#include "gekko/button_glyphs.h"
#include "gekko/gx_texture_decode.h"
#include "gekko/texture_overrides.h"
#include "peripheral/gamepad_binding.h"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

using gekko::button_glyphs::ButtonGlyphs;
using gekko::button_glyphs::Family;
using shadow::peripheral::ButtonStyle;
using shadow::peripheral::GamepadBinding;

void style_key() {
    ButtonStyle style{};
    for (const ButtonStyle s : {ButtonStyle::Auto, ButtonStyle::Original, ButtonStyle::Xbox, ButtonStyle::PlayStation,
                                ButtonStyle::Switch}) {
        check(shadow::peripheral::parse_button_style(shadow::peripheral::button_style_name(s), style) && style == s,
              std::string("style name round trip: ") + shadow::peripheral::button_style_name(s));
    }
    check(!shadow::peripheral::parse_button_style("Xbox", style), "style names are lower case");
    GamepadBinding binding;
    check(binding.button_style == ButtonStyle::Auto, "pad.cfg default is auto");
    std::string error;
    check(shadow::peripheral::parse_gamepad_binding("a = 0x1000\nbutton_style = playstation # mine\n", binding,
                                                    &error) &&
              binding.button_style == ButtonStyle::PlayStation,
          "pad.cfg button_style = playstation: " + error);
    check(!shadow::peripheral::parse_gamepad_binding("button_style = ps5\n", binding, &error),
          "pad.cfg rejects an unknown style");
}

void families() {
    using gekko::button_glyphs::family_of_vendors;
    check(family_of_vendors({}) == Family::Xbox, "no controller: Xbox");
    check(family_of_vendors({0x045E}) == Family::Xbox, "Microsoft: Xbox");
    check(family_of_vendors({0x2DC8}) == Family::Xbox, "another maker: Xbox");
    check(family_of_vendors({0x045E, 0x054C}) == Family::PlayStation, "DualSense behind an XInput mapper");
    check(family_of_vendors({0x28DE, 0x057E}) == Family::Switch, "Switch Pro behind Steam Input");
    check(family_of_vendors({0x057E, 0x054C}) == Family::PlayStation, "Sony before Nintendo");
}

std::vector<gekko::u8> read_all(gekko::assets::AssetSystem& assets, const std::string& path, gekko::u64 chunk) {
    std::vector<gekko::u8> out;
    const gekko::s32 entry = assets.names().resolve(path);
    if (entry <= 0) return out;
    const gekko::u32 token = assets.token_of(static_cast<gekko::u32>(entry));
    const gekko::u64 size = assets.names().entry(static_cast<gekko::u32>(entry)).size;
    out.resize(size);
    for (gekko::u64 at = 0; at < size; at += chunk) {
        const gekko::u64 n = std::min(chunk, size - at);
        gekko::u64 got = 0;
        if (assets.read(token + static_cast<gekko::u32>(at), n, out.data() + at, got) !=
            gekko::assets::AssetSystem::ReadStatus::Ok) {
            out.clear();
            return out;
        }
    }
    return out;
}

bool mount(gekko::assets::AssetSystem& assets, const std::filesystem::path& files) {
    std::string error;
    if (!shadow::backends::mount_host_directory(assets, files, std::vector<gekko::u8>(0x20, 0u), error)) {
        check(false, "mount: " + error);
        return false;
    }
    return true;
}

// The pixel a C8 texel byte holds (8 x 4 tiles).
void c8_pixel(std::size_t byte, gekko::u32 width, gekko::u32& x, gekko::u32& y) {
    const std::size_t tile = byte / 32u, in = byte % 32u;
    const gekko::u32 tiles_x = width / 8u;
    x = static_cast<gekko::u32>((tile % tiles_x) * 8u + in % 8u);
    y = static_cast<gekko::u32>((tile / tiles_x) * 4u + in / 8u);
}

void disc(const std::filesystem::path& files) {
    // Off: nothing is installed, every byte is the file's.
    std::vector<gekko::u8> original;
    {
        gekko::assets::AssetSystem assets;
        if (!mount(assets, files)) return;
        ButtonGlyphs glyphs;
        glyphs.configure(ButtonStyle::Original, GamepadBinding{});
        check(glyphs.attach(assets) == 0u, "original style patches no file");
        original = read_all(assets, "csdFiles/Options_EN.txd", 1u << 20);
        check(!original.empty(), "Options_EN.txd reads");
    }
    // The gamemenu sheet's place in Options_EN.txd: the texture-native body starts 24 bytes before its name.
    const char kName[] = "gamemenu";
    std::size_t name_at = 0;
    for (std::size_t i = 0; i + sizeof(kName) <= original.size(); ++i) {
        if (std::memcmp(original.data() + i, kName, sizeof(kName)) == 0) {
            name_at = i;
            break;
        }
    }
    check(name_at != 0u, "gamemenu is in Options_EN.txd");
    const std::size_t body = name_at - 24u;
    const std::size_t palette = body + 104u, texels = palette + 512u + 4u, texel_bytes = 256u * 256u;

    // The button cells of gamemenu (make_modern_buttons.py CELL_ROLE).
    struct Cell {
        gekko::u32 x0, y0, x1, y1;
        char gc;
    };
    const Cell cells[] = {{128, 203, 156, 231, 'a'}, {156, 203, 184, 231, 'x'}, {184, 203, 212, 231, 'y'},
                          {212, 203, 240, 231, 'a'}, {128, 230, 156, 256, 'y'}, {156, 230, 184, 256, 'a'},
                          {184, 230, 212, 256, 'a'}, {212, 230, 240, 256, 'y'}, {229, 177, 256, 202, 'b'}};
    auto cell_of = [&](gekko::u32 x, gekko::u32 y) -> const Cell* {
        for (const Cell& c : cells) {
            if (x >= c.x0 && x < c.x1 && y >= c.y0 && y < c.y1) return &c;
        }
        return nullptr;
    };

    std::vector<gekko::u8> by_label, swapped;
    for (const int pass : {0, 1}) {
        gekko::assets::AssetSystem assets;
        if (!mount(assets, files)) return;
        ButtonGlyphs glyphs;
        GamepadBinding binding;
        if (pass == 1) {  // the user's pad.cfg: GC B on the west button, GC X on the east one
            binding.b = shadow::peripheral::kGamepadWest;
            binding.x = shadow::peripheral::kGamepadEast;
        }
        glyphs.configure(ButtonStyle::Xbox, binding);
        const gekko::u32 patched_files = glyphs.attach(assets);
        check(patched_files == 187u, "187 files hold a button sheet (" + std::to_string(patched_files) + ")");
        check(glyphs.report().sheets == 27u, "27 distinct sheets (" + std::to_string(glyphs.report().sheets) + ")");
        check(glyphs.report().mismatched == 0u, "every listed file is this disc's");
        const std::vector<gekko::u8> whole = read_all(assets, "csdFiles/Options_EN.txd", 1u << 20);
        const std::vector<gekko::u8> pieces = read_all(assets, "csdFiles/Options_EN.txd", 999u);
        check(!whole.empty() && whole == pieces, "a read in pieces patches the same as one read");
        (pass == 0 ? by_label : swapped) = whole;
        // Every other button file decodes the same way in pieces (the HUD sheets embedded in .gncp files).
        for (const char* path : {"csdFiles/CommandWindow.gncp", "csdFiles/Hissatu_Hero.gncp", "csdFiles/VHArrow.gncp",
                                 "csdFiles/pause/pause0604_JP.txd", "InfoWindow.txd", "csdFiles/Synopsis_SP.txd"}) {
            check(read_all(assets, path, 1u << 20) == read_all(assets, path, 333u), std::string(path) + " in pieces");
        }
    }
    if (by_label.size() != original.size() || swapped.size() != original.size()) return;

    // Only the palette and the button cells change; the gamemenu sheet's own palette has room, so no texel outside
    // the cells moves.
    std::size_t changed_cells = 0;
    bool outside = false, elsewhere = false;
    for (std::size_t i = 0; i < original.size(); ++i) {
        if (by_label[i] == original[i]) continue;
        if (i >= palette && i < palette + 512u) continue;
        if (i < texels || i >= texels + texel_bytes) {
            elsewhere = true;
            continue;
        }
        gekko::u32 x = 0, y = 0;
        c8_pixel(i - texels, 256u, x, y);
        if (cell_of(x, y) == nullptr) outside = true;
        else ++changed_cells;
    }
    check(!elsewhere, "nothing outside the gamemenu sheet changes");
    check(!outside, "no gamemenu texel outside the button cells changes");
    check(changed_cells > 1000u, "the button cells change (" + std::to_string(changed_cells) + " texels)");

    // The binding moves the GC B and X cells only.
    bool binding_outside = false, binding_bx = false;
    for (std::size_t i = texels; i < texels + texel_bytes; ++i) {
        if (by_label[i] == swapped[i]) continue;
        gekko::u32 x = 0, y = 0;
        c8_pixel(i - texels, 256u, x, y);
        const Cell* c = cell_of(x, y);
        if (c == nullptr || (c->gc != 'b' && c->gc != 'x')) binding_outside = true;
        else binding_bx = true;
    }
    check(binding_bx, "swapping B and X in pad.cfg swaps the B and X cells");
    check(!binding_outside, "swapping B and X leaves the A and Y cells alone");
}

// The sharp sheets, on the vehicle arrow (16 colours: its paletted cap could not keep the Xbox B red - the user's
// grey checkpoint button). Its cell 48,0-84,36 means GC X; the mask 0,0-40,40 is the cap's white silhouette.
void sharp(const std::filesystem::path& files) {
    gekko::u32 sheet = 0;
    gekko::u64 span_at = 0;
    check(ButtonGlyphs::table_sheet_of("csdFiles/VHArrow.gncp", sheet, span_at), "VHArrow.gncp is listed");
    for (const int pass : {0, 1}) {
        gekko::assets::AssetSystem assets;
        if (!mount(assets, files)) return;
        ButtonGlyphs glyphs;
        GamepadBinding binding;
        if (pass == 1) {  // the user's pad.cfg: GC X on the east button, so the cell shows the Xbox B
            binding.b = shadow::peripheral::kGamepadWest;
            binding.x = shadow::peripheral::kGamepadEast;
        }
        glyphs.configure(ButtonStyle::Xbox, binding);
        std::vector<ButtonGlyphs::SharpSheet> sunk;
        glyphs.set_sharp_sink([&](ButtonGlyphs::SharpSheet&& s) {
            sunk.push_back(std::move(s));
            return true;
        });
        glyphs.attach(assets);
        const std::vector<gekko::u8> read = read_all(assets, "csdFiles/VHArrow.gncp", 1u << 20);
        check(sunk.size() == 1u, "reading VHArrow.gncp hands one sharp sheet to the sink");
        check(glyphs.report().sharp_served == 1u, "the report counts it");
        read_all(assets, "csdFiles/VHArrow.gncp", 777u);
        check(sunk.size() == 1u, "a second read of the same sheet hands nothing more");
        if (sunk.size() != 1u || read.empty()) continue;
        const ButtonGlyphs::SharpSheet& s = sunk[0];
        // What the sink keys it by is what the game reads (the renderer hashes the raster made from those bytes).
        check(s.gx_format == 8u && s.depth == 4u && s.tlut_format == 2u && s.width == 128u && s.height == 64u,
              "VHArrow raster header: C4, depth 4, RGB5A3, 128 x 64");
        check(s.palette_bytes == 32u && std::memcmp(read.data() + span_at, s.palette, 32u) == 0,
              "the sink's palette is the palette the game reads");
        check(s.texel_bytes == 128u * 64u / 2u &&
                  std::memcmp(read.data() + span_at + 36u, s.texels, s.texel_bytes) == 0,
              "the sink's texels are the texels the game reads");
        check(s.sharp_width == 160u && s.sharp_height == 80u && s.rgba8.size() == 160u * 80u * 4u,
              "the sharp sheet is the sheet at 5/4: 160 x 80");
        // The lifted renderer imports it as GX RGBA8: the encoding decodes back to the same pixels.
        {
            const std::vector<gekko::u8> gx =
                gekko::texture_overrides::encode_gx_rgba8(s.rgba8.data(), s.sharp_width, s.sharp_height);
            std::vector<gekko::u8> back(s.rgba8.size());
            check(gekko::gx_decode_texture(6u, s.sharp_width, s.sharp_height, gx.data(), gx.size(), nullptr, 0u, 0u,
                                           back.data()) &&
                      back == s.rgba8,
                  "the GX RGBA8 override decodes to the sharp sheet exactly");
            // BUTTON_GLYPHS_DUMP=<dir>: the sharp sheet as raw RGBA8 (160 x 80), to look at.
            if (const char* dir = std::getenv("BUTTON_GLYPHS_DUMP")) {
                const std::string path = std::string(dir) + "/vharrow_sharp_" + std::to_string(pass) + ".rgba";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(back.data(), 1, back.size(), f);
                    std::fclose(f);
                }
            }
        }
        // The lifted import's key for the patched raster finds what the sink keys it by.
        check(gekko::texture_overrides::content_key(read.data() + span_at + 36u, s.texel_bytes,
                                                    read.data() + span_at, 32u) ==
                  gekko::texture_overrides::content_key(s.texels, s.texel_bytes, s.palette, s.palette_bytes),
              "the content key of the bytes the game reads is the sink's key");

        // The cap in the X cell (sharp 60,0-105,45) in its family colour: Xbox X blue by label, Xbox B red when
        // pad.cfg puts GC X on the east button.
        auto count = [&](gekko::u32 x0, gekko::u32 y0, gekko::u32 x1, gekko::u32 y1, auto&& want) {
            gekko::u32 n = 0;
            for (gekko::u32 y = y0; y < y1; ++y) {
                for (gekko::u32 x = x0; x < x1; ++x) {
                    const gekko::u8* p = &s.rgba8[(static_cast<std::size_t>(y) * s.sharp_width + x) * 4u];
                    if (p[3] > 200u && want(p[0], p[1], p[2])) ++n;
                }
            }
            return n;
        };
        const auto red = [](int r, int g, int b) { return r > 170 && g < 110 && b < 110; };
        const auto blue = [](int r, int g, int b) { return b > 170 && r < 140 && b > g + 20; };
        const gekko::u32 reds = count(60, 0, 105, 45, red), blues = count(60, 0, 105, 45, blue);
        if (pass == 0) {
            check(blues > 40u && reds == 0u, "by label the cap is the blue Xbox X (" + std::to_string(blues) +
                                                 " blue, " + std::to_string(reds) + " red)");
        } else {
            check(reds > 40u && blues == 0u, "GC X on east: the cap is the red Xbox B (" + std::to_string(reds) +
                                                 " red, " + std::to_string(blues) + " blue)");
        }

        // Outside the cells it is the disc's art, scaled: each texel of the blue arrow under the cap whose 5 x 5
        // neighbourhood is opaque (no edge, where the resampling legitimately differs) against the sharp pixel
        // at the same place.
        gekko::assets::AssetSystem plain;
        if (!mount(plain, files)) return;
        const std::vector<gekko::u8> disc = read_all(plain, "csdFiles/VHArrow.gncp", 1u << 20);
        std::vector<gekko::u8> base(128u * 64u * 4u);
        check(gekko::gx_decode_texture(8u, 128u, 64u, disc.data() + span_at + 36u, 128u * 64u / 2u,
                                       disc.data() + span_at, 2u, 16u, base.data()),
              "the disc's VHArrow decodes");
        // Each sharp pixel against the original sampled bilinearly at the pixel's source position (what the GPU
        // drew from the paletted sheet), where all four texels of that sample are opaque.
        gekko::u32 compared = 0;
        double worst = 0.0, total = 0.0;
        for (gekko::u32 sy = 50; sy < 78; ++sy) {     // the arrow: below the cap
            for (gekko::u32 sx = 62; sx < 103; ++sx) {
                const double fx = (sx + 0.5) * 0.8 - 0.5, fy = (sy + 0.5) * 0.8 - 0.5;
                const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
                const double ax = fx - x0, ay = fy - y0;
                bool opaque = true;
                for (int k = 0; k < 4 && opaque; ++k) {
                    opaque = base[((y0 + k / 2) * 128 + x0 + k % 2) * 4 + 3] == 255u;
                }
                if (!opaque) continue;
                for (int c = 0; c < 3; ++c) {
                    auto at = [&](int x, int y) { return static_cast<double>(base[(y * 128 + x) * 4 + c]); };
                    const double want = (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) +
                                        (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
                    const double d = std::fabs(want - s.rgba8[(sy * 160u + sx) * 4u + c]);
                    worst = std::max(worst, d);
                    total += d;
                }
                ++compared;
            }
        }
        const double mean = compared != 0u ? total / (3.0 * compared) : 0.0;
        check(compared > 50u && worst <= 24.0 && mean <= 4.0,
              "the arrow outside the cells keeps its look in the sharp sheet (" + std::to_string(compared) +
                  " pixels, mean difference " + std::to_string(mean) + ", largest " + std::to_string(worst) + ")");
    }
}

// BUTTON_GLYPHS_DUMP=<dir>: every sharp sheet as the runtime builds it, for each family and two bindings (label;
// pad.cfg with GC B on west and GC X on east), as raw RGBA8 named <first file>__<family>_<binding>_<w>x<h>.rgba.
void dump_all(const std::filesystem::path& files, const std::string& dir) {
    const char* families[] = {"xbox", "playstation", "switch"};
    for (const int swapped : {0, 1}) {
        gekko::assets::AssetSystem assets;
        if (!mount(assets, files)) return;
        ButtonGlyphs glyphs;
        GamepadBinding binding;
        if (swapped == 1) {
            binding.b = shadow::peripheral::kGamepadWest;
            binding.x = shadow::peripheral::kGamepadEast;
        }
        glyphs.configure(ButtonStyle::Xbox, binding);
        glyphs.attach(assets);
        for (gekko::u32 sheet = 0; sheet < 64u; ++sheet) {
            for (gekko::u32 f = 1; f <= 3u; ++f) {
                ButtonGlyphs::SharpSheet s;
                if (!glyphs.sharp_sheet(sheet, static_cast<Family>(f), s)) continue;
                const std::string name = dir + "/sheet" + std::to_string(sheet) + "__" + families[f - 1u] +
                                         (swapped ? "_swapped_" : "_label_") + std::to_string(s.sharp_width) + "x" +
                                         std::to_string(s.sharp_height) + ".rgba";
                if (FILE* out = std::fopen(name.c_str(), "wb")) {
                    std::fwrite(s.rgba8.data(), 1, s.rgba8.size(), out);
                    std::fclose(out);
                }
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    style_key();
    families();
    if (argc > 1) {
        const std::filesystem::path files = std::filesystem::path(argv[1]).parent_path().parent_path() / "files";
        if (std::filesystem::exists(files / "csdFiles")) {
            disc(files);
            sharp(files);
            if (const char* dir = std::getenv("BUTTON_GLYPHS_DUMP")) dump_all(files, dir);
        } else {
            std::printf("game data not found beside %s: the disc checks were skipped\n", argv[1]);
        }
    }
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("button glyph checks passed\n");
    return 0;
}
