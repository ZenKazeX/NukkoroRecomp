// ModManager.exe (the Mod Manager): installs, enables, orders and configures mods for the PC build of Shadow the Hedgehog (a static
// recompilation made with GekkoForge). One native Windows program: a window when started without arguments, a
// command line for scripts and tests when given a command (ModManager.exe help).
//
// It edits only <mods dir>/mods.json and the mod folders; the game reads them at start-up (shadow/mods/mods.h).
// Why a separate program and not a game menu: docs/mods/DESIGN.md ("The manager").

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "shadow/mods/insight.h"
#include "shadow/mods/install.h"
#include "shadow/mods/mods.h"

#include "appearance.h"
#include "theme.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
namespace mods = shadow::mods;

namespace {

// ---- text --------------------------------------------------------------------------------------------------------

std::wstring wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Windows line ends for edit controls.
std::wstring crlf(const std::wstring& s) {
    std::wstring out;
    for (const wchar_t c : s) {
        if (c == L'\n') out += L"\r\n";
        else if (c != L'\r') out.push_back(c);
    }
    return out;
}

// This program's own file (ModManager.exe, or whatever it is called) and its folder.
fs::path executable_path() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf);
}

fs::path executable_dir() { return executable_path().parent_path(); }

// The window's title and the title of its messages. Kept as UTF-8 and widened when used: the game's name as a UTF-16
// literal would be the same bytes as the game's own font files, which the release's content scan refuses.
const wchar_t* window_title() {
    static const std::wstring title = wide("Mod Manager - Shadow the Hedgehog");
    return title.c_str();
}

// The game data root the way the game finds it (runtime/src/native_bootstrap.cpp): GameData/ or the folder itself,
// beside the executable, then in the working directory.
std::optional<fs::path> find_game_root(const std::optional<fs::path>& explicit_root) {
    std::vector<fs::path> candidates;
    if (explicit_root) {
        candidates.push_back(*explicit_root);
    } else {
        const fs::path exe = executable_dir();
        candidates = {exe / "GameData", exe, fs::current_path() / "GameData", fs::current_path()};
    }
    for (const fs::path& c : candidates) {
        std::error_code ec;
        if (fs::is_directory(c / "files", ec)) return c;
    }
    return std::nullopt;
}

// ---- the model ---------------------------------------------------------------------------------------------------

struct Model {
    fs::path mods_dir;
    fs::path game_root;
    std::vector<mods::InstalledMod> installed;
    std::vector<mods::Issue> discover_issues;
    mods::State state;
    std::string state_error;  // the state file could not be read; nothing is written until the user decides
    mods::GameIndex game;
    std::string game_error;
    mods::VoiceIndex voices;  // the disc's voice entries and the subtitle lines that play them
    mods::RegionData region;  // mods-data/region_diff.json: the files PAL and NTSC-U differ in
    mutable std::map<std::string, std::vector<mods::Issue>> region_cache;  // mod id -> its region notes
    mods::json::Value reloaded;  // mods-data/reloaded_features.json: Reloaded's code features (map-rlcode agent)
    mods::json::Value contexts;  // mods-data/file_contexts.json: which stage/menu/cutscene reads which file (map-files)
    mods::Resolution resolution;
    mods::Plan plan;

    fs::path state_file() const { return mods_dir / std::string(mods::kStateName); }

