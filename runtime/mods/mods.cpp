// Mod model: manifests, discovery, state, resolution, game index (shadow/mods/mods.h).
#include "shadow/mods/mods.h"
#include "shadow/mods/insight.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <chrono>
#include <system_error>
#include <thread>

namespace shadow::mods {

namespace fs = std::filesystem;

// ---- small helpers -----------------------------------------------------------------------------------------------

std::string utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    std::string s(text.begin(), text.end());
    if (s.rfind("//?/UNC/", 0) == 0) return "//" + s.substr(8);
    if (s.rfind("//?/", 0) == 0) return s.substr(4);
    return s;
}

fs::path long_path(const fs::path& path) {
#if defined(_WIN32)
    std::error_code ec;
    std::wstring s = fs::absolute(path, ec).wstring();
    if (ec || s.empty()) return path;
    std::replace(s.begin(), s.end(), L'/', L'\\');
    if (s.rfind(L"\\\\?\\", 0) == 0) return fs::path(s);
    if (s.rfind(L"\\\\", 0) == 0) return fs::path(L"\\\\?\\UNC\\" + s.substr(2));
    return fs::path(L"\\\\?\\" + s);
#else
    return path;
#endif
}

fs::path from_utf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool read_text(const fs::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return static_cast<bool>(in) || in.eof();
}

bool write_atomically(const fs::path& file, const std::string& text, std::string& error) {
    std::error_code ec;
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path(), ec);
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + utf8(temp);
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            error = "cannot write " + utf8(temp);
            return false;
        }
    }
    // Another program can hold the file open for a moment (a virus scanner, an indexer, a backup tool) and refuse
    // the replace; try for about half a second before reporting it (docs/mods/GUI_TEST.md, finding 1).
    for (int attempt = 0; attempt < 8; ++attempt) {
        ec.clear();
        fs::rename(temp, file, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
    }
    error = "cannot replace " + utf8(file) + ": " + ec.message() + " (is another program using it?)";
    std::error_code rm;
    fs::remove(temp, rm);
    return false;
}

// A real directory or file, never a link or junction (std::filesystem reports junctions as their own type on MSVC).
bool is_plain_dir(const fs::directory_entry& e) {
    std::error_code ec;
    const fs::file_status s = e.symlink_status(ec);
    return !ec && s.type() == fs::file_type::directory;
}
bool is_plain_file(const fs::directory_entry& e) {
    std::error_code ec;
    const fs::file_status s = e.symlink_status(ec);
    return !ec && s.type() == fs::file_type::regular;
}

// Every plain file below `root`, as ('/'-separated relative path, absolute path), sorted by relative path.
// Links and junctions are not followed. Depth is bounded.
void list_files(const fs::path& root, std::vector<std::pair<std::string, fs::path>>& out, unsigned depth = 0,
                const std::string& prefix = {}) {
    if (depth > 32u) return;
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (fs::directory_iterator it(depth == 0 ? long_path(root) : root, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(*it);
    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path() < b.path(); });
    for (const fs::directory_entry& e : entries) {
        const std::string name = utf8(e.path().filename());
        const std::string rel = prefix.empty() ? name : prefix + "/" + name;
        if (is_plain_dir(e)) {
            list_files(e.path(), out, depth + 1u, rel);
        } else if (is_plain_file(e)) {
            out.emplace_back(rel, e.path());
        }
    }
}

// The storage contract's name rule (shadow/runtime/v1/runtime.h): printable ASCII, no '/' or '\\', not "." or "..".
bool disc_name_ok(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (const char ch : name) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < 0x20u || b > 0x7Eu || b == '/' || b == '\\') return false;
    }
    return true;
}

bool disc_path_ok(std::string_view path) {
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos
                                                                                          : slash - start);
        if (!disc_name_ok(part)) return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

bool is_texture_name(std::string_view name) {
    const std::string l = lower(name);
    if (l.rfind("tex1_", 0) != 0) return false;
    return l.size() > 4 && (l.ends_with(".png") || l.ends_with(".dds"));
}

std::string version_text(const json::Value& v) {
    if (v.is_string()) return v.as_string();
    if (v.is_number()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v.as_number());
        return buf;
    }
    return {};
}

}  // namespace

const char* severity_name(Severity s) noexcept {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "?";
}

bool has_errors(const std::vector<Issue>& issues, std::string_view mod_id) {
    return std::any_of(issues.begin(), issues.end(), [&](const Issue& i) {
        return i.severity == Severity::Error && (mod_id.empty() || i.mod_id == mod_id);
    });
}

// ---- versions ----------------------------------------------------------------------------------------------------

std::optional<Version> Version::parse(std::string_view text) {
    Version v;
    std::size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;
    int part = 0;
    bool digits = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            const std::uint64_t next = static_cast<std::uint64_t>(v.parts[part]) * 10u + static_cast<unsigned>(c - '0');
            if (next > 0xFFFFFFFFull) return std::nullopt;
            v.parts[part] = static_cast<std::uint32_t>(next);
            digits = true;
        } else if (c == '.') {
            if (!digits || part == 3) return std::nullopt;
            ++part;
            digits = false;
        } else if (c == '-' || c == '+') {
            if (!digits) return std::nullopt;
            v.suffix = std::string(text.substr(i + 1));
            return v;
        } else {
            return std::nullopt;
        }
    }
    if (!digits) return std::nullopt;
    return v;
}

int Version::compare(const Version& o) const noexcept {
    for (int i = 0; i < 4; ++i)
        if (parts[i] != o.parts[i]) return parts[i] < o.parts[i] ? -1 : 1;
    if (suffix == o.suffix) return 0;
    if (suffix.empty()) return 1;
    if (o.suffix.empty()) return -1;
    return suffix < o.suffix ? -1 : 1;
}

// ---- manifest ----------------------------------------------------------------------------------------------------

bool valid_mod_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64u) return false;
    if (!std::isalnum(static_cast<unsigned char>(id[0]))) return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string make_mod_id(std::string_view text) {
    std::string out;
    bool dash = false;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) && c < 0x80) {
            if (dash && !out.empty()) out.push_back('-');
            out.push_back(static_cast<char>(std::tolower(c)));
            dash = false;
        } else {
            dash = true;
        }
        if (out.size() >= 48u) break;
    }
    if (out.empty()) out = "mod";
    return out;
}

