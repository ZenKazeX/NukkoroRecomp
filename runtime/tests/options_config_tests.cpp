// Tests of the PC settings files: gekko/config_file.h (the line-preserving reader / writer), gekko/pc_settings.h
// (options.cfg: load, validate, apply, save, restart) and display.cfg's Alt+Enter toggle (gekko/display_settings.h
// FullscreenToggle: borderless committed and saved at once, exclusive verified by the window owner's report, refused
// or unreported within 15 s rolled back).
//
// Sources to link: config_file.cpp, pc_settings.cpp, display_settings.cpp. This file stubs the engine owners
// pc_settings.cpp pushes to (audio gain, XInput preferences) and records what reached them.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gekko/audio_runtime.h"
#include "gekko/config_file.h"
#include "gekko/display_settings.h"
#include "gekko/pc_settings.h"
#include "gekko/xinput_pad.h"
#include "test_scratch.h"

// ---- stubs of what the code under test pushes to -------------------------------------------------------------------
namespace {
float g_gain = 1.0f;
unsigned g_rumble = 100;
bool g_inv_x = false, g_inv_y = false;
}  // namespace
namespace gekko {
void set_audio_output_gain(float gain) noexcept { g_gain = gain < 0 ? 0 : (gain > 1 ? 1 : gain); }
float audio_output_gain() noexcept { return g_gain; }
void set_xinput_preferences(unsigned rumble, bool x, bool y) noexcept {
    g_rumble = rumble;
    g_inv_x = x;
    g_inv_y = y;
}
}  // namespace gekko