    void index_game() {
        game_error.clear();
        if (!game.build(game_root / "files", game_error)) game.paths.clear();
        std::string voice_error;
        if (!game.empty()) voices.build(game_root / "files", voice_error);
        std::string region_error;
        region.load(executable_dir() / "mods-data" / "region_diff.json", region_error);
        {
            std::ifstream in(mods::long_path(executable_dir() / "mods-data" / "reloaded_features.json"), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string e;
            if (text.empty() || !mods::json::parse(text, reloaded, e)) reloaded = mods::json::Value{};
        }
        {
            std::ifstream in(mods::long_path(executable_dir() / "mods-data" / "file_contexts.json"), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string e;
            if (text.empty() || !mods::json::parse(text, contexts, e)) contexts = mods::json::Value{};
        }
    }

    // Re-reads the mods folder and the state, fixes the state up and re-plans. Saves when reconcile changed it.
    void refresh() {
        std::error_code ec;
        fs::create_directories(mods_dir, ec);
        discover_issues.clear();
        installed = mods::discover(mods_dir, discover_issues);
        region_cache.clear();
        if (state_error.empty()) {
            mods::State loaded;
            std::string error;
            if (!mods::load_state(state_file(), loaded, error)) {
                state_error = error;
            } else {
                state = std::move(loaded);
            }
        }
        if (mods::reconcile(state, installed) && state_error.empty()) save();
        replan();
    }

    void replan() {
        resolution = mods::resolve(installed, state);
        plan = mods::build_plan(resolution, state, game, game_root / "files");
        texture_conflicts = mods::texture_conflicts(plan);
    }

    std::string save_error;  // the last save that failed, for the window to say so
    bool save() {
        if (!state_error.empty()) return false;
        save_error.clear();
        return mods::save_state(state_file(), state, save_error);
    }

    // Starts over with an empty state (after the user agreed to replace a damaged file).
    void reset_state() {
        state = mods::State{};
        state_error.clear();
        mods::reconcile(state, installed);
        save();
        replan();
    }

    const mods::InstalledMod* find(const std::string& id) const {
        for (const mods::InstalledMod& m : installed)
            if (m.usable && m.manifest.id == id) return &m;
        return nullptr;
    }
    mods::StateEntry* entry(const std::string& id) {
        for (mods::StateEntry& e : state.order)
            if (e.id == id) return &e;
        return nullptr;
    }
    bool loads(const std::string& id) const {
        return std::any_of(resolution.load_order.begin(), resolution.load_order.end(),
                           [&](const mods::InstalledMod* m) { return m->manifest.id == id; });
    }

    // Enabled mods this one needs that are installed but disabled (for the "also enable" question).
    std::vector<std::string> disabled_dependencies(const std::string& id) {
        std::vector<std::string> out;
        std::set<std::string> seen;
        std::vector<std::string> todo{id};
        while (!todo.empty()) {
            const std::string cur = todo.back();
            todo.pop_back();
            const mods::InstalledMod* m = find(cur);
            if (m == nullptr) continue;
            for (const mods::Dependency& d : m->manifest.dependencies) {
                if (d.optional || !seen.insert(d.id).second) continue;
                if (mods::StateEntry* e = entry(d.id); e != nullptr && !e->enabled) out.push_back(d.id);
                todo.push_back(d.id);
            }
        }
        return out;
    }
    std::vector<std::string> missing_dependencies(const std::string& id) const {
        std::vector<std::string> out;
        if (const mods::InstalledMod* m = find(id))
            for (const mods::Dependency& d : m->manifest.dependencies)
                if (!d.optional && find(d.id) == nullptr) out.push_back(d.id);
        return out;
    }

    std::string content_text(const mods::InstalledMod& m) const {
        std::string s;
        const auto add = [&](std::size_t n, const char* one, const char* many) {
            if (n == 0) return;
            s += (s.empty() ? "" : ", ") + std::to_string(n) + " " + (n == 1 ? one : many);
        };
        add(m.content.files, "file", "files");
        add(m.content.afs_entries, "voice/AFS entry", "voice/AFS entries");
        add(m.content.textures, "texture", "textures");
        if (!m.manifest.options.empty())
            add(m.manifest.options.size(), "option", "options");
        if (!m.content.unsupported.empty()) {
            const std::size_t items = mods::list_code_items(m).size();
            s += (s.empty() ? "" : ", ") +
                 (items ? std::to_string(items) + (items == 1 ? " code item" : " code items") : std::string("disc patch"));
        }
        return s.empty() ? "nothing" : s;
    }

    std::string status_text(const mods::InstalledMod& m) const {
        if (!m.usable) {
            for (const mods::Issue& i : m.issues)
                if (i.severity == mods::Severity::Error) return "Invalid: " + i.message;
            return "Invalid";
        }
        const mods::StateEntry* e = nullptr;
        for (const mods::StateEntry& x : state.order)
            if (x.id == m.manifest.id) e = &x;
        if (e == nullptr || !e->enabled) return "Off";
        if (!state.mods_enabled) return "On (all mods switched off)";
        if (resolution.skipped.count(m.manifest.id)) {
            for (const mods::Issue& i : resolution.issues)
                if (i.mod_id == m.manifest.id && i.severity == mods::Severity::Error) {
                    const std::size_t colon = i.message.find(": ");
                    return "Not loaded - " + (colon == std::string::npos ? i.message : i.message.substr(colon + 2));
                }
            return "Not loaded";
        }
        std::size_t wins = 0, loses = 0;
        for (const mods::FileOverlay& f : plan.files) {
            if (f.mod_id == m.manifest.id && !f.overridden.empty()) ++wins;
            if (std::find(f.overridden.begin(), f.overridden.end(), m.manifest.id) != f.overridden.end()) ++loses;
        }
        std::string s = "Loads";
        const std::vector<mods::Issue>& region_issues = region_notes_of(m);
        const bool region_gameplay = std::any_of(region_issues.begin(), region_issues.end(), [](const mods::Issue& i) {
            return i.severity == mods::Severity::Warning;
        });
        if (wins || loses)
            s += " (" + std::to_string(wins) + " file" + (wins == 1 ? "" : "s") + " over others, " +
                 std::to_string(loses) + " overridden)";
        if (region_gameplay) s += " - NTSC-U stage layouts, see details";
        return s;
    }

    // Everything about one mod, for the details pane and `info`.
    const std::vector<mods::Issue>& region_notes_of(const mods::InstalledMod& m) const {
        auto it = region_cache.find(m.manifest.id);
        if (it == region_cache.end())
            it = region_cache.emplace(m.manifest.id, mods::region_notes(m, region, game_root / "files")).first;
        return it->second;
    }

    // A printf-style name the game builds ("stg%04d/stg%04d_nrm.dat") against a lower-case path.
    static bool matches_format(std::string_view fmt, std::string_view path) {
        std::size_t i = 0, j = 0;
        while (i < fmt.size()) {
            if (fmt[i] == '%') {
                std::size_t k = i + 1;
                int width = 0;
                while (k < fmt.size() && fmt[k] >= '0' && fmt[k] <= '9') width = width * 10 + (fmt[k++] - '0');
                if (k >= fmt.size()) return false;
                const char conv = fmt[k];
                i = k + 1;
                if (conv == 'd') {
                    int n = 0;
                    while (j < path.size() && path[j] >= '0' && path[j] <= '9' && (width == 0 || n < width)) ++j, ++n;
                    if (n == 0 || (width && n != width)) return false;
                } else if (conv == 'c') {
                    if (j >= path.size()) return false;
                    ++j;
                } else {  // %s: the shortest run that lets the rest match
                    for (std::size_t end = j; end <= path.size(); ++end) {
                        if (end > j && path[end - 1] == '/') return false;
                        if (matches_format(fmt.substr(i), path.substr(end))) return end > j;
                    }
                    return false;
                }
            } else {
                if (j >= path.size() || std::tolower(static_cast<unsigned char>(fmt[i])) != path[j]) return false;
                ++i;
                ++j;
            }
        }
        return j == path.size();
    }

    // Where a mod's files are read: every stage, the front end, named stages and cutscenes; and its files the game
    // never opens. From docs/mods/map/FILE_CONTEXTS.md (observed by traced runs, or derived from names and tables).
    std::string contexts_text(const mods::InstalledMod& m) const {
        const mods::json::Value* files = contexts.find("files");
        if (files == nullptr || !files->is_object()) return {};
        const mods::json::Value* stages = contexts.find("stages");
        std::vector<fs::path> roots{m.folder / "files"};
        for (const mods::ModOption& o : m.manifest.options)
            for (const mods::OptionChoice& c : o.choices)
                if (!c.folder.empty()) roots.push_back(m.folder / mods::from_utf8(c.folder) / "files");
        std::set<std::string> stage_ids, events, other;
        bool every = false;
        std::vector<std::string> never;
        std::size_t observed = 0, derived = 0;
        std::map<std::string, const mods::json::Value*> by_lower;
        for (const auto& [path, v] : files->members()) {
            std::string l = path;
            for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            by_lower[l] = &v;
        }
        for (const fs::path& root : roots) {
            std::error_code ec;
            const fs::path base = mods::long_path(root);
            if (!fs::is_directory(base, ec)) continue;
            for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string rel = mods::utf8(fs::relative(it->path(), base, ec));
                for (char& c : rel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const auto f = by_lower.find(rel);
                if (f == by_lower.end()) {
                    bool named = false;
                    if (const mods::json::Value* fmts = contexts.find("dol_name_formats"); fmts && fmts->is_array())
                        for (const mods::json::Value& x : fmts->items())
                            if (matches_format(x.as_string(), rel) ||
                                matches_format(x.as_string(), rel.substr(rel.rfind('/') + 1))) named = true;
                    if (const mods::json::Value* names = contexts.find("dol_names"); !named && names && names->is_array())
                        for (const mods::json::Value& x : names->items()) {
                            std::string n = x.as_string();
                            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (n == rel || n == rel.substr(rel.rfind('/') + 1)) named = true;
                        }
                    if (!named) never.push_back(rel);
                    continue;
                }
                const mods::json::Value& info = *f->second;
                if (const mods::json::Value* src = info.find("source"); src && src->as_string() == "observed") ++observed;
                else ++derived;
                if (const mods::json::Value* cs = info.find("contexts"); cs && cs->is_array())
                    for (const mods::json::Value& c : cs->items()) {
                        const std::string& t = c.as_string();
                        if (t == "all-stages") every = true;
                        else if (t.rfind("stage:", 0) == 0) stage_ids.insert(t.substr(6));
                        else if (t.rfind("event:", 0) == 0) events.insert(t.substr(6));
                        else if (t.rfind("lang:", 0) != 0) other.insert(t);
                    }
            }
        }
        if (stage_ids.empty() && events.empty() && other.empty() && !every && never.empty()) return {};
        std::string s = "Affects: ";
        std::vector<std::string> parts;
        if (every) parts.push_back("every stage");
        if (!stage_ids.empty()) {
            std::string names;
            std::size_t n = 0;
            for (const std::string& id : stage_ids) {
                if (++n > 6) break;
                std::string name = id;
                if (stages)
                    if (const mods::json::Value* st = stages->find(id))
                        if (const mods::json::Value* nm = st->find("name")) name = nm->as_string();
                names += (names.empty() ? "" : ", ") + name;
            }
            parts.push_back(std::to_string(stage_ids.size()) + (stage_ids.size() == 1 ? " stage (" : " stages (") + names +
                            (stage_ids.size() > 6 ? ", ...)" : ")"));
        }
        if (!events.empty()) parts.push_back(std::to_string(events.size()) + (events.size() == 1 ? " cutscene" : " cutscenes"));
        static const std::map<std::string, std::string> kReadable = {
            {"frontend", "menus"},          {"in-stage-ui", "in-stage HUD"}, {"mode:2p-battle", "2P battle"},
            {"mode:expert", "Expert mode"}, {"mode:last-story", "Last Story"}, {"movie", "movies"},
            {"credits", "credits"}};
        for (const std::string& o : other) {
            if (o == "boot" || o == "named-in-code") continue;  // internal: loaded at start-up / a name in the code
            const auto r = kReadable.find(o);
            parts.push_back(r != kReadable.end() ? r->second : o);
        }
        for (std::size_t i = 0; i < parts.size(); ++i) s += (i ? ", " : "") + parts[i];
        s += "\n  (" + std::to_string(observed) + " files seen read in traced runs, " + std::to_string(derived) +
             " placed by name or table)\n";
        if (!never.empty()) {
            s += "Never opened by the game (" + std::to_string(never.size()) + "; installed but unused):";
            for (std::size_t i = 0; i < never.size() && i < 10; ++i) s += (i ? ", " : " ") + never[i];
            s += never.size() > 10 ? ", ...\n" : "\n";
        }
        return s;
    }

    // Reloaded's code features this build cannot run, when the mod ships Reloaded's code material (its MCM
    // libraries or Gecko ini under codes/). From docs/mods/map/RELOADED_CODE.md.
    std::string reloaded_text(const mods::InstalledMod& m) const {
        const mods::json::Value* features = reloaded.find("features");
        if (features == nullptr || !features->is_array()) return {};
        std::set<std::string> libraries;
        for (const mods::json::Value& f : features->items())
            if (const mods::json::Value* entries = f.find("library_entries"); entries && entries->is_array())
                for (const mods::json::Value& e : entries->items())
                    if (const mods::json::Value* file = e.find("file")) libraries.insert(file->as_string());
        libraries.insert("GUPR8P.ini");
        bool ships = false;
        std::error_code ec;
        const fs::path codes = mods::long_path(m.folder / "codes");
        for (fs::recursive_directory_iterator it(codes, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && libraries.count(mods::utf8(it->path().filename()))) ships = true;
        if (!ships) return {};
        std::string s, inert;
        std::size_t missing = 0, inert_files = 0;
        for (const mods::json::Value& f : features->items()) {
            const std::string kind = f.find("kind") ? f.find("kind")->as_string() : "";
            if (kind == "data") continue;
            ++missing;
            const std::string name = f.find("name") ? f.find("name")->as_string() : "?";
            const std::string effect = f.find("effect_without_code") ? f.find("effect_without_code")->as_string() : "";
            const std::string evidence = f.find("evidence") ? f.find("evidence")->as_string() : "";
            s += "  - " + name + (evidence == "observed" ? " [seen in game]" : "") + ": " + effect + "\n";
            if (const mods::json::Value* dd = f.find("dependent_data"); dd && dd->is_array())
                for (const mods::json::Value& d : dd->items()) {
                    const double n = d.find("files") ? d.find("files")->as_number() : 0.0;
                    if (n <= 0) continue;
                    inert_files += static_cast<std::size_t>(n);
                    inert += "  " + (d.find("pattern") ? d.find("pattern")->as_string() : std::string("?")) + " (" +
                             std::to_string(static_cast<long long>(n)) + ", for " + name + ")\n";
                }
        }
        std::string out = "\nReloaded features this build does not have (" + std::to_string(missing) +
                          "; they are code in Reloaded's own executable, see docs/mods/map/RELOADED_CODE.md):\n" + s;
        if (inert_files)
            out += "\nInstalled data tied to those features (patterns may overlap; each file loads where the stock "
                   "game asks for it, otherwise it is unused):\n" + inert;
        return out;
    }

    // The spoken lines a mod's voice entries replace, with their subtitles (read from the player's own tables).
    std::string voice_text(const mods::InstalledMod& m) const {
        std::error_code ec;
        const fs::path dir = mods::long_path(m.folder / "afs" / "PRS_VOICE_E");
        if (!fs::is_directory(dir, ec)) return {};
        std::vector<std::string> names;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) names.push_back(mods::utf8(it->path().filename()));
        std::sort(names.begin(), names.end());
        std::string s = "\nVoice lines replaced (" + std::to_string(names.size()) + "):\n";
        std::size_t shown = 0;
        for (const std::string& n : names) {
            if (++shown > 20) {
                s += "  ...\n";
                break;
            }
            const std::vector<mods::VoiceLine>* lines = voices.find(n);
            if (lines == nullptr || lines->empty()) {
                s += "  " + n + " (no English subtitle refers to it)\n";
                continue;
            }
            // The first line that has text (background voices often carry an empty subtitle).
            const mods::VoiceLine& l = *std::find_if(lines->begin(), lines->end() - 1,
                                                     [](const mods::VoiceLine& v) { return !v.text.empty(); });
            std::string stage = l.table;
            if (const std::size_t a = stage.find('/'), b = stage.rfind('/'); a != std::string::npos && b > a)
                stage = stage.substr(a + 1, b - a - 1);
            std::string text = l.text;
            // The mod's own subtitle table for that stage, when it ships one, is what the player will read.
            if (const fs::path own = m.folder / "files" / mods::from_utf8(l.table); fs::is_regular_file(own, ec)) {
                std::vector<mods::FntEntry> table;
                std::string e;
                std::string lower_name = n;
                for (char& c : lower_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const auto idx = voices.entry_index.find(lower_name);
                if (mods::read_fnt(own, table, e) && idx != voices.entry_index.end()) {
                    const auto by_key = std::find_if(table.begin(), table.end(), [&](const mods::FntEntry& f) {
                        return f.key == l.key && f.audio_id == static_cast<std::int32_t>(idx->second);
                    });
                    const auto by_id = std::find_if(table.begin(), table.end(), [&](const mods::FntEntry& f) {
                        return f.audio_id == static_cast<std::int32_t>(idx->second);
                    });
                    if (by_key != table.end()) text = by_key->text;
                    else if (by_id != table.end()) text = by_id->text;
                }
            }
            std::replace(text.begin(), text.end(), '\n', ' ');
            if (text.size() > 90) text = text.substr(0, 87) + "...";
            if (text.empty()) text = "(no subtitle text; " + n + ")";
            s += "  " + stage + ": \"" + text + "\"" +
                 (lines->size() > 1 ? " (+" + std::to_string(lines->size() - 1) + " more)" : "") + "\n";
        }
        return s;
    }

    std::string details(const mods::InstalledMod& m) const {
        std::string s;
        const mods::Manifest& mf = m.manifest;
        s += mf.name + "\n";
        s += "Version " + mf.version + (mf.author.empty() ? "" : "  by " + mf.author) + "\n";
        s += "Id: " + mf.id + "    Folder: " + m.folder_name + "\n";
        if (!mf.game_id.empty()) s += "Made for disc: " + mf.game_id + " (this game reads the PAL disc, GUPP8P)\n";
        if (!mf.website.empty()) s += "Website: " + mf.website + "\n";
        s += "Content: " + content_text(m) + "\n";
        s += "Status: " + status_text(m) + "\n";
        if (m.usable) s += contexts_text(m);
        if (m.content.afs_entries) s += "Voice entries are read by every stage's dialogue.\n";
        if (!mf.dependencies.empty()) {
            s += "Requires:";
            for (const mods::Dependency& d : mf.dependencies)
                s += " " + d.id + (d.min_version.empty() ? "" : " " + d.min_version + "+") + (d.optional ? " (optional)" : "");
            s += "\n";
        }
        if (!mf.incompatible.empty()) {
            s += "Incompatible with:";
            for (const std::string& i : mf.incompatible) s += " " + i;
            s += "\n";
        }
        if (!mf.description.empty()) s += "\n" + mf.description + "\n";
        std::string problems;
        for (const mods::Issue& i : m.issues)
            problems += std::string("- ") + mods::severity_name(i.severity) + ": " + i.message + "\n";
        for (const std::string& u : m.content.unsupported) (void)u;  // already in m.issues
        for (const mods::Issue& i : resolution.issues)
            if (i.mod_id == mf.id) problems += std::string("- ") + mods::severity_name(i.severity) + ": " + i.message + "\n";
        for (const mods::Issue& i : plan.issues)
            if (i.mod_id == mf.id && i.severity != mods::Severity::Info)
                problems += std::string("- ") + mods::severity_name(i.severity) + ": " + i.message + "\n";
        std::string over, under;
        std::size_t n_over = 0, n_under = 0;
        for (const mods::FileOverlay& f : plan.files) {
            if (f.mod_id == mf.id && !f.overridden.empty()) {
                if (++n_over <= 12) over += "  " + f.game_path + " (over " + f.overridden.front() + ")\n";
            }
            if (std::find(f.overridden.begin(), f.overridden.end(), mf.id) != f.overridden.end()) {
                if (++n_under <= 12) under += "  " + f.game_path + " (" + f.mod_id + " wins)\n";
            }
        }
        for (const mods::AfsOverlay& a : plan.afs)
            for (const mods::AfsEntryOverlay& e : a.entries) {
                if (e.mod_id == mf.id && !e.overridden.empty() && ++n_over <= 12)
                    over += "  " + a.archive_path + ":" + e.entry_name + " (over " + e.overridden.front() + ")\n";
                if (std::find(e.overridden.begin(), e.overridden.end(), mf.id) != e.overridden.end() && ++n_under <= 12)
                    under += "  " + a.archive_path + ":" + e.entry_name + " (" + e.mod_id + " wins)\n";
            }
        if (n_over) s += "\nFile conflicts this mod wins (" + std::to_string(n_over) + "):\n" + over + (n_over > 12 ? "  ...\n" : "");
        if (n_under) s += "\nFile conflicts this mod loses (" + std::to_string(n_under) + "):\n" + under + (n_under > 12 ? "  ...\n" : "");
        s += voice_text(m);
        if (const std::vector<mods::CodeItem> items = mods::list_code_items(m); !items.empty()) {
            const mods::StateEntry* e = nullptr;
            for (const mods::StateEntry& x : state.order)
                if (x.id == m.manifest.id) e = &x;
            std::size_t chosen = 0;
            for (const mods::CodeItem& c : items) chosen += e && e->codes.count(c.id) && e->codes.at(c.id) ? 1 : 0;
            s += "\nCode: " + std::to_string(items.size()) + (items.size() == 1 ? " item, " : " items, ") +
                 std::to_string(chosen) + " chosen (Code... button)\n";
        }
        s += reloaded_text(m);
        if (m.usable) {
            std::string region_text;
            for (const mods::Issue& i : region_notes_of(m))
                region_text += std::string("- ") + mods::severity_name(i.severity) + ": " + i.message + "\n";
            if (!region_text.empty()) s += "\nRegion (this game reads the PAL disc):\n" + region_text;
        }
        for (const mods::TextureConflict& t : texture_conflicts) {
            if (t.winner == mf.id)
                s += "\nTextures this mod wins over " + t.loser + ": " + std::to_string(t.count) + " (e.g. " +
                     t.examples.front() + ")\n";
            if (t.loser == mf.id)
                s += "\nTextures " + t.winner + " wins over this mod: " + std::to_string(t.count) + " (e.g. " +
                     t.examples.front() + ")\n";
        }
        if (!problems.empty()) s += "\nNotes:\n" + problems;
        return s;
    }

    std::string summary() const {
        std::size_t enabled = 0;
        for (const mods::StateEntry& e : state.order) enabled += e.enabled ? 1 : 0;
        std::size_t conflicts = 0;
        for (const mods::FileOverlay& f : plan.files) conflicts += f.overridden.empty() ? 0 : 1;
        for (const mods::AfsOverlay& a : plan.afs)
            for (const mods::AfsEntryOverlay& e : a.entries) conflicts += e.overridden.empty() ? 0 : 1;
        std::size_t afs_entries = 0;
        for (const mods::AfsOverlay& a : plan.afs) afs_entries += a.entries.size();
        std::string s = std::to_string(installed.size()) + " installed, " + std::to_string(enabled) + " enabled, " +
                        std::to_string(resolution.load_order.size()) + " load";
        if (!state.mods_enabled) s = std::to_string(installed.size()) + " installed - all mods switched off";
        s += " | " + std::to_string(plan.files.size()) + " files, " + std::to_string(afs_entries) + " AFS entries, " +
             std::to_string(plan.texture_dirs.size()) + " texture packs | " + std::to_string(conflicts) +
             " file conflict" + (conflicts == 1 ? "" : "s");
        std::size_t texture_clashes = 0;
        for (const mods::TextureConflict& t : texture_conflicts) texture_clashes += t.count;
        if (texture_clashes) s += ", " + std::to_string(texture_clashes) + " texture conflict" + (texture_clashes == 1 ? "" : "s");
        if (!resolution.skipped.empty()) s += " | " + std::to_string(resolution.skipped.size()) + " cannot load";
        return s;
    }

    // Moves a mod to a position in the priority list (0 = highest), clamped.
    void move_to(const std::string& id, std::size_t position) {
        auto it = std::find_if(state.order.begin(), state.order.end(), [&](const mods::StateEntry& e) { return e.id == id; });
        if (it == state.order.end()) return;
        mods::StateEntry e = *it;
        state.order.erase(it);
        position = std::min(position, state.order.size());
        state.order.insert(state.order.begin() + static_cast<std::ptrdiff_t>(position), e);
    }
    std::size_t position_of(const std::string& id) const {
        for (std::size_t i = 0; i < state.order.size(); ++i)
            if (state.order[i].id == id) return i;
        return state.order.size();
    }
    // Enabled mods that need `id` (directly or through another mod), for "disable them too?".
    std::vector<std::string> enabled_dependents(const std::string& id) const {
        std::vector<std::string> out;
        std::set<std::string> found{id};
        for (bool again = true; again;) {
            again = false;
            for (const mods::StateEntry& e : state.order) {
                if (!e.enabled || found.count(e.id)) continue;
                const mods::InstalledMod* m = find(e.id);
                if (m == nullptr) continue;
                for (const mods::Dependency& d : m->manifest.dependencies)
                    if (!d.optional && found.count(d.id)) {
                        found.insert(e.id);
                        out.push_back(e.id);
                        again = true;
                        break;
                    }
            }
        }
        return out;
    }
    std::vector<mods::TextureConflict> texture_conflicts;  // between the active texture packs

    void move(const std::string& id, int delta) {
        for (std::size_t i = 0; i < state.order.size(); ++i)
            if (state.order[i].id == id) {
                const long long j = static_cast<long long>(i) + delta;
                if (j < 0 || j >= static_cast<long long>(state.order.size())) return;
                std::swap(state.order[i], state.order[static_cast<std::size_t>(j)]);
                return;
            }
    }
};

// ---- command line ------------------------------------------------------------------------------------------------

void out(const std::string& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) h = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        const std::wstring w = wide(s);
        DWORD n = 0;
        WriteConsoleW(h, w.data(), static_cast<DWORD>(w.size()), &n, nullptr);
    } else {
        DWORD n = 0;
        WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
    }
}

void print_issues(const std::vector<mods::Issue>& issues, bool infos) {
    for (const mods::Issue& i : issues)
        if (infos || i.severity != mods::Severity::Info)
            out(std::string("  ") + mods::severity_name(i.severity) + ": " + (i.mod_id.empty() ? "" : i.mod_id + ": ") +
                i.message + "\n");
}

const char kHelp[] =
    "ModManager - mods for Shadow the Hedgehog (PC, a static recompilation made with GekkoForge)\n"
    "\n"
    "Without arguments: the window. Commands (state is saved to <mods dir>/mods.json):\n"
    "  list                          installed mods in priority order (highest first)\n"
    "  info <id>                     everything about one mod\n"
    "  install <archive|folder>...   install downloads (.zip, .7z, .rar, folders); --replace updates, --name N\n"
    "  enable <id> | disable <id>    switch a mod on or off (enable also enables disabled dependencies)\n"
    "  remove <id>                   delete an installed mod's folder\n"
    "  move <id> <position>          set the priority (1 = highest)\n"
    "  option <id> <option> <choice> choose an option of a mod\n"
    "  master on|off                 load mods at all\n"
    "  enable-all | disable-all      switch every installed mod on or off\n"
    "  codes <id>                    the code items a mod offers and which are chosen\n"
    "  code <id> <item> on|off       choose a code item (applied through tools/mods/codemods/build_variant.py)\n"
    "  code-signature                the signature of the chosen code of the enabled mods (empty: none)\n"
    "  code-plan                     the chosen code items of the enabled mods as JSON (for build_variant.py)\n"
    "  profile list|save|load|delete [<name>]   named sets of enabled mods, order and options\n"
    "  set-info <id> <field> <value> edit a mod's name, version, author, website or description\n"
    "  check                         resolve and plan; exit code 1 when an enabled mod cannot load\n"
    "  validate <folder>             check every ONE/TXD/DFF/AFS/ADX file below a folder for damage\n"
    "  plan                          the resolved plan as JSON (load order, every file and its source, AFS\n"
    "                                entries, texture packs, issues) for scripts and tests\n"
    "Options: --mods-dir <dir> (default: mods\\ beside this program), --game-data <dir> (default: as the game)\n";

int run_cli(std::vector<std::string> args) {
    std::optional<fs::path> mods_dir, game_data;
    bool replace = false;
    std::string name_hint;
    std::vector<std::string> rest;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--mods-dir" && i + 1 < args.size()) mods_dir = mods::from_utf8(args[++i]);
        else if (args[i] == "--game-data" && i + 1 < args.size()) game_data = mods::from_utf8(args[++i]);
        else if (args[i] == "--replace") replace = true;
        else if (args[i] == "--name" && i + 1 < args.size()) name_hint = args[++i];
        else rest.push_back(args[i]);
    }
    if (rest.empty() || rest[0] == "help" || rest[0] == "--help" || rest[0] == "-h") {
        out(kHelp);
        return 0;
    }
    Model m;
    m.mods_dir = mods_dir ? *mods_dir : executable_dir() / "mods";
    const auto root = find_game_root(game_data);
    if (!root) {
        out("error: the game's data (a folder with files/) was not found; pass --game-data\n");
        return 2;
    }
    m.game_root = *root;
    m.index_game();
    if (!m.game_error.empty()) {
        out("error: " + m.game_error + "\n");
        return 2;
    }
    m.refresh();
    if (!m.state_error.empty()) {
        out("error: " + m.state_error + " (fix or delete it; nothing was changed)\n");
        return 2;
    }
    const std::string& cmd = rest[0];
    const auto need = [&](std::size_t n) {
        if (rest.size() < n + 1) {
            out("error: '" + cmd + "' needs " + std::to_string(n) + " argument(s)\n");
            return false;
        }
        return true;
    };
    const auto known = [&](const std::string& id) {
        if (m.find(id) == nullptr) {
            out("error: no usable installed mod has the id '" + id + "'\n");
            return false;
        }
        return true;
    };
    if (cmd == "list") {
        out(std::string("mods ") + (m.state.mods_enabled ? "on" : "OFF") + ", folder " + mods::utf8(m.mods_dir) + "\n");
        int n = 0;
        for (const mods::StateEntry& e : m.state.order) {
            const mods::InstalledMod* mod = m.find(e.id);
            out(std::to_string(++n) + ". [" + (e.enabled ? "x" : " ") + "] " + e.id + "  " + mod->manifest.name + " " +
                mod->manifest.version + "  | " + m.content_text(*mod) + " | " + m.status_text(*mod) + "\n");
        }
        for (const mods::InstalledMod& mod : m.installed)
            if (!mod.usable) out("   [!] " + mod.folder_name + "  | " + m.status_text(mod) + "\n");
        out(m.summary() + "\n");
        return 0;
    }
    if (cmd == "info") {
        if (!need(1) || !known(rest[1])) return 2;
        out(m.details(*m.find(rest[1])));
        return 0;
    }
    if (cmd == "install") {
        if (!need(1)) return 2;
        int failures = 0;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            mods::InstallRequest req{mods::from_utf8(rest[i]), m.mods_dir, &m.game, replace, name_hint};
            const mods::InstallResult r = mods::install(req);
            if (r.ok) {
                const auto count = [](std::size_t n, const char* one, const char* many) {
                    return std::to_string(n) + " " + (n == 1 ? one : many);
                };
                out("installed " + r.id + " (" + r.manifest.name + "): " + count(r.content.files, "file", "files") + ", " +
                    count(r.content.afs_entries, "AFS entry", "AFS entries") + ", " +
                    count(r.content.textures, "texture", "textures") + ", " +
                    count(r.manifest.options.size(), "option", "options") + "\n");
            } else {
                out("FAILED " + rest[i] + "\n");
                ++failures;
            }
            print_issues(r.issues, true);
            for (const std::string& l : r.left_out) out("  left out: " + l + "\n");
        }
        m.refresh();
        return failures ? 1 : 0;
    }
    if (cmd == "enable" || cmd == "disable") {
        if (!need(1) || !known(rest[1])) return 2;
        const bool on = cmd == "enable";
        m.entry(rest[1])->enabled = on;
        if (on) {
            for (const std::string& d : m.disabled_dependencies(rest[1])) {
                m.entry(d)->enabled = true;
                out("also enabled " + d + " (required)\n");
            }
            for (const std::string& d : m.missing_dependencies(rest[1]))
                out("warning: requires '" + d + "', which is not installed\n");
        }
        m.save();
        m.replan();
        out(m.summary() + "\n");
        return 0;
    }
    if (cmd == "remove") {
        if (!need(1) || !known(rest[1])) return 2;
        std::string error;
        if (!mods::remove_installed(m.mods_dir, m.find(rest[1])->folder, error)) {
            out("error: " + error + "\n");
            return 1;
        }
        m.refresh();
        out("removed " + rest[1] + "\n");
        return 0;
    }
    if (cmd == "move") {
        if (!need(2) || !known(rest[1])) return 2;
        const long long pos = std::stoll(rest[2]) - 1;
        auto it = std::find_if(m.state.order.begin(), m.state.order.end(),
                               [&](const mods::StateEntry& e) { return e.id == rest[1]; });
        mods::StateEntry e = *it;
        m.state.order.erase(it);
        const std::size_t at = static_cast<std::size_t>(std::clamp<long long>(pos, 0, static_cast<long long>(m.state.order.size())));
        m.state.order.insert(m.state.order.begin() + static_cast<std::ptrdiff_t>(at), e);
        m.save();
        m.replan();
        out(m.summary() + "\n");
        return 0;
    }
    if (cmd == "option") {
        if (!need(3) || !known(rest[1])) return 2;
        const mods::InstalledMod* mod = m.find(rest[1]);
        for (const mods::ModOption& o : mod->manifest.options)
            if (o.id == rest[2])
                for (const mods::OptionChoice& c : o.choices)
                    if (c.id == rest[3]) {
                        m.entry(rest[1])->options[o.id] = c.id;
                        m.save();
                        m.replan();
                        out(rest[1] + ": " + o.id + " = " + c.id + "\n");
                        return 0;
                    }
        out("error: no such option or choice\n");
        return 2;
    }
    if (cmd == "codes") {
        if (!need(1) || !known(rest[1])) return 2;
        const mods::StateEntry* e = m.entry(rest[1]);
        for (const mods::CodeItem& c : mods::list_code_items(*m.find(rest[1]))) {
            const bool on = e && e->codes.count(c.id) && e->codes.at(c.id);
            out(std::string("[") + (on ? "x" : " ") + "] " + c.id + "  " + c.label + "\n");
        }
        return 0;
    }
    if (cmd == "code") {
        if (!need(3) || !known(rest[1])) return 2;
        bool offered = false;
        for (const mods::CodeItem& c : mods::list_code_items(*m.find(rest[1]))) offered |= c.id == rest[2];
        if (!offered) return out("error: " + rest[1] + " offers no code item '" + rest[2] + "' (see: codes " + rest[1] + ")\n"), 2;
        m.entry(rest[1])->codes[rest[2]] = rest[3] == "on";
        m.save();
        out(rest[1] + ": " + rest[2] + " " + (rest[3] == "on" ? "on" : "off") + "; signature " +
            mods::code_selection_signature(m.installed, m.state) + "\n");
        return 0;
    }
    if (cmd == "code-plan") {
        // The chosen code of the enabled mods, in load order, for tools/mods/codemods/build_variant.py.
        namespace mj = mods::json;
        mj::Value v = mj::Value::object();
        v.set("signature", mj::Value::string(mods::code_selection_signature(m.installed, m.state)));
        v.set("mods_dir", mj::Value::string(mods::utf8(m.mods_dir)));
        mj::Value items = mj::Value::array();
        for (auto it = m.state.order.rbegin(); it != m.state.order.rend(); ++it) {
            if (!it->enabled || !m.state.mods_enabled) continue;
            const mods::InstalledMod* mod = m.find(it->id);
            if (mod == nullptr) continue;
            for (const mods::CodeItem& c : mods::list_code_items(*mod)) {
                auto chosen = it->codes.find(c.id);
                if (chosen == it->codes.end() || !chosen->second) continue;
                mj::Value o = mj::Value::object();
                o.set("mod", mj::Value::string(it->id));
                o.set("game_id", mj::Value::string(mod->manifest.game_id));
                o.set("item", mj::Value::string(c.id));
                o.set("label", mj::Value::string(c.label));
                o.set("input", mj::Value::string(c.input));
                items.push(std::move(o));
            }
        }
        v.set("items", std::move(items));
        out(mj::write(v));
        return 0;
    }
    if (cmd == "code-signature") {
        out(mods::code_selection_signature(m.installed, m.state) + "\n");
        return 0;
    }
    if (cmd == "enable-all" || cmd == "disable-all") {
        for (mods::StateEntry& e : m.state.order) e.enabled = cmd == "enable-all";
        m.save();
        m.replan();
        out(m.summary() + "\n");
        return 0;
    }
    if (cmd == "profile") {
        if (!need(1)) return 2;
        std::string error;
        const std::string& what = rest[1];
        if (what == "list") {
            for (const std::string& p : mods::list_profiles(m.mods_dir)) out(p + "\n");
            return 0;
        }
        if (!need(2)) return 2;
        const std::string& name = rest[2];
        if (what == "save") {
            if (!mods::save_profile(m.mods_dir, name, m.state, error)) return out("error: " + error + "\n"), 1;
            out("saved profile '" + name + "'\n");
            return 0;
        }
        if (what == "load") {
            mods::State loaded;
            if (!mods::load_profile(m.mods_dir, name, loaded, error)) return out("error: " + error + "\n"), 1;
            mods::reconcile(loaded, m.installed);
            m.state = loaded;
            m.save();
            m.replan();
            out("loaded profile '" + name + "': " + m.summary() + "\n");
            return 0;
        }
        if (what == "delete") {
            if (!mods::delete_profile(m.mods_dir, name, error)) return out("error: " + error + "\n"), 1;
            out("deleted profile '" + name + "'\n");
            return 0;
        }
        out("error: profile list|save|load|delete\n");
        return 2;
    }
    if (cmd == "set-info") {
        if (!need(3) || !known(rest[1])) return 2;
        mods::Manifest mf = m.find(rest[1])->manifest;
        const std::string& field = rest[2];
        const std::string& value = rest[3];
        if (field == "name") mf.name = value;
        else if (field == "version") {
            if (!mods::Version::parse(value)) return out("error: '" + value + "' is not a version\n"), 2;
            mf.version = value;
        } else if (field == "author") mf.author = value;
        else if (field == "website") mf.website = value;
        else if (field == "description") mf.description = value;
        else return out("error: field is name, version, author, website or description\n"), 2;
        mf.extra.set("user_edited", mods::json::Value::boolean(true));  // an update keeps these edits
        std::string error;
        if (!mods::write_manifest(m.find(rest[1])->folder / std::string(mods::kManifestName), mf, error))
            return out("error: " + error + "\n"), 1;
        out(rest[1] + ": " + field + " = " + value + "\n");
        return 0;
    }
    if (cmd == "master") {
        if (!need(1)) return 2;
        m.state.mods_enabled = rest[1] == "on";
        m.save();
        out(std::string("mods ") + (m.state.mods_enabled ? "on" : "off") + "\n");
        return 0;
    }
    if (cmd == "validate") {
        if (!need(1)) return 2;
        std::size_t checked = 0, bad = 0;
        std::error_code ec;
        const fs::path folder = mods::long_path(mods::from_utf8(rest[1]));
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            ++checked;
            std::string why;
            if (!mods::check_file_format(it->path(), mods::utf8(it->path().filename()), why)) {
                ++bad;
                out("DAMAGED " + mods::utf8(it->path()) + " " + why + "\n");
            }
        }
        out(std::to_string(checked) + " files, " + std::to_string(bad) + " damaged\n");
        return bad ? 1 : 0;
    }
    if (cmd == "plan") {
        namespace mj = mods::json;
        mj::Value v = mj::Value::object();
        v.set("mods_enabled", mj::Value::boolean(m.state.mods_enabled));
        mj::Value order = mj::Value::array();
        for (const mods::InstalledMod* mod : m.resolution.load_order) {
            mj::Value o = mj::Value::object();
            o.set("id", mj::Value::string(mod->manifest.id));
            o.set("version", mj::Value::string(mod->manifest.version));
            o.set("folder", mj::Value::string(mods::utf8(mod->folder)));
            order.push(std::move(o));
        }
        v.set("load_order", std::move(order));  // lowest priority first
        mj::Value skipped = mj::Value::array();
        for (const std::string& id : m.resolution.skipped) skipped.push(mj::Value::string(id));
        v.set("skipped", std::move(skipped));
        mj::Value files = mj::Value::array();
        for (const mods::FileOverlay& f : m.plan.files) {
            mj::Value o = mj::Value::object();
            o.set("game_path", mj::Value::string(f.game_path));
            o.set("host", mj::Value::string(mods::utf8(f.host)));
            o.set("mod", mj::Value::string(f.mod_id));
            o.set("added", mj::Value::boolean(f.added));
            mj::Value over = mj::Value::array();
            for (const std::string& x : f.overridden) over.push(mj::Value::string(x));
            o.set("overridden", std::move(over));
            files.push(std::move(o));
        }
        v.set("files", std::move(files));
        mj::Value afs = mj::Value::array();
        for (const mods::AfsOverlay& a : m.plan.afs) {
            mj::Value o = mj::Value::object();
            o.set("archive", mj::Value::string(a.archive_path));
            mj::Value entries = mj::Value::array();
            for (const mods::AfsEntryOverlay& e : a.entries) {
                mj::Value x = mj::Value::object();
                x.set("entry", mj::Value::string(e.entry_name));
                x.set("host", mj::Value::string(mods::utf8(e.host)));
                x.set("mod", mj::Value::string(e.mod_id));
                entries.push(std::move(x));
            }
            o.set("entries", std::move(entries));
            afs.push(std::move(o));
        }
        v.set("afs", std::move(afs));
        mj::Value packs = mj::Value::array();
        for (const fs::path& d : m.plan.texture_dirs) packs.push(mj::Value::string(mods::utf8(d)));
        v.set("texture_dirs", std::move(packs));
        mj::Value issues = mj::Value::array();
        const auto add = [&](const std::vector<mods::Issue>& list) {
            for (const mods::Issue& i : list) {
                mj::Value o = mj::Value::object();
                o.set("severity", mj::Value::string(mods::severity_name(i.severity)));
                o.set("mod", mj::Value::string(i.mod_id));
                o.set("message", mj::Value::string(i.message));
                issues.push(std::move(o));
            }
        };
        add(m.resolution.issues);
        add(m.plan.issues);
        v.set("issues", std::move(issues));
        out(mj::write(v));
        return 0;
    }
    if (cmd == "check") {
        out(m.summary() + "\nload order (lowest priority first):\n");
        for (const mods::InstalledMod* mod : m.resolution.load_order)
            out("  " + mod->manifest.id + " " + mod->manifest.version + "\n");
        print_issues(m.discover_issues, false);
        for (const mods::InstalledMod& mod : m.installed) {
            const mods::StateEntry* e = m.entry(mod.manifest.id);
            if ((e && e->enabled) || !mod.usable) print_issues(mod.issues, false);
            if (e && e->enabled && mod.usable) print_issues(m.region_notes_of(mod), true);
        }
        print_issues(m.resolution.issues, true);
        print_issues(m.plan.issues, true);
        return m.resolution.skipped.empty() ? 0 : 1;
    }
    out("error: unknown command '" + cmd + "'\n" + kHelp);
    return 2;
}

