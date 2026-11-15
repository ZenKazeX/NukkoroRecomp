// The save screen's words on a PC. Contract: include/gekko/native_save_text.h.

#include "gekko/native_save_text.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <set>
#include <sstream>

namespace gekko::save_text {

namespace {

constexpr std::size_t kEntryBytes = 20u;

u32 load_le32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
           static_cast<u32>(p[3]) << 24;
}

void store_le32(u8* p, u32 value) noexcept {
    for (u32 i = 0; i < 4u; ++i) p[i] = static_cast<u8>(value >> (8u * i));
}

// The entries and texts of a table; `tail` is what follows the last text (the
// authoring tool's zero padding, kept).
struct Table {
    std::vector<std::array<u8, kEntryBytes>> entries;
    std::vector<std::u16string> texts;
    std::vector<u8> tail;
};

bool parse(const std::vector<u8>& bytes, Table& table, std::string& error) {
    if (bytes.size() < 4u) {
        error = "the message table is shorter than its count";
        return false;
    }
    const u32 count = load_le32(bytes.data());
    const std::size_t text_base = 4u + static_cast<std::size_t>(count) * kEntryBytes;
    if (count == 0u || text_base > bytes.size()) {
        error = "the message table's entries run past its end";
        return false;
    }
    table.entries.resize(count);
    table.texts.resize(count);
    std::size_t end_of_texts = text_base;
    for (u32 index = 0; index < count; ++index) {
        const u8* entry = bytes.data() + 4u + static_cast<std::size_t>(index) * kEntryBytes;
        std::copy_n(entry, kEntryBytes, table.entries[index].begin());
        std::size_t at = load_le32(entry);
        if (at < text_base || (at & 1u) != 0u) {
            error = "message " + std::to_string(index) + " has no text in the table's text area";
            return false;
        }
        std::u16string& text = table.texts[index];
        for (;; at += 2u) {
            if (at + 2u > bytes.size()) {
                error = "message " + std::to_string(index) + " runs past the table's end";
                return false;
            }
            const char16_t unit = static_cast<char16_t>(bytes[at] | bytes[at + 1u] << 8);
            if (unit == 0u) break;
            text.push_back(unit);
        }
        end_of_texts = std::max(end_of_texts, at + 2u);
    }
    table.tail.assign(bytes.begin() + static_cast<std::ptrdiff_t>(end_of_texts), bytes.end());
    return true;
}

// Every text in entry order after the entries, then the tail.
std::vector<u8> serialize(const Table& table) {
    const u32 count = static_cast<u32>(table.entries.size());
    std::vector<u8> out(4u + static_cast<std::size_t>(count) * kEntryBytes);
    store_le32(out.data(), count);
    for (u32 index = 0; index < count; ++index) {
        u8* entry = out.data() + 4u + static_cast<std::size_t>(index) * kEntryBytes;
        std::copy(table.entries[index].begin(), table.entries[index].end(), entry);
        store_le32(entry, static_cast<u32>(out.size()));
        for (const char16_t unit : table.texts[index]) {
            out.push_back(static_cast<u8>(unit & 0xFFu));
            out.push_back(static_cast<u8>(unit >> 8));
        }
        out.push_back(0u);
        out.push_back(0u);
    }
    out.insert(out.end(), table.tail.begin(), table.tail.end());
    return out;
}

// A message's UTF-8 text as the table's UTF-16 units (the save fonts hold only Basic Multilingual Plane glyphs).
std::u16string utf16_of(const char* text) {
    std::u16string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0u;) {
        u32 code = *p++;
        int more = code >= 0xF0u ? 3 : code >= 0xE0u ? 2 : code >= 0xC0u ? 1 : 0;
        if (more != 0) code &= 0x3Fu >> more;
        for (; more > 0 && (*p & 0xC0u) == 0x80u; --more) code = code << 6 | (*p++ & 0x3Fu);
        out.push_back(static_cast<char16_t>(code <= 0xFFFFu ? code : 0xFFFDu));
    }
    return out;
}

// The glyph codes a .met lists: "METRICS1", the name, the count, then one
// "code x0 y0 x1 y1 #" line per glyph.
bool glyph_codes(const std::vector<u8>& met, std::set<u32>& codes, std::string& error) {
    std::istringstream input(std::string(met.begin(), met.end()));
    std::string line;
    std::size_t number = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++number;
        if (number == 1u) {
            if (line != "METRICS1") {
                error = "the glyph list is not a METRICS1 file";
                return false;
            }
            continue;
        }
        if (number <= 3u) continue;  // the name, and a small number that is not the glyph count
        std::istringstream fields(line);
        u32 code = 0;
        if (fields >> code) codes.insert(code);
    }
    if (codes.empty()) {
        error = "the glyph list lists no glyph";
        return false;
    }
    return true;
}

