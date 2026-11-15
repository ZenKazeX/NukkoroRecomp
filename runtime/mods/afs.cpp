// AFS archives: reading the table of contents and rebuilding with replaced entries (shadow/mods/mods.h).
//
// The layout (CRI's AFS, as on this disc; all little-endian):
//   0x00  "AFS\0"
//   0x04  u32 entry count N
//   0x08  N x {u32 offset, u32 size}
//   8+8N  {u32 name table offset, u32 name table size}   (0/0 when the archive has no names)
//   ...   zero padding up to the first entry's offset
//   data  each entry at an aligned offset (2048 on this disc)
//   names N x 0x30 bytes: 32-byte NUL-padded name, 6 x u16 date/time, u32 the entry's size
// Checked against PRS_VOICE_E.afs (4,480 entries, names at the end, every offset a multiple of 2048).
#include "shadow/mods/mods.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <system_error>

namespace shadow::mods {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kNameRecord = 0x30;

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
void put_le32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint64_t align_up(std::uint64_t v, std::uint64_t a) { return (v + a - 1u) / a * a; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ---- structural checks of the loose data files -------------------------------------------------------------------
// Paste inside the anonymous namespace of afs.cpp (after align_up). Needs le32() and lower() from the same file.
// Every rule here stops a read outside the file or a table that is certainly cut off; each held on every shipped file
// of the PAL, NTSC-U and NTSC-J trees and on every file of the real mods tried (docs/mods/map/FORMAT_CHECKS.md).

using Bytes = std::vector<std::uint8_t>;

std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | static_cast<std::uint32_t>(p[3]);
}

bool bad(std::string& why, std::string text) {
    why = std::move(text);
    return false;
}

std::string num(std::uint64_t v) { return std::to_string(v); }

bool is_ascii_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// Decimal or 0x-hex integer as the metrics reader's "%i" takes it (no leading zeros, so no octal).
bool parse_int(std::string_view t, long long& out) {
    std::size_t i = 0;
    bool neg = false;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) neg = t[i++] == '-';
    int base = 10;
    if (i + 2 < t.size() && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i >= t.size()) return false;
    if (base == 10 && t[i] == '0' && i + 1 < t.size()) return false;
    long long v = 0;
    for (; i < t.size(); ++i) {
        const int c = t[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : (base == 16 && c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (base == 16 && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0 || d >= base) return false;
        v = v * base + d;
        if (v > (1ll << 40)) return false;
    }
    out = neg ? -v : v;
    return true;
}

// The "1B" container (ScoreData.bin, AudioData*.bin, stgNNNN_hint.bin): 0x20-byte big-endian header, data, relocation
// stream. The loader rebases the words the stream names, so a stream that leaves the data writes outside the buffer.
bool check_container(const Bytes& d, std::uint64_t& data_len, std::uint64_t& reloc_len, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 0x20) return bad(why, "is " + num(n) + " bytes, shorter than the 32-byte container header");
    data_len = be32(&d[4]);
    reloc_len = be32(&d[8]);
    if (data_len % 4 || reloc_len % 4) return bad(why, "has a data/relocation length that is not a multiple of 4 (damaged)");
    if (0x20 + data_len + reloc_len > n) return bad(why, "has tables that overrun the file (truncated or damaged)");
    if (d[0x17] != 0x42) return bad(why, "has no big-endian marker (\"1B\") in its header (damaged)");
    const std::uint8_t* s = d.data() + 0x20 + data_len;
    std::uint64_t pos = 0;
    for (std::uint64_t i = 0; i < reloc_len;) {
        const std::uint8_t c = s[i++];
        std::uint64_t delta = 0;
        switch (c >> 6) {
            case 0: continue;
            case 1: delta = c & 0x3Fu; break;
            case 2:
                if (i + 1 > reloc_len) return bad(why, "has a relocation stream cut inside a code (truncated or damaged)");
                delta = (static_cast<std::uint64_t>(c & 0x3Fu) << 8) | s[i];
                i += 1;
                break;
            default:
                if (i + 3 > reloc_len) return bad(why, "has a relocation stream cut inside a code (truncated or damaged)");
                delta = (static_cast<std::uint64_t>(c & 0x3Fu) << 24) | static_cast<std::uint64_t>(s[i]) << 16 |
                        static_cast<std::uint64_t>(s[i + 1]) << 8 | s[i + 2];
                i += 3;
                break;
        }
        pos += delta;
        if (pos * 4 + 4 > data_len) return bad(why, "has a relocation that points past its data (damaged)");
    }
    return true;
}

bool check_bin(const Bytes& d, const std::string& base, std::string& why) {
    const std::uint64_t n = d.size();
    if (base == "setid.bin") {
        if (n < 8) return bad(why, "is " + num(n) + " bytes, shorter than its 8-byte header");
        const std::uint64_t want = 8 + 12ull * le32(&d[4]);
        if (want != n)
            return bad(why, "is " + num(n) + " bytes but its header says " + num(le32(&d[4])) + " records (" + num(want) +
                                " bytes): truncated or damaged");
        return true;
    }
    if (base.size() > 10 && base.compare(base.size() - 10, 10, "_light.bin") == 0) {
        if (n < 0x340) return bad(why, "is " + num(n) + " bytes, the game reads 832");
        for (std::size_t i = 0; i < 208; ++i) {
            const std::uint32_t w = le32(&d[4 * i]);
            if ((w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu) != 0) return bad(why, "holds a NaN in its light table");
        }
        return true;
    }
    const bool hint = base.size() > 9 && base.compare(base.size() - 9, 9, "_hint.bin") == 0;
    const bool audio = base.rfind("audiodata", 0) == 0;
    if (!hint && !audio && base.rfind("scoredat", 0) != 0) return true;  // not a table of the game
    std::uint64_t data = 0, reloc = 0;
    if (!check_container(d, data, reloc, why)) return false;
    if (hint) {
        if (reloc != 0 || data % 12 || data < 12) return bad(why, "is not a hint table (its data is not whole 12-byte records)");
        const std::uint8_t* last = d.data() + 0x20 + data - 12;
        if (be32(last) != 0xFFFFFFFFu || be32(last + 4) != 0xFFFFFFFFu)
            return bad(why, "has no terminator record in its hint table (truncated or damaged)");
    } else if (audio) {
        if (data < 8) return bad(why, "is too short for its sound table");
        const std::uint64_t rows = be32(&d[0x24]);
        const std::uint64_t rows_end = 8 + 0x18 * rows;
        if (rows_end > data) return bad(why, "says it has " + num(rows) + " sound rows but its data is " + num(data) + " bytes");
        for (std::uint64_t i = 0; i < rows; ++i) {
            const std::uint64_t off = be32(&d[0x28 + 0x18 * i]);
            if (off < rows_end || off >= data) return bad(why, "has a sound row whose name lies outside the file's string area");
            if (std::find(d.begin() + 0x20 + off, d.begin() + 0x20 + data, std::uint8_t{0}) == d.begin() + 0x20 + data)
                return bad(why, "has a sound row name without a terminator");
        }
    }
    return true;
}

bool check_dat(const Bytes& d, const std::string& base, std::string& why) {
    const std::uint64_t n = d.size();
    if (base.size() > 8 && base.compare(base.size() - 8, 8, "_cam.dat") == 0) {
        if (n < 0x18) return bad(why, "is " + num(n) + " bytes, shorter than the 24-byte camera header");
        if (le32(&d[0]) != 3 || le32(&d[4]) != 0x0131CD93u) return bad(why, "is not a stage camera file (header damaged)");
        const std::uint64_t want = 0x18 + 220ull * le32(&d[0x14]);
        if (want != n)
            return bad(why, "is " + num(n) + " bytes but its header says " + num(le32(&d[0x14])) + " camera records (" +
                                num(want) + " bytes): truncated or damaged");
        return true;
    }
    // Object layouts stgNNNN_cmn / nrm / hrd / ds1: the loader copies them into fixed arrays without a bounds check.
    if (n < 12) return bad(why, "is " + num(n) + " bytes, shorter than the 12-byte layout header");
    if (std::memcmp(d.data(), "sky2", 4) != 0) return bad(why, "is not an object layout (no \"sky2\" signature)");
    const std::uint64_t count = le32(&d[4]), misc = le32(&d[8]);
    const std::uint64_t want = 12 + 0x2Cull * count + misc;
    if (want != n)
        return bad(why, "is " + num(n) + " bytes but its header says " + num(count) + " entries + " + num(misc) + " bytes (" +
                            num(want) + "): truncated or damaged");
    if (misc % 4) return bad(why, "has a misc size that is not a multiple of 4");
    if (count > 0x800) return bad(why, "has " + num(count) + " objects, the game has room for 2048");
    if (misc > 0x10000) return bad(why, "has " + num(misc) + " bytes of object parameters, the game has room for 65536");
    std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint32_t len = le32(&d[12 + 0x2C * i + 0x24]);
        if (len % 4) return bad(why, "has an object whose parameter size is not a multiple of 4");
        total += len;
    }
    if (total != misc) return bad(why, "has object parameter sizes that do not add up to its header (damaged)");
    return true;
}

