// EXIT in the main menu: the picture and the CSD entry, built from the player's disc at mount. Contract:
// include/gekko/main_menu_exit.h; layouts and evidence: docs/re/main-menu-exit/README.md.

#include "gekko/main_menu_exit.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>
#include <set>
#include <string_view>

namespace gekko::main_menu_exit {

bool enabled() {
    const char* v = std::getenv("SHADOW_MAIN_MENU_EXIT");
    return v == nullptr || v[0] != '0';
}

std::string picture_path(const char* language) { return std::string("csdFiles/honban_") + language + ".txd"; }

namespace {

u32 be32(const u8* p) {
    return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 | p[3];
}
u32 le32(const u8* p) {
    return static_cast<u32>(p[3]) << 24 | static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[1]) << 8 | p[0];
}
void put_be32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}
void put_le32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}
float as_float(u32 u) { return std::bit_cast<float>(u); }
u32 as_bits(float f) { return std::bit_cast<u32>(f); }

// ---------------------------------------------------------------------------------------------------------------
// The picture
// ---------------------------------------------------------------------------------------------------------------

// A GameCube texture of a RenderWare texture dictionary (tools/assets/txd_extract.py has the layout): the struct body
// of a TEXTURENATIVE chunk, big-endian fields, the palette (C8: 256 RGB5A3 / RGB565 words) and the texels after it.
struct Texture {
    std::size_t body = 0;
    u32 width = 0, height = 0, format = 0, palette_format = 0;
    std::size_t palette = 0;   // file offset of the palette (C8 only)
    std::size_t texels = 0;    // file offset of the texels
    u32 texel_bytes = 0;
};

constexpr u32 kTexDictionary = 0x16u, kTextureNative = 0x15u, kStruct = 0x01u;
constexpr u32 kFormatRgb5a3 = 5u, kFormatC8 = 9u, kPaletteRgb5a3 = 2u;

std::optional<Texture> find_texture(const std::vector<u8>& txd, std::string_view name) {
    if (txd.size() < 12u || le32(txd.data()) != kTexDictionary) return std::nullopt;
    const std::size_t end = std::min<std::size_t>(txd.size(), 12u + static_cast<std::size_t>(le32(txd.data() + 4)));
    for (std::size_t p = 12u; p + 12u <= end;) {
        const u32 id = le32(txd.data() + p), size = le32(txd.data() + p + 4);
        if (id == kTextureNative && p + 24u + 104u <= end && le32(txd.data() + p + 12) == kStruct) {
            const std::size_t body = p + 24u;
            const char* n = reinterpret_cast<const char*>(txd.data() + body + 24u);
            if (std::string_view(n, strnlen(n, 32u)) == name) {
                Texture t;
                t.body = body;
                t.width = static_cast<u32>(txd[body + 92] << 8 | txd[body + 93]);
                t.height = static_cast<u32>(txd[body + 94] << 8 | txd[body + 95]);
                t.format = txd[body + 98];
                t.palette_format = txd[body + 99];
                const u32 palette_words = t.format == kFormatC8 ? 256u : (t.format == 8u ? 16u : 0u);
                t.palette = body + 104u;
                const std::size_t length_at = t.palette + 2u * palette_words;
                if (length_at + 4u > txd.size()) return std::nullopt;
                t.texel_bytes = be32(txd.data() + length_at);
                t.texels = length_at + 4u;
                if (t.texels + t.texel_bytes > txd.size()) return std::nullopt;
                return t;
            }
        }
        p += 12u + static_cast<std::size_t>(size);
    }
    return std::nullopt;
}

// GX tiling: C8 in 8 x 4 blocks of bytes, RGB5A3 in 4 x 4 blocks of big-endian 16-bit words.
std::size_t c8_at(const Texture& t, u32 x, u32 y) {
    return t.texels + ((y / 4u) * (t.width / 8u) + x / 8u) * 32u + (y % 4u) * 8u + (x % 8u);
}
std::size_t rgb5a3_at(const Texture& t, u32 x, u32 y) {
    return t.texels + 2u * (((y / 4u) * (t.width / 4u) + x / 4u) * 16u + (y % 4u) * 4u + (x % 4u));
}
bool transparent(u16 rgb5a3) { return (rgb5a3 & 0xF000u) == 0u; }  // top bit 0 (A3 form) and alpha 0

// Each language's word, built column by column (column numbers and the reasoning: docs/re/main-menu-exit). A column is
// 21 rows; a menu entry's cell starts five rows above the letters' face. The pieces:
//  - Slice: columns of a sheet, the 21 rows from five above the face row `face` (rows above the sheet: transparent).
//    Whole letters with their own edge shading, the black columns between letters and the outline at a word's ends.
//  - Rows: columns of a sheet with the row of each of the 21 output rows chosen (a letter put together from the rows of
//    another: B from P), or opaque black / transparent.
//  - Over: pixels drawn over columns already placed (Q's tail over O), their colours taken from given pixels of the
//    sheet (a face pixel, an edge pixel, an outline pixel).
//  - Kanji: a glyph of the game's own message font (Advertise_JP.txd, 24 x 24 cells) scaled to the menu's 13-row face,
//    brightened, cut at a coverage threshold, with the menu's shading (a darker row under every stroke, a dark row
//    under the glyph) and its black outline. The menu sheet has no kanji for leaving the game.
// Every sheet a word reads is recognised by FNV-1a 64 of its palette and texels; when one is not the PAL sheet the
// English word is used, and for the Japanese word a font page that is not the PAL one does the same.
enum class Src : u8 { Own, English, Font };
enum class Kind : u8 { Slice, Rows, Over, Kanji };
constexpr s16 kBlack = -1, kClear = -2;
struct Tone {
    char ch;
    u32 x, y;  // a pixel of the source sheet with this colour
};
struct Op {
    Kind kind;
    Src src = Src::Own;
    u32 face = 0, first = 0, last = 0;  // Slice / Rows: columns; Slice: face row
    const s16* rows = nullptr;          // Rows: the source row of each of the 21 rows
    u32 at = 0;                         // Over: first column (from the word's start)
    const char* const* art = nullptr;   // Over: 21 strings, ' ' = keep
    const Tone* tones = nullptr;        // Over: the colour of each art character
    const char* page = nullptr;         // Kanji: font page
    u32 x0 = 0, y0 = 0, x1 = 0, y1 = 0, width = 0, trim_left = 0, trim_right = 0;
};