// The message ids this file names are positions in the PAL tables: Save_*.fnt 80-126 (all six languages share these
// keys) and Advertise_*.fnt 346-347. A table from another disc or a mod can hold one message more or fewer before
// them (the NTSC-U Save and Advertise tables lack key 5489100, so every later position is one off), so a position
// is resolved through the key it carries on the PAL disc. On the PAL tables this is the position itself.
constexpr u32 kPalSaveKeys[47] = {
    6012100u, 6013100u, 6014100u, 6015100u, 6016100u, 6017100u, 6018100u, 6019100u, 6020100u, 6021100u,
    6022100u, 6023100u, 6024100u, 6025100u, 6026100u, 6027100u, 6028100u, 6029100u, 6030100u, 6031100u,
    6032100u, 6033100u, 6034100u, 6035100u, 6036100u, 6037100u, 6038100u, 6039100u, 6040100u, 6041100u,
    6042100u, 6043100u, 6044100u, 6045100u, 6046100u, 6047100u, 6048100u, 6049100u, 6051100u, 6052100u,
    6053100u, 6054100u, 6055100u, 6056100u, 6057100u, 6058100u, 6059100u};

u32 pal_key_of(u32 id) noexcept {
    if (id >= 80u && id <= 126u) return kPalSaveKeys[id - 80u];
    if (id == 346u) return 5414100u;  // Advertise: ON
    if (id == 347u) return 5415100u;  // Advertise: OFF
    return 0u;
}

// The position of PAL message `id` in `table`, or -1 when the table does not carry that message.
std::ptrdiff_t resolve(const Table& table, u32 id) {
    const u32 key = pal_key_of(id);
    const auto key_at = [&](std::size_t i) { return load_le32(table.entries[i].data() + 4u); };
    if (key == 0u) return id < table.entries.size() ? static_cast<std::ptrdiff_t>(id) : -1;
    if (id < table.entries.size() && key_at(id) == key) return static_cast<std::ptrdiff_t>(id);
    for (std::size_t i = 0; i < table.entries.size(); ++i)
        if (key_at(i) == key) return static_cast<std::ptrdiff_t>(i);
    return -1;
}

}  // namespace

const std::vector<Message>& pc_english_messages() {
    // docs/SAVE_SCREEN_OPTIONS.md, option A. The ids are the GameCube set's
    // (80-126); the PS2 and Xbox sets in the same table are never shown.
    static const std::vector<Message> messages = {
        {80, "Saving.\nDo not quit the game or\nturn off your PC."},
        {81, "The Save Data could not be created.\nIf you continue, Autosave\nwill be disabled."},
        {82, "The Save Data is from another\nversion and cannot be used."},
        {83, "There is no Game Data.\nCreate Game Data?"},
        {84, "The Save Data is damaged\nand cannot be used."},
        {85, "The Save Data is from another\nversion. Do you want to create\nnew Save Data?"},
        {86, "The Save Data is damaged.\nDo you want to create\nnew Save Data?"},
        {87, "Creating new Save Data will erase\nall saved progress.\nDo you want to continue?"},
        {88, "There is not enough disk space\nto save Game Data.\nPlease free some disk space\nand try again."},
        {90, "No Save Data was found."},
        {91, "There is not enough disk space\nto save Game Data.\nPlease free some disk space\nand try again.\n"
             "Do you want to continue the game?"},
        {92, "Do you want to continue the\ngame with this Save Data?"},
        {93, "No Game Data was found."},
        {94, "The Save Data could not be created.\nIf you continue, Autosave\nwill be disabled."},
        {95, "The Game Data is corrupted.\nThis Game Data needs to be deleted."},
        {96, "The Save Data is from another\nversion and cannot be used."},
        {97, "SHADOW THE HEDGEHOG& uses an\nAutosave feature. Do not quit the\ngame or turn off your PC while\n"
             "the game is saving."},
        {98, "The Save Data is damaged\nand cannot be used."},
        {99, "The Save Data is from another\nversion and cannot be used."},
        {100, "The Save Data is damaged.\nNew Save Data must be created."},
        {101, "Deleting Game Data. Do not quit\nthe game or turn off your PC."},
        {103, "Creating Save Data.\nDo not quit the game or\nturn off your PC."},
        {104, "Save Data created.\nPress the A Button to continue."},
        {105, "The Save Data could not be created."},
        {106, "Saving.\nDo not quit the game or\nturn off your PC."},
        {108, "Loading Game Data.\nDo not quit the game or\nturn off your PC."},
        {110, "Load failed.\nPlease check the Save Data\nand try again."},
        {113, "Create New Save Data"},
        {125, "This Save Data belongs to another\ngame file. Do you want to\noverwrite it? Progress will be\n"
              "saved here automatically."},
    };
    return messages;
}

namespace {
#include "pc_save_texts.inc"  // kPcSaveTexts: {suffix, id, UTF-8 text}, generated from tools/save_text
}  // namespace

