// Tests of the game's save file on the host (gekko/save_file.h): DES and MD5 against their standard vectors, the
// GCI round trip and atomic replacement, extraction from a synthesised memory-card image (the current directory copy
// by update counter, the block chain), and the record check on an encrypted record built here. With arguments -
// paths of real saves (a .gci or a card image) - each is checked read-only and its record's options printed.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gekko/save_file.h"
#include "gekko/save_import.h"
#include "test_scratch.h"

namespace {

namespace sf = gekko::save_file;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::string hex(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15];
    return s;
}

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v >> 8);
    b[off + 1] = static_cast<std::uint8_t>(v);
}

void sums(const std::vector<std::uint8_t>& b, std::size_t off, std::size_t n, std::uint16_t* a, std::uint16_t* c) {
    std::uint32_t t = 0, k = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        const std::uint16_t w = static_cast<std::uint16_t>(b[off + i] << 8 | b[off + i + 1]);
        t = (t + w) & 0xFFFFu;
        k = (k + (w ^ 0xFFFFu)) & 0xFFFFu;
    }
    *a = static_cast<std::uint16_t>(t == 0xFFFFu ? 0 : t);
    *c = static_cast<std::uint16_t>(k == 0xFFFFu ? 0 : k);
}

void primitives() {
    // DES: the classic worked example (key 133457799BBCDFF1, 0123456789ABCDEF -> 85E813540F0AB405).
    const std::uint8_t key[8] = {0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1};
    std::uint8_t block[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    sf::des_ecb_encrypt(key, block, 8);
    check(hex(block, 8) == "85e813540f0ab405", "DES encrypts the standard vector");
    sf::des_ecb_decrypt(key, block, 8);
    check(hex(block, 8) == "0123456789abcdef", "DES decrypts it back");

    // MD5: RFC 1321 vectors, including a message that spills into a second block.
    check(hex(sf::md5(nullptr, 0).data(), 16) == "d41d8cd98f00b204e9800998ecf8427e", "MD5 of nothing");
    const std::string abc = "abc";
    check(hex(sf::md5(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()).data(), 16) ==
              "900150983cd24fb0d6963f7d28e17f72",
          "MD5 of abc");
    const std::string digits = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    check(hex(sf::md5(reinterpret_cast<const std::uint8_t*>(digits.data()), digits.size()).data(), 16) ==
              "57edf4a22be3c955ac49da2e2107b67a",
          "MD5 of eighty digits");
}

// A 16 KiB file holding a valid, encrypted record with the given options.
std::vector<std::uint8_t> game_file(std::uint32_t language, std::uint32_t speaker, std::uint8_t vibe) {
    std::vector<std::uint8_t> data(2 * sf::kBlockBytes, 0);
    std::vector<std::uint8_t> rec(sf::kRecordBytes, 0);
    for (std::size_t i = 0x30; i < rec.size(); ++i) rec[i] = static_cast<std::uint8_t>(i * 7u);
    const auto be32 = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) rec[off + i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
    };
    be32(0x18, language);
    be32(0x1C, 1);
    be32(0x20, speaker);
    rec[0x24] = vibe;
    be32(0x1774, 0x0002A8CBu);
    be32(0x1778, 0);
    const std::array<std::uint8_t, 16> sum = sf::md5(rec.data() + 0x10, rec.size() - 0x10, 0x00);
    std::copy(sum.begin(), sum.end(), rec.begin());
    static constexpr std::uint8_t kKey[8] = {0xEE, 0xB0, 0x6C, 0xAE, 0xA4, 0x78, 0x49, 0x90};
    sf::des_ecb_encrypt(kKey, rec.data(), 0x1F58);
    std::copy(rec.begin(), rec.end(), data.begin() + sf::kRecordOffset);
    return data;
}

