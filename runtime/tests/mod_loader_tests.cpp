// Tests for the mod loader library (shadow/mods/*.h): JSON, versions, manifests, state, resolution, the plan,
// AFS rebuilding, the zip reader and the installer's placement rules. Everything runs on files made here in a
// temporary directory; the real-mod checks are in docs/mods/TESTING.md (they need the downloads and the game).

#include "shadow/mods/install.h"
#include "shadow/mods/json.h"
#include "shadow/mods/mods.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505 4244 4100 4456 4457 4996)
#endif
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace shadow::mods;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

void write_file(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path make_temp() {
    std::random_device rd;
    const fs::path p = fs::temp_directory_path() / ("shadow_mods_tests_" + std::to_string(rd()));
    fs::create_directories(p);
    return p;
}

void put32(std::string& s, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s[at + i] = static_cast<char>(v >> (8 * i));
}
std::uint32_t get32(const std::string& s, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + i])) << (8 * i);
    return v;
}

// An AFS like the disc's: names table after the data, every offset aligned to 2048.
std::string make_afs(const std::vector<std::pair<std::string, std::string>>& entries) {
    const std::size_t n = entries.size();
    std::size_t first = 2048;
    std::vector<std::size_t> offsets;
    std::size_t at = first;
    for (const auto& e : entries) {
        offsets.push_back(at);
        at = (at + e.second.size() + 2047) / 2048 * 2048;
    }
    const std::size_t names_at = at;
    const std::size_t total = names_at + (0x30 * n + 2047) / 2048 * 2048;
    std::string s(total, '\0');
    std::memcpy(s.data(), "AFS\0", 4);
    put32(s, 4, static_cast<std::uint32_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        put32(s, 8 + 8 * i, static_cast<std::uint32_t>(offsets[i]));
        put32(s, 12 + 8 * i, static_cast<std::uint32_t>(entries[i].second.size()));
        std::memcpy(s.data() + offsets[i], entries[i].second.data(), entries[i].second.size());
        char* rec = s.data() + names_at + 0x30 * i;
        std::memcpy(rec, entries[i].first.data(), std::min<std::size_t>(entries[i].first.size(), 31));
        rec[0x20] = 0x07;  // a date word
        put32(s, names_at + 0x30 * i + 0x2C, static_cast<std::uint32_t>(entries[i].second.size()));
    }
    put32(s, 8 + 8 * n, static_cast<std::uint32_t>(names_at));
    put32(s, 12 + 8 * n, static_cast<std::uint32_t>(0x30 * n));
    return s;
}

std::uint32_t crc32(const std::string& d) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (const char ch : d) {
        c ^= static_cast<unsigned char>(ch);
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

struct ZipItem {
    std::string name;
    std::string data;
    bool deflate = false;
    bool bad_crc = false;
};

void le16(std::string& s, std::uint32_t v) {
    s.push_back(static_cast<char>(v));
    s.push_back(static_cast<char>(v >> 8));
}
void le32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
}

std::string make_zip(const std::vector<ZipItem>& items) {
    std::string zip, cd;
    for (const ZipItem& it : items) {
        std::string packed = it.data;
        if (it.deflate) {
            int len = 0;
            unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(it.data.data())),
                                                  static_cast<int>(it.data.size()), &len, 8);
            packed.assign(reinterpret_cast<char*>(z) + 2, static_cast<std::size_t>(len - 6));  // raw deflate
            STBIW_FREE(z);
        }
        const std::uint32_t crc = crc32(it.data) ^ (it.bad_crc ? 1u : 0u);
        const std::uint32_t offset = static_cast<std::uint32_t>(zip.size());
        le32(zip, 0x04034b50u);
        le16(zip, 20); le16(zip, 0x0800); le16(zip, it.deflate ? 8 : 0); le16(zip, 0); le16(zip, 0);
        le32(zip, crc); le32(zip, static_cast<std::uint32_t>(packed.size()));
        le32(zip, static_cast<std::uint32_t>(it.data.size()));
        le16(zip, static_cast<std::uint32_t>(it.name.size())); le16(zip, 0);
        zip += it.name;
        zip += packed;
        le32(cd, 0x02014b50u);
        le16(cd, 20); le16(cd, 20); le16(cd, 0x0800); le16(cd, it.deflate ? 8 : 0); le16(cd, 0); le16(cd, 0);
        le32(cd, crc); le32(cd, static_cast<std::uint32_t>(packed.size()));
        le32(cd, static_cast<std::uint32_t>(it.data.size()));
        le16(cd, static_cast<std::uint32_t>(it.name.size())); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0);
        le32(cd, 0); le32(cd, offset);
        cd += it.name;
    }
    const std::uint32_t cd_at = static_cast<std::uint32_t>(zip.size());
    zip += cd;
    le32(zip, 0x06054b50u);
    le16(zip, 0); le16(zip, 0);
    le16(zip, static_cast<std::uint32_t>(items.size())); le16(zip, static_cast<std::uint32_t>(items.size()));
    le32(zip, static_cast<std::uint32_t>(cd.size())); le32(zip, cd_at); le16(zip, 0);
    return zip;
}

// The smallest files the format checks accept: a ONE archive with no entries plus `tag` bytes, and an ADX header.
std::string make_one(const std::string& tag) {
    std::string data = "One Ver 0.60";
    data.resize(0x34, '\0');
    data += tag;
    std::string s(12, '\0');
    put32(s, 4, static_cast<std::uint32_t>(data.size()));
    put32(s, 8, 0x1C020037u);
    return s + data;
}
std::string make_adx(const std::string& tag) {
    std::string s(32, '\0');
    s[0] = static_cast<char>(0x80);
    s[3] = 0x10;  // the "(c)CRI" mark at 0x10 + 4
    return s + tag;
}

// A one-object layout (stgNNNN_*.dat: "sky2", count, parameter bytes, 0x2C-byte records, parameters) tagged with `tag`.
std::string make_dat(const std::string& tag) {
    std::string s(12 + 0x2C + 4, '\0');
    s.replace(0, 4, "sky2");
    put32(s, 4, 1);
    put32(s, 8, 4);
    s.replace(12, std::min<std::size_t>(tag.size(), 0x20), tag.substr(0, 0x20));
    put32(s, 12 + 0x24, 4);
    return s;
}

void write_mod(const fs::path& mods, const std::string& folder, const std::string& manifest_json,
               const std::vector<std::pair<std::string, std::string>>& files) {
    write_file(mods / folder / "mod.json", manifest_json);
    for (const auto& [rel, data] : files) write_file(mods / folder / rel, data);
}

// ---- tests -------------------------------------------------------------------------------------------------------