// Gaps and outlines: two black columns between letters, three outline columns at a word's ends.
// EN EXIT: EXPERT MODE (face 111): E + gap + X + gap 47..79; OPTIONS (face 47): I + gap 125..130; EXPERT's T and the
// word's right outline 123..137.
constexpr Op kEnglish[] = {{Kind::Slice, Src::Own, 111u, 47u, 79u},
                           {Kind::Slice, Src::Own, 47u, 125u, 130u},
                           {Kind::Slice, Src::Own, 111u, 123u, 137u}};
// FR QUITTER. The sheet has no Q: HISTOIRE's O (face 68, 165..177) with a tail drawn over its lower right corner into
// the three gap columns after it, in the sheet's own face / edge / outline colours. U from JEU (face 4), I, T from HISTOIRE, E R from
// MODE EXPERT (face 111); MODE's left outline 42..44, HISTOIRE's final outline 212..214; gaps 128..129 (H|I).
constexpr const char* kQTail[21] = {"", "", "", "", "", "", "", "", "", "", "", "",
                                    "    pp",  // rows 12..19, from the O's fourth column
                                    "     pp",
                                    "      pp",
                                    "       pppp",
                                    "        pppp",
                                    "          ll",
                                    "         ####",
                                    "         ####",
                                    ""};
constexpr Tone kFrenchTones[] = {{'p', 170u, 68u}, {'l', 182u, 80u}, {'#', 170u, 66u}, {0, 0u, 0u}};
constexpr Op kFrench[] = {{Kind::Slice, Src::Own, 68u, 42u, 44u},
                          {Kind::Slice, Src::Own, 68u, 165u, 177u},
                          {Kind::Slice, Src::Own, 68u, 128u, 129u},
                          {Kind::Slice, Src::Own, 68u, 128u, 128u},  // a third gap column: room for the tail
                          {.kind = Kind::Over, .at = 6u, .art = kQTail, .tones = kFrenchTones},
                          {Kind::Slice, Src::Own, 4u, 117u, 129u},
                          {Kind::Slice, Src::Own, 68u, 128u, 129u},
                          {Kind::Slice, Src::Own, 68u, 130u, 133u},
                          {Kind::Slice, Src::Own, 68u, 128u, 129u},
                          {Kind::Slice, Src::Own, 68u, 150u, 162u},
                          {Kind::Slice, Src::Own, 68u, 128u, 129u},
                          {Kind::Slice, Src::Own, 68u, 150u, 162u},
                          {Kind::Slice, Src::Own, 68u, 128u, 129u},
                          {Kind::Slice, Src::Own, 111u, 165u, 191u},
                          {Kind::Slice, Src::Own, 68u, 212u, 214u}};
// GE BEENDEN. The sheet has no B: EXPERTEN's P (face 111, columns 58..70) with its bowl rows repeated below the middle
// bar (rows 113..115 and 117..118 again) and its stem's foot replaced by outline. E and E N from EXPERTEN, D from
// STORY-MODUS (face 68), E N and the final outline from OPTIONEN (face 47); gap 152..154 (N|E of OPTIONEN).
constexpr s16 kBRows[21] = {106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 117,
                            118, 113, 114, 115, 117, 118, kBlack, kBlack, kBlack, kClear};
constexpr Op kGerman[] = {{Kind::Slice, Src::Own, 111u, 25u, 28u},
                          {Kind::Rows, Src::Own, 0u, 58u, 70u, kBRows},
                          {Kind::Slice, Src::Own, 47u, 152u, 154u},
                          {Kind::Slice, Src::Own, 111u, 116u, 125u},
                          {Kind::Slice, Src::Own, 47u, 152u, 154u},
                          {Kind::Slice, Src::Own, 111u, 116u, 141u},
                          {Kind::Slice, Src::Own, 47u, 152u, 154u},
                          {Kind::Slice, Src::Own, 68u, 164u, 176u},
                          {Kind::Slice, Src::Own, 47u, 152u, 154u},
                          {Kind::Slice, Src::Own, 47u, 155u, 180u},
                          {Kind::Slice, Src::Own, 47u, 181u, 183u}};
// IT ESCI. The sheet has no C: the English sheet's C (SELECT MODE, face 90, 106..118; its outline one row above and
// below the Italian rows is left out). E S and I from MODALITA ESPERTO (face 111) with E's left outline, OPZIONI's
// final outline (face 47); gap 105..106 (NUOVA, face 132).
constexpr s16 kCRows[21] = {85, 86, kClear, 88, 89, 90, 91, 92, 93, 94, 95,
                            96, 97, 98, 99, 100, 101, 102, 103, kClear, 105};
constexpr Op kItalian[] = {{Kind::Slice, Src::Own, 111u, 134u, 162u},
                           {Kind::Slice, Src::Own, 132u, 105u, 106u},
                           {Kind::Rows, Src::English, 0u, 106u, 118u, kCRows},
                           {Kind::Slice, Src::Own, 132u, 105u, 106u},
                           {Kind::Slice, Src::Own, 111u, 98u, 101u},
                           {Kind::Slice, Src::Own, 47u, 170u, 172u}};
// SP SALIR: MODO SELECCIONAR (face 90) the word-initial S with its left outline 87..102, L 119..128, the final R and its
// right outline 225..239; PARTIDA (face 4) A 79..90; MODO HISTORIA (face 68) I 130..132; OPCIONES (face 47) gap 153..155.
constexpr Op kSpanish[] = {{Kind::Slice, Src::Own, 90u, 87u, 102u}, {Kind::Slice, Src::Own, 47u, 153u, 155u},
                           {Kind::Slice, Src::Own, 4u, 79u, 90u},   {Kind::Slice, Src::Own, 47u, 153u, 155u},
                           {Kind::Slice, Src::Own, 90u, 119u, 128u}, {Kind::Slice, Src::Own, 47u, 153u, 155u},
                           {Kind::Slice, Src::Own, 68u, 130u, 132u}, {Kind::Slice, Src::Own, 47u, 153u, 155u},
                           {Kind::Slice, Src::Own, 90u, 225u, 239u}};
// JP shuuryou (U+7D42 U+4E86, "end": the word Japanese menus use for quitting): the message font's U+7D42 (page JP01,
// cell 168,96) and U+4E86 (page JP06, cell 24,24), 14 and 12 columns wide; the two outlines meet in two black columns.
constexpr Op kJapanese[] = {
    {.kind = Kind::Kanji, .src = Src::Font, .page = "Advertise_JP01", .x0 = 168u, .y0 = 96u, .x1 = 191u, .y1 = 119u,
     .width = 14u, .trim_left = 0u, .trim_right = 2u},
    {.kind = Kind::Kanji, .src = Src::Font, .page = "Advertise_JP06", .x0 = 24u, .y0 = 24u, .x1 = 47u, .y1 = 47u,
     .width = 12u, .trim_left = 1u, .trim_right = 0u},
};