// ---- the window ----------------------------------------------------------------------------------------------------

enum : int {
    kIdList = 100,
    kIdInstall,
    kIdRemove,
    kIdUp,
    kIdDown,
    kIdFolder,
    kIdRefresh,
    kIdMaster,
    kIdPlay,
    kIdDetails,
    kIdPreview,
    kIdStatus,
    kIdFilter,
    kIdProfiles,
    kIdCode,
    kIdProgress,
    kIdTextSize,
    kIdColours,
    kIdOptionLabel0 = 200,
    kIdOptionCombo0 = 220,
    // context menu and keyboard commands
    kCmdToggle = 300,
    kCmdTop,
    kCmdBottom,
    kCmdEnableAll,
    kCmdDisableAll,
    kCmdEditInfo,
    kCmdFocusFilter,
    kCmdTextLarger,
    kCmdTextSmaller,
    kCmdTextNormal,
    kCmdProfileSave = 400,
    kCmdProfileLoad0 = 500,
    kCmdProfileDelete0 = 700,
};
constexpr int kMaxOptions = 6;
constexpr UINT kMsgInstalled = WM_APP + 1;
constexpr UINT kMsgLook = WM_APP + 3;  // wparam: ui::Theme, lparam: text size

struct Gui {
    Model model;
    HWND details_label = nullptr, list_label = nullptr;
    HWND window = nullptr, list = nullptr, details = nullptr, preview = nullptr, status = nullptr, master = nullptr;
    HWND buttons[8] = {};
    HWND option_labels[kMaxOptions] = {}, option_combos[kMaxOptions] = {};
    HFONT font = nullptr, bold = nullptr;
    HBITMAP preview_bitmap = nullptr;
    int preview_w = 0, preview_h = 0;
    UINT dpi = 96;
    bool filling = false;
    std::atomic<bool> busy{false};
    std::string selected;  // mod id or "!folder" for an unusable one
    fs::path game_exe;
    fs::path snapshot;     // --snapshot: where the window's picture goes
    HWND filter = nullptr, profiles = nullptr, progress = nullptr, code = nullptr;
    std::vector<std::string> row_keys;      // the key of each list row as shown (the filter hides rows)
    std::vector<COLORREF> row_status_color;  // the status column's colour per row (CLR_DEFAULT: the theme's)
    std::string drag_key;                    // the mod being dragged to a new place, empty when none
    HACCEL accelerators = nullptr;
    std::vector<std::string> profile_names;  // as offered in the last Profiles menu
    ui::Appearance look;                     // the text size and colours (appearance.cfg beside the manager)
    HWND text_size = nullptr, colours = nullptr;
    bool look_pending = false;  // a colour change waits for a pop-up window to close
    RECT panel{};  // the card behind the selected mod's preview, options and details
    HWND card = nullptr;   // the panel on the card that holds the preview, the options and the details; it scrolls
    int card_scroll = 0;   // when they are taller than it (a short window at a large zoom)
    int card_content_h = 0;
    std::string card_mod;  // the mod the card shows (a new one opens at its top)
    int columns_w = -1, columns_zoom = 0;  // the list's width and zoom its columns were last set for
    int status_w = 0, status_h = 0;  // the summary's room in the bottom bar, in pixels (layout())
    HWND tips = nullptr;             // the buttons' names, shown when they show only their icons
    bool icons_only = false;
    bool checking = false;           // --check-layout: zoom changes are not stored

    // At the screen's scale and the chosen text size.
    int px(int v) const { return MulDiv(v, static_cast<int>(dpi) * look.text_size, 96 * 100); }
};
Gui* g = nullptr;

LRESULT popup_colors(UINT msg, WPARAM wp, LPARAM lp, HWND w, bool* handled);
void apply_pending_look();

struct InstallJob {
    std::vector<fs::path> sources;
    std::vector<mods::InstallResult> results;
    bool replace = false;  // the user agreed to replace installed copies of the same mods
};

HBITMAP load_image(const fs::path& file, int& w, int& h) {
    w = h = 0;
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return nullptr;
    HBITMAP bitmap = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                     &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT uw = 0, uh = 0;
        conv->GetSize(&uw, &uh);
        if (uw > 0 && uh > 0 && uw <= 8192 && uh <= 8192) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = static_cast<LONG>(uw);
            bi.bmiHeader.biHeight = -static_cast<LONG>(uh);
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bitmap && SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, uw * uh * 4, static_cast<BYTE*>(bits)))) {
                w = static_cast<int>(uw);
                h = static_cast<int>(uh);
            } else if (bitmap) {
                DeleteObject(bitmap);
                bitmap = nullptr;
            }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    factory->Release();
    return bitmap;
}

// The window's pixels (PrintWindow, so it works when covered) as a PNG through WIC.
void save_snapshot(HWND w, const fs::path& file) {
    RECT rc;
    GetWindowRect(w, &rc);
    const int width = rc.right - rc.left, height = rc.bottom - rc.top;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(w, mem, PW_RENDERFULLCONTENT);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    auto* px = static_cast<BYTE*>(bits);
    for (int i = 0; i < width * height; ++i) px[i * 4 + 3] = 255;
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) {
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&format);
        frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(width * 4),
                           static_cast<UINT>(width * height * 4), px);
        frame->Commit();
        encoder->Commit();
    }
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    DeleteObject(bmp);
}

const mods::InstalledMod* selected_mod() {
    if (g->selected.empty()) return nullptr;
    if (g->selected[0] == '!') {
        for (const mods::InstalledMod& m : g->model.installed)
            if (!m.usable && "!" + m.folder_name == g->selected) return &m;
        return nullptr;
    }
    return g->model.find(g->selected);
}

void layout();

void set_busy(bool busy) {
    g->busy = busy;
    if (g->progress) {
        ShowWindow(g->progress, busy ? SW_SHOW : SW_HIDE);
        SendMessageW(g->progress, PBM_SETMARQUEE, busy ? TRUE : FALSE, 30);
    }
    EnableWindow(g->filter, !busy);
    EnableWindow(g->profiles, !busy);
    for (HWND b : g->buttons) EnableWindow(b, !busy);
    EnableWindow(g->list, !busy);
    EnableWindow(g->master, !busy);
    if (g->list != nullptr) layout();  // the progress bar's room in the bottom bar
}

