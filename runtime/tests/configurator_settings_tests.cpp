// The settings program's files (tools/padconfig/settings_model.h, pad_config.h) against the game's own loaders:
// display.cfg through gekko::parse_display_file, options.cfg through gekko::PcSettingsService::load, controls.cfg
// through shadow::peripheral::parse_controls and pad.cfg through parse_gamepad_binding. Checks that a round trip keeps
// unknown lines and comments, that only changed keys are written, that ranges are enforced, that the defaults are the
// game's, and that a file is created only when something is saved.
//
// The engine owners pc_settings.cpp pushes to (audio gain, XInput preferences) are stubs here.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gekko/display.h"
#include "gekko/display_settings.h"
#include "gekko/pc_settings.h"
#include "pad_config.h"
#include "settings_model.h"
#include "test_scratch.h"

namespace gekko {
void set_audio_output_gain(float) noexcept {}
void set_xinput_preferences(unsigned, bool, bool) noexcept {}
}  // namespace gekko

namespace {

using namespace padconfig;
namespace sp = shadow::peripheral;
namespace fs = std::filesystem;

int failures = 0, checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void write_all(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary | std::ios::trunc) << text; }

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        out.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    return out;
}

// The lines of `after` that are not in `before` (the written ones).
std::vector<std::string> new_lines(const std::string& before, const std::string& after) {
    const std::vector<std::string> a = lines_of(before), b = lines_of(after);
    std::vector<std::string> out;
    for (const std::string& line : b)
        if (std::find(a.begin(), a.end(), line) == a.end()) out.push_back(line);
    return out;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// display.cfg as the game reads it.
bool game_display(const std::string& text, gekko::DisplayConfig& config) {
    config = gekko::DisplayConfig{};
    std::string error;
    return gekko::parse_display_file(text, config, &error);
}

gekko::PcSettings game_options(const fs::path& path) {
    gekko::PcSettingsService service;
    service.load(path);
    return service.current();
}

void defaults_are_the_games() {
    const DisplayPrefs d;
    const gekko::DisplaySettings s;
    check(d.fullscreen == s.fullscreen && d.exclusive == s.exclusive && d.width == s.window_width &&
              d.height == s.window_height && d.fullscreen_width == s.fullscreen_width &&
              d.fullscreen_height == s.fullscreen_height && d.aspect == s.aspect && d.render_scale == s.render_scale &&
              d.msaa == s.msaa && d.anisotropy == s.anisotropy && d.render_rate == s.render_rate && d.vsync == s.vsync &&
              d.frame_interpolation == s.frame_interpolation && d.draw_distance == static_cast<double>(s.draw_distance) &&
              d.fps_counter == s.fps_counter,
          "display defaults are the game's (gekko::DisplaySettings)");
    const OptionPrefs o;
    const gekko::PcSettings p;
    check(o.master_volume == p.master_volume && o.music_volume == p.music_volume && o.sfx_volume == p.sfx_volume &&
              o.voice_volume == p.voice_volume && o.rumble_strength == p.rumble_strength &&
              o.invert_camera_x == p.invert_camera_x && o.invert_camera_y == p.invert_camera_y &&
              o.subtitles == p.subtitles && o.language == p.language && o.speaker == p.speaker &&
              o.vibration == p.vibration && o.voice == p.voice && o.autosave == p.autosave,
          "options defaults are the game's (gekko::PcSettings)");
    DisplayPrefs read;
    check(read_display("", read, nullptr) && read == d, "an empty display.cfg reads as the defaults");
    OptionPrefs ro;
    read_options("", ro);
    check(ro == o, "an empty options.cfg reads as the defaults");
    ControlsConfig rc;
    read_controls("", rc);
    check(rc == sp::default_controls(), "an empty controls.cfg reads as the game's default layout");
}

void display_file() {
    const std::string original =
        "# my display settings\r\n"
        "window_width = 1600   # wide\r\n"
        "height=900\r\n"
        "\r\n"
        "aspect_ratio = 2.37\r\n"
        "hud_scale = 1.0\r\n"
        "render_width = 0\r\n"
        "msaa = 4x\r\n";
    DisplayPrefs loaded;
    std::string error;
    check(read_display(original, loaded, &error), "a display.cfg the game reads is read");
    gekko::DisplayConfig game;
    check(game_display(original, game), "(the game reads it too)");
    check(loaded.width == 1600 && loaded.height == 900 && loaded.msaa == 4 && std::abs(loaded.aspect - 2.37) < 1e-9,
          "display values are read as the game reads them");

    // Unchanged: the text stays byte for byte.
    check(write_display(original, loaded, loaded) == original, "nothing changed: display.cfg text unchanged");

    DisplayPrefs after = loaded;
    after.vsync = false;
    after.render_rate = 144;
    after.frame_interpolation = true;
    after.width = 1920;
    after.aspect = 16.0 / 9.0;
    after.anisotropy = 1;
    after.draw_distance = 3;
    const std::string written = write_display(original, loaded, after);
    check(contains(written, "# my display settings\r\n") && contains(written, "hud_scale = 1.0\r\n") &&
              contains(written, "render_width = 0\r\n") && contains(written, "msaa = 4x\r\n") &&
              contains(written, "height=900\r\n"),
          "display.cfg: comments, unknown-to-the-program and unchanged lines are kept");
    check(contains(written, "window_width = 1920   # wide\r\n"), "display.cfg: a changed key keeps its spelling and comment");
    check(!contains(written, "aspect_ratio"), "display.cfg: the older spelling of the ratio is replaced");
    const std::vector<std::string> added = new_lines(original, written);
    check(added.size() == 7, "display.cfg: only the changed keys are written (7 lines)");
    check(game_display(written, game), "the game reads the written display.cfg");
    check(game.vsync && !*game.vsync && game.render_rate && *game.render_rate == 144.0 && game.frame_interpolation &&
              *game.frame_interpolation && game.window_width == 1920 && game.window_height == 900 && game.aspect_ratio &&
              *game.aspect_ratio == 16.0 / 9.0 && game.anisotropy && *game.anisotropy == 1 && game.draw_distance &&
              *game.draw_distance == 3.0f && game.msaa && *game.msaa == 4,
          "the game reads back exactly the values chosen");

    // Unlimited frame rate, exclusive full screen.
    DisplayPrefs more = after;
    more.render_rate = 0;
    more.fullscreen = more.exclusive = true;
    more.fullscreen_width = 2560;
    more.fullscreen_height = 1440;
    const std::string written2 = write_display(written, after, more);
    check(game_display(written2, game) && game.render_rate && *game.render_rate == 0.0 && game.fullscreen &&
              game.exclusive && game.fullscreen_width == 2560 && game.fullscreen_height == 1440,
          "unlimited frame rate and exclusive full screen are read by the game");
    DisplayPrefs back;
    check(read_display(written2, back, nullptr) && back == more, "the program reads back what it wrote");

    // Ranges.
    DisplayPrefs bad;
    bad.width = 200;
    check(!check_display(bad).empty(), "a window below 320 x 240 is refused");
    bad = {};
    bad.render_scale = 300;
    check(!check_display(bad).empty(), "a resolution scale above 200 is refused");
    bad = {};
    bad.render_rate = 2000;
    check(!check_display(bad).empty(), "a frame rate above 1000 is refused");
    bad = {};
    bad.msaa = 3;
    check(!check_display(bad).empty(), "an edge smoothing level the game lacks is refused");
    bad = {};
    bad.draw_distance = 9;
    check(!check_display(bad).empty(), "a draw distance above 8 is refused");
    bad = {};
    bad.fullscreen_width = 1920;
    check(!check_display(bad).empty(), "a full screen resolution with only one side is refused");
    check(check_display(DisplayPrefs{}).empty(), "the defaults are accepted");

    // A file the game refuses is reported, as the game words it.
    check(!read_display("vsync = maybe\n", loaded, &error) && contains(error, "line 1"), "a bad display.cfg is reported");
    check(!game_display("vsync = maybe\n", game), "(the game refuses it too)");
    check(!read_display("colour = red\n", loaded, &error), "an unknown display.cfg key is reported like the game");
}

void options_file(const fs::path& dir) {
    const fs::path file = dir / "options.cfg";
    const std::string original =
        "# options\n"
        "master_volume = 80\n"
        "mystery = 3\n"
        "music_volume = loud\n"
        "language = 2   # French\n"
        "vibration = true\n";
    write_all(file, original);
    OptionPrefs loaded;
    std::vector<std::string> warnings;
    read_options(original, loaded, &warnings);
    const gekko::PcSettings game = game_options(file);
    check(loaded.master_volume == game.master_volume && loaded.music_volume == game.music_volume &&
              loaded.language == game.language && loaded.vibration == game.vibration && loaded.voice == game.voice,
          "options.cfg is read as the game reads it (bad values keep their default)");
    check(warnings.size() == 2, "the unknown key and the bad value are reported");

    OptionPrefs after = loaded;
    after.voice = 0;
    after.subtitles = false;
    after.sfx_volume = 55;
    after.language = -1;  // back to the game's choice: the line goes
    std::string error;
    check(save_options(file, loaded, after, &error), "options.cfg saves");
    const std::string written = read_all(file);
    check(contains(written, "# options\n") && contains(written, "mystery = 3\n") &&
              contains(written, "master_volume = 80\n") && contains(written, "music_volume = loud\n") &&
              contains(written, "vibration = true\n"),
          "options.cfg: comments, unknown keys and unchanged lines are kept");
    check(!contains(written, "language"), "options.cfg: a game option set back to the game's choice is removed");
    check(new_lines(original, written).size() == 3, "options.cfg: only the changed keys are written");
    const gekko::PcSettings reread = game_options(file);
    check(reread.voice == 0 && !reread.subtitles && reread.sfx_volume == 55 && reread.language == -1 &&
              reread.master_volume == 80 && reread.vibration == 1,
          "the game reads back exactly the values chosen");

    // Ranges are refused and nothing is written.
    OptionPrefs bad = after;
    bad.rumble_strength = 10;
    check(!save_options(file, after, bad, &error) && !error.empty(), "rumble below 25 % is refused");
    bad = after;
    bad.master_volume = 101;
    check(!check_options(bad).empty(), "a volume above 100 is refused");
    bad = after;
    bad.voice = 2;
    check(!check_options(bad).empty(), "a spoken language the game lacks is refused");
    check(read_all(file) == written, "a refused save leaves the file as it was");
    check(!fs::exists(dir / "options.cfg.new"), "no temporary file is left");
}

void controls_file(const fs::path& dir) {
    const fs::path file = dir / "controls.cfg";
    const std::string original =
        "# my keys\n"
        "jump = key:Space, mouse:X1\n"
        "attack = key:Nope\n"
        "mouse_sensitivity_x = 2.5   # fast\n";
    write_all(file, original);
    ControlsConfig loaded;
    read_controls(original, loaded);
    ControlsConfig game = sp::default_controls();
    sp::parse_controls(original, game, nullptr);
    check(loaded == game, "controls.cfg is read as the game reads it");

    ControlsConfig after = loaded;
    after.binds[static_cast<std::size_t>(sp::Action::Chaos)] = {sp::key_bind('G'), {}, {}};
    after.mouse_invert_y = true;
    after.walk_deflection = 50;
    std::string error;
    check(save_controls(file, loaded, after, &error), "controls.cfg saves");
    const std::string written = read_all(file);
    check(contains(written, "# my keys\n") && contains(written, "jump = key:Space, mouse:X1\n") &&
              contains(written, "attack = key:Nope\n") && contains(written, "mouse_sensitivity_x = 2.5   # fast\n"),
          "controls.cfg: comments and unchanged lines are kept");
    check(new_lines(original, written).size() == 3, "controls.cfg: only the changed keys are written");
    ControlsConfig reread = sp::default_controls();
    std::vector<std::string> errors;
    sp::parse_controls(written, reread, &errors);
    check(reread == after, "the game reads back exactly the controls chosen");

    ControlsConfig bad = after;
    bad.mouse_sensitivity_x = 80;
    check(!save_controls(file, after, bad, &error) && !error.empty(), "a mouse speed above 50 is refused");
    check(read_all(file) == written, "a refused save leaves controls.cfg as it was");
}

void pad_file(const fs::path& dir) {
    const fs::path file = dir / "pad.cfg";
    std::string error;
    GamepadBinding b;
    check(save_binding(file, b, &error), "a missing pad.cfg is written whole");
    std::string text = read_all(file) + "# my own note\n";
    write_all(file, text);
    GamepadBinding changed = b;
    changed.a = sp::kGamepadEast;  // assign() would swap; a direct change is enough for the file
    changed.trigger_threshold = 90;
    check(save_binding(file, changed, &error), "pad.cfg saves");
    const std::string written = read_all(file);
    check(contains(written, "# my own note\n") && contains(written, "Written by Settings.exe"),
          "pad.cfg: comments are kept");
    check(contains(written, "= 0x2000   # B\n"), "pad.cfg: the changed button's own comment names its new button");
    check(new_lines(text, written).size() == 2, "pad.cfg: only the changed keys are written");
    GamepadBinding reread;
    check(sp::parse_gamepad_binding(written, reread, &error) && same(reread, changed), "the game reads back the mapping");
}

void created_only_on_save(const fs::path& dir) {
    const fs::path display = dir / "display.cfg";
    std::string error;
    const DisplayPrefs d;
    check(save_display(display, d, d, &error) && !fs::exists(display), "no change: display.cfg is not created");
    const OptionPrefs o;
    check(save_options(dir / "fresh-options.cfg", o, o, &error) && !fs::exists(dir / "fresh-options.cfg"),
          "no change: options.cfg is not created");
    DisplayPrefs v = d;
    v.vsync = false;
    check(save_display(display, d, v, &error) && fs::exists(display), "a change creates display.cfg");
    check(read_all(display) == "vsync=false\n", "a new display.cfg holds only the changed key");
    gekko::DisplayConfig game;
    check(game_display(read_all(display), game) && game.vsync && !*game.vsync, "the game reads the new file");

    // A key the game wrote meanwhile (the window resized) is kept.
    write_all(display, "vsync=false\nwidth=1700\n");
    DisplayPrefs w = v;
    w.fps_counter = true;
    check(save_display(display, v, w, &error), "display.cfg saves over a file the game changed");
    check(contains(read_all(display), "width=1700"), "the game's own change is kept");

    // A file that would stop the game is not written into.
    write_all(display, "vsync = sometimes\n");
    DisplayPrefs x = w;
    x.msaa = 8;
    check(!save_display(display, w, x, &error) && contains(error, "display.cfg"), "a display.cfg the game refuses is reported");
    check(read_all(display) == "vsync = sometimes\n", "and left as it was");
}

}  // namespace

int main() {
    const fs::path dir = gekko::test_support::scratch_directory("configurator-settings-tests");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    defaults_are_the_games();
    display_file();
    options_file(dir);
    controls_file(dir);
    pad_file(dir);
    created_only_on_save(dir);
    fs::remove_all(dir, ec);
    if (failures != 0) {
        std::printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("PASS configurator settings (%d checks)\n", checks);
    return 0;
}
