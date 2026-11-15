// What a mod changes, in the player's terms (shadow/mods/insight.h).
#include "shadow/mods/insight.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <system_error>

namespace shadow::mods {

namespace fs = std::filesystem;

namespace {

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void put_utf8(std::string& s, std::uint32_t cp) {
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

bool read_fnt(const fs::path& file, std::vector<FntEntry>& out, std::string& error) {
    out.clear();
    std::ifstream in(long_path(file), std::ios::binary);
    if (!in) {
        error = "cannot open " + utf8(file);
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto* d = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() < 4) {
        error = utf8(file.filename()) + " is too short for a subtitle table";
        return false;
    }
    const std::uint32_t count = le32(d);
    if (count > 100000u || 4ull + 20ull * count > bytes.size()) {
        error = utf8(file.filename()) + ": the entry count does not fit the file";
        return false;
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const unsigned char* r = d + 4 + 20 * i;
        FntEntry e;
        const std::uint32_t text_at = le32(r);
        e.key = le32(r + 4);
        e.type = le32(r + 8);
        e.active_time = le32(r + 12);
        e.audio_id = static_cast<std::int32_t>(le32(r + 16));
        // UTF-16LE up to the first NUL (the last string may be followed by padding or junk).
        for (std::size_t at = text_at; at + 1 < bytes.size(); at += 2) {
            std::uint32_t u = static_cast<std::uint32_t>(d[at] | d[at + 1] << 8);
            if (u == 0) break;
            if (u >= 0xD800 && u <= 0xDBFF && at + 3 < bytes.size()) {
                const std::uint32_t lo = static_cast<std::uint32_t>(d[at + 2] | d[at + 3] << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    at += 2;
                }
            }
            put_utf8(e.text, u);
            if (e.text.size() > 4096) break;
        }
        out.push_back(std::move(e));
    }
    return true;
}

bool VoiceIndex::build(const fs::path& files_root, std::string& error) {
    entry_index.clear();
    lines.clear();
    AfsArchive a;
    if (!read_afs(files_root / "PRS_VOICE_E.afs", a, error)) return false;
    for (std::size_t i = 0; i < a.entries.size(); ++i)
        if (!a.entries[i].name.empty()) entry_index[lower(a.entries[i].name)] = static_cast<std::uint32_t>(i);
    std::error_code ec;
    const fs::path fonts = files_root / "fonts";
    for (fs::recursive_directory_iterator it(fonts, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = lower(utf8(it->path().filename()));
        if (!name.ends_with("_en.fnt")) continue;
        std::vector<FntEntry> entries;
        std::string e;
        if (!read_fnt(it->path(), entries, e)) continue;
        const std::string table = "fonts/" + utf8(fs::relative(it->path(), fonts, ec));
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const FntEntry& f = entries[i];
            if (f.audio_id < 0 || static_cast<std::size_t>(f.audio_id) >= a.entries.size()) continue;
            // Chained parts (key+1, key+2... without audio) are shown while this voice plays: one line.
            std::string text = f.text;
            for (std::size_t j = i + 1; j < entries.size() && entries[j].key == f.key + (j - i) &&
                                        entries[j].audio_id == -1 && entries[j].active_time != 0;
                 ++j)
                if (!entries[j].text.empty()) text += (text.empty() ? "" : " ") + entries[j].text;
            lines[static_cast<std::uint32_t>(f.audio_id)].push_back({table, f.key, text});
        }
    }
    return true;
}

const std::vector<VoiceLine>* VoiceIndex::find(std::string_view entry_name) const {
    const auto it = entry_index.find(lower(entry_name));
    if (it == entry_index.end()) return nullptr;
    const auto l = lines.find(it->second);
    return l == lines.end() ? nullptr : &l->second;
}

}  // namespace shadow::mods

// ---- region ------------------------------------------------------------------------------------------------------

namespace shadow::mods {

namespace {

// MD5 (RFC 1321), streaming.
struct Md5 {
    std::uint32_t a = 0x67452301u, b = 0xefcdab89u, c = 0x98badcfeu, d = 0x10325476u;
    std::uint64_t length = 0;
    unsigned char buffer[64] = {};
    std::size_t used = 0;

