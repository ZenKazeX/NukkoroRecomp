#pragma once

// The save screen's words on a PC: the game's English save-message table
// (files/FONTS/SAVE/Save_EN.fnt) with its GameCube memory-card messages
// rewritten for a PC, as the user chose on 2026-10-27 (docs/SAVE_SCREEN_OPTIONS.md,
// option A: same screens, PC words). Screens, flow, buttons and timing are the
// game's own; only the text of the messages that speak of a memory card, a slot,
// formatting or the POWER button changes.
//
// The table: u32 LE entry count, then one 20-byte entry per message (u32 LE text
// offset and four words the game's authoring tool wrote, kept as they are), then
// the texts, UTF-16LE, each ending in a NUL. The text is drawn glyph by glyph
// from the language's atlas (Save_EN.txd), whose glyph list is Save_EN00.met
// ("METRICS1", name, a small number, then one "code x0 y0 x1 y1 #" line per glyph): every
// new text uses only glyphs that atlas has, so no font art changes.
//
// The front-end's English table (Advertise_EN.fnt) carries copies of the same
// messages; the boot's autosave notice is drawn from it. They are reworded by
// their original text (reword_table).
//
// The other languages keep their original text until they are translated.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gekko/types.h"

namespace gekko::save_text {

// The table's path in the asset tree, and its glyph list.
inline constexpr std::string_view kEnglishTable = "FONTS/SAVE/Save_EN.fnt";
inline constexpr std::string_view kEnglishGlyphs = "FONTS/SAVE/Save_EN00.met";

struct Message {
    u32 id;
    const char* text;  // ASCII; '\n' breaks a line, '&' is the atlas's trademark glyph
};

// Option A's English messages, by id.
const std::vector<Message>& pc_english_messages();

// The table with every message of `messages` replaced; every other entry,
// every entry's other words and every other text are kept. False (with the
// reason) when the table cannot be read, an id is outside it, or a text uses
// a glyph `glyph_list` lacks.
bool rewrite_table(const std::vector<u8>& table, const std::vector<u8>& glyph_list,
                   const std::vector<Message>& messages, std::vector<u8>& out, std::string& error);

// The table with the texts of the messages `texts` names (by index) replaced, UTF-16 as given; every other entry,
// word and text is kept. False (with the reason) when the table cannot be read or an index is outside it. Glyphs are
// the caller's to check (gekko/native_options_display.h checks every glyph list of its language).
bool replace_texts(const std::vector<u8>& table, const std::vector<std::pair<u32, std::u16string>>& texts,
                   std::vector<u8>& out, std::string& error);

// One message of a table, as UTF-16 code units (for tests and reports).
bool message_text(const std::vector<u8>& table, u32 id, std::u16string& text);

// The same messages in another table. The front-end's table
// (files/FONTS/ADVERTISE/Advertise_EN.fnt, its own atlas) carries copies of the
// save messages - the boot's autosave notice is drawn from it - at other ids.
// A rewording is the original text of a save message and its PC text.
struct Rewording {
    std::u16string original;
    std::u16string replacement;
};
inline constexpr std::string_view kEnglishFrontEndTable = "FONTS/ADVERTISE/Advertise_EN.fnt";
inline constexpr std::string_view kEnglishFrontEndGlyphs = "FONTS/ADVERTISE/Advertise_EN00.met";

// The rewordings of `messages`, read out of the original save table.
bool rewordings_of(const std::vector<u8>& save_table, const std::vector<Message>& messages,
                   std::vector<Rewording>& out, std::string& error);

// The table with every message whose whole text equals a rewording's original
// replaced by that rewording; `reworded` counts the replaced messages. Refused
// like rewrite_table.
bool reword_table(const std::vector<u8>& table, const std::vector<u8>& glyph_list,
                  const std::vector<Rewording>& rewordings, std::vector<u8>& out, u32& reworded, std::string& error);

}  // namespace gekko::save_text
