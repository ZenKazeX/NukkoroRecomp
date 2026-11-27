// Tests of the EXIT entry's files (gekko/main_menu_exit.h). With the game data (argument: sys/main.dol): honban.gncp
// gets the cast "exit" before blackcover and the motion Menu_Exit, every pointer word it adds is relocated, and a
// second addition is refused; each honban_<lang>.txd keeps its size and changes only inside the picture's area of
// TitleParts; EN, FR, GE, IT and SP cut their own word, JP the English one. A second argument writes the built project
// there (for tools/assets/gncp_read.py), a third the built texture dictionaries into that folder.

#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gekko/main_menu_exit.h"

namespace {

namespace mme = gekko::main_menu_exit;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool read_file(const std::filesystem::path& path, std::vector<u8>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

u32 be(const std::vector<u8>& b, std::size_t o) {
    return static_cast<u32>(b[o]) << 24 | static_cast<u32>(b[o + 1]) << 16 | static_cast<u32>(b[o + 2]) << 8 | b[o + 3];
}

// A minimal reader of the built project: data area from 0x20, pointer words relocated by the NOF0 list.
struct Reader {
    const std::vector<u8>& f;
    u32 rd(u32 o) const { return be(f, 0x20u + o); }
    std::string name(u32 o) const { return std::string(reinterpret_cast<const char*>(f.data() + 0x20u + o)); }
};

void project(const std::filesystem::path& files, const char* dump) {
    std::vector<u8> gncp, out, twice;
    if (!read_file(files / "csdFiles/honban.gncp", gncp)) {
        check(false, "honban.gncp is readable");
        return;
    }
    std::string error;
    check(mme::add_exit_entry(gncp, out, error), "the entry is added: " + error);
    if (out.empty()) return;
    check(out.size() % 32u == 0u && out.size() > gncp.size() && out.size() + 32u <= 0x10000u,
          "the project grows, stays a multiple of 32 bytes and within its 64 KiB address-space page");
    check(std::memcmp(out.data(), "NGIF", 4) == 0 && be(out, 0x14) == 0x20u + be(out, 0x10), "the NGIF header holds");
    check(!mme::add_exit_entry(out, twice, error), "a project with EXIT is refused");
    // Every word NOF0 lists lies in the data area and points into it.
    const u32 data = be(out, 0x10), nof = be(out, 0x14), count = be(out, nof + 8u);
    bool relocations = true;
    for (u32 i = 0; i < count; ++i) {
        const u32 at = be(out, nof + 0x10u + 4u * i);
        relocations = relocations && at + 4u <= data && be(out, 0x20u + at) < data;
    }
    check(relocations, "every relocated word points into the data area");
    // Scene Menu: 15 groups, "exit" in group 13 and "blackcover" in 14, Menu_Exit among 14 motions, 56 cells.
    const Reader r{out};
    const u32 table = r.rd(r.rd(0x08u));
    u32 desc = 0;
    for (u32 i = 0; i < r.rd(table); ++i)
        if (r.name(r.rd(r.rd(table + 8u) + 8u * i)) == "Menu") desc = r.rd(r.rd(table + 4u) + 4u * r.rd(r.rd(table + 8u) + 8u * i + 4u));
    check(desc != 0u, "scene Menu is found");
    if (desc == 0u) return;
    check(r.rd(desc + 0x24u) == 15u && r.rd(desc + 0x1Cu) == 55u && r.rd(desc + 0x34u) == 14u,
          "15 groups, 55 cells (cell 14 reused), 14 motions");
    u32 exit_group = 99u, cover_group = 99u;
    std::string previous;
    bool sorted = true;
    for (u32 i = 0; i < r.rd(desc + 0x2Cu); ++i) {
        const u32 e = r.rd(desc + 0x30u) + 12u * i;
        const std::string n = r.name(r.rd(e));
        sorted = sorted && previous < n;
        previous = n;
        if (n == "exit") exit_group = r.rd(e + 4u);
        if (n == "blackcover") cover_group = r.rd(e + 4u);
    }
    check(sorted && exit_group == 13u && cover_group == 14u, "cast names sorted, exit in group 13, blackcover in 14");
    bool menu_exit = false;
    previous.clear();
    sorted = true;
    for (u32 i = 0; i < r.rd(desc + 0x34u); ++i) {
        const std::string n = r.name(r.rd(r.rd(desc + 0x3Cu) + 8u * i));
        sorted = sorted && previous < n;
        previous = n;
        if (n == "Menu_Exit") menu_exit = r.rd(r.rd(desc + 0x3Cu) + 8u * i + 4u) == 13u;
    }
    check(sorted && menu_exit, "motion names sorted, Menu_Exit is motion 13");
    // EXIT's authored y is one step below Options (2 * 0.7146 - 0.65).
    const u32 cast = r.rd(r.rd(r.rd(desc + 0x28u) + 16u * 13u + 4u));
    const float y = std::bit_cast<float>(r.rd(r.rd(cast + 0x30u) + 8u));
    check(y > 0.779f && y < 0.780f, "EXIT sits at y 0.7792 (" + std::to_string(y) + ")");
    if (dump != nullptr) {
        std::ofstream o(dump, std::ios::binary);
        o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
}

void pictures(const std::filesystem::path& files, const char* dump_pictures) {
    std::vector<u8> letters;
    std::string error;
    check(read_file(files / mme::kLetterSheetPath, letters) && mme::letters_available(letters, error),
          "the English letter sheet is the PAL one: " + error);
    for (const char* language : mme::kLanguages) {
        std::vector<u8> txd, out, twice;
        const std::string path = mme::picture_path(language);
        if (!read_file(files / path, txd)) {
            check(false, path + " is readable");
            continue;
        }
        std::string word;
        check(mme::add_exit_picture(txd, language, letters, out, error, &word), path + ": the picture is written: " + error);
        const std::string expected = std::string(language) == "JP" ? "EN" : std::string(language);
        check(word == expected, path + ": the word is cut from the " + expected + " sheet (" + word + ")");
        check(out.size() == txd.size(), path + ": the file keeps its size");
        std::size_t changed = 0, first = out.size(), last = 0;
        for (std::size_t i = 0; i < txd.size() && i < out.size(); ++i) {
            if (txd[i] == out[i]) continue;
            ++changed;
            first = i < first ? i : first;
            last = i;
        }
        // The word's texels (2 bytes each) change; the area (112 x 21) lies in 6 rows of 4 x 4 blocks of TitleParts.
        check(changed > 54u * 10u && changed <= 112u * 21u * 2u, path + ": only the picture's texels change (" +
                                                                     std::to_string(changed) + " bytes)");
        check(last - first < 6u * 128u * 32u, path + ": the changes stay in one band of TitleParts");
        check(!mme::add_exit_picture(out, language, letters, twice, error), path + ": a second picture is refused");
        if (dump_pictures != nullptr) {
            std::ofstream o(std::string(dump_pictures) + "/honban_" + language + ".txd", std::ios::binary);
            o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        }
    }
    std::vector<u8> french;
    if (read_file(files / "csdFiles/honban_FR.txd", french))
        check(!mme::letters_available(french, error), "another sheet is not taken for the letters");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::filesystem::path files = std::filesystem::path(argv[1]).parent_path().parent_path() / "files";
        if (std::filesystem::exists(files / "csdFiles")) {
            project(files, argc > 2 ? argv[2] : nullptr);
            pictures(files, argc > 3 ? argv[3] : nullptr);
        } else {
            std::printf("game data not found beside %s: the disc checks were skipped\n", argv[1]);
        }
    } else {
        std::printf("no game data given: the disc checks were skipped\n");
    }
    const std::vector<u8> junk(256, 0x5A);
    std::vector<u8> out;
    std::string error;
    check(!mme::add_exit_entry(junk, out, error), "a file that is not a project is refused");
    check(!mme::add_exit_picture(junk, "EN", junk, out, error), "a file that is not a texture dictionary is refused");
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("main_menu_exit_tests: all passed\n");
    return 0;
}