namespace {

void string_member(const json::Value& obj, std::string_view key, std::string& out, std::vector<Issue>& issues,
                   const std::string& id) {
    if (const json::Value* v = obj.find(key)) {
        if (v->is_string()) {
            out = v->as_string();
        } else if (!v->is_null()) {
            issues.push_back({Severity::Warning, id, "mod.json: '" + std::string(key) + "' is not a string; ignored"});
        }
    }
}

void string_list(const json::Value& obj, std::string_view key, std::vector<std::string>& out,
                 std::vector<Issue>& issues, const std::string& id) {
    const json::Value* v = obj.find(key);
    if (v == nullptr || v->is_null()) return;
    if (!v->is_array()) {
        issues.push_back({Severity::Warning, id, "mod.json: '" + std::string(key) + "' is not a list; ignored"});
        return;
    }
    for (const json::Value& item : v->items()) {
        if (item.is_string() && valid_mod_id(item.as_string())) {
            out.push_back(item.as_string());
        } else {
            issues.push_back({Severity::Warning, id,
                              "mod.json: an entry of '" + std::string(key) + "' is not a valid mod id; ignored"});
        }
    }
}

const char* const kKnownKeys[] = {"schema", "id",           "name",       "version",    "author",
                                  "description", "website", "game_id",    "preview",    "dependencies",
                                  "incompatible", "load_after", "options"};

}  // namespace

bool parse_manifest(const json::Value& value, Manifest& out, std::vector<Issue>& issues) {
    out = Manifest{};
    if (!value.is_object()) {
        issues.push_back({Severity::Error, {}, "mod.json: the top level is not an object"});
        return false;
    }
    if (const json::Value* s = value.find("schema")) {
        out.schema = static_cast<int>(s->as_number(kManifestSchema));
    }
    string_member(value, "id", out.id, issues, {});
    const std::string id = out.id;
    bool ok = true;
    if (!valid_mod_id(out.id)) {
        issues.push_back({Severity::Error, out.id,
                          "mod.json: 'id' must be 1-64 characters of a-z, 0-9, '.', '_' or '-' starting with a "
                          "letter or digit"});
        ok = false;
    }
    if (out.schema > kManifestSchema)
        issues.push_back({Severity::Warning, id,
                          "mod.json: schema " + std::to_string(out.schema) + " is newer than this loader (" +
                              std::to_string(kManifestSchema) + "); unknown fields are ignored"});
    string_member(value, "name", out.name, issues, id);
    if (out.name.empty()) out.name = out.id;
    if (const json::Value* v = value.find("version")) out.version = version_text(*v);
    if (out.version.empty()) out.version = "1.0";
    if (!Version::parse(out.version)) {
        issues.push_back({Severity::Error, id, "mod.json: 'version' \"" + out.version + "\" is not a version"});
        ok = false;
    }
    string_member(value, "author", out.author, issues, id);
    string_member(value, "description", out.description, issues, id);
    string_member(value, "website", out.website, issues, id);
    string_member(value, "game_id", out.game_id, issues, id);
    string_member(value, "preview", out.preview, issues, id);
    if (const json::Value* deps = value.find("dependencies"); deps != nullptr && !deps->is_null()) {
        if (!deps->is_array()) {
            issues.push_back({Severity::Error, id, "mod.json: 'dependencies' is not a list"});
            ok = false;
        } else {
            for (const json::Value& d : deps->items()) {
                Dependency dep;
                if (d.is_string()) {
                    // "id" or "id>=1.2"
                    const std::string& t = d.as_string();
                    const std::size_t ge = t.find(">=");
                    dep.id = t.substr(0, ge);
                    if (ge != std::string::npos) dep.min_version = t.substr(ge + 2);
                } else if (d.is_object()) {
                    if (const json::Value* i = d.find("id")) dep.id = i->as_string();
                    if (const json::Value* m = d.find("min_version")) dep.min_version = version_text(*m);
                    if (const json::Value* o = d.find("optional")) dep.optional = o->as_bool();
                }
                if (!valid_mod_id(dep.id) || (!dep.min_version.empty() && !Version::parse(dep.min_version))) {
                    issues.push_back({Severity::Error, id, "mod.json: a dependency is not \"id\", \"id>=version\" or "
                                                           "{\"id\", \"min_version\", \"optional\"}"});
                    ok = false;
                    continue;
                }
                if (dep.id == out.id) {
                    issues.push_back({Severity::Error, id, "mod.json: the mod depends on itself"});
                    ok = false;
                    continue;
                }
                out.dependencies.push_back(std::move(dep));
            }
        }
    }
    string_list(value, "incompatible", out.incompatible, issues, id);
    string_list(value, "load_after", out.load_after, issues, id);
    if (const json::Value* opts = value.find("options"); opts != nullptr && !opts->is_null()) {
        if (!opts->is_array()) {
            issues.push_back({Severity::Warning, id, "mod.json: 'options' is not a list; ignored"});
        } else {
            for (const json::Value& o : opts->items()) {
                ModOption opt;
                if (o.is_object()) {
                    string_member(o, "id", opt.id, issues, id);
                    string_member(o, "name", opt.name, issues, id);
                    string_member(o, "description", opt.description, issues, id);
                    string_member(o, "default", opt.default_choice, issues, id);
                    if (const json::Value* cs = o.find("choices"); cs != nullptr && cs->is_array()) {
                        for (const json::Value& c : cs->items()) {
                            OptionChoice ch;
                            if (!c.is_object()) continue;
                            string_member(c, "id", ch.id, issues, id);
                            string_member(c, "name", ch.name, issues, id);
                            string_member(c, "folder", ch.folder, issues, id);
                            if (ch.name.empty()) ch.name = ch.id;
                            const bool folder_ok = ch.folder.find("..") == std::string::npos &&
                                                   (ch.folder.empty() || (ch.folder[0] != '/' && ch.folder[0] != '\\' &&
                                                                          ch.folder.find(':') == std::string::npos));
                            if (!valid_mod_id(ch.id) || !folder_ok) {
                                issues.push_back({Severity::Warning, id,
                                                  "mod.json: option '" + opt.id +
                                                      "' has a choice with a bad id or a folder outside the mod; "
                                                      "ignored"});
                                continue;
                            }
                            opt.choices.push_back(std::move(ch));
                        }
                    }
                }
                if (!valid_mod_id(opt.id) || opt.choices.empty()) {
                    issues.push_back({Severity::Warning, id, "mod.json: an option without a valid id or choices; ignored"});
                    continue;
                }
                if (opt.name.empty()) opt.name = opt.id;
                if (!opt.default_choice.empty() &&
                    std::none_of(opt.choices.begin(), opt.choices.end(),
                                 [&](const OptionChoice& c) { return c.id == opt.default_choice; }))
                    opt.default_choice.clear();
                if (opt.default_choice.empty()) opt.default_choice = opt.choices.front().id;
                out.options.push_back(std::move(opt));
            }
        }
    }
    out.extra = json::Value::object();
    for (const auto& [key, v] : value.members()) {
        if (std::find(std::begin(kKnownKeys), std::end(kKnownKeys), key) == std::end(kKnownKeys)) out.extra.set(key, v);
    }
    return ok;
}