void show_details() {
    const mods::InstalledMod* m = selected_mod();
    if (g->selected != g->card_mod) {
        g->card_mod = g->selected;
        g->card_scroll = 0;
    }
    if (g->preview_bitmap) {
        DeleteObject(g->preview_bitmap);
        g->preview_bitmap = nullptr;
    }
    for (int i = 0; i < kMaxOptions; ++i) {
        ShowWindow(g->option_labels[i], SW_HIDE);
        ShowWindow(g->option_combos[i], SW_HIDE);
    }
    if (m == nullptr) {
        SetWindowTextW(g->details,
                       crlf(L"Select a mod to see its details.\n\nInstall mods by dropping archives (.zip, .7z, .rar) or "
                            L"folders onto this window, or with Install.\n\nMods higher in the list have priority: when "
                            L"two mods replace the same file, the higher one wins.")
                           .c_str());
        InvalidateRect(g->preview, nullptr, TRUE);
        return;
    }
    SetWindowTextW(g->details, crlf(wide(g->model.details(*m))).c_str());
    if (!m->manifest.preview.empty()) {
        const fs::path p = m->folder / mods::from_utf8(m->manifest.preview);
        g->preview_bitmap = load_image(p, g->preview_w, g->preview_h);
    }
    InvalidateRect(g->preview, nullptr, TRUE);
    const mods::StateEntry* e = g->model.entry(m->manifest.id);
    g->filling = true;
    for (std::size_t i = 0; i < m->manifest.options.size() && i < kMaxOptions; ++i) {
        const mods::ModOption& o = m->manifest.options[i];
        SetWindowTextW(g->option_labels[i], wide(o.name).c_str());
        SendMessageW(g->option_combos[i], CB_RESETCONTENT, 0, 0);
        int sel = 0;
        for (std::size_t c = 0; c < o.choices.size(); ++c) {
            SendMessageW(g->option_combos[i], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(o.choices[c].name).c_str()));
            const std::string current =
                e != nullptr && e->options.count(o.id) ? e->options.at(o.id) : o.default_choice;
            if (o.choices[c].id == current) sel = static_cast<int>(c);
        }
        SendMessageW(g->option_combos[i], CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
        ShowWindow(g->option_labels[i], SW_SHOW);
        ShowWindow(g->option_combos[i], SW_SHOW);
        EnableWindow(g->option_combos[i], m->usable);
    }
    g->filling = false;
    layout();  // the options' names and number decide their rows
}

// The summary in the bottom bar; the bar is laid out again when the text needs more or fewer lines.
void set_status(const std::wstring& text) {
    SetWindowTextW(g->status, text.c_str());
    if (g->status_w <= 0) return;
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(g->status, WM_GETFONT, 0, 0));
    if (std::max(ui::text_extent(font, L"").cy, ui::text_extent(font, text, g->status_w).cy) != g->status_h) layout();
}

std::string filter_text();
std::string lower_text(std::string s);
COLORREF status_color(const std::string& status);

void fill_list() {
    g->filling = true;
    SendMessageW(g->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g->list);
    g->row_keys.clear();
    g->row_status_color.clear();
    const std::string filter = filter_text();
    int row = 0, select_row = -1;
    const auto add = [&](const std::string& key, const mods::InstalledMod& m, bool checked) {
        const std::string status_text = g->model.status_text(m);
        if (!filter.empty()) {
            const std::string hay = lower_text(m.manifest.name + " " + m.folder_name + " " + m.manifest.id + " " +
                                               m.manifest.author + " " + g->model.content_text(m) + " " + status_text);
            if (hay.find(filter) == std::string::npos) return;
        }
        g->row_keys.push_back(key);
        g->row_status_color.push_back(status_color(status_text));
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        std::wstring name = wide(m.usable ? m.manifest.name : m.folder_name);
        item.pszText = name.data();
        ListView_InsertItem(g->list, &item);
        std::wstring version = wide(m.usable ? m.manifest.version : ""), author = wide(m.manifest.author),
                     content = wide(g->model.content_text(m)), status = wide(status_text);
        ListView_SetItemText(g->list, row, 1, version.data());
        ListView_SetItemText(g->list, row, 2, author.data());
        ListView_SetItemText(g->list, row, 3, content.data());
        ListView_SetItemText(g->list, row, 4, status.data());
        ListView_SetCheckState(g->list, row, checked);
        if (key == g->selected) select_row = row;
        ++row;
    };
    for (const mods::StateEntry& e : g->model.state.order)
        if (const mods::InstalledMod* m = g->model.find(e.id)) add(e.id, *m, e.enabled);
    for (const mods::InstalledMod& m : g->model.installed)
        if (!m.usable) add("!" + m.folder_name, m, false);
    if (select_row >= 0) {
        ListView_SetItemState(g->list, select_row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g->list, select_row, FALSE);
    }
    SendMessageW(g->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g->list, nullptr, TRUE);
    Button_SetCheck(g->master, g->model.state.mods_enabled ? BST_CHECKED : BST_UNCHECKED);
    std::string summary = g->model.summary();
    if (!filter.empty())
        summary = "Filter: " + std::to_string(g->row_keys.size()) + " of " + std::to_string(g->model.installed.size()) +
                  " mods shown | " + summary;
    set_status(wide(summary));
    g->filling = false;
    show_details();
}

std::string key_of_row(int row) {
    if (row < 0 || static_cast<std::size_t>(row) >= g->row_keys.size()) return {};
    return g->row_keys[static_cast<std::size_t>(row)];
}