struct Word {
    const char* language;
    u64 sheet_fnv;
    const Op* ops;
    std::size_t count;
};
constexpr u64 kEnglishFnv = 0xA740D0F8621883A2ull;
constexpr Word kWords[] = {
    {"EN", kEnglishFnv, kEnglish, std::size(kEnglish)},
    {"FR", 0x585DDA4E31E39422ull, kFrench, std::size(kFrench)},
    {"GE", 0x76D2BE282F079492ull, kGerman, std::size(kGerman)},
    {"IT", 0x7B270734E33F5D47ull, kItalian, std::size(kItalian)},
    {"SP", 0x0E95623510E88704ull, kSpanish, std::size(kSpanish)},
    {"JP", 0x1B150BEF1B601C07ull, kJapanese, std::size(kJapanese)},
};
// The two pages of Advertise_JP.txd the kanji come from (C4, RGB565 palette): FNV-1a 64 of palette and texels.
struct FontPage {
    const char* name;
    u64 fnv;
};
constexpr FontPage kFontPages[] = {{"Advertise_JP01", 0xCD8DD5DDD784AAF3ull}, {"Advertise_JP06", 0x698BD132ADE66AE2ull}};

constexpr u32 kBand = 21u;        // rows of a menu entry's cell
constexpr u32 kFaceRow = 5u;      // the face starts five rows into the cell, as in the menu's own cells
constexpr u32 kCellX = 4u;        // the cell in TitleParts (512 x 512; rows 400..511 are empty in all six languages)
constexpr u32 kPictureY = 452u;
constexpr u32 kCellWidth = 112u;  // the longest word (BEENDEN, 107) and margins; every word is centred in it
constexpr u16 kOpaqueBlack = 0x8000u;

u64 fnv_of(const std::vector<u8>& bytes, std::size_t palette, std::size_t palette_end, const Texture& t) {
    u64 h = 0xCBF29CE484222325ull;
    for (std::size_t i = palette; i < palette_end; ++i) h = (h ^ bytes[i]) * 0x100000001B3ull;
    for (std::size_t i = t.texels; i < t.texels + t.texel_bytes; ++i) h = (h ^ bytes[i]) * 0x100000001B3ull;
    return h;
}

bool sheet_of(const std::vector<u8>& txd, u64 fnv, Texture& sheet) {
    const std::optional<Texture> t = find_texture(txd, "gamemenu");
    if (!t || t->width != 256u || t->height != 256u || t->format != kFormatC8 || t->palette_format != kPaletteRgb5a3 ||
        t->texel_bytes != 256u * 256u || fnv_of(txd, t->palette, t->texels - 4u, *t) != fnv)
        return false;
    sheet = *t;
    return true;
}

u16 sheet_colour(const std::vector<u8>& letters, const Texture& sheet, u32 x, u32 y) {
    const u8 index = letters[c8_at(sheet, x, y)];
    return static_cast<u16>(letters[sheet.palette + 2u * index] << 8 | letters[sheet.palette + 2u * index + 1u]);
}

// A C4 font page's brightness at (x, y), 0..255: the RGB565 palette colour's mean, as tools/assets/txd_extract.py
// decodes it (5 / 6 bits widened by repeating their top bits).
u32 font_brightness(const std::vector<u8>& txd, const Texture& page, u32 x, u32 y) {
    const std::size_t at = page.texels + ((y / 8u) * (page.width / 8u) + x / 8u) * 32u + (y % 8u) * 4u + (x % 8u) / 2u;
    const u32 index = (x % 2u) == 0u ? txd[at] >> 4 : txd[at] & 0xFu;
    const u32 v = static_cast<u32>(txd[page.palette + 2u * index] << 8 | txd[page.palette + 2u * index + 1u]);
    const u32 r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
    return ((r << 3 | r >> 2) + (g << 2 | g >> 4) + (b << 3 | b >> 2)) / 3u;
}

using Column = std::array<u16, kBand>;
u16 grey(u32 v) {
    const u32 c = v >> 3;
    return static_cast<u16>(kOpaqueBlack | c << 10 | c << 5 | c);
}