json::Value manifest_to_json(const Manifest& m) {
    json::Value v = json::Value::object();
    v.set("schema", json::Value::number(m.schema));
    v.set("id", json::Value::string(m.id));
    v.set("name", json::Value::string(m.name));
    v.set("version", json::Value::string(m.version));
    if (!m.author.empty()) v.set("author", json::Value::string(m.author));
    if (!m.description.empty()) v.set("description", json::Value::string(m.description));
    if (!m.website.empty()) v.set("website", json::Value::string(m.website));
    if (!m.game_id.empty()) v.set("game_id", json::Value::string(m.game_id));
    if (!m.preview.empty()) v.set("preview", json::Value::string(m.preview));
    if (!m.dependencies.empty()) {
        json::Value deps = json::Value::array();
        for (const Dependency& d : m.dependencies) {
            if (d.min_version.empty() && !d.optional) {
                deps.push(json::Value::string(d.id));
                continue;
            }
            json::Value o = json::Value::object();
            o.set("id", json::Value::string(d.id));
            if (!d.min_version.empty()) o.set("min_version", json::Value::string(d.min_version));
            if (d.optional) o.set("optional", json::Value::boolean(true));
            deps.push(std::move(o));
        }
        v.set("dependencies", std::move(deps));
    }
    const auto list = [&](const char* key, const std::vector<std::string>& items) {
        if (items.empty()) return;
        json::Value a = json::Value::array();
        for (const std::string& s : items) a.push(json::Value::string(s));
        v.set(key, std::move(a));
    };
    list("incompatible", m.incompatible);
    list("load_after", m.load_after);
    if (!m.options.empty()) {
        json::Value opts = json::Value::array();
        for (const ModOption& o : m.options) {
            json::Value jo = json::Value::object();
            jo.set("id", json::Value::string(o.id));
            jo.set("name", json::Value::string(o.name));
            if (!o.description.empty()) jo.set("description", json::Value::string(o.description));
            jo.set("default", json::Value::string(o.default_choice));
            json::Value cs = json::Value::array();
            for (const OptionChoice& c : o.choices) {
                json::Value jc = json::Value::object();
                jc.set("id", json::Value::string(c.id));
                jc.set("name", json::Value::string(c.name));
                jc.set("folder", json::Value::string(c.folder));
                cs.push(std::move(jc));
            }
            jo.set("choices", std::move(cs));
            opts.push(std::move(jo));
        }
        v.set("options", std::move(opts));
    }
    for (const auto& [key, extra] : m.extra.members()) v.set(key, extra);
    return v;
}

bool read_manifest(const fs::path& file, Manifest& out, std::vector<Issue>& issues) {
    std::string text;
    if (!read_text(file, text)) {
        issues.push_back({Severity::Error, {}, "cannot read " + utf8(file)});
        return false;
    }
    json::Value v;
    std::string error;
    if (!json::parse(text, v, error)) {
        issues.push_back({Severity::Error, {}, "mod.json is not valid JSON (" + error + ")"});
        return false;
    }
    return parse_manifest(v, out, issues);
}

bool write_manifest(const fs::path& file, const Manifest& manifest, std::string& error) {
    return write_atomically(file, json::write(manifest_to_json(manifest)), error);
}

// ---- discovery ---------------------------------------------------------------------------------------------------

ContentSummary summarize_content(const fs::path& root) {
    ContentSummary s;
    std::error_code ec;
    std::vector<std::pair<std::string, fs::path>> files;
    if (fs::is_directory(root / "files", ec)) list_files(root / "files", files);
    s.files = files.size();
    if (fs::is_directory(root / "afs", ec)) {
        for (fs::directory_iterator it(root / "afs", ec), end; !ec && it != end; it.increment(ec)) {
            if (!is_plain_dir(*it)) continue;
            std::error_code ec2;
            for (fs::directory_iterator e(it->path(), ec2), end2; !ec2 && e != end2; e.increment(ec2))
                if (is_plain_file(*e)) ++s.afs_entries;
        }
    }
    files.clear();
    if (fs::is_directory(root / "textures", ec)) list_files(root / "textures", files);
    for (const auto& [rel, path] : files) {
        if (is_texture_name(path.filename().string())) ++s.textures;
        else ++s.other_textures;
    }
    if (fs::is_regular_file(root / "sys" / "main.dol", ec))
        s.unsupported.push_back("sys/main.dol: a replacement game executable (code)");
    if (fs::is_directory(root / "codes", ec)) {
        files.clear();
        list_files(root / "codes", files);
        for (const auto& [rel, path] : files) {
            const std::string ext = lower(path.extension().string());
            const char* kind = ext == ".dol"                       ? "a replacement game executable (code)"
                               : ext == ".gct" || ext == ".ini"    ? "Gecko / Action Replay code patches"
                               : ext == ".xdelta" || ext == ".vcdiff" || ext == ".ups" || ext == ".bps" ||
                                       ext == ".ips" || ext == ".ppf"
                                   ? "a binary patch of a whole disc image"
                                   : "code-patch material (assembly or code lists)";
            if (s.unsupported.size() < 16) s.unsupported.push_back("codes/" + rel + ": " + kind);
        }
        if (files.size() > 16) s.unsupported.push_back("... and " + std::to_string(files.size() - 16) + " more in codes/");
    }
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!is_plain_file(*it)) continue;
        const std::string ext = lower(it->path().extension().string());
        if (ext == ".xdelta" || ext == ".vcdiff" || ext == ".ups" || ext == ".bps" || ext == ".ips" || ext == ".ppf")
            s.unsupported.push_back(utf8(it->path().filename()) + ": a binary patch of a whole disc image");
        else if (ext == ".gct" || ext == ".dol")
            s.unsupported.push_back(utf8(it->path().filename()) + ": game code");
    }
    return s;
}