void test_json() {
    json::Value v;
    std::string err;
    check(json::parse("{\"a\": [1, 2.5, -3e2, true, false, null], \"b\": \"x\\\"y\\\\z\\u00e9\\ud83d\\ude00\"}", v, err),
          "json: parses a mixed document (" + err + ")");
    check(v.find("a") && v.find("a")->items().size() == 6, "json: array of six");
    check(v.find("a")->items()[2].as_number() == -300.0, "json: exponent");
    check(v.find("b")->as_string() == "x\"y\\z\xC3\xA9\xF0\x9F\x98\x80", "json: escapes and surrogate pair to UTF-8");
    json::Value back;
    check(json::parse(json::write(v), back, err), "json: write output parses");
    check(json::write(back) == json::write(v), "json: write is stable");
    check(!json::parse("{\"a\": 1,}", v, err), "json: trailing comma refused");
    check(err.find("line 1") != std::string::npos, "json: error has a position");
    check(!json::parse("[1] x", v, err), "json: trailing content refused");
    check(!json::parse("\"\\ud800\"", v, err), "json: unpaired surrogate refused");
    std::string deep(100, '[');
    check(!json::parse(deep, v, err), "json: nesting limit");
    check(json::parse("\xEF\xBB\xBF{}", v, err) && v.is_object(), "json: UTF-8 BOM accepted");
    check(json::parse("{\"k\":1,\"k\":2}", v, err) && v.find("k")->as_number() == 2, "json: duplicate key, last wins");
}

void test_versions() {
    const auto v = [](const char* t) { return *Version::parse(t); };
    check(Version::parse("1.2.3").has_value() && Version::parse("v2").has_value(), "version: accepts 1.2.3 and v2");
    check(!Version::parse("").has_value() && !Version::parse("1..2").has_value() && !Version::parse("abc"),
          "version: refuses junk");
    check(v("1.2").compare(v("1.2.0")) == 0, "version: missing parts are 0");
    check(v("1.10").compare(v("1.9")) > 0, "version: numeric, not text");
    check(v("1.2-beta").compare(v("1.2")) < 0, "version: suffix sorts before release");
}

void test_manifest() {
    std::vector<Issue> issues;
    Manifest m;
    json::Value v;
    std::string err;
    json::parse(R"({"id":"maria-eyes","name":"Maria Better Eyes","version":"1.1","author":"x",
        "dependencies":["base-fix>=2.0", {"id":"opt-dep","optional":true}],
        "incompatible":["other"], "load_after":["first"], "custom_field": 7,
        "options":[{"id":"style","name":"Style","choices":[{"id":"a","name":"A","folder":"options/style/a"},
                                                             {"id":"b","folder":"../escape"}]}]})",
                v, err);
    check(parse_manifest(v, m, issues), "manifest: a full one parses");
    check(m.dependencies.size() == 2 && m.dependencies[0].min_version == "2.0" && m.dependencies[1].optional,
          "manifest: dependency forms");
    check(m.options.size() == 1 && m.options[0].choices.size() == 1, "manifest: a choice leaving the mod is dropped");
    check(m.extra.find("custom_field") != nullptr, "manifest: unknown members kept");
    json::Value round = manifest_to_json(m);
    Manifest m2;
    check(parse_manifest(round, m2, issues) && m2.name == m.name && m2.dependencies.size() == 2 &&
              round.find("custom_field") != nullptr,
          "manifest: round trip");
    issues.clear();
    json::parse(R"({"id":"Bad Id","name":"x"})", v, err);
    check(!parse_manifest(v, m, issues) && has_errors(issues), "manifest: invalid id refused");
    issues.clear();
    json::parse(R"({"id":"a","dependencies":["a"]})", v, err);
    check(!parse_manifest(v, m, issues), "manifest: self dependency refused");
    issues.clear();
    json::parse(R"({"id":"a","version":"one"})", v, err);
    check(!parse_manifest(v, m, issues), "manifest: bad version refused");
    check(make_mod_id("Maria Better Eyes!") == "maria-better-eyes" && make_mod_id("!!!") == "mod",
          "manifest: ids from names");
}

void test_state(const fs::path& tmp) {
    State s;
    s.mods_enabled = false;
    s.order.push_back({"a", true, {{"style", "b"}}});
    s.order.push_back({"b", false, {}});
    std::string err;
    const fs::path f = tmp / "state" / "mods.json";
    check(save_state(f, s, err), "state: save (" + err + ")");
    State back;
    check(load_state(f, back, err) && !back.mods_enabled && back.order.size() == 2 && back.order[0].enabled &&
              back.order[0].options["style"] == "b",
          "state: round trip");
    check(load_state(tmp / "state" / "none.json", back, err) && back.order.empty(), "state: missing file is empty");
    write_file(tmp / "state" / "bad.json", "{\"order\": [");
    check(!load_state(tmp / "state" / "bad.json", back, err) && !err.empty(), "state: damaged file is an error");
    write_file(tmp / "state" / "new.json", "{\"schema\": 99}");
    check(!load_state(tmp / "state" / "new.json", back, err), "state: newer schema refused");
    check(!fs::exists(tmp / "state" / "mods.json.tmp"), "state: no temporary left");
#if defined(_WIN32)
    // Another program holding mods.json for a moment (no delete sharing, as scanners and backup tools do): the save
    // waits it out. Held for 200 ms; the save retries for about 600 ms.
    {
        HANDLE held = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        check(held != INVALID_HANDLE_VALUE, "state: test lock taken");
        std::thread release([held] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            CloseHandle(held);
        });
        s.mods_enabled = true;
        const bool saved = save_state(f, s, err);
        release.join();
        State reread;
        check(saved && load_state(f, reread, err) && reread.mods_enabled, "state: a save waits out a 200 ms lock");
        // Held for longer than the retries: the save reports it and the old file stays whole.
        held = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        s.mods_enabled = false;
        const bool refused = !save_state(f, s, err);
        CloseHandle(held);
        check(refused && err.find("another program") != std::string::npos && load_state(f, reread, err) &&
                  reread.mods_enabled && !fs::exists(tmp / "state" / "mods.json.tmp"),
              "state: a lock held past the retries is reported and the old file stays");
    }
#endif
}