void record_and_gci(const std::filesystem::path& dir) {
    const std::vector<std::uint8_t> data = game_file(3, 2, 1);
    sf::RecordCheck r = sf::check_record(data);
    check(r.valid && r.text_language == 3 && r.speaker_mode == 2 && r.vibration == 1 && r.stamp == 0x0002A8CB00000000ull,
          "the record decrypts, checks and reads");
    std::vector<std::uint8_t> broken = data;
    broken[sf::kRecordOffset + 0x100] ^= 1u;
    check(!sf::check_record(broken).valid, "a changed byte fails the check");

    sf::Gci gci;
    const char* name = "SHADOWTHEHEDGEHOG";
    std::memcpy(gci.header.data(), "GUPP8P", 6);
    gci.header[6] = 0xFF;
    std::memcpy(gci.header.data() + 8, name, std::strlen(name));
    gci.header[0x38] = 0;
    gci.header[0x39] = 2;
    gci.data = data;
    const std::filesystem::path path = dir / "save_file_tests.gci";
    std::string error;
    check(sf::write_gci(path, gci, &error), "a GCI is written");
    check(!std::filesystem::exists(path.string() + ".tmp"), "no temporary is left");
    check(std::filesystem::file_size(path) == 0x40u + 0x4000u, "64 bytes + 16 KiB");
    sf::Gci back;
    check(sf::read_gci(path, back, &error) && back.header == gci.header && back.data == gci.data, "and read back");
    check(back.game_code() == "GUPP" && back.maker_code() == "8P" && back.file_name() == name &&
              back.block_count() == 2,
          "its directory entry");
    // Replacing keeps the file whole: a second write over it.
    gci.data = game_file(1, 0, 0);
    check(sf::write_gci(path, gci, &error) && sf::read_gci(path, back, &error) &&
              sf::check_record(back.data).text_language == 1,
          "a replacement is read back");
    {
        std::ofstream cut(path, std::ios::binary | std::ios::trunc);
        cut.write(reinterpret_cast<const char*>(gci.header.data()), 0x40);
    }
    check(!sf::read_gci(path, back, &error), "a GCI shorter than its blocks is refused");
    std::filesystem::remove(path);
}

std::vector<std::uint8_t> make_card(const std::vector<std::uint8_t>& data);

void card_image() {
    const std::vector<std::uint8_t> data = game_file(5, 1, 0);
    std::vector<std::uint8_t> card = make_card(data);
    const auto block = [&](std::size_t i) { return i * sf::kBlockBytes; };
    sf::Gci gci;
    std::string error;
    check(sf::extract_from_card(card, "GUPP", "8P", "SHADOWTHEHEDGEHOG", gci, &error), "the file is found");
    check(gci.data == data && gci.block_count() == 2, "from the current directory, blocks in chain order");
    check(sf::check_record(gci.data).valid && sf::check_record(gci.data).text_language == 5, "its record checks");
    check(!sf::extract_from_card(card, "GUPE", "8P", "SHADOWTHEHEDGEHOG", gci, &error), "another region's is absent");
    card[block(2) + 0x100] ^= 1u;  // the current directory's checksum breaks: the older copy is used
    check(sf::extract_from_card(card, "GUPP", "8P", "SHADOWTHEHEDGEHOG", gci, &error) && gci.data != data,
          "a broken directory copy falls back to the other");
    std::vector<std::uint8_t> small(4 * sf::kBlockBytes, 0);
    check(!sf::extract_from_card(small, "GUPP", "8P", "SHADOWTHEHEDGEHOG", gci, &error), "not a card");
}

// A 32-block card: header, directory copies 1 (older) and 2 (current), allocation tables 3 (current) and 4. The
// current directory lists the game's file at blocks 9 -> 7 holding `data`; the stale copy lists it at 5 -> 6 (zeros).
std::vector<std::uint8_t> make_card(const std::vector<std::uint8_t>& data) {
    const std::size_t blocks = 32;
    std::vector<std::uint8_t> card(blocks * sf::kBlockBytes, 0);
    const auto block = [&](std::size_t i) { return i * sf::kBlockBytes; };
    for (std::size_t i = 1; i <= 2; ++i)
        std::fill_n(card.begin() + static_cast<std::ptrdiff_t>(block(i)), 0x1FC0, std::uint8_t{0xFF});  // free entries
    // The current directory (block 2, counter 5) lists the game's file at blocks 9 -> 7; the stale copy (block 1,
    // counter 4) lists it at blocks 5 -> 6 (wrong data there).
    const auto entry = [&](std::size_t dir, std::uint16_t first) {
        std::uint8_t* e = card.data() + block(dir);
        std::memcpy(e, "GUPP8P", 6);
        e[6] = 0xFF;
        e[7] = 2;
        std::memset(e + 8, 0, 32);
        std::memcpy(e + 8, "SHADOWTHEHEDGEHOG", 17);
        put16(card, block(dir) + 0x36, first);
        put16(card, block(dir) + 0x38, 2);
    };
    entry(1, 5);
    entry(2, 9);
    put16(card, block(1) + 0x1FFA, 4);
    put16(card, block(2) + 0x1FFA, 5);
    for (std::size_t d = 1; d <= 2; ++d) {
        std::uint16_t a = 0, b = 0;
        sums(card, block(d), 0x1FFC, &a, &b);
        put16(card, block(d) + 0x1FFC, a);
        put16(card, block(d) + 0x1FFE, b);
    }
    for (std::size_t t = 3; t <= 4; ++t) {
        put16(card, block(t) + 4, t == 3 ? 9 : 8);  // update counter
        put16(card, block(t) + 0x0A + (9 - 5) * 2, 7);
        put16(card, block(t) + 0x0A + (7 - 5) * 2, 0xFFFF);
        put16(card, block(t) + 0x0A + (5 - 5) * 2, 6);
        put16(card, block(t) + 0x0A + (6 - 5) * 2, 0xFFFF);
        std::uint16_t a = 0, b = 0;
        sums(card, block(t) + 4, sf::kBlockBytes - 4, &a, &b);
        put16(card, block(t), a);
        put16(card, block(t) + 2, b);
    }
    std::copy_n(data.begin(), sf::kBlockBytes, card.begin() + static_cast<std::ptrdiff_t>(block(9)));
    std::copy_n(data.begin() + sf::kBlockBytes, sf::kBlockBytes, card.begin() + static_cast<std::ptrdiff_t>(block(7)));
    return card;
}

