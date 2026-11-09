// Settings-validation tests for the Options system (2026-11-05 validation effort, harness agent):
//   1. config_file     parse, line-preserving rewrite, aliases, CRLF / LF, appended keys, atomic update
//   2. PcSettingsService load (missing / corrupt / out of range / unknown keys), -1 "not set", save writes only the
//                      changed keys, round trip, thread safety of apply / current
//   3. game providers  settings_game.cpp over a fake settings::GameOptions: pending / applied / discard /
//                      reset_defaults / apply for every row, audio.voice without the Japanese files, System
//                      restore_all, Save Data autosave, failure paths, the menu's "restore all" order
//   4. registry        apply_pending results and the refusal of applies while a display trial runs
//   5. menu text table every setting id has a label and a help in all six languages, line lengths, English reference
//
// The audio gain and the XInput preferences (the owners PcSettingsService hands its values to) are replaced by
// recording stubs below. A check that fails prints FAIL; a KNOWN line is a defect or design limit that is reported
// but does not fail the run (see main()).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gekko/config_file.h"
#include "gekko/display_settings.h"
#include "gekko/native_options_display.h"
#include "gekko/pc_settings.h"
#include "gekko/settings_display.h"
#include "gekko/settings_game.h"
#include "gekko/settings_registry.h"
#include "gekko/voice_language.h"

// The menu's own text table (generated; included here as native_options_menu.cpp does).
#include "../src/gekko/options_menu_texts.inc"
#include "test_scratch.h"

namespace gekko {
// Recording stand-ins for the two owners pc_settings.cpp pushes to (audio_runtime.cpp, xinput_pad.cpp).
namespace {
std::atomic<float> g_gain{1.0f};
std::atomic<unsigned> g_rumble{100u};
std::atomic<bool> g_inv_x{false}, g_inv_y{false};
std::atomic<int> g_pushes{0};
}  // namespace
void set_audio_output_gain(float gain) noexcept {
    g_gain = gain;
    ++g_pushes;
}
void set_xinput_preferences(unsigned rumble_percent, bool invert_camera_x, bool invert_camera_y) noexcept {
    g_rumble = rumble_percent;
    g_inv_x = invert_camera_x;
    g_inv_y = invert_camera_y;
}
}  // namespace gekko

namespace {

namespace fs = std::filesystem;
namespace gs = gekko::settings;
namespace cf = gekko::config_file;
namespace od = gekko::options_display;
using gekko::PcSettings;
using gekko::PcSettingsService;

int g_failures = 0;
int g_checks = 0;
int g_known = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}
void checkf(bool condition, const std::string& what) { check(condition, what.c_str()); }