std::string lower_text(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string filter_text() {
    if (g->filter == nullptr) return {};
    const int n = GetWindowTextLengthW(g->filter);
    std::wstring w(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(g->filter, w.data(), n + 1);
    w.resize(static_cast<std::size_t>(n));
    return lower_text(narrow(w));
}

// The status column's colour: green loads, orange loads with a warning, red cannot load, grey off.
// Lighter shades on the dark background, so they stay as readable as on the light one.
COLORREF status_color(const std::string& status) {
    const bool dark = ui::colors().dark;
    if (status.rfind("Loads", 0) == 0) {
        if (status.find("NTSC-U") != std::string::npos) return dark ? RGB(255, 176, 80) : RGB(184, 100, 0);
        return dark ? RGB(120, 214, 140) : RGB(30, 120, 50);
    }
    if (status.rfind("Not loaded", 0) == 0 || status.rfind("Invalid", 0) == 0) return dark ? RGB(255, 128, 128) : RGB(190, 30, 30);
    if (status.rfind("Off", 0) == 0 || status.rfind("On (all", 0) == 0) return ui::colors().muted;
    return CLR_DEFAULT;
}

void changed() {
    if (!g->model.save()) {
        // The choice is shown but not stored: say so instead of pretending (docs/mods/GUI_TEST.md, finding 1).
        const std::string why = !g->model.state_error.empty()
                                    ? "the mod list file is damaged (" + g->model.state_error + ")"
                                    : g->model.save_error;
        MessageBoxW(g->window,
                    wide("This change was NOT saved: " + why +
                         "\n\nThe game will not see it. Close the program that uses mods.json and try again.")
                        .c_str(),
                    window_title(), MB_OK | MB_ICONERROR);
    }
    g->model.replan();
    fill_list();
}

void refresh_all() {
    g->model.refresh();
    if (!g->model.state_error.empty()) {
        const std::wstring text = wide("The mod list file could not be read:\n\n" + g->model.state_error +
                                       "\n\nStart a new list? (Every mod will be switched off; the old file is "
                                       "replaced.)");
        if (MessageBoxW(g->window, text.c_str(), window_title(), MB_YESNO | MB_ICONWARNING) == IDYES)
            g->model.reset_state();
    }
    fill_list();
    if (!g->model.state_error.empty())  // declined: nothing can be saved until the file is fixed or replaced
        set_status(L"mods.json is damaged - read only. Fix or delete it, then press Refresh.");
}

// While mods.json cannot be read, edits are refused (they could not be saved). True when refused.
bool read_only_refused() {
    if (g->model.state_error.empty()) return false;
    MessageBoxW(g->window,
                wide("The mod list file is damaged, so changes cannot be saved:\n\n" + g->model.state_error +
                     "\n\nFix or delete mods.json, then press Refresh.")
                    .c_str(),
                window_title(), MB_OK | MB_ICONWARNING);
    fill_list();  // put the controls back as stored
    set_status(L"mods.json is damaged - read only. Fix or delete it, then press Refresh.");
    return true;
}

void start_install(std::vector<fs::path> sources, bool replace = false) {
    if (g->busy || sources.empty() || read_only_refused()) return;
    if (g->model.game.empty()) {
        MessageBoxW(g->window, wide("The game's files were not found (" + g->model.game_error +
                                    "), so downloads cannot be placed.\nPut ModManager.exe beside "
                                    "ShadowTheHedgehog.exe.")
                                   .c_str(),
                    window_title(), MB_OK | MB_ICONERROR);
        return;
    }
    set_busy(true);
    set_status(L"Installing " + sources.front().filename().wstring() +
               (sources.size() > 1 ? L" and " + std::to_wstring(sources.size() - 1) + L" more" : L"") + L"...");
    auto* job = new InstallJob{std::move(sources), {}, replace};
    const fs::path mods_dir = g->model.mods_dir;
    const mods::GameIndex* game = &g->model.game;
    HWND window = g->window;
    std::thread([job, mods_dir, game, window] {
        for (const fs::path& s : job->sources) {
            // An installed mod of the same id is replaced only when the user said so (finish_install asks).
            mods::InstallRequest req{s, mods_dir, game, job->replace, ""};
            mods::InstallResult r = mods::install(req);
            if (r.ok && job->replace)
                r.issues.insert(r.issues.begin(), {mods::Severity::Info, r.id, "updated the installed copy"});
            job->results.push_back(std::move(r));
        }
        PostMessageW(window, kMsgInstalled, 0, reinterpret_cast<LPARAM>(job));
    }).detach();
}

void finish_install(InstallJob* job) {
    std::unique_ptr<InstallJob> owned(job);
    set_busy(false);
    g->model.refresh();
    std::string report;
    bool any_ok = false, any_failed = false;
    std::vector<fs::path> updates;  // downloads of mods that are installed already
    std::vector<std::string> update_names;
    for (std::size_t i = 0; i < job->results.size(); ++i) {
        const mods::InstallResult& r = job->results[i];
        const std::string src = mods::utf8(job->sources[i].filename());
        const bool exists = !r.ok && std::any_of(r.issues.begin(), r.issues.end(), [](const mods::Issue& is) {
            return is.message.find("is installed") != std::string::npos;
        });
        if (exists) {
            updates.push_back(job->sources[i]);
            std::string id;  // the installed mod: named by the "is installed" issue
            for (const mods::Issue& is : r.issues)
                if (is.message.find("is installed") != std::string::npos) id = is.mod_id;
            const mods::InstalledMod* old = g->model.find(id);
            update_names.push_back(src + "  ->  " + (old ? old->manifest.name + " " + old->manifest.version : id));
            continue;
        }
        if (r.ok) {
            any_ok = true;
            g->selected = r.id;
            report += "Installed \"" + r.manifest.name + "\" from " + src + ": " + std::to_string(r.content.files) +
                      (r.content.files == 1 ? " file, " : " files, ") + std::to_string(r.content.afs_entries) +
                      (r.content.afs_entries == 1 ? " AFS entry, " : " AFS entries, ") +
                      std::to_string(r.content.textures) + (r.content.textures == 1 ? " texture" : " textures") +
                      (r.manifest.options.empty() ? "" : ", " + std::to_string(r.manifest.options.size()) + " options") +
                      ". It is off until you tick it.\n";
        } else {
            any_failed = true;
            report += "Could not install " + src + ".\n";
        }
        std::size_t notes = 0;
        for (const mods::Issue& is : r.issues)
            if (is.severity != mods::Severity::Info || !r.ok)
                if (++notes <= 8) report += std::string("  - ") + is.message + "\n";
        if (!r.left_out.empty())
            report += "  - " + std::to_string(r.left_out.size()) + " file(s) of the download were not used (" +
                      r.left_out.front() + (r.left_out.size() > 1 ? ", ..." : "") + ")\n";
        report += "\n";
    }
    fill_list();
    if (!report.empty())
        MessageBoxW(g->window, crlf(wide(report)).c_str(), window_title(),
                    MB_OK | (any_failed ? (any_ok ? MB_ICONWARNING : MB_ICONERROR) : MB_ICONINFORMATION));
    if (!updates.empty()) {
        std::string list;
        for (const std::string& n : update_names) list += "  " + n + "\n";
        const std::string text = std::string(updates.size() == 1 ? "This mod is" : "These mods are") +
                                 " installed already:\n\n" + list +
                                 "\nReplace the installed copy with the download? Its place in the list, its on/off "
                                 "state and its options are kept.";
        if (MessageBoxW(g->window, crlf(wide(text)).c_str(), L"Update mods", MB_YESNO | MB_ICONQUESTION) == IDYES)
            start_install(std::move(updates), true);
    }
}

void on_check_changed(int row, bool checked) {
    if (read_only_refused()) return;
    const std::string key = key_of_row(row);
    if (key.empty() || key[0] == '!') {
        if (checked && !key.empty()) {
            g->filling = true;
            ListView_SetCheckState(g->list, row, FALSE);
            g->filling = false;
            MessageBoxW(g->window, L"This folder is not a usable mod; see its notes.", window_title(),
                        MB_OK | MB_ICONINFORMATION);
        }
        return;
    }
    mods::StateEntry* e = g->model.entry(key);
    if (e == nullptr || e->enabled == checked) return;
    e->enabled = checked;
    if (!checked) {
        const std::vector<std::string> dependents = g->model.enabled_dependents(key);
        if (!dependents.empty()) {
            std::string list;
            for (const std::string& d : dependents) list += "  " + g->model.find(d)->manifest.name + " (" + d + ")\n";
            if (MessageBoxW(g->window, crlf(wide("These enabled mods need it and will not load without it:\n" + list +
                                                 "\nSwitch them off too?"))
                                           .c_str(),
                            window_title(), MB_YESNO | MB_ICONQUESTION) == IDYES)
                for (const std::string& d : dependents) g->model.entry(d)->enabled = false;
        }
    }
    if (checked) {
        const std::vector<std::string> missing = g->model.missing_dependencies(key);
        if (!missing.empty()) {
            std::string list;
            for (const std::string& d : missing) list += "  " + d + "\n";
            MessageBoxW(g->window, crlf(wide("This mod needs mods that are not installed:\n" + list +
                                             "\nIt stays enabled but will not load until they are installed."))
                                       .c_str(),
                        window_title(), MB_OK | MB_ICONWARNING);
        }
        const std::vector<std::string> off = g->model.disabled_dependencies(key);
        if (!off.empty()) {
            std::string list;
            for (const std::string& d : off) list += "  " + g->model.find(d)->manifest.name + " (" + d + ")\n";
            if (MessageBoxW(g->window, crlf(wide("This mod needs:\n" + list + "\nEnable them too?")).c_str(),
                            window_title(), MB_YESNO | MB_ICONQUESTION) == IDYES)
                for (const std::string& d : off) g->model.entry(d)->enabled = true;
        }
    }
    g->selected = key;
    changed();
}

void remove_selected() {
    const mods::InstalledMod* m = selected_mod();
    if (m == nullptr || read_only_refused()) return;
    const std::wstring text = wide("Remove \"" + (m->usable ? m->manifest.name : m->folder_name) +
                                   "\"?\n\nIts folder is moved to the Recycle Bin:\n" + mods::utf8(m->folder));
    if (MessageBoxW(g->window, text.c_str(), L"Remove mod", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    // The Recycle Bin, so a removal can be undone (SHFileOperation needs a double-NUL-terminated list).
    std::wstring from = m->folder.wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd = g->window;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    const int rc = SHFileOperationW(&op);
    if (rc != 0 || op.fAnyOperationsAborted) {
        MessageBoxW(g->window, L"The folder could not be removed (is a file of it open?).", L"Remove mod",
                    MB_OK | MB_ICONERROR);
    }
    g->selected.clear();
    refresh_all();
}

// ---- code mods (docs/mods/codemods/README.md) ------------------------------------------------------------------------

// The toolchain that applies code: the source tree (tools/mods/codemods, the recompiler), the NTSC-U main.dol the
// code was written against, Python. Kept in mods/.manager.json; asked for once.
struct Toolchain {
    fs::path repo, src_dol;
    std::wstring python = L"python";
};

Toolchain load_toolchain() {
    Toolchain t;
    std::ifstream in(mods::long_path(g->model.mods_dir / ".manager.json"), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    mods::json::Value v;
    std::string e;
    if (!text.empty() && mods::json::parse(text, v, e))
        if (const mods::json::Value* tc = v.find("toolchain")) {
            if (const auto* r = tc->find("repo")) t.repo = mods::from_utf8(r->as_string());
            if (const auto* d = tc->find("src_dol")) t.src_dol = mods::from_utf8(d->as_string());
            if (const auto* py = tc->find("python"); py && !py->as_string().empty()) t.python = wide(py->as_string());
        }
    return t;
}

void save_toolchain(const Toolchain& t) {
    const fs::path file = g->model.mods_dir / ".manager.json";
    std::ifstream in(mods::long_path(file), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    mods::json::Value v;
    std::string e;
    if (text.empty() || !mods::json::parse(text, v, e)) v = mods::json::Value::object();
    mods::json::Value tc = mods::json::Value::object();
    tc.set("repo", mods::json::Value::string(mods::utf8(t.repo)));
    tc.set("src_dol", mods::json::Value::string(mods::utf8(t.src_dol)));
    tc.set("python", mods::json::Value::string(narrow(t.python)));
    v.set("toolchain", std::move(tc));
    std::ofstream out(mods::long_path(file), std::ios::binary | std::ios::trunc);
    out << mods::json::write(v);
}

// What was built for the current choice of code: <mods>/.variants/<signature>.json, or a null value.
mods::json::Value variant_report(const std::string& signature) {
    mods::json::Value v;
    if (signature.empty()) return v;
    std::ifstream in(mods::long_path(g->model.mods_dir / ".variants" / (signature + ".json")), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string e;
    if (text.empty() || !mods::json::parse(text, v, e)) return mods::json::Value{};
    return v;
}

std::string variant_summary(const mods::json::Value& r) {
    if (!r.is_object()) return "not built for this choice";
    const mods::json::Value* c = r.find("counts");
    const auto n = [&](const char* k) { return c && c->find(k) ? static_cast<long long>(c->find(k)->as_number()) : 0LL; };
    const mods::json::Value* b = r.find("build");
    const bool ok = b && b->find("ok") && b->find("ok")->as_bool();
    std::string s = std::to_string(n("applied")) + " patches built in, " + std::to_string(n("runtime")) +
                    " for the code engine, " + std::to_string(n("skipped")) + " skipped (no effect here, refused or "
                                                                                  "uncertain; see the report)";
    if (!ok) return "the last build FAILED (" + s + ")";
    const mods::json::Value* exe = r.find("exe");
    return (exe && exe->is_string() ? "built: " : "prepared (no variant needed): ") + s;
}

// Runs build_variant.py for the current choice in the background; the window is busy until it ends.
constexpr UINT kMsgCodeBuilt = WM_APP + 2;
void start_code_build(HWND owner) {
    Toolchain t = load_toolchain();
    std::error_code ec;
    if (t.repo.empty() || !fs::is_regular_file(t.repo / "tools" / "mods" / "codemods" / "build_variant.py", ec)) {
        // A folder picker for the source tree.
        BROWSEINFOW bi{};
        bi.hwndOwner = owner;
        bi.lpszTitle = L"Choose the game's source tree (it holds tools\\mods\\codemods\\build_variant.py)";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl == nullptr) return;
        wchar_t path[MAX_PATH];
        SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
        t.repo = path;
        if (!fs::is_regular_file(t.repo / "tools" / "mods" / "codemods" / "build_variant.py", ec)) {
            MessageBoxW(owner, L"That folder has no tools\\mods\\codemods\\build_variant.py.", L"Code mods", MB_OK | MB_ICONERROR);
            return;
        }
    }
    if (t.src_dol.empty() || !fs::is_regular_file(t.src_dol, ec)) {
        const fs::path guess = t.repo.parent_path() / "EXTRACTED_DISKS" / "USA" / "sys" / "main.dol";
        if (fs::is_regular_file(guess, ec)) {
            t.src_dol = guess;
        } else {
            std::wstring buf(4096, L'\0');
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = owner;
            ofn.lpstrFilter = L"main.dol\0main.dol\0";
            ofn.lpstrFile = buf.data();
            ofn.nMaxFile = static_cast<DWORD>(buf.size());
            ofn.lpstrTitle = L"Choose the NTSC-U disc's sys\\main.dol (the code mods were written against it)";
            ofn.Flags = OFN_FILEMUSTEXIST;
            if (!GetOpenFileNameW(&ofn)) return;
            t.src_dol = buf.c_str();
        }
    }
    save_toolchain(t);
    const std::wstring script = (t.repo / "tools" / "mods" / "codemods" / "build_variant.py").wstring();
    std::wstring cmd = L"\"" + t.python + L"\" -I \"" + script + L"\" --repo \"" + t.repo.wstring() + L"\" --manager \"" +
                       executable_path().wstring() + L"\" --mods-dir \"" +
                       g->model.mods_dir.wstring() + L"\" --game \"" + g->model.game_root.wstring() +
                       L"\" --src-dol \"" + t.src_dol.wstring() + L"\"";
    const fs::path log = g->model.mods_dir / ".variants" / "build-output.txt";
    fs::create_directories(log.parent_path(), ec);
    set_busy(true);
    set_status(L"Building the code mods (the first build takes about 12 minutes)...");
    HWND window = g->window;
    std::thread([cmd, log, window] {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = out;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> line(cmd.begin(), cmd.end());
        line.push_back(L'\0');
        DWORD code = 99;
        if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        PostMessageW(window, kMsgCodeBuilt, code, 0);
    }).detach();
}

void finish_code_build(DWORD code) {
    set_busy(false);
    fill_list();
    const std::string sig = mods::code_selection_signature(g->model.installed, g->model.state);
    const mods::json::Value r = variant_report(sig);
    const std::string text = (code == 0 ? std::string("Code mods built.\n\n") : std::string("The code-mod build failed.\n\n")) +
                             variant_summary(r) + "\n\nOutput: " +
                             mods::utf8(g->model.mods_dir / ".variants" / "build-output.txt");
    MessageBoxW(g->window, crlf(wide(text)).c_str(), L"Code mods", MB_OK | (code == 0 ? MB_ICONINFORMATION : MB_ICONERROR));
}

// The code window: every code item of the enabled mods, a checkbox each; the state of the variant for the choice.
struct CodeRow {
    std::string mod, item;
};
struct CodeWindow {
    HWND window = nullptr, list = nullptr, info = nullptr;
    std::vector<CodeRow> rows;
    bool done = false, build = false;
};
CodeWindow* g_code = nullptr;

void code_window_refresh() {
    const std::string sig = mods::code_selection_signature(g->model.installed, g->model.state);
    std::string info = sig.empty() ? std::string("No code chosen: the stock executable runs.")
                                   : "Chosen code " + sig + ": " + variant_summary(variant_report(sig));
    info += "\n\nCode patches need a rebuilt variant of the game executable (the source tree and Visual Studio on this "
            "machine); data codes run in the game's code engine. Patches that land in code this build replaced with "
            "native C++ have no effect and are reported, not applied. A built variant contains the game's code: keep it "
            "on this machine.";
    SetWindowTextW(g_code->info, crlf(wide(info)).c_str());
}

LRESULT CALLBACK code_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    bool handled = false;
    const LRESULT colored = popup_colors(msg, wp, lp, w, &handled);
    if (handled) return colored;
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
                g_code->done = true;
                return 0;
            }
            if (LOWORD(wp) == 10) {  // Build
                g_code->build = true;
                g_code->done = true;
                return 0;
            }
            if (LOWORD(wp) == 11) {  // Report
                const std::string sig = mods::code_selection_signature(g->model.installed, g->model.state);
                const fs::path f = g->model.mods_dir / ".variants" / (sig + ".json");
                std::error_code ec;
                if (!sig.empty() && fs::exists(f, ec)) ShellExecuteW(w, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (g_code && hdr->hwndFrom == g_code->list && hdr->code == LVN_ITEMCHANGED) {
                auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
                const UINT oldc = (nm->uOldState & LVIS_STATEIMAGEMASK) >> 12, newc = (nm->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                if ((nm->uChanged & LVIF_STATE) && oldc && newc && oldc != newc &&
                    static_cast<std::size_t>(nm->iItem) < g_code->rows.size()) {
                    const CodeRow& row = g_code->rows[static_cast<std::size_t>(nm->iItem)];
                    if (mods::StateEntry* e = g->model.entry(row.mod)) {
                        e->codes[row.item] = newc == 2;
                        if (!g->model.save()) MessageBoxW(w, L"The choice was not saved (mods.json).", L"Code mods", MB_OK | MB_ICONERROR);
                        code_window_refresh();
                    }
                }
            }
            break;
        }
        case WM_CLOSE:
            g_code->done = true;
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void code_window() {
    if (read_only_refused()) return;
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = code_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"ShadowModManagerCode";
        RegisterClassExW(&wc);
        registered = true;
    }
    CodeWindow cw;
    g_code = &cw;
    // At a large zoom, no larger than the screen: the list and the text inside it scroll.
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(g->window, MONITOR_DEFAULTTONEAREST), &monitor);
    const int W = std::min(g->px(760), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) * 9 / 10);
    const int H = std::min(g->px(520), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) * 9 / 10);
    const int pad = g->px(10);
    RECT owner;
    GetWindowRect(g->window, &owner);
    cw.window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ShadowModManagerCode", L"Code mods", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                owner.left + (owner.right - owner.left - W) / 2, owner.top + (owner.bottom - owner.top - H) / 2,
                                W, H, g->window, nullptr, inst, nullptr);
    ui::apply_to_window(cw.window);  // the title bar; the controls are coloured as they are made
    RECT cr;
    GetClientRect(cw.window, &cr);
    const int cw_ = cr.right, ch = cr.bottom;
    HWND label = CreateWindowExW(0, WC_STATICW, L"Code the enabled mods offer (tick to apply)", WS_CHILD | WS_VISIBLE, pad, pad,
                                 cw_ - pad * 2, g->px(18), cw.window, nullptr, inst, nullptr);
    cw.list = CreateWindowExW(0, WC_LISTVIEWW, L"Code items", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT |
                                                                  LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                              pad, pad + g->px(22), cw_ - pad * 2, ch - g->px(22) - pad * 3 - g->px(110) - g->px(30),
                              cw.window, nullptr, inst, nullptr);
    ListView_SetExtendedListViewStyle(cw.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ui::apply_to_control(cw.list);  // before the rows: it remakes the check boxes
    const wchar_t* cols[] = {L"Code", L"Mod"};
    const int widths[] = {cw_ - pad * 2 - g->px(200), g->px(180)};
    for (int i = 0; i < 2; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = widths[i];
        ListView_InsertColumn(cw.list, i, &c);
    }
    int row = 0;
    for (auto it = g->model.state.order.begin(); it != g->model.state.order.end(); ++it) {
        if (!it->enabled) continue;
        const mods::InstalledMod* mod = g->model.find(it->id);
        if (mod == nullptr) continue;
        for (const mods::CodeItem& c : mods::list_code_items(*mod)) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            std::wstring label_text = wide(c.label), mod_name = wide(mod->manifest.name);
            item.pszText = label_text.data();
            ListView_InsertItem(cw.list, &item);
            ListView_SetItemText(cw.list, row, 1, mod_name.data());
            const bool on = it->codes.count(c.id) && it->codes.at(c.id);
            ListView_SetCheckState(cw.list, row, on);
            cw.rows.push_back({it->id, c.id});
            ++row;
        }
    }
    cw.info = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                              pad, ch - pad * 2 - g->px(30) - g->px(110), cw_ - pad * 2, g->px(110), cw.window, nullptr, inst, nullptr);
    const int bw = g->px(110), by = ch - pad - g->px(30);
    HWND build = CreateWindowExW(0, WC_BUTTONW, L"Build...", WS_CHILD | WS_VISIBLE | WS_TABSTOP, pad, by, bw, g->px(30), cw.window,
                                 reinterpret_cast<HMENU>(10), inst, nullptr);
    HWND rep = CreateWindowExW(0, WC_BUTTONW, L"Open report", WS_CHILD | WS_VISIBLE | WS_TABSTOP, pad * 2 + bw, by, bw, g->px(30),
                               cw.window, reinterpret_cast<HMENU>(11), inst, nullptr);
    HWND close = CreateWindowExW(0, WC_BUTTONW, L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, cw_ - pad - bw, by,
                                 bw, g->px(30), cw.window, reinterpret_cast<HMENU>(IDOK), inst, nullptr);
    for (HWND h : {label, cw.list, cw.info, build, rep, close}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g->font), TRUE);
    for (HWND h : {label, cw.info, build, rep, close}) ui::apply_to_control(h);
    if (row == 0) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        std::wstring none = L"(no enabled mod offers code)";
        item.pszText = none.data();
        ListView_InsertItem(cw.list, &item);
        EnableWindow(build, FALSE);
    }
    code_window_refresh();
    EnableWindow(g->window, FALSE);
    ShowWindow(cw.window, SW_SHOW);
    MSG m;
    while (!cw.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) break;
        if (!IsDialogMessageW(cw.window, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(g->window, TRUE);
    apply_pending_look();
    DestroyWindow(cw.window);
    g_code = nullptr;
    SetForegroundWindow(g->window);
    fill_list();
    if (cw.build) start_code_build(g->window);
}

void play() {
    std::error_code ec;
    if (!fs::is_regular_file(g->game_exe, ec)) {
        MessageBoxW(g->window, (L"ShadowTheHedgehog.exe was not found beside the manager:\n" + g->game_exe.wstring()).c_str(),
                    window_title(), MB_OK | MB_ICONERROR);
        return;
    }
    if (!g->model.resolution.skipped.empty()) {
        std::string list;
        for (const std::string& id : g->model.resolution.skipped) list += "  " + id + "\n";
        if (MessageBoxW(g->window,
                        crlf(wide("These enabled mods cannot load and will be left out:\n" + list + "\nPlay anyway?"))
                            .c_str(),
                        window_title(), MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
    }
    std::wstring args;
    if (g->model.mods_dir != g->game_exe.parent_path() / "mods")
        args = L"--mods-dir \"" + g->model.mods_dir.wstring() + L"\"";
    fs::path exe = g->game_exe;
    // Chosen code: the variant built for exactly this choice, or the stock executable when the choice needs none.
    const std::string sig = mods::code_selection_signature(g->model.installed, g->model.state);
    if (!sig.empty()) {
        const mods::json::Value r = variant_report(sig);
        const mods::json::Value* b = r.is_object() ? r.find("build") : nullptr;
        const bool ok = b && b->find("ok") && b->find("ok")->as_bool();
        const mods::json::Value* name = r.is_object() ? r.find("exe") : nullptr;
        const fs::path variant = name && name->is_string() ? g->model.mods_dir / ".variants" / mods::from_utf8(name->as_string()) : fs::path();
        if (ok && !variant.empty() && fs::is_regular_file(variant, ec)) {
            exe = variant;
            args += (args.empty() ? L"" : L" ") + (L"--game-data \"" + g->model.game_root.wstring() + L"\"");
            if (args.find(L"--mods-dir") == std::wstring::npos)
                args += L" --mods-dir \"" + g->model.mods_dir.wstring() + L"\"";
        } else if (!(ok && variant.empty())) {
            const int answer = MessageBoxW(g->window,
                L"The code you chose is not built yet (or its build failed).\n\nYes: build it now (Code mods), then press Play again.\n"
                L"No: play now without the code.",
                L"Code mods", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDYES) {
                start_code_build(g->window);
                return;
            }
            if (answer != IDNO) return;
        }
    }
    ShellExecuteW(g->window, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(),
                  g->game_exe.parent_path().c_str(), SW_SHOWNORMAL);
}

// ---- a small modal form (edit info, profile name) -------------------------------------------------------------------

struct FormField {
    std::wstring label;
    std::wstring value;
    bool multiline = false;
};

struct FormState {
    std::vector<FormField>* fields = nullptr;
    std::vector<HWND> edits;
    bool done = false, ok = false;
};

LRESULT CALLBACK form_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<FormState*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    bool handled = false;
    const LRESULT colored = popup_colors(msg, wp, lp, w, &handled);
    if (handled) return colored;
    switch (msg) {
        case WM_COMMAND:
            if (st && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
                st->ok = LOWORD(wp) == IDOK;
                if (st->ok)
                    for (std::size_t i = 0; i < st->edits.size(); ++i) {
                        const int n = GetWindowTextLengthW(st->edits[i]);
                        std::wstring t(static_cast<std::size_t>(n) + 1, L'\0');
                        GetWindowTextW(st->edits[i], t.data(), n + 1);
                        t.resize(static_cast<std::size_t>(n));
                        (*st->fields)[i].value = t;
                    }
                st->done = true;
                return 0;
            }
            break;
        case WM_CLOSE:
            if (st) st->done = true;
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

// A form with one edit per field, OK and Cancel; true with the new values on OK. Modal to the main window.
bool edit_form(const wchar_t* title, std::vector<FormField>& fields) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = form_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"ShadowModManagerForm";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int pad = g->px(10), label_w = g->px(110), edit_w = g->px(380), line = g->px(26);
    int height = pad;
    for (const FormField& f : fields) height += (f.multiline ? g->px(110) : line) + g->px(8);
    height += line + pad * 2;
    RECT owner;
    GetWindowRect(g->window, &owner);
    RECT want{0, 0, pad * 3 + label_w + edit_w, height};
    AdjustWindowRectExForDpi(&want, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, g->dpi);
    const int ww = want.right - want.left, wh = want.bottom - want.top;
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ShadowModManagerForm", title, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             owner.left + (owner.right - owner.left - ww) / 2, owner.top + (owner.bottom - owner.top - wh) / 2,
                             ww, wh, g->window, nullptr, inst, nullptr);
    FormState st;
    st.fields = &fields;
    SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    int y = pad;
    for (const FormField& f : fields) {
        const int h = f.multiline ? g->px(110) : line;
        HWND label = CreateWindowExW(0, WC_STATICW, f.label.c_str(), WS_CHILD | WS_VISIBLE, pad, y + g->px(4), label_w,
                                     line, w, nullptr, inst, nullptr);
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, f.value.c_str(),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                        (f.multiline ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL) : 0),
                                    pad * 2 + label_w, y, edit_w, h, w, nullptr, inst, nullptr);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(g->font), TRUE);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g->font), TRUE);
        st.edits.push_back(edit);
        y += h + g->px(8);
    }
    const int bw = g->px(90);
    HWND ok = CreateWindowExW(0, WC_BUTTONW, L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              pad * 3 + label_w + edit_w - bw * 2 - g->px(6), y + pad / 2, bw, line, w,
                              reinterpret_cast<HMENU>(IDOK), inst, nullptr);
    HWND cancel = CreateWindowExW(0, WC_BUTTONW, L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  pad * 3 + label_w + edit_w - bw, y + pad / 2, bw, line, w,
                                  reinterpret_cast<HMENU>(IDCANCEL), inst, nullptr);
    SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(g->font), TRUE);
    SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(g->font), TRUE);
    ui::apply_to_window(w);
    EnableWindow(g->window, FALSE);
    ShowWindow(w, SW_SHOW);
    if (!st.edits.empty()) {
        SetFocus(st.edits.front());
        SendMessageW(st.edits.front(), EM_SETSEL, 0, -1);
    }
    MSG m;
    while (!st.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            st.done = true;
            break;
        }
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(g->window, TRUE);
    apply_pending_look();
    DestroyWindow(w);
    SetForegroundWindow(g->window);
    return st.ok;
}

// ---- actions --------------------------------------------------------------------------------------------------------

void edit_info() {
    const mods::InstalledMod* m = selected_mod();
    if (m == nullptr || !m->usable) return;
    std::vector<FormField> f = {{L"Name", wide(m->manifest.name)},
                                {L"Version", wide(m->manifest.version)},
                                {L"Author", wide(m->manifest.author)},
                                {L"Website", wide(m->manifest.website)},
                                {L"Description", crlf(wide(m->manifest.description)), true}};
    const std::wstring title = L"Edit " + wide(m->manifest.name);
    while (edit_form(title.c_str(), f)) {
        mods::Manifest mf = m->manifest;
        mf.name = narrow(f[0].value);
        mf.version = narrow(f[1].value);
        mf.author = narrow(f[2].value);
        mf.website = narrow(f[3].value);
        std::string d = narrow(f[4].value);
        d.erase(std::remove(d.begin(), d.end(), '\r'), d.end());
        mf.description = d;
        mf.extra.set("user_edited", mods::json::Value::boolean(true));  // an update keeps these edits
        if (mf.name.empty() || !mods::Version::parse(mf.version)) {
            MessageBoxW(g->window, L"The name must not be empty and the version must look like 1.0 or 2.3.1.",
                        L"Edit mod", MB_OK | MB_ICONWARNING);
            continue;
        }
        std::string error;
        if (!mods::write_manifest(m->folder / std::string(mods::kManifestName), mf, error))
            MessageBoxW(g->window, wide("Could not save mod.json: " + error).c_str(), L"Edit mod", MB_OK | MB_ICONERROR);
        refresh_all();
        return;
    }
}