bool check_fnt(const Bytes& d, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 4) return bad(why, "is " + num(n) + " bytes, shorter than its count word");
    const std::uint64_t count = le32(&d[0]);
    const std::uint64_t table_end = 4 + 20 * count;
    if (count == 0 || table_end > n)
        return bad(why, "says " + num(count) + " messages but is " + num(n) + " bytes (truncated or damaged)");
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t off = le32(&d[4 + 20 * i]);
        if ((off & 1) || off < table_end || off + 2 > n) return bad(why, "has message " + num(i) + " pointing outside its text area");
        std::uint64_t j = off;
        while (j + 1 < n && (d[j] | d[j + 1])) j += 2;
        if (j + 1 >= n) return bad(why, "has a message that is not terminated before the end of the file (truncated)");
    }
    return true;
}

bool check_met(const Bytes& d, std::string& why) {
    if (d.empty()) return bad(why, "is empty");
    if (std::find(d.begin(), d.end(), std::uint8_t{0}) != d.end()) return bad(why, "contains a NUL byte (a text file)");
    std::string text;  // carriage returns dropped: the Reloaded fonts ship CRLF
    for (const std::uint8_t c : d)
        if (c != '\r') text.push_back(static_cast<char>(c));
    if (text.back() != '\n') return bad(why, "does not end with a line break (its last glyph line is cut)");
    std::vector<std::string_view> lines;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t nl = text.find('\n', at);
        lines.emplace_back(text.data() + at, nl - at);
        at = nl + 1;
    }
    if (lines.size() < 4) return bad(why, "has " + num(lines.size()) + " lines, a glyph file needs 4");
    if (lines[0] != "METRICS1") return bad(why, "is not a METRICS1 glyph file");
    long long v = 0;
    std::string_view third = lines[2];
    while (!third.empty() && is_ascii_space(static_cast<unsigned char>(third.front()))) third.remove_prefix(1);
    while (!third.empty() && is_ascii_space(static_cast<unsigned char>(third.back()))) third.remove_suffix(1);
    if (!parse_int(third, v)) return bad(why, "has a third line that is not a number");
    std::size_t glyphs = 0;
    for (std::size_t k = 3; k < lines.size(); ++k) {
        std::string_view line = lines[k].substr(0, lines[k].find('#'));
        std::vector<std::string_view> tok;
        for (std::size_t i = 0; i < line.size();) {
            while (i < line.size() && is_ascii_space(static_cast<unsigned char>(line[i]))) ++i;
            const std::size_t b = i;
            while (i < line.size() && !is_ascii_space(static_cast<unsigned char>(line[i]))) ++i;
            if (i > b) tok.push_back(line.substr(b, i - b));
        }
        if (lines[k].find_first_not_of(" \t\v\f") == std::string_view::npos) continue;  // blank line
        long long f[5] = {};
        bool ok = tok.size() == 5;
        for (std::size_t i = 0; ok && i < 5; ++i) ok = parse_int(tok[i], f[i]);
        if (!ok) return bad(why, "has glyph line " + num(k + 1) + " that is not \"code x0 y0 x1 y1\"");
        if (f[0] <= 0 || f[0] > 0xFFFF) return bad(why, "has a glyph code outside 1..65535 on line " + num(k + 1));
        if (f[1] < 0 || f[2] < 0 || f[3] < f[1] || f[4] < f[2]) return bad(why, "has a glyph rectangle that is inverted or negative on line " + num(k + 1));
        ++glyphs;
    }
    if (glyphs == 0) return bad(why, "has no glyph lines");
    return true;
}