std::vector<InstalledMod> discover(const fs::path& mods_dir, std::vector<Issue>& issues) {
    std::vector<InstalledMod> mods;
    std::error_code ec;
    if (!fs::is_directory(mods_dir, ec)) return mods;
    std::vector<fs::directory_entry> entries;
    for (fs::directory_iterator it(mods_dir, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(*it);
    if (ec) {
        issues.push_back({Severity::Error, {}, "cannot list " + utf8(mods_dir) + ": " + ec.message()});
        return mods;
    }
    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path() < b.path(); });
    std::set<std::string> ids;
    for (const fs::directory_entry& e : entries) {
        const std::string name = utf8(e.path().filename());
        if (name.empty() || name[0] == '.') continue;
        if (!is_plain_dir(e)) continue;
        InstalledMod mod;
        mod.folder = e.path();
        mod.folder_name = name;
        mod.content = summarize_content(e.path());
        const fs::path manifest_file = e.path() / std::string(kManifestName);
        if (fs::is_regular_file(manifest_file, ec)) {
            std::vector<Issue> mi;
            mod.usable = read_manifest(manifest_file, mod.manifest, mi);
            for (Issue& i : mi) {
                if (i.mod_id.empty()) i.mod_id = mod.manifest.id.empty() ? name : mod.manifest.id;
                mod.issues.push_back(std::move(i));
            }
        } else if (mod.content.files + mod.content.afs_entries + mod.content.textures > 0) {
            // A folder put here by hand in the documented layout: it works, named after its folder.
            mod.manifest.id = make_mod_id(name);
            mod.manifest.name = name;
            mod.manifest.version = "1.0";
            mod.usable = true;
            mod.issues.push_back({Severity::Info, mod.manifest.id,
                                  "no mod.json: named after its folder (the mod manager can add one)"});
        } else {
            mod.manifest.id = make_mod_id(name);
            mod.manifest.name = name;
            mod.issues.push_back({Severity::Error, mod.manifest.id,
                                  "folder '" + name + "' has no mod.json and no files/, afs/ or textures/ content: "
                                  "not a mod (install archives with the mod manager)"});
        }
        if (mod.usable && !ids.insert(mod.manifest.id).second) {
            mod.usable = false;
            mod.issues.push_back({Severity::Error, mod.manifest.id,
                                  "folder '" + name + "' repeats the id '" + mod.manifest.id +
                                      "' of another folder; only the first (in name order) is used"});
        }
        for (const std::string& u : mod.content.unsupported) {
            // A whole-disc-image patch cannot be applied; code can, when chosen (docs/mods/codemods/README.md).
            if (u.find("disc image") != std::string::npos)
                mod.issues.push_back({Severity::Warning, mod.manifest.id,
                                      u + " - a patch of a whole disc image is not applied (install the files it "
                                          "contains instead)"});
            else
                mod.issues.push_back({Severity::Info, mod.manifest.id,
                                      u + " - code: applied only when chosen under Code mods"});
        }
        mods.push_back(std::move(mod));
    }
    return mods;
}

// ---- state -------------------------------------------------------------------------------------------------------

bool load_state(const fs::path& file, State& out, std::string& error) {
    out = State{};
    error.clear();
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    std::string text;
    if (!read_text(file, text)) {
        error = "cannot read " + utf8(file);
        return false;
    }
    json::Value v;
    if (!json::parse(text, v, error)) {
        error = utf8(file.filename()) + " is not valid JSON (" + error + ")";
        return false;
    }
    if (!v.is_object()) {
        error = utf8(file.filename()) + ": the top level is not an object";
        return false;
    }
    if (const json::Value* s = v.find("schema")) out.schema = static_cast<int>(s->as_number(kStateSchema));
    if (out.schema > kStateSchema) {
        error = utf8(file.filename()) + " was written by a newer mod manager (schema " + std::to_string(out.schema) +
                ")";
        return false;
    }
    if (const json::Value* m = v.find("mods_enabled")) out.mods_enabled = m->as_bool(true);
    const json::Value* order = v.find("order");
    if (order != nullptr && !order->is_array()) {
        error = utf8(file.filename()) + ": 'order' is not a list";
        return false;
    }
    std::set<std::string> seen;
    if (order != nullptr) {
        for (const json::Value& e : order->items()) {
            StateEntry entry;
            if (!e.is_object()) continue;
            if (const json::Value* id = e.find("id")) entry.id = id->as_string();
            if (!valid_mod_id(entry.id) || !seen.insert(entry.id).second) continue;
            if (const json::Value* en = e.find("enabled")) entry.enabled = en->as_bool();
            if (const json::Value* opts = e.find("options"); opts != nullptr && opts->is_object())
                for (const auto& [k, ch] : opts->members())
                    if (ch.is_string()) entry.options[k] = ch.as_string();
            if (const json::Value* codes = e.find("codes"); codes != nullptr && codes->is_object())
                for (const auto& [k, on] : codes->members())
                    if (on.is_bool()) entry.codes[k] = on.as_bool();
            out.order.push_back(std::move(entry));
        }
    }
    return true;
}

bool save_state(const fs::path& file, const State& state, std::string& error) {
    json::Value v = json::Value::object();
    v.set("schema", json::Value::number(kStateSchema));
    v.set("mods_enabled", json::Value::boolean(state.mods_enabled));
    json::Value order = json::Value::array();
    for (const StateEntry& e : state.order) {
        json::Value o = json::Value::object();
        o.set("id", json::Value::string(e.id));
        o.set("enabled", json::Value::boolean(e.enabled));
        if (!e.options.empty()) {
            json::Value opts = json::Value::object();
            for (const auto& [k, c] : e.options) opts.set(k, json::Value::string(c));
            o.set("options", std::move(opts));
        }
        if (!e.codes.empty()) {
            json::Value codes = json::Value::object();
            for (const auto& [k, on] : e.codes) codes.set(k, json::Value::boolean(on));
            o.set("codes", std::move(codes));
        }
        order.push(std::move(o));
    }
    v.set("order", std::move(order));
    return write_atomically(file, json::write(v), error);
}