void write_bytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void import(const std::filesystem::path& base) {
    namespace si = gekko::save_import;
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    const std::filesystem::path root = base / "root", save_data = base / "save-data";
    const std::filesystem::path gci_path = root / "GC" / "EUR" / "Card A" / "8P-GUPP-SHADOWTHEHEDGEHOG.gci";

    // No card at all: a new game's file name, nothing to import.
    si::Paths p = si::resolve(std::nullopt, std::nullopt, root, save_data);
    check(p.save == save_data / "ShadowTheHedgehog.gci" && !p.card_source, "no card: ShadowTheHedgehog.gci");
    si::Result r = si::import_if_missing(p);
    check(!r.imported && !std::filesystem::exists(p.save), "no card: nothing imported");

    // The default GCI (language 2) and its working image (language 4, the progress the game played on).
    sf::Gci gci;
    std::memcpy(gci.header.data(), "GUPP8P", 6);
    std::memcpy(gci.header.data() + 8, "SHADOWTHEHEDGEHOG", 17);
    gci.header[0x39] = 2;
    gci.data = game_file(2, 0, 1);
    std::string error;
    std::filesystem::create_directories(gci_path.parent_path());
    check(sf::write_gci(gci_path, gci, &error), "the default GCI is placed");
    const std::vector<std::uint8_t> gci_bytes = read_bytes(gci_path);
    write_bytes(save_data / "8P-GUPP-SHADOWTHEHEDGEHOG.pc.raw", make_card(game_file(4, 2, 0)));
    const std::vector<std::uint8_t> image_bytes = read_bytes(save_data / "8P-GUPP-SHADOWTHEHEDGEHOG.pc.raw");
    p = si::resolve(std::nullopt, std::nullopt, root, save_data);
    check(p.card_source && *p.card_source == gci_path, "the default GCI is the card source");
    check(p.save == save_data / "8P-GUPP-SHADOWTHEHEDGEHOG.gci", "its save file: save-data/<stem>.gci");
    r = si::import_if_missing(p);
    check(r.imported && r.has_options && r.text_language == 4 && r.speaker_mode == 2 && !r.vibration,
          "imported from the working image (the progress played on)");
    sf::Gci back;
    check(sf::read_gci(p.save, back, &error) && sf::check_record(back.data).text_language == 4, "the file reads");
    check(std::filesystem::exists(p.save.string() + ".source"), "with its .source note");
    check(read_bytes(gci_path) == gci_bytes &&
              read_bytes(save_data / "8P-GUPP-SHADOWTHEHEDGEHOG.pc.raw") == image_bytes,
          "the sources are untouched");
    r = si::import_if_missing(p);
    check(r.existed && !r.imported, "a second start reads nothing");

    // A broken working image: the GCI is used.
    std::filesystem::remove(p.save);
    write_bytes(save_data / "8P-GUPP-SHADOWTHEHEDGEHOG.pc.raw", std::vector<std::uint8_t>(5 * sf::kBlockBytes, 0));
    r = si::import_if_missing(p);
    check(r.imported && r.text_language == 2 && r.source == gci_path.string(), "falls back to the GCI");

    // --memory-card <a card image>: the save beside it ("card.raw" -> "card.gci").
    const std::filesystem::path raw = base / "run" / "card.raw";
    write_bytes(raw, make_card(game_file(5, 1, 1)));
    p = si::resolve(std::nullopt, raw, root, save_data);
    check(p.save == base / "run" / "card.gci" && p.working_image.empty(), "a card image's save sits beside it");
    r = si::import_if_missing(p);
    check(r.imported && r.text_language == 5, "and is imported from it");

    // A source the derived path would be: never written ("-pc").
    const std::filesystem::path inside = save_data / "mine.gci";
    std::filesystem::copy_file(gci_path, inside, std::filesystem::copy_options::overwrite_existing);
    p = si::resolve(std::nullopt, inside, root, save_data);
    check(p.save == save_data / "mine-pc.gci", "a GCI already in save-data gets -pc");
    // Another region's save: the file the save store can find (game GUPP), the data byte for byte.
    for (const char* region : {"GUPE8P", "GUPJ8P"}) {
        sf::Gci other;
        std::memcpy(other.header.data(), region, 6);
        std::memcpy(other.header.data() + 8, "SHADOWTHEHEDGEHOG", 17);
        other.header[0x39] = 2;
        other.header[0x28] = 0x12;
        other.header[0x2B] = 0x34;
        other.header[0x30] = 0x55;  // a value the conversion normalises
        other.data = game_file(3, 1, 1);
        const std::filesystem::path source = base / (std::string(region) + ".gci");
        check(sf::write_gci(source, other, &error), "the other region's GCI is placed");
        const std::vector<std::uint8_t> source_bytes = read_bytes(source);
        const std::filesystem::path out = base / (std::string(region) + "-pc.gci");
        p = si::resolve(out, source, root, save_data);
        r = si::import_if_missing(p);
        sf::Gci converted;
        check(r.imported && sf::read_gci(out, converted, &error) && converted.game_code() == "GUPP" &&
                  converted.maker_code() == "8P" && converted.file_name() == "SHADOWTHEHEDGEHOG",
              "an other-region GCI is imported as this build's save");
        check(converted.data == other.data, "its data is byte for byte");
        check(converted.header[0x28] == 0x12 && converted.header[0x2B] == 0x34 && converted.header[0x30] == 0x00,
              "the modified time is kept, the entry normalised");
        check(read_bytes(source) == source_bytes, "the source is untouched");
        check(r.message.find("converted from") != std::string::npos, "the message says it was converted");

        // A save an earlier build imported unconverted is repaired, with a backup, once.
        check(sf::write_gci(out, other, &error), "an old unconverted import is placed");
        const std::vector<std::uint8_t> old_bytes = read_bytes(out);
        r = si::import_if_missing(p);
        check(r.existed && sf::read_gci(out, converted, &error) && converted.game_code() == "GUPP" &&
                  converted.data == other.data, "the old import is repaired");
        check(read_bytes(out.string() + ".before-region-fix") == old_bytes, "with a backup of the old file");
        const std::vector<std::uint8_t> repaired = read_bytes(out);
        r = si::import_if_missing(p);
        check(read_bytes(out) == repaired, "a second start changes nothing");

        // A broken record is refused, not rewritten.
        sf::Gci broken = other;
        broken.data[0x2040 + 0x100] ^= 0x01;
        const std::filesystem::path bad = base / (std::string(region) + "-bad.gci");
        check(sf::write_gci(bad, broken, &error), "a corrupt save is placed");
        const std::vector<std::uint8_t> bad_bytes = read_bytes(bad);
        p = si::resolve(bad, std::nullopt, root, save_data);
        r = si::import_if_missing(p);
        check(read_bytes(bad) == bad_bytes && r.message.find("converted") == std::string::npos,
              "a corrupt old import stays as it is");
    }
    // --save-file wins.
    p = si::resolve(base / "elsewhere.gci", std::nullopt, root, save_data);
    check(p.save == base / "elsewhere.gci" && p.card_source && *p.card_source == gci_path, "--save-file names it");
    std::filesystem::remove_all(base, ec);
}

