// Tests of the Options menu's Display tile (gekko/native_options_display.h, docs/OPTIONS_DISPLAY_TILE.md): the page's
// model, and - with the game data present (argument: sys/main.dol) - the tile's art in every language's Options2 and
// the help line in every language's front-end table.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "assets/chunk_stream.h"
#include "assets/texture_dictionary.h"
#include "assets/texture_import.h"
#include "gekko/native_options_display.h"
#include "gekko/native_save_text.h"

namespace {

namespace od = gekko::options_display;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::string narrow(const std::u16string& s) {
    std::string out;
    for (const char16_t c : s) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

bool read_file(const std::filesystem::path& path, std::vector<u8>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

void model() {
    od::set_screen_size(0, 0);
    od::set_work_area(0, 0);
    const od::Texts& t = od::texts(od::Language::EN);
    const std::u16string on = u"ON", off = u"OFF";
    const auto text = [&](const od::Model& m, od::Row row) { return narrow(m.value_text(row, t, on, off)); };
    gekko::DisplaySettings s;
    s.window_width = 1920;
    s.window_height = 1080;
    od::Model m = od::Model::from(s);
    check(text(m, od::kDisplayMode) == "Windowed", "windowed");
    check(text(m, od::kScreenRatio) == "Auto", "the ratio starts at Auto");
    check(text(m, od::kRenderResolution) == "100%", "the resolution scale in effect is 100%");
    check(text(m, od::kDrawDistance) == "x1 Original", "the default draw distance is x1 Original");
    check(m.settings(s) == s, "an untouched page changes nothing");

    // The ratio is its own setting: the scale and the window stay.
    m.step(od::kScreenRatio, 1);  // 4:3
    check(text(m, od::kScreenRatio) == "4:3", "the ratio steps to 4:3");
    check(m.viewport().width == 1440 && m.viewport().height == 1080, "4:3 in 1920 x 1080 is a 1440 x 1080 picture");
    gekko::DisplaySettings n = m.settings(s);
    check(n.window_width == 1920 && n.window_height == 1080 && n.aspect == 4.0 / 3.0 && n.render_scale == 100.0,
          "4:3 is applied as the aspect only");
    check(od::Model::from(n).settings(n) == n && text(od::Model::from(n), od::kScreenRatio) == "4:3",
          "4:3 reads back and round-trips");
    {
        od::Model r = od::Model::from(s);
        std::string order;
        for (int i = 0; i < 8; ++i) {
            r.step(od::kScreenRatio, 1);
            order += text(r, od::kScreenRatio) + " ";
        }
        check(order == "4:3 5:4 16:10 16:9 21:9 32:10 32:9 32:9 ", "ratios step in their order: " + order);
    }

    // The scale is its own setting, 25..200 %.
    m.step(od::kRenderResolution, -3);  // 100 -> 75 -> 67 -> 50
    check(text(m, od::kRenderResolution) == "50%", "the scale steps to 50%");
    n = m.settings(s);
    check(n.render_scale == 50.0 && n.window_width == 1920 && n.aspect == 4.0 / 3.0, "50% changes the scale only");
    {
        od::Model r = od::Model::from(s);
        r.step(od::kRenderResolution, 100);
        check(text(r, od::kRenderResolution) == "200%", "the scale stops at 200%");
        r.step(od::kRenderResolution, -100);
        check(text(r, od::kRenderResolution) == "25%", "and at 25%");
    }

    // Fullscreen keeps every other value, and the window size.
    m.step(od::kDisplayMode, 1);
    check(text(m, od::kDisplayMode) == "Fullscreen", "fullscreen");
    n = m.settings(s);
    check(n.fullscreen && !n.exclusive && n.window_width == 1920 && n.aspect == 4.0 / 3.0 && n.render_scale == 50.0,
          "fullscreen keeps the ratio, the scale and the window size");
    m.step(od::kDisplayMode, 5);
    check(m.value[od::kDisplayMode] == 1u, "two modes only (exclusive is a display.cfg key for tests)");
    {
        // An exclusive display.cfg keeps its key through the page.
        gekko::DisplaySettings x = s;
        x.fullscreen = x.exclusive = true;
        x.fullscreen_width = 800;
        x.fullscreen_height = 600;
        const od::Model xm = od::Model::from(x);
        check(text(xm, od::kDisplayMode) == "Fullscreen" && xm.settings(x) == x, "exclusive (display.cfg) round-trips");
    }

    // A ratio or scale without a name / list entry is kept as itself; the texture limit shows as "in use".
    gekko::DisplaySettings ca = s;
    ca.aspect = 2.37;
    ca.render_scale = 90.0;
    const od::Model cam = od::Model::from(ca);
    check(text(cam, od::kScreenRatio) == "2.37:1" && text(cam, od::kRenderResolution) == "90%",
          "a custom ratio and scale are listed");
    check(cam.settings(ca) == ca, "a custom ratio and scale round-trip");
    gekko::DisplaySettings big = s;
    big.fullscreen = true;
    big.render_scale = 200.0;
    od::set_screen_size(10240, 3200);
    od::Model bm = od::Model::from(big);
    check(text(bm, od::kRenderResolution) == "200% (160% in use)", "200% of 10240x3200 shows the scale in use");
    bm.step(od::kVsync, 1);
    check(bm.settings(big).render_scale == 200.0, "another row's change never rewrites the scale");
    od::set_screen_size(0, 0);

    // MSAA and filtering are not page rows, but their values round-trip untouched.
    {
        gekko::DisplaySettings q = s;
        q.msaa = 8;
        q.anisotropy = 2;
        check(od::Model::from(q).settings(q) == q, "MSAA / filtering values are kept");
    }

    // The page shows four rows, each changing what it names.
    check(od::kShownCount == 4u, "four rows");
    for (const od::Row row : od::kShownRows)
        check(row == od::kDisplayMode || row == od::kScreenRatio || row == od::kRenderResolution || row == od::kVsync,
              "mode, ratio, resolution scale, V-Sync");
}

// Fullscreen shows the picture in the desktop's size; windowed in the window's (the work area limits an oversize one).
void fullscreen_screen() {
    od::set_screen_size(1920, 1080);
    od::set_work_area(0, 0);
    gekko::DisplaySettings s;
    s.window_width = 1280;
    s.window_height = 960;
    s.fullscreen = true;
    od::Model m = od::Model::from(s);
    check(m.viewport().width == 1920 && m.viewport().height == 1080, "fullscreen: the picture is the screen's size");
    check(m.settings(s) == s, "fullscreen: an untouched page changes nothing");
    od::Model r = m;
    r.step(od::kScreenRatio, 1);  // 4:3
    check(r.viewport().width == 1440 && r.viewport().height == 1080, "fullscreen 4:3: a 1440 x 1080 picture, boxed");
    od::Model w = m;
    w.step(od::kDisplayMode, -1);
    check(w.viewport().width == 1280 && w.viewport().height == 960, "windowed: the window's 1280 x 960");
    od::set_work_area(1904, 1001);
    {
        gekko::DisplaySettings huge;
        huge.window_width = 2560;
        huge.window_height = 1440;
        const od::Model hm = od::Model::from(huge);
        check(hm.output().width == 1780u && hm.output().height == 1001u,
              "a window larger than the desktop shows as the largest of its shape that fits");
    }
    od::set_work_area(0, 0);
    od::set_screen_size(0, 0);
}

// A presented frame shows the page of its own game frame: not a later one, not one the game stopped drawing.
void frame_history() {
    constexpr std::uint64_t kFrame = 40500000ull / 60u;
    od::Shared shared;
    od::Page shown;
    shown.visible = true;
    shown.game_ticks = 10 * kFrame;
    shared.push(shown);
    od::Page hidden;
    hidden.game_ticks = 11 * kFrame;  // the next frame draws another tile's rows
    shared.push(hidden);
    check(shared.at(10 * kFrame + kFrame / 2) == &shared.pages[0], "the frame that drew the page shows it");
    check(shared.at(11 * kFrame + kFrame / 2) == nullptr, "the frame that drew another tile shows nothing");
    check(shared.at(10 * kFrame - 1) == nullptr, "a frame before the page shows nothing of it");
    od::Shared gone;
    gone.push(shown);
    check(gone.at(10 * kFrame + kFrame) != nullptr, "the next frame still shows it");
    check(gone.at(13 * kFrame) == nullptr, "a page the game stopped drawing is gone after two frames");
}

void utf8() {
    const std::u16string latin{u'R', static_cast<char16_t>(0x00E9), u's', u'o', u'l', u'u', u't', u'i', u'o', u'n'};
    const std::u16string cjk{static_cast<char16_t>(0x89E3), static_cast<char16_t>(0x50CF), static_cast<char16_t>(0x5EA6)};
    check(od::utf16("R\xC3\xA9solution") == latin, "UTF-8 decodes");
    check(od::utf16("\xE8\xA7\xA3\xE5\x83\x8F\xE5\xBA\xA6") == cjk, "UTF-8 CJK decodes");
}

// Every language's Options2: the two cells hold the tile, every other texel is the original's.
void tile_art(const std::filesystem::path& files) {
    namespace assets = shadow::assets;
    for (const std::string_view suffix : od::kLanguageSuffix) {
        const std::string name = "Options_" + std::string(suffix) + ".txd";
        std::vector<u8> txd, out;
        if (!read_file(files / "csdFiles" / name, txd)) {
            check(false, name + " is readable");
            continue;
        }
        std::string error;
        check(od::place_tile_art(txd, out, error), name + ": the tile is placed: " + error);
        check(out.size() == txd.size(), name + ": the dictionary keeps its size");
        const auto decode = [&](const std::vector<u8>& bytes, assets::TextureImage& image) {
            const assets::ByteView view{bytes.data(), bytes.size()};
            std::vector<assets::Chunk> top;
            assets::TextureDictionary dictionary;
            std::string e;
            if (!assets::read_stream(view, &top)) return false;
            const assets::Chunk* dict = assets::find(top, assets::chunk::kTextureDictionary);
            if (dict == nullptr || !assets::parse_texture_dictionary(view, *dict, &dictionary, &e)) return false;
            for (const assets::TextureRecord& r : dictionary.textures) {
                if (r.name != "Options2") continue;
                assets::ImportOptions options;
                options.generate_mips = false;
                return assets::import_texture(r, options, &image, &e);
            }
            return false;
        };
        assets::TextureImage before, after;
        if (!decode(txd, before) || !decode(out, after)) {
            check(false, name + ": Options2 decodes");
            continue;
        }
        u32 inside_changed = 0, outside_changed = 0;
        for (u32 y = 0; y < before.height; ++y) {
            for (u32 x = 0; x < before.width; ++x) {
                // The six tiles' cells: x 0..331 (normal 0 / 83, selected 166 / 249, 82 wide), y 0..246.
                const bool cell = y < 246u && x < 331u && (x % 83u) < 82u && (y % 82u) < 82u;
                const std::size_t at = (static_cast<std::size_t>(y) * before.width + x) * 4u;
                const bool same = std::equal(before.rgba.begin() + at, before.rgba.begin() + at + 4,
                                             after.rgba.begin() + at);
                (cell ? inside_changed : outside_changed) += same ? 0u : 1u;
            }
        }
        check(outside_changed == 0u, name + ": no texel outside the twelve cells changes (" +
                                         std::to_string(outside_changed) + ")");
        check(inside_changed > 1000u, name + ": the cells hold new art (" + std::to_string(inside_changed) + ")");
    }
}

// Every language's front-end table: the PC menu's description messages inserted in id order, every original message
// kept (by id), and a lookup by id finds the new and the old ones.
void help_line(const std::filesystem::path& files) {
    const std::vector<std::pair<u32, std::u16string>> messages = {
        {8000u, u"Change the screen mode."}, {8100u, u"Change the screen resolution."}, {8131u, u"Restore."}};
    for (u32 index = 0; index < od::kLanguageSuffix.size(); ++index) {
        const std::filesystem::path dir = files / "fonts" / "Advertise";
        const std::string base = "Advertise_" + std::string(od::kLanguageSuffix[index]);
        std::vector<u8> table, out;
        std::vector<std::vector<u8>> lists;
        if (!read_file(dir / (base + ".fnt"), table)) {
            check(false, base + ".fnt is readable");
            continue;
        }
        for (u32 n = 0; n < 100u; ++n) {
            char name[16];
            std::snprintf(name, sizeof(name), "%02u.met", n);
            std::vector<u8> met;
            if (!read_file(dir / (base + name), met)) break;
            lists.push_back(std::move(met));
        }
        std::string error;
        u32 missing = 0;
        check(od::insert_messages(table, lists, messages, 5001u, out, missing, error),
              base + ": the descriptions are inserted: " + error);
        std::u16string text, original;
        if (index != static_cast<u32>(od::Language::JP)) {
            check(od::message_by_id(out, 8100u, text) && text == u"Change the screen resolution.",
                  base + ": message 8100 is found by id");
        }
        for (const u32 id : {5001u, 5003u, 5414u, 5417u, 6100u}) {
            std::u16string before, after;
            check(od::message_by_id(table, id, before) && od::message_by_id(out, id, after) && before == after,
                  base + ": message " + std::to_string(id) + " is unchanged");
        }
        std::vector<u8> twice;
        check(!od::insert_messages(out, lists, messages, 5001u, twice, missing, error),
              base + ": an id that exists is refused");
        (void)original;
    }
}

}  // namespace

int main(int argc, char** argv) {
    model();
    fullscreen_screen();
    frame_history();
    utf8();
    if (argc > 1) {
        const std::filesystem::path files = std::filesystem::path(argv[1]).parent_path().parent_path() / "files";
        if (std::filesystem::exists(files / "csdFiles")) {
            tile_art(files);
            help_line(files);
        } else {
            std::printf("game data not found beside %s: the disc checks were skipped\n", argv[1]);
        }
    }
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("options_display_tests: all passed\n");
    return 0;
}
