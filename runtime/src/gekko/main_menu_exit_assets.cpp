// EXIT in the main menu: the picture and the CSD entry, built from the player's disc at mount. Contract:
// include/gekko/main_menu_exit.h; layouts and evidence: docs/re/main-menu-exit/README.md.

#include "gekko/main_menu_exit.h"

#include <algorithm>
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

// Each language's word, cut from that language's own gamemenu sheet (256 x 256 C8, RGB5A3 palette): a slice is
// {top row of the letters' face, first column, last column}; the 21 rows from five above the face are copied (rows
// above the sheet are transparent), so every word sits in its cell as the menu's own words sit in theirs. A letter is
// its face columns; between two letters, and at the word's ends, go three columns of outline cut from a place of the
// same sheet where two straight-sided letters meet (or a word starts / ends). Column numbers: docs/re/main-menu-exit.
// A sheet is recognised by FNV-1a 64 of its palette and texels; another sheet (a region, a mod) is never cut blind:
// the English word is used then.
struct Slice {
    u32 face, first, last;
};
// EN EXIT: EXPERT MODE (face 111): E + E/X gap + X + X/P gap 47..79; OPTIONS (face 47): I + I/O gap 125..130;
// EXPERT's T and the word's right outline 123..137.
constexpr Slice kEnglish[] = {{111u, 47u, 79u}, {47u, 125u, 130u}, {111u, 123u, 137u}};
// FR SORTIR (no Q on the sheet: QUITTER cannot be made): MODE HISTOIRE (face 68) gives S O R T I, H|I its gap 127..129,
// MODE its left outline 42..44; SELECTION's final N its right outline 221..223 (face 90).
constexpr Slice kFrench[] = {{68u, 42u, 44u},    {68u, 136u, 147u}, {68u, 127u, 129u}, {68u, 165u, 176u},
                             {68u, 127u, 129u},  {68u, 186u, 197u}, {68u, 127u, 129u}, {68u, 151u, 162u},
                             {68u, 127u, 129u},  {68u, 180u, 182u}, {68u, 127u, 129u}, {68u, 186u, 197u},
                             {90u, 221u, 223u}};
// GE ENDE (no B on the sheet: BEENDEN cannot be made): EXPERTEN-MODUS (face 111) gives the word-initial E with its left
// outline 25..38, N 129..141 and E 116..125; STORY-MODUS (face 68) D 164..176; OPTIONEN (face 47) the N|E gap 152..154
// and its final N's right outline 181..183.
constexpr Slice kGerman[] = {{111u, 25u, 38u},  {47u, 152u, 154u}, {111u, 129u, 141u}, {47u, 152u, 154u},
                             {68u, 164u, 176u}, {47u, 152u, 154u}, {111u, 116u, 125u}, {47u, 181u, 183u}};
// IT TERMINA (no C on the sheet: ESCI / USCITA cannot be made): PARTITA's left outline 61..63 (face 4); MODALITA
// ESPERTO (face 111) T 205..215, E + gap + R 179..202, M 26..39, I 98..100; NUOVA (face 132) N 91..103, the N|U gap
// 104..106, A and the word's right outline 153..167.
constexpr Slice kItalian[] = {{4u, 61u, 63u},     {111u, 205u, 215u}, {132u, 104u, 106u}, {111u, 179u, 202u},
                              {132u, 104u, 106u}, {111u, 26u, 39u},   {132u, 104u, 106u}, {111u, 98u, 100u},
                              {132u, 104u, 106u}, {132u, 91u, 103u},  {132u, 104u, 106u}, {132u, 153u, 167u}};
// SP SALIR: MODO SELECCIONAR (face 90) the word-initial S with its left outline 87..102, L 119..128, the final R and its
// right outline 225..239; PARTIDA (face 4) A 79..90; MODO HISTORIA (face 68) I 130..132; OPCIONES (face 47) the N|E
// gap 153..155.
constexpr Slice kSpanish[] = {{90u, 87u, 102u},  {47u, 153u, 155u}, {4u, 79u, 90u},   {47u, 153u, 155u},
                              {90u, 119u, 128u}, {47u, 153u, 155u}, {68u, 130u, 132u}, {47u, 153u, 155u},
                              {90u, 225u, 239u}};

struct Word {
    const char* language;
    u64 sheet_fnv;
    const Slice* slices;
    std::size_t count;
};
// JP: the Japanese sheet's kana and kanji (1P / 2P, opushon, sutoorii / serekuto / haado moodo, hajime kara, zenkai no
// tsuzuki kara) make no word for leaving the game; JP shows the English word, cut from the English sheet.
constexpr Word kWords[] = {
    {"EN", 0xA740D0F8621883A2ull, kEnglish, std::size(kEnglish)},
    {"FR", 0x585DDA4E31E39422ull, kFrench, std::size(kFrench)},
    {"GE", 0x76D2BE282F079492ull, kGerman, std::size(kGerman)},
    {"IT", 0x7B270734E33F5D47ull, kItalian, std::size(kItalian)},
    {"SP", 0x0E95623510E88704ull, kSpanish, std::size(kSpanish)},
};

