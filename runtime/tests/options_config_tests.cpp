// Tests of the Options configuration layer: gekko/config_file.h (the line-preserving reader / writer), gekko/pc_settings.h
// (options.cfg: load, validate, apply, save) and the game-facing providers of gekko/settings_game.h (Audio, Controls,
// Save Data) over a fake settings::GameOptions - the lifecycle default -> change -> apply -> save -> restart -> restore.
//
// Sources to link: config_file.cpp, pc_settings.cpp, settings_game.cpp, settings_registry.cpp. This file stubs the
// engine owners pc_settings.cpp pushes to (audio gain, XInput preferences), the Display / Graphics providers and the
// Japanese-voice flag, and records what reached them.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gekko/audio_runtime.h"
#include "gekko/config_file.h"
#include "gekko/pc_settings.h"
#include "gekko/settings_display.h"
#include "gekko/settings_game.h"
#include "gekko/settings_registry.h"
#include "gekko/voice_language.h"
#include "gekko/xinput_pad.h"
#include "test_scratch.h"

// ---- stubs of what the code under test pushes to -------------------------------------------------------------------
namespace {
float g_gain = 1.0f;
unsigned g_rumble = 100;
bool g_inv_x = false, g_inv_y = false;
bool g_japanese = true;
}  // namespace
namespace gekko {
void set_audio_output_gain(float gain) noexcept { g_gain = gain < 0 ? 0 : (gain > 1 ? 1 : gain); }
float audio_output_gain() noexcept { return g_gain; }
void set_xinput_preferences(unsigned rumble, bool x, bool y) noexcept {
    g_rumble = rumble;
    g_inv_x = x;
    g_inv_y = y;
}
void set_japanese_voices_available(bool a) noexcept { g_japanese = a; }
bool japanese_voices_available() noexcept { return g_japanese; }
namespace settings {
std::unique_ptr<Provider> make_display_provider() { return nullptr; }
std::unique_ptr<Provider> make_graphics_provider() { return nullptr; }
}  // namespace settings
}  // namespace gekko