std::vector<InstalledMod> setup_mods(const fs::path& mods) {
    write_mod(mods, "base", R"({"id":"base","name":"Base","version":"2.1"})", {{"files/a.bin", "base-a"}});
    write_mod(mods, "addon", R"({"id":"addon","name":"Addon","dependencies":["base>=2.0"]})",
              {{"files/a.bin", "addon-a"}, {"files/new/b.bin", "addon-b"}});
    write_mod(mods, "needs-new", R"({"id":"needs-new","dependencies":["base>=3"]})", {{"files/c.bin", "c"}});
    write_mod(mods, "needs-missing", R"({"id":"needs-missing","dependencies":["ghost"]})", {{"files/d.bin", "d"}});
    write_mod(mods, "chain", R"({"id":"chain","dependencies":["needs-missing"]})", {{"files/e.bin", "e"}});
    write_mod(mods, "rival", R"({"id":"rival","incompatible":["base"]})", {{"files/a.bin", "rival-a"}});
    write_mod(mods, "cyc1", R"({"id":"cyc1","load_after":["cyc2"]})", {{"files/f.bin", "f1"}});
    write_mod(mods, "cyc2", R"({"id":"cyc2","load_after":["cyc1"]})", {{"files/f.bin", "f2"}});
    write_mod(mods, "dupe", R"({"id":"base"})", {{"files/a.bin", "dupe"}});
    write_file(mods / "handmade" / "files" / "g.bin", "g");
    write_file(mods / "junk" / "readme.txt", "not a mod");
    write_mod(mods, "broken", "{ not json", {});
    std::vector<Issue> issues;
    return discover(mods, issues);
}

const InstalledMod* by_folder(const std::vector<InstalledMod>& mods, const std::string& folder) {
    for (const InstalledMod& m : mods)
        if (m.folder_name == folder) return &m;
    return nullptr;
}

void test_discover_and_resolve(const fs::path& tmp) {
    const fs::path mods = tmp / "mods1";
    const std::vector<InstalledMod> found = setup_mods(mods);
    check(by_folder(found, "base") && by_folder(found, "base")->usable, "discover: base usable");
    check(by_folder(found, "dupe") && !by_folder(found, "dupe")->usable, "discover: duplicate id unusable");
    check(by_folder(found, "handmade") && by_folder(found, "handmade")->usable &&
              by_folder(found, "handmade")->manifest.id == "handmade",
          "discover: a folder in the layout without mod.json works");
    check(by_folder(found, "junk") && !by_folder(found, "junk")->usable, "discover: a folder without content is not a mod");
    check(by_folder(found, "broken") && !by_folder(found, "broken")->usable, "discover: bad JSON unusable");

    State state;
    check(reconcile(state, found), "reconcile: adds new mods");
    check(std::none_of(state.order.begin(), state.order.end(), [](const StateEntry& e) { return e.enabled; }),
          "reconcile: new mods start disabled");
    // Priority (highest first): addon, base, rival, chain, needs-missing, needs-new, cyc1, cyc2, handmade
    state.order = {{"addon", true, {}},         {"base", true, {}},       {"rival", true, {}},
                   {"chain", true, {}},         {"needs-missing", true, {}}, {"needs-new", true, {}},
                   {"cyc1", true, {}},          {"cyc2", true, {}},       {"handmade", false, {}}};
    Resolution r = resolve(found, state);
    auto loaded = [&](const std::string& id) {
        return std::any_of(r.load_order.begin(), r.load_order.end(),
                           [&](const InstalledMod* m) { return m->manifest.id == id; });
    };
    check(loaded("addon") && loaded("base"), "resolve: addon and base load");
    check(!loaded("rival") && r.skipped.count("rival"),
          "resolve: rival declares base incompatible; base has the higher priority and wins");
    check(!loaded("needs-new"), "resolve: dependency too old skips");
    check(!loaded("needs-missing") && !loaded("chain"), "resolve: missing dependency skips, and its dependents");
    check(!loaded("cyc1") && !loaded("cyc2"), "resolve: load_after cycle skips both");
    check(!loaded("handmade"), "resolve: disabled mod does not load");
    // addon depends on base: base must be lower priority (earlier) even though listed below.
    std::size_t ia = 0, ib = 0;
    for (std::size_t i = 0; i < r.load_order.size(); ++i) {
        if (r.load_order[i]->manifest.id == "addon") ia = i;
        if (r.load_order[i]->manifest.id == "base") ib = i;
    }
    check(ib < ia, "resolve: a dependency loads before its dependent");
    // The incompatible mod listed higher wins instead, and what needs base goes with it.
    state.order = {{"rival", true, {}}, {"addon", true, {}}, {"base", true, {}}};
    r = resolve(found, state);
    check(loaded("rival") && !loaded("base") && !loaded("addon"), "resolve: higher-priority incompatible mod wins");
    // Put base above addon: base must still load first (dependency order beats list order).
    state.order = {{"base", true, {}}, {"addon", true, {}}};
    r = resolve(found, state);
    check(r.load_order.size() == 2 && r.load_order[0]->manifest.id == "base" && r.load_order[1]->manifest.id == "addon",
          "resolve: dependency order overrides the list");
    // Disabled dependency.
    state.order = {{"addon", true, {}}, {"base", false, {}}};
    r = resolve(found, state);
    check(r.load_order.empty() && has_errors(r.issues, "addon"), "resolve: disabled dependency skips with an error");
    // Master switch.
    state.order = {{"base", true, {}}};
    state.mods_enabled = false;
    r = resolve(found, state);
    check(r.load_order.empty() && r.issues.empty(), "resolve: master switch off loads nothing");
}

void test_plan(const fs::path& tmp) {
    const fs::path mods = tmp / "mods2";
    write_mod(mods, "low", R"({"id":"low"})",
              {{"files/Stage/A.BIN", "low-a"}, {"files/b.bin", "low-b"}, {"textures/tex1_8x8_0000000000000001_6.png", "t"}});
    write_mod(mods, "high",
              R"({"id":"high","options":[{"id":"extra","choices":[{"id":"off","folder":""},{"id":"on","folder":"options/extra/on"}]}]})",
              {{"files/stage/a.bin", "high-a"},
               {"options/extra/on/files/b.bin", "high-opt-b"},
               {"textures/sub/tex1_8x8_0000000000000002_6.png", "t"}});
    write_file(mods / "low" / "files" / "caf\xC3\xA9.bin", "non-ascii");
    write_file(tmp / "game2" / "files" / "stage" / "a.bin", "disc-a");
    write_file(tmp / "game2" / "files" / "b.bin", "disc-b");
    GameIndex game;
    std::string err;
    check(game.build(tmp / "game2" / "files", err), "plan: game index");
    std::vector<Issue> issues;
    const std::vector<InstalledMod> found = discover(mods, issues);
    State state;
    reconcile(state, found);
    state.order = {{"high", true, {{"extra", "off"}}}, {"low", true, {}}};
    Resolution r = resolve(found, state);
    Plan p = build_plan(r, state, game, tmp / "game2" / "files");
    const auto file = [&](const std::string& path) -> const FileOverlay* {
        for (const FileOverlay& f : p.files)
            if (f.game_path == path) return &f;
        return nullptr;
    };
    check(file("stage/a.bin") && file("stage/a.bin")->mod_id == "high" &&
              read_file(file("stage/a.bin")->host) == "high-a" && file("stage/a.bin")->overridden.size() == 1,
          "plan: the higher-priority mod wins, the disc's spelling is kept, the loser is listed");
    check(file("b.bin") && file("b.bin")->mod_id == "low", "plan: option off adds nothing");
    check(std::none_of(p.files.begin(), p.files.end(), [](const FileOverlay& f) { return f.game_path.find("caf") == 0; }),
          "plan: a non-ASCII name is skipped");
    check(p.texture_dirs.size() == 2 && p.texture_dirs[0].parent_path().filename() == "high",
          "plan: texture packs highest first");
    state.order[0].options["extra"] = "on";
    r = resolve(found, state);
    p = build_plan(r, state, game, tmp / "game2" / "files");
    check(file("b.bin") && file("b.bin")->mod_id == "high" && read_file(file("b.bin")->host) == "high-opt-b",
          "plan: option on contributes its folder");
}

