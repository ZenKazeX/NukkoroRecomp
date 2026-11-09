// The native asset namespace.
//
// With the game data present (argument: sys/main.dol) the namespace the
// runtime builds from the HOST TREE (files/) is checked against the disc's
// own FST as an oracle, entry for entry: numbering, names, nesting, sizes,
// the name layout, the reservation the boot keeps. The oracle's publication
// (support/fst_oracle.h: the table with each file's word1 replaced by its
// token, what the boot published before the native namespace) is checked
// against the disc's table. Production publishes nothing. Synthetic trees
// cover ordering, the path grammar and refusals.

#include "gekko/asset_mount.h"
#include "shadow/backends/storage_backends.h"
#include "gekko/asset_namespace.h"
#include "gekko/native_save_text.h"
#include "support/fst_oracle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "test_scratch.h"

namespace {
using gekko::u32;
using gekko::u64;
using gekko::u8;
using gekko::assets::AssetNamespace;
using Node = AssetNamespace::Node;

int g_failed = 0;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

u32 be32(const std::vector<u8>& b, std::size_t at) {
    return (static_cast<u32>(b[at]) << 24) | (static_cast<u32>(b[at + 1]) << 16) |
           (static_cast<u32>(b[at + 2]) << 8) | static_cast<u32>(b[at + 3]);
}

Node file(const std::string& name, u64 size) { return Node{name, false, size, {}}; }
Node dir(const std::string& name, std::vector<Node> children) { return Node{name, true, 0u, std::move(children)}; }

void ordering_and_layout() {
    AssetNamespace names;
    std::string error;
    require(AssetNamespace::from_tree(dir("", {file("b.bin", 2), dir("A", {file("z", 1), file("Y", 3)}),
                                              file("a.txt", 4), file("_x", 5), file("B.BIN", 6)}),
                                      names, error),
            error);
    // Folded order: "_x" < "a" < "a.txt" < "b.bin" (tie: "B.BIN" before "b.bin").
    const char* expected[] = {"", "_x", "A", "Y", "z", "a.txt", "B.BIN", "b.bin"};
    require(names.count() == 8u, "entry count");
    for (u32 i = 0; i < 8u; ++i) require(names.name(i) == expected[i], "entry order at " + std::to_string(i));
    require(names.is_directory(2) && names.entry(2).end == 5u && names.entry(3).parent == 2u, "nesting");
    require(names.entry(0).end == 8u && names.entry(1).end == 2u, "subtree ends");
    require(names.entry(1).name_offset == 0u, "the first child's name is at offset 0");
    require(names.names() == std::string("_x\0A\0Y\0z\0a.txt\0B.BIN\0b.bin\0", 27), "name pool layout");
    require(names.reservation_bytes() == 8u * 12u + names.names().size(), "reservation");
    require(names.file_count() == 6u && names.directory_count() == 2u && names.total_bytes() == 21u, "totals");
    require(names.path(3) == "A/Y", "path");
}

void grammar() {
    AssetNamespace names;
    std::string error;
    require(AssetNamespace::from_tree(dir("", {dir("Data", {file("a.bin", 1), dir("Sub", {file("x", 1)})}),
                                              file("x", 1), file("readme.TXT", 1)}),
                                      names, error),
            error);
    const auto at = [&](const char* path) { return names.resolve(path); };
    const gekko::s32 data = at("data"), a = at("DATA/A.BIN"), sub = at("Data/Sub"), x = at("x");
    require(data > 0 && a > 0 && sub > 0 && x > 0, "lookup");
    require(at("") == 0 && at("/") == 0 && at(".") == 0 && at("..") == 0 && at("//") == 0, "root forms");
    require(at("Data/") == data && at("Data/.") == data && at("Data/Sub/..") == data, "dots");
    require(at("Data/../x") == x && at("Data//x") == x && at("/Data/Sub/../../x") == x, "restarts and parents");
    require(at("x/") == AssetNamespace::kNotFound, "a file followed by '/' is not a directory");
    require(at("Data") == data, "a final name may be a directory");
    require(at("readme.txt") > 0 && at("README.TXT") == at("readme.txt"), "folded names");
    require(at("readme.tx") == AssetNamespace::kNotFound && at("readme.txtx") == AssetNamespace::kNotFound, "lengths");
    require(at("nope/x") == AssetNamespace::kNotFound && at("Data/nope") == AssetNamespace::kNotFound, "misses");
    require(names.resolve(std::string("x\0junk", 6)) == x, "the path ends at its NUL");
    require(at("...") == AssetNamespace::kNotFound && at(".x") == AssetNamespace::kNotFound, "dot names");
}

void refusals() {
    AssetNamespace names;
    std::string error;
    require(!AssetNamespace::from_tree(file("x", 1), names, error), "a file root was accepted");
    require(!AssetNamespace::from_tree(dir("", {file("a/b", 1)}), names, error), "a '/' in a name was accepted");
    std::vector<u8> table(24u + 2u, 0u);
    table[0] = 0x01;  // root directory
    table[11] = 2u;   // two entries
    table[12] = 0x01; // entry 1: a directory ...
    table[19] = 7u;   // ... whose parent field is 7 (not its nesting)
    table[23] = 2u;
    table[24] = 'd';
    require(!gekko::test_support::namespace_from_fst(table, names, error),
            "a directory with a foreign parent was accepted");
    // The production ordered builder: refuses what is not a tree, keeps the
    // order it is given.
    using Pre = AssetNamespace::PreorderEntry;
    require(!AssetNamespace::from_preorder({Pre{"d", true, 3u, 0u}}, names, error),
            "a directory reaching past its parent was accepted");
    require(!AssetNamespace::from_preorder({Pre{"a/b", false, 0u, 1u}}, names, error),
            "a '/' in a pre-order name was accepted");
    require(AssetNamespace::from_preorder({Pre{"d", true, 3u, 0u}, Pre{"x", false, 0u, 7u}}, names, error) &&
                names.resolve("d/x") == 2 && names.entry(2).size == 7u && names.entry(2).parent == 1u,
            "a valid pre-order list: " + error);
    require(AssetNamespace::from_preorder({Pre{"b", false, 0u, 1u}, Pre{"a", false, 0u, 2u}}, names, error) &&
                names.name(1) == "b" && names.name(2) == "a",
            "the pre-order builder reordered its entries");
    // A host tree with a non-ASCII name is refused, never transcoded.
    const auto root = gekko::test_support::scratch_directory("gekko-namespace-refusal");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    std::ofstream(root / std::filesystem::u8path(u8"été.bin")).put('x');
    require(!shadow::backends::namespace_from_host_directory(root, names, error), "a non-ASCII host name was accepted");
    std::filesystem::remove_all(root, ec);
}

// The host tree against the disc's FST, and the oracle's publication against
// the old publication.
void disc_oracle(const std::filesystem::path& game) {
    const std::vector<u8> fst = read_file(game / "sys" / "fst.bin");
    require(fst.size() > 12u, "missing sys/fst.bin");
    AssetNamespace host, oracle;
    std::string error;
    require(shadow::backends::namespace_from_host_directory(game / "files", host, error), error);
    require(gekko::test_support::namespace_from_fst(fst, oracle, error), error);
    require(host.count() == oracle.count(), "the host tree has " + std::to_string(host.count()) +
                                                " entries, the disc " + std::to_string(oracle.count()));
    for (u32 i = 0; i < host.count(); ++i) {
        const AssetNamespace::Entry& h = host.entry(i);
        const AssetNamespace::Entry& o = oracle.entry(i);
        require(host.name(i) == oracle.name(i) && h.directory == o.directory && h.parent == o.parent &&
                    h.end == o.end && h.size == o.size && h.name_offset == o.name_offset,
                "entry " + std::to_string(i) + " (" + host.path(i) + ") differs from the disc's");
    }
    const u32 count = be32(fst, 8u);
    require(host.names() == std::string(reinterpret_cast<const char*>(fst.data()) + count * 12u,
                                        fst.size() - count * 12u),
            "the name layout differs from the disc's string table");
    require(host.reservation_bytes() == fst.size(), "the reservation differs from the disc's directory size");

    gekko::assets::AssetSystem mounted;
    require(shadow::backends::mount_host_directory(mounted, game / "files", std::vector<u8>(0x20u, 0u), error), error);
    // The old publication: the table, every file's word1 replaced by its token.
    std::vector<u8> expected = fst;
    u32 previous = 0;
    for (u32 i = 1; i < count; ++i) {
        if ((be32(fst, i * 12u) & 0xFF000000u) != 0u) continue;
        const u32 token = mounted.token_of(i);
        require(token > previous, "tokens are not assigned in entry order");
        previous = token;
        expected[i * 12u + 4u] = static_cast<u8>(token >> 24);
        expected[i * 12u + 5u] = static_cast<u8>(token >> 16);
        expected[i * 12u + 6u] = static_cast<u8>(token >> 8);
        expected[i * 12u + 7u] = static_cast<u8>(token);
    }
    require(gekko::test_support::fst_publication(mounted) == expected,
            "the oracle's publication differs from the old publication");
    require(mounted.report().files == host.file_count() && mounted.report().files == 3480u, "file count");
    std::printf("disc oracle: %u entries (%u files, %u directories, %llu bytes) identical\n", host.count(),
                host.file_count(), host.directory_count(), static_cast<unsigned long long>(host.total_bytes()));
}

u32 le32(const std::vector<u8>& b, std::size_t at) {
    return static_cast<u32>(b[at]) | (static_cast<u32>(b[at + 1]) << 8) | (static_cast<u32>(b[at + 2]) << 16) |
           (static_cast<u32>(b[at + 3]) << 24);
}

std::u16string widen(const char* text) {
    std::u16string out;
    for (; *text != '\0'; ++text) out.push_back(static_cast<char16_t>(static_cast<u8>(*text)));
    return out;
}

// A save-message table in the game's layout: count, 20-byte entries (offset
// and four kept words), UTF-16LE texts, then `tail` zero bytes.
std::vector<u8> make_message_table(const std::vector<std::u16string>& texts, std::size_t tail) {
    const std::size_t base = 4u + texts.size() * 20u;
    std::vector<u8> table(base, 0u);
    const auto put = [&table](std::size_t at, u32 v) {
        for (u32 i = 0; i < 4u; ++i) table[at + i] = static_cast<u8>(v >> (8u * i));
    };
    put(0u, static_cast<u32>(texts.size()));
    for (std::size_t i = 0; i < texts.size(); ++i) {
        put(4u + i * 20u, static_cast<u32>(table.size()));
        put(8u + i * 20u, 0x53C1CCu + static_cast<u32>(i) * 1000u);
        put(12u + i * 20u, 0x14u);
        put(20u + i * 20u, 0xFFFFFFFFu);
        for (const char16_t unit : texts[i]) {
            table.push_back(static_cast<u8>(unit));
            table.push_back(static_cast<u8>(unit >> 8));
        }
        table.push_back(0u);
        table.push_back(0u);
    }
    table.resize(table.size() + tail, 0u);
    return table;
}

std::vector<u8> make_glyph_list(const std::string& characters) {
    std::string met = "METRICS1\nTest00\n" + std::to_string(characters.size()) + "\n";
    for (const char c : characters) met += "  " + std::to_string(static_cast<u8>(c)) + "   0   0  11  23 #\n";
    return std::vector<u8>(met.begin(), met.end());
}

// gekko/native_save_text.h over a synthetic table: an unchanged rewrite is
// the table byte for byte; a rewrite changes only the named texts and the
// offsets that follow them; a glyph the atlas lacks refuses the whole rewrite.
void save_text_rewrite() {
    using gekko::save_text::Message;
    const std::vector<u8> table = make_message_table({u"First", u"Memory Card", u"Last\u3000"}, 12u);
    const std::vector<u8> glyphs = make_glyph_list(" .CDOPSaentv");
    std::vector<u8> out;
    std::string error;
    const bool unchanged_ok = gekko::save_text::rewrite_table(table, glyphs, {}, out, error);
    require(unchanged_ok && out == table,
            "an unchanged rewrite must reproduce the table byte for byte: " + error);
    const std::vector<Message> pc = {{1u, "Save Data.\nOn PC"}};
    const bool rewritten = gekko::save_text::rewrite_table(table, glyphs, pc, out, error);
    require(rewritten, "the rewrite: " + error);
    std::u16string text;
    require(gekko::save_text::message_text(out, 1u, text) && text == widen("Save Data.\nOn PC"), "the new text");
    require(gekko::save_text::message_text(out, 0u, text) && text == u"First" &&
                gekko::save_text::message_text(out, 2u, text) && text == u"Last\u3000",
            "the other texts are kept");
    for (std::size_t i = 0; i < 3u; ++i) {
        require(std::equal(table.begin() + 8 + static_cast<std::ptrdiff_t>(i) * 20,
                           table.begin() + 24 + static_cast<std::ptrdiff_t>(i) * 20,
                           out.begin() + 8 + static_cast<std::ptrdiff_t>(i) * 20),
                "every entry's other words are kept");
    }
    require(std::all_of(out.end() - 12, out.end(), [](u8 b) { return b == 0u; }) &&
                out.size() == table.size() + 2u * (16u - 11u),
            "the tail is kept after the longer text");
    require(!gekko::save_text::rewrite_table(table, glyphs, {{1u, "Zone"}}, out, error) &&
                error.find("glyph") != std::string::npos,
            "a glyph the atlas lacks must refuse the rewrite");
    require(!gekko::save_text::rewrite_table(table, glyphs, {{3u, "Data"}}, out, error),
            "an id outside the table must refuse the rewrite");
}

// The game's own tables: every language's unchanged rewrite is byte-exact, and
// the PC English rewrite fits the English atlas, changes exactly option A's
// GameCube messages and nothing else.
void save_text_on_disc(const std::filesystem::path& game) {
    const std::filesystem::path save = game / "files" / "FONTS" / "SAVE";
    std::string error;
    std::vector<u8> out;
    for (const char* language : {"EN", "FR", "GE", "IT", "SP", "JP"}) {
        const std::vector<u8> table = read_file(save / (std::string("Save_") + language + ".fnt"));
        const std::vector<u8> glyphs = read_file(save / (std::string("Save_") + language + "00.met"));
        const bool language_ok = gekko::save_text::rewrite_table(table, glyphs, {}, out, error);
        require(language_ok && out == table,
                std::string("the unchanged ") + language + " table must rewrite byte for byte: " + error);
    }
    const std::vector<u8> english = read_file(save / "Save_EN.fnt");
    const std::vector<u8> glyphs = read_file(save / "Save_EN00.met");
    const auto& messages = gekko::save_text::pc_english_messages();
    const bool english_ok = gekko::save_text::rewrite_table(english, glyphs, messages, out, error);
    require(english_ok,
            "the PC English messages must fit the English atlas: " + error);
    const u32 count = le32(english, 0u);
    for (u32 id = 0; id < count; ++id) {
        std::u16string before;
        std::u16string after;
        require(gekko::save_text::message_text(english, id, before) && gekko::save_text::message_text(out, id, after),
                "message " + std::to_string(id) + " reads back");
        const auto replaced = std::find_if(messages.begin(), messages.end(),
                                           [id](const gekko::save_text::Message& m) { return m.id == id; });
        if (replaced != messages.end()) {
            require(id >= 80u && id <= 126u, "only GameCube messages are reworded");
            require(after == widen(replaced->text), "message " + std::to_string(id) + " carries the PC text");
        } else {
            require(after == before, "message " + std::to_string(id) + " is unchanged");
        }
        require(std::equal(english.begin() + 8 + static_cast<std::ptrdiff_t>(id) * 20,
                           english.begin() + 24 + static_cast<std::ptrdiff_t>(id) * 20,
                           out.begin() + 8 + static_cast<std::ptrdiff_t>(id) * 20),
                "message " + std::to_string(id) + " keeps its other words");
    }
    // No GameCube message may still speak of the console's hardware.
    for (u32 id = 80u; id <= 126u; ++id) {
        std::u16string text;
        gekko::save_text::message_text(out, id, text);
        for (const char16_t* word : {u"Memory Card", u"Slot A", u"POWER", u"format"}) {
            require(text.find(word) == std::u16string::npos,
                    "message " + std::to_string(id) + " still mentions the console's card");
        }
    }

    // The front-end tables: an empty rewording is byte-exact in every
    // language; the English one rewords each copy of a reworded message once,
    // within its own atlas, and no copy of a GameCube message keeps card words.
    const std::filesystem::path front = game / "files" / "FONTS" / "ADVERTISE";
    u32 reworded = 0;
    for (const char* language : {"EN", "FR", "GE", "IT", "SP", "JP"}) {
        const std::vector<u8> table = read_file(front / (std::string("Advertise_") + language + ".fnt"));
        const std::vector<u8> front_glyphs = read_file(front / (std::string("Advertise_") + language + "00.met"));
        const bool language_ok = gekko::save_text::reword_table(table, front_glyphs, {}, out, reworded, error);
        require(language_ok && out == table && reworded == 0u,
                std::string("the unchanged front-end ") + language + " table must rewrite byte for byte: " + error);
    }
    const std::vector<u8> front_english = read_file(front / "Advertise_EN.fnt");
    std::vector<gekko::save_text::Rewording> rewordings;
    const bool rewordings_ok = gekko::save_text::rewordings_of(english, messages, rewordings, error);
    require(rewordings_ok, "the rewordings: " + error);
    const bool front_ok = gekko::save_text::reword_table(front_english, read_file(front / "Advertise_EN00.met"),
                                                         rewordings, out, reworded, error);
    require(front_ok, "the PC English messages must fit the front-end atlas: " + error);
    require(reworded == messages.size(),
            "each reworded message has exactly one front-end copy (" + std::to_string(reworded) + " reworded)");
    std::vector<std::u16string> gamecube_originals;
    for (u32 id = 80u; id <= 126u; ++id) {
        std::u16string text;
        gekko::save_text::message_text(english, id, text);
        gamecube_originals.push_back(text);
    }
    for (u32 id = 0; id < le32(front_english, 0u); ++id) {
        std::u16string before;
        std::u16string after;
        gekko::save_text::message_text(front_english, id, before);
        gekko::save_text::message_text(out, id, after);
        if (std::find(gamecube_originals.begin(), gamecube_originals.end(), before) == gamecube_originals.end()) {
            require(after == before, "front-end message " + std::to_string(id) + " is not a save message: unchanged");
            continue;
        }
        for (const char16_t* word : {u"Memory Card", u"Slot A", u"POWER", u"format"}) {
            require(after.find(word) == std::u16string::npos,
                    "front-end message " + std::to_string(id) + " still mentions the console's card");
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    const std::pair<const char*, void (*)()> tests[] = {
        {"ordering and layout", ordering_and_layout},
        {"path grammar", grammar},
        {"refusals", refusals},
        {"save text rewrite", save_text_rewrite},
    };
    for (const auto& test : tests) {
        try {
            test.second();
            std::printf("PASS %s\n", test.first);
        } catch (const std::exception& e) {
            ++g_failed;
            std::printf("FAIL %s: %s\n", test.first, e.what());
        }
    }
    if (argc > 1) {
        try {
            disc_oracle(std::filesystem::path(argv[1]).parent_path().parent_path());
            std::printf("PASS the host tree reproduces the disc's namespace\n");
        } catch (const std::exception& e) {
            ++g_failed;
            std::printf("FAIL disc oracle: %s\n", e.what());
        }
        try {
            save_text_on_disc(std::filesystem::path(argv[1]).parent_path().parent_path());
            std::printf("PASS the PC save text rewrites the game's English table\n");
        } catch (const std::exception& e) {
            ++g_failed;
            std::printf("FAIL save text on disc: %s\n", e.what());
        }
    }
    return g_failed == 0 ? 0 : 1;
}