namespace {

namespace cf = gekko::config_file;
namespace gs = gekko::settings;
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
        check(!s.apply(bad, &error), "apply: language 7 refused (6 = Hungarian is valid)");
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

// ---- providers over a fake game -----------------------------------------------------------------------------------
// The fake plays the game's side: apply() only queues (the game's state 3 writes the values a frame later), exactly the
// shape of Menu::apply.
class FakeGame final : public gs::GameOptions {
public:
    bool available() const override { return on; }
    unsigned speaker() const override { return g_speaker; }
    bool vibration() const override { return g_vibration; }
    unsigned language() const override { return g_language; }
    unsigned voice() const override { return g_voice; }
    bool apply(unsigned sp, bool vib, unsigned lang, unsigned voi, std::string*) override {
        ++applies;
        q_sp = sp, q_vib = vib, q_lang = lang, q_voi = voi;
        queued = true;
        return true;
    }
    bool autosave() const override { return g_autosave; }
    bool set_autosave(bool v, std::string*) override {
        g_autosave = v;
        return true;
    }
    bool run(std::string_view, std::string*) override { return true; }
    void frame() {  // state 3 runs
        if (!queued) return;
        g_speaker = q_sp, g_vibration = q_vib, g_language = q_lang, g_voice = q_voi;
        queued = false;
    }
    bool on = true, queued = false;
    unsigned g_speaker = 0, g_language = 1, g_voice = 1, q_sp = 0, q_lang = 1, q_voi = 1;
    bool g_vibration = true, g_autosave = true, q_vib = true;
    int applies = 0;
};

const gs::SettingView* find(const std::vector<gs::SettingView>& v, const char* id) {
    for (const gs::SettingView& s : v)
        if (s.id == id) return &s;
    return nullptr;
}

// The menu's restore_defaults loop (native_options_menu.cpp) for a set of categories.
void restore(gs::Registry& r, bool every, gs::Category item) {
    static constexpr gs::Category kOrder[] = {gs::Category::Graphics, gs::Category::Audio, gs::Category::Controls,
                                              gs::Category::Gameplay, gs::Category::System, gs::Category::Display};
    for (const gs::Category c : kOrder) {
        if (!every && c != item) continue;
        r.begin(c);
        r.reset_defaults(c);
        std::string error;
        r.apply_pending(c, &error);
        r.begin(c);
    }
}

void test_providers() {
    write_file(file("prov.cfg"), "language=2\nvoice=1\nmaster_volume=65\nrumble_strength=30\n");
    gekko::pc_settings().load(file("prov.cfg"), nullptr);
    gs::Registry reg;
    reg.add(gs::make_audio_provider());
    reg.add(gs::make_controls_provider());
    reg.add(gs::make_gameplay_provider());
    reg.add(gs::make_system_provider("test build"));
    FakeGame game;
    game.g_language = 2;
    gs::set_game_options(&game);

    // What the pages show after "a restart": the file's values, not the defaults.
    reg.begin(gs::Category::Audio);
    reg.begin(gs::Category::Controls);
    auto audio = reg.list(gs::Category::Audio);
    check(find(audio, "audio.master_volume")->applied == 65 && find(audio, "audio.subtitles")->applied == 2,
          "page: shows the restored volume and the game's language");
    auto controls = reg.list(gs::Category::Controls);
    check(find(controls, "controls.rumble_strength")->applied == 0, "page: rumble 30 % in the file shows its nearest step (25), not 100");
    check(!reg.dirty(gs::Category::Controls), "page: nothing pending after begin");

    // Change, apply: engine state changes, the file holds it.
    check(reg.set("audio.master_volume", 40) && !reg.dirty(gs::Category::Audio), "volume: the slider applies as it moves");
    check(gekko::audio_output_gain() == 0.4f, "volume: in effect at once");
    std::string error;
    check(reg.apply_pending(gs::Category::Audio, &error) == gs::ApplyResult::Nothing, "volume: nothing left to apply");
    check(gekko::audio_output_gain() == 0.4f, "volume: engine gain changed");
    check(read_file(file("prov.cfg")).find("master_volume=40") != std::string::npos ||
              read_file(file("prov.cfg")).find("master_volume=40") != std::string::npos,
          "volume: saved");
    check(game.applies == 0, "volume: the game's apply is not run for a PC-only change");

    // Discard (B): pending dropped, nothing persisted.
    const std::string before_discard = read_file(file("prov.cfg"));
    reg.begin(gs::Category::Audio);
    reg.set("audio.subtitles", 4);
    reg.discard_pending(gs::Category::Audio);
    check(!reg.dirty(gs::Category::Audio) && find(reg.list(gs::Category::Audio), "audio.master_volume")->pending == 40,
          "discard: pending values dropped");
    check(gekko::audio_output_gain() == 0.4f && read_file(file("prov.cfg")) == before_discard && game.applies == 0,
          "discard: engine and file untouched");

    // A game option (subtitles -> German): the game receives it, options.cfg is written, then the "game" applies it.
    reg.begin(gs::Category::Audio);
    reg.set("audio.subtitles", 4);
    check(reg.apply_pending(gs::Category::Audio, &error) == gs::ApplyResult::Applied && game.applies == 1 &&
              game.q_lang == 4 && game.q_sp == 0 && game.q_voi == 1,
          "language: handed to the game's apply with the other options unchanged");
    game.frame();
    check(game.g_language == 4, "language: in effect after the game's state 3");
    check(read_file(file("prov.cfg")).find("language=4") != std::string::npos, "language: saved to options.cfg");
    check(find(reg.list(gs::Category::Audio), "audio.subtitles")->applied == 4, "language: the page shows what is in effect");

    // Regression: a page begun before another page's change must not take that change back (stale copies).
    reg.begin(gs::Category::Controls);          // copies volume 40, language 4
    reg.begin(gs::Category::Audio);
    reg.set("audio.master_volume", 90);
    reg.set("audio.speaker", 1);
    reg.apply_pending(gs::Category::Audio, &error);
    game.frame();
    check(gekko::audio_output_gain() == 0.9f && game.g_speaker == 1, "stale: audio change in effect");
    reg.set("controls.invert_camera_x", 1);     // the Controls copy still says volume 40, speaker 0
    reg.apply_pending(gs::Category::Controls, &error);
    game.frame();
    check(gekko::audio_output_gain() == 0.9f && game.g_speaker == 1 && game.g_language == 4 && g_inv_x,
          "stale: a Controls apply does not undo the Audio page's volume / speaker");
    check(gekko::pc_settings().current().master_volume == 90 && gekko::pc_settings().current().speaker == 1,
          "stale: options.cfg values keep the Audio page's change");

    // Restore defaults of one page: languages untouched; engine + file restored.
    reg.begin(gs::Category::Controls);
    reg.set("controls.vibration", 0);
    reg.set("controls.rumble_strength", 1);
    reg.apply_pending(gs::Category::Controls, &error);
    game.frame();
    check(!game.g_vibration && g_rumble == 50, "controls: vibration and strength changed");
    restore(reg, false, gs::Category::Controls);
    game.frame();
    check(game.g_vibration && g_rumble == 100 && !g_inv_x, "restore (Controls): engine state back to defaults");
    check(game.g_language == 4 && game.g_speaker == 1 && gekko::audio_output_gain() == 0.9f,
          "restore (Controls): other categories and the language untouched");
    const std::string cfg = read_file(file("prov.cfg"));
    check(cfg.find("rumble_strength=100") != std::string::npos && cfg.find("invert_camera_x=false") != std::string::npos &&
              cfg.find("vibration=true") != std::string::npos,
          "restore (Controls): the file holds the defaults");

    // Restore ALL defaults when the Controls / Save Data pages were last begun long before (stale) and several categories
    // change the game's options in one go before the game runs its state 3 (the in-flight merge).
    reg.begin(gs::Category::Controls);
    reg.begin(gs::Category::Gameplay);
    reg.set("controls.vibration", 0);
    reg.apply_pending(gs::Category::Controls, &error);
    reg.begin(gs::Category::Gameplay);
    reg.set("gameplay.autosave", 0);
    reg.apply_pending(gs::Category::Gameplay, &error);
    game.frame();
    reg.begin(gs::Category::Audio);
    reg.set("audio.master_volume", 10);
    reg.apply_pending(gs::Category::Audio, &error);
    check(!game.g_vibration && !game.g_autosave && gekko::audio_output_gain() == 0.1f, "setup for restore-all");
    // Make the Controls page's copy stale on purpose: it is never begun again before the restore.
    game.applies = 0;
    restore(reg, true, gs::Category::System);
    game.frame();
    check(game.g_vibration && game.g_autosave && game.g_speaker == 0, "restore all: vibration, autosave, speaker restored in the engine");
    check(gekko::audio_output_gain() == 1.0f && g_rumble == 100 && !g_inv_x && !g_inv_y, "restore all: PC values restored");
    check(game.g_language == 4 && game.g_voice == 1, "restore all: languages left alone");
    PcSettingsService reread;
    reread.load(file("prov.cfg"), nullptr);
    const PcSettings rc = reread.current();
    check(rc.master_volume == 100 && rc.rumble_strength == 100 && rc.speaker == 0 && rc.vibration == 1 && rc.autosave == 1 &&
              rc.language == 4 && rc.voice == 1,
          "restore all: the file holds the defaults and the untouched languages");
    check(!reg.dirty(gs::Category::Audio) && !reg.dirty(gs::Category::Controls) && !reg.dirty(gs::Category::Gameplay),
          "restore all: stable, nothing pending");
    // Second restore all: nothing to do, no game apply.
    game.applies = 0;
    restore(reg, true, gs::Category::System);
    check(game.applies == 0, "restore all twice: stable, the game is not asked again");

    // Without the Options screen: PC values still apply, game values refuse without changing the PC state.
    game.on = false;
    reg.begin(gs::Category::Controls);
    auto cv = reg.list(gs::Category::Controls);
    check(!find(cv, "controls.vibration")->available && find(cv, "controls.rumble_strength")->available,
          "no game: game rows unavailable, PC rows available");
    check(!reg.set("controls.vibration", 0), "no game: an unavailable row cannot be changed");
    game.on = true;
    gs::set_game_options(nullptr);

    // Invalid values through the setting API: clamped, no crash.
    reg.begin(gs::Category::Audio);
    reg.set("audio.master_volume", 9999);
    check(find(reg.list(gs::Category::Audio), "audio.master_volume")->pending == 100, "set: slider clamped to 100");
    reg.set("audio.master_volume", -50);
    check(find(reg.list(gs::Category::Audio), "audio.master_volume")->pending == 0, "set: slider clamped to 0");
    check(!reg.set("audio.nonexistent", 1) && !reg.set("nonsense", 1), "set: unknown ids refused");
    reg.discard_pending(gs::Category::Audio);
}

void test_rapid_switching() {
    // 300 rapid changes + applies + discards: the file and the engine end consistent, the file stays well-formed.
    write_file(file("rapid.cfg"), "# rapid\n");
    gekko::pc_settings().load(file("rapid.cfg"), nullptr);
    gs::Registry reg;
    reg.add(gs::make_audio_provider());
    reg.add(gs::make_controls_provider());
    FakeGame game;
    gs::set_game_options(&game);
    std::string error;
    unsigned seed = 12345u;
    const auto rnd = [&seed](unsigned n) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 16) % n;
    };
    for (int i = 0; i < 300; ++i) {
        reg.begin(gs::Category::Audio);
        reg.begin(gs::Category::Controls);
        reg.set("audio.master_volume", static_cast<int>(rnd(21)) * 5);
        reg.set("audio.subtitles", static_cast<int>(rnd(6)));
        reg.set("audio.speaker", static_cast<int>(rnd(3)));
        reg.set("controls.rumble_strength", static_cast<int>(rnd(4)));
        reg.set("controls.invert_camera_y", static_cast<int>(rnd(2)));
        if (rnd(3) == 0) {
            reg.discard_pending(gs::Category::Audio);
            reg.discard_pending(gs::Category::Controls);
        } else {
            reg.apply_pending(gs::Category::Audio, &error);
            if (rnd(2) == 0) game.frame();  // sometimes the next apply lands before the game's state 3
            reg.apply_pending(gs::Category::Controls, &error);
        }
        game.frame();
    }
    PcSettingsService again;
    again.load(file("rapid.cfg"), nullptr);
    const PcSettings now = gekko::pc_settings().current();
    check(again.current() == now, "rapid: the file always equals the state in effect");
    check(game.g_language == static_cast<unsigned>(now.language) && game.g_speaker == static_cast<unsigned>(now.speaker),
          "rapid: the game's values equal options.cfg's");
    gs::set_game_options(nullptr);
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
    test_providers();
    test_rapid_switching();
    std::filesystem::remove_all(g_dir, ec);
    std::printf("options_config_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