void test_afs(const fs::path& tmp) {
    const std::string original = make_afs({{"one.adx", std::string(3000, 'a')}, {"two.adx", "bb"}, {"three.adx", "ccc"}});
    write_file(tmp / "afs" / "VOICE.afs", original);
    AfsArchive a;
    std::string err;
    check(read_afs(tmp / "afs" / "VOICE.afs", a, err) && a.entries.size() == 3 && a.entries[1].name == "two.adx" &&
              a.alignment == 2048,
          "afs: reads entries, names and alignment");
    fs::path out;
    AfsOverlay none{"VOICE.afs", {}};
    check(materialize_afs(tmp / "afs" / "VOICE.afs", none, tmp / "cache", out, err), "afs: rebuild without changes");
    check(read_file(out) == original, "afs: an unchanged rebuild is byte-identical to the original");
    write_file(tmp / "afs" / "new_two.adx", std::string(5000, 'N'));
    AfsOverlay one{"VOICE.afs", {{"TWO.ADX", tmp / "afs" / "new_two.adx", "m", {}}}};
    fs::path out2;
    check(materialize_afs(tmp / "afs" / "VOICE.afs", one, tmp / "cache", out2, err), "afs: rebuild with a replacement");
    AfsArchive b;
    check(read_afs(out2, b, err) && b.entries.size() == 3, "afs: rebuilt archive reads");
    const std::string rebuilt = read_file(out2);
    check(b.entries[1].size == 5000 && rebuilt.substr(b.entries[1].offset, 5000) == std::string(5000, 'N'),
          "afs: the entry is replaced");
    check(rebuilt.substr(b.entries[2].offset, 3) == "ccc" && b.entries[2].offset % 2048 == 0,
          "afs: following entries moved and aligned");
    check(get32(rebuilt, static_cast<std::size_t>(b.name_table_offset) + 0x30 + 0x2C) == 5000,
          "afs: the name record's size word follows the entry");
    check(!fs::exists(out), "afs: the older build of the same archive was removed");
    const auto time = fs::last_write_time(out2);
    fs::path out3;
    check(materialize_afs(tmp / "afs" / "VOICE.afs", one, tmp / "cache", out3, err) && out3 == out2 &&
              fs::last_write_time(out3) == time,
          "afs: an existing build is reused");
}

void test_zip(const fs::path& tmp) {
    std::string big;
    for (int i = 0; i < 5000; ++i) big += "line " + std::to_string(i) + "\n";
    write_file(tmp / "z" / "good.zip",
               make_zip({{"dir/", "", false}, {"dir/stored.txt", "hello", false}, {"dir/deflated.txt", big, true}}));
    std::string err;
    check(extract_archive(tmp / "z" / "good.zip", tmp / "z" / "good", err), "zip: extracts (" + err + ")");
    check(read_file(tmp / "z" / "good" / "dir" / "stored.txt") == "hello", "zip: stored entry");
    check(read_file(tmp / "z" / "good" / "dir" / "deflated.txt") == big, "zip: deflated entry");
    write_file(tmp / "z" / "evil.zip", make_zip({{"../evil.txt", "x", false}}));
    check(!extract_archive(tmp / "z" / "evil.zip", tmp / "z" / "evil", err) && !fs::exists(tmp / "z" / "evil.txt"),
          "zip: parent traversal refused");
    write_file(tmp / "z" / "abs.zip", make_zip({{"C:/x.txt", "x", false}}));
    check(!extract_archive(tmp / "z" / "abs.zip", tmp / "z" / "abs", err), "zip: drive path refused");
    write_file(tmp / "z" / "crc.zip", make_zip({{"a.txt", "data", true, true}}));
    check(!extract_archive(tmp / "z" / "crc.zip", tmp / "z" / "crc", err) && err.find("checksum") != std::string::npos,
          "zip: checksum failure detected");
    write_file(tmp / "z" / "trunc.zip", make_zip({{"a.txt", "data", false}}).substr(0, 20));
    check(!extract_archive(tmp / "z" / "trunc.zip", tmp / "z" / "trunc", err), "zip: truncated archive refused");
}