bool check_gncp(const Bytes& d, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 12) return bad(why, "is " + num(n) + " bytes, too short for a UI project");
    const std::uint8_t* image = d.data();
    std::uint64_t m = n;
    const std::uint8_t* tex = nullptr;
    std::uint64_t tex_len = 0;
    const bool wrapped = std::memcmp(d.data(), "CPAF", 4) == 0 || std::memcmp(d.data(), "FAPC", 4) == 0;
    if (wrapped) {
        const bool big = d[0] == 'C';
        auto rd = [&](std::uint64_t o) { return big ? be32(&d[o]) : le32(&d[o]); };
        const std::uint64_t l1 = rd(4);
        if (8 + l1 + 4 > n) return bad(why, "has a first part that runs past the file (truncated or damaged)");
        const std::uint64_t l2 = rd(8 + l1);
        if (12 + l1 + l2 > n) return bad(why, "has a texture part that runs past the file (truncated or damaged)");
        image = d.data() + 8;
        m = l1;
        tex = d.data() + 12 + l1;
        tex_len = l2;
    }
    if (m < 0x20 || std::memcmp(image, "NGIF", 4) != 0) return bad(why, "has no \"NGIF\" UI header (damaged)");
    const std::uint64_t data_off = be32(image + 0xC), total = be32(image + 0x10), reloc_off = be32(image + 0x14);
    if (data_off < 0x18 || data_off + total > m || total < 0x20) return bad(why, "has a data area outside the file (truncated or damaged)");
    if (reloc_off < data_off || reloc_off + 0x10 > m) return bad(why, "has a pointer table outside the file (truncated or damaged)");
    const std::uint64_t count = be32(image + reloc_off + 8);
    if (reloc_off + 0x10 + 4 * count > m) return bad(why, "has a pointer table that overruns the file (truncated or damaged)");
    const std::uint8_t* data = image + data_off;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t o = be32(image + reloc_off + 0x10 + 4 * i) & ~3ull;
        if (o + 4 > total) return bad(why, "has a pointer entry outside its data (damaged)");
        if (be32(data + o) > total) return bad(why, "has a pointer that leads outside its data (damaged)");
    }
    if (std::memcmp(data, "nCPJ", 4) != 0 || le32(data + 4) != total - 8) return bad(why, "has a damaged project chunk");
    if (static_cast<std::uint64_t>(be32(data + 8)) + 0x10 > total) return bad(why, "has a project root outside its data (damaged)");
    if (tex_len != 0 && (tex_len < 12 || le32(tex) != 0x16 || static_cast<std::uint64_t>(le32(tex + 4)) + 12 > tex_len))
        return bad(why, "has an embedded texture dictionary that is not whole (truncated or damaged)");
    return true;
}