void real_save(const std::filesystem::path& path) {
    sf::Gci gci;
    std::string error;
    bool ok = false;
    if (path.extension() == ".gci") {
        ok = sf::read_gci(path, gci, &error);
    } else {
        std::ifstream in(path, std::ios::binary);
        const std::vector<std::uint8_t> card((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ok = sf::extract_from_card(card, "GUPP", "8P", "SHADOWTHEHEDGEHOG", gci, &error);
    }
    if (!ok) {
        std::printf("%s: %s\n", path.string().c_str(), error.c_str());
        return;
    }
    const sf::RecordCheck r = sf::check_record(gci.data);
    std::printf("%s: %s, language %u, voice %u, speaker %u, vibration %u, stamp %016llx\n", path.string().c_str(),
                r.valid ? "valid" : "INVALID", r.text_language, r.voice_language, r.speaker_mode, r.vibration,
                static_cast<unsigned long long>(r.stamp));
    check(r.valid, "a real save's record checks");
}

}  // namespace

int main(int argc, char** argv) {
    primitives();
    {
        const auto gci_dir = gekko::test_support::scratch_directory("save_file_tests_gci");
        std::filesystem::create_directories(gci_dir);
        record_and_gci(gci_dir);
        std::error_code ec;
        std::filesystem::remove_all(gci_dir, ec);
    }
    card_image();
    import(gekko::test_support::scratch_directory("save_file_tests_import"));
    for (int i = 1; i < argc; ++i) real_save(argv[i]);
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("save_file_tests: all passed\n");
    return 0;
}