void test_install(const fs::path& tmp) {
    const fs::path game_files = tmp / "game3" / "files";
    write_file(game_files / "character" / "shadow.one", make_one("disc-shadow"));
    write_file(game_files / "character" / "maria.one", make_one("disc-maria"));
    write_file(game_files / "stg0100" / "stg0100_nrm.dat", make_dat("disc-dat"));
    write_file(game_files / "fonts" / "stg0100" / "stg0100_EN.fnt", "disc-fnt");
    write_file(game_files / "a" / "dup.bin", "a");
    write_file(game_files / "b" / "dup.bin", "b");
    write_file(game_files / "PRS_VOICE_E.afs", make_afs({{"act04_g149_sl.adx", "orig"}, {"x.adx", "x"}}));
    GameIndex game;
    std::string err;
    game.build(game_files, err);
    const fs::path mods = tmp / "mods3";
    const fs::path dl = tmp / "downloads";
    // A Reloaded-like tree: ISO EDITS/files, AFS folder, Dolphin textures, Gecko ini, main.dol, an alternative set.
    write_file(dl / "reloaded" / "ISO EDITS" / "files" / "stg0100" / "stg0100_nrm.dat", make_dat("rl-dat"));
    write_file(dl / "reloaded" / "ISO EDITS" / "files" / "stg0210" / "stg0210_nrm.dat", make_dat("rl-new"));
    write_file(dl / "reloaded" / "ISO EDITS" / "sys" / "main.dol", "dol");
    write_file(dl / "reloaded" / "ISO EDITS" / "sys" / "bi2.bin", "bi2");
    write_file(dl / "reloaded" / "AFS" / "PRS_VOICE_E" / "act04_g149_sl.adx", make_adx("new-voice"));
    write_file(dl / "reloaded" / "Dolphin Addons" / "Textures" / "GUPR8P" / "tex1_8x8_00000000000000aa_6.png", "png");
    write_file(dl / "reloaded" / "code" / "GUPR8P.ini", "[Gecko]\n$x\n04000000 00000000\n");
    write_file(dl / "reloaded" / "workspace" / "Widescreen-overrides" / "stg0100" / "stg0100_nrm.dat", make_dat("ws-dat"));
    write_file(dl / "reloaded" / "README.md", "Reloaded test readme");
    InstallRequest req{dl / "reloaded", mods, &game, false, "Reloaded Test"};
    InstallResult r = install(req);
    check(r.ok, "install: reloaded-like tree installs");
    const fs::path m = r.folder;
    check(read_file(m / "files" / "stg0100" / "stg0100_nrm.dat") == make_dat("rl-dat"), "install: disc file by path tail");
    check(read_file(m / "files" / "stg0210" / "stg0210_nrm.dat") == make_dat("rl-new"), "install: new file below files/");
    check(read_file(m / "afs" / "PRS_VOICE_E" / "act04_g149_sl.adx") == make_adx("new-voice"), "install: AFS entry folder");
    check(fs::exists(m / "textures" / "Dolphin Addons" / "Textures" / "GUPR8P" / "tex1_8x8_00000000000000aa_6.png"),
          "install: textures");
    check(fs::exists(m / "codes" / "code" / "GUPR8P.ini") && fs::exists(m / "codes" / "ISO EDITS" / "sys" / "main.dol"),
          "install: code patches kept aside");
    check(!fs::exists(m / "files" / "bi2.bin"), "install: disc header files left out");
    check(r.manifest.options.size() == 1 &&
              read_file(m / "options" / r.manifest.options[0].id / "on" / "files" / "stg0100" / "stg0100_nrm.dat") ==
                  make_dat("ws-dat"),
          "install: an alternative set becomes an option");
    check(r.manifest.description == "Reloaded test readme" && r.manifest.game_id == "GUPR8P",
          "install: description and game id");
    check(r.content.unsupported.size() >= 2, "install: code patches reported");
    // Loose file by unique name; ambiguous name refused; nothing usable fails.
    write_file(dl / "loose" / "Tails" / "shadow.one", make_one("tails"));
    r = install({dl / "loose", mods, &game, false, "Tails"});
    check(r.ok && read_file(r.folder / "files" / "character" / "shadow.one") == make_one("tails"), "install: loose file by name");
    // The same download again is the same mod: refused as installed, or replaced on request (no "tails-2").
    r = install({dl / "loose", mods, &game, false, "Tails"});
    check(!r.ok && std::any_of(r.issues.begin(), r.issues.end(),
                               [](const Issue& i) { return i.message.find("is installed") != std::string::npos; }) &&
              !fs::exists(mods / "tails-2"),
          "install: the same download again is recognised, not installed twice");
    write_file(dl / "loose" / "Tails" / "shadow.one", make_one("tails v2"));
    r = install({dl / "loose", mods, &game, true, "Tails"});
    check(r.ok && r.id == "tails" && read_file(mods / "tails" / "files" / "character" / "shadow.one") == make_one("tails v2"),
          "install: replacing updates the mod in place");
    // A user's edits of the details survive an update of the content.
    {
        Manifest mf;
        std::vector<Issue> mi;
        read_manifest(mods / "tails" / "mod.json", mf, mi);
        mf.name = "My Tails";
        mf.extra.set("user_edited", json::Value::boolean(true));
        write_manifest(mods / "tails" / "mod.json", mf, err);
        write_file(dl / "loose" / "Tails" / "shadow.one", make_one("tails v3"));
        r = install({dl / "loose", mods, &game, true, "Tails"});
        read_manifest(mods / "tails" / "mod.json", mf, mi);
        check(r.ok && mf.name == "My Tails" && read_file(mods / "tails" / "files" / "character" / "shadow.one") ==
                                                   make_one("tails v3"),
              "install: an update keeps the user's edited name and takes the new content");
    }
    write_file(dl / "other" / "Tails" / "shadow.one", make_one("someone else's tails"));
    r = install({dl / "other", mods, &game, false, "Tails"});
    check(r.ok && r.id == "tails-2", "install: a different download with the same name gets its own id");
    remove_installed(mods, mods / "tails-2", err);
    write_file(dl / "ambig" / "dup.bin", "?");
    write_file(dl / "ambig" / "readme.txt", "hi");
    r = install({dl / "ambig", mods, &game, false, "Ambig"});
    check(!r.ok, "install: only an ambiguous file means nothing to install");
    // GameCube Rebuilder layout (root/) and a zip download with a nested Dolphin resource pack.
    const std::string inner = make_zip({{"manifest.json", R"({"name":"No Gloss","authors":["A","B"],"version":"1.1"})", false},
                                        {"textures/GUPE8P/tex1_32x32_0000000000000003_8.png", "p", true}});
    write_file(dl / "pack.zip",
               make_zip({{"Maria/root/character/maria.one", make_one("fixed-maria"), true}, {"inner.zip", inner, false}}));
    r = install({dl / "pack.zip", mods, &game, false, ""});
    check(r.ok && read_file(r.folder / "files" / "character" / "maria.one") == make_one("fixed-maria"),
          "install: zip with root/ layout");
    check(r.ok && r.content.textures == 1 && r.manifest.name == "No Gloss" && r.manifest.author == "A, B",
          "install: nested resource pack unpacked and its manifest used");
    // A truncated ONE (an interrupted download) is refused at install, named.
    write_file(dl / "truncated" / "files" / "character" / "shadow.one", make_one("whole").substr(0, 40));
    r = install({dl / "truncated", mods, &game, false, "Truncated"});
    check(!r.ok && std::any_of(r.issues.begin(), r.issues.end(),
                               [](const Issue& i) { return i.message.find("truncated") != std::string::npos; }),
          "install: a truncated ONE archive is refused");
    std::string why;
    check(!check_file_format(dl / "truncated" / "files" / "character" / "shadow.one", "shadow.one", why) &&
              check_file_format(game_files / "character" / "shadow.one", "shadow.one", why) &&
              check_file_format(game_files / "stg0100" / "stg0100_nrm.dat", "stg0100_nrm.dat", why),
          "format: truncated ONE fails, whole ONE and unchecked kinds pass");
    // Layouts (docs/mods/map/FORMAT_CHECKS.md): a half-length stage layout stopped the game in testing.
    write_file(dl / "cut.dat", make_dat("whole").substr(0, 30));
    write_file(dl / "whole.dat", make_dat("whole"));
    check(!check_file_format(dl / "cut.dat", "stg0100_nrm.dat", why) && check_file_format(dl / "whole.dat", "stg0100_nrm.dat", why),
          "format: a cut layout fails, a whole one passes");
    // Movies: the disc's layout (pack headers, end code, 0xFF padding) and sfdmux's (a private stream 2 packet first,
    // no end code, no padding: the community's re-edited intros). A cut stream fails in either layout.
    {
        const auto packet = [](std::uint8_t id, std::size_t payload) {
            std::string p = {'\0', '\0', '\1', static_cast<char>(id), static_cast<char>(payload >> 8),
                             static_cast<char>(payload & 0xFF)};
            return p + std::string(payload, '\x5A');
        };
        // MPEG-2 pack header: 14 bytes, no stuffing.
        const std::string pack = std::string("\0\0\1\xBA\x44\0\4\0\4\1\1\x89\xC3\xF8", 14);
        std::string disc;
        for (int i = 0; i < 4; ++i) disc += pack + packet(0xE0, 2048 - 14 - 6);
        disc += std::string("\0\0\1\xB9", 4) + std::string(2044, '\xFF');
        std::string sfdmux = packet(0xBF, 2030);
        for (int i = 0; i < 5; ++i) sfdmux += packet(0xE0, 1000 + 37 * i) + packet(0xC0, 300);
        write_file(dl / "movie_disc.sfd", disc);
        write_file(dl / "movie_disc_cut.sfd", disc.substr(0, 2 * 2048));            // at a pack boundary
        write_file(dl / "movie_sfdmux.sfd", sfdmux);
        write_file(dl / "movie_sfdmux_cut.sfd", sfdmux.substr(0, sfdmux.size() - 100));  // inside a packet
        write_file(dl / "movie_junk.sfd", std::string(4096, 'x'));
        check(check_file_format(dl / "movie_disc.sfd", "Op.sfd", why) &&
                  check_file_format(dl / "movie_sfdmux.sfd", "Op.sfd", why),
              "format: a movie in the disc's layout and one muxed by sfdmux (no end code, no padding) pass");
        check(!check_file_format(dl / "movie_disc_cut.sfd", "Op.sfd", why) &&
                  !check_file_format(dl / "movie_sfdmux_cut.sfd", "Op.sfd", why) &&
                  !check_file_format(dl / "movie_junk.sfd", "E1001.sfd", why),
              "format: a cut movie (either layout) and a non-movie fail");
        // A whole movie larger than the game's 512x480 (the Xbox version's 640x480): not usable, left out at install
        // while the rest of the download installs.
        const auto with_size = [&](unsigned w, unsigned h) {
            std::string video = std::string("\0\0\1\xB3", 4);
            video += static_cast<char>(w >> 4);
            video += static_cast<char>((w & 15u) << 4 | h >> 8);
            video += static_cast<char>(h & 0xFF);
            video += '\x15';  // aspect / frame rate
            video += std::string(2048 - 14 - 6 - video.size(), '\x5A');
            std::string pes = std::string("\0\0\1\xE0", 4);
            pes += static_cast<char>(video.size() >> 8);
            pes += static_cast<char>(video.size() & 0xFF);
            std::string m = pack + pes + video;
            for (int i = 0; i < 3; ++i) m += pack + packet(0xE0, 2048 - 14 - 6);
            return m + std::string("\0\0\1\xB9", 4) + std::string(2044, '\xFF');
        };
        write_file(dl / "movie_512.sfd", with_size(512, 480));
        write_file(dl / "movie_640.sfd", with_size(640, 480));
        check(check_file_compatible(dl / "movie_512.sfd", "E1001.sfd", why) &&
                  check_file_format(dl / "movie_512.sfd", "E1001.sfd", why) &&
                  !check_file_compatible(dl / "movie_640.sfd", "E1001.sfd", why) &&
                  why.find("640x480") != std::string::npos &&
                  !check_file_format(dl / "movie_640.sfd", "E1001.sfd", why),
              "format: a 512x480 movie is usable, a whole 640x480 one is not (named)");
        write_file(dl / "xbox" / "files" / "E1001.sfd", with_size(640, 480));
        write_file(dl / "xbox" / "files" / "character" / "shadow.one", make_one("xbox-pack"));
        r = install({dl / "xbox", mods, &game, false, "Xbox Pack"});
        check(r.ok && !fs::exists(r.folder / "files" / "E1001.sfd") &&
                  read_file(r.folder / "files" / "character" / "shadow.one") == make_one("xbox-pack") &&
                  std::any_of(r.left_out.begin(), r.left_out.end(),
                              [](const std::string& l) { return l.find("E1001.sfd: is a 640x480 movie") != std::string::npos; }),
              "install: a 640x480 movie is left out (named), the rest of the download installs");
        if (r.ok) remove_installed(mods, r.folder, err);
    }
    // A native mod (mod.json under one wrapping folder) and an update.
    write_mod(dl / "native" / "wrap", "", R"({"id":"native-mod","name":"Native","version":"1.0"})",
              {{"files/b/dup.bin", "native"}});
    r = install({dl / "native" / "wrap", mods, &game, false, ""});
    check(r.ok && r.id == "native-mod" && fs::exists(mods / "native-mod" / "files" / "b" / "dup.bin"),
          "install: native mod copied");
    r = install({dl / "native" / "wrap", mods, &game, false, ""});
    check(!r.ok, "install: same id refused without replace");
    write_file(dl / "native" / "wrap" / "files" / "b" / "dup.bin", "native-2");
    r = install({dl / "native" / "wrap", mods, &game, true, ""});
    check(r.ok && read_file(mods / "native-mod" / "files" / "b" / "dup.bin") == "native-2", "install: update replaces");
    // Saves and programs are never installed; a download that is only one says what it is.
    {
        const auto has = [](const InstallResult& res, const std::string& text) {
            return std::any_of(res.issues.begin(), res.issues.end(),
                               [&](const Issue& i) { return i.message.find(text) != std::string::npos; });
        };
        const auto gci = [](const std::string& game, const std::string& name) {
            std::string g(0x40 + 2 * 0x2000, '\0');
            g.replace(0, 4, game);
            g.replace(4, 2, "8P");
            g.replace(8, name.size(), name);
            g[0x39] = 2;  // block count (big-endian)
            return g;
        };
        write_file(dl / "save-own" / "8P-GUPE-SHADOWTHEHEDGEHOG.gci", gci("GUPE", "SHADOWTHEHEDGEHOG"));
        write_file(dl / "save-own" / "readme.txt", "100% save");
        r = install({dl / "save-own", mods, &game, false, "Save"});
        check(!r.ok && has(r, "is a save, not a mod") && has(r, "file \"SHADOWTHEHEDGEHOG\"") && has(r, "--save-file") &&
                  !fs::exists(mods / "save"),
              "install: a download that is the game's own save is refused, named, with how to use it");
        write_file(dl / "save-other" / "8P-GUPR-SHADOWRELOADED.gci", gci("GUPR", "SHADOWRELOADED"));
        r = install({dl / "save-other", mods, &game, false, "Save"});
        check(!r.ok && has(r, "game GUPR, maker 8P, file \"SHADOWRELOADED\"") && has(r, "cannot be used here") &&
                  !has(r, "--save-file"),
              "install: another game's save (Reloaded's) is refused as unusable here");
        write_file(dl / "program" / "Tool" / "Tool.exe", "MZ fake");
        write_file(dl / "program" / "Tool" / "helper.dll", "MZ fake");
        write_file(dl / "program" / "Tool" / "Lists" / "list.ini", "[x]\na=1\n");
        r = install({dl / "program", mods, &game, false, "Tool"});
        check(!r.ok && has(r, "this download is a program (Tool/Tool.exe)") && !fs::exists(mods / "tool"),
              "install: a program is refused and named, nothing installed");
        // Data next to a program: the data installs, the program is left out (also from a mod with its own mod.json).
        write_file(dl / "mixed" / "files" / "character" / "shadow.one", make_one("mixed"));
        write_file(dl / "mixed" / "Installer.exe", "MZ fake");
        r = install({dl / "mixed", mods, &game, false, "Mixed"});
        check(r.ok && !fs::exists(r.folder / "Installer.exe") && !fs::exists(r.folder / "docs" / "Installer.exe") &&
                  std::any_of(r.left_out.begin(), r.left_out.end(),
                              [](const std::string& l) { return l.find("Installer.exe: a program") == 0; }),
              "install: a program beside data is left out, the data installs");
        remove_installed(mods, r.folder, err);
        write_mod(dl / "native-prog", "", R"({"id":"native-prog","name":"Native with a program"})",
                  {{"files/b/dup.bin", "x"}, {"tools/run.exe", "MZ"}});
        r = install({dl / "native-prog", mods, &game, false, ""});
        check(r.ok && !fs::exists(mods / "native-prog" / "tools" / "run.exe") &&
                  fs::exists(mods / "native-prog" / "files" / "b" / "dup.bin"),
              "install: a program inside a native mod is not installed");
        remove_installed(mods, mods / "native-prog", err);
    }
    check(!fs::exists(mods / ".staging"), "install: staging removed");
    // Removal stays inside the mods folder.
    check(!remove_installed(mods, tmp, err), "remove: refuses a folder outside mods/");
    check(remove_installed(mods, mods / "native-mod", err) && !fs::exists(mods / "native-mod"), "remove: removes");

    // End to end: prepare_runtime over the installed Reloaded-like mod.
    State state;
    std::vector<Issue> issues;
    std::vector<InstalledMod> found = discover(mods, issues);
    reconcile(state, found);
    for (StateEntry& e : state.order) e.enabled = e.id == "reloaded-test";
    check(save_state(mods / "mods.json", state, err), "runtime: state saved");
    RuntimeSetup setup = prepare_runtime(mods, game_files);
    check(setup.active && setup.texture_dirs.size() == 1, "runtime: active with one texture pack");
    const auto served = [&](const std::string& path) -> fs::path {
        for (const auto& [p, h] : setup.files)
            if (p == path) return h;
        return {};
    };
    check(read_file(served("stg0100/stg0100_nrm.dat")) == make_dat("rl-dat"), "runtime: replaced file served");
    check(read_file(served("stg0210/stg0210_nrm.dat")) == make_dat("rl-new"), "runtime: added file served");
    AfsArchive va;
    check(!served("PRS_VOICE_E.afs").empty() && read_afs(served("PRS_VOICE_E.afs"), va, err) &&
              read_file(served("PRS_VOICE_E.afs")).substr(va.entries[0].offset, 41) == make_adx("new-voice"),
          "runtime: AFS rebuilt with the voice entry");
    {
        // The mod's code is offered as a code item (chosen under Code mods), with a note on the installed mod.
        std::vector<Issue> di;
        bool noted = false, offered = false;
        for (const InstalledMod& im : discover(mods, di))
            if (im.manifest.id == "reloaded-test") {
                noted = std::any_of(im.issues.begin(), im.issues.end(), [](const Issue& i) {
                    return i.message.find("main.dol") != std::string::npos && i.message.find("Code mods") != std::string::npos;
                });
                for (const CodeItem& c : list_code_items(im)) offered |= c.id == "dol";
            }
        // Code choices are saved, survive a reload, and make a selection signature; unchosen code has none.
        State cs;
        std::vector<InstalledMod> found2 = discover(mods, di);
        reconcile(cs, found2);
        for (StateEntry& e : cs.order) e.enabled = e.id == "reloaded-test";
        check(code_selection_signature(found2, cs).empty(), "code: nothing chosen, no signature");
        for (StateEntry& e : cs.order)
            if (e.id == "reloaded-test") e.codes["dol"] = true;
        const std::string sig = code_selection_signature(found2, cs);
        save_state(tmp / "codes-state.json", cs, err);
        State back2;
        load_state(tmp / "codes-state.json", back2, err);
        check(!sig.empty() && code_selection_signature(found2, back2) == sig, "code: choices saved; signature stable");
        for (StateEntry& e : back2.order)
            if (e.id == "reloaded-test") e.codes["no-such-item"] = true;
        reconcile(back2, found2);
        bool stale = false;
        for (const StateEntry& e : back2.order) stale |= e.codes.count("no-such-item") != 0;
        check(!stale, "code: a choice for an item the mod no longer offers is dropped");
        check(noted && offered, "code: the mod's main.dol is offered as a code item and noted");
        {
            // The same game ini shipped twice (Dolphin and Nintendont folders) is offered once; a different ini that
            // reuses a code name gets its own id, so one choice never switches on two files' code (2P-ShdTH).
            const fs::path twice = tmp / "mods-ini-twice";
            const std::string ini = "[Gecko]\n$Split [author]\n04000000 00000000\n$Other\n04000004 00000001\n";
            write_mod(twice, "twop", R"({"id":"twop","name":"2P"})",
                      {{"files/b/dup.bin", "x"}, {"codes/Dolphin/GUPE8P.ini", ini}, {"codes/Nintendont/GUPE8P.ini", ini},
                       {"codes/Other/GUPE8P.ini", "[Gecko]\n$Split\n04000008 00000002\n"}});
            std::vector<Issue> ti;
            std::vector<std::string> ids;
            for (const InstalledMod& im : discover(twice, ti))
                for (const CodeItem& c : list_code_items(im)) ids.push_back(c.id);
            std::set<std::string> unique(ids.begin(), ids.end());
            check(ids.size() == 3 && unique.size() == 3 && unique.count("gecko:Split") &&
                      unique.count("gecko:Other") && unique.count("gecko:Split [Other/GUPE8P.ini]"),
                  "code: an ini shipped twice is offered once; a reused code name in another ini gets its own id (" +
                      [&] { std::string j; for (const auto& i : ids) j += i + ";"; return j; }() + ")");
        }

        // The game runs the data codes the pipeline built for exactly the chosen selection.
        check(save_state(mods / "mods.json", cs, err), "codes: state with a chosen code saved");
        RuntimeSetup cr = prepare_runtime(mods, game_files);
        check(cr.code_signature == sig && cr.codes.empty() &&
                  std::any_of(cr.issues.begin(), cr.issues.end(),
                              [](const Issue& i) { return i.message.find("not built") != std::string::npos; }),
              "codes: chosen but not built - nothing runs, and the game says so");
        write_file(mods / ".variants" / (sig + ".json"),
                   "{\"signature\":\"" + sig + "\",\"engine_codes\":["
                   "{\"name\":\"reloaded-test:dol#No HUD\",\"lines\":[\"0057E8A8 00000000\"],\"needs_variant\":false},"
                   "{\"name\":\"reloaded-test:dol#Mixed\",\"lines\":[\"2856FE0E 00002020\",\"04577788 00007724\","
                   "\"E2000001 80008000\"],\"needs_variant\":true},"
                   "{\"name\":\"reloaded-test:dol#Damaged\",\"lines\":[\"0057E8A8\"]}]}");
        cr = prepare_runtime(mods, game_files);
        check(cr.active && cr.codes.size() == 2 && cr.codes[0].lines.size() == 1 &&
                  cr.codes[0].lines[0] == std::make_pair(0x0057E8A8u, 0u) && !cr.codes[0].needs_variant &&
                  cr.codes[1].lines.size() == 3 && cr.codes[1].needs_variant,
              "codes: the built selection's data codes are read, with what needs the variant");
        check(std::any_of(cr.issues.begin(), cr.issues.end(),
                          [](const Issue& i) { return i.message.find("Damaged") != std::string::npos; }),
              "codes: a damaged code is left out and reported");
        for (StateEntry& e : cs.order) e.codes.clear();
        save_state(mods / "mods.json", cs, err);
        cr = prepare_runtime(mods, game_files);
        check(cr.code_signature.empty() && cr.codes.empty(), "codes: nothing chosen - no code runs, even if one was built");
        save_state(mods / "mods.json", state, err);
    }
    // A file damaged after installation is left out by the plan (the disc's file is served), with an error.
    write_file(mods / "reloaded-test" / "files" / "character" / "maria.one", "garbage");
    setup = prepare_runtime(mods, game_files);
    check(served("character/maria.one").empty() &&
              std::any_of(setup.issues.begin(), setup.issues.end(),
                          [](const Issue& i) { return i.message.find("maria.one") != std::string::npos; }),
          "runtime: a damaged installed file is skipped and reported");
    fs::remove(mods / "reloaded-test" / "files" / "character" / "maria.one");
    // Master switch off and damaged state: inactive.
    state.mods_enabled = false;
    save_state(mods / "mods.json", state, err);
    check(!prepare_runtime(mods, game_files).active, "runtime: master switch off");
    write_file(mods / "mods.json", "{broken");
    setup = prepare_runtime(mods, game_files);
    check(!setup.active && has_errors(setup.issues), "runtime: damaged state runs without mods and says so");
    check(!prepare_runtime(tmp / "no-such-dir", game_files).active, "runtime: no mods folder, nothing to do");
}