namespace {

namespace cf = gekko::config_file;
using gekko::PcSettings;
using gekko::PcSettingsService;

int g_failures = 0, g_checks = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::filesystem::path g_dir;
std::filesystem::path file(const char* name) { return g_dir / name; }
void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << text;
}
std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
bool has_warning(const std::vector<std::string>& w, const char* needle) {
    for (const std::string& s : w)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

// ---- config_file ---------------------------------------------------------------------------------------------------
void test_config_file() {
    const auto kv = [](const char* key, const char* value) { return cf::KeyValue{{key}, value}; };
    // Parse: comments, case, spacing, bad lines.
    std::vector<int> bad;
    const auto entries = cf::parse("# c\nMaster_Volume =  50 # tail\nnonsense\n\n  x=1\r\ny = 2", &bad);
    check(entries.size() == 3u && entries[0].key == "master_volume" && entries[0].value == "50" &&
              entries[0].line == 2 && entries[1].key == "x" && entries[2].value == "2",
          "parse: keys lowered, values trimmed, comments removed");
    check(bad.size() == 1u && bad[0] == 3, "parse: a line without '=' is reported");
    // Rewrite keeps everything else byte for byte, including CRLF, comments and unknown keys.
    const std::string src = "# options\r\nmaster_volume = 50 # loud\r\nfuture_key=abc\r\n";
    check(cf::rewrite(src, {kv("master_volume", "75")}) == "# options\r\nmaster_volume = 75 # loud\r\nfuture_key=abc\r\n",
          "rewrite: value range replaced only; CRLF, comment and unknown key kept");
    check(cf::rewrite(src, {kv("rumble_strength", "50")}) ==
              "# options\r\nmaster_volume = 50 # loud\r\nfuture_key=abc\r\nrumble_strength=50\r\n",
          "rewrite: a missing key is appended with the file's line ending");
    check(cf::rewrite("a=1", {kv("b", "2")}) == "a=1\nb=2\n", "rewrite: no trailing newline gets one before the append");
    check(cf::rewrite("", {kv("b", "2")}) == "b=2\n", "rewrite: empty file");
    check(cf::rewrite("a=1\na=2\n", {kv("a", "9")}) == "a=9\na=9\n", "rewrite: a duplicated key is rewritten at every line");
    check(cf::rewrite("A = 1\n", {kv("a", "3")}) == "A = 3\n", "rewrite: keys are case-insensitive, the spelling stays");
    check(cf::rewrite("# a=1\n", {kv("a", "3")}) == "# a=1\na=3\n", "rewrite: a commented-out key is not the key");
    // A UTF-8 BOM (Notepad) is no part of the first key.
    const std::string bom = "\xEF\xBB\xBF" "master_volume=40\n";
    const auto be = cf::parse(bom);
    check(be.size() == 1u && be[0].key == "master_volume", "parse: BOM on the first line is skipped");
    check(cf::rewrite(bom, {kv("master_volume", "41")}) == "\xEF\xBB\xBF" "master_volume=41\n",
          "rewrite: BOM kept, first key found (not appended twice)");
    // update(): missing file created, unchanged file not touched, no temporary left behind.
    const auto p = file("cf_update.cfg");
    std::filesystem::remove(p);
    std::string error;
    check(cf::update(p, {kv("k", "v")}, &error) && read_file(p) == "k=v\n", "update: creates a missing file");
    check(!std::filesystem::exists(file("cf_update.cfg.tmp")), "update: no temporary file left");
    check(cf::update(p, {kv("k", "v")}, &error), "update: unchanged is a success");
    // A directory where the file should be: refused, reported, no crash.
    const auto d = file("cf_dir.cfg");
    std::filesystem::create_directories(d);
    check(!cf::update(d, {kv("k", "v")}, &error) && !error.empty(), "update: a directory in place of the file is an error");
}

// ---- pc_settings ----------------------------------------------------------------------------------------------------
void test_pc_load_defaults_and_bad_values() {
    {
        PcSettingsService s;
        std::vector<std::string> w;
        s.load(file("does_not_exist.cfg"), &w);
        check(s.current() == PcSettings{}, "load: a missing file = the defaults");
        check(s.loaded() && gekko::audio_output_gain() == 1.0f && !g_inv_x, "load: defaults pushed to the engine owners");
        check(!std::filesystem::exists(file("does_not_exist.cfg")), "load: a missing file is not created by loading");
    }
    {
        write_file(file("good.cfg"),
                   "master_volume=35\nrumble_strength=50\ninvert_camera_x=true\ninvert_camera_y=on\nlanguage=3\n"
                   "speaker=2\nvibration=off\nvoice=0\nautosave=no\n");
        PcSettingsService s;
        std::vector<std::string> w;
        s.load(file("good.cfg"), &w);
        const PcSettings c = s.current();
        check(c.master_volume == 35 && c.rumble_strength == 50 && c.invert_camera_x && c.invert_camera_y &&
                  c.language == 3 && c.speaker == 2 && c.vibration == 0 && c.voice == 0 && c.autosave == 0,
              "load: every key parsed");
        check(w.empty(), "load: a good file warns about nothing");
        check(gekko::audio_output_gain() == 0.35f && g_rumble == 50 && g_inv_x && g_inv_y,
              "load: the values reached the engine owners at load");
    }
    {
        // Each bad value falls back to its default, with a warning, and the good ones survive.
        write_file(file("bad.cfg"),
                   "master_volume=150\nrumble_strength=10\nlanguage=9\nspeaker=-3\nvibration=maybe\nvoice=2\n"
                   "autosave=7\ninvert_camera_x=zzz\nmaster_volume_x=1\ngarbage line\nrumble_strength=75\n");
        PcSettingsService s;
        std::vector<std::string> w;
        s.load(file("bad.cfg"), &w);
        const PcSettings c = s.current();
        check(c.master_volume == 100 && c.rumble_strength == 75 && c.language == -1 && c.speaker == -1 &&
                  c.vibration == -1 && c.voice == -1 && c.autosave == -1 && !c.invert_camera_x,
              "load: out-of-range / unparsable values give defaults, the later good value is kept");
        check(has_warning(w, "bad value for master_volume") && has_warning(w, "bad value for language") &&
                  has_warning(w, "bad value for vibration") && has_warning(w, "unknown key") &&
                  has_warning(w, "not key = value"),
              "load: each problem is reported");
        check(PcSettingsService::validate(c, nullptr), "load: the result always validates");
    }
    {
        write_file(file("bom.cfg"), "\xEF\xBB\xBF" "master_volume=20\n");
        PcSettingsService s;
        s.load(file("bom.cfg"), nullptr);
        check(s.current().master_volume == 20, "load: a file with a BOM is read");
    }
    {
        // Binary garbage and an overlong line: no crash, defaults, the file untouched.
        std::string junk(70000, 'x');
        junk += "\n\x01\x02\xFF\xFE=\x00\x01\n";
        write_file(file("junk.cfg"), junk);
        PcSettingsService s;
        std::vector<std::string> w;
        s.load(file("junk.cfg"), &w);
        check(s.current() == PcSettings{} && read_file(file("junk.cfg")) == junk, "load: garbage = defaults, file untouched");
    }
    {
        // A directory in place of options.cfg: defaults with a warning, and a later save fails cleanly.
        std::filesystem::create_directories(file("is_a_dir.cfg"));
        PcSettingsService s;
        std::vector<std::string> w;
        s.load(file("is_a_dir.cfg"), &w);
        check(s.current() == PcSettings{}, "load: an unreadable path = defaults");
        PcSettings p;
        p.master_volume = 10;
        std::string error;
        check(s.apply(p, &error) && !s.save(&error) && !error.empty(), "save: failure is reported, the setting stays in effect");
        check(s.current().master_volume == 10, "save: a failed save keeps the value in effect");
    }
    {
        PcSettingsService s;
        std::string error;
        check(!s.save(&error) && !error.empty(), "save before load: refused, no crash");
        PcSettings bad;
        bad.master_volume = 101;
        check(!s.apply(bad, &error) && s.current().master_volume == 100, "apply: an invalid value changes nothing");
        bad = PcSettings{};
        bad.rumble_strength = 24;
        check(!s.apply(bad, &error), "apply: rumble below 25 refused");
        bad = PcSettings{};
        bad.language = 7;
        check(!s.apply(bad, &error), "apply: language 7 refused");
        bad.language = 6;
        check(!s.apply(bad, &error), "apply: language 6 (the retired Hungarian) refused");
    }
}

void test_pc_save_semantics() {
    const std::string original =
        "# my settings\r\nmaster_volume = 50 # keep the comment\r\nfuture_option=xyz\r\nlanguage=1\r\n";
    write_file(file("save.cfg"), original);
    PcSettingsService s;
    s.load(file("save.cfg"), nullptr);
    std::string error;
    check(s.save(&error) && read_file(file("save.cfg")) == original, "save: nothing changed = file byte-identical");
    PcSettings p = s.current();
    p.master_volume = 80;
    p.rumble_strength = 25;
    check(s.apply(p, &error) && gekko::audio_output_gain() == 0.8f && g_rumble == 25, "apply: engine owners updated at once");
    check(read_file(file("save.cfg")) == original, "apply alone does not write the file");
    check(s.save(&error), "save succeeds");
    check(read_file(file("save.cfg")) ==
              "# my settings\r\nmaster_volume = 80 # keep the comment\r\nfuture_option=xyz\r\nlanguage=1\r\nrumble_strength=25\r\n",
          "save: only changed keys written; comment, CRLF and unknown key kept; language untouched");
    // -1 "not set" fields are never written, even when they differ from the file.
    p = s.current();
    p.speaker = -1;
    p.vibration = -1;
    check(s.apply(p, &error) && s.save(&error), "save: unset fields survive a save");
    check(read_file(file("save.cfg")).find("speaker") == std::string::npos, "save: -1 is not written as a key");
    // Restart: a second service sees exactly what was saved, and the first call to load pushes to the owners.
    p = s.current();
    p.language = 5;
    p.voice = 0;
    p.speaker = 2;
    p.vibration = 0;
    p.autosave = 0;
    p.invert_camera_y = true;
    check(s.apply(p, &error) && s.save(&error), "save: the game's own options");
    gekko::set_audio_output_gain(1.0f);
    gekko::set_xinput_preferences(100, false, false);
    PcSettingsService t;
    t.load(file("save.cfg"), nullptr);
    check(t.current() == s.current(), "restart: load gives exactly what was saved");
    check(gekko::audio_output_gain() == 0.8f && g_rumble == 25 && g_inv_y && !g_inv_x,
          "restart: the loaded values are applied to the engine owners without any menu");
    // Back to the defaults then save: the file then holds the defaults explicitly (a later load = defaults).
    PcSettings d = PcSettings{};
    d.language = t.current().language;
    d.speaker = 0;
    d.vibration = 1;
    d.voice = t.current().voice;
    d.autosave = 1;
    check(t.apply(d, &error) && t.save(&error), "defaults saved");
    PcSettingsService u;
    u.load(file("save.cfg"), nullptr);
    check(u.current() == d && g_rumble == 100 && gekko::audio_output_gain() == 1.0f && !g_inv_y, "defaults survive a restart");
    check(read_file(file("save.cfg")).find("future_option=xyz") != std::string::npos, "unknown key kept through all saves");
    // A value that was bad in the file (defaulted at load) and is set to the default later: not rewritten, stable.
    write_file(file("stale.cfg"), "master_volume=500\n");
    PcSettingsService v;
    std::vector<std::string> w;
    v.load(file("stale.cfg"), &w);
    check(!w.empty() && v.current().master_volume == 100, "bad file value: defaulted with a warning");
    p = v.current();
    p.master_volume = 60;
    check(v.apply(p, &error) && v.save(&error) && read_file(file("stale.cfg")) == "master_volume=60\n",
          "bad file value is replaced by the next real change");
}

// ---- Alt+Enter (display.cfg) --------------------------------------------------------------------------------------
// Ported from the retired settings registry's transactions test: the same toggle, now display_settings.h's own.
void test_fullscreen_toggle() {
    using Clock = gekko::FullscreenToggle::Clock;
    const std::filesystem::path cfg = file("display.cfg");
    write_file(cfg, "# the player's display settings\nvsync=true\n");
    gekko::DisplaySettingsService service;
    gekko::DisplaySettings start;
    start.fullscreen = false;
    service.initialize(start, cfg);
    gekko::FullscreenToggle toggle;
    const Clock::time_point t0 = Clock::now();

    // Windowed <-> borderless: committed and saved at once.
    check(toggle.toggle(service, nullptr, t0) && service.current().fullscreen && !service.provisional() &&
              !toggle.verifying(),
          "Alt+Enter to borderless fullscreen: committed");
    check(read_file(cfg).find("fullscreen=true") != std::string::npos, "Alt+Enter: saved to display.cfg");
    check(read_file(cfg).find("# the player's display settings") != std::string::npos, "Alt+Enter: the comment kept");
    check(toggle.toggle(service, nullptr, t0) && !service.current().fullscreen, "Alt+Enter back to windowed");
    check(read_file(cfg).find("fullscreen=false") != std::string::npos, "Alt+Enter: windowed saved");

    // Exclusive (display.cfg) by Alt+Enter: provisional until the window owner reports; refused -> rolled back.
    gekko::DisplaySettings ex = service.current();
    ex.exclusive = true;
    ex.fullscreen_width = 2560;
    ex.fullscreen_height = 1440;
    check(service.apply(ex), "exclusive set for fullscreen (windowed now)");
    service.save();
    const std::string before = read_file(cfg);
    check(toggle.toggle(service, nullptr, t0) && service.provisional() && toggle.verifying(),
          "Alt+Enter into exclusive is provisional");
    std::string error;
    check(!toggle.toggle(service, &error, t0) && !error.empty(), "a second Alt+Enter is refused while it is verified");
    toggle.tick(service, t0);
    check(toggle.verifying(), "no report yet: still verified");
    {
        gekko::DisplayWindowState w;
        w.generation = service.generation();
        w.fullscreen = w.exclusive_wanted = true;
        w.note = "exclusive fullscreen: lists no 2560x1440 mode; showing borderless at the desktop's size";
        service.report_window(w);
    }
    toggle.tick(service, t0);
    check(!toggle.verifying() && !service.current().fullscreen && !service.provisional() &&
              toggle.last_failure().find("2560x1440") != std::string::npos,
          "refused -> rolled back to windowed, the reason kept");
    check(read_file(cfg) == before, "refused: nothing saved");

    // Verified -> committed and saved.
    ex = service.current();
    ex.fullscreen_width = 1920;
    ex.fullscreen_height = 1080;
    service.apply(ex);
    check(toggle.toggle(service, nullptr, t0) && service.provisional(), "Alt+Enter into exclusive 1920x1080");
    {
        gekko::DisplayWindowState w;
        w.generation = service.generation();
        w.fullscreen = w.exclusive_wanted = w.exclusive_active = true;
        service.report_window(w);
    }
    toggle.tick(service, t0);
    check(!toggle.verifying() && service.current().fullscreen && !service.provisional() &&
              read_file(cfg).find("fullscreen_width=1920") != std::string::npos,
          "verified -> committed and saved");

    // Never reported: rolled back after 15 s, not before.
    check(toggle.toggle(service, nullptr, t0) && !service.current().fullscreen, "back to windowed");
    check(toggle.toggle(service, nullptr, t0) && service.provisional(), "Alt+Enter into exclusive again");
    toggle.tick(service, t0 + std::chrono::seconds(14));
    check(toggle.verifying() && service.current().fullscreen, "unreported: kept for 14 s");
    toggle.tick(service, t0 + gekko::FullscreenToggle::kVerifyLength);
    check(!toggle.verifying() && !service.current().fullscreen && !service.provisional(),
          "unreported for 15 s: rolled back to windowed");
}

}  // namespace

int main() {
    g_dir = gekko::test_support::scratch_directory("options_config_tests");
    std::error_code ec;
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir);
    test_config_file();
    test_pc_load_defaults_and_bad_values();
    test_pc_save_semantics();
    test_fullscreen_toggle();
    std::filesystem::remove_all(g_dir, ec);
    std::printf("options_config_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