// A kanji in the menu's style (see the comment above kEnglish): columns `op.width` + 6, the glyph's face on the 13 rows
// from kFaceRow. Brightened coverage (x 2.2) at or over 0.36 is face, shaded 110..255; a pixel under a stroke is a
// shadow (light inside the glyph, dark under it); everything within the outline's reach of ink is opaque black.
void kanji(const std::vector<u8>& font, const Texture& page, const Op& op, u16 clear, std::vector<Column>& out) {
    constexpr u32 kFaceRows = 13u;
    constexpr double kGain = 2.2, kThreshold = 0.36, kFull = 0.85;
    std::vector<std::vector<double>> g(op.y1 - op.y0 + 1u, std::vector<double>(op.x1 - op.x0 + 1u));
    u32 iy0 = ~0u, iy1 = 0u, ix0 = ~0u, ix1 = 0u;
    for (u32 y = 0; y < g.size(); ++y)
        for (u32 x = 0; x < g[0].size(); ++x) {
            const u32 v = font_brightness(font, page, op.x0 + x, op.y0 + y);
            g[y][x] = v;
            if (v > 40u) {
                iy0 = std::min(iy0, y);
                iy1 = std::max(iy1, y + 1u);
                ix0 = std::min(ix0, x);
                ix1 = std::max(ix1, x + 1u);
            }
        }
    const u32 w = op.width, width = w + 6u;
    std::vector<std::array<bool, kBand>> face(width), shadow(width);
    std::vector<std::array<u32, kBand>> value(width);
    for (auto& f : face) f.fill(false);
    for (auto& s : shadow) s.fill(false);
    if (iy0 == ~0u) iy0 = iy1 = ix0 = ix1 = 0u;
    for (u32 oy = 0; oy < kFaceRows; ++oy) {
        for (u32 ox = 0; ox < w; ++ox) {
            const double sy0 = iy0 + double(iy1 - iy0) * oy / kFaceRows, sy1 = iy0 + double(iy1 - iy0) * (oy + 1) / kFaceRows;
            const double sx0 = ix0 + double(ix1 - ix0) * ox / w, sx1 = ix0 + double(ix1 - ix0) * (ox + 1) / w;
            double total = 0.0, area = 0.0;
            for (u32 y = static_cast<u32>(sy0); y < sy1; ++y) {
                const double hy = std::min(sy1, y + 1.0) - std::max(sy0, double(y));
                for (u32 x = static_cast<u32>(sx0); x < sx1; ++x) {
                    const double hx = std::min(sx1, x + 1.0) - std::max(sx0, double(x));
                    total += g[y][x] / 255.0 * hx * hy;
                    area += hx * hy;
                }
            }
            const double c = std::min(1.0, (area > 0.0 ? total / area : 0.0) * kGain);
            if (c >= kThreshold) {
                face[3u + ox][kFaceRow + oy] = true;
                value[3u + ox][kFaceRow + oy] =
                    static_cast<u32>(110.0 + 145.0 * std::min(1.0, (c - kThreshold) / (kFull - kThreshold)));
            }
        }
    }
    for (u32 x = 0; x < width; ++x)
        for (u32 y = 1; y < kBand; ++y) shadow[x][y] = !face[x][y] && face[x][y - 1u];
    for (u32 x = op.trim_left; x < width - op.trim_right; ++x) {
        Column col;
        for (u32 y = 0; y < kBand; ++y) {
            if (face[x][y]) {
                col[y] = grey(value[x][y]);
            } else if (shadow[x][y]) {
                col[y] = grey(y >= kFaceRow + kFaceRows ? 70u : 120u);
            } else {
                bool near = false;
                for (s32 dy = -3; dy <= 2 && !near; ++dy)
                    for (s32 dx = -2; dx <= 2 && !near; ++dx) {
                        if ((dx == -2 || dx == 2) && (dy == -3 || dy == 2)) continue;
                        const s32 nx = static_cast<s32>(x) + dx, ny = static_cast<s32>(y) + dy;
                        near = nx >= 0 && ny >= 0 && nx < static_cast<s32>(width) && ny < static_cast<s32>(kBand) &&
                               (face[nx][ny] || shadow[nx][ny]);
                    }
                col[y] = near ? kOpaqueBlack : clear;
            }
        }
        out.push_back(col);
    }
}

// The word's columns, or false when a piece's source is missing.
bool build_word(const Word& word, const std::vector<u8>& own, const Texture& own_sheet, const std::vector<u8>& english,
                const Texture& english_sheet, const std::vector<u8>* font, u16 clear, std::vector<Column>& out) {
    for (std::size_t i = 0; i < word.count; ++i) {
        const Op& op = word.ops[i];
        const std::vector<u8>& bytes = op.src == Src::English ? english : own;
        const Texture& sheet = op.src == Src::English ? english_sheet : own_sheet;
        switch (op.kind) {
        case Kind::Slice:
        case Kind::Rows:
            for (u32 x = op.first; x <= op.last; ++x) {
                Column col;
                for (u32 y = 0; y < kBand; ++y) {
                    if (op.kind == Kind::Slice) {
                        const s32 row = static_cast<s32>(op.face) - static_cast<s32>(kFaceRow) + static_cast<s32>(y);
                        col[y] = row < 0 ? clear : sheet_colour(bytes, sheet, x, static_cast<u32>(row));
                    } else {
                        const s16 row = op.rows[y];
                        col[y] = row == kBlack ? kOpaqueBlack
                                 : row < 0     ? clear
                                               : sheet_colour(bytes, sheet, x, static_cast<u32>(row));
                    }
                }
                out.push_back(col);
            }
            break;
        case Kind::Over:
            for (u32 y = 0; y < kBand; ++y) {
                const std::string_view line(op.art[y]);
                for (std::size_t k = 0; k < line.size(); ++k) {
                    if (line[k] == ' ') continue;
                    const std::size_t x = op.at + k;
                    if (x >= out.size()) return false;
                    for (const Tone* t = op.tones; t->ch != 0; ++t)
                        if (t->ch == line[k]) out[x][y] = sheet_colour(bytes, sheet, t->x, t->y);
                }
            }
            break;
        case Kind::Kanji: {
            if (font == nullptr) return false;
            const std::optional<Texture> page = find_texture(*font, op.page);
            if (!page || page->format != 8u || page->width != 256u || page->height != 256u) return false;
            bool known = false;
            for (const FontPage& p : kFontPages)
                known = known || (std::string_view(p.name) == op.page &&
                                  fnv_of(*font, page->palette, page->palette + 32u, *page) == p.fnv);
            if (!known) return false;
            kanji(*font, *page, op, clear, out);
            break;
        }
        }
    }
    return true;
}

}  // namespace

bool letters_available(const std::vector<u8>& letters_txd, std::string& error) {
    Texture sheet;
    if (sheet_of(letters_txd, kEnglishFnv, sheet)) return true;
    error = "honban_EN.txd's gamemenu sheet is not the PAL one the letters are cut from";
    return false;
}