void test_profiles_and_textures(const fs::path& tmp) {
    const fs::path mods = tmp / "mods4";
    write_mod(mods, "a", R"({"id":"a"})", {{"textures/tex1_8x8_00000000000000aa_6.png", "A"},
                                           {"textures/sub/TEX1_8X8_00000000000000BB_6.png", "A"}});
    write_mod(mods, "b", R"({"id":"b"})", {{"textures/tex1_8x8_00000000000000aa_6.png", "B"},
                                           {"textures/tex1_8x8_00000000000000bb_6.png", "B"},
                                           {"textures/tex1_8x8_00000000000000cc_6.png", "B"}});
    std::vector<Issue> issues;
    const std::vector<InstalledMod> found = discover(mods, issues);
    State state;
    reconcile(state, found);
    state.order = {{"a", true, {}}, {"b", true, {}}};
    std::string err;
    // Profiles: saved, listed, loaded, refused names, deleted; a profile folder is not a mod.
    check(save_profile(mods, "Speedrun set", state, err), "profile: save (" + err + ")");
    check(!save_profile(mods, "../evil", state, err) && !save_profile(mods, "", state, err) &&
              !valid_profile_name(" lead") && valid_profile_name("Set (2)"),
          "profile: names outside the rule are refused");
    check(list_profiles(mods) == std::vector<std::string>{"Speedrun set"}, "profile: listed");
    State back;
    check(load_profile(mods, "Speedrun set", back, err) && back.order.size() == 2 && back.order[0].id == "a" &&
              back.order[0].enabled,
          "profile: loads what was saved");
    check(!load_profile(mods, "nope", back, err), "profile: a missing profile is an error");
    issues.clear();
    check(discover(mods, issues).size() == 2, "profile: the .profiles folder is not discovered as a mod");
    check(delete_profile(mods, "Speedrun set", err) && list_profiles(mods).empty(), "profile: deleted");
    // Texture conflicts: a wins aa and bb (case-insensitive) over b; cc is b's alone.
    const Resolution r = resolve(found, state);
    GameIndex none;
    const Plan plan = build_plan(r, state, none, tmp);
    check(plan.texture_dirs.size() == 2 && plan.texture_mods.size() == 2 && plan.texture_mods[0] == "a",
          "textures: packs and their mods, highest first");
    const std::vector<TextureConflict> tc = texture_conflicts(plan);
    check(tc.size() == 1 && tc[0].winner == "a" && tc[0].loser == "b" && tc[0].count == 2,
          "textures: two shared names, the higher pack wins");
}

}  // namespace

int main() {
    const fs::path tmp = make_temp();
    test_json();
    test_versions();
    test_manifest();
    test_state(tmp);
    test_discover_and_resolve(tmp);
    test_plan(tmp);
    test_afs(tmp);
    test_zip(tmp);
    test_install(tmp);
    test_profiles_and_textures(tmp);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