    static std::uint32_t rol(std::uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
    void block(const unsigned char* p) {
        static const std::uint32_t k[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) m[i] = le32(p + 4 * i);
        std::uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) {
                f = (B & C) | (~B & D);
                g = i;
            } else if (i < 32) {
                f = (D & B) | (~D & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                f = C ^ (B | ~D);
                g = (7 * i) % 16;
            }
            const std::uint32_t t = D;
            D = C;
            C = B;
            B = B + rol(A + f + k[i] + m[g], r[i]);
            A = t;
        }
        a += A;
        b += B;
        c += C;
        d += D;
    }
    void update(const unsigned char* p, std::size_t n) {
        length += n;
        while (n > 0) {
            const std::size_t take = std::min<std::size_t>(64 - used, n);
            std::copy(p, p + take, buffer + used);
            used += take;
            p += take;
            n -= take;
            if (used == 64) {
                block(buffer);
                used = 0;
            }
        }
    }
    std::string hex() {
        const std::uint64_t bits = length * 8;
        const unsigned char one = 0x80, zero = 0;
        update(&one, 1);
        while (used != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (8 * i));
        update(len, 8);
        std::string out;
        char buf[3];
        for (const std::uint32_t w : {a, b, c, d})
            for (int i = 0; i < 4; ++i) {
                std::snprintf(buf, sizeof(buf), "%02x", (w >> (8 * i)) & 0xFFu);
                out += buf;
            }
        return out;
    }
};

}  // namespace

std::string md5_file(const fs::path& file) {
    std::ifstream in(long_path(file), std::ios::binary);
    if (!in) return {};
    Md5 md5;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        md5.update(reinterpret_cast<const unsigned char*>(buf.data()), static_cast<std::size_t>(in.gcount()));
    }
    return md5.hex();
}

bool RegionData::load(const fs::path& json_file, std::string& error) {
    paths.clear();
    std::ifstream in(long_path(json_file), std::ios::binary);
    if (!in) {
        error = "cannot open " + utf8(json_file);
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    json::Value v;
    if (!json::parse(text, v, error)) return false;
    const json::Value* p = v.find("paths");
    if (p == nullptr || !p->is_object()) {
        error = "no 'paths' object";
        return false;
    }
    const auto str = [](const json::Value& o, const char* key) {
        const json::Value* x = o.find(key);
        return x != nullptr && x->is_string() ? x->as_string() : std::string();
    };
    for (const auto& [path, e] : p->members()) {
        RegionFile f{str(e, "pal_md5"), str(e, "usa_md5"), str(e, "kind"),  str(e, "risk"),
                     str(e, "summary"), str(e, "advice"),  str(e, "stage_title")};
        paths[lower(path)] = std::move(f);
    }
    return true;
}

std::vector<Issue> region_notes(const InstalledMod& mod, const RegionData& region, const fs::path& files_root) {
    std::vector<Issue> notes;
    const std::string& id = mod.manifest.id;
    // Every files/ root: the base and each option's choice folders.
    std::vector<std::pair<std::string, fs::path>> roots{{"", mod.folder / "files"}};
    for (const ModOption& o : mod.manifest.options)
        for (const OptionChoice& c : o.choices)
            if (!c.folder.empty())
                roots.emplace_back("option '" + o.name + "' = " + c.name + ": ",
                                   mod.folder / from_utf8(c.folder) / "files");
    std::size_t gameplay = 0;
    bool any_font = false, only_en_fonts = true;
    for (const auto& [label, root] : roots) {
        std::error_code ec;
        const fs::path base = long_path(root);
        if (!fs::is_directory(base, ec)) continue;
        for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string rel = utf8(fs::relative(it->path(), base, ec));
            const std::string key = lower(rel);
            if (key.rfind("fonts/", 0) == 0) {
                any_font = true;
                if (lower(utf8(it->path().stem())).find("_en") == std::string::npos) only_en_fonts = false;
            }
            // Message tables that lack messages of the PAL table: the PC wording applies per message, so those
            // keep their own words (decided by the key set, not by where the file came from).
            if ((key.rfind("fonts/save/save_", 0) == 0 || key.rfind("fonts/advertise/advertise_", 0) == 0) &&
                key.ends_with(".fnt")) {
                std::vector<FntEntry> mine, stock;
                std::string e;
                if (read_fnt(it->path(), mine, e) && read_fnt(files_root / from_utf8(rel), stock, e)) {
                    std::set<std::uint32_t> have;
                    for (const FntEntry& f : mine) have.insert(f.key);
                    std::size_t missing = 0;
                    for (const FntEntry& f : stock) missing += have.count(f.key) ? 0u : 1u;
                    if (missing)
                        notes.push_back({Severity::Info, id,
                                         label + rel + " lacks " + std::to_string(missing) +
                                             " message(s) of the PAL table (an NTSC-U-style table); those keep their "
                                             "own wording"});
                }
            }
            const auto r = region.paths.find(key);
            if (r == region.paths.end()) continue;
            const RegionFile& f = r->second;
            if (f.risk == "none") continue;
            const std::string md5 = md5_file(it->path());
            if (md5 == f.pal_md5) continue;
            const bool layout = f.kind == "stage-layout";
            const Severity sev = f.risk == "gameplay" ? Severity::Warning : Severity::Info;
            if (layout && f.risk == "gameplay") ++gameplay;
            const std::string where = f.stage_title.empty() ? "" : " (" + f.stage_title + ")";
            if (md5 == f.usa_md5)
                notes.push_back({sev, id, label + rel + where + " is the NTSC-U disc's version. PAL's differs: " +
                                              (f.summary.ends_with(".") ? f.summary.substr(0, f.summary.size() - 1) : f.summary) + ". " + f.advice});
            else if (layout)
                notes.push_back({sev, id, label + rel + where + " is an edited copy of a layout PAL revised (" +
                                              f.summary + "); check that the mod keeps PAL's changes."});
        }
    }
    if (any_font && only_en_fonts)
        notes.push_back({Severity::Info, id,
                         "changes English text only: no effect while another game language is selected"});
    if (gameplay && mod.manifest.game_id == "GUPE8P") notes.push_back({Severity::Info, id, "built for the NTSC-U disc (GUPE8P)"});
    return notes;
}

}  // namespace shadow::mods