void set_all(bool on) {
    if (read_only_refused()) return;
    for (mods::StateEntry& e : g->model.state.order) e.enabled = on;
    changed();
}

void move_selected_to(std::size_t position) {
    if (g->selected.empty() || g->selected[0] == '!' || read_only_refused()) return;
    g->model.move_to(g->selected, position);
    changed();
}

void profiles_menu() {
    HMENU menu = CreatePopupMenu();
    HMENU del = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdProfileSave, L"Save current set as...");
    g->profile_names = mods::list_profiles(g->model.mods_dir);
    if (!g->profile_names.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (std::size_t i = 0; i < g->profile_names.size() && i < 150; ++i) {
        AppendMenuW(menu, MF_STRING, kCmdProfileLoad0 + static_cast<UINT>(i), (L"Load \"" + wide(g->profile_names[i]) + L"\"").c_str());
        AppendMenuW(del, MF_STRING, kCmdProfileDelete0 + static_cast<UINT>(i), wide(g->profile_names[i]).c_str());
    }
    if (!g->profile_names.empty()) AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(del), L"Delete");
    RECT r;
    GetWindowRect(g->profiles, &r);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, r.left, r.bottom, 0, g->window, nullptr));
    DestroyMenu(menu);  // destroys the submenu too
    if (cmd == 0) return;
    std::string error;
    if (cmd == kCmdProfileSave) {
        std::vector<FormField> f = {{L"Profile name", L""}};
        while (edit_form(L"Save profile", f)) {
            const std::string name = narrow(f[0].value);
            if (!mods::valid_profile_name(name)) {
                MessageBoxW(g->window, L"A profile name is 1-40 letters, digits, spaces, '-', '_', '(', ')' or '.'.",
                            L"Save profile", MB_OK | MB_ICONWARNING);
                continue;
            }
            const auto names = mods::list_profiles(g->model.mods_dir);
            if (std::find(names.begin(), names.end(), name) != names.end() &&
                MessageBoxW(g->window, wide("Replace the profile \"" + name + "\"?").c_str(), L"Save profile",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                continue;
            if (!mods::save_profile(g->model.mods_dir, name, g->model.state, error))
                MessageBoxW(g->window, wide(error).c_str(), L"Save profile", MB_OK | MB_ICONERROR);
            else
                set_status(wide("Saved profile \"" + name + "\""));
            return;
        }
        return;
    }
    if (cmd >= kCmdProfileLoad0 && cmd < kCmdProfileLoad0 + g->profile_names.size()) {
        if (read_only_refused()) return;
        const std::string& name = g->profile_names[cmd - kCmdProfileLoad0];
        mods::State loaded;
        if (!mods::load_profile(g->model.mods_dir, name, loaded, error)) {
            MessageBoxW(g->window, wide(error).c_str(), L"Load profile", MB_OK | MB_ICONERROR);
            return;
        }
        if (MessageBoxW(g->window,
                        wide("Load \"" + name + "\"? The current choices (on/off, order, options) are replaced; save "
                             "them as a profile first to keep them.")
                            .c_str(),
                        L"Load profile", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        mods::reconcile(loaded, g->model.installed);
        g->model.state = loaded;
        changed();
        return;
    }
    if (cmd >= kCmdProfileDelete0 && cmd < kCmdProfileDelete0 + g->profile_names.size()) {
        const std::string& name = g->profile_names[cmd - kCmdProfileDelete0];
        if (MessageBoxW(g->window, wide("Delete the profile \"" + name + "\"? Installed mods are not touched.").c_str(),
                        L"Delete profile", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES &&
            !mods::delete_profile(g->model.mods_dir, name, error))
            MessageBoxW(g->window, wide(error).c_str(), L"Delete profile", MB_OK | MB_ICONERROR);
    }
}

void toggle_selected() {
    for (std::size_t row = 0; row < g->row_keys.size(); ++row)
        if (g->row_keys[row] == g->selected) {
            const bool on = ListView_GetCheckState(g->list, static_cast<int>(row)) != 0;
            ListView_SetCheckState(g->list, static_cast<int>(row), !on);  // LVN_ITEMCHANGED does the rest
            return;
        }
}

void context_menu(int x, int y) {
    const mods::InstalledMod* m = selected_mod();
    HMENU menu = CreatePopupMenu();
    const bool usable = m != nullptr && m->usable;
    const mods::StateEntry* e = usable ? g->model.entry(m->manifest.id) : nullptr;
    const UINT on = usable ? MF_STRING : MF_STRING | MF_GRAYED;
    if (m != nullptr) {
        AppendMenuW(menu, on, kCmdToggle, e && e->enabled ? L"Switch off\tSpace" : L"Switch on\tSpace");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, on, kCmdTop, L"Move to top (highest priority)\tAlt+Home");
        AppendMenuW(menu, on, kIdUp, L"Move up\tAlt+Up");
        AppendMenuW(menu, on, kIdDown, L"Move down\tAlt+Down");
        AppendMenuW(menu, on, kCmdBottom, L"Move to bottom\tAlt+End");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, on, kCmdEditInfo, L"Edit name, version, author...\tCtrl+E");
        AppendMenuW(menu, MF_STRING, kIdFolder, L"Open folder");
        AppendMenuW(menu, MF_STRING, kIdRemove, L"Remove...\tDel");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, kCmdEnableAll, L"Switch all on");
    AppendMenuW(menu, MF_STRING, kCmdDisableAll, L"Switch all off");
    AppendMenuW(menu, MF_STRING, kIdInstall, L"Install...\tCtrl+O");
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, g->window, nullptr));
    DestroyMenu(menu);
    if (cmd != 0) PostMessageW(g->window, WM_COMMAND, cmd, 0);
}

// ---- drag to reorder --------------------------------------------------------------------------------------------------

void drag_move(bool drop) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g->list, &pt);
    LVINSERTMARK mark{sizeof(mark)};
    ListView_InsertMarkHitTest(g->list, &pt, &mark);
    if (!drop) {
        ListView_SetInsertMark(g->list, &mark);
        return;
    }
    LVINSERTMARK none{sizeof(none)};
    none.iItem = -1;
    ListView_SetInsertMark(g->list, &none);
    const std::string dragged = g->drag_key;
    g->drag_key.clear();
    const std::string target = key_of_row(mark.iItem);
    if (target.empty() || target == dragged || target[0] == '!' || read_only_refused()) return;
    std::size_t to = g->model.position_of(target) + ((mark.dwFlags & LVIM_AFTER) ? 1 : 0);
    if (g->model.position_of(dragged) < to) --to;  // the dragged entry leaves its old place first
    g->selected = dragged;
    g->model.move_to(dragged, to);
    changed();
}

// ---- window placement -------------------------------------------------------------------------------------------------

fs::path settings_file() { return g->model.mods_dir / ".manager.json"; }

void save_placement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(g->window, &wp)) return;
    mods::json::Value v = mods::json::Value::object();
    const RECT& r = wp.rcNormalPosition;
    mods::json::Value win = mods::json::Value::object();
    win.set("left", mods::json::Value::number(r.left));
    win.set("top", mods::json::Value::number(r.top));
    win.set("right", mods::json::Value::number(r.right));
    win.set("bottom", mods::json::Value::number(r.bottom));
    win.set("maximized", mods::json::Value::boolean(wp.showCmd == SW_SHOWMAXIMIZED));
    v.set("window", std::move(win));
    std::error_code ec;
    fs::create_directories(g->model.mods_dir, ec);
    std::ofstream out(mods::long_path(settings_file()), std::ios::binary | std::ios::trunc);
    out << mods::json::write(v);
}

// The last size and place, when it is still on a screen. False when there is none (first start).
bool restore_placement(int& show) {
    std::ifstream in(mods::long_path(settings_file()), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    mods::json::Value v;
    std::string e;
    if (text.empty() || !mods::json::parse(text, v, e)) return false;
    const mods::json::Value* win = v.find("window");
    if (win == nullptr) return false;
    const auto num = [&](const char* k) { return win->find(k) ? static_cast<LONG>(win->find(k)->as_number()) : 0; };
    RECT r{num("left"), num("top"), num("right"), num("bottom")};
    if (r.right - r.left < 200 || r.bottom - r.top < 150) return false;
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) == nullptr) return false;  // that screen is gone
    WINDOWPLACEMENT wp{sizeof(wp)};
    wp.rcNormalPosition = r;
    wp.showCmd = SW_HIDE;
    SetWindowPlacement(g->window, &wp);
    show = win->find("maximized") && win->find("maximized")->as_bool() ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    return true;
}

std::wstring text_in(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring t(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(h, t.data(), n + 1);
    t.resize(static_cast<std::size_t>(n));
    return t;
}

// The window's smallest size: the same at every zoom (the layout fits the room it has).
SIZE smallest_window(UINT dpi) { return SIZE{MulDiv(760, static_cast<int>(dpi), 96), MulDiv(520, static_cast<int>(dpi), 96)}; }

// The height of a drop-down list's box (its closed part), in the list's font.
int combo_height(HWND combo) {
    return static_cast<int>(SendMessageW(combo, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0)) + 2 * GetSystemMetricsForDpi(SM_CYEDGE, g->dpi) + 2;
}

// A drop-down list's width for its longest choice.
int combo_width(HWND combo) {
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0));
    int widest = 0;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        const int n = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0));
        if (n <= 0) continue;
        std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
        SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<std::size_t>(n));
        widest = std::max(widest, static_cast<int>(ui::text_extent(font, text).cx));
    }
    return widest + ui::dropdown_chrome(font);
}

// Zoom never changes the window's size: everything is placed for the room the window has. The buttons and the bars
// wrap into more rows, the buttons show only their icons when their words would take more than two rows, the selected
// mod's card goes under the list when the window is narrow, and long names wrap.
void layout() {
    if (g->list == nullptr || g->card == nullptr) return;
    RECT rc;
    GetClientRect(g->window, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    const int pad = g->px(12), gap = g->px(6), bh = g->px(32);
    const int left = pad, right = w - pad, inner = std::max(1, right - left);
    const HFONT font = g->font;
    const int line = ui::text_extent(font, L"").cy;

    // ---- the top bar: six buttons, the filter, then Code, Profiles, Load mods and Play on the right
    const int filter_min = g->px(160);
    HWND right_group[] = {g->code, g->profiles, g->master, g->buttons[6]};
    bool icons_only = false;  // measured both ways below; the buttons' look is changed only once it is chosen
    const auto width_of = [&](HWND c) {
        if (c == g->master) return ui::check_box_width(c) + g->px(8);
        const bool six = std::find(g->buttons, g->buttons + 6, c) != g->buttons + 6;
        return std::max(g->px(40), six ? ui::button_width(c, icons_only) : ui::button_width(c));
    };
    // Lays the bar out (or only measures it, place = false); returns its bottom.
    const auto top_bar = [&](bool place) {
        int x = left, y = pad;
        const auto put = [&](HWND c, int cx, int cy, int cw, int ch) {
            if (place) MoveWindow(c, cx, cy, cw, ch, TRUE);
        };
        for (int i = 0; i < 6; ++i) {
            const int bw = width_of(g->buttons[i]);
            if (x > left && x + bw > right) {
                x = left;
                y += bh + gap;
            }
            put(g->buttons[i], x, y, bw, bh);
            x += bw + gap;
        }
        int group_w = 0;
        for (HWND c : right_group) group_w += width_of(c) + (c != right_group[0] ? gap : 0);
        const auto put_group_rows = [&](int from_y) {  // right-aligned, wrapping
            int y2 = from_y;
            std::size_t first = 0;
            while (first < std::size(right_group)) {
                std::size_t end = first;
                int row_w = 0;
                while (end < std::size(right_group) &&
                       (end == first || row_w + gap + width_of(right_group[end]) <= inner)) {
                    row_w += (end > first ? gap : 0) + width_of(right_group[end]);
                    ++end;
                }
                int gx = right - row_w;
                for (std::size_t i = first; i < end; ++i) {
                    const int cw = std::min(width_of(right_group[i]), inner);
                    put(right_group[i], std::max(left, gx), y2, cw, bh);
                    gx += cw + gap;
                }
                first = end;
                if (first < std::size(right_group)) y2 += bh + gap;
            }
            return y2;
        };
        if (right - x >= filter_min + gap + group_w) {  // all on this row
            put(g->filter, x, y + g->px(3), right - group_w - gap - x, bh - g->px(6));
            put_group_rows(y);
            return y + bh;
        }
        if (inner >= filter_min + gap + group_w) {  // the filter and the group on a row of their own
            y += bh + gap;
            put(g->filter, left, y + g->px(3), inner - group_w - gap, bh - g->px(6));
            put_group_rows(y);
            return y + bh;
        }
        if (right - x >= filter_min) {  // the filter fills this row, the group goes under
            put(g->filter, x, y + g->px(3), right - x, bh - g->px(6));
        } else {
            y += bh + gap;
            put(g->filter, left, y + g->px(3), inner, bh - g->px(6));
        }
        return put_group_rows(y + bh + gap) + bh;
    };
    icons_only = top_bar(false) > pad + 2 * bh + gap && ui::can_show_icon_only(g->buttons[0]);
    for (int i = 0; i < 6; ++i) ui::set_icon_only(g->buttons[i], icons_only);
    if (g->tips != nullptr && icons_only != g->icons_only) SendMessageW(g->tips, TTM_ACTIVATE, icons_only ? TRUE : FALSE, 0);
    g->icons_only = icons_only;
    const int bar_bottom = top_bar(true);

    // ---- the bottom bar: the summary, the progress while installing, the zoom and the theme
    const int field_h = combo_height(g->text_size);
    const int size_w = std::min(combo_width(g->text_size), inner), colours_w = std::min(combo_width(g->colours), inner);
    const int progress_w = g->busy ? g->px(160) : 0;
    const int tools_w = size_w + gap + colours_w + (progress_w > 0 ? progress_w + gap : 0);
    const bool one_row = inner - tools_w - gap * 2 >= g->px(240);
    g->status_w = one_row ? inner - tools_w - gap * 2 : inner;
    g->status_h = std::max(line, static_cast<int>(ui::text_extent(font, text_in(g->status), g->status_w).cy));

    int bar_top;
    if (one_row) {
        const int row_h = std::max(field_h, g->status_h);
        bar_top = h - pad / 2 - row_h - gap;
        const int y = bar_top + gap;
        MoveWindow(g->status, left, y + (row_h - g->status_h) / 2, g->status_w, g->status_h, TRUE);
        int x = right - colours_w;
        MoveWindow(g->colours, x, y + (row_h - field_h) / 2, colours_w, g->px(200), TRUE);
        x -= gap + size_w;
        MoveWindow(g->text_size, x, y + (row_h - field_h) / 2, size_w, g->px(200), TRUE);
        if (progress_w > 0) MoveWindow(g->progress, x - gap - progress_w, y + (row_h - g->px(16)) / 2, progress_w, g->px(16), TRUE);
    } else {
        // The summary on its own rows; under it the progress, the zoom and the theme, right-aligned (one under another
        // when even those do not fit side by side).
        const bool tools_in_row = tools_w <= inner;
        const int tools_h = tools_in_row ? field_h : field_h * 2 + gap + (progress_w > 0 ? g->px(16) + gap : 0);
        bar_top = h - pad / 2 - tools_h - gap - g->status_h - gap;
        MoveWindow(g->status, left, bar_top + gap, g->status_w, g->status_h, TRUE);
        const int y = bar_top + gap + g->status_h + gap;
        if (tools_in_row) {
            int x = right - colours_w;
            MoveWindow(g->colours, x, y, colours_w, g->px(200), TRUE);
            x -= gap + size_w;
            MoveWindow(g->text_size, x, y, size_w, g->px(200), TRUE);
            if (progress_w > 0) MoveWindow(g->progress, x - gap - progress_w, y + (field_h - g->px(16)) / 2, progress_w, g->px(16), TRUE);
        } else {
            int y2 = y;
            if (progress_w > 0) {
                MoveWindow(g->progress, right - std::min(progress_w, inner), y2, std::min(progress_w, inner), g->px(16), TRUE);
                y2 += g->px(16) + gap;
            }
            MoveWindow(g->text_size, right - size_w, y2, size_w, g->px(200), TRUE);
            MoveWindow(g->colours, right - colours_w, y2 + field_h + gap, colours_w, g->px(200), TRUE);
        }
    }

    // ---- the list, and the selected mod's card beside it, or under it when the window is narrow
    const int top = bar_bottom + pad;
    const int bottom = std::max(top + line * 4, bar_top - gap);
    const bool side_by_side = inner >= g->px(560);
    const int panel_w = side_by_side ? std::clamp(inner * 38 / 100, g->px(280), inner - g->px(260) - pad) : inner;
    const int list_w = side_by_side ? inner - panel_w - pad : inner;
    const int list_label_h = std::max(line, static_cast<int>(ui::text_extent(font, L"Installed mods (higher in the list wins a file both change)", list_w).cy));
    const int list_bottom = side_by_side ? bottom : top + (bottom - top) * 55 / 100;
    MoveWindow(g->list_label, left, top, list_w, list_label_h, TRUE);
    MoveWindow(g->list, left, top + list_label_h + g->px(2), list_w, std::max(line, list_bottom - top - list_label_h - g->px(2)), TRUE);
    g->panel = side_by_side ? RECT{left + list_w + pad, top, right, bottom} : RECT{left, list_bottom + pad, right, bottom};
    // The panel on the card (clear of its rounded corners) and what is on it, from its top; it scrolls when that is
    // taller than the card.
    const int edge = g->px(4);
    const int box_w = std::max(1, static_cast<int>(g->panel.right - g->panel.left) - edge * 2);
    const int box_h = std::max(1, static_cast<int>(g->panel.bottom - g->panel.top) - edge * 2);
    MoveWindow(g->card, g->panel.left + edge, g->panel.top + edge, box_w, box_h, TRUE);
    const mods::InstalledMod* m = selected_mod();
    const int options = m ? static_cast<int>(std::min<std::size_t>(m->manifest.options.size(), kMaxOptions)) : 0;
    const int option_field = combo_height(g->option_combos[0]);
    const int details_min = line * 4 + g->px(8);
    // Places the panel's controls for a panel `width` wide (or only measures them); returns their height.
    const auto card_content = [&](int width, bool place) {
        const int in = g->px(8), rx = in, inside_w = std::max(1, width - in * 2);
        const int shift = place ? g->card_scroll : 0;
        const auto put = [&](HWND c, int x, int y, int cw, int ch) {
            if (place) MoveWindow(c, x, y - shift, cw, ch, TRUE);
        };
        int y = in;
        // The preview gives up its room first when the card is short.
        int preview_h = std::min(g->px(200), box_h / 3);
        if (preview_h < g->px(48)) preview_h = 0;
        put(g->preview, rx, y, inside_w, preview_h);
        if (preview_h > 0) y += preview_h + gap;
        // Each option's name beside its list while both fit, the name over the list otherwise.
        int name_w = 0;
        for (int i = 0; i < options; ++i)
            name_w = std::max(name_w, static_cast<int>(ui::text_extent(font, text_in(g->option_labels[i])).cx) + g->px(4));
        const bool names_beside = name_w + gap + g->px(140) <= inside_w;
        for (int i = 0; i < kMaxOptions; ++i) {
            if (i >= options) {  // hidden: out of the way
                put(g->option_labels[i], rx, in, inside_w, line);
                put(g->option_combos[i], rx, in, inside_w, g->px(200));
                continue;
            }
            if (names_beside) {
                put(g->option_labels[i], rx, y + (option_field - line) / 2, name_w, line);
                put(g->option_combos[i], rx + name_w + gap, y, inside_w - name_w - gap, g->px(200));
            } else {
                const int name_h = std::max(line, static_cast<int>(ui::text_extent(font, text_in(g->option_labels[i]), inside_w).cy));
                put(g->option_labels[i], rx, y, inside_w, name_h);
                y += name_h + g->px(2);
                put(g->option_combos[i], rx, y, inside_w, g->px(200));
            }
            y += option_field + gap;
        }
        put(g->details_label, rx, y, inside_w, line);
        y += line + g->px(2);
        // The details fill the rest of the card, or take a few lines of their own when the card scrolls.
        const int details_h = std::max(details_min, box_h - in - y);
        put(g->details, rx, y, inside_w, details_h);
        return y + details_h + in;
    };
    int content = card_content(box_w, false);
    const bool scrolls = content > box_h;
    const int content_w = scrolls ? box_w - GetSystemMetricsForDpi(SM_CXVSCROLL, g->dpi) : box_w;
    if (scrolls) content = card_content(content_w, false);
    g->card_content_h = content;
    g->card_scroll = scrolls ? std::clamp(g->card_scroll, 0, content - box_h) : 0;
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMax = scrolls ? content - 1 : 0;
    info.nPage = static_cast<UINT>(box_h);
    info.nPos = g->card_scroll;
    SetScrollInfo(g->card, SB_VERT, &info, TRUE);
    card_content(content_w, true);
    InvalidateRect(g->window, nullptr, TRUE);
    // List columns: the name takes what the others leave. Set only when the list's width or the zoom changed, so a
    // column the player widened stays so while they pick mods or scroll the card.
    if (list_w == g->columns_w && g->look.text_size == g->columns_zoom) return;
    g->columns_w = list_w;
    g->columns_zoom = g->look.text_size;
    const int fixed = g->px(60) + g->px(110) + g->px(170) + g->px(250);
    ListView_SetColumnWidth(g->list, 0, std::max(g->px(140), list_w - fixed - g->px(24)));
    ListView_SetColumnWidth(g->list, 1, g->px(60));
    ListView_SetColumnWidth(g->list, 2, g->px(110));
    ListView_SetColumnWidth(g->list, 3, g->px(170));
    ListView_SetColumnWidth(g->list, 4, g->px(250));
}

void make_fonts() {
    if (g->font) DeleteObject(g->font);
    if (g->bold) DeleteObject(g->bold);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g->dpi);
    ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, g->look.text_size, 100);
    lstrcpynW(ncm.lfMessageFont.lfFaceName, ui::text_face(), LF_FACESIZE);
    g->font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW b = ncm.lfMessageFont;
    b.lfWeight = FW_SEMIBOLD;
    g->bold = CreateFontIndirectW(&b);
    EnumChildWindows(
        g->window,
        [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(g->font));
    SendMessageW(g->buttons[6], WM_SETFONT, reinterpret_cast<WPARAM>(g->bold), TRUE);
    EnumChildWindows(
        g->window,
        [](HWND child, LPARAM) -> BOOL {
            ui::fit_dropdown(child);  // a drop-down list's heights follow its font (nothing for other controls)
            return TRUE;
        },
        0);
}