bool check_mlt(const Bytes& d, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 0x70 || std::memcmp(d.data(), "gcaxMLT ", 8) != 0) return bad(why, "is not a sound bank (no \"gcaxMLT\" signature)");
    if (be32(&d[8]) != 0 || be32(&d[12]) != n)
        return bad(why, "is " + num(n) + " bytes but its header says " + num(be32(&d[12])) + " (truncated or damaged)");
    if (std::memcmp(&d[0x10], "gcaxMLTM", 8) != 0 || std::memcmp(&d[0x40], "gcaxMPB ", 8) != 0 ||
        std::memcmp(&d[0x50], "gcaxMPBW", 8) != 0)
        return bad(why, "has a damaged bank header");
    const std::uint64_t ip = 0x60ull + be32(&d[0x5C]);
    if (ip + 16 > n || std::memcmp(&d[ip], "gcaxMPBP", 8) != 0) return bad(why, "has wave data that does not end at its program table");
    if (ip + 16 + be32(&d[ip + 12]) > n) return bad(why, "has a program table that runs past the file (truncated or damaged)");
    return true;
}

bool check_scr(const Bytes& d, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 0x188) return bad(why, "is " + num(n) + " bytes, shorter than the 392-byte event script header");
    const std::uint64_t want = 0x188 + 0x80ull * le32(&d[4]);
    if (want != n)
        return bad(why, "is " + num(n) + " bytes but its header says " + num(le32(&d[4])) + " records (" + num(want) +
                            " bytes): truncated or damaged");
    return true;
}

bool check_fog(const Bytes& d, std::string& why) {
    if (d.size() < 960) return bad(why, "is " + num(d.size()) + " bytes, the game reads 960 (24 fog areas of 40)");
    bool first_filled = false;
    for (std::size_t i = 0; i < 40; ++i) first_filled = first_filled || d[i] != 0;
    for (std::size_t e = 0; e < 24; ++e) {
        for (const std::size_t off : {16u, 20u, 28u, 32u}) {
            const std::uint32_t w = le32(&d[40 * e + off]);
            if ((w & 0x7F800000u) == 0x7F800000u) return bad(why, "has a non-finite number in fog area " + num(e));
        }
    }
    if (!first_filled) return bad(why, "has an empty first fog area (damaged: every shipped file fills it)");
    return true;
}

bool check_srd(const Bytes& d, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 0x20 || std::memcmp(d.data(), "SSRD", 4) != 0) return bad(why, "is not a staff roll table (no \"SSRD\" signature)");
    const std::uint64_t ents = le32(&d[0x10]), tab = le32(&d[0x14]), strips = le32(&d[0x18]), soff = le32(&d[0x1C]);
    if (tab + 12 * ents > n) return bad(why, "has an entry table that overruns the file (truncated or damaged)");
    if (soff + 8 * strips > n) return bad(why, "has a strip table that overruns the file (truncated or damaged)");
    for (std::uint64_t i = 0; i < ents; ++i)
        if (static_cast<std::uint64_t>(le32(&d[tab + 12 * i + 8])) + 1 > n) return bad(why, "has an entry pointing outside the file");
    return true;
}

// nukkoro.inf / nukkoro2.inf: Shift-JIS text; bytes only. A cut file loses its last section / key.
bool check_inf(const Bytes& d, const std::string& base, std::string& why) {
    if (d.empty()) return bad(why, "is empty");
    if (std::find(d.begin(), d.end(), std::uint8_t{0}) != d.end()) return bad(why, "contains a NUL byte (a text file)");
    const std::string text(d.begin(), d.end());
    bool sections = false, last_section = false, last_key = false;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string_view line(text.data() + at, nl - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        at = nl + 1;
        if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
            bool ok = true;
            for (const char c : line.substr(1, line.size() - 2)) ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
            if (ok) {
                sections = true;
                last_section = last_section || line == "[EVENT0910]";
            }
        } else if (line.rfind("AREA", 0) == 0) {
            std::string_view rest = line.substr(4);
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
            last_key = last_key || (!rest.empty() && rest.front() == ':');
        }
    }
    if (!sections) return bad(why, "has no [section] headers");
    if (base == "nukkoro2.inf" && !last_section) return bad(why, "is cut off: its last section [EVENT0910] is missing");
    if (base == "nukkoro.inf" && !last_key) return bad(why, "is cut off: its last setting (AREA) is missing");
    return true;
}