bool reconcile(State& state, const std::vector<InstalledMod>& mods) {
    bool changed = false;
    std::map<std::string, const InstalledMod*> usable;
    for (const InstalledMod& m : mods)
        if (m.usable) usable[m.manifest.id] = &m;
    const std::size_t before = state.order.size();
    state.order.erase(std::remove_if(state.order.begin(), state.order.end(),
                                     [&](const StateEntry& e) { return usable.count(e.id) == 0; }),
                      state.order.end());
    changed |= state.order.size() != before;
    for (const InstalledMod& m : mods) {
        if (!m.usable) continue;
        if (std::none_of(state.order.begin(), state.order.end(),
                         [&](const StateEntry& e) { return e.id == m.manifest.id; })) {
            state.order.push_back(StateEntry{m.manifest.id, false, {}});
            changed = true;
        }
    }
    for (StateEntry& e : state.order) {
        const InstalledMod* m = usable[e.id];
        std::map<std::string, std::string> fixed;
        for (const ModOption& o : m->manifest.options) {
            auto it = e.options.find(o.id);
            const bool valid = it != e.options.end() &&
                               std::any_of(o.choices.begin(), o.choices.end(),
                                           [&](const OptionChoice& c) { return c.id == it->second; });
            fixed[o.id] = valid ? it->second : o.default_choice;
        }
        if (fixed != e.options) {
            e.options = std::move(fixed);
            changed = true;
        }
        // Code choices for items the mod no longer offers are dropped.
        if (!e.codes.empty()) {
            std::set<std::string> offered;
            for (const CodeItem& c : list_code_items(*m)) offered.insert(c.id);
            for (auto it = e.codes.begin(); it != e.codes.end();) {
                if (!offered.count(it->first)) {
                    it = e.codes.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
    }
    return changed;
}

// ---- resolution --------------------------------------------------------------------------------------------------

Resolution resolve(const std::vector<InstalledMod>& mods, const State& state) {
    Resolution r;
    if (!state.mods_enabled) return r;
    std::map<std::string, const InstalledMod*> installed;  // usable, by id
    for (const InstalledMod& m : mods)
        if (m.usable) installed[m.manifest.id] = &m;
    // Enabled, in the user's priority (highest first).
    std::vector<const InstalledMod*> wanted;
    std::set<std::string> enabled;
    for (const StateEntry& e : state.order) {
        auto it = installed.find(e.id);
        if (!e.enabled || it == installed.end()) continue;
        wanted.push_back(it->second);
        enabled.insert(e.id);
    }
    std::set<std::string> skipped;
    const auto skip = [&](const InstalledMod* m, std::string why) {
        if (skipped.insert(m->manifest.id).second)
            r.issues.push_back({Severity::Error, m->manifest.id, m->manifest.name + " is not loaded: " + why});
    };
    std::vector<const InstalledMod*> order;  // lowest priority first
    for (int round = 0; round <= static_cast<int>(wanted.size()) + 1; ++round) {
        // Dependencies, to a fixed point (a skipped mod can break the mods that need it).
        for (bool again = true; again;) {
            again = false;
            for (const InstalledMod* m : wanted) {
                if (skipped.count(m->manifest.id)) continue;
                for (const Dependency& d : m->manifest.dependencies) {
                    if (d.optional) continue;
                    auto it = installed.find(d.id);
                    if (it == installed.end()) {
                        skip(m, "it requires '" + d.id + "', which is not installed");
                    } else if (!enabled.count(d.id)) {
                        skip(m, "it requires " + it->second->manifest.name + " (" + d.id + "), which is disabled");
                    } else if (skipped.count(d.id)) {
                        skip(m, "it requires " + it->second->manifest.name + " (" + d.id + "), which cannot load");
                    } else if (!d.min_version.empty()) {
                        const auto have = Version::parse(it->second->manifest.version);
                        const auto need = Version::parse(d.min_version);
                        if (!have || !need || have->compare(*need) < 0)
                            skip(m, "it requires " + it->second->manifest.name + " " + d.min_version +
                                        " or newer (installed: " + it->second->manifest.version + ")");
                    }
                    if (skipped.count(m->manifest.id)) {
                        again = true;
                        break;
                    }
                }
            }
        }
        // Incompatibilities: the higher-priority mod wins.
        std::vector<const InstalledMod*> kept;
        bool changed = false;
        for (const InstalledMod* m : wanted) {
            if (skipped.count(m->manifest.id)) continue;
            const InstalledMod* clash = nullptr;
            for (const InstalledMod* k : kept) {
                const auto declares = [](const InstalledMod* a, const InstalledMod* b) {
                    return std::find(a->manifest.incompatible.begin(), a->manifest.incompatible.end(),
                                     b->manifest.id) != a->manifest.incompatible.end();
                };
                if (declares(m, k) || declares(k, m)) {
                    clash = k;
                    break;
                }
            }
            if (clash != nullptr) {
                skip(m, "it is incompatible with " + clash->manifest.name + " (" + clash->manifest.id +
                            "), which has a higher priority");
                changed = true;
                continue;
            }
            kept.push_back(m);
        }
        if (changed) continue;
        // Order: the user's (lowest first), moved only where a dependency or load_after needs it (stable Kahn).
        std::vector<const InstalledMod*> base(kept.rbegin(), kept.rend());
        std::map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < base.size(); ++i) index[base[i]->manifest.id] = i;
        std::vector<std::vector<std::size_t>> after(base.size());  // edges: before -> after
        std::vector<std::size_t> indegree(base.size(), 0);
        for (std::size_t i = 0; i < base.size(); ++i) {
            std::set<std::size_t> befores;
            for (const Dependency& d : base[i]->manifest.dependencies)
                if (auto it = index.find(d.id); it != index.end()) befores.insert(it->second);
            for (const std::string& a : base[i]->manifest.load_after)
                if (auto it = index.find(a); it != index.end()) befores.insert(it->second);
            for (std::size_t b : befores) {
                after[b].push_back(i);
                ++indegree[i];
            }
        }
        std::vector<std::size_t> out;
        std::vector<bool> done(base.size(), false);
        for (std::size_t n = 0; n < base.size(); ++n) {
            std::size_t pick = base.size();
            for (std::size_t i = 0; i < base.size(); ++i)
                if (!done[i] && indegree[i] == 0) {
                    pick = i;
                    break;
                }
            if (pick == base.size()) break;
            done[pick] = true;
            out.push_back(pick);
            for (std::size_t a : after[pick]) --indegree[a];
        }
        if (out.size() < base.size()) {
            // The mods left are on a cycle or behind one; skip the ones that reach themselves.
            bool any = false;
            for (std::size_t s = 0; s < base.size(); ++s) {
                if (done[s]) continue;
                std::vector<std::size_t> stack(after[s].begin(), after[s].end());
                std::vector<bool> seen(base.size(), false);
                bool cycle = false;
                while (!stack.empty() && !cycle) {
                    const std::size_t x = stack.back();
                    stack.pop_back();
                    if (x == s) cycle = true;
                    if (seen[x]) continue;
                    seen[x] = true;
                    for (std::size_t y : after[x]) stack.push_back(y);
                }
                if (cycle) {
                    skip(base[s], "its dependencies or load_after entries form a cycle");
                    any = true;
                }
            }
            if (any) continue;
        }
        for (std::size_t k = 0; k < out.size(); ++k) {
            order.push_back(base[out[k]]);
            if (out[k] != k && k < base.size())
                r.issues.push_back({Severity::Info, base[out[k]]->manifest.id,
                                    base[out[k]]->manifest.name +
                                        " loads in a different place than listed, after the mods it depends on or "
                                        "must follow"});
        }
        break;
    }
    // Info lines for moves are only kept once per mod.
    r.load_order = std::move(order);
    r.skipped = std::move(skipped);
    return r;
}

// ---- game index --------------------------------------------------------------------------------------------------

bool GameIndex::build(const fs::path& files_root, std::string& error) {
    paths.clear();
    by_name.clear();
    std::error_code ec;
    if (!fs::is_directory(files_root, ec)) {
        error = "not a directory: " + utf8(files_root);
        return false;
    }
    std::vector<std::pair<std::string, fs::path>> files;
    list_files(files_root, files);
    for (const auto& [rel, path] : files) {
        const std::string l = lower(rel);
        paths.emplace(l, rel);
        const std::size_t slash = l.rfind('/');
        by_name[slash == std::string::npos ? l : l.substr(slash + 1)].push_back(rel);
    }
    return true;
}

const std::string* GameIndex::find(std::string_view path) const {
    std::string l = lower(path);
    std::replace(l.begin(), l.end(), '\\', '/');
    auto it = paths.find(l);
    return it == paths.end() ? nullptr : &it->second;
}

// ---- plan --------------------------------------------------------------------------------------------------------

Plan build_plan(const Resolution& resolution, const State& state, const GameIndex& game, const fs::path& files_root) {
    Plan plan;
    std::map<std::string, FileOverlay> files;                       // lower-case path -> overlay
    std::map<std::string, std::map<std::string, AfsEntryOverlay>> afs;  // lower archive -> lower entry -> overlay
    std::map<std::string, std::string> afs_spelling;                // lower archive -> disc spelling
    std::map<std::string, std::set<std::string>> afs_names;         // lower archive -> lower entry names (checked)
    std::map<std::string, bool> afs_read;
    std::vector<fs::path> textures;  // lowest first, reversed at the end
    std::vector<std::string> texture_mods;
    for (const InstalledMod* mod : resolution.load_order) {
        const std::string& id = mod->manifest.id;
        bool contributes = false;
        // The content roots: the mod, then each option's chosen folder.
        std::vector<fs::path> roots{mod->folder};
        auto entry = std::find_if(state.order.begin(), state.order.end(), [&](const StateEntry& e) { return e.id == id; });
        for (const ModOption& o : mod->manifest.options) {
            std::string choice = o.default_choice;
            if (entry != state.order.end())
                if (auto it = entry->options.find(o.id); it != entry->options.end()) choice = it->second;
            for (const OptionChoice& c : o.choices)
                if (c.id == choice && !c.folder.empty()) roots.push_back(mod->folder / from_utf8(c.folder));
        }
        for (const fs::path& root : roots) {
            std::error_code ec;
            if (root != mod->folder && !fs::is_directory(root, ec)) {
                plan.issues.push_back({Severity::Warning, id, "the chosen option's folder " + utf8(root) + " is missing"});
                continue;
            }
            // files/
            std::vector<std::pair<std::string, fs::path>> list;
            if (fs::is_directory(root / "files", ec)) list_files(root / "files", list);
            for (const auto& [rel, host] : list) {
                if (!disc_path_ok(rel)) {
                    plan.issues.push_back({Severity::Warning, id,
                                           "files/" + rel + ": the disc's names are printable ASCII only; skipped"});
                    continue;
                }
                if (std::string why; !check_file_format(host, rel, why)) {
                    plan.issues.push_back({Severity::Error, id, "files/" + rel + " " + why + "; skipped (the disc's "
                                                                "file is used)"});
                    continue;
                }
                const std::string key = lower(rel);
                const std::string* disc = game.find(rel);
                auto [it, inserted] = files.try_emplace(key);
                FileOverlay& f = it->second;
                if (!inserted && f.mod_id != id) f.overridden.insert(f.overridden.begin(), f.mod_id);
                f.game_path = disc != nullptr ? *disc : rel;
                f.host = host;
                f.mod_id = id;
                f.added = disc == nullptr;
                contributes = true;
            }
            // afs/<archive>/<entry>
            if (fs::is_directory(root / "afs", ec)) {
                std::vector<fs::directory_entry> dirs;
                for (fs::directory_iterator it(root / "afs", ec), end; !ec && it != end; it.increment(ec))
                    if (is_plain_dir(*it)) dirs.push_back(*it);
                std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) { return a.path() < b.path(); });
                for (const fs::directory_entry& d : dirs) {
                    const std::string archive = utf8(d.path().filename()) + ".afs";
                    const std::string akey = lower(archive);
                    const std::string* disc = game.find(archive);
                    if (!game.empty() && disc == nullptr) {
                        plan.issues.push_back({Severity::Warning, id,
                                               "afs/" + utf8(d.path().filename()) + ": the disc has no " + archive +
                                                   "; skipped"});
                        continue;
                    }
                    afs_spelling[akey] = disc != nullptr ? *disc : archive;
                    if (!game.empty() && !afs_read[akey]) {
                        afs_read[akey] = true;
                        AfsArchive a;
                        std::string err;
                        if (read_afs(files_root / from_utf8(*disc), a, err)) {
                            for (const AfsEntry& e : a.entries)
                                if (!e.name.empty()) afs_names[akey].insert(lower(e.name));
                        } else {
                            plan.issues.push_back({Severity::Warning, id, archive + ": " + err});
                        }
                    }
                    std::vector<std::pair<std::string, fs::path>> entries;
                    for (fs::directory_iterator e(d.path(), ec), end; !ec && e != end; e.increment(ec))
                        if (is_plain_file(*e)) entries.emplace_back(utf8(e->path().filename()), e->path());
                    std::sort(entries.begin(), entries.end());
                    for (const auto& [name, host] : entries) {
                        const std::string ekey = lower(name);
                        if (std::string why; !check_file_format(host, name, why)) {
                            plan.issues.push_back({Severity::Error, id, "afs/" + utf8(d.path().filename()) + "/" + name +
                                                                            " " + why + "; skipped"});
                            continue;
                        }
                        if (afs_read[akey] && !afs_names[akey].count(ekey)) {
                            plan.issues.push_back({Severity::Warning, id,
                                                   archive + " has no entry named " + name + "; skipped"});
                            continue;
                        }
                        auto [it, inserted] = afs[akey].try_emplace(ekey);
                        AfsEntryOverlay& o = it->second;
                        if (!inserted && o.mod_id != id) o.overridden.insert(o.overridden.begin(), o.mod_id);
                        o.entry_name = name;
                        o.host = host;
                        o.mod_id = id;
                        contributes = true;
                    }
                }
            }
            // textures/
            if (fs::is_directory(root / "textures", ec)) {
                if (summarize_content(root).textures > 0) {
                    textures.push_back(root / "textures");
                    texture_mods.push_back(id);
                    contributes = true;
                }
            }
        }
        if (contributes) plan.active.push_back(id);
    }
    for (auto& [key, f] : files) plan.files.push_back(std::move(f));
    for (auto& [akey, entries] : afs) {
        AfsOverlay a;
        a.archive_path = afs_spelling[akey];
        for (auto& [ekey, e] : entries) a.entries.push_back(std::move(e));
        if (files.count(akey))
            plan.issues.push_back({Severity::Warning, {},
                                   a.archive_path + " is replaced whole by " + files[akey].mod_id +
                                       " and has entries replaced by other mods: the entries apply to the replacement"});
        plan.afs.push_back(std::move(a));
    }
    plan.texture_dirs.assign(textures.rbegin(), textures.rend());
    plan.texture_mods.assign(texture_mods.rbegin(), texture_mods.rend());
    for (const FileOverlay& f : plan.files)
        if (!f.overridden.empty()) {
            std::string list;
            for (const std::string& o : f.overridden) list += (list.empty() ? "" : ", ") + o;
            plan.issues.push_back({Severity::Info, f.mod_id, f.game_path + " also in " + list + " (lower priority)"});
        }
    return plan;
}

