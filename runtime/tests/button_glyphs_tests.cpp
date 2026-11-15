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
#include "../lift/render/view.h"
#include "peripheral/gamepad_binding.h"

namespace {

int g_failures = 0;
unsigned long long g_sharp_digest = 1469598103934665603ull;

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
        binding.b = shadow::peripheral::kGamepadEast;  // by label: each letter on the button of the same name
        binding.x = shadow::peripheral::kGamepadWest;
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
        binding.b = shadow::peripheral::kGamepadEast;  // by label: each letter on the button of the same name
        binding.x = shadow::peripheral::kGamepadWest;
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

// Every glyph the player can be shown is the same size in every layout (user request 2026-11-13). Measured on what
// the game receives: the visible box (alpha above 24 of 255) of every button cell in the paletted sheet the game
// reads and in the sharp sheet the native renderer draws, for each of the 3 layouts, per sheet.
//  * the same cell: every layout's glyph has the same box within 2 px (the caps are identical; a start prompt's
//    word under its cap is a different length per layout, so only its height is compared) and the same centre;
//  * one layout, one sheet: every plain face cap (A / B / X / Y) is the same size within 1 px, whichever disc glyph
//    it replaced (the disc's own varied from 21 to 26 px).
struct Box {
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1;
    bool empty() const { return x1 < 0; }
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
    double cx() const { return (x0 + x1) / 2.0; }
    double cy() const { return (y0 + y1) / 2.0; }
};

Box visible_box(const std::vector<gekko::u8>& rgba, gekko::u32 stride, gekko::u32 x0, gekko::u32 y0, gekko::u32 x1,
                gekko::u32 y1) {
    Box b;
    for (gekko::u32 y = y0; y < y1; ++y) {
        for (gekko::u32 x = x0; x < x1; ++x) {
            if (rgba[(static_cast<std::size_t>(y) * stride + x) * 4u + 3u] <= 24u) continue;
            b.x0 = std::min(b.x0, static_cast<int>(x - x0));
            b.y0 = std::min(b.y0, static_cast<int>(y - y0));
            b.x1 = std::max(b.x1, static_cast<int>(x - x0) + 1);
            b.y1 = std::max(b.y1, static_cast<int>(y - y0) + 1);
        }
    }
    return b;
}

std::string box_text(const Box& b) { return std::to_string(b.w()) + "x" + std::to_string(b.h()); }

// How the game draws a button cell: screen px per texel (x, y). Every cast of every .gncp was measured (quad over
// texels, docs/agents/button-glyphs/draw_scale.py): 1:1 except the vehicle arrow's button (quad 36 x 33.48 for its
// 36 x 36 cell), its outline disc (46.2 x 42 for 42 x 42) and the cutscene skip button (46 x 29 for 45 x 28 and 46 x 28).
void draw_scale(const ButtonGlyphs::CellInfo& c, double& sx, double& sy) {
    sx = sy = 1.0;
    if (c.kind == 4u && c.x0 == 36u && c.y0 == 25u) {
        sx = 46.0 / 45.0;
        sy = 29.0 / 28.0;
    }
    if (c.kind == 4u && c.x0 == 82u && c.y0 == 27u) {
        sx = 1.0;
        sy = 29.0 / 28.0;
    }
    if (c.kind <= 3u && c.x0 == 48u && c.y0 == 0u) {
        sx = 1.0;
        sy = 0.93;
    }
    if (c.kind == 5u && c.x0 == 2u && c.y0 == 0u) {
        sx = 46.2 / 42.0;
        sy = 1.0;
    }
}

// The cap's diameters in px, to a fraction of a pixel (make_modern_buttons.py cap_diameters is the same measure): the
// cap is the opaque pixels (alpha >= 200: the drop shadow is translucent) of the rows from the top of the first opaque
// run down to the first row with none near the middle (a word under the cap is another run); for a disc the standard
// deviation of those pixels along an axis is a quarter of its diameter.
bool cap_diameters(const std::vector<gekko::u8>& rgba, gekko::u32 stride, gekko::u32 x0, gekko::u32 y0, gekko::u32 x1,
                   gekko::u32 y1, double& dx, double& dy) {
    auto opaque = [&](gekko::u32 x, gekko::u32 y) {
        return rgba[(static_cast<std::size_t>(y) * stride + x) * 4u + 3u] >= 200u;
    };
    const gekko::u32 w = x1 - x0;
    gekko::u32 top = y0;
    auto row_has = [&](gekko::u32 y, gekko::u32 a, gekko::u32 b) {
        for (gekko::u32 x = a; x < b; ++x) {
            if (opaque(x, y)) return true;
        }
        return false;
    };
    while (top < y1 && !row_has(top, x0, x1)) ++top;
    if (top >= y1) return false;
    gekko::u32 end = top;
    while (end + 1u < y1 && row_has(end + 1u, x0 + w / 4u, x1 - w / 4u)) ++end;
    double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0;
    for (gekko::u32 y = top; y <= end; ++y) {
        for (gekko::u32 x = x0; x < x1; ++x) {
            if (!opaque(x, y)) continue;
            n += 1.0;
            sx += x;
            sy += y;
            sxx += static_cast<double>(x) * x;
            syy += static_cast<double>(y) * y;
        }
    }
    dx = 4.0 * std::sqrt(sxx / n - (sx / n) * (sx / n));
    dy = 4.0 * std::sqrt(syy / n - (sy / n) * (sy / n));
    return true;
}

// The native renderer puts the game's 640 x 480 canvas on the output with one scale in x and y (canvas_fit): at every
// output aspect and anchor a canvas px is as wide as it is tall, so a quad the game draws square stays square.
void canvas_is_square() {
    using shadow::render::Anchor;
    for (const float aspect : {0.75f, 1.0f, 4.0f / 3.0f, 16.0f / 10.0f, 16.0f / 9.0f, 21.0f / 9.0f, 3.2f, 32.0f / 9.0f}) {
        for (const Anchor anchor : {Anchor::Left, Anchor::Center, Anchor::Right, Anchor::Projected, Anchor::Continued}) {
            const shadow::render::CanvasFit fit =
                shadow::render::canvas_fit(640.0f, 480.0f, 4.0f / 3.0f, anchor, aspect);
            const double view_h = 1000.0, view_w = view_h * aspect;
            const double px_x = fit.sx * 0.5 * view_w, px_y = -fit.sy * 0.5 * view_h;
            check(std::fabs(px_x / px_y - 1.0) < 1e-4,
                  "a canvas px is square on screen at aspect " + std::to_string(aspect));
        }
    }
}

void equal_sizes(const std::filesystem::path& files) {
    using Info = ButtonGlyphs::CellInfo;
    std::size_t compared_cells = 0, compared_sheets = 0, compared_caps = 0;
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
            std::vector<Info> cells;
            if (!ButtonGlyphs::sheet_cells(sheet, cells)) break;
            // [layout][cell] -> box on the sharp sheet / on the paletted sheet
            std::vector<Box> sharp_box[3], pal_box[3];
            bool have = true;
            for (gekko::u32 f = 0; f < 3u && have; ++f) {
                ButtonGlyphs::SharpSheet s;
                if (!glyphs.sharp_sheet(sheet, static_cast<Family>(f + 1u), s)) {
                    have = false;
                    break;
                }
                // cs2/ram: a digest of every sharp sheet's bytes (the whole rgba8), printed at the end: the run with
                // GEKKO_CS2_RAM=0 (scaled art made at attach by a worker) and the default (made at first use) must match.
                for (const gekko::u8 byte : s.rgba8) g_sharp_digest = (g_sharp_digest ^ byte) * 1099511628211ull;
                g_sharp_digest = (g_sharp_digest ^ sheet) * 1099511628211ull;
                std::vector<gekko::u8> pal(static_cast<std::size_t>(s.width) * s.height * 4u);
                check(gekko::gx_decode_texture(s.gx_format, s.width, s.height, s.texels, s.texel_bytes, s.palette,
                                               s.tlut_format, static_cast<gekko::u32>(s.palette_bytes / 2u),
                                               pal.data()),
                      "the patched sheet decodes (sheet " + std::to_string(sheet) + ")");
                for (const Info& c : cells) {
                    const gekko::u32 sx0 = c.x0 * s.sharp_width / s.width, sy0 = c.y0 * s.sharp_height / s.height,
                                     sx1 = (c.x1 * s.sharp_width + s.width - 1u) / s.width,
                                     sy1 = (c.y1 * s.sharp_height + s.height - 1u) / s.height;
                    sharp_box[f].push_back(visible_box(s.rgba8, s.sharp_width, sx0, sy0, sx1, sy1));
                    pal_box[f].push_back(visible_box(pal, s.width, c.x0, c.y0, c.x1, c.y1));
                    // Round on screen: the cap's width and height after the game's draw scale (canvas px; the sharp
                    // sheet is sharp_width / width of the sheet), within 0.25 px on the sharp sheet and 1 px on the
                    // paletted fallback (the vehicle arrow's outline disc is measured the same way).
                    double dsx = 1.0, dsy = 1.0;
                    draw_scale(c, dsx, dsy);
                    const double k = static_cast<double>(s.sharp_width) / s.width;
                    const std::string cell_text = "sheet " + std::to_string(sheet) + " layout " + std::to_string(f) +
                                                  " cell " + std::to_string(c.x0) + "," + std::to_string(c.y0);
                    for (const int sharp_pass : {1, 0}) {
                        double dx = 0.0, dy = 0.0;
                        bool ok = false;
                        if (sharp_pass != 0) {
                            ok = cap_diameters(s.rgba8, s.sharp_width, sx0, sy0, sx1, sy1, dx, dy);
                        } else {
                            ok = cap_diameters(pal, s.width, c.x0, c.y0, c.x1, c.y1, dx, dy);
                        }
                        check(ok, "a cap is found: " + cell_text);
                        if (!ok) continue;
                        const double unit = sharp_pass != 0 ? 1.0 / k : 1.0;
                        const double on_x = dx * unit * dsx, on_y = dy * unit * dsy;
                        check(std::fabs(on_x - on_y) <= (sharp_pass != 0 ? 0.25 : 1.0) + 1e-9,
                              std::string("round on screen (") + (sharp_pass != 0 ? "sharp" : "paletted") + "): " +
                                  cell_text + " draws " + std::to_string(on_x) + " x " + std::to_string(on_y) + " px");
                        ++compared_caps;
                    }
                }
            }
            if (!have) continue;
            ++compared_sheets;
            std::vector<std::size_t> face;  // the plain face cells (GC A B X Y)
            for (std::size_t i = 0; i < cells.size(); ++i) {
                if (cells[i].kind <= 3u) face.push_back(i);
            }
            const std::string sheet_text = "sheet " + std::to_string(sheet) + (swapped ? " (swapped)" : "");
            for (std::size_t i = 0; i < cells.size(); ++i) {
                const bool start = cells[i].kind == 4u;
                const std::string where = sheet_text + " cell " + std::to_string(cells[i].x0) + "," +
                                          std::to_string(cells[i].y0);
                for (const int sharp_pass : {1, 0}) {
                    const auto* boxes = sharp_pass != 0 ? sharp_box : pal_box;
                    const std::string what = sharp_pass != 0 ? "sharp" : "paletted";
                    for (gekko::u32 f = 1; f < 3u; ++f) {
                        const Box& a = boxes[0][i];
                        const Box& b = boxes[f][i];
                        check(!a.empty() && !b.empty(), what + " glyph present: " + where);
                        if (a.empty() || b.empty()) continue;
                        check(std::abs(a.h() - b.h()) <= 2 && (start || std::abs(a.w() - b.w()) <= 2),
                              what + " box the same in every layout: " + where + " layout " + std::to_string(f) +
                                  " " + box_text(b) + " vs Xbox " + box_text(a));
                        check(std::fabs(a.cx() - b.cx()) <= 1.0 && std::fabs(a.cy() - b.cy()) <= 1.0,
                              what + " centre the same in every layout: " + where + " layout " + std::to_string(f));
                    }
                }
                ++compared_cells;
            }
            // One layout, one sheet: the plain face caps are one size.
            for (gekko::u32 f = 0; f < 3u; ++f) {
                for (const int sharp_pass : {1, 0}) {
                    const auto* boxes = sharp_pass != 0 ? sharp_box : pal_box;
                    for (const std::size_t i : face) {
                        const Box& a = boxes[f][face[0]];
                        const Box& b = boxes[f][i];
                        if (a.empty() || b.empty()) continue;
                        check(std::abs(a.w() - b.w()) <= 1 && std::abs(a.h() - b.h()) <= 1,
                              std::string(sharp_pass != 0 ? "sharp" : "paletted") + " face caps one size, " +
                                  sheet_text + " layout " + std::to_string(f) + " cell " +
                                  std::to_string(cells[i].x0) + "," + std::to_string(cells[i].y0) + ": " +
                                  box_text(b) + " vs " + box_text(a));
                    }
                }
            }
        }
    }
    check(compared_sheets >= 54u && compared_cells >= 200u,
          "glyph sizes compared on 27 sheets x 2 bindings (" + std::to_string(compared_sheets) + " sheets, " +
              std::to_string(compared_cells) + " cells)");
    check(compared_caps >= 800u, "caps measured for roundness (" + std::to_string(compared_caps) + ")");
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
        binding.b = shadow::peripheral::kGamepadEast;  // by label: each letter on the button of the same name
        binding.x = shadow::peripheral::kGamepadWest;
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
    canvas_is_square();
    if (argc > 1) {
        const std::filesystem::path files = std::filesystem::path(argv[1]).parent_path().parent_path() / "files";
        if (std::filesystem::exists(files / "csdFiles")) {
            disc(files);
            sharp(files);
            equal_sizes(files);
            if (const char* dir = std::getenv("BUTTON_GLYPHS_DUMP")) dump_all(files, dir);
        } else {
            std::printf("game data not found beside %s: the disc checks were skipped\n", argv[1]);
        }
    }
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("sharp-sheet digest %016llx\n", g_sharp_digest);
    std::printf("button glyph checks passed\n");
    return 0;
}