// Movies (.sfd, and the .flo files Reloaded ships): an MPEG program stream, walked packet by packet to its end. The
// disc's movies start with a pack header and end with the program end code, padded with 0xFF to the sector; movies
// muxed by CRI's later sfdmux (the community's re-edited intros) start with a private stream 2 packet
// ("SofdecStream2") and end at the last packet's last byte, with no end code and no padding. Both are whole when every
// packet lies inside the file and the walk ends exactly at its end (or at an end code followed only by padding); a
// cut download ends inside a packet.
bool check_movie(std::istream& in, std::uint64_t size, std::string& why) {
    if (size < 16) return bad(why, "is too small to be a movie (" + num(size) + " bytes)");
    Bytes buf;
    std::uint64_t buf_at = 0;  // file offset of buf[0]
    const auto fetch = [&](std::uint64_t at, std::size_t n) -> const std::uint8_t* {
        if (at + n > size) return nullptr;
        if (at < buf_at || at + n > buf_at + buf.size()) {
            buf.resize(static_cast<std::size_t>(std::min<std::uint64_t>(size - at, 1u << 20)));
            in.clear();
            in.seekg(static_cast<std::streamoff>(at));
            if (!in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))) return nullptr;
            buf_at = at;
        }
        return buf.data() + (at - buf_at);
    };
    std::uint64_t at = 0, packets = 0;
    bool packed = false;  // starts with a pack header: the disc's layout, which always ends with the end code
    while (at < size) {
        const std::uint8_t* p = fetch(at, std::min<std::uint64_t>(size - at, 16));
        if (p == nullptr) return bad(why, "cannot be read");
        if (size - at < 4 || p[0] != 0 || p[1] != 0 || p[2] != 1 || p[3] < 0xB9) {
            if (packets == 0) return bad(why, "does not start with an MPEG packet (not a movie, or damaged)");
            return bad(why, "has no MPEG packet at byte " + num(at) + " (damaged movie)");
        }
        std::uint64_t length;
        if (p[3] == 0xB9) {  // program end: only padding may follow
            for (std::uint64_t i = at + 4; i < size;) {
                const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(size - i, 1u << 20));
                const std::uint8_t* q = fetch(i, n);
                if (q == nullptr) return bad(why, "cannot be read");
                for (std::size_t k = 0; k < n; ++k)
                    if (q[k] != 0xFF && q[k] != 0x00) return bad(why, "has data after its MPEG end code (damaged movie)");
                i += n;
            }
            return true;
        }
        if (p[3] == 0xBA) {  // pack header: MPEG-2 (14 bytes + stuffing) or MPEG-1 (12 bytes)
            if (size - at < 14) return bad(why, "ends inside a pack header (truncated movie)");
            length = (p[4] & 0xC0) == 0x40 ? 14u + (p[13] & 7u) : 12u;
        } else {
            if (size - at < 6) return bad(why, "ends inside a packet header (truncated movie)");
            length = 6u + (static_cast<std::uint64_t>(p[4]) << 8 | p[5]);
        }
        if (at + length > size)
            return bad(why, "ends inside a packet (" + num(size - at) + " of " + num(length) + " bytes at byte " + num(at) +
                                "; truncated movie)");
        if (packets == 0) packed = p[3] == 0xBA;
        at += length;
        ++packets;
    }
    // A stream in the disc's layout cut at a pack boundary is caught by its missing end code.
    if (packed) return bad(why, "does not end with the MPEG end code (truncated movie)");
    return true;
}

bool check_small_kinds(const std::string& ext, const std::string& base, const Bytes& d, std::string& why) {
    if (ext == ".tp") {
        const std::uint64_t want = (base == "mc_bunner_e.tp" || base == "mc_bunner_j.tp") ? 6144
                                 : (base == "mc_icon01.tp" || base == "mc_icon02.tp" || base == "mc_icon03.tp") ? 2048 : 0;
        if (want != 0 && d.size() < want) return bad(why, "is " + num(d.size()) + " bytes, the memory card picture is " + num(want));
        return true;
    }
    if (ext == ".bnr") {
        const std::uint64_t want = d.size() >= 4 && std::memcmp(d.data(), "BNR1", 4) == 0 ? 0x1960
                                   : d.size() >= 4 && std::memcmp(d.data(), "BNR2", 4) == 0 ? 0x1FA0 : 0;
        if (want == 0) return bad(why, "is not a disc banner (no \"BNR1\"/\"BNR2\" signature)");
        if (d.size() < want) return bad(why, "is " + num(d.size()) + " bytes, a banner of this kind is " + num(want));
        return true;
    }
    return true;
}

// True when the kind is one this checker covers (the .one/.txd/.dff/.afs/.adx kinds have their own code below).
bool is_data_kind(const std::string& ext) {
    for (const char* k : {".dat", ".inf", ".fnt", ".met", ".gncp", ".mlt", ".bin", ".scr", ".fog", ".bnr", ".sfd", ".flo", ".tp", ".srd"})
        if (ext == k) return true;
    return false;
}