bool add_exit_picture(const std::vector<u8>& txd, const char* language, const std::vector<u8>& english_txd,
                      std::vector<u8>& out, std::string& error, std::string* word_language,
                      const std::vector<u8>* japanese_font) {
    Texture english_sheet, own_sheet;
    if (!sheet_of(english_txd, kEnglishFnv, english_sheet)) {
        error = "honban_EN.txd's gamemenu sheet is not the PAL one the letters are cut from";
        return false;
    }
    const std::optional<Texture> parts = find_texture(txd, "TitleParts");
    if (!parts || parts->width != 512u || parts->height != 512u || parts->format != kFormatRgb5a3 ||
        parts->texel_bytes != 512u * 512u * 2u) {
        error = "no 512 x 512 RGB5A3 TitleParts texture";
        return false;
    }
    for (u32 y = kPictureY; y < kPictureY + kBand; ++y) {
        for (u32 x = kCellX; x < kCellX + kCellWidth; ++x) {
            const std::size_t at = rgb5a3_at(*parts, x, y);
            if (!transparent(static_cast<u16>(txd[at] << 8 | txd[at + 1]))) {
                error = "the TitleParts area for the picture is not empty";
                return false;
            }
        }
    }
    // The language's own word from its own sheet; otherwise (a sheet or a font page this table does not know) English.
    std::vector<Column> columns;
    const Word* chosen = &kWords[0];
    u16 clear = sheet_colour(english_txd, english_sheet, 0u, 0u);
    for (const Word& w : kWords) {
        if (std::string_view(w.language) != language || std::string_view(language) == "EN") continue;
        if (!sheet_of(txd, w.sheet_fnv, own_sheet)) break;
        const u16 own_clear = sheet_colour(txd, own_sheet, 0u, 0u);
        if (build_word(w, txd, own_sheet, english_txd, english_sheet, japanese_font, own_clear, columns)) {
            chosen = &w;
            clear = own_clear;
        } else {
            columns.clear();
        }
    }
    if (chosen == &kWords[0] &&
        !build_word(kWords[0], english_txd, english_sheet, english_txd, english_sheet, nullptr, clear, columns)) {
        error = "the English word could not be built";
        return false;
    }
    if (!transparent(clear) || columns.size() > kCellWidth) {
        error = "the word does not fit its cell";
        return false;
    }
    if (word_language != nullptr) *word_language = chosen->language;
    out = txd;
    const auto put = [&](u32 x, u32 y, u16 v) {
        const std::size_t at = rgb5a3_at(*parts, x, y);
        out[at] = static_cast<u8>(v >> 8);
        out[at + 1] = static_cast<u8>(v);
    };
    const u32 left = kCellX + (kCellWidth - static_cast<u32>(columns.size())) / 2u;
    for (u32 y = 0; y < kBand; ++y) {
        for (u32 x = kCellX; x < kCellX + kCellWidth; ++x) put(x, kPictureY + y, clear);
        for (u32 x = 0; x < columns.size(); ++x) put(left + x, kPictureY + y, columns[x][y]);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The entry
// ---------------------------------------------------------------------------------------------------------------

namespace {

// A bare NGIF project: header (0x20 bytes), the nCPJ data area, the NOF0 table of the data area's pointer words, NEND.
// Every pointer is an offset from the data area's start; the loader (0x80360224) adds the area's address to each word
// NOF0 lists. Additions are appended to the data area and their pointer words appended to NOF0.
class Project {
public:
    bool parse(const std::vector<u8>& file, std::string& error) {
        if (file.size() < 0x20u || std::memcmp(file.data(), "NGIF", 4) != 0 || be32(file.data() + 8) != 1u ||
            be32(file.data() + 0xC) != 0x20u) {
            error = "not a bare NGIF project";
            return false;
        }
        const u32 data_size = be32(file.data() + 0x10), nof = be32(file.data() + 0x14);
        if (nof != 0x20u + data_size || nof + 0x10u > file.size() || std::memcmp(file.data() + 0x20, "nCPJ", 4) != 0 ||
            le32(file.data() + 0x24) != data_size - 8u || std::memcmp(file.data() + nof, "NOF0", 4) != 0) {
            error = "unexpected NGIF layout";
            return false;
        }
        const u32 nof_size = le32(file.data() + nof + 4), count = be32(file.data() + nof + 8);
        if (nof + 8u + nof_size > file.size() || 0x10u + 4u * static_cast<u64>(count) > nof_size + 8u) {
            error = "unexpected NOF0 chunk";
            return false;
        }
        header_.assign(file.begin(), file.begin() + 0x20);
        data_.assign(file.begin() + 0x20, file.begin() + nof);
        for (u32 i = 0; i < count; ++i) {
            const u32 o = be32(file.data() + nof + 0x10 + 4u * i);
            relocs_.push_back(o);
            reloc_set_.insert(o);
        }
        tail_.assign(file.begin() + nof + 8 + nof_size, file.end());
        return true;
    }

    std::vector<u8> build() const {
        std::vector<u8> out(header_);
        // The data area ends on 16 bytes, as the original's does.
        const u32 data_size = static_cast<u32>((data_.size() + 15u) & ~std::size_t{15u});
        const u32 nof_size = ((16u + 4u * static_cast<u32>(relocs_.size()) + 15u) & ~15u) - 8u;
        put_be32(out.data() + 0x10, data_size);
        put_be32(out.data() + 0x14, 0x20u + data_size);
        put_be32(out.data() + 0x18, nof_size);
        out.insert(out.end(), data_.begin(), data_.end());
        out.resize(0x20u + data_size, 0u);
        put_le32(out.data() + 0x24, data_size - 8u);
        const std::size_t nof = out.size();
        out.resize(nof + 8u + nof_size, 0u);
        std::memcpy(out.data() + nof, "NOF0", 4);
        put_le32(out.data() + nof + 4, nof_size);
        put_be32(out.data() + nof + 8, static_cast<u32>(relocs_.size()));
        for (std::size_t i = 0; i < relocs_.size(); ++i) put_be32(out.data() + nof + 0x10 + 4u * i, relocs_[i]);
        out.insert(out.end(), tail_.begin(), tail_.end());
        out.resize((out.size() + 31u) & ~std::size_t{31u}, 0u);  // the file length stays a multiple of 32
        return out;
    }

    bool in(u32 o, u32 n = 4u) const { return static_cast<u64>(o) + n <= data_.size(); }
    u32 rd(u32 o) const { return in(o) ? be32(data_.data() + o) : 0u; }
    float rf(u32 o) const { return as_float(rd(o)); }
    void wr(u32 o, u32 v) { put_be32(data_.data() + o, v); }
    std::string_view name(u32 o) const {
        if (o == 0u || !in(o, 1u)) return {};
        const char* s = reinterpret_cast<const char*>(data_.data() + o);
        return std::string_view(s, strnlen(s, data_.size() - o));
    }
    bool pointer(u32 o) const { return reloc_set_.count(o) != 0u; }

    // A new zeroed block at the end of the data area (word aligned: the project holds words, floats and strings).
    u32 alloc(u32 size) {
        data_.resize((data_.size() + 3u) & ~std::size_t{3u}, 0u);
        const u32 at = static_cast<u32>(data_.size());
        data_.resize(data_.size() + ((size + 3u) & ~3u), 0u);
        return at;
    }
    // Writes a pointer word (listed for relocation, or unlisted when 0).
    void set_ptr(u32 o, u32 target) {
        wr(o, target);
        const bool listed = pointer(o);
        if (target != 0u && !listed) {
            relocs_.push_back(o);
            reloc_set_.insert(o);
        } else if (target == 0u && listed) {
            reloc_set_.erase(o);
            relocs_.erase(std::find(relocs_.begin(), relocs_.end(), o));
        }
    }
    // Copies [src, src + size) to dst, pointer words staying pointers.
    void copy_into(u32 dst, u32 src, u32 size) {
        std::memmove(data_.data() + dst, data_.data() + src, size);
        for (u32 k = 0; k < size; k += 4u)
            if (pointer(src + k)) set_ptr(dst + k, rd(src + k));
    }
    u32 copy(u32 src, u32 size) {
        const u32 at = alloc(size);
        copy_into(at, src, size);
        return at;
    }
    u32 string(std::string_view s) {
        const u32 at = alloc(static_cast<u32>(s.size()) + 1u);
        std::memcpy(data_.data() + at, s.data(), s.size());
        return at;
    }

private:
    std::vector<u8> header_, data_, tail_;
    std::vector<u32> relocs_;
    std::set<u32> reloc_set_;
};

// Scene descriptor fields (tools/assets/gncp_read.py).
constexpr u32 kCellCount = 0x1Cu, kCells = 0x20u, kGroupCount = 0x24u, kGroups = 0x28u, kNameCount = 0x2Cu,
              kNames = 0x30u, kMotionCount = 0x34u, kMotions = 0x38u, kMotionNames = 0x3Cu, kMotionRanges = 0x44u;
constexpr u32 kCellSize = 20u, kGroupSize = 16u, kNameSize = 12u, kCastSize = 80u, kTrackSize = 12u, kKeySize = 24u;
// Cast record: +0x0C four corners (x, y), +0x2C the property mask, +0x30 the property block, +0x3C the pattern count,
// +0x40 the pattern array. Property slots: 1 x, 2 y, 7 colour.
constexpr u32 kCastCorners = 0x0Cu, kCastMask = 0x2Cu, kCastBlock = 0x30u, kCastPatternCount = 0x3Cu,
              kCastPatterns = 0x40u;
constexpr u32 kSlotY = 2u;
constexpr u32 kSpareCell = 14u;  // scene Menu's unused cell (the CONTINUE row of the sheet)
constexpr u32 kTrackY = 2u, kTrackColour = 7u;  // motion track bits (bit 0 unused)

// The new cell: the word's area in TitleParts (texture 0, 512 x 512).
constexpr float kTexSize = 512.0f;

struct Scene {
    u32 desc = 0;
    u32 options = 0, second = 0, cover = 0;  // groups of "options", "2pgame", "blackcover"
};

std::optional<u32> find_name(const Project& p, u32 count, u32 table, u32 stride, std::string_view name) {
    for (u32 i = 0; i < count; ++i)
        if (p.name(p.rd(table + stride * i)) == name) return i;
    return std::nullopt;
}

// The cast group of name `name` in scene `desc` (one cast at index 0 required).
std::optional<u32> group_of(const Project& p, u32 desc, std::string_view name) {
    const std::optional<u32> i = find_name(p, p.rd(desc + kNameCount), p.rd(desc + kNames), kNameSize, name);
    if (!i) return std::nullopt;
    const u32 entry = p.rd(desc + kNames) + kNameSize * *i;
    if (p.rd(entry + 8u) != 0u) return std::nullopt;
    const u32 group = p.rd(entry + 4u);
    if (group >= p.rd(desc + kGroupCount) || p.rd(p.rd(desc + kGroups) + kGroupSize * group) != 1u) return std::nullopt;
    return group;
}

u32 cast_of(const Project& p, u32 desc, u32 group) {
    return p.rd(p.rd(p.rd(desc + kGroups) + kGroupSize * group + 4u));
}

u32 slot_of(u32 mask, u32 k) { return 4u * static_cast<u32>(std::popcount(mask & ((1u << k) - 1u))); }

// A copy of one cast's motion entry {track mask, tracks} at `entry` (8 bytes), its tracks and keys new. `map_y` maps
// the Y track's values; `colour_from` (an entry, or 0) gives the colour track's values (same key count required).
bool copy_motion_cast(Project& p, u32 dst, u32 entry, bool map_y, float y_from, u32 colour_from, std::string& error) {
    p.copy_into(dst, entry, 8u);
    const u32 mask = p.rd(entry), tracks = p.rd(entry + 4u);
    if (tracks == 0u) return true;
    if (!(map_y && ((mask >> kTrackY) & 1u) != 0u) && colour_from == 0u) return true;  // nothing changes: shared tracks
    const u32 count = static_cast<u32>(std::popcount(mask & 0xFEu));
    const u32 new_tracks = p.copy(tracks, kTrackSize * count);
    p.set_ptr(dst + 4u, new_tracks);
    u32 slot = 0;
    for (u32 bit = 1; bit < 8u; ++bit) {
        if (((mask >> bit) & 1u) == 0u) continue;
        const u32 track = new_tracks + kTrackSize * slot++;
        const u32 keys = p.rd(track + 8u), key_count = p.rd(track + 4u);
        const bool moved = bit == kTrackY && map_y, recoloured = bit == kTrackColour && colour_from != 0u;
        if (keys == 0u || (!moved && !recoloured)) continue;  // the same keys as the source: shared
        const u32 new_keys = p.copy(keys, kKeySize * key_count);
        p.set_ptr(track + 8u, new_keys);
        if (bit == kTrackY && map_y)
            for (u32 k = 0; k < key_count; ++k)
                p.wr(new_keys + kKeySize * k + 4u, as_bits(2.0f * p.rf(new_keys + kKeySize * k + 4u) - y_from));
        if (bit == kTrackColour && colour_from != 0u) {
            // The colour keys of the cast whose entry is `colour_from`, frame by frame.
            const u32 other_mask = p.rd(colour_from), other_tracks = p.rd(colour_from + 4u);
            if (((other_mask >> kTrackColour) & 1u) == 0u || other_tracks == 0u) {
                error = "the unselected entry has no colour track";
                return false;
            }
            const u32 other = other_tracks + kTrackSize * static_cast<u32>(std::popcount(other_mask & 0x7Eu));
            const u32 other_keys = p.rd(other + 8u);
            if (p.rd(other + 4u) != key_count || other_keys == 0u) {
                error = "the colour tracks differ in their keys";
                return false;
            }
            for (u32 k = 0; k < key_count; ++k) {
                if (p.rd(other_keys + kKeySize * k) != p.rd(new_keys + kKeySize * k)) {
                    error = "the colour tracks differ in their frames";
                    return false;
                }
                p.wr(new_keys + kKeySize * k + 4u, p.rd(other_keys + kKeySize * k + 4u));
            }
        }
    }
    return true;
}

}  // namespace

bool add_exit_entry(const std::vector<u8>& gncp, std::vector<u8>& out, std::string& error) {
    Project p;
    if (!p.parse(gncp, error)) return false;
    // The project root (+0x10 of the data area) -> {scene count, descriptors, names}.
    const u32 table = p.rd(p.rd(0x08u));
    const std::optional<u32> menu = find_name(p, p.rd(table), p.rd(table + 8u), 8u, "Menu");
    if (!menu) {
        error = "no scene Menu";
        return false;
    }
    Scene s;
    s.desc = p.rd(p.rd(table + 4u) + 4u * p.rd(p.rd(table + 8u) + 8u * *menu + 4u));
    const std::optional<u32> options = group_of(p, s.desc, "options"), second = group_of(p, s.desc, "2pgame"),
                             cover = group_of(p, s.desc, "blackcover");
    const u32 group_count = p.rd(s.desc + kGroupCount), motion_count = p.rd(s.desc + kMotionCount);
    if (!options || !second || !cover || *cover + 1u != group_count || find_name(p, p.rd(s.desc + kNameCount),
                                                                                p.rd(s.desc + kNames), kNameSize, "exit")) {
        error = "scene Menu is not the retail one (options / 2pgame / blackcover last; no exit)";
        return false;
    }
    s.options = *options;
    s.second = *second;
    s.cover = *cover;
    const u32 motion_names = p.rd(s.desc + kMotionNames);
    const std::optional<u32> menu_options = find_name(p, motion_count, motion_names, 8u, "Menu_Options");
    if (!menu_options || find_name(p, motion_count, motion_names, 8u, "Menu_Exit")) {
        error = "scene Menu has no motion Menu_Options (or already Menu_Exit)";
        return false;
    }
    const u32 options_motion = p.rd(motion_names + 8u * *menu_options + 4u);
    const u32 options_cast = cast_of(p, s.desc, s.options), second_cast = cast_of(p, s.desc, s.second);
    const u32 options_mask = p.rd(options_cast + kCastMask);
    const u32 options_y_at = p.rd(options_cast + kCastBlock) + slot_of(options_mask, kSlotY);
    const u32 second_y_at =
        p.rd(second_cast + kCastBlock) + slot_of(p.rd(second_cast + kCastMask), kSlotY);
    if (((options_mask >> kSlotY) & 1u) == 0u || ((p.rd(second_cast + kCastMask) >> kSlotY) & 1u) == 0u) {
        error = "options / 2pgame have no authored y";
        return false;
    }
    const float y_from = p.rf(second_y_at);  // 0.65: EXIT is one step (y_options - y_2pgame) below Options
    const u32 insert = s.cover;              // the new group goes before blackcover

    // The cell: scene Menu's cell table is its own (no other scene's descriptor names it), and no pattern of its casts
    // names cell 14 (the sheet's CONTINUE row, unused). That cell becomes the word's area: the table is not copied.
    const u32 cells = p.rd(s.desc + kCells);
    for (u32 i = 0; i < p.rd(table); ++i) {
        const u32 other = p.rd(p.rd(table + 4u) + 4u * i);
        if (other != s.desc && p.rd(other + kCells) == cells) {
            error = "scene Menu shares its cell table";
            return false;
        }
    }
    bool spare = p.rd(s.desc + kCellCount) > kSpareCell;
    for (u32 g = 0; g < group_count && spare; ++g) {
        const u32 record = p.rd(s.desc + kGroups) + kGroupSize * g;
        for (u32 c = 0; c < p.rd(record) && spare; ++c) {
            const u32 cast_record = p.rd(p.rd(record + 4u) + 4u * c);
            for (u32 k = 0; k < p.rd(cast_record + kCastPatternCount) && spare; ++k)
                spare = p.rd(p.rd(cast_record + kCastPatterns) + 4u * k) != kSpareCell;
        }
    }
    if (!spare) {
        error = "cell 14 of scene Menu is missing or in use";
        return false;
    }
    const u32 cell = cells + kCellSize * kSpareCell;
    p.wr(cell, 0u);  // texture 0: TitleParts
    p.wr(cell + 4u, as_bits(static_cast<float>(kCellX) / kTexSize));
    p.wr(cell + 8u, as_bits(static_cast<float>(kPictureY) / kTexSize));
    p.wr(cell + 12u, as_bits(static_cast<float>(kCellX + kCellWidth) / kTexSize));
    p.wr(cell + 16u, as_bits(static_cast<float>(kPictureY + kBand) / kTexSize));

    // The cast: options' record, block and patterns, on the new cell, kCellWidth wide, one step lower.
    const u32 cast = p.copy(options_cast, kCastSize);
    const u32 mask_bytes = 4u * static_cast<u32>(std::popcount(options_mask));
    const u32 block = p.copy(p.rd(options_cast + kCastBlock), mask_bytes);
    p.set_ptr(cast + kCastBlock, block);
    const u32 pattern_count = p.rd(options_cast + kCastPatternCount);
    if (pattern_count == 0u || pattern_count > 64u) {
        error = "options' pattern list is not the retail one";
        return false;
    }
    const u32 patterns = p.copy(p.rd(options_cast + kCastPatterns), 4u * pattern_count);
    p.set_ptr(cast + kCastPatterns, patterns);
    p.wr(patterns, kSpareCell);
    const float half_width = static_cast<float>(kCellWidth) / 2.0f / 640.0f;
    for (u32 k = 0; k < 4u; ++k) {
        const u32 x_at = cast + kCastCorners + 8u * k;
        p.wr(x_at, as_bits(p.rf(x_at) < 0.0f ? -half_width : half_width));
    }
    p.wr(block + slot_of(options_mask, kSlotY), as_bits(2.0f * p.rf(options_y_at) - y_from));

    // The groups: 0 .. insert-1, the new one, then blackcover.
    const u32 groups_old = p.rd(s.desc + kGroups);
    const u32 groups = p.alloc(kGroupSize * (group_count + 1u));
    for (u32 g = 0; g < group_count; ++g)
        p.copy_into(groups + kGroupSize * (g < insert ? g : g + 1u), groups_old + kGroupSize * g, kGroupSize);
    const u32 group = groups + kGroupSize * insert;
    p.copy_into(group, groups_old + kGroupSize * s.options, kGroupSize);
    const u32 cast_list = p.alloc(4u);
    p.set_ptr(cast_list, cast);
    p.set_ptr(group + 4u, cast_list);
    const u32 links_old = p.rd(groups_old + kGroupSize * s.options + 12u);
    p.set_ptr(group + 12u, links_old == 0u ? 0u : p.copy(links_old, 8u));
    p.wr(s.desc + kGroupCount, group_count + 1u);
    p.set_ptr(s.desc + kGroups, groups);

    // The cast names, sorted, blackcover's group renumbered.
    const u32 name_count = p.rd(s.desc + kNameCount), names_old = p.rd(s.desc + kNames);
    const u32 names = p.alloc(kNameSize * (name_count + 1u));
    const u32 exit_name = p.string("exit");
    u32 slot = 0;
    bool placed = false;
    for (u32 i = 0; i <= name_count; ++i) {
        const bool here = !placed && (i == name_count || p.name(p.rd(names_old + kNameSize * i)) > "exit");
        if (here) {
            p.set_ptr(names + kNameSize * slot, exit_name);
            p.wr(names + kNameSize * slot + 4u, insert);
            p.wr(names + kNameSize * slot + 8u, 0u);
            ++slot;
            placed = true;
        }
        if (i == name_count) break;
        const u32 to = names + kNameSize * slot++;
        p.copy_into(to, names_old + kNameSize * i, kNameSize);
        if (p.rd(to + 4u) >= insert) p.wr(to + 4u, p.rd(to + 4u) + 1u);
    }
    p.wr(s.desc + kNameCount, name_count + 1u);
    p.set_ptr(s.desc + kNames, names);

    // The motions: every motion's group list gets EXIT (options' tracks, one step lower) before blackcover, and a new
    // motion Menu_Exit is Menu_Options with options unselected (2pgame's colour there) and EXIT selected.
    const u32 motions_old = p.rd(s.desc + kMotions);
    const u32 motions = p.alloc(8u * (motion_count + 1u));
    u32 options_menu_groups = 0;  // Menu_Options' new group list
    for (u32 m = 0; m < motion_count; ++m) {
        const u32 entry_old = motions_old + 8u * m;
        if (p.rd(entry_old) != group_count || p.rd(entry_old + 4u) == 0u) {
            error = "a motion of scene Menu does not list every group";
            return false;
        }
        const u32 list_old = p.rd(entry_old + 4u);
        const u32 list = p.alloc(8u * (group_count + 1u));
        for (u32 g = 0; g < group_count; ++g) p.copy_into(list + 8u * (g < insert ? g : g + 1u), list_old + 8u * g, 8u);
        // EXIT's entry: options' cast list in this motion (one cast) with its tracks.
        const u32 options_entry = list_old + 8u * s.options;
        p.copy_into(list + 8u * insert, options_entry, 8u);
        const u32 casts_old = p.rd(options_entry + 4u);
        if (casts_old != 0u && p.rd(options_entry) == 1u) {
            const u32 casts = p.alloc(8u);
            const u32 second_cast_entry = m == options_motion ? p.rd(list_old + 8u * s.second + 4u) : 0u;
            if (m == options_motion && second_cast_entry == 0u) {
                error = "Menu_Options has no 2pgame tracks";
                return false;
            }
            if (!copy_motion_cast(p, casts, casts_old, true, y_from, second_cast_entry, error)) return false;
            p.set_ptr(list + 8u * insert + 4u, casts);
        } else if (casts_old != 0u) {
            error = "options has more than one cast in a motion";
            return false;
        }
        p.wr(motions + 8u * m, group_count + 1u);
        p.set_ptr(motions + 8u * m + 4u, list);
        if (m == options_motion) options_menu_groups = list;
    }
    {
        const u32 list = p.alloc(8u * (group_count + 1u));
        p.copy_into(list, options_menu_groups, 8u * (group_count + 1u));
        const u32 list_old = p.rd(motions_old + 8u * options_motion + 4u);
        const u32 options_casts = p.rd(list_old + 8u * s.options + 4u), second_casts = p.rd(list_old + 8u * s.second + 4u);
        if (options_casts == 0u || second_casts == 0u) {
            error = "Menu_Options has no options / 2pgame tracks";
            return false;
        }
        const u32 options_new = p.alloc(8u), exit_new = p.alloc(8u);
        if (!copy_motion_cast(p, options_new, options_casts, false, y_from, second_casts, error)) return false;
        if (!copy_motion_cast(p, exit_new, options_casts, true, y_from, 0u, error)) return false;
        p.set_ptr(list + 8u * s.options + 4u, options_new);
        p.set_ptr(list + 8u * insert + 4u, exit_new);
        p.wr(motions + 8u * motion_count, group_count + 1u);
        p.set_ptr(motions + 8u * motion_count + 4u, list);
    }
    p.wr(s.desc + kMotionCount, motion_count + 1u);
    p.set_ptr(s.desc + kMotions, motions);

    // The motion names (sorted) and the frame ranges (by motion index).
    const u32 mnames = p.alloc(8u * (motion_count + 1u));
    const u32 exit_motion = p.string("Menu_Exit");
    slot = 0;
    placed = false;
    for (u32 i = 0; i <= motion_count; ++i) {
        const bool here = !placed && (i == motion_count || p.name(p.rd(motion_names + 8u * i)) > "Menu_Exit");
        if (here) {
            p.set_ptr(mnames + 8u * slot, exit_motion);
            p.wr(mnames + 8u * slot + 4u, motion_count);
            ++slot;
            placed = true;
        }
        if (i == motion_count) break;
        p.copy_into(mnames + 8u * slot++, motion_names + 8u * i, 8u);
    }
    p.set_ptr(s.desc + kMotionNames, mnames);
    const u32 ranges_old = p.rd(s.desc + kMotionRanges);
    const u32 ranges = p.alloc(8u * (motion_count + 1u));
    p.copy_into(ranges, ranges_old, 8u * motion_count);
    p.copy_into(ranges + 8u * motion_count, ranges_old + 8u * options_motion, 8u);
    p.set_ptr(s.desc + kMotionRanges, ranges);

    out = p.build();
    // The file keeps the pages of the asset address space it had (64 KiB pages, 32 bytes of tail allowance:
    // gekko/asset_runtime.h), so every file after it keeps its guest token and a run that never shows the menu reads
    // exactly the tokens it read before.
    const auto pages = [](std::size_t bytes) { return (bytes + 32u + 0xFFFFu) >> 16; };
    if (pages(out.size()) != pages(gncp.size())) {
        error = "the project would grow past its address-space page (" + std::to_string(out.size()) + " bytes)";
        out.clear();
        return false;
    }
    return true;
}

}  // namespace gekko::main_menu_exit