// ---- code mods ---------------------------------------------------------------------------------------------------

std::vector<CodeItem> list_code_items(const InstalledMod& mod) {
    std::vector<CodeItem> out;
    std::vector<std::pair<std::string, fs::path>> files;
    std::error_code ec;
    if (fs::is_directory(mod.folder / "codes", ec)) list_files(mod.folder / "codes", files);
    fs::path dol;
    std::vector<fs::path> mcm, ini, text;
    for (const auto& [rel, path] : files) {
        const std::string ext = lower(path.extension().string());
        if (ext == ".dol") {
            if (dol.empty()) dol = path;
            continue;
        }
        if (ext == ".ini") {
            ini.push_back(path);
            continue;
        }
        if (ext != ".txt") continue;
        std::string t;
        read_text(path, t);
        if (t.find("-==-") != std::string::npos) mcm.push_back(path);
        else text.push_back(path);
    }
    if (!dol.empty()) {
        out.push_back({"dol", "Executable changes: " + utf8(dol.filename()) + " (every change it makes to its game's "
                                                                           "executable)",
                       "dol-diff:" + utf8(dol), dol});
    } else if (!mcm.empty()) {
        out.push_back({"mcm", "Melee Code Manager libraries (" + std::to_string(mcm.size()) + " files)",
                       "mcm:" + utf8(mcm.front().parent_path()), mcm.front().parent_path()});
    }
    // A download often ships the same Dolphin game ini twice (a "Dolphin" and a "Nintendont" folder): an ini whose
    // text equals an earlier one is offered once. Names are unique across the mod: a code name a different ini
    // already offered gets that ini's path in its id, so choosing one never switches on another file's code.
    std::set<std::string> ini_texts, seen;
    for (const fs::path& f : ini) {
        std::string t;
        read_text(f, t);
        if (!ini_texts.insert(t).second) continue;
        std::string where = utf8(f.filename());  // the file's path below codes/
        for (const auto& [rel, path] : files)
            if (path == f) where = rel;
        std::replace(where.begin(), where.end(), '\\', '/');
        std::istringstream lines(t);
        std::string line, section, pending;
        std::set<std::string> seen_here;
        while (std::getline(lines, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line.front() == '[') {
                section = lower(line);
                continue;
            }
            if (section != "[gecko]" && section != "[actionreplay]") continue;
            if (line.size() > 1 && line.front() == '$') {
                pending = line.substr(1);
                if (const std::size_t brace = pending.find(" ["); brace != std::string::npos) pending.resize(brace);
                continue;
            }
            // A name counts once a code line follows it (headings like "=== Preference Codes ===" have none).
            const bool code_line = line.size() == 17 && line[8] == ' ' &&
                                   std::all_of(line.begin(), line.begin() + 8, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }) &&
                                   std::all_of(line.begin() + 9, line.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
            if (code_line && !pending.empty()) {
                if (seen_here.insert(pending).second) {
                    const bool clash = !seen.insert(pending).second;
                    out.push_back({"gecko:" + pending + (clash ? " [" + where + "]" : ""),
                                   pending + " (" + (clash ? where : utf8(f.filename())) + ")", "gecko-ini:" + utf8(f), f});
                }
                pending.clear();
            }
        }
    }
    for (const fs::path& f : text) {
        std::string t;
        read_text(f, t);
        // Only files that hold code lines: 8 hex digits, a space, 8 hex digits.
        bool code = false;
        std::istringstream lines(t);
        std::string line;
        while (!code && std::getline(lines, line)) {
            std::size_t run = 0, groups = 0;
            for (const char c : line) {
                if (std::isxdigit(static_cast<unsigned char>(c))) {
                    if (++run == 8) ++groups;
                } else {
                    if (run > 8) groups = 0;
                    run = 0;
                }
            }
            code = groups >= 2 && line.size() < 64;
        }
        if (code) out.push_back({"text:" + utf8(f.stem()), utf8(f.filename()), "gecko-text:" + utf8(f), f});
    }
    return out;
}

