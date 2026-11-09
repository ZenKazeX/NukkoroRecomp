// Hungarian text as the game's English font files. Contract: include/gekko/native_hungarian_text.h.
// Glyph code points are numbers here (no non-ASCII literals: the compiler's source charset must not matter).

#include "gekko/native_hungarian_text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

#include "gekko/native_save_text.h"
#include "gekko/pc_settings.h"

namespace gekko::hungarian {

namespace {

constexpr u32 kCellW = 12, kCellH = 24;
constexpr u32 kSheet = 256;                  // the sheets are 256 x 256 I4
constexpr u32 kTexelOffset = 0xC0;           // the sheet's texels within a font .txd
constexpr std::size_t kTxdBytes = 32984;     // 0xC0 + 256 * 256 / 2 + the tail

using Cell = std::array<u8, kCellW * kCellH>;  // one 4-bit level per pixel

bool parse_met(const std::vector<u8>& met, std::vector<std::array<u32, 5>>& out) {
    std::istringstream in(std::string(met.begin(), met.end()));
    std::string line;
    u32 number = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (++number <= 3u) continue;
        std::istringstream f(line);
        std::array<u32, 5> e{};
        if (f >> e[0] >> e[1] >> e[2] >> e[3] >> e[4]) out.push_back(e);
    }
    return !out.empty();
}

// I4 texel (x, y) of the 8 x 8-tile layout.
std::size_t i4_at(u32 x, u32 y) { return kTexelOffset + (static_cast<std::size_t>(y / 8u) * (kSheet / 8u) + x / 8u) * 32u + (y % 8u) * 4u + (x % 8u) / 2u; }
u8 i4_get(const std::vector<u8>& txd, u32 x, u32 y) {
    const u8 b = txd[i4_at(x, y)];
    return (x & 1u) ? (b & 15u) : (b >> 4);
}
void i4_set(std::vector<u8>& txd, u32 x, u32 y, u8 v) {
    u8& b = txd[i4_at(x, y)];
    b = (x & 1u) ? static_cast<u8>((b & 0xF0u) | v) : static_cast<u8>((b & 0x0Fu) | (v << 4));
}

u8& px(Cell& c, u32 x, u32 y) { return c[y * kCellW + x]; }
u8 px(const Cell& c, u32 x, u32 y) { return c[y * kCellW + x]; }

// The accented letters the texts use, by one rule: the game's own plain letter, its pixels unchanged, + a mark from
// one accent library taken from the game's own accented letters. (The sheets' accented letters are separate, smaller
// renderings: their bodies are a row or two shorter and sit higher than the plain letters', which shows in running
// text where nearly every word has one.)
//   * The library: the acute of e-acute, the diaeresis of o-diaeresis, the tilde of n-tilde, the cedilla of
//     c-cedilla (their ink outside the letter's body, unchanged pixels), and the double acute: two copies of the
//     acute narrowed to kDoubleAcuteScale of its width (an area resample of its coverage, quantized to the sheets'
//     0 / 7 / 8 / 15 levels), centred where the diaeresis's two dots are, set kDoubleAcuteSpread times as far apart
//     (at the dots' own spacing the two strokes run together at the game's text size).
//   * A mark above: its bottom row two rows above the font's x-height (lowercase: the top of o) or cap height
//     (capitals: the top of O), one blank row between. A lowercase letter loses what is above its x-height (the dot
//     of i). A mark below (the cedilla) hangs from the font's baseline (the bottom of o).
//   * Horizontally, the mark keeps its offset from the body's centre (bounding boxes) in the glyph it comes from.
//   * A composition must fit the sheets' 24-row cell (the text engine draws every box at the cell's height: a taller
//     box is squeezed, measured in game). A plain capital starts at row 2, so a mark above it does not fit, nor does
//     the cedilla below c: those letters stay the sheets' own renderings (whose bodies are drawn smaller for that
//     reason), and O / U with a double acute are built the same way on the sheets' O / U with a diaeresis (its body,
//     the library's double acute in place of the dots).
constexpr double kDoubleAcuteScale = 0.8, kDoubleAcuteSpread = 1.25;

u8 sheet_level(double coverage) {
    if (coverage < 0.22) return 0;
    if (coverage < 0.5) return 7;
    if (coverage < 0.78) return 8;
    return 15;
}

// A glyph on a canvas that reaches kPad rows above and below the sheets' 24-row cell (canvas row kPad = cell row 0).
constexpr u32 kPad = 8, kCanvasH = kPad + kCellH + kPad;
struct Glyph {
    std::array<u8, kCellW * kCanvasH> pixels{};
    u8& at(int x, int y) { return pixels[static_cast<std::size_t>(y + static_cast<int>(kPad)) * kCellW + static_cast<std::size_t>(x)]; }
    u8 at(int x, int y) const { return pixels[static_cast<std::size_t>(y + static_cast<int>(kPad)) * kCellW + static_cast<std::size_t>(x)]; }
    static Glyph of(const Cell& c) {
        Glyph g;
        for (u32 y = 0; y < kCellH; ++y)
            for (u32 x = 0; x < kCellW; ++x) g.at(static_cast<int>(x), static_cast<int>(y)) = px(c, x, y);
        return g;
    }
};

struct Ink {
    int x0 = static_cast<int>(kCellW), x1 = -1, y0 = static_cast<int>(kCellH + kPad), y1 = -static_cast<int>(kPad) - 1;
    bool any() const { return x0 <= x1; }
    double centre() const { return (x0 + x1 + 1) / 2.0; }
};
Ink ink_of(const Glyph& g, int from = -static_cast<int>(kPad), int to = static_cast<int>(kCellH + kPad) - 1) {
    Ink r;
    for (int y = from; y <= to; ++y)
        for (int x = 0; x < static_cast<int>(kCellW); ++x)
            if (g.at(x, y) != 0u) {
                r.x0 = std::min(r.x0, x);
                r.x1 = std::max(r.x1, x);
                r.y0 = std::min(r.y0, y);
                r.y1 = std::max(r.y1, y);
            }
    return r;
}

struct Mark {
    Glyph pixels;             // in the source glyph's rows / columns
    int y0 = 0, y1 = 0;
    double centre = 0.0;      // the bounding box's horizontal centre
    double offset = 0.0;      // centre - the source body's centre
    bool below = false;
};

// The mark of an original accented glyph: its ink above row `body_top`, or below row `body_bottom`.
bool mark_of(const Glyph& source, int body_top, int body_bottom, bool below, Mark& m) {
    const int m0 = below ? body_bottom + 1 : 0, m1 = below ? static_cast<int>(kCellH) - 1 : body_top - 1;
    for (int y = m0; y <= m1; ++y)
        for (int x = 0; x < static_cast<int>(kCellW); ++x) m.pixels.at(x, y) = source.at(x, y);
    const Ink mi = ink_of(m.pixels);
    const Ink bi = below ? ink_of(source, 0, body_bottom) : ink_of(source, body_top, static_cast<int>(kCellH) - 1);
    if (!mi.any() || !bi.any()) return false;
    m.y0 = mi.y0;
    m.y1 = mi.y1;
    m.centre = mi.centre();
    m.offset = m.centre - bi.centre();
    m.below = below;
    return true;
}

double weighted_x(const Glyph& g, double lo, double hi) {
    double sum = 0.0, at = 0.0;
    for (int y = -static_cast<int>(kPad); y < static_cast<int>(kCellH + kPad); ++y)
        for (int x = 0; x < static_cast<int>(kCellW); ++x)
            if (x > lo && x < hi && g.at(x, y) != 0u) {
                sum += g.at(x, y);
                at += (x + 0.5) * g.at(x, y);
            }
    return sum > 0.0 ? at / sum : kCellW / 2.0;
}

Mark double_acute_of(const Mark& acute, const Mark& diaeresis) {
    // The dots' centres (level-weighted), split at the widest blank gap between their columns.
    std::vector<int> cols;
    for (int x = 0; x < static_cast<int>(kCellW); ++x)
        for (int y = diaeresis.y0; y <= diaeresis.y1; ++y)
            if (diaeresis.pixels.at(x, y) != 0u) {
                cols.push_back(x);
                break;
            }
    double split = kCellW / 2.0;
    int widest = 0;
    for (std::size_t i = 0; i + 1u < cols.size(); ++i)
        if (cols[i + 1] - cols[i] >= widest) {
            widest = cols[i + 1] - cols[i];
            split = (cols[i] + cols[i + 1]) / 2.0;
        }
    double dots[2] = {weighted_x(diaeresis.pixels, -1.0, split), weighted_x(diaeresis.pixels, split, kCellW + 1.0)};
    const double middle = (dots[0] + dots[1]) / 2.0;
    for (double& d : dots) d = middle + (d - middle) * kDoubleAcuteSpread;
    const double acute_x = weighted_x(acute.pixels, -1.0, kCellW + 1.0);
    Mark m;
    for (const double centre : dots) {
        for (int y = acute.y0; y <= acute.y1; ++y)
            for (int j = 0; j < static_cast<int>(kCellW); ++j) {
                const double s0 = (j - centre) / kDoubleAcuteScale + acute_x;
                const double s1 = (j + 1.0 - centre) / kDoubleAcuteScale + acute_x;
                double covered = 0.0;
                for (int x = 0; x < static_cast<int>(kCellW); ++x) {
                    const double overlap = std::min(s1, x + 1.0) - std::max(s0, static_cast<double>(x));
                    if (overlap > 0.0) covered += overlap * acute.pixels.at(x, y) / 15.0;
                }
                m.pixels.at(j, y) = std::max(m.pixels.at(j, y), sheet_level(covered / (s1 - s0)));
            }
    }
    const Ink mi = ink_of(m.pixels);
    m.y0 = mi.y0;
    m.y1 = mi.y1;
    m.centre = mi.centre();
    m.offset = diaeresis.offset + (m.centre - diaeresis.centre);  // the pair sits where the dots sit
    return m;
}

// `base` + `mark`; `reference` is the font's o (lowercase) or O (capitals).
Glyph compose(const Glyph& base, const Mark& mark, const Glyph& reference, bool capital) {
    Glyph b = base;
    const Ink ref = ink_of(reference);
    if (!capital && !mark.below)
        for (int y = -static_cast<int>(kPad); y < ref.y0; ++y)
            for (int x = 0; x < static_cast<int>(kCellW); ++x) b.at(x, y) = 0;  // the dot of i
    const int dy = mark.below ? ref.y1 + 1 - mark.y0 : ref.y0 - 2 - mark.y1;
    const int dx = static_cast<int>(std::nearbyint(ink_of(b).centre() + mark.offset - mark.centre));
    for (int y = mark.y0; y <= mark.y1; ++y)
        for (int x = 0; x < static_cast<int>(kCellW); ++x) {
            const int tx = x + dx, ty = y + dy;
            if (mark.pixels.at(x, y) == 0u || tx < 0 || tx >= static_cast<int>(kCellW) || ty < -static_cast<int>(kPad) ||
                ty >= static_cast<int>(kCellH + kPad))
                continue;
            b.at(tx, ty) = std::max(b.at(tx, ty), mark.pixels.at(x, y));
        }
    return b;
}

struct Atlas {
    std::map<u32, Glyph> glyphs;  // code -> glyph
    // The few 24-wide glyphs the texts use (the note and the Japanese language name's three characters), kept as the
    // game draws them.
    std::map<u32, std::array<u8, 2u * kCellW * kCellH>> wide;
    std::string error;
};

Atlas& atlas(const Reader& read) {
    static std::mutex mutex;
    static Atlas built;
    static bool done = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (done) return built;
    done = true;
    // The union of the glyphs of the five Latin sheets of every family (the typeface is the same in all of them).
    std::map<u32, Cell> sheet;
    for (const char* suffix : {"EN", "FR", "GE", "IT", "SP"}) {
        for (const std::string& family : families()) {
            const std::string base = "FONTS/" + [&] { std::string u = family; for (char& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); return u; }() + "/" + family + "_" + suffix;
            std::vector<u8> txd, met;
            if (!read(base + ".txd", txd) || !read(base + "00.met", met)) continue;
            if (txd.size() != kTxdBytes) continue;
            std::vector<std::array<u32, 5>> entries;
            if (!parse_met(met, entries)) continue;
            for (const auto& e : entries) {
                if (e[4] - e[2] + 1u != kCellH || e[3] >= kSheet || e[4] >= kSheet) continue;
                if (e[3] - e[1] + 1u == 2u * kCellW) {
                    if ((e[0] == 0x266Au || e[0] == 0x65E5u || e[0] == 0x672Cu || e[0] == 0x8A9Eu) && built.wide.count(e[0]) == 0u) {
                        std::array<u8, 2u * kCellW * kCellH> w{};
                        for (u32 y = 0; y < kCellH; ++y)
                            for (u32 x = 0; x < 2u * kCellW; ++x) w[y * 2u * kCellW + x] = i4_get(txd, e[1] + x, e[2] + y);
                        built.wide.emplace(e[0], w);
                    }
                    continue;
                }
                if (e[3] - e[1] + 1u != kCellW || sheet.count(e[0]) != 0u) continue;
                Cell c{};
                for (u32 y = 0; y < kCellH; ++y)
                    for (u32 x = 0; x < kCellW; ++x) px(c, x, y) = i4_get(txd, e[1] + x, e[2] + y);
                sheet.emplace(e[0], c);
            }
        }
    }
    for (const auto& [code, cell] : sheet) built.glyphs.emplace(code, Glyph::of(cell));
    auto have = [&](u32 code) { return built.glyphs.count(code) != 0u; };
    for (const u32 code : {0xE9u, 0xF6u, 0xF1u, 0xE7u, u32{'a'}, u32{'e'}, u32{'i'}, u32{'o'}, u32{'u'}, u32{'n'},
                           u32{'c'}, u32{'A'}, u32{'E'}, u32{'I'}, u32{'O'}, u32{'U'}, u32{','}}) {
        if (!have(code)) {
            built.error = "the game's sheets lack the letters the Hungarian glyphs are built from";
            return built;
        }
    }
    // The accent library (see compose): the marks' rows in their source letters are the sheets' (e-acute's body
    // starts at row 9, o-diaeresis's and n-tilde's at row 8, c-cedilla's body ends at row 19).
    Mark acute, diaeresis, tilde, cedilla;
    if (!mark_of(built.glyphs[0xE9], 9, 0, false, acute) || !mark_of(built.glyphs[0xF6], 8, 0, false, diaeresis) ||
        !mark_of(built.glyphs[0xF1], 8, 0, false, tilde) || !mark_of(built.glyphs[0xE7], 0, 19, true, cedilla)) {
        built.error = "the game's accented letters do not have the expected accent rows";
        return built;
    }
    const Mark double_acute = double_acute_of(acute, diaeresis);
    {
        // `body`: for a letter whose composition does not fit, the sheets' letter whose body (rows from `body_top`)
        // carries the mark instead (0: none; the sheets' own rendering of the code stays).
        struct Letter {
            u32 code, base;
            const Mark* mark;
            u32 body, body_top;
        };
        const Letter letters[] = {
            {0xE1, 'a', &acute, 0, 0},     {0xE9, 'e', &acute, 0, 0},     {0xED, 'i', &acute, 0, 0},
            {0xF3, 'o', &acute, 0, 0},     {0xFA, 'u', &acute, 0, 0},     {0xF6, 'o', &diaeresis, 0, 0},
            {0xFC, 'u', &diaeresis, 0, 0}, {0x151, 'o', &double_acute, 0, 0}, {0x171, 'u', &double_acute, 0, 0},
            {0xF1, 'n', &tilde, 0, 0},     {0xE7, 'c', &cedilla, 0, 0},
            {0xC1, 'A', &acute, 0, 0},     {0xC9, 'E', &acute, 0, 0},     {0xCD, 'I', &acute, 0, 0},
            {0xD3, 'O', &acute, 0, 0},     {0xDA, 'U', &acute, 0, 0},     {0xD6, 'O', &diaeresis, 0, 0},
            {0xDC, 'U', &diaeresis, 0, 0}, {0x150, 'O', &double_acute, 0xD6, 7}, {0x170, 'U', &double_acute, 0xDC, 7},
        };
        auto fits = [](const Glyph& g) {
            const Ink i = ink_of(g);
            return i.y0 >= 0 && i.y1 < static_cast<int>(kCellH);
        };
        std::map<u32, Glyph> made;  // from the original letters only, then in place of the sheets' renderings
        for (const Letter& l : letters) {
            const bool capital = l.base >= 'A' && l.base <= 'Z';
            Glyph g = compose(built.glyphs[l.base], *l.mark, built.glyphs[capital ? 'O' : 'o'], capital);
            if (fits(g)) {
                made[l.code] = g;
            } else if (l.body != 0u && have(l.body)) {
                Glyph body = built.glyphs[l.body];
                for (int y = 0; y < static_cast<int>(l.body_top); ++y)
                    for (int x = 0; x < static_cast<int>(kCellW); ++x) body.at(x, y) = 0;
                g = compose(body, *l.mark, body, false);
                if (fits(g)) made[l.code] = g;
            }
        }
        for (const auto& [code, g] : made) built.glyphs[code] = g;
    }
    auto add = [&](u32 code, const Glyph& g) { if (!have(code)) built.glyphs.emplace(code, g); };
    // Quotes: the original game sets BOTH quotation marks at the top, so the opening (low in Hungarian typography) and the
    // closing quote are both the game's own straight quote glyph.
    if (have('"')) add(0x201E, built.glyphs['"']);  // opening quote
    if (have('"')) add(0x201D, built.glyphs['"']);  // closing double quote
    if (have('-')) add(0x2013, built.glyphs['-']);  // en dash
    return built;
}

// The cells' layout. A letter's box (the cell the glyph list gives the text engine, which advances by its width) is
// its ink with kSideBearing blank columns on each side: the gap the sheets' own wide letters have between their ink
// in their 12-wide cells (1 to 2 columns, mostly 2), now for every letter, so a thin i, l or j no longer stands in a
// 12-wide cell with five blank columns each side. Everything else keeps the sheets' own 12-wide cell with its glyph
// where the sheets have it: the space (the original word gap), digits, punctuation (so "...", "?!", quotes and
// dashes are spaced exactly as in the English text) and symbols; the wide glyphs keep their 24-wide cells. Every box is the sheets' 24 rows high (the engine draws a box at
// that height whatever the glyph list says). Boxes are packed left to right, row by row (the wide glyphs on fresh
// rows), with one blank texel column and row between neighbours, so a filtered sample at a box's edge never reaches
// into the next box's ink.
constexpr int kSideBearing = 1;
bool is_letter(u32 code) {  // the Latin letters (Basic Latin, Latin-1 and Latin Extended-A)
    return (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z') ||
           (code >= 0xC0u && code <= 0x17Fu && code != 0xD7u && code != 0xF7u);
}
constexpr u32 kBoxGap = 1;
struct Layout {
    struct Box {
        u32 code, x, y, width, height;
        int src_x, src_y;  // the glyph's column / row at the box's left / top
        bool wide;
    };
    std::vector<Box> boxes;
};
Layout layout_of(const Atlas& a) {
    Layout l;
    u32 x = 0, y = 0, row_height = 0;
    auto place = [&](u32 code, u32 width, u32 height, int src_x, int src_y, bool wide) {
        if (x + width > kSheet) {
            x = 0;
            y += row_height + kBoxGap;
            row_height = 0;
        }
        l.boxes.push_back({code, x, y, width, height, src_x, src_y, wide});
        x += width + kBoxGap;
        row_height = std::max(row_height, height);
    };
    for (const auto& [code, g] : a.glyphs) {
        const Ink ink = ink_of(g);
        if (!ink.any() || !is_letter(code)) {  // the space, digits, punctuation, symbols: the sheets' own cell
            place(code, kCellW, kCellH, 0, 0, false);
            continue;
        }
        place(code, static_cast<u32>(ink.x1 - ink.x0 + 1 + 2 * kSideBearing), kCellH, ink.x0 - kSideBearing, 0,
              false);
    }
    if (x != 0u) {
        x = 0;
        y += row_height + kBoxGap;
        row_height = 0;
    }
    for (const auto& [code, w] : a.wide) {
        (void)w;
        place(code, 2u * kCellW, kCellH, 0, 0, true);
    }
    return l;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

bool selected() { return kOffered && pc_settings().current().language == kLanguage; }

std::u16string utf16(std::string_view s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i++]);
        u32 code = c;
        u32 extra = 0;
        if (c >= 0xF0u) code = c & 0x07u, extra = 3;
        else if (c >= 0xE0u) code = c & 0x0Fu, extra = 2;
        else if (c >= 0xC0u) code = c & 0x1Fu, extra = 1;
        for (u32 k = 0; k < extra && i < s.size(); ++k, ++i) code = code << 6 | (static_cast<u8>(s[i]) & 0x3Fu);
        if (code >= 0x10000u) {
            code -= 0x10000u;
            out.push_back(static_cast<char16_t>(0xD800u + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00u + (code & 0x3FFu)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
    return out;
}

std::u16string option_string(std::string_view key) {
    for (std::size_t i = 0; i < kOptionStringCount; ++i)
        if (key == kOptionStrings[i].key) return utf16(kOptionStrings[i].text);
    return {};
}

const std::vector<std::string>& families() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> v;
        for (std::size_t i = 0; i < kRowCount; ++i)
            if (v.empty() || v.back() != kRows[i].family) v.push_back(kRows[i].family);  // rows are sorted by family
        return v;
    }();
    return list;
}

std::string font_path(std::string_view family, std::string_view extension) {
    const std::string f(family);
    if (extension == "met") return "FONTS/" + upper(f) + "/" + f + "_EN00.met";
    return "FONTS/" + upper(f) + "/" + f + "_EN." + std::string(extension);
}

bool build_font_uncached(const std::string& fam, const Reader& read, Font& out, std::string& error);

bool build_font(std::string_view family, const Reader& read, Font& out, std::string& error) {
    const std::string fam(family);
    // One build per family (a family's three files are served by three replacements; the originals do not change).
    static std::mutex cache_mutex;
    static std::map<std::string, Font> cache;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const auto it = cache.find(fam);
        if (it != cache.end()) {
            out = it->second;
            return true;
        }
    }
    if (!build_font_uncached(fam, read, out, error)) return false;
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache.emplace(fam, out);
    return true;
}

bool build_font_uncached(const std::string& fam, const Reader& read, Font& out, std::string& error) {
    std::vector<u8> fnt, txd, met;
    if (!read(font_path(fam, "fnt"), fnt) || !read(font_path(fam, "txd"), txd) || !read(font_path(fam, "met"), met)) {
        error = fam + ": an original English font file is missing";
        return false;
    }
    if (txd.size() != kTxdBytes) {
        error = fam + ": the glyph sheet is not the expected 256 x 256 I4 dictionary";
        return false;
    }
    const Atlas& a = atlas(read);
    if (!a.error.empty()) {
        error = a.error;
        return false;
    }
    const Layout layout = layout_of(a);
    for (const Layout::Box& box : layout.boxes) {
        if (box.x + box.width > kSheet || box.y + box.height > kSheet) {
            error = "the Hungarian atlas does not fit the sheet";
            return false;
        }
    }
    // The sheet.
    out.txd = txd;
    std::fill(out.txd.begin() + kTexelOffset, out.txd.begin() + kTexelOffset + kSheet * kSheet / 2u, u8{0});
    for (const Layout::Box& at : layout.boxes) {
        for (u32 y = 0; y < at.height; ++y)
            for (u32 x = 0; x < at.width; ++x) {
                u8 v = 0;
                if (at.wide) {
                    v = a.wide.at(at.code)[y * at.width + x];
                } else {
                    const int gx = at.src_x + static_cast<int>(x), gy = at.src_y + static_cast<int>(y);
                    if (gx >= 0 && gx < static_cast<int>(kCellW)) v = a.glyphs.at(at.code).at(gx, gy);
                }
                i4_set(out.txd, at.x + x, at.y + y, v);
            }
    }
    // The glyph list: the original's three header lines, then one line per cell.
    {
        std::string text(met.begin(), met.end());
        std::size_t cut = 0;
        for (int line = 0; line < 3; ++line) {
            cut = text.find('\n', cut);
            if (cut == std::string::npos) {
                error = fam + ": the glyph list has no header";
                return false;
            }
            ++cut;
        }
        std::string made = text.substr(0, cut);
        char buf[64];
        for (const Layout::Box& at : layout.boxes) {
            std::snprintf(buf, sizeof(buf), "%5u %3u %3u %3u %3u #\n", at.code, at.x, at.y, at.x + at.width - 1u,
                          at.y + at.height - 1u);
            made += buf;
        }
        out.met.assign(made.begin(), made.end());
    }
    // The table.
    std::vector<std::pair<u32, std::u16string>> texts;
    u32 dropped = 0;
    // The drafts first, then the package's rows (they replace a draft of the same message).
    std::vector<std::pair<const Row*, bool>> sources;
    for (std::size_t i = 0; i < kDraftRowCount; ++i)
        if (fam == kDraftRows[i].family) sources.emplace_back(&kDraftRows[i], true);
    for (std::size_t i = 0; i < kRowCount; ++i)
        if (fam == kRows[i].family) sources.emplace_back(&kRows[i], false);
    for (const auto& [row, is_draft] : sources) {
        (void)is_draft;
        std::u16string t = utf16(row->text);
        for (char16_t& unit : t) {
            if (unit == u'\n' || unit == u' ' || a.glyphs.count(unit) != 0u || a.wide.count(unit) != 0u) continue;
            unit = u'?';  // the atlas has no such glyph: a visible mark, never a missing-glyph box
            ++dropped;
        }
        texts.emplace_back(static_cast<u32>(row->index), std::move(t));
    }
    if (!save_text::replace_texts(fnt, texts, out.fnt, error)) {
        error = fam + ": " + error;
        return false;
    }
    // The save screens' PC wording: in the Save table by index; in the front end's table at the messages that are
    // copies of them (found by their original English text).
    if (fam == "Save" || fam == "Advertise") {
        std::vector<std::pair<u32, std::u16string>> pc;
        std::vector<std::u16string> save_texts, adv_texts;
        if (fam == "Advertise") {
            std::vector<u8> save_original;
            std::string e2;
            if (!read(save_text::kEnglishTable, save_original) || !save_text::texts_of(save_original, save_texts, e2) ||
                !save_text::texts_of(fnt, adv_texts, e2)) {
                error = fam + ": the original save table is missing";
                return false;
            }
        }
        for (std::size_t m = 0; m < kSaveMessageCount; ++m) {
            std::u16string t = utf16(kSaveMessages[m].text);
            for (char16_t& unit : t) {
                if (unit == u'\n' || unit == u' ' || a.glyphs.count(unit) != 0u) continue;
                unit = u'?';
                ++dropped;
            }
            const u32 index = static_cast<u32>(kSaveMessages[m].index);
            if (fam == "Save") {
                pc.emplace_back(index, std::move(t));
            } else if (index < save_texts.size()) {
                for (std::size_t i = 0; i < adv_texts.size(); ++i)
                    if (adv_texts[i] == save_texts[index]) pc.emplace_back(static_cast<u32>(i), t);
            }
        }
        std::vector<u8> reworded;
        if (!pc.empty()) {
            if (!save_text::replace_texts(out.fnt, pc, reworded, error)) {
                error = fam + ": " + error;
                return false;
            }
            out.fnt = std::move(reworded);
        }
    }
    if (dropped != 0u) std::cerr << "[hungarian] " << fam << ": " << dropped << " characters without a glyph shown as '?'\n";
    return true;
}

bool palsel_file(const std::vector<u8>& original, std::vector<u8>& out, std::string& error) {
    // The RenderWare stream: dictionary (0x16) > native texture (0x15) > struct (0x01), whose body holds the name at +24,
    // width / height (big-endian u16) at +92, the format at +98 (9 = C8), the palette format at +99 (1 = RGB565), the
    // 256-entry palette at +104, the pixel length (u32) and the pixels.
    auto le32 = [&](std::size_t at) -> u32 {
        return at + 4u <= original.size() ? static_cast<u32>(original[at]) | static_cast<u32>(original[at + 1]) << 8 |
                                                static_cast<u32>(original[at + 2]) << 16 | static_cast<u32>(original[at + 3]) << 24
                                          : 0u;
    };
    std::size_t at = 12u;  // inside the dictionary
    const std::size_t end = original.size();
    while (at + 12u <= end) {
        const u32 type = le32(at), size = le32(at + 4u);
        if (type == 0x15u && at + 24u + 620u <= end) {
            const std::size_t body = at + 24u;
            const std::string name(reinterpret_cast<const char*>(&original[body + 24u]),
                                   strnlen(reinterpret_cast<const char*>(&original[body + 24u]), 32u));
            const u32 width = original[body + 92] << 8 | original[body + 93];
            const u32 height = original[body + 94] << 8 | original[body + 95];
            if (name == "screentest" && width == 512u && height == 512u && original[body + 98] == 9u &&
                original[body + 99] == 1u && body + 620u + 512u * 512u <= end) {
                out = original;
                for (u32 i = 0; i < 256u; ++i) {
                    out[body + 104u + 2u * i] = static_cast<u8>(kPalSelPalette[i] >> 8);
                    out[body + 104u + 2u * i + 1u] = static_cast<u8>(kPalSelPalette[i] & 0xFFu);
                }
                std::size_t to = body + 620u;
                for (unsigned i = 0; i + 1u < kPalSelRunBytes; i += 2u) {
                    for (u32 k = 0; k < kPalSelRuns[i]; ++k) {
                        if (to >= body + 620u + 512u * 512u) {
                            error = "the Hungarian 50/60 Hz picture is larger than the texture";
                            return false;
                        }
                        out[to++] = kPalSelRuns[i + 1u];
                    }
                }
                if (to != body + 620u + 512u * 512u) {
                    error = "the Hungarian 50/60 Hz picture is shorter than the texture";
                    return false;
                }
                return true;
            }
        }
        at += 12u + size;
        if (type == 0x15u) continue;
    }
    error = "palSel/PalSelEn.txd has no 512 x 512 C8 + RGB565 \"screentest\" texture";
    return false;
}

bool front_end_file(std::string_view path, const Reader& read, std::vector<u8>& out, std::string& error) {
    std::string lower(path);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool fnt = lower == "fonts/advertise/advertise_en.fnt";
    const bool txd = lower == "fonts/advertise/advertise_en.txd";
    const bool met = lower == "fonts/advertise/advertise_en00.met";
    if (!fnt && !txd && !met) {
        error = "not a front-end English-slot file";
        return false;
    }
    Font font;
    if (!build_font("Advertise", read, font, error)) return false;
    out = fnt ? font.fnt : txd ? font.txd : font.met;
    return true;
}

}  // namespace gekko::hungarian
