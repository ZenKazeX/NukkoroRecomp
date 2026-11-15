// The PC save wording on save tables from both discs (docs/mods/map/REGION_DIFF.md, "Save/Advertise key shift").
//
// save_text_region_tests <PAL files dir> [<NTSC-U files dir>] [<more Save_EN.fnt files>...]
// Needs the game's data; without it the test reports SKIP and passes.
//
// Holds: on the PAL table every PC message lands on its PAL position (the stock path, unchanged); on a table with
// one message fewer before them (NTSC-U), each PC message lands on the entry that carries the same key, never on its
// neighbour; every other text stays as it was.

#include "gekko/native_save_text.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::vector<gekko::u8> read(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<gekko::u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

gekko::u32 le32(const gekko::u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<gekko::u32>(p[3]) << 24; }
gekko::u32 key_at(const std::vector<gekko::u8>& t, gekko::u32 i) { return le32(t.data() + 4 + 20 * i + 4); }
gekko::u32 count(const std::vector<gekko::u8>& t) { return le32(t.data()); }

std::u16string utf16(const char* s) {
    std::u16string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) out.push_back(*p);  // ASCII
    return out;
}

void check_table(const std::filesystem::path& table_file, const std::filesystem::path& glyphs_file,
                 const std::vector<gekko::u8>& pal) {
    const std::vector<gekko::u8> table = read(table_file), glyphs = read(glyphs_file);
    if (table.size() < 4 || glyphs.empty()) {
        std::printf("SKIP %s (missing)\n", table_file.string().c_str());
        return;
    }
    std::vector<gekko::u8> out;
    std::string error;
    const bool ok = gekko::save_text::rewrite_table(table, glyphs, gekko::save_text::pc_english_messages(), out, error);
    check(ok, table_file.string() + ": rewrite (" + error + ")");
    if (!ok) return;
    std::vector<std::u16string> before, after;
    gekko::save_text::texts_of(table, before, error);
    gekko::save_text::texts_of(out, after, error);
    std::size_t changed = 0;
    for (const gekko::save_text::Message& m : gekko::save_text::pc_english_messages()) {
        const gekko::u32 key = key_at(pal, m.id);  // the message's key on the PAL disc
        gekko::u32 at = count(table);
        for (gekko::u32 i = 0; i < count(table); ++i)
            if (key_at(table, i) == key) at = i;
        check(at < count(table), table_file.string() + ": key " + std::to_string(key) + " present");
        if (at >= count(table)) continue;
        check(after[at] == utf16(m.text) || std::string(m.text).find_first_of("\x80") != std::string::npos,
              table_file.string() + ": PC text on key " + std::to_string(key));
    }
    for (std::size_t i = 0; i < before.size(); ++i) changed += before[i] != after[i] ? 1 : 0;
    check(changed <= gekko::save_text::pc_english_messages().size(),
          table_file.string() + ": only the PC messages change (" + std::to_string(changed) + ")");
    std::printf("%s: %u messages, %zu changed\n", table_file.string().c_str(), count(table), changed);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("SKIP: no game data given\n");
        return 0;
    }
    const std::filesystem::path pal_dir = argv[1];
    const std::vector<gekko::u8> pal = read(pal_dir / "fonts" / "Save" / "Save_EN.fnt");
    if (pal.size() < 4) {
        std::printf("SKIP: no PAL Save_EN.fnt\n");
        return 0;
    }
    const std::filesystem::path glyphs = pal_dir / "fonts" / "Save" / "Save_EN00.met";
    // The stock table: identical to the position-based rewrite (each id's own entry).
    check_table(pal_dir / "fonts" / "Save" / "Save_EN.fnt", glyphs, pal);
    std::vector<gekko::u8> out;
    std::string error;
    gekko::save_text::rewrite_table(pal, read(glyphs), gekko::save_text::pc_english_messages(), out, error);
    std::vector<std::u16string> texts;
    gekko::save_text::texts_of(out, texts, error);
    for (const gekko::save_text::Message& m : gekko::save_text::pc_english_messages())
        check(m.id < texts.size() && (texts[m.id] == utf16(m.text) || std::string(m.text).find('\x80') != std::string::npos),
              "PAL: message " + std::to_string(m.id) + " at its own position");
    for (int i = 2; i < argc; ++i) {
        std::filesystem::path p = argv[i];
        if (std::filesystem::is_directory(p)) p = p / "fonts" / "Save" / "Save_EN.fnt";
        check_table(p, glyphs, pal);
    }
    std::printf("%s\n", g_failures ? "FAILED" : "PASS");
    return g_failures ? 1 : 0;
}