std::string code_selection_signature(const std::vector<InstalledMod>& mods, const State& state) {
    if (!state.mods_enabled) return {};
    std::string text;
    for (auto it = state.order.rbegin(); it != state.order.rend(); ++it) {  // load order: lowest priority first
        if (!it->enabled) continue;
        const InstalledMod* mod = nullptr;
        for (const InstalledMod& m : mods)
            if (m.usable && m.manifest.id == it->id) mod = &m;
        if (mod == nullptr) continue;
        for (const CodeItem& c : list_code_items(*mod)) {
            auto chosen = it->codes.find(c.id);
            if (chosen == it->codes.end() || !chosen->second) continue;
            std::error_code ec;
            const bool folder = fs::is_directory(c.file, ec);
            std::string digest;
            if (folder) {
                std::vector<std::pair<std::string, fs::path>> files;
                list_files(c.file, files);
                for (const auto& [rel, p] : files) digest += rel + "=" + md5_file(p) + ";";
            } else {
                digest = md5_file(c.file);
            }
            text += it->id + "|" + c.id + "|" + digest + "\n";
        }
    }
    if (text.empty()) return {};
    // A short stable name: FNV-1a 64 of the description (the description itself is in the plan file).
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001B3ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// ---- profiles ----------------------------------------------------------------------------------------------------

bool valid_profile_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 40 || name.front() == ' ' || name.back() == ' ' || name.front() == '.') return false;
    for (const char c : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '(' ||
                        c == ')' || c == '.';
        if (!ok || static_cast<unsigned char>(c) >= 0x80) return false;
    }
    return true;
}

namespace {
fs::path profile_file(const fs::path& mods_dir, std::string_view name) {
    return mods_dir / ".profiles" / from_utf8(std::string(name) + ".json");
}
}  // namespace

std::vector<std::string> list_profiles(const fs::path& mods_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(mods_dir / ".profiles", ec), end; !ec && it != end; it.increment(ec)) {
        if (!is_plain_file(*it) || lower(it->path().extension().string()) != ".json") continue;
        const std::string name = utf8(it->path().stem());
        if (valid_profile_name(name)) out.push_back(name);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return out;
}

bool save_profile(const fs::path& mods_dir, std::string_view name, const State& state, std::string& error) {
    if (!valid_profile_name(name)) {
        error = "a profile name is 1-40 letters, digits, spaces, '-', '_', '(', ')' or '.'";
        return false;
    }
    return save_state(profile_file(mods_dir, name), state, error);
}

bool load_profile(const fs::path& mods_dir, std::string_view name, State& out, std::string& error) {
    const fs::path file = profile_file(mods_dir, name);
    std::error_code ec;
    if (!valid_profile_name(name) || !fs::is_regular_file(file, ec)) {
        error = "no profile named '" + std::string(name) + "'";
        return false;
    }
    return load_state(file, out, error);
}