bool check_data_file(const fs::path& file, const std::string& ext, std::string_view game_path, std::uint64_t size, std::string& why) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return bad(why, "cannot be read");
    if (ext == ".sfd" || ext == ".flo") return check_movie(in, size, why);
    if (size > (64ull << 20)) return bad(why, "is far larger than any file of its kind (" + num(size) + " bytes)");
    Bytes d(static_cast<std::size_t>(size));
    if (size != 0 && !in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(size))) return bad(why, "cannot be read");
    std::string base = std::string(game_path);
    if (const std::size_t slash = base.find_last_of("/\\"); slash != std::string::npos) base.erase(0, slash + 1);
    base = lower(base);
    if (ext == ".dat") return check_dat(d, base, why);
    if (ext == ".bin") return check_bin(d, base, why);
    if (ext == ".fnt") return check_fnt(d, why);
    if (ext == ".met") return check_met(d, why);
    if (ext == ".gncp") return check_gncp(d, why);
    if (ext == ".mlt") return check_mlt(d, why);
    if (ext == ".scr") return check_scr(d, why);
    if (ext == ".fog") return check_fog(d, why);
    if (ext == ".srd") return check_srd(d, why);
    if (ext == ".inf") return check_inf(d, base, why);
    return check_small_kinds(ext, base, d, why);
}

// FNV-1a 64 over a byte string.
void fnv(std::uint64_t& h, std::string_view s) {
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001B3ull;
    }
    h ^= 0xFF;
    h *= 0x100000001B3ull;
}

std::string stamp(const fs::path& p) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    const auto time = fs::last_write_time(p, ec).time_since_epoch().count();
    return utf8(p) + "|" + std::to_string(size) + "|" + std::to_string(static_cast<long long>(time));
}

bool copy_range(std::ifstream& in, std::uint64_t offset, std::uint64_t size, std::ofstream& out,
                std::vector<char>& buffer) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    while (size > 0) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(size, buffer.size()));
        if (!in.read(buffer.data(), static_cast<std::streamsize>(n))) return false;
        out.write(buffer.data(), static_cast<std::streamsize>(n));
        size -= n;
    }
    return static_cast<bool>(out);
}

void pad_to(std::ofstream& out, std::uint64_t& at, std::uint64_t target) {
    static const char zeros[2048] = {};
    while (at < target) {
        const std::uint64_t n = std::min<std::uint64_t>(target - at, sizeof(zeros));
        out.write(zeros, static_cast<std::streamsize>(n));
        at += n;
    }
}

}  // namespace

bool read_afs(const fs::path& file, AfsArchive& out, std::string& error) {
    out = AfsArchive{};
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open " + utf8(file);
        return false;
    }
    std::error_code ec;
    const std::uint64_t file_size = fs::file_size(file, ec);
    std::uint8_t head[8];
    if (!in.read(reinterpret_cast<char*>(head), 8) || std::memcmp(head, "AFS\0", 4) != 0) {
        error = utf8(file.filename()) + " is not an AFS archive";
        return false;
    }
    const std::uint32_t count = le32(head + 4);
    if (count == 0 || count > 1000000u || 16ull + 8ull * count > file_size) {
        error = utf8(file.filename()) + ": implausible entry count";
        return false;
    }
    std::vector<std::uint8_t> toc(8u * count + 8u);
    if (!in.read(reinterpret_cast<char*>(toc.data()), static_cast<std::streamsize>(toc.size()))) {
        error = utf8(file.filename()) + ": truncated table of contents";
        return false;
    }
    out.entries.resize(count);
    std::uint64_t align = 2048;
    for (std::uint32_t i = 0; i < count; ++i) {
        out.entries[i].offset = le32(toc.data() + 8u * i);
        out.entries[i].size = le32(toc.data() + 8u * i + 4u);
        if (out.entries[i].offset + out.entries[i].size > file_size) {
            error = utf8(file.filename()) + ": entry " + std::to_string(i) + " lies outside the file";
            return false;
        }
        while (align > 1 && out.entries[i].offset % align != 0) align /= 2;
    }
    out.alignment = align;
    out.name_table_offset = le32(toc.data() + 8u * count);
    out.name_table_size = le32(toc.data() + 8u * count + 4u);
    if (out.name_table_offset != 0 && out.name_table_size >= kNameRecord * count &&
        out.name_table_offset + out.name_table_size <= file_size) {
        out.name_records.resize(static_cast<std::size_t>(kNameRecord * count));
        in.clear();
        in.seekg(static_cast<std::streamoff>(out.name_table_offset));
        if (!in.read(reinterpret_cast<char*>(out.name_records.data()),
                     static_cast<std::streamsize>(out.name_records.size()))) {
            error = utf8(file.filename()) + ": truncated name table";
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const char* name = reinterpret_cast<const char*>(out.name_records.data() + kNameRecord * i);
            out.entries[i].name.assign(name, strnlen(name, 32));
        }
    } else {
        out.name_table_offset = out.name_table_size = 0;
    }
    return true;
}