// A defect or limit that is real but reported, not failed (cosmetic, hand-edited-file only, or a design decision).
void known(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        std::printf("KNOWN: %s\n", what);
        ++g_known;
    }
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
void spit(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

fs::path g_dir;  // the test's scratch folder

const gs::SettingView* find(const std::vector<gs::SettingView>& views, const std::string& id) {
    for (const gs::SettingView& v : views)
        if (v.id == id) return &v;
    return nullptr;
}
gs::SettingView view(gs::Registry& r, gs::Category c, const std::string& id) {
    const std::vector<gs::SettingView> list = r.list(c);
    const gs::SettingView* v = find(list, id);
    return v != nullptr ? *v : gs::SettingView{};
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// ---------------------------------------------------------------------------------------------------------------
// 1. config_file
// ---------------------------------------------------------------------------------------------------------------

void config_parse() {
    std::vector<int> bad;
    const std::string text = "a=1\r\nB = 2 # note\n# only a comment\n\nbad line\n=5\nc==3\n   \nd=\nlast=7";
    const std::vector<cf::Entry> e = cf::parse(text, &bad);
    check(e.size() == 6u, "parse: six entries (a, b, '', c, d, last)");
    if (e.size() == 6u) {
        check(e[0].key == "a" && e[0].value == "1" && e[0].line == 1, "parse: CRLF stripped, line 1");
        check(e[1].key == "b" && e[1].value == "2" && e[1].line == 2, "parse: key lower-cased, comment removed");
        check(e[2].key.empty() && e[2].value == "5", "parse: an empty key is reported as an entry with an empty key");
        check(e[3].key == "c" && e[3].value == "=3", "parse: only the first '=' splits");
        check(e[4].key == "d" && e[4].value.empty(), "parse: empty value");
        check(e[5].key == "last" && e[5].value == "7" && e[5].line == 10, "parse: last line without newline");
    }
    check(bad.size() == 1u && bad[0] == 5, "parse: 'bad line' (line 5) reported; blank and comment lines are not");
    check(cf::parse("", nullptr).empty() && cf::parse("\n\n# x\n", nullptr).empty(), "parse: empty input");
    const std::vector<cf::Entry> dup = cf::parse("a=1\na=2\n", nullptr);
    check(dup.size() == 2u && dup[1].value == "2", "parse: duplicates are both returned, in order");
    const std::vector<cf::Entry> nul = cf::parse(std::string("a=1\0z\nb=2\n", 10), nullptr);
    check(nul.size() == 2u, "parse: an embedded NUL does not stop the parse");
}

void config_rewrite() {
    using KV = cf::KeyValue;
    check(cf::rewrite("a=1\r\nb=2\r\n", {KV{{"a"}, "9"}}) == "a=9\r\nb=2\r\n", "rewrite: CRLF kept");
    check(cf::rewrite("a=1\nb=2\n", {KV{{"b"}, "9"}}) == "a=1\nb=9\n", "rewrite: LF kept");
    check(cf::rewrite("Width = 5 # keep me\n", {KV{{"width"}, "7"}}) == "Width = 7 # keep me\n",
          "rewrite: spelling, spacing and trailing comment stay");
    check(cf::rewrite("a\t=\t1\t\n", {KV{{"a"}, "2"}}) == "a\t=\t2\t\n", "rewrite: tabs");
    check(cf::rewrite("a=\n", {KV{{"a"}, "9"}}) == "a=9\n", "rewrite: an empty value is filled");
    check(cf::rewrite("A=1\n", {KV{{"a"}, "2"}}) == "A=2\n", "rewrite: keys match case-insensitively");
    check(cf::rewrite("full_screen=0\n", {KV{{"fullscreen", "full_screen"}, "1"}}) == "full_screen=1\n",
          "rewrite: an alias spelling in the file is replaced in place");
    check(cf::rewrite("", {KV{{"fullscreen", "full_screen"}, "1"}}) == "fullscreen=1\n",
          "rewrite: a missing key is appended under the first spelling");
    check(cf::rewrite("a=1", {KV{{"b"}, "2"}}) == "a=1\nb=2\n", "rewrite: no trailing newline -> one is added (LF)");
    check(cf::rewrite("a=1\r\nb=2", {KV{{"c"}, "3"}}) == "a=1\r\nb=2\r\nc=3\r\n",
          "rewrite: CRLF file appends CRLF");
    check(cf::rewrite("a=1", {KV{{"a"}, "2"}}) == "a=2", "rewrite: in place at EOF without a newline adds none");
    check(cf::rewrite("# a=1\n", {KV{{"a"}, "2"}}) == "# a=1\na=2\n", "rewrite: a commented-out key is not touched");
    check(cf::rewrite("x=1 # a=5\n", {KV{{"a"}, "2"}}) == "x=1 # a=5\na=2\n", "rewrite: '=' after '#' is a comment");
    check(cf::rewrite("a=1\na=2\n", {KV{{"a"}, "9"}}) == "a=9\na=9\n", "rewrite: every line of a duplicated key");
    check(cf::rewrite("a=1\nfoo=bar\n# c\n\nb=2\n", {KV{{"b"}, "3"}}) == "a=1\nfoo=bar\n# c\n\nb=3\n",
          "rewrite: unknown keys, comments and blank lines are byte-identical");
    const std::string same = "a = 1 # c\r\nb=2\r\n";
    check(cf::rewrite(same, {KV{{"a"}, "1"}, KV{{"b"}, "2"}}) == same, "rewrite: unchanged values leave the text equal");
    const std::vector<KV> two = {KV{{"a"}, "5"}, KV{{"z"}, "6"}};
    const std::string once = cf::rewrite("a=1\n", two);
    check(once == "a=5\nz=6\n" && cf::rewrite(once, two) == once, "rewrite: idempotent");
    check(cf::rewrite("", {KV{{"a"}, "1"}, KV{{"b"}, "2"}}) == "a=1\nb=2\n", "rewrite: empty text, two appended keys");
    check(cf::rewrite("a=1\n", {}) == "a=1\n", "rewrite: no values -> unchanged");
    check(cf::rewrite("\r\n\r\na=1\r\n", {KV{{"a"}, "2"}}) == "\r\n\r\na=2\r\n", "rewrite: blank CRLF lines kept");
    known(cf::rewrite("a=1", {KV{std::vector<std::string>{}, "2"}}) == "a=1",
          "rewrite: an entry without spellings appends a newline to a file without one (no caller passes it)");
}

void config_update() {
    using KV = cf::KeyValue;
    const fs::path dir = g_dir / "cfg";
    fs::create_directories(dir);
    const fs::path f = dir / "t.cfg";
    std::string err;
    fs::remove(f);
    check(cf::update(f, {KV{{"a"}, "1"}}, &err) && slurp(f) == "a=1\n", "update: a missing file is created");
    check(!fs::exists(dir / "t.cfg.tmp"), "update: no temporary left");
    spit(f, "# head\r\na = 1 # x\r\nb=2\r\n");
    check(cf::update(f, {KV{{"b"}, "3"}}, &err) && slurp(f) == "# head\r\na = 1 # x\r\nb=3\r\n",
          "update: only the changed value differs, CRLF kept");
    // Nothing changes -> nothing is written: an old mtime stays.
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(2);
    fs::last_write_time(f, old);
    check(cf::update(f, {KV{{"b"}, "3"}}, &err), "update: unchanged -> true");
    check(fs::last_write_time(f) == old, "update: unchanged -> the file is not rewritten");
    // A parent folder that is missing: false, an error, no throw.
    err.clear();
    check(!cf::update(dir / "nope" / "x.cfg", {KV{{"a"}, "1"}}, &err) && !err.empty(),
          "update: a missing folder fails with an error");
    // The target is a directory: the rename fails, the old state stays, no temporary.
    fs::create_directories(dir / "asdir.cfg");
    err.clear();
    const bool ok = cf::update(dir / "asdir.cfg", {KV{{"a"}, "1"}}, &err);
    check(!ok, "update: a directory in place of the file fails");
    check(!fs::exists(dir / "asdir.cfg.tmp"), "update: ... and leaves no temporary");
    // Many updates in a row (the menu's rapid changes) keep the file valid and ordered.
    spit(f, "v=0\n");
    for (int i = 1; i <= 200; ++i) cf::update(f, {KV{{"v"}, std::to_string(i)}}, &err);
    check(slurp(f) == "v=200\n", "update: 200 successive updates");
    std::string text;
    check(cf::read(dir / "absent.cfg", text, &err) && text.empty(), "read: a missing file reads as empty");
}

// ---------------------------------------------------------------------------------------------------------------
// 2. PcSettingsService
// ---------------------------------------------------------------------------------------------------------------

struct Loaded {
    PcSettings s;
    std::vector<std::string> warnings;
};
Loaded load_text(const std::string& text, const char* name = "pc.cfg") {
    const fs::path p = g_dir / "pc" / name;
    spit(p, text);
    PcSettingsService svc;
    Loaded l;
    svc.load(p, &l.warnings);
    l.s = svc.current();
    return l;
}
bool is_default(const PcSettings& s) { return s == PcSettings{}; }

void pc_defaults_and_load() {
    {
        PcSettingsService svc;
        std::vector<std::string> w;
        svc.load(g_dir / "pc" / "missing.cfg", &w);
        check(svc.loaded() && is_default(svc.current()) && w.empty(), "pc load: a missing file = defaults, no warning");
        check(svc.current().language == -1 && svc.current().speaker == -1 && svc.current().vibration == -1 &&
                  svc.current().voice == -1 && svc.current().autosave == -1,
              "pc load: the game-owned fields are 'not set' (-1)");
        check(gekko::g_gain == 1.0f && gekko::g_rumble == 100u && !gekko::g_inv_x && !gekko::g_inv_y,
              "pc load: the defaults are pushed to the owners");
        check(svc.path() == g_dir / "pc" / "missing.cfg", "pc load: path remembered");
    }
    {
        const Loaded l = load_text(
            "master_volume = 55\nRUMBLE_STRENGTH=50 # half\ninvert_camera_x=on\ninvert_camera_y=0\nlanguage=3\n"
            "speaker=2\nvibration=false\nvoice=0\nautosave=no\n");
        check(l.warnings.empty(), "pc load: a valid file has no warnings");
        check(l.s.master_volume == 55 && l.s.rumble_strength == 50 && l.s.invert_camera_x && !l.s.invert_camera_y &&
                  l.s.language == 3 && l.s.speaker == 2 && l.s.vibration == 0 && l.s.voice == 0 && l.s.autosave == 0,
              "pc load: every key parsed (case-insensitive, bool spellings, comment)");
        check(gekko::g_gain == 0.55f && gekko::g_rumble == 50u && gekko::g_inv_x && !gekko::g_inv_y,
              "pc load: the owners receive 0.55 gain, 50 % rumble, invert X");
    }
    struct Bad {
        const char* text;
        const char* what;
    };
    const Bad bads[] = {
        {"master_volume=abc\n", "non-numeric"},         {"master_volume=\n", "empty value"},
        {"master_volume=101\n", "above range"},         {"master_volume=-1\n", "below range"},
        {"master_volume=50.5\n", "fraction"},           {"master_volume=5x\n", "trailing junk"},
        {"master_volume=99999999999999\n", "overflow"}, {"rumble_strength=24\n", "rumble 24"},
        {"rumble_strength=0\n", "rumble 0"},            {"rumble_strength=101\n", "rumble 101"},
        {"invert_camera_x=2\n", "bool 2"},              {"invert_camera_y=maybe\n", "bool maybe"},
        {"language=7\n", "language 7"},                {"language=-2\n", "language -2"},
        {"speaker=3\n", "speaker 3"},                   {"speaker=-5\n", "speaker -5"},
        {"vibration=2\n", "vibration 2"},               {"vibration=-1\n", "vibration -1"},
        {"voice=2\n", "voice 2"},                       {"autosave=maybe\n", "autosave maybe"},
        {"autosave=7\n", "autosave 7"},
    };
    for (const Bad& b : bads) {
        const Loaded l = load_text(b.text);
        checkf(is_default(l.s), std::string("pc load: bad value (") + b.what + ") -> defaults");
        checkf(l.warnings.size() == 1u && contains(l.warnings[0], "bad value"),
               std::string("pc load: bad value (") + b.what + ") -> one warning");
    }
    {
        const Loaded l = load_text("language=6\n");
        check(l.s.language == 6 && l.warnings.empty(), "pc load: language 6 (Hungarian, the English slot) is valid");
    }
    {
        const Loaded l = load_text("master_volume=999\nrumble_strength=60\nfoo=1\nnot a key value\n");
        check(l.s.master_volume == 100 && l.s.rumble_strength == 60, "pc load: a bad key does not spoil the next one");
        check(l.warnings.size() == 3u, "pc load: bad value + unknown key + malformed line = 3 warnings");
        bool unknown = false, malformed = false;
        for (const std::string& w : l.warnings) {
            unknown |= contains(w, "unknown key 'foo'");
            malformed |= contains(w, "not key = value");
        }
        check(unknown && malformed, "pc load: warnings name the unknown key and the malformed line");
    }
    {
        const Loaded l = load_text("master_volume=10\nmaster_volume=bad\n");
        check(l.s.master_volume == 10 && l.warnings.size() == 1u, "pc load: a later bad duplicate keeps the earlier value");
        check(load_text("master_volume=10\nmaster_volume=20\n").s.master_volume == 20, "pc load: the last duplicate wins");
        check(load_text("language=-1\n").s.language == -1, "pc load: language=-1 is 'not set'");
        check(load_text("master_volume=0\n").s.master_volume == 0 && gekko::g_gain == 0.0f, "pc load: volume 0 is valid");
        check(load_text("rumble_strength=25\n").s.rumble_strength == 25, "pc load: rumble 25 is valid (minimum)");
    }
    {
        std::string junk;
        for (int i = 0; i < 4096; ++i) junk.push_back(static_cast<char>((i * 131 + 7) & 0xFF));
        const Loaded g1 = load_text(junk, "junk.cfg");
        check(is_default(g1.s), "pc load: binary garbage -> defaults");
        std::string longline = "master_volume=" + std::string(1u << 20, '9') + "\n";
        const Loaded g2 = load_text(longline, "long.cfg");
        check(is_default(g2.s) && g2.warnings.size() == 1u, "pc load: a 1 MiB value -> default + warning");
        const Loaded g3 = load_text(std::string("master_volume=40\0rumble_strength=30\n", 36), "nul.cfg");
        check(g3.s.master_volume == 100 || g3.s.master_volume == 40, "pc load: an embedded NUL is survivable");
        const Loaded g4 = load_text("", "empty.cfg");
        check(is_default(g4.s) && g4.warnings.empty(), "pc load: an empty file = defaults");
        PcSettingsService svc;
        std::vector<std::string> w;
        fs::create_directories(g_dir / "pc" / "isdir.cfg");
        svc.load(g_dir / "pc" / "isdir.cfg", &w);
        check(is_default(svc.current()), "pc load: a directory in place of the file -> defaults");
    }
    {
        // A reload replaces everything (a warm restart in one process).
        PcSettingsService svc;
        spit(g_dir / "pc" / "a.cfg", "master_volume=30\nlanguage=2\n");
        svc.load(g_dir / "pc" / "a.cfg");
        spit(g_dir / "pc" / "b.cfg", "rumble_strength=70\n");
        svc.load(g_dir / "pc" / "b.cfg");
        check(svc.current().master_volume == 100 && svc.current().language == -1 && svc.current().rumble_strength == 70,
              "pc load: a second load starts from the defaults again");
    }
}

void pc_apply_save() {
    const fs::path p = g_dir / "pc" / "save.cfg";
    {
        PcSettingsService svc;
        std::string err;
        check(!svc.save(&err) && contains(err, "not loaded"), "pc save: before load -> false, 'not loaded'");
    }
    spit(p, "# my options\r\nfoo=bar\r\nmaster_volume = 20 # quiet\r\n");
    PcSettingsService svc;
    std::vector<std::string> w;
    svc.load(p, &w);
    check(w.size() == 1u && contains(w[0], "unknown key"), "pc save: setup - unknown key warned");
    check(svc.save() && slurp(p) == "# my options\r\nfoo=bar\r\nmaster_volume = 20 # quiet\r\n", "pc save: nothing changed -> byte-identical");
    PcSettings s = svc.current();
    s.rumble_strength = 50;
    check(svc.apply(s) && gekko::g_rumble == 50u, "pc apply: pushed to the owner at once");
    check(slurp(p).find("rumble") == std::string::npos, "pc apply: not saved by apply()");
    check(svc.save(), "pc save: ok");
    check(slurp(p) == "# my options\r\nfoo=bar\r\nmaster_volume = 20 # quiet\r\nrumble_strength=50\r\n",
          "pc save: only rumble_strength appended (CRLF), unknown key and comment kept");
    s.master_volume = 100;  // back to the default, but the file says 20: must be written
    check(svc.apply(s) && svc.save(), "pc save: master back to 100");
    check(slurp(p) == "# my options\r\nfoo=bar\r\nmaster_volume = 100 # quiet\r\nrumble_strength=50\r\n",
          "pc save: a value equal to the default but different from the file is written in place");
    // Game-owned: -1 is never written; a set value is.
    s.language = 4;
    s.vibration = 0;
    s.autosave = 1;
    s.invert_camera_y = true;
    check(svc.apply(s) && svc.save(), "pc save: game-owned fields");
    const std::string t = slurp(p);
    check(contains(t, "language=4\r\n") && contains(t, "vibration=false\r\n") && contains(t, "autosave=true\r\n") &&
              contains(t, "invert_camera_y=true\r\n"),
          "pc save: language, vibration (bool spelling), autosave, invert_camera_y written");
    check(!contains(t, "speaker") && !contains(t, "voice"), "pc save: speaker and voice (still -1) are not written");
    // Round trip.
    PcSettingsService again;
    std::vector<std::string> w2;
    again.load(p, &w2);
    check(again.current() == svc.current(), "pc round trip: load(save(x)) == x");
    // apply() invalid
    PcSettings bad = svc.current();
    const PcSettings before = svc.current();
    bad.master_volume = 101;
    std::string err;
    check(!svc.apply(bad, &err) && contains(err, "master_volume") && svc.current() == before, "pc apply: invalid refused, state unchanged");
    bad = before;
    bad.rumble_strength = 10;
    check(!svc.apply(bad, &err) && gekko::g_rumble == 50u, "pc apply: invalid rumble refused, owner not touched");
    bad = before;
    bad.voice = 5;
    check(!svc.apply(bad, &err), "pc apply: invalid voice refused");
    check(PcSettingsService::validate(PcSettings{}, nullptr), "pc validate: the defaults are valid");
    // Unsetting is not possible once written (language=-1 after a saved 4 writes nothing).
    PcSettings u = svc.current();
    u.language = -1;
    svc.apply(u);
    svc.save();
    known(slurp(p).find("language=4") == std::string::npos,
          "pc save: a game-owned field set back to -1 stays in the file (cannot be un-set; harmless, documented)");
    // A failing save keeps the change pending for the next try.
    const fs::path q = g_dir / "pc" / "gone" / "x.cfg";
    PcSettingsService svc2;
    spit(q, "master_volume=10\n");
    svc2.load(q);
    PcSettings m = svc2.current();
    m.master_volume = 77;
    svc2.apply(m);
    fs::remove_all(q.parent_path());
    err.clear();
    check(!svc2.save(&err) && !err.empty(), "pc save: an unwritable path fails with an error");
    fs::create_directories(q.parent_path());
    check(svc2.save() && contains(slurp(q), "master_volume=77"), "pc save: the retry after the failure writes the value");
    // Rapid changes: 500 apply+save cycles leave a loadable file with the last state.
    PcSettingsService svc3;
    const fs::path r = g_dir / "pc" / "rapid.cfg";
    spit(r, "");
    svc3.load(r);
    PcSettings cur;
    for (int i = 0; i < 500; ++i) {
        cur.master_volume = (i * 5) % 105 > 100 ? 100 : (i * 5) % 101;
        cur.rumble_strength = 25 + (i % 4) * 25;
        cur.invert_camera_x = (i & 1) != 0;
        cur.language = i % 6;
        cur.speaker = i % 3;
        cur.vibration = i & 1;
        cur.voice = (i >> 1) & 1;
        cur.autosave = (i >> 2) & 1;
        if (!svc3.apply(cur) || !svc3.save()) {
            check(false, "pc rapid: apply/save failed");
            break;
        }
    }
    PcSettingsService check3;
    std::vector<std::string> w3;
    check3.load(r, &w3);
    check(check3.current() == cur && w3.empty(), "pc rapid: 500 apply+save cycles -> the file holds the last state");
    check(!fs::exists(fs::path(r.string() + ".tmp")), "pc rapid: no temporary left");
}

void pc_threads() {
    PcSettingsService svc;
    svc.load(g_dir / "pc" / "threads.cfg");
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::thread reader([&] {
        while (!stop) {
            const PcSettings c = svc.current();
            // Writers only ever apply pairs (v, v): a torn read would see v != rumble-25-derived partner.
            if (c.master_volume != 100 && c.rumble_strength != 25 + (c.master_volume % 4) * 25) ++torn;
        }
    });
    std::vector<std::thread> writers;
    for (int t = 0; t < 3; ++t)
        writers.emplace_back([&, t] {
            for (int i = 0; i < 4000; ++i) {
                PcSettings s;
                s.master_volume = (i * 3 + t) % 100;
                s.rumble_strength = 25 + (s.master_volume % 4) * 25;
                svc.apply(s);
            }
        });
    for (std::thread& w : writers) w.join();
    stop = true;
    reader.join();
    check(torn == 0, "pc threads: concurrent apply/current never shows a torn PcSettings");
}

// ---------------------------------------------------------------------------------------------------------------
// 3. The game providers over a fake GameOptions
// ---------------------------------------------------------------------------------------------------------------

class FakeGame final : public gs::GameOptions {
public:
    bool up = true;
    unsigned spk = 0;
    bool vib = true;
    unsigned lang = 1, vc = 1;
    bool auto_save = true;
    int apply_calls = 0, autosave_calls = 0;
    bool fail_apply = false, fail_autosave = false;
    std::vector<std::string> runs;
    struct Call {
        unsigned speaker;
        bool vibration;
        unsigned language, voice;
    };
    std::vector<Call> calls;

    bool available() const override { return up; }
    unsigned speaker() const override { return spk; }
    bool vibration() const override { return vib; }
    unsigned language() const override { return lang; }
    unsigned voice() const override { return vc; }
    bool apply(unsigned s, bool v, unsigned l, unsigned vo, std::string* error) override {
        ++apply_calls;
        calls.push_back({s, v, l, vo});
        if (fail_apply) {
            if (error != nullptr) *error = "fake apply failed";
            return false;
        }
        spk = s, vib = v, lang = l, vc = vo;
        return true;
    }
    bool autosave() const override { return auto_save; }
    bool set_autosave(bool on, std::string* error) override {
        ++autosave_calls;
        if (fail_autosave) {
            if (error != nullptr) *error = "fake autosave failed";
            return false;
        }
        auto_save = on;
        return true;
    }
    bool run(std::string_view action, std::string*) override {
        runs.emplace_back(action);
        return true;
    }
};

struct Rig {
    FakeGame game;
    gs::Registry reg;
    fs::path cfg;
    explicit Rig(const std::string& name, const std::string& cfg_text, bool japanese = true) {
        cfg = g_dir / "game" / (name + ".cfg");
        spit(cfg, cfg_text);
        gekko::pc_settings().load(cfg);
        gekko::set_japanese_voices_available(japanese);
        gs::set_game_options(&game);
        reg.add(gs::make_audio_provider());
        reg.add(gs::make_controls_provider());
        reg.add(gs::make_gameplay_provider());
        reg.add(gs::make_system_provider("test build"));
    }
    ~Rig() { gs::set_game_options(nullptr); }
    std::string file() const { return slurp(cfg); }
    gs::SettingView v(gs::Category c, const char* id) { return view(reg, c, id); }
    // Change one setting on the open page and apply it as the menu's A does.
    gs::ApplyResult change(gs::Category c, const char* id, int value, std::string* err = nullptr) {
        reg.begin(c);
        reg.set(id, value);
        return reg.apply_pending(c, err);
    }
};

const char* const kFullCfg = "language=1\nspeaker=0\nvibration=true\nautosave=true\nvoice=1\n";

void audio_rows() {
    Rig rig("audio", kFullCfg);
    gs::Registry& r = rig.reg;
    r.begin(gs::Category::Audio);
    const std::vector<gs::SettingView> list = r.list(gs::Category::Audio);
    check(list.size() == 9u && list[0].id == "audio.master_volume" && list[1].id == "audio.music_volume" &&
              list[2].id == "audio.sfx_volume" && list[3].id == "audio.voice_volume" && list[4].id == "audio.speaker" &&
              list[5].id == "audio.voice" && list[6].id == "audio.subtitles" && list[7].id == "audio.subtitles_shown" &&
              list[8].id == "audio.sound_test",
          "audio: rows master/music/sfx/voice volume, speaker, voice, subtitles (Text Language), subtitles_shown, sound_test");
    bool all_available = true;
    for (const gs::SettingView& v : list) all_available &= v.available;
    check(all_available, "audio: every row available while the Options screen runs (Japanese files present)");
    check(!r.dirty(gs::Category::Audio), "audio: clean after begin");
    check(r.apply_pending(gs::Category::Audio) == gs::ApplyResult::Nothing, "audio: Nothing when nothing is pending");

    // master volume (PC side only)
    gs::SettingView mv = rig.v(gs::Category::Audio, "audio.master_volume");
    check(mv.kind == gs::Kind::Slider && mv.slider_min == 0 && mv.slider_max == 100 && mv.slider_step == 5 &&
              mv.applied == 100,
          "audio: master volume slider 0..100 step 5, default 100");
    check(r.step("audio.master_volume", -1) && rig.v(gs::Category::Audio, "audio.master_volume").pending == 95,
          "audio: a step moves by 5");
    check(r.set("audio.master_volume", -50) && rig.v(gs::Category::Audio, "audio.master_volume").pending == 0,
          "audio: set clamps below at 0");
    check(!r.set("audio.master_volume", -50), "audio: set to the same value reports no change");
    check(r.set("audio.master_volume", 1000) && rig.v(gs::Category::Audio, "audio.master_volume").pending == 100,
          "audio: set clamps above at 100");
    check(!r.step("audio.master_volume", 1), "audio: stepping past 100 reports no change");
    r.set("audio.master_volume", 35);
    check(!r.dirty(gs::Category::Audio), "audio: a volume slider applies as it moves, nothing pending");
    check(gekko::g_gain == 0.35f, "audio: the owner receives the moved volume at once");
    std::string err;
    check(r.apply_pending(gs::Category::Audio, &err) == gs::ApplyResult::Nothing, "audio: nothing left to apply after a slider");
    check(gekko::g_gain == 0.35f, "audio: the owner receives 0.35 (runtime state, not the label)");
    check(rig.game.apply_calls == 0, "audio: a PC-only change never runs the game's apply (no fade)");
    check(contains(rig.file(), "master_volume=35\n"), "audio: saved to options.cfg");
    check(rig.v(gs::Category::Audio, "audio.master_volume").applied == 35, "audio: applied value follows");

    // speaker
    check(rig.change(gs::Category::Audio, "audio.speaker", 1) == gs::ApplyResult::Applied, "audio: speaker -> monaural");
    check(rig.game.apply_calls == 1 && rig.game.calls.back().speaker == 1 && rig.game.spk == 1,
          "audio: the game's apply received speaker 1");
    check(rig.game.calls.back().vibration && rig.game.calls.back().language == 1 && rig.game.calls.back().voice == 1,
          "audio: ... with the other three game options passed through unchanged");
    check(contains(rig.file(), "speaker=1\n"), "audio: speaker saved");
    check(rig.change(gs::Category::Audio, "audio.speaker", 2) == gs::ApplyResult::Applied && rig.game.spk == 2 &&
              contains(rig.file(), "speaker=2\n"),
          "audio: speaker -> Dolby Pro Logic II, saved");
    check(!rig.reg.set("audio.speaker", 7) || rig.v(gs::Category::Audio, "audio.speaker").pending == 2,
          "audio: an out-of-range speaker clamps to the last choice");

    // voice (choices en, jp; the game's enum is 0 JP / 1 EN)
    gs::SettingView vv = rig.v(gs::Category::Audio, "audio.voice");
    check(vv.choices.size() == 2u && vv.choices[0].key == "en" && vv.choices[1].key == "jp" && vv.applied == 0,
          "audio: voice choices en | jp, English in effect");
    check(rig.change(gs::Category::Audio, "audio.voice", 1) == gs::ApplyResult::Applied && rig.game.vc == 0u,
          "audio: voice jp -> the game's voice 0 (Japanese)");
    check(contains(rig.file(), "voice=0\n") && rig.v(gs::Category::Audio, "audio.voice").applied == 1,
          "audio: voice=0 saved and shown as jp");
    check(rig.change(gs::Category::Audio, "audio.voice", 0) == gs::ApplyResult::Applied && rig.game.vc == 1u &&
              contains(rig.file(), "voice=1\n"),
          "audio: voice back to en -> game voice 1, saved");

    // subtitles: every language, with the other options intact
    static const char* const names[] = {"jp", "en", "fr", "sp", "ge", "it"};
    for (int l = 0; l < 6; ++l) {
        const int cur = static_cast<int>(rig.game.lang);
        const gs::ApplyResult res = rig.change(gs::Category::Audio, "audio.subtitles", l);
        checkf(res == (l == cur ? gs::ApplyResult::Nothing : gs::ApplyResult::Applied),
               std::string("audio: subtitles -> ") + names[l] + " result");
        checkf(static_cast<int>(rig.game.lang) == l && contains(rig.file(), "language=" + std::to_string(l) + "\n"),
               std::string("audio: subtitles ") + names[l] + " in effect and saved");
        checkf(rig.game.spk == 2u && rig.game.vc == 1u, "audio: speaker and voice survive a language change");
    }
    r.begin(gs::Category::Audio);
    check(!r.step("audio.subtitles", 1), "audio: stepping past Italian reports no change");
    r.set("audio.subtitles", -3);
    check(rig.v(gs::Category::Audio, "audio.subtitles").pending == 0, "audio: set(-3) clamps to Japanese");
    r.set("audio.subtitles", 99);
    check(rig.v(gs::Category::Audio, "audio.subtitles").pending == 5, "audio: set(99) clamps to Italian");
    check(rig.v(gs::Category::Audio, "audio.subtitles").choices[0].english == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
          "audio: the language names are the game's (Japanese in kanji)");

    // sound test is an Action
    gs::SettingView st = rig.v(gs::Category::Audio, "audio.sound_test");
    check(st.kind == gs::Kind::Action && !r.step("audio.sound_test", 1) && !r.set("audio.sound_test", 1),
          "audio: sound test is an action, not changeable");
    check(r.run("audio.sound_test") && rig.game.runs.size() == 1u && rig.game.runs[0] == "audio.sound_test",
          "audio: run reaches the game");

    // discard
    r.begin(gs::Category::Audio);
    r.set("audio.master_volume", 10);
    r.set("audio.speaker", 0);
    r.discard_pending(gs::Category::Audio);
    check(!r.dirty(gs::Category::Audio) && rig.v(gs::Category::Audio, "audio.master_volume").pending == 10 &&
              gekko::g_gain == 0.10f && rig.v(gs::Category::Audio, "audio.speaker").pending == 2,
          "audio: discard puts game options back; a moved volume is already in effect and stays");
    check(r.apply_pending(gs::Category::Audio) == gs::ApplyResult::Nothing, "audio: nothing to apply after discard");

    // reset_defaults: volume and speaker; the languages stay
    r.begin(gs::Category::Audio);
    r.set("audio.voice", 1);
    r.apply_pending(gs::Category::Audio);
    r.begin(gs::Category::Audio);
    r.reset_defaults(gs::Category::Audio);
    check(r.dirty(gs::Category::Audio) && gekko::g_gain == 0.10f, "audio: defaults are pending, not applied");
    check(r.apply_pending(gs::Category::Audio) == gs::ApplyResult::Applied, "audio: defaults applied");
    check(gekko::g_gain == 1.0f && rig.game.spk == 0u, "audio: master 100 %, stereo");
    check(rig.game.vc == 0u && rig.game.lang == 5u, "audio: Restore defaults leaves the voice and subtitle language alone");
    check(contains(rig.file(), "master_volume=100\n") && contains(rig.file(), "speaker=0\n"), "audio: defaults saved");
    r.begin(gs::Category::Audio);
    r.reset_defaults(gs::Category::Audio);
    check(!r.dirty(gs::Category::Audio) && r.apply_pending(gs::Category::Audio) == gs::ApplyResult::Nothing,
          "audio: restoring defaults twice is stable (Nothing)");
}

void audio_voice_unavailable_and_game_down() {
    {
        Rig rig("voice_nojp", kFullCfg, /*japanese=*/false);
        rig.reg.begin(gs::Category::Audio);
        const gs::SettingView v = rig.v(gs::Category::Audio, "audio.voice");
        check(!v.available && contains(v.reason, "Japanese"), "voice: disabled with a reason without the Japanese files");
        check(!rig.reg.set("audio.voice", 1) && !rig.reg.step("audio.voice", 1), "voice: cannot be changed while disabled");
        check(v.applied == 0 && v.pending == 0, "voice: shows English");
        check(rig.v(gs::Category::Audio, "audio.speaker").available, "voice: the other game rows stay available");
        // A game already on Japanese (options.cfg voice=0) but the files gone: the row shows English although the
        // game's G+0x5C says Japanese - what the user hears would follow G+0x5C.
        rig.game.vc = 0;
        rig.reg.begin(gs::Category::Audio);
        const gs::SettingView v2 = rig.v(gs::Category::Audio, "audio.voice");
        known(v2.applied == 1,
              "voice: Japanese in effect without the files is shown as jp (the row is disabled)");
    }
    {
        Rig rig("game_down", kFullCfg);
        rig.game.up = false;
        rig.reg.begin(gs::Category::Audio);
        const std::vector<gs::SettingView> l = rig.reg.list(gs::Category::Audio);
        check(find(l, "audio.master_volume")->available, "game down: master volume stays available");
        bool others_off = true;
        for (const char* id : {"audio.speaker", "audio.voice", "audio.subtitles", "audio.sound_test"}) {
            const gs::SettingView* v = find(l, id);
            others_off &= v != nullptr && !v->available && !v->reason.empty();
        }
        check(others_off, "game down: speaker, voice, subtitles, sound test disabled with a reason");
        check(!rig.reg.set("audio.speaker", 1) && !rig.reg.set("audio.subtitles", 3), "game down: no change possible");
        std::string err;
        check(!rig.reg.run("audio.sound_test", &err) && contains(err, "Options screen"), "game down: run refused");
        check(rig.change(gs::Category::Audio, "audio.master_volume", 20) == gs::ApplyResult::Nothing &&
                  rig.game.apply_calls == 0 && gekko::g_gain == 0.20f,
              "game down: the PC volume still applies, as the slider moves");
        // The game goes away between begin and apply.
        rig.game.up = true;
        rig.reg.begin(gs::Category::Audio);
        rig.reg.set("audio.speaker", 2);
        rig.game.up = false;
        err.clear();
        check(rig.reg.apply_pending(gs::Category::Audio, &err) == gs::ApplyResult::Failed && !err.empty(),
              "game down: apply with a pending game option fails with an error");
        check(rig.game.apply_calls == 0 && contains(rig.file(), "master_volume=20"), "game down: no game call, PC part kept");
    }
}

void game_failure_paths() {
    Rig rig("fail", kFullCfg);
    gs::Registry& r = rig.reg;
    r.begin(gs::Category::Audio);
    r.set("audio.master_volume", 60);
    r.set("audio.speaker", 1);
    rig.game.fail_apply = true;
    std::string err;
    check(r.apply_pending(gs::Category::Audio, &err) == gs::ApplyResult::Failed && contains(err, "fake apply failed"),
          "failure: the game's refusal is reported");
    check(gekko::g_gain == 0.60f, "failure: the PC part was applied before the game part failed (in effect)");
    check(contains(rig.file(), "master_volume=60\n"), "failure: the live volume was saved as it moved");
    check(!contains(rig.file(), "speaker=1\n"), "failure: the failed game option is not saved");
    check(r.dirty(gs::Category::Audio) && rig.v(gs::Category::Audio, "audio.speaker").pending == 1 &&
              rig.v(gs::Category::Audio, "audio.speaker").applied == 0,
          "failure: the game option stays pending, not applied");
    rig.game.fail_apply = false;
    err.clear();
    check(r.apply_pending(gs::Category::Audio, &err) == gs::ApplyResult::Applied && rig.game.spk == 1u,
          "failure: the retry applies the game option");
    check(contains(rig.file(), "master_volume=60\n") && contains(rig.file(), "speaker=1\n"),
          "failure: the retry saves both parts");
    // autosave switch fails
    r.begin(gs::Category::Gameplay);
    r.set("gameplay.autosave", 0);
    rig.game.fail_autosave = true;
    check(r.apply_pending(gs::Category::Gameplay, &err) == gs::ApplyResult::Failed && rig.game.auto_save,
          "failure: autosave refusal leaves the switch on");
    rig.game.fail_autosave = false;
    check(r.apply_pending(gs::Category::Gameplay, &err) == gs::ApplyResult::Applied && !rig.game.auto_save,
          "failure: the autosave retry works");
    // Saving fails (folder gone) after an Applied apply: Failed, but in effect.
    r.begin(gs::Category::Controls);
    r.set("controls.rumble_strength", 1);
    fs::remove_all(rig.cfg.parent_path() / "gone");
    PcSettings s = gekko::pc_settings().current();
    // point the service at an unwritable path: reload from a file in a folder we then remove
    const fs::path gone = rig.cfg.parent_path() / "gone" / "o.cfg";
    spit(gone, "");
    gekko::pc_settings().load(gone);
    fs::remove_all(gone.parent_path());
    r.begin(gs::Category::Controls);
    r.set("controls.rumble_strength", 1);
    err.clear();
    check(r.apply_pending(gs::Category::Controls, &err) == gs::ApplyResult::Failed && !err.empty() &&
              gekko::g_rumble == 50u,
          "failure: a save error is Failed, the change stays in effect (50 %)");
    known(r.apply_pending(gs::Category::Controls, &err) != gs::ApplyResult::Nothing,
          "failure: pressing A again after a failed save does not retry it (apply_pending returns Nothing; the next "
          "successful save of any change writes it)");
    (void)s;
}

void controls_rows() {
    Rig rig("controls", kFullCfg);
    gs::Registry& r = rig.reg;
    r.begin(gs::Category::Controls);
    const std::vector<gs::SettingView> list = r.list(gs::Category::Controls);
    check(list.size() == 4u && list[0].id == "controls.vibration" && list[1].id == "controls.rumble_strength" &&
              list[2].id == "controls.invert_camera_x" && list[3].id == "controls.invert_camera_y",
          "controls: rows vibration, rumble_strength, invert_camera_x, invert_camera_y");
    gs::SettingView rs = rig.v(gs::Category::Controls, "controls.rumble_strength");
    check(rs.choices.size() == 4u && rs.choices[0].key == "25" && rs.choices[3].key == "100" && rs.applied == 3,
          "controls: rumble 25 | 50 | 75 | 100, 100 in effect");
    // vibration (game)
    check(rig.change(gs::Category::Controls, "controls.vibration", 0) == gs::ApplyResult::Applied && !rig.game.vib &&
              rig.game.calls.back().vibration == false,
          "controls: vibration off reaches the game");
    check(contains(rig.file(), "vibration=false\n"), "controls: vibration saved as false");
    check(rig.change(gs::Category::Controls, "controls.vibration", 1) == gs::ApplyResult::Applied && rig.game.vib &&
              contains(rig.file(), "vibration=true\n"),
          "controls: vibration back on");
    // rumble strength (PC)
    for (int i = 0; i < 4; ++i) {
        checkf(rig.change(gs::Category::Controls, "controls.rumble_strength", i) !=
                   gs::ApplyResult::Failed,
               "controls: rumble level apply");
        const unsigned want = 25u + 25u * static_cast<unsigned>(i);
        checkf(gekko::g_rumble == want && contains(rig.file(), "rumble_strength=" + std::to_string(want) + "\n"),
               "controls: rumble " + std::to_string(want) + " % in effect and saved");
    }
    r.begin(gs::Category::Controls);
    check(!r.step("controls.rumble_strength", 1), "controls: no level above 100 %");
    r.set("controls.rumble_strength", -4);
    check(rig.v(gs::Category::Controls, "controls.rumble_strength").pending == 0, "controls: set clamps to 25 %");
    r.discard_pending(gs::Category::Controls);
    // invert
    check(rig.change(gs::Category::Controls, "controls.invert_camera_x", 1) == gs::ApplyResult::Applied && gekko::g_inv_x &&
              !gekko::g_inv_y && contains(rig.file(), "invert_camera_x=true\n"),
          "controls: invert X in effect and saved");
    check(rig.change(gs::Category::Controls, "controls.invert_camera_y", 1) == gs::ApplyResult::Applied && gekko::g_inv_x &&
              gekko::g_inv_y,
          "controls: invert Y keeps X");
    check(rig.game.apply_calls == 2, "controls: PC-only changes never ran the game's apply (2 vibration changes only)");
    // Several rows pending at once, one apply.
    r.begin(gs::Category::Controls);
    r.set("controls.rumble_strength", 1);
    r.set("controls.invert_camera_x", 0);
    r.set("controls.vibration", 0);
    check(r.apply_pending(gs::Category::Controls) == gs::ApplyResult::Applied, "controls: one apply for three rows");
    check(gekko::g_rumble == 50u && !gekko::g_inv_x && gekko::g_inv_y && !rig.game.vib, "controls: all three in effect");
    // defaults
    r.begin(gs::Category::Controls);
    r.reset_defaults(gs::Category::Controls);
    check(r.apply_pending(gs::Category::Controls) == gs::ApplyResult::Applied, "controls: defaults applied");
    check(gekko::g_rumble == 100u && !gekko::g_inv_x && !gekko::g_inv_y && rig.game.vib,
          "controls: 100 %, no inversion, vibration on");
    r.begin(gs::Category::Controls);
    r.reset_defaults(gs::Category::Controls);
    check(r.apply_pending(gs::Category::Controls) == gs::ApplyResult::Nothing, "controls: defaults twice -> Nothing");
    // an options.cfg rumble value that is not one of the four levels
    Rig odd("controls_odd", "rumble_strength=30\n");
    odd.reg.begin(gs::Category::Controls);
    const gs::SettingView ov = odd.v(gs::Category::Controls, "controls.rumble_strength");
    known(gekko::g_rumble != 30u || ov.applied != 3,
          "controls: rumble_strength=30 in options.cfg is in effect (30 %) but the row shows 100 %");
}

void savedata_and_system() {
    Rig rig("savedata", "autosave=false\n");
    gs::Registry& r = rig.reg;
    rig.game.auto_save = false;
    r.begin(gs::Category::Gameplay);
    const std::vector<gs::SettingView> list = r.list(gs::Category::Gameplay);
    check(list.size() == 2u && list[0].id == "gameplay.load_game" && list[1].id == "gameplay.autosave",
          "save data: rows load_game, autosave");
    check(list[0].kind == gs::Kind::Action && list[1].kind == gs::Kind::Toggle && list[1].applied == 0,
          "save data: Load is an action, autosave a toggle (off in effect)");
    check(rig.change(gs::Category::Gameplay, "gameplay.autosave", 1) == gs::ApplyResult::Applied && rig.game.auto_save &&
              rig.game.autosave_calls == 1,
          "save data: autosave on reaches the game's switch");
    check(rig.game.apply_calls == 0, "save data: an autosave change never starts the game's fade / apply (state 3)");
    check(contains(rig.file(), "autosave=true\n"), "save data: autosave=true saved");
    check(rig.change(gs::Category::Gameplay, "gameplay.autosave", 0) == gs::ApplyResult::Applied && !rig.game.auto_save &&
              contains(rig.file(), "autosave=false\n"),
          "save data: autosave off saved");
    check(rig.change(gs::Category::Gameplay, "gameplay.autosave", 0) == gs::ApplyResult::Nothing,
          "save data: setting the value already in effect -> Nothing");
    r.begin(gs::Category::Gameplay);
    r.reset_defaults(gs::Category::Gameplay);
    check(r.apply_pending(gs::Category::Gameplay) == gs::ApplyResult::Applied && rig.game.auto_save,
          "save data: Restore defaults -> autosave on (the game's default)");
    check(r.run("gameplay.load_game") && !rig.game.runs.empty() && rig.game.runs.back() == "gameplay.load_game",
          "save data: Load Game Data runs the game's command");
    check(!r.step("gameplay.load_game", 1), "save data: the action is not steppable");
    rig.game.up = false;
    r.begin(gs::Category::Gameplay);
    std::string err;
    check(!r.run("gameplay.load_game", &err) && !err.empty() && !rig.v(gs::Category::Gameplay, "gameplay.autosave").available,
          "save data: without the Options screen both rows are unavailable and Load is refused");
    rig.game.up = true;

    // System
    r.begin(gs::Category::System);
    const std::vector<gs::SettingView> sys = r.list(gs::Category::System);
    check(sys.size() == 2u && sys[0].id == "system.restore_all" && sys[1].id == "system.build",
          "system: rows restore_all, build");
    check(sys[0].kind == gs::Kind::Action && sys[0].available, "system: restore_all is an available action");
    check(!sys[1].available && sys[1].choices.size() == 1u && sys[1].choices[0].english == "test build",
          "system: build is information (shown, unavailable) with the build text");
    check(!r.step("system.build", 1) && !r.set("system.build", 0), "system: build cannot be changed");
    err.clear();
    check(!r.run("system.restore_all", &err) && contains(err, "menu"),
          "system: restore_all is run by the menu, the provider refuses (no silent no-op)");
    check(!r.dirty(gs::Category::System) && r.apply_pending(gs::Category::System) == gs::ApplyResult::Nothing,
          "system: never dirty");
    r.reset_defaults(gs::Category::System);
    check(r.apply_pending(gs::Category::System) == gs::ApplyResult::Nothing, "system: reset_defaults is a no-op");
    check(!r.run("audio.sound_test", &err) || rig.game.runs.back() == "audio.sound_test",
          "system: routing of a foreign action id goes to its own category");
}

// The menu's "restore all" (native_options_menu.cpp restore_defaults(every = true)): per category reset_defaults,
// apply_pending, begin - in this order, without a begin first.
void restore_all_like_the_menu(gs::Registry& r) {
    static constexpr gs::Category kOrder[] = {gs::Category::Graphics, gs::Category::Audio, gs::Category::Controls,
                                              gs::Category::Gameplay, gs::Category::System, gs::Category::Display};
    for (const gs::Category c : kOrder) {
        if (!r.has(c)) continue;
        r.reset_defaults(c);
        r.apply_pending(c);
        if (!r.trial().active) r.begin(c);
    }
}

void restore_all_flows() {
    // (a) plain: every non-default value back to its default; the languages stay.
    {
        Rig rig("restore_a", "master_volume=30\nrumble_strength=50\ninvert_camera_x=true\ninvert_camera_y=true\n"
                             "language=3\nspeaker=2\nvibration=false\nvoice=0\nautosave=false\n");
        rig.game.spk = 2, rig.game.vib = false, rig.game.lang = 3, rig.game.vc = 0, rig.game.auto_save = false;
        for (const gs::Category c : {gs::Category::Audio, gs::Category::Controls, gs::Category::Gameplay, gs::Category::System})
            rig.reg.begin(c);
        restore_all_like_the_menu(rig.reg);
        check(gekko::g_gain == 1.0f && gekko::g_rumble == 100u && !gekko::g_inv_x && !gekko::g_inv_y,
              "restore all (fresh pages): volume, rumble, inversion back to the defaults");
        check(rig.game.spk == 0u && rig.game.vib && rig.game.auto_save, "restore all: speaker, vibration, autosave defaults");
        check(rig.game.lang == 3u && rig.game.vc == 0u, "restore all: subtitle and voice language kept");
        const std::string f = rig.file();
        check(contains(f, "master_volume=100\n") && contains(f, "rumble_strength=100\n") && contains(f, "speaker=0\n") &&
                  contains(f, "autosave=true\n") && contains(f, "language=3\n") && contains(f, "voice=0\n"),
              "restore all: the file holds the defaults and the kept languages");
    }
    // (b) the menu does not begin() a page it did not visit since another category changed the shared PC settings:
    //     Controls was last opened at volume 40; Audio then sets 100; Restore all must end at 100.
    {
        Rig rig("restore_b", "master_volume=40\nrumble_strength=50\n");
        rig.reg.begin(gs::Category::Controls);  // the page was opened (and left) at volume 40
        rig.change(gs::Category::Audio, "audio.master_volume", 100);
        check(gekko::g_gain == 1.0f, "restore all (b): setup - volume 100 in effect");
        restore_all_like_the_menu(rig.reg);
        check(gekko::g_gain == 1.0f, "restore all (b): the volume stays 100 (a stale Controls snapshot must not undo Audio)");
        check(!contains(rig.file(), "master_volume=40"), "restore all (b): the file does not go back to volume 40");
    }
    // (c) the same for the game-owned values persisted by Save Data: Audio changes the language, Save Data's page
    //     was opened before; Restore all (autosave off -> on) must not persist the old language.
    {
        Rig rig("restore_c", "language=1\nautosave=false\n");
        rig.game.auto_save = false;
        rig.reg.begin(gs::Category::Gameplay);  // opened at language 1
        rig.change(gs::Category::Audio, "audio.subtitles", 2);  // French
        check(rig.game.lang == 2u && contains(rig.file(), "language=2\n"), "restore all (c): setup - French in effect and saved");
        restore_all_like_the_menu(rig.reg);
        check(rig.game.lang == 2u, "restore all (c): the game is still French");
        check(contains(rig.file(), "language=2\n") && !contains(rig.file(), "language=1"),
              "restore all (c): options.cfg still says French (a stale Save Data snapshot must not persist English)");
    }
    // (d) a menu page change the other way: Audio and Controls each apply, the second does not undo the first.
    {
        Rig rig("restore_d", kFullCfg);
        rig.reg.begin(gs::Category::Audio);
        rig.reg.begin(gs::Category::Controls);
        rig.reg.set("audio.master_volume", 10);
        rig.reg.apply_pending(gs::Category::Audio);
        rig.reg.set("controls.rumble_strength", 0);
        rig.reg.apply_pending(gs::Category::Controls);
        check(gekko::g_gain == 0.10f && gekko::g_rumble == 25u,
              "two pages opened together: applying Controls after Audio keeps Audio's volume (no stale snapshot)");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 4. Registry interaction with a display trial
// ---------------------------------------------------------------------------------------------------------------

gs::Registry::Clock::time_point g_now{};
gs::Registry::Clock::time_point fake_now() { return g_now; }

void display_setup() {
    od::set_screen_size(1920, 1080);
    od::set_msaa_supported(0x0Fu);
    const fs::path cfg = g_dir / "display.cfg";
    spit(cfg, "# test display.cfg\nwidth=1280\nheight=720\nfullscreen=false\n");
    gekko::DisplaySettings start;
    start.window_width = 1280;
    start.window_height = 720;
    start.msaa = 4;
    start.anisotropy = 16;
    gekko::display_settings().initialize(start, cfg);
}

void trial_interaction() {
    display_setup();
    Rig rig("trial", kFullCfg);
    gs::Registry& r = rig.reg;
    r.set_clock(fake_now);
    r.add(gs::make_display_provider());
    r.add(gs::make_graphics_provider());

    r.begin(gs::Category::Display);
    r.set("display.mode", 1);  // fullscreen
    check(r.apply_pending(gs::Category::Display) == gs::ApplyResult::TrialStarted && r.trial().active,
          "trial: a mode change starts the 15 s trial");

    // Every other category refuses its apply while the trial runs - and keeps its pending values.
    r.begin(gs::Category::Audio);
    r.set("audio.speaker", 1);
    std::string err;
    check(r.apply_pending(gs::Category::Audio, &err) == gs::ApplyResult::Failed && contains(err, "waiting for confirmation"),
          "trial: Audio apply refused while the trial runs");
    check(gekko::g_gain == 1.0f && r.dirty(gs::Category::Audio), "trial: ... nothing applied, the pending value kept");
    r.begin(gs::Category::Controls);
    r.set("controls.invert_camera_x", 1);
    check(r.apply_pending(gs::Category::Controls, &err) == gs::ApplyResult::Failed && !gekko::g_inv_x,
          "trial: Controls apply refused as well");
    r.begin(gs::Category::Gameplay);
    r.set("gameplay.autosave", 0);
    check(r.apply_pending(gs::Category::Gameplay, &err) == gs::ApplyResult::Failed && rig.game.auto_save,
          "trial: Save Data apply refused as well");
    check(r.report().apply_failures >= 3u, "trial: the registry counts the refusals");
    // Reading and stepping still work (the page stays usable).
    r.begin(gs::Category::Audio);
    check(r.step("audio.speaker", 1) && r.dirty(gs::Category::Audio), "trial: pending values can still change");

    // Confirm ends the trial; the earlier refusals now apply.
    g_now += std::chrono::seconds(3);
    check(r.confirm(&err) && !r.trial().active, "trial: confirm");
    r.begin(gs::Category::Audio);
    r.set("audio.speaker", 2);
    check(r.apply_pending(gs::Category::Audio) == gs::ApplyResult::Applied && rig.game.spk == 2u,
          "trial: Audio applies after the confirm");
    r.set("audio.master_volume", 45);
    check(gekko::g_gain == 0.45f && !r.dirty(gs::Category::Audio), "trial: the live volume works after the confirm");
    // Restore-all while a trial is pending, as the menu runs it, leaves nothing half-applied.
    r.begin(gs::Category::Display);
    r.set("display.mode", 0);
    const gs::ApplyResult dr = r.apply_pending(gs::Category::Display);
    check(dr == gs::ApplyResult::TrialStarted || dr == gs::ApplyResult::Applied, "trial: second display change");
    if (r.trial().active) {
        g_now += std::chrono::seconds(16);
        r.tick();
        check(!r.trial().active, "trial: expiry rolls back via tick()");
    }
    r.begin(gs::Category::Audio);
    r.reset_defaults(gs::Category::Audio);
    check(r.apply_pending(gs::Category::Audio) == gs::ApplyResult::Applied && gekko::g_gain == 1.0f,
          "trial: after the expiry Audio defaults apply");
}

// ---------------------------------------------------------------------------------------------------------------
// 5. The menu text table
// ---------------------------------------------------------------------------------------------------------------

// Code points of a UTF-8 string; -1 when it is not well formed.
int utf8_len(const std::string& s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
        if (extra == 99 || i + extra >= s.size() + (extra == 0 ? 1 : 0)) return -1;
        for (std::size_t k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return -1;
        i += extra + 1;
        ++n;
    }
    return n;
}

const MenuText* find_text(const std::string& key) {
    for (const MenuText& t : kMenuTexts)
        if (key == t.key) return &t;
    return nullptr;
}

void menu_texts() {
    static const char* const kLang[6] = {"EN", "FR", "GE", "IT", "JP", "SP"};
    // Every id the registry lists: the game providers (Rig) and the display / graphics providers.
    display_setup();
    Rig rig("texts", kFullCfg);
    rig.reg.add(gs::make_display_provider());
    rig.reg.add(gs::make_graphics_provider());
    std::vector<gs::SettingView> all;
    for (unsigned c = 0; c < gs::kCategoryCount; ++c) {
        const gs::Category cat = static_cast<gs::Category>(c);
        rig.reg.begin(cat);
        for (const gs::SettingView& v : rig.reg.list(cat)) all.push_back(v);
    }
    check(all.size() >= 20u, "texts: the providers list at least 20 settings in total");
    std::set<std::string> keys;
    for (const gs::SettingView& v : all) {
        keys.insert(v.id + ".label");
        keys.insert(v.id + ".help");
    }
    // The per-category defaults rows, the category tiles and the shared words the menu uses.
    for (const char* p : {"display", "graphics", "audio", "controls", "gameplay"}) keys.insert(std::string(p) + ".defaults.help");
    for (const char* n : {"display", "graphics", "audio", "controls", "savedata", "system"})
        keys.insert(std::string("category.") + n + ".help");
    for (const char* k : {"menu.defaults.label", "menu.confirm.help", "menu.sound_test.help", "word.auto", "word.borderless",
                          "word.exclusive", "word.windowed", "word.confirm.keep", "word.confirm.question",
                          "word.confirm.return", "word.confirm.returning"})
        keys.insert(k);
    int missing = 0;
    for (const std::string& key : keys) {
        const MenuText* t = find_text(key);
        if (t == nullptr) {
            // Some shared words may legitimately live only in code; the setting ids and categories may not.
            const bool essential = key.rfind("word.", 0) != 0 && key != "menu.confirm.help";
            checkf(!essential, "texts: key present in the table: " + key);
            ++missing;
            continue;
        }
        const bool help = key.size() > 5 && key.compare(key.size() - 5, 5, ".help") == 0;
        for (int l = 0; l < 6; ++l) {
            const char* s = t->text[l];
            if (s == nullptr || *s == '\0') {
                checkf(false, std::string("texts: ") + key + " has a " + kLang[l] + " text");
                continue;
            }
            const std::string text = s;
            checkf(utf8_len(text) >= 0, std::string("texts: ") + key + " " + kLang[l] + " is well-formed UTF-8");
            // word.confirm.returning is the one printf template (the countdown, "%d" = seconds): exactly one %d.
            bool percent_ok = !contains(text, "%");
            if (key == "word.confirm.returning") {
                const std::size_t at = text.find('%');
                percent_ok = at != std::string::npos && text.compare(at, 2, "%d") == 0 &&
                             text.find('%', at + 1) == std::string::npos;
            }
            checkf(!contains(text, "\r") && !contains(text, "{") && percent_ok,
                   std::string("texts: ") + key + " " + kLang[l] + " has no CR / format artifacts");
            if (help) {
                const std::size_t limit = l == 4 ? 20u : 44u;
                std::size_t begin = 0;
                int lines = 0;
                while (begin <= text.size()) {
                    std::size_t end = text.find('\n', begin);
                    if (end == std::string::npos) end = text.size();
                    const std::string line = text.substr(begin, end - begin);
                    ++lines;
                    checkf(static_cast<std::size_t>(std::max(utf8_len(line), 0)) <= limit,
                           std::string("texts: ") + key + " " + kLang[l] + " line " + std::to_string(lines) + " <= " +
                               std::to_string(limit) + " characters");
                    begin = end + 1;
                }
                checkf(lines <= 2, std::string("texts: ") + key + " " + kLang[l] + " has at most two lines");
            }
        }
    }
    // The English column is the reference wording the providers carry (the menu's fallback).
    for (const gs::SettingView& v : all) {
        const MenuText* lab = find_text(v.id + ".label");
        const MenuText* hlp = find_text(v.id + ".help");
        if (lab != nullptr && !v.english_label.empty())
            checkf(v.english_label == lab->text[0], "texts: EN label of " + v.id + " equals the provider's english_label");
        if (hlp != nullptr && !v.english_help.empty())
            checkf(v.english_help == hlp->text[0], "texts: EN help of " + v.id + " equals the provider's english_help");
    }
    // Keys are unique and sorted-lookup safe.
    std::set<std::string> seen;
    bool unique = true;
    for (const MenuText& t : kMenuTexts) unique &= seen.insert(t.key).second;
    check(unique, "texts: no duplicate keys in the table");
    (void)missing;
}

}  // namespace

int main() {
    g_dir = gekko::test_support::scratch_directory("options_settings_tests");
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir);

    config_parse();
    config_rewrite();
    config_update();
    pc_defaults_and_load();
    pc_apply_save();
    pc_threads();
    audio_rows();
    audio_voice_unavailable_and_game_down();
    game_failure_paths();
    controls_rows();
    savedata_and_system();
    restore_all_flows();
    trial_interaction();
    menu_texts();

    fs::remove_all(g_dir, ec);
    std::printf("options_settings_tests: %d checks, %d failure(s), %d known issue(s)\n", g_checks, g_failures, g_known);
    return g_failures == 0 ? 0 : 1;
}