// ---- text size and colours (tools/ui, shared with the settings program) -----------------------------------------------

// The colour choices in the list's order, which is ui::Theme's.
const char* const kThemeLabels[] = {"Theme: Same as Windows", "Theme: Light", "Theme: Dark", "Theme: Shadow"};

fs::path appearance_folder() { return executable_dir(); }

void show_look() {
    SendMessageW(g->text_size, CB_SETCURSEL, static_cast<WPARAM>((g->look.text_size - ui::kSmallestText) / ui::kTextStep), 0);
    SendMessageW(g->colours, CB_SETCURSEL, static_cast<WPARAM>(g->look.theme), 0);
}

// Uses a new text size or colour choice at once and keeps it in appearance.cfg. The window keeps its size: layout()
// fits the larger text into it.
void apply_look(ui::Appearance next, bool redraw = false) {
    next.text_size = ui::step_text_size(next.text_size, 0);
    const bool dark = ui::is_dark(next.theme);
    if (next == g->look && dark == ui::colors().dark && !redraw) return;
    const int before = g->look.text_size;
    const bool stored = next == g->look || g->checking || ui::save_appearance(appearance_folder(), next);
    g->look = next;
    ui::set_dark(dark, next.theme == ui::Theme::Shadow);
    make_fonts();
    g->filling = true;  // remaking the list's check boxes clears them: fill_list sets them again
    ui::apply_to_window(g->window);
    g->filling = false;
    show_look();
    layout();
    fill_list();
    RedrawWindow(g->window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    if (!stored)
        set_status(wide("The zoom and theme could not be stored in appearance.cfg, so they are used only until this "
                        "window closes."));
    else if (g->look.text_size != before)
        set_status(wide("Zoom " + std::to_string(g->look.text_size) +
                        " %. Ctrl and + or - also zoom in and out, and Ctrl and 0 goes back to 100 %."));
}

// A colour change Windows announced while a pop-up window was open.
void apply_pending_look() {
    if (!g->look_pending) return;
    g->look_pending = false;
    apply_look(g->look, true);
}

// A zoom or theme list's choice is used when the list closes, or at once when it is changed while closed (arrow keys
// on the closed list): not while the player moves through the open list.
bool look_chosen(HWND list, int code) {
    return code == CBN_CLOSEUP || (code == CBN_SELCHANGE && SendMessageW(list, CB_GETDROPPEDSTATE, 0, 0) == FALSE);
}

void step_text(int steps) {
    ui::Appearance next = g->look;
    next.text_size = steps == 0 ? ui::kSmallestText : ui::step_text_size(g->look.text_size, steps);
    apply_look(next);
}

// A pop-up window of the manager (Code mods, the small forms) in the current colours.
LRESULT popup_colors(UINT msg, WPARAM wp, LPARAM lp, HWND w, bool* handled) {
    *handled = true;
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT r;
            GetClientRect(w, &r);
            FillRect(reinterpret_cast<HDC>(wp), &r, ui::window_brush());
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return ui::control_color(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
        case WM_NOTIFY: {
            LRESULT drawn = 0;
            if (ui::custom_draw(reinterpret_cast<const NMHDR*>(lp), &drawn)) return drawn;
            break;
        }
    }
    *handled = false;
    return 0;
}

// The selected mod's panel: on the card's colour; what its controls tell goes to the main window, as if they were its
// own; it scrolls up and down when they are taller than it.
LRESULT CALLBACK card_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT r;
            GetClientRect(h, &r);
            FillRect(reinterpret_cast<HDC>(wp), &r, ui::surface_brush());
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_NOTIFY:
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
            return SendMessageW(GetParent(h), msg, wp, lp);
        case WM_COMMAND:
            if (HIWORD(wp) == CBN_SETFOCUS || HIWORD(wp) == EN_SETFOCUS) {
                // Tab reached a control scrolled out of the card: scroll it into view.
                RECT r, box;
                GetWindowRect(reinterpret_cast<HWND>(lp), &r);
                MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&r), 2);
                GetClientRect(h, &box);
                int scroll = g->card_scroll;
                if (r.top < 0) scroll += r.top - g->px(8);
                else if (r.bottom > box.bottom) scroll += std::min(static_cast<int>(r.top) - g->px(8), static_cast<int>(r.bottom - box.bottom) + g->px(8));
                if (scroll != g->card_scroll) {
                    g->card_scroll = scroll;
                    layout();
                }
            }
            return SendMessageW(GetParent(h), msg, wp, lp);
        case WM_VSCROLL: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(h, SB_VERT, &info);
            const int line = g->px(24), page = std::max(line, static_cast<int>(info.nPage) - line);
            int pos = info.nPos;
            switch (LOWORD(wp)) {
                case SB_LINEUP: pos -= line; break;
                case SB_LINEDOWN: pos += line; break;
                case SB_PAGEUP: pos -= page; break;
                case SB_PAGEDOWN: pos += page; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = info.nTrackPos; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = info.nMax; break;
                default: return 0;
            }
            g->card_scroll = pos;
            layout();
            return 0;
        }
        case WM_MOUSEWHEEL:
            if ((GetWindowLongW(h, GWL_STYLE) & WS_VSCROLL) == 0) break;
            g->card_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * g->px(48) / WHEEL_DELTA;
            layout();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void create_controls(HWND w) {
    g->window = w;
    g->dpi = GetDpiForWindow(w);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const wchar_t* labels[] = {L"Install...", L"Remove", L"Move up", L"Move down", L"Open folder", L"Refresh", L"Play"};
    const int ids[] = {kIdInstall, kIdRemove, kIdUp, kIdDown, kIdFolder, kIdRefresh, kIdPlay};
    for (int i = 0; i < 7; ++i)
        g->buttons[i] = CreateWindowExW(0, WC_BUTTONW, labels[i],
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == 6 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                                        0, 0, 10, 10, w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])), inst, nullptr);
    g->filter = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0,
                                0, 10, 10, w, reinterpret_cast<HMENU>(kIdFilter), inst, nullptr);
    SendMessageW(g->filter, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter (Ctrl+F)"));
    g->code = CreateWindowExW(0, WC_BUTTONW, L"Code...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                              w, reinterpret_cast<HMENU>(kIdCode), inst, nullptr);
    g->profiles = CreateWindowExW(0, WC_BUTTONW, L"Profiles \u25BE", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                  0, 10, 10, w, reinterpret_cast<HMENU>(kIdProfiles), inst, nullptr);
    g->progress = CreateWindowExW(0, PROGRESS_CLASSW, L"Installing", WS_CHILD | PBS_MARQUEE, 0, 0, 10, 10, w,
                                  reinterpret_cast<HMENU>(kIdProgress), inst, nullptr);
    g->master = CreateWindowExW(0, WC_BUTTONW, L"Load mods", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0,
                                10, 10, w, reinterpret_cast<HMENU>(kIdMaster), inst, nullptr);
    // A visible label before the list: the list-view's own window text is not used as its accessible name.
    g->list_label = CreateWindowExW(0, WC_STATICW, L"Installed mods (higher in the list wins a file both change)",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, w, nullptr, inst, nullptr);
    g->list = CreateWindowExW(0, WC_LISTVIEWW, L"Installed mods",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL |
                                  LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                              0, 0, 10, 10, w, reinterpret_cast<HMENU>(kIdList), inst, nullptr);
    ListView_SetExtendedListViewStyle(g->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                   LVS_EX_LABELTIP);  // its theme: ui::apply_to_window below
    const wchar_t* cols[] = {L"Mod (highest priority first)", L"Version", L"Author", L"Content", L"Status"};
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = 100;
        ListView_InsertColumn(g->list, i, &c);
    }
    // The selected mod's preview, options and details, on a panel of their own that scrolls when they do not fit.
    WNDCLASSEXW card_class{sizeof(card_class)};
    card_class.lpfnWndProc = card_proc;
    card_class.hInstance = inst;
    card_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    card_class.lpszClassName = L"ShadowModManagerCard";
    RegisterClassExW(&card_class);
    g->card = CreateWindowExW(WS_EX_CONTROLPARENT, L"ShadowModManagerCard", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, 10, 10, w, nullptr, inst, nullptr);
    HWND card = g->card;
    g->preview = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 10, 10, card,
                                 reinterpret_cast<HMENU>(kIdPreview), inst, nullptr);
    for (int i = 0; i < kMaxOptions; ++i) {
        g->option_labels[i] = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, card,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdOptionLabel0 + i)), inst, nullptr);
        g->option_combos[i] = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                              0, 0, 10, 10, card,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdOptionCombo0 + i)), inst, nullptr);
    }
    // The label names the pane for screen readers and UI Automation (it precedes the edit in tab order).
    g->details_label = CreateWindowExW(0, WC_STATICW, L"Details", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10,
                                       10, card, nullptr, inst, nullptr);
    g->details = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0,
                                 10, 10, card, reinterpret_cast<HMENU>(kIdDetails), inst, nullptr);
    g->status = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, w,
                                reinterpret_cast<HMENU>(kIdStatus), inst, nullptr);
    // The text size and colours, shared with the settings program (appearance.cfg).
    g->text_size = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                   0, 0, 10, 10, w, reinterpret_cast<HMENU>(kIdTextSize), inst, nullptr);
    for (int size = ui::kSmallestText; size <= ui::kLargestText; size += ui::kTextStep)
        SendMessageW(g->text_size, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide("Zoom: " + std::to_string(size) + " %").c_str()));
    g->colours = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                 0, 0, 10, 10, w, reinterpret_cast<HMENU>(kIdColours), inst, nullptr);
    for (const char* label : kThemeLabels) SendMessageW(g->colours, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(label).c_str()));
    // Icons before the buttons' words (Segoe Fluent Icons), and Play as the window's main button.
    const wchar_t icons[] = {0xE710 /*add*/, 0xE74D /*delete*/, 0xE74A /*up*/, 0xE74B /*down*/, 0xE838 /*folder*/,
                             0xE72C /*refresh*/, 0xE768 /*play*/};
    for (int i = 0; i < 7; ++i) ui::set_icon(g->buttons[i], icons[i]);
    ui::set_primary(g->buttons[6]);
    // The six buttons' names as tooltips, used when a narrow window or a large zoom leaves them only their icons.
    g->tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, w, nullptr, inst, nullptr);
    for (int i = 0; i < 6; ++i) {
        TOOLINFOW tip{};
        tip.cbSize = sizeof(tip);
        tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tip.hwnd = w;
        tip.uId = reinterpret_cast<UINT_PTR>(g->buttons[i]);
        tip.lpszText = const_cast<wchar_t*>(labels[i]);
        SendMessageW(g->tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    SendMessageW(g->tips, TTM_ACTIVATE, FALSE, 0);
    make_fonts();
    show_look();
    DragAcceptFiles(w, TRUE);
    ui::apply_to_window(w);
}

LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (g->list != nullptr && msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        if (g->panel.right > g->panel.left) ui::paint_card(dc, g->panel, g->px(6));
        EndPaint(w, &ps);
        return 0;
    }
    if (g->list != nullptr && (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN)) {
        // The labels on the details card have the card's background.
        const auto control = reinterpret_cast<HWND>(lp);
        bool on_panel = control == g->details_label;
        for (HWND label : g->option_labels) on_panel = on_panel || control == label;
        for (HWND combo : g->option_combos) on_panel = on_panel || control == combo;
        if (on_panel) return ui::control_color(msg, reinterpret_cast<HDC>(wp), control, false, true);
    }
    if (g->list != nullptr) {
        bool handled = false;
        const LRESULT colored = popup_colors(msg, wp, lp, w, &handled);
        if (handled) return colored;
    }
    switch (msg) {
        case kMsgLook:  // posted, so the list that was used is not remade while it is still telling about the choice
            apply_look(ui::Appearance{static_cast<ui::Theme>(wp), static_cast<int>(lp)});
            return 0;
        case WM_SETTINGCHANGE:
            // Windows' light or dark app mode (followed when the theme is "same as Windows"), its accent colour and its
            // high contrast mode: drawn again in the new colours.
            if (((lp != 0 && lstrcmpW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) || wp == SPI_SETHIGHCONTRAST) &&
                ui::colors_differ(ui::is_dark(g->look.theme), g->look.theme == ui::Theme::Shadow)) {
                // Not under an open pop-up (Code mods, a form): its controls use the fonts apply_look remakes.
                if (IsWindowEnabled(w)) apply_look(g->look, true);
                else g->look_pending = true;
            }
            break;
        case WM_MEASUREITEM:
            if (ui::measure_item(reinterpret_cast<MEASUREITEMSTRUCT*>(lp))) return TRUE;
            break;
        case WM_CREATE:
            create_controls(w);
            return 0;
        case WM_SIZE:
            if (g->list) layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const SIZE smallest = smallest_window(GetDpiForWindow(w));
            mmi->ptMinTrackSize = {smallest.cx, smallest.cy};
            return 0;
        }
        case WM_DPICHANGED: {
            g->dpi = HIWORD(wp);
            g->columns_w = -1;  // the columns again at the new scale
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
            make_fonts();
            layout();
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            std::vector<fs::path> sources;
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                std::wstring buf(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                DragQueryFileW(drop, i, buf.data(), static_cast<UINT>(buf.size()));
                buf.resize(wcslen(buf.c_str()));
                sources.emplace_back(buf);
            }
            DragFinish(drop);
            start_install(std::move(sources));
            return 0;
        }
        case kMsgInstalled:
            finish_install(reinterpret_cast<InstallJob*>(lp));
            return 0;
        case kMsgCodeBuilt:
            finish_code_build(static_cast<DWORD>(wp));
            return 0;
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wp) == g->list && !g->busy) {
                int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
                if (x == -1 && y == -1) {  // the keyboard's menu key: under the selected row
                    RECT r{};
                    const int row = ListView_GetNextItem(g->list, -1, LVNI_SELECTED);
                    if (row >= 0) ListView_GetItemRect(g->list, row, &r, LVIR_LABEL);
                    POINT pt{r.left + g->px(20), r.bottom};
                    ClientToScreen(g->list, &pt);
                    x = pt.x;
                    y = pt.y;
                } else {
                    // Right-click selects the row under the pointer first.
                    POINT pt{x, y};
                    ScreenToClient(g->list, &pt);
                    LVHITTESTINFO hit{};
                    hit.pt = pt;
                    const int row = ListView_HitTest(g->list, &hit);
                    if (row >= 0) ListView_SetItemState(g->list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
                context_menu(x, y);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (!g->drag_key.empty()) drag_move(false);
            break;
        case WM_LBUTTONUP:
            if (!g->drag_key.empty()) {
                // ReleaseCapture sends WM_CAPTURECHANGED at once, whose handler cancels a drag: hold the key aside.
                const std::string dragged = g->drag_key;
                g->drag_key.clear();
                ReleaseCapture();
                g->drag_key = dragged;
                drag_move(true);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (!g->drag_key.empty() && reinterpret_cast<HWND>(lp) != w) {
                g->drag_key.clear();
                LVINSERTMARK none{sizeof(none)};
                none.iItem = -1;
                ListView_SetInsertMark(g->list, &none);
            }
            break;
        case WM_DRAWITEM: {
            auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (ui::draw_item(di)) return TRUE;  // the drop-down lists
            if (di->CtlID != kIdPreview) break;
            FillRect(di->hDC, &di->rcItem, ui::surface_brush());  // on the details card
            const int bw = di->rcItem.right - di->rcItem.left, bh = di->rcItem.bottom - di->rcItem.top;
            if (g->preview_bitmap && g->preview_w > 0) {
                const double s = std::min(static_cast<double>(bw) / g->preview_w, static_cast<double>(bh) / g->preview_h);
                const int dw = static_cast<int>(g->preview_w * s), dh = static_cast<int>(g->preview_h * s);
                HDC mem = CreateCompatibleDC(di->hDC);
                HGDIOBJ old = SelectObject(mem, g->preview_bitmap);
                SetStretchBltMode(di->hDC, HALFTONE);
                BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                AlphaBlend(di->hDC, di->rcItem.left + (bw - dw) / 2, di->rcItem.top + (bh - dh) / 2, dw, dh, mem, 0, 0,
                           g->preview_w, g->preview_h, bf);
                SelectObject(mem, old);
                DeleteDC(mem);
            } else {
                SetBkMode(di->hDC, TRANSPARENT);
                SelectObject(di->hDC, g->font);
                SetTextColor(di->hDC, ui::colors().muted);
                RECT r = di->rcItem;
                DrawTextW(di->hDC, selected_mod() ? L"No preview image" : L"Drop mod archives or folders here", -1,
                          &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            return TRUE;
        }
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->idFrom == kIdList && hdr->code == LVN_ITEMCHANGED && !g->filling) {
                auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
                if (nm->uChanged & LVIF_STATE) {
                    const UINT oldc = (nm->uOldState & LVIS_STATEIMAGEMASK) >> 12;
                    const UINT newc = (nm->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                    if (oldc != 0 && newc != 0 && oldc != newc) {
                        on_check_changed(nm->iItem, newc == 2);
                    } else if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED)) {
                        g->selected = key_of_row(nm->iItem);
                        show_details();
                        layout();
                    }
                }
            } else if (hdr->idFrom == kIdList && hdr->code == LVN_KEYDOWN) {
                auto* kd = reinterpret_cast<NMLVKEYDOWN*>(lp);
                if (kd->wVKey == VK_DELETE) remove_selected();
                if (GetKeyState(VK_MENU) & 0x8000) {
                    if (kd->wVKey == VK_UP || kd->wVKey == VK_DOWN)
                        PostMessageW(w, WM_COMMAND, kd->wVKey == VK_UP ? kIdUp : kIdDown, 0);
                    if (kd->wVKey == VK_HOME || kd->wVKey == VK_END)
                        PostMessageW(w, WM_COMMAND, kd->wVKey == VK_HOME ? kCmdTop : kCmdBottom, 0);
                }
            } else if (hdr->idFrom == kIdList && hdr->code == LVN_BEGINDRAG && !g->busy) {
                auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
                const std::string key = key_of_row(nm->iItem);
                if (!key.empty() && key[0] != '!') {
                    g->drag_key = key;
                    SetCapture(w);
                }
            } else if (hdr->idFrom == kIdList && hdr->code == NM_DBLCLK && !g->busy) {
                auto* ia = reinterpret_cast<NMITEMACTIVATE*>(lp);
                if (ia->iItem >= 0) toggle_selected();
            } else if (hdr->idFrom == kIdList && hdr->code == NM_CUSTOMDRAW) {
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                switch (cd->nmcd.dwDrawStage) {
                    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
                    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
                    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                        const std::size_t row = cd->nmcd.dwItemSpec;
                        cd->clrText = CLR_DEFAULT;
                        if (cd->iSubItem == 4 && row < g->row_status_color.size()) cd->clrText = g->row_status_color[row];
                        return CDRF_DODEFAULT;
                    }
                }
            }
            break;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == kIdFilter && HIWORD(wp) == EN_CHANGE) {
                fill_list();
                return 0;
            }
            if (id == kIdTextSize && look_chosen(g->text_size, HIWORD(wp))) {
                const int i = static_cast<int>(SendMessageW(g->text_size, CB_GETCURSEL, 0, 0));
                if (i >= 0) PostMessageW(w, kMsgLook, static_cast<WPARAM>(g->look.theme), ui::kSmallestText + i * ui::kTextStep);
                return 0;
            }
            if (id == kIdColours && look_chosen(g->colours, HIWORD(wp))) {
                const int i = static_cast<int>(SendMessageW(g->colours, CB_GETCURSEL, 0, 0));
                if (i >= 0 && i < static_cast<int>(std::size(kThemeLabels))) PostMessageW(w, kMsgLook, static_cast<WPARAM>(i), g->look.text_size);
                return 0;
            }
            if (id == kCmdTextLarger || id == kCmdTextSmaller || id == kCmdTextNormal) {
                step_text(id == kCmdTextLarger ? 1 : id == kCmdTextSmaller ? -1 : 0);
                return 0;
            }
            if (g->busy) break;
            if (id >= kIdOptionCombo0 && id < kIdOptionCombo0 + kMaxOptions && HIWORD(wp) == CBN_SELCHANGE &&
                !g->filling) {
                if (read_only_refused()) return 0;
                const mods::InstalledMod* m = selected_mod();
                const int i = id - kIdOptionCombo0;
                if (m && m->usable && i < static_cast<int>(m->manifest.options.size())) {
                    const int sel = static_cast<int>(SendMessageW(g->option_combos[i], CB_GETCURSEL, 0, 0));
                    const mods::ModOption& o = m->manifest.options[static_cast<std::size_t>(i)];
                    if (sel >= 0 && sel < static_cast<int>(o.choices.size()))
                        g->model.entry(m->manifest.id)->options[o.id] = o.choices[static_cast<std::size_t>(sel)].id;
                    changed();
                }
                return 0;
            }
            switch (id) {
                case kIdInstall: {
                    std::wstring buf(65536, L'\0');
                    OPENFILENAMEW ofn{};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = w;
                    ofn.lpstrFilter = L"Mod archives (*.zip;*.7z;*.rar)\0*.zip;*.7z;*.rar\0All files\0*.*\0";
                    ofn.lpstrFile = buf.data();
                    ofn.nMaxFile = static_cast<DWORD>(buf.size());
                    ofn.lpstrTitle = L"Install mods (folders: drop them on the window)";
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
                    if (GetOpenFileNameW(&ofn)) {
                        std::vector<fs::path> sources;
                        const fs::path first(buf.c_str());
                        const wchar_t* p = buf.c_str() + wcslen(buf.c_str()) + 1;
                        if (*p == 0) {
                            sources.push_back(first);
                        } else {
                            for (; *p; p += wcslen(p) + 1) sources.push_back(first / p);
                        }
                        start_install(std::move(sources));
                    }
                    return 0;
                }
                case kIdRemove: remove_selected(); return 0;
                case kIdUp:
                case kIdDown:
                    if (!g->selected.empty() && g->selected[0] != '!' && !read_only_refused()) {
                        g->model.move(g->selected, id == kIdUp ? -1 : 1);
                        changed();
                    }
                    return 0;
                case kIdFolder: {
                    const mods::InstalledMod* m = selected_mod();
                    const fs::path p = m ? m->folder : g->model.mods_dir;
                    ShellExecuteW(w, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                case kIdRefresh: refresh_all(); return 0;
                case kIdMaster:
                    if (read_only_refused()) return 0;
                    g->model.state.mods_enabled = Button_GetCheck(g->master) == BST_CHECKED;
                    changed();
                    return 0;
                case kIdPlay: play(); return 0;
                case kIdProfiles: profiles_menu(); return 0;
                case kIdCode: code_window(); return 0;
                case kCmdToggle: toggle_selected(); return 0;
                case kCmdTop: move_selected_to(0); return 0;
                case kCmdBottom: move_selected_to(g->model.state.order.size()); return 0;
                case kCmdEnableAll: set_all(true); return 0;
                case kCmdDisableAll: set_all(false); return 0;
                case kCmdEditInfo: edit_info(); return 0;
                case kCmdFocusFilter:
                    SetFocus(g->filter);
                    SendMessageW(g->filter, EM_SETSEL, 0, -1);
                    return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (g->busy) {
                MessageBoxW(w, L"An installation is still running.", window_title(), MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (g->snapshot.empty()) save_placement();
            DestroyWindow(w);
            return 0;
        case WM_TIMER:
            if (wp == 1) {
                KillTimer(w, 1);
                save_snapshot(w, g->snapshot);
                DestroyWindow(w);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

// --check-layout <report>: the window at every zoom, in its start-up size, its smallest size and a wide one, while idle
// and while installing, with a mod that has options selected when there is one; the window is never shown. Each
// control whose text does not fit, each control outside the window, and any zoom that changed the window's size is
// written to the report (UTF-8), one per line. Zoom is not stored. Returns the number of problems.
int check_layout(const fs::path& report_file) {
    g->checking = true;
    const ui::Appearance first = g->look;
    for (const mods::StateEntry& e : g->model.state.order) {
        const mods::InstalledMod* m = g->model.find(e.id);
        if (m != nullptr && !m->manifest.options.empty()) {
            g->selected = e.id;
            break;
        }
    }
    fill_list();
    std::wstring report;
    int problems = 0;
    RECT start;
    GetWindowRect(g->window, &start);
    const SIZE smallest = smallest_window(g->dpi);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(g->window, MONITOR_DEFAULTTONEAREST), &monitor);
    const struct {
        const wchar_t* name;
        int w, h;
    } sizes[] = {{L"start-up size", start.right - start.left, start.bottom - start.top},
                 {L"smallest size", smallest.cx, smallest.cy},
                 {L"wide", static_cast<int>(monitor.rcWork.right - monitor.rcWork.left),
                  std::min(static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top), MulDiv(800, static_cast<int>(g->dpi), 96))}};
    report += L"# selected: " + wide(g->selected.empty() ? std::string("none") : g->selected) + L"\n";
    for (const auto& size : sizes) {
        SetWindowPos(g->window, nullptr, monitor.rcWork.left, monitor.rcWork.top, size.w, size.h, SWP_NOZORDER | SWP_NOACTIVATE);
        for (int zoom = ui::kSmallestText; zoom <= ui::kLargestText; zoom += ui::kTextStep) {
            RECT before, after;
            GetWindowRect(g->window, &before);
            apply_look(ui::Appearance{g->look.theme, zoom});
            GetWindowRect(g->window, &after);
            const std::wstring when = std::wstring(size.name) + L" " + std::to_wstring(before.right - before.left) + L"x" +
                                      std::to_wstring(before.bottom - before.top) + L", zoom " + std::to_wstring(zoom) + L" %";
            report += L"# " + when + (g->icons_only ? L": icons only" : L": buttons with words") + L"\n";
            if (!EqualRect(&before, &after)) {
                report += when + L": the window's size changed\n";
                ++problems;
            }
            for (const bool busy : {false, true}) {
                set_busy(busy);
                for (const std::wstring& line : ui::find_clipped(g->window, true)) {
                    report += when + (busy ? L", installing: " : L": ") + line + L"\n";
                    ++problems;
                }
            }
            set_busy(false);
        }
    }
    apply_look(first);
    SetWindowPos(g->window, nullptr, start.left, start.top, start.right - start.left, start.bottom - start.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    report += L"problems: " + std::to_wstring(problems) + L"\n";
    std::ofstream out(mods::long_path(report_file), std::ios::binary | std::ios::trunc);
    out << narrow(report);
    return problems;
}

int run_gui(const std::vector<std::string>& args) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    static Gui gui;
    g = &gui;
    std::optional<fs::path> mods_dir, game_data;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--mods-dir") mods_dir = mods::from_utf8(args[i + 1]);
        if (args[i] == "--game-data") game_data = mods::from_utf8(args[i + 1]);
    }
    g->model.mods_dir = mods_dir ? *mods_dir : executable_dir() / "mods";
    g->game_exe = executable_dir() / "ShadowTheHedgehog.exe";
    if (const auto root = find_game_root(game_data)) {
        g->model.game_root = *root;
        g->model.index_game();
    } else {
        g->model.game_error = "no folder with files/ beside the manager or in the working directory";
    }
    ui::start_drawing();
    g->look = ui::load_appearance(appearance_folder());
    g->look.text_size = ui::step_text_size(g->look.text_size, 0);
    ui::set_dark(ui::is_dark(g->look.theme), g->look.theme == ui::Theme::Shadow);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"ShadowModManager";
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    // WS_CLIPCHILDREN: the card painted on the window never covers the panel and controls on it.
    HWND w = CreateWindowExW(0, wc.lpszClassName, window_title(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                             CW_USEDEFAULT, 1180, 720, nullptr, nullptr, wc.hInstance, nullptr);
    const UINT dpi = GetDpiForWindow(w);
    SetWindowPos(w, nullptr, 0, 0, MulDiv(1180, static_cast<int>(dpi), 96), MulDiv(720, static_cast<int>(dpi), 96),
                 SWP_NOMOVE | SWP_NOZORDER);
    refresh_all();
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--check-layout") {
            const int problems = check_layout(mods::from_utf8(args[i + 1]));
            DestroyWindow(w);
            return problems > 0 ? 1 : 0;
        }
    if (g->model.game.empty())
        MessageBoxW(w, wide("The game's files were not found: " + g->model.game_error +
                            ".\nInstalling needs them; put ModManager.exe beside ShadowTheHedgehog.exe.")
                           .c_str(),
                    window_title(), MB_OK | MB_ICONWARNING);
    layout();
    bool snapshot_run = false;
    for (const std::string& a : args) snapshot_run |= a == "--snapshot";
    int show = SW_SHOWDEFAULT;
    if (!snapshot_run) restore_placement(show);  // a picture for the docs always uses the default size
    ShowWindow(w, show);
    layout();
    ACCEL keys[] = {{FCONTROL | FVIRTKEY, 'O', kIdInstall},       {FVIRTKEY, VK_F5, kIdRefresh},
                    {FCONTROL | FVIRTKEY, 'F', kCmdFocusFilter},  {FCONTROL | FVIRTKEY, 'E', kCmdEditInfo},
                    {FCONTROL | FVIRTKEY, 'P', kIdPlay},          {FCONTROL | FVIRTKEY, VK_OEM_PLUS, kCmdTextLarger}, {FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_PLUS, kCmdTextLarger},
                    {FCONTROL | FVIRTKEY, VK_ADD, kCmdTextLarger}, {FCONTROL | FVIRTKEY, VK_OEM_MINUS, kCmdTextSmaller},
                    {FCONTROL | FVIRTKEY, VK_SUBTRACT, kCmdTextSmaller}, {FCONTROL | FVIRTKEY, '0', kCmdTextNormal},
                    {FCONTROL | FVIRTKEY, VK_NUMPAD0, kCmdTextNormal}};
    g->accelerators = CreateAcceleratorTableW(keys, static_cast<int>(std::size(keys)));
    // --snapshot <file.png>: the window as drawn, written once it has painted, then the program ends (documentation
    // and checks of the layout without anyone at the screen). --select <id> picks the mod shown in the details.
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--select") {
            g->selected = args[i + 1];
            fill_list();
            layout();
        }
        if (args[i] == "--snapshot") {
            g->snapshot = mods::from_utf8(args[i + 1]);
            SetTimer(w, 1, 1500, nullptr);
        }
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(w, g->accelerators, &m)) continue;
        // Ctrl with the mouse wheel: the text size, over any part of the window.
        if (m.message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(m.wParam) & MK_CONTROL) != 0 && GetAncestor(m.hwnd, GA_ROOT) == w) {
            step_text(GET_WHEEL_DELTA_WPARAM(m.wParam) > 0 ? 1 : -1);
            continue;
        }
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DestroyAcceleratorTable(g->accelerators);
    ui::stop_drawing();
    CoUninitialize();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(narrow(argv[i]));
    LocalFree(argv);
    bool command = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--mods-dir" || args[i] == "--game-data" || args[i] == "--snapshot" || args[i] == "--select" ||
            args[i] == "--check-layout") {
            ++i;
            continue;
        }
        command = true;
    }
    return command ? run_cli(args) : run_gui(args);
}
