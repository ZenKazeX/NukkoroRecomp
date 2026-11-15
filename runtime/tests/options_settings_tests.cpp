// Settings-validation tests for the PC settings file (options.cfg, docs/PC_SETTINGS_FILES.md):
//   1. config_file     parse, line-preserving rewrite, aliases, CRLF / LF, appended keys, atomic update
//   2. PcSettingsService load (missing / corrupt / out of range / unknown keys), -1 "not set", the retired language 6
//                      (the Hungarian of earlier builds) read as English with a warning, save writes only the changed
//                      keys, round trip, thread safety of apply / current
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
#include "gekko/pc_settings.h"
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
namespace cf = gekko::config_file;
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
        // The Hungarian text of earlier builds ran on the game's English: a file that still says 6 loads as English,
        // with one warning naming it, and the other keys of the file are kept.
        const Loaded l = load_text("language=6\nspeaker=2\n");
        check(l.s.language == 1 && l.s.speaker == 2, "pc load: language 6 (retired Hungarian) reads as English (1)");
        check(l.warnings.size() == 1u && contains(l.warnings[0], "language 6") && contains(l.warnings[0], "English"),
              "pc load: language 6 -> one warning naming the fallback");
        PcSettings six;
        six.language = 6;
        std::string error;
        check(!PcSettingsService::validate(six, &error), "pc validate: language 6 is no longer a value to apply");
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

    fs::remove_all(g_dir, ec);
    std::printf("options_settings_tests: %d checks, %d failure(s), %d known issue(s)\n", g_checks, g_failures, g_known);
    return g_failures == 0 ? 0 : 1;
}