constexpr u32 kBand = 21u;        // rows of a menu entry's cell
constexpr u32 kFaceRow = 5u;      // the face starts five rows into the cell, as in the menu's own cells
constexpr u32 kCellX = 4u;        // the cell in TitleParts (512 x 512; rows 400..511 are empty in all six languages)
constexpr u32 kPictureY = 452u;
constexpr u32 kCellWidth = 112u;  // the longest word (TERMINA, 98) and margins; every word is centred in it

u32 word_width(const Word& w) {
    u32 n = 0;
    for (std::size_t i = 0; i < w.count; ++i) n += w.slices[i].last - w.slices[i].first + 1u;
    return n;
}

bool sheet_of(const std::vector<u8>& txd, u64 fnv, Texture& sheet) {
    const std::optional<Texture> t = find_texture(txd, "gamemenu");
    if (!t || t->width != 256u || t->height != 256u || t->format != kFormatC8 || t->palette_format != kPaletteRgb5a3 ||
        t->texel_bytes != 256u * 256u)
        return false;
    u64 h = 0xCBF29CE484222325ull;
    for (std::size_t i = t->palette; i < t->texels - 4u; ++i) h = (h ^ txd[i]) * 0x100000001B3ull;
    for (std::size_t i = t->texels; i < t->texels + t->texel_bytes; ++i) h = (h ^ txd[i]) * 0x100000001B3ull;
    if (h != fnv) return false;
    sheet = *t;
    return true;
}

u16 sheet_colour(const std::vector<u8>& letters, const Texture& sheet, u32 x, u32 y) {
    const u8 index = letters[c8_at(sheet, x, y)];
    return static_cast<u16>(letters[sheet.palette + 2u * index] << 8 | letters[sheet.palette + 2u * index + 1u]);
}

}  // namespace

bool letters_available(const std::vector<u8>& letters_txd, std::string& error) {
    Texture sheet;
    if (sheet_of(letters_txd, kWords[0].sheet_fnv, sheet)) return true;
    error = "honban_EN.txd's gamemenu sheet is not the PAL one the letters are cut from";
    return false;
}

bool add_exit_picture(const std::vector<u8>& txd, const char* language, const std::vector<u8>& english_txd,
                      std::vector<u8>& out, std::string& error, std::string* word_language) {
    // The language's own word from its own sheet; otherwise (JP, or a sheet this table does not know) the English one.
    const Word* word = &kWords[0];
    const std::vector<u8>* letters = &english_txd;
    Texture sheet;
    for (const Word& w : kWords) {
        if (std::string_view(w.language) != language || std::string_view(language) == "EN") continue;
        if (sheet_of(txd, w.sheet_fnv, sheet)) {
            word = &w;
            letters = &txd;
        }
    }
    if (word == &kWords[0] && !sheet_of(english_txd, kWords[0].sheet_fnv, sheet)) {
        error = "honban_EN.txd's gamemenu sheet is not the PAL one the letters are cut from";
        return false;
    }
    if (word_language != nullptr) *word_language = word->language;
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
    // The sheet's own transparent colour around the word (its top-left texel), so filtering at the word's edge mixes in
    // what it mixes in on the sheet.
    const u16 clear = sheet_colour(*letters, sheet, 0u, 0u);
    if (!transparent(clear)) {
        error = "the gamemenu sheet's corner is not transparent";
        return false;
    }
    out = txd;
    const auto put = [&](u32 x, u32 y, u16 v) {
        const std::size_t at = rgb5a3_at(*parts, x, y);
        out[at] = static_cast<u8>(v >> 8);
        out[at + 1] = static_cast<u8>(v);
    };
    const u32 left = kCellX + (kCellWidth - word_width(*word)) / 2u;
    for (u32 y = 0; y < kBand; ++y) {
        for (u32 x = kCellX; x < kCellX + kCellWidth; ++x) put(x, kPictureY + y, clear);
        u32 x = left;
        for (std::size_t i = 0; i < word->count; ++i) {
            const Slice& s = word->slices[i];
            const s32 row = static_cast<s32>(s.face) - static_cast<s32>(kFaceRow) + static_cast<s32>(y);
            for (u32 c = s.first; c <= s.last; ++c, ++x)
                put(x, kPictureY + y, row < 0 ? clear : sheet_colour(*letters, sheet, c, static_cast<u32>(row)));
        }
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