bool delete_profile(const fs::path& mods_dir, std::string_view name, std::string& error) {
    std::error_code ec;
    if (!valid_profile_name(name) || !fs::remove(profile_file(mods_dir, name), ec)) {
        error = "no profile named '" + std::string(name) + "'";
        return false;
    }
    return true;
}

// ---- texture packs ------------------------------------------------------------------------------------------------

std::vector<TextureConflict> texture_conflicts(const Plan& plan) {
    std::map<std::string, std::size_t> owner;  // lower-case texture name -> index of the winning pack
    std::map<std::pair<std::size_t, std::size_t>, TextureConflict> pairs;
    for (std::size_t i = 0; i < plan.texture_dirs.size(); ++i) {
        std::vector<std::pair<std::string, fs::path>> files;
        list_files(plan.texture_dirs[i], files);
        std::set<std::string> mine;
        for (const auto& [rel, path] : files) {
            const std::string name = lower(utf8(path.filename()));
            if (!is_texture_name(name) || !mine.insert(name).second) continue;
            auto [it, inserted] = owner.try_emplace(name, i);
            if (inserted) continue;
            TextureConflict& c = pairs[{it->second, i}];
            c.winner = it->second < plan.texture_mods.size() ? plan.texture_mods[it->second] : utf8(plan.texture_dirs[it->second]);
            c.loser = i < plan.texture_mods.size() ? plan.texture_mods[i] : utf8(plan.texture_dirs[i]);
            ++c.count;
            if (c.examples.size() < 3) c.examples.push_back(utf8(path.filename()));
        }
    }
    std::vector<TextureConflict> out;
    for (auto& [k, c] : pairs) out.push_back(std::move(c));
    return out;
}

// ---- runtime -----------------------------------------------------------------------------------------------------

RuntimeSetup prepare_runtime(const fs::path& mods_dir, const fs::path& files_root) {
    RuntimeSetup setup;
    try {
        std::error_code ec;
        if (!fs::is_directory(mods_dir, ec)) return setup;
        State state;
        std::string error;
        if (!load_state(mods_dir / std::string(kStateName), state, error)) {
            setup.issues.push_back({Severity::Error, {}, error + " - running without mods"});
            return setup;
        }
        if (!state.mods_enabled) return setup;
        if (std::none_of(state.order.begin(), state.order.end(), [](const StateEntry& e) { return e.enabled; }))
            return setup;
        std::vector<Issue> issues;
        std::vector<InstalledMod> mods = discover(mods_dir, issues);
        reconcile(state, mods);  // in memory only: the game never rewrites the manager's file
        for (const InstalledMod& m : mods) {
            const bool on = std::any_of(state.order.begin(), state.order.end(),
                                        [&](const StateEntry& e) { return e.enabled && e.id == m.manifest.id; });
            if (on || !m.usable)
                for (const Issue& i : m.issues)
                    if (i.severity != Severity::Info) issues.push_back(i);
        }
        GameIndex game;
        if (!game.build(files_root, error)) {
            setup.issues.push_back({Severity::Error, {}, error + " - running without mods"});
            return setup;
        }
        const Resolution resolution = resolve(mods, state);
        issues.insert(issues.end(), resolution.issues.begin(), resolution.issues.end());
        Plan plan = build_plan(resolution, state, game, files_root);
        issues.insert(issues.end(), plan.issues.begin(), plan.issues.end());
        std::map<std::string, fs::path> files;
        for (const FileOverlay& f : plan.files) files[lower(f.game_path)] = f.host;
        for (const AfsOverlay& a : plan.afs) {
            const std::string key = lower(a.archive_path);
            const fs::path original =
                files.count(key) ? files[key] : files_root / from_utf8(a.archive_path);
            fs::path rebuilt;
            if (!materialize_afs(original, a, mods_dir / std::string(kCacheName), rebuilt, error)) {
                issues.push_back({Severity::Error, {}, a.archive_path + ": " + error + "; its entry replacements are not loaded"});
                continue;
            }
            files[key] = rebuilt;
        }
        std::map<std::string, std::string> spelling;
        for (const FileOverlay& f : plan.files) spelling[lower(f.game_path)] = f.game_path;
        for (const AfsOverlay& a : plan.afs) spelling[lower(a.archive_path)] = a.archive_path;
        for (const auto& [key, host] : files) setup.files.emplace_back(spelling[key], host);
        setup.texture_dirs = plan.texture_dirs;
        for (const std::string& id : plan.active) {
            for (const InstalledMod& m : mods)
                if (m.manifest.id == id)
                    setup.active_mods.push_back(m.manifest.name + " " + m.manifest.version + " (" + id + ")");
        }
        // Chosen code: what the code-mod pipeline built for exactly this selection (its report names the signature).
        setup.code_signature = code_selection_signature(mods, state);
        if (!setup.code_signature.empty()) {
            const fs::path built = mods_dir / ".variants" / (setup.code_signature + ".json");
            std::string text, why;
            json::Value report;
            if (!read_text(built, text)) {
                issues.push_back({Severity::Warning, {}, "code mods are chosen but not built for this selection "
                                  "(Code mods... > Build in the manager); no code runs"});
            } else if (!json::parse(text, report, why) || !report.is_object()) {
                issues.push_back({Severity::Error, {}, utf8(built) + ": " + why + "; no code runs"});
            } else if (const json::Value* list = report.find("engine_codes"); list && list->is_array()) {
                for (const json::Value& c : list->items()) {
                    RuntimeCode code;
                    if (const json::Value* n = c.find("name")) code.name = n->as_string();
                    if (const json::Value* n = c.find("needs_variant")) code.needs_variant = n->as_bool(false);
                    bool ok = !code.name.empty();
                    if (const json::Value* lines = c.find("lines"); ok && lines && lines->is_array()) {
                        for (const json::Value& l : lines->items()) {
                            const std::string& s = l.as_string();
                            unsigned a = 0, b = 0;
                            char tail = 0;
                            if (s.size() != 17 || std::sscanf(s.c_str(), "%8x %8x%c", &a, &b, &tail) != 2) {
                                ok = false;
                                break;
                            }
                            code.lines.emplace_back(a, b);
                        }
                    } else {
                        ok = false;
                    }
                    if (!ok || code.lines.empty())
                        issues.push_back({Severity::Error, {}, "code '" + code.name + "' in " + utf8(built.filename()) +
                                          " is damaged; it does not run"});
                    else
                        setup.codes.push_back(std::move(code));
                }
            }
        }
        setup.issues = std::move(issues);
        setup.active = !setup.files.empty() || !setup.texture_dirs.empty() || !setup.codes.empty();
    } catch (const std::exception& e) {
        setup = RuntimeSetup{};
        setup.issues.push_back({Severity::Error, {}, std::string("mod loader failed (") + e.what() +
                                                         ") - running without mods"});
    }
    return setup;
}

}  // namespace shadow::mods