bool materialize_afs(const fs::path& original, const AfsOverlay& overlay, const fs::path& cache_dir, fs::path& out,
                     std::string& error) {
    AfsArchive a;
    if (!read_afs(original, a, error)) return false;
    if (a.name_records.empty()) {
        error = utf8(original.filename()) + " has no entry names, so entries cannot be replaced by name";
        return false;
    }
    std::map<std::string, const AfsEntryOverlay*> by_name;
    for (const AfsEntryOverlay& e : overlay.entries) by_name[lower(e.entry_name)] = &e;
    // Which entry takes which file (every entry of a repeated name takes the replacement).
    std::vector<const AfsEntryOverlay*> replace(a.entries.size(), nullptr);
    std::vector<std::uint64_t> sizes(a.entries.size());
    std::uint64_t digest = 0xCBF29CE484222325ull;
    fnv(digest, "afs-v1");
    fnv(digest, stamp(original));
    for (std::size_t i = 0; i < a.entries.size(); ++i) {
        sizes[i] = a.entries[i].size;
        auto it = by_name.find(lower(a.entries[i].name));
        if (it == by_name.end()) continue;
        std::error_code ec;
        const std::uint64_t size = fs::file_size(it->second->host, ec);
        if (ec) {
            error = "cannot read " + utf8(it->second->host);
            return false;
        }
        if (size > 0xFFFFFFFFull) {
            error = utf8(it->second->host) + " is larger than an AFS entry can be";
            return false;
        }
        replace[i] = it->second;
        sizes[i] = size;
        fnv(digest, std::to_string(i));
        fnv(digest, stamp(it->second->host));
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(digest));
    const std::string stem = utf8(original.stem());
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    out = cache_dir / from_utf8(stem + "-" + hex + ".afs");
    // Layout: the header area as long as the original's, then every entry aligned as the original.
    const std::uint64_t count = a.entries.size();
    std::uint64_t first = a.entries.front().offset;
    for (const AfsEntry& e : a.entries) first = std::min(first, e.offset);
    first = std::max<std::uint64_t>(first, align_up(16 + 8 * count, a.alignment));
    std::vector<std::uint64_t> offsets(count);
    std::uint64_t at = first;
    for (std::size_t i = 0; i < count; ++i) {
        offsets[i] = at;
        at = align_up(at + sizes[i], a.alignment);
    }
    const std::uint64_t names_at = at;
    const std::uint64_t total = names_at + align_up(kNameRecord * count, a.alignment);
    if (total > 0xFFFFFFFFull) {
        error = "the rebuilt archive would exceed 4 GiB";
        return false;
    }
    if (fs::is_regular_file(out, ec) && fs::file_size(out, ec) == total) return true;  // already built
    // Remove older builds of this archive (they are large).
    for (fs::directory_iterator it(cache_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = utf8(it->path().filename());
        if (n.size() == stem.size() + 21 && n.rfind(stem + "-", 0) == 0 && n.ends_with(".afs")) {
            std::error_code rm;
            fs::remove(it->path(), rm);
        }
    }
    std::vector<std::uint8_t> header(static_cast<std::size_t>(first), 0);
    std::memcpy(header.data(), "AFS\0", 4);
    put_le32(header.data() + 4, static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        put_le32(header.data() + 8 + 8 * i, static_cast<std::uint32_t>(offsets[i]));
        put_le32(header.data() + 12 + 8 * i, static_cast<std::uint32_t>(sizes[i]));
    }
    put_le32(header.data() + 8 + 8 * count, static_cast<std::uint32_t>(names_at));
    put_le32(header.data() + 12 + 8 * count, static_cast<std::uint32_t>(kNameRecord * count));
    std::vector<std::uint8_t> names = a.name_records;
    for (std::size_t i = 0; i < count; ++i) {
        // The record's last word is the entry's size when it was written that way; keep that true.
        std::uint8_t* rec = names.data() + kNameRecord * i;
        if (le32(rec + 0x2C) == a.entries[i].size) put_le32(rec + 0x2C, static_cast<std::uint32_t>(sizes[i]));
    }
    fs::path temp = out;
    temp += ".tmp";
    {
        std::ifstream in(original, std::ios::binary);
        std::ofstream o(temp, std::ios::binary | std::ios::trunc);
        if (!in || !o) {
            error = "cannot write " + utf8(temp);
            return false;
        }
        std::vector<char> buffer(1u << 20);
        o.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        std::uint64_t written = header.size();
        for (std::size_t i = 0; i < count; ++i) {
            pad_to(o, written, offsets[i]);
            if (replace[i] != nullptr) {
                std::ifstream r(replace[i]->host, std::ios::binary);
                if (!r || !copy_range(r, 0, sizes[i], o, buffer)) {
                    error = "cannot read " + utf8(replace[i]->host);
                    o.close();
                    fs::remove(temp, ec);
                    return false;
                }
            } else if (!copy_range(in, a.entries[i].offset, sizes[i], o, buffer)) {
                error = "cannot read entry " + std::to_string(i) + " of " + utf8(original);
                o.close();
                fs::remove(temp, ec);
                return false;
            }
            written += sizes[i];
        }
        pad_to(o, written, names_at);
        o.write(reinterpret_cast<const char*>(names.data()), static_cast<std::streamsize>(names.size()));
        written += names.size();
        pad_to(o, written, total);
        o.flush();
        if (!o) {
            error = "cannot write " + utf8(temp) + " (disk full?)";
            o.close();
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, out, ec);
    if (ec) {
        error = "cannot rename " + utf8(temp) + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

}  // namespace shadow::mods

namespace shadow::mods {

bool check_file_compatible(const std::filesystem::path& file, std::string_view game_path, std::string& why) {
    std::string ext;
    if (const std::size_t dot = game_path.rfind('.'); dot != std::string_view::npos) ext = lower(game_path.substr(dot));
    if (ext != ".sfd" && ext != ".flo") return true;
    // The MPEG video sequence header (00 00 01 B3: 12-bit width, 12-bit height) of the first video packet, which
    // every movie of the disc and of the mods tested carries within its first 64 KiB.
    std::ifstream in(file, std::ios::binary);
    Bytes head(64u << 10);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    for (std::size_t i = 0; i + 7 <= head.size(); ++i) {
        if (head[i] != 0 || head[i + 1] != 0 || head[i + 2] != 1 || head[i + 3] != 0xB3) continue;
        const unsigned width = static_cast<unsigned>(head[i + 4]) << 4 | head[i + 5] >> 4;
        const unsigned height = (static_cast<unsigned>(head[i + 5]) & 15u) << 8 | head[i + 6];
        if (width > 512u || height > 480u)
            return bad(why, "is a " + num(width) + "x" + num(height) + " movie; this game's movies are 512x480 and a "
                            "larger one plays scrambled (a movie of another version of the game, such as the Xbox's)");
        return true;
    }
    return true;  // no sequence header found early: nothing to judge (the stream check decides whether it is whole)
}

bool check_file_format(const std::filesystem::path& file, std::string_view game_path, std::string& why) {
    namespace fs = std::filesystem;
    std::string ext;
    if (const std::size_t dot = game_path.rfind('.'); dot != std::string_view::npos) ext = lower(game_path.substr(dot));
    if (!check_file_compatible(file, game_path, why)) return false;
    if (ext != ".one" && ext != ".txd" && ext != ".dff" && ext != ".afs" && ext != ".adx" && !is_data_kind(ext))
        return true;
    std::error_code ec;
    const std::uint64_t size = fs::file_size(file, ec);
    if (ec) {
        why = "cannot be read";
        return false;
    }
    // The loose data kinds (docs/mods/map/FORMAT_CHECKS.md): layouts, tables, fonts, UI projects, banks, movies.
    if (is_data_kind(ext)) return check_data_file(file, ext, game_path, size, why);
    if (ext == ".afs") {
        AfsArchive a;
        std::string e;
        if (!read_afs(file, a, e)) {
            why = e;
            return false;
        }
        return true;
    }
    std::uint8_t head[0x34] = {};
    std::ifstream in(file, std::ios::binary);
    const std::uint64_t want = std::min<std::uint64_t>(size, sizeof(head));
    if (!in.read(reinterpret_cast<char*>(head), static_cast<std::streamsize>(want))) {
        why = "cannot be read";
        return false;
    }
    if (ext == ".adx") {
        // CRI ADX: 0x8000, then the big-endian offset of the "(c)CRI" mark minus 4.
        const std::uint32_t copyright = static_cast<std::uint32_t>(head[2]) << 8 | head[3];
        if (size < 16 || head[0] != 0x80 || head[1] != 0x00 || copyright + 4u > size) {
            why = "is not a whole ADX stream (header damaged or file truncated)";
            return false;
        }
        return true;
    }
    if (size < 12) {
        why = "is shorter than a RenderWare chunk header (" + std::to_string(size) + " bytes)";
        return false;
    }
    const std::uint64_t length = le32(head + 4);
    if (ext == ".one") {
        if (size < 0x24 || std::memcmp(head + 12, "One Ver", 7) != 0) {
            why = "is not a ONE archive (no \"One Ver\" signature)";
            return false;
        }
        if (length + 12u != size) {
            why = "is " + std::to_string(size) + " bytes but its header says " + std::to_string(length + 12u) +
                  " (truncated or damaged download)";
            return false;
        }
        const std::uint64_t count = le32(head + 12 + 0x10);
        if (0x34ull + 0x38ull * count > length) {
            why = "has an entry table larger than the file (damaged)";
            return false;
        }
        return true;
    }
    if (length + 12u > size) {  // .txd / .dff: the first chunk must fit (files may carry padding after it)
        why = "is " + std::to_string(size) + " bytes but its first RenderWare chunk needs " +
              std::to_string(length + 12u) + " (truncated or damaged download)";
        return false;
    }
    return true;
}

}  // namespace shadow::mods
