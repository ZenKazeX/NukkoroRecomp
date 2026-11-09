#pragma once

// Hungarian text (the user's own localization, shadow_hu_localizacio.zip), served as the game's English font files.
//
// The game's text language stays English (G + 0x58 = 1, the PC settings' language 6 is mapped to it by the owners of
// the game's language: gekko/save_store.cpp, gekko/settings_game.cpp): every language-indexed table, the save
// record's language word and the save file stay valid. What changes is what the English font files hold. When the PC
// settings' language is 6 at mount, every FONTS/<family>/<family>_EN.fnt / .txd / 00.met is built here:
//   * the .fnt is the original English table with the translated messages' texts replaced (same entries, same words;
//     a message the package has no translation for keeps its English text);
//   * the glyph sheet is one Hungarian atlas in the game's own typeface (I4): every glyph the five Latin sheets of
//     the game have, taken from them unchanged, except the accented letters the texts use, which are the game's
//     plain letters + the game's own accents (one library: acute, diaeresis, double acute, tilde, cedilla), where
//     that fits the 24-row cell (lowercase); the accented capitals stay the sheets' own; o / u / O / U with a double
//     acute and the low double quote are built from the sheets' letters. All sheets of the game share one typeface
//     (the glyph art of a code is identical in every font), so one atlas serves every family;
//   * the glyph list (.met) lists the atlas's boxes: each glyph's ink with one blank column each side (24 rows high;
//     the space keeps its 12-wide cell, the wide glyphs theirs); the .txd keeps the original's header and name.
// The Japanese font files and the other five Latin languages are untouched. The voice language is independent.
//
// Data: native_hungarian_data.cpp is GENERATED from the package by tools/hungarian/gen_hu_data.py.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "gekko/types.h"

namespace gekko::hungarian {

// The PC settings' language value that selects Hungarian (options.cfg "language 6", label "Magyar").
inline constexpr int kLanguage = 6;

// True when the PC settings select Hungarian (read at asset mount; a change takes effect at the next start).
// Hungarian is withdrawn from the game for now: the Options menu does not offer it and a saved "language 6" reads as English.
inline constexpr bool kOffered = false;
bool selected();
inline bool active() noexcept { return selected(); }

// A translated message: the font family of its table ("Advertise", "Movie", "Save", "stg0100" ...), its index in the
// original English table and its text (UTF-8, '\n' = the game's line break).
struct Row {
    const char* family;
    int index;
    const char* text;
};
struct OptionString {
    const char* key;
    const char* text;  // UTF-8
};
extern const Row kRows[];
extern const std::size_t kRowCount;
// Machine-drafted translations of the messages the package does not cover (tools/hungarian/gen_hu_draft.py); the
// package's own rows win where both exist. Reviewable and replaceable: it is a separate table on purpose.
extern const Row kDraftRows[];
extern const std::size_t kDraftRowCount;
// The save screens' PC wording in Hungarian (the English build's option A, gekko/native_save_text.h): the message's index
// in Save_EN.fnt and its text. Applied to the Save table, and to the front end's copies of the same messages (found by
// their original text).
struct SaveMessage {
    int index;
    const char* text;
};
extern const SaveMessage kSaveMessages[];
extern const std::size_t kSaveMessageCount;
extern const OptionString kOptionStrings[];
extern const std::size_t kOptionStringCount;
extern const unsigned short kPalSelPalette[256];  // RGB565
extern const unsigned char kPalSelRuns[];         // (count, index) pairs of the 512 x 512 C8 pixels in 8 x 4 tile order
extern const unsigned kPalSelRunBytes;

// The Hungarian text of a PC Options menu string (its English table's key, e.g. "display.mode.label"); empty when
// the package has none.
std::u16string option_string(std::string_view key);

// The families the package translates, in order.
const std::vector<std::string>& families();

// The files of one family, built from the original English files.
struct Font {
    std::vector<u8> fnt, txd, met;
};
using Reader = std::function<bool(std::string_view path, std::vector<u8>& bytes)>;
// `read` reads any file of the asset tree (the originals). False and `error` when an original is missing or has
// another layout than expected.
bool build_font(std::string_view family, const Reader& read, Font& out, std::string& error);

// The front end's three English-slot files (FONTS/ADVERTISE/Advertise_EN.fnt, .txd, 00.met) as Hungarian bytes, for the
// Options menu's own mount lambda (it adds its messages to the table and gives the sheet to the overlay). `path` is
// matched case-insensitively. False (with `error`) for another path or a build failure.
bool front_end_file(std::string_view path, const Reader& read, std::vector<u8>& out, std::string& error);

// The PAL 50/60 Hz screen (palSel/PalSelEn.txd, texture "screentest": a picture with its words baked in) with the
// Hungarian picture in place of the English one: the original file with that texture's palette and pixels replaced
// (tools/hungarian/render_palsel.ps1 + make_palsel.py; same size, format C8 + RGB565, same bands as the original).
inline constexpr std::string_view kPalSelPath = "palSel/PalSelEn.txd";
bool palsel_file(const std::vector<u8>& original, std::vector<u8>& out, std::string& error);

// The path of a family's English-slot file: FONTS/<FAMILY>/<family>_EN.fnt, .txd or 00.met.
std::string font_path(std::string_view family, std::string_view extension);

// UTF-8 to UTF-16 (the package's texts).
std::u16string utf16(std::string_view utf8);

}  // namespace gekko::hungarian