const std::vector<Message>& pc_messages(std::string_view suffix) {
    static const std::vector<Message> none;
    if (suffix == "EN") return pc_english_messages();
    static const auto by_language = [] {
        std::vector<std::pair<std::string, std::vector<Message>>> out;
        for (const PcSaveText& t : kPcSaveTexts) {
            auto it = std::find_if(out.begin(), out.end(), [&](const auto& p) { return p.first == t.suffix; });
            if (it == out.end()) it = out.insert(out.end(), {t.suffix, {}});
            it->second.push_back(Message{t.id, t.text});
        }
        return out;
    }();
    for (const auto& [language, messages] : by_language)
        if (language == suffix) return messages;
    return none;
}

bool rewrite_table(const std::vector<u8>& table_bytes, const std::vector<u8>& glyph_list,
                   const std::vector<Message>& messages, std::vector<u8>& out, std::string& error) {
    return rewrite_table(table_bytes, std::vector<std::vector<u8>>{glyph_list}, messages, out, error);
}

bool rewrite_table(const std::vector<u8>& table_bytes, const std::vector<std::vector<u8>>& glyph_lists,
                   const std::vector<Message>& messages, std::vector<u8>& out, std::string& error) {
    Table table;
    if (!parse(table_bytes, table, error)) return false;
    std::set<u32> glyphs;
    for (const std::vector<u8>& list : glyph_lists)
        if (!glyph_codes(list, glyphs, error)) return false;
    for (const Message& message : messages) {
        const std::ptrdiff_t at = resolve(table, message.id);
        if (at < 0) {
            error = "message " + std::to_string(message.id) + " is not in the table";
            return false;
        }
        std::u16string text = utf16_of(message.text);
        for (const char16_t unit : text) {
            if (unit != u'\n' && glyphs.count(unit) == 0u) {
                error = "message " + std::to_string(message.id) + " uses a glyph the atlas lacks: U+" +
                        std::to_string(static_cast<u32>(unit));
                return false;
            }
        }
        table.texts[static_cast<std::size_t>(at)] = std::move(text);
    }
    out = serialize(table);
    return true;
}

bool rewordings_of(const std::vector<u8>& save_table, const std::vector<Message>& messages,
                   std::vector<Rewording>& out, std::string& error) {
    Table table;
    if (!parse(save_table, table, error)) return false;
    out.clear();
    for (const Message& message : messages) {
        const std::ptrdiff_t at = resolve(table, message.id);
        if (at < 0) {
            error = "message " + std::to_string(message.id) + " is not in the table";
            return false;
        }
        out.push_back(Rewording{table.texts[static_cast<std::size_t>(at)], utf16_of(message.text)});
    }
    return true;
}

bool reword_table(const std::vector<u8>& table_bytes, const std::vector<u8>& glyph_list,
                  const std::vector<Rewording>& rewordings, std::vector<u8>& out, u32& reworded, std::string& error) {
    return reword_table(table_bytes, std::vector<std::vector<u8>>{glyph_list}, rewordings, out, reworded, error);
}

bool reword_table(const std::vector<u8>& table_bytes, const std::vector<std::vector<u8>>& glyph_lists,
                  const std::vector<Rewording>& rewordings, std::vector<u8>& out, u32& reworded, std::string& error) {
    reworded = 0;
    Table table;
    if (!parse(table_bytes, table, error)) return false;
    std::set<u32> glyphs;
    for (const std::vector<u8>& list : glyph_lists)
        if (!glyph_codes(list, glyphs, error)) return false;
    for (const Rewording& rewording : rewordings) {
        for (const char16_t unit : rewording.replacement) {
            if (unit != u'\n' && glyphs.count(unit) == 0u) {
                error = "a reworded message uses a glyph the atlas lacks: U+" + std::to_string(unit);
                return false;
            }
        }
    }
    for (std::u16string& text : table.texts) {
        const auto match = std::find_if(rewordings.begin(), rewordings.end(),
                                        [&text](const Rewording& r) { return r.original == text; });
        if (match == rewordings.end()) continue;
        text = match->replacement;
        ++reworded;
    }
    out = serialize(table);
    return true;
}

bool texts_of(const std::vector<u8>& table_bytes, std::vector<std::u16string>& out, std::string& error) {
    Table table;
    if (!parse(table_bytes, table, error)) return false;
    out = std::move(table.texts);
    return true;
}

bool message_text(const std::vector<u8>& table_bytes, u32 id, std::u16string& text) {
    Table table;
    std::string error;
    if (!parse(table_bytes, table, error)) return false;
    const std::ptrdiff_t at = resolve(table, id);
    if (at < 0) return false;
    text = table.texts[static_cast<std::size_t>(at)];
    return true;
}

}  // namespace gekko::save_text
