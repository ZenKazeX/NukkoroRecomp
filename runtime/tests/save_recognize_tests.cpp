// Tests of the universal save recognizer (gekko/save_recognize.h, docs/SAVE_FORMATS.md).
//
// Without arguments (CI): the decoders against reference vectors (CRC-32, raw deflate streams made by zlib, LZARI
// round trips) and every container built here - GameCube GCI and card image in all three regions, Action Replay
// MAX, SharkPort, PSV (sound and with a broken file table), CodeBreaker, an Xbox Save.dat in a ZIP and on disk -
// identified, unwrapped, and (GameCube only) converted to the PC GCI; scan() over a folder of them.
//
// With arguments - sample files or folders of them, read only: each file is identified and printed as a table row.
// A file whose name is one of the user's 19 samples (2026-11-05) is also checked against what docs/SAVE_FORMATS.md
// records for it; a GameCube save is converted in memory and the result checked (nothing is written next to it).

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "gekko/save_file.h"
#include "gekko/save_recognize.h"
#include "test_scratch.h"

namespace {

namespace sf = gekko::save_file;
namespace sr = gekko::save_recognize;
using Bytes = std::vector<std::uint8_t>;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

Bytes text_bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
Bytes from_hex(const char* h) {
    Bytes b;
    for (std::size_t i = 0; h[i] != 0 && h[i + 1] != 0; i += 2) {
        const auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        b.push_back(static_cast<std::uint8_t>(nib(h[i]) << 4 | nib(h[i + 1])));
    }
    return b;
}
void le32(Bytes& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void le16(Bytes& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}
void be16(Bytes& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v >> 8);
    b[off + 1] = static_cast<std::uint8_t>(v);
}
void append_le32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_text(Bytes& b, std::size_t off, const std::string& s) { std::memcpy(b.data() + off, s.data(), s.size()); }

// A deterministic pseudo-random fill (xorshift), so "encrypted" payloads look like ciphertext.
void noise(Bytes& b, std::size_t from, std::size_t to, std::uint32_t seed) {
    std::uint32_t x = seed | 1u;
    for (std::size_t i = from; i < to; ++i) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        b[i] = static_cast<std::uint8_t>(x);
    }
}

// ---- Decoders --------------------------------------------------------------------------------------------------------

void decoders() {
    const Bytes digits = text_bytes("123456789");
    check(sr::crc32(digits) == 0xCBF43926u, "CRC-32 of 123456789");
    check(sr::crc32({}) == 0u, "CRC-32 of nothing");

    // Raw deflate streams produced by zlib (Python zlib.compressobj(level, DEFLATED, -15)).
    const Bytes title = text_bytes("Shadow the Hedgehog");
    Bytes out;
    std::string error;
    check(sr::inflate_raw(from_hex("011300ecff536861646f7720746865204865646765686f67"), out, title.size(), &error) &&
              out == title,
          "inflate: a stored block (zlib level 0)");
    check(sr::inflate_raw(from_hex("0bce484cc92f5728c94855f0484d494fcdc84f0700"), out, title.size(), &error) &&
              out == title,
          "inflate: a fixed-Huffman block (zlib level 6)");
    std::string ranks;
    for (int i = 0; i < 24; ++i) {
        char line[32];
        std::snprintf(line, sizeof line, "stage %02d rank %c; ", i, 'A' + i % 5);
        ranks += line;
    }
    const Bytes dynamic = from_hex(
        "5dce390a80401005d1abcc11e6b73b466e0799400c0403f5fe1834b4506905c57bde72ec29e77497eb4cd3981e0ff23047300f4b84ca"
        "c31aa1f6b04568386d39ed38ed391d3015a5a254948a52512a4a45a92815a5a2d428354a8d52fba51f");
    check(sr::inflate_raw(dynamic, out, ranks.size(), &error) && out == text_bytes(ranks) &&
              sr::crc32(out) == 0xADC9B14Au,
          "inflate: a dynamic-Huffman block with matches (zlib level 9)");
    Bytes cut(dynamic.begin(), dynamic.begin() + 40);
    check(!sr::inflate_raw(cut, out, ranks.size(), &error), "inflate: a cut stream fails");
    check(!sr::inflate_raw(dynamic, out, ranks.size() - 1, &error), "inflate: more output than expected fails");

    // LZARI: round trips through the encoder here (the decoder is checked against real Action Replay MAX files
    // when the samples are passed).
    std::vector<Bytes> inputs;
    inputs.push_back({});
    inputs.push_back(text_bytes("a"));
    inputs.push_back(Bytes(10000, 0));
    inputs.push_back(text_bytes(ranks + ranks + ranks));
    Bytes mixed(20000);
    noise(mixed, 0, mixed.size(), 7);
    for (std::size_t i = 4000; i < 9000; ++i) mixed[i] = mixed[i - 1500];  // long-distance repeats
    for (std::size_t i = 12000; i < 13000; ++i) mixed[i] = 0x20;           // the ring buffer's initial byte
    inputs.push_back(mixed);
    for (std::size_t n = 0; n < inputs.size(); ++n) {
        const Bytes enc = sr::lzari_encode(inputs[n]);
        Bytes dec;
        check(sr::lzari_decode(enc, inputs[n].size(), dec, &error) && dec == inputs[n],
              "LZARI round trip " + std::to_string(n));
    }
    const Bytes enc = sr::lzari_encode(mixed);
    Bytes dec;
    check(enc.size() < mixed.size(), "LZARI compresses repeated data");
    check(!sr::lzari_decode(Bytes(enc.begin(), enc.begin() + 100), mixed.size(), dec, &error),
          "LZARI: a cut stream fails");
}

// ---- GameCube fixtures -----------------------------------------------------------------------------------------------

// The game's 16 KiB file with a valid, encrypted record (as gekko/save_file.h describes it).
Bytes gc_data(std::uint32_t language, std::uint32_t seed) {
    Bytes data(2 * sf::kBlockBytes, 0);
    put_text(data, 0, "SHADOW THE HEDGEHOG");
    put_text(data, 0x20, "GAME DATA");
    noise(data, 0x40, 0x2040, seed);  // banner and icon
    Bytes rec(sf::kRecordBytes, 0);
    noise(rec, 0x30, 0x1700, seed + 1);
    rec[0x1B] = static_cast<std::uint8_t>(language);
    rec[0x1F] = 1;
    rec[0x24] = 1;
    rec[0x1774] = 0x12;
    const auto sum = sf::md5(rec.data() + 0x10, rec.size() - 0x10, 0x00);
    std::copy(sum.begin(), sum.end(), rec.begin());
    static constexpr std::uint8_t kKey[8] = {0xEE, 0xB0, 0x6C, 0xAE, 0xA4, 0x78, 0x49, 0x90};
    sf::des_ecb_encrypt(kKey, rec.data(), 0x1F58);
    std::copy(rec.begin(), rec.end(), data.begin() + sf::kRecordOffset);
    return data;
}

Bytes gci_bytes(const char* game, const Bytes& data, std::uint32_t icon_addr = 0x40) {
    Bytes b(0x40, 0);
    put_text(b, 0, game);
    put_text(b, 4, "8P");
    b[6] = 0xFF;
    b[7] = 2;
    put_text(b, 8, "SHADOWTHEHEDGEHOG");
    b[0x28] = 0x23, b[0x29] = 0x74, b[0x2A] = 0xD7, b[0x2B] = 0x7A;  // modified
    for (int i = 0; i < 4; ++i) b[0x2C + i] = static_cast<std::uint8_t>(icon_addr >> (24 - 8 * i));
    be16(b, 0x30, 0x5556);  // odd values a source may carry; the PC GCI normalises them
    be16(b, 0x32, 0xFFF2);
    b[0x34] = 4;
    b[0x35] = 1;
    be16(b, 0x36, 0xCF);
    be16(b, 0x38, static_cast<std::uint16_t>(data.size() / sf::kBlockBytes));
    be16(b, 0x3A, 0xFFFF);
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

void card_sums(const Bytes& b, std::size_t off, std::size_t n, std::uint16_t* a, std::uint16_t* c) {
    std::uint32_t t = 0, k = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        const std::uint16_t w = static_cast<std::uint16_t>(b[off + i] << 8 | b[off + i + 1]);
        t = (t + w) & 0xFFFFu;
        k = (k + (w ^ 0xFFFFu)) & 0xFFFFu;
    }
    *a = static_cast<std::uint16_t>(t == 0xFFFFu ? 0 : t);
    *c = static_cast<std::uint16_t>(k == 0xFFFFu ? 0 : k);
}

// A 16-block card with the game's file (`game`) at blocks 5 -> 6; `with_file` false leaves the directory empty.
Bytes card_image(const char* game, const Bytes& data, bool with_file) {
    Bytes card(16 * sf::kBlockBytes, 0);
    const auto block = [](std::size_t i) { return i * sf::kBlockBytes; };
    for (std::size_t d = 1; d <= 2; ++d) {
        std::fill_n(card.begin() + static_cast<std::ptrdiff_t>(block(d)), 0x1FC0, std::uint8_t{0xFF});
        if (with_file) {
            std::uint8_t* e = card.data() + block(d);
            std::memset(e, 0, 0x40);
            std::memcpy(e, game, 4);
            std::memcpy(e + 4, "8P", 2);
            e[6] = 0xFF;
            std::memcpy(e + 8, "SHADOWTHEHEDGEHOG", 17);
            be16(card, block(d) + 0x36, 5);
            be16(card, block(d) + 0x38, 2);
        }
        be16(card, block(d) + 0x1FFA, static_cast<std::uint16_t>(d));
        std::uint16_t a = 0, b = 0;
        card_sums(card, block(d), 0x1FFC, &a, &b);
        be16(card, block(d) + 0x1FFC, a);
        be16(card, block(d) + 0x1FFE, b);
    }
    for (std::size_t t = 3; t <= 4; ++t) {
        be16(card, block(t) + 4, static_cast<std::uint16_t>(t));
        if (with_file) {
            be16(card, block(t) + 0x0A, 6);
            be16(card, block(t) + 0x0C, 0xFFFF);
        }
        std::uint16_t a = 0, b = 0;
        card_sums(card, block(t) + 4, sf::kBlockBytes - 4, &a, &b);
        be16(card, block(t), a);
        be16(card, block(t) + 2, b);
    }
    std::copy(data.begin(), data.end(), card.begin() + static_cast<std::ptrdiff_t>(block(5)));
    return card;
}

void gamecube() {
    const struct {
        const char* game;
        sr::Region region;
    } regions[] = {{"GUPE", sr::Region::NtscU}, {"GUPP", sr::Region::Pal}, {"GUPJ", sr::Region::NtscJ}};
    std::uint32_t seed = 100;
    for (const auto& r : regions) {
        const Bytes data = gc_data(2, ++seed);
        const Bytes gci = gci_bytes(r.game, data);
        const std::string tag = std::string(r.game) + ": ";
        sr::Identification id = sr::identify_bytes(gci, "x.gci");
        check(id.recognized && id.saves.size() == 1, tag + "a GCI is recognised");
        if (id.saves.size() != 1) continue;
        const sr::Save& s = id.saves[0];
        check(s.platform == sr::Platform::GameCube && s.container == sr::Container::Gci && s.region == r.region &&
                  s.product == r.game,
              tag + "platform, container, region");
        check(s.valid && s.record_state == sr::RecordState::Decoded && s.convertible, tag + "valid and convertible");
        check(id.platform == s.platform && id.region == s.region && id.valid, tag + "the summary is the save's");
        sr::Record rec;
        std::string error;
        check(sr::extract_record(s, rec, &error) && rec.md5_ok && rec.bytes.size() == sf::kRecordBytes &&
                  rec.options.text_language == 2 && rec.options.voice_language == 1 && rec.options.vibration == 1,
              tag + "the record decodes");
        sf::Gci pc;
        check(sr::to_pc_gci(s, pc, &error), tag + "converts to the PC GCI");
        check(pc.game_code() == "GUPP" && pc.maker_code() == "8P" && pc.file_name() == "SHADOWTHEHEDGEHOG" &&
                  pc.block_count() == 2 && pc.data == data && sf::check_record(pc.data).valid,
              tag + "the PC GCI: GUPP/8P, the data byte for byte, the record valid");
        check(pc.header[0x30] == 0 && pc.header[0x31] == 2 && pc.header[0x35] == 0 && pc.header[0x36] == 0 &&
                  pc.header[0x37] == 5 && pc.header[0x28] == 0x23 && pc.header[0x2B] == 0x7A,
              tag + "the directory entry is the save store's, with the source's modified time");
        // The same file in a card image.
        const Bytes card = card_image(r.game, data, true);
        id = sr::identify_bytes(card, "card.raw");
        check(id.recognized && id.outer == sr::Container::GcCardImage && id.saves.size() == 1 &&
                  id.saves[0].region == r.region && id.saves[0].valid && id.saves[0].convertible,
              tag + "found in a card image");
        if (id.saves.size() == 1) {
            check(sr::to_pc_gci(id.saves[0], pc, &error) && pc.data == data, tag + "and converted from it");
        }
    }
    // A record with a changed byte.
    Bytes data = gc_data(1, 7);
    data[sf::kRecordOffset + 0x200] ^= 1u;
    sr::Identification id = sr::identify_bytes(gci_bytes("GUPE", data), "x.gci");
    check(id.recognized && !id.valid && !id.saves[0].convertible &&
              id.saves[0].record_state == sr::RecordState::Decoded,
          "GC: a broken MD5 is recognised, invalid, not convertible");
    sf::Gci pc;
    std::string error;
    check(!sr::to_pc_gci(id.saves[0], pc, &error), "GC: and refused by to_pc_gci");
    // An interrupted create.
    id = sr::identify_bytes(gci_bytes("GUPP", gc_data(1, 8), 0xFFFFFFFFu), "x.gci");
    check(id.recognized && !id.valid && !id.saves[0].convertible, "GC: an interrupted create is not valid");
    // Another game's GCI and another game's region code.
    Bytes other = gci_bytes("GUPE", gc_data(1, 9));
    put_text(other, 0, "GZLE");
    id = sr::identify_bytes(other, "x.gci");
    check(!id.recognized && id.outer == sr::Container::Gci, "GC: another game's GCI is not this game's");
    Bytes unknown_region = gci_bytes("GUPX", gc_data(1, 10));
    id = sr::identify_bytes(unknown_region, "x.gci");
    check(id.recognized && id.region == sr::Region::Unknown && !id.saves[0].convertible,
          "GC: an unknown region letter is unknown, not guessed, and not converted");
    // A card without the game's file.
    id = sr::identify_bytes(card_image("GUPP", gc_data(1, 11), false), "card.raw");
    check(!id.recognized && id.outer == sr::Container::GcCardImage, "GC: a card without the game's file");
}

// ---- PS2 fixtures ----------------------------------------------------------------------------------------------------

struct Ps2File {
    std::string name;
    Bytes data;
};

// The game's PS2 directory: the data file (an encrypted-looking 0x1C40-byte record, zero padding to 0x2000) and an
// icon.sys.
std::vector<Ps2File> ps2_dir(const std::string& dir, std::uint32_t seed) {
    Bytes data(0x2000, 0);
    noise(data, 0, 0x1C40, seed);
    Bytes icon(0x3C4, 0);
    put_text(icon, 0, "PS2D");
    return {{dir, data}, {"icon.sys", icon}};
}

Bytes max_file(const std::string& dir, const std::vector<Ps2File>& files) {
    Bytes stream;
    for (const Ps2File& f : files) {
        const std::size_t at = stream.size();
        stream.resize(at + 0x24, 0);
        le32(stream, at, static_cast<std::uint32_t>(f.data.size()));
        put_text(stream, at + 4, f.name);
        stream.insert(stream.end(), f.data.begin(), f.data.end());
        const std::size_t next = ((stream.size() + 8 + 15) & ~std::size_t{15}) - 8;
        stream.resize(next, 0);
    }
    const Bytes packed = sr::lzari_encode(stream);
    Bytes b(0x5C, 0);
    put_text(b, 0, "Ps2PowerSave");
    put_text(b, 0x10, dir);
    put_text(b, 0x30, "SHADOW THE HEDGEHOG");
    le32(b, 0x50, static_cast<std::uint32_t>(packed.size() + 4));
    le32(b, 0x54, static_cast<std::uint32_t>(files.size()));
    le32(b, 0x58, static_cast<std::uint32_t>(stream.size()));
    b.insert(b.end(), packed.begin(), packed.end());
    le32(b, 0x0C, sr::crc32(b));
    return b;
}

Bytes xps_file(const std::string& dir, const std::vector<Ps2File>& files) {
    Bytes b;
    append_le32(b, 13);
    for (char c : std::string("SharkPortSave")) b.push_back(static_cast<std::uint8_t>(c));
    append_le32(b, 0);
    for (const std::string s : {"SHADOW THE HEDGEHOG", "SHADOW THE HEDGEHOG", "made by save_recognize_tests"}) {
        append_le32(b, static_cast<std::uint32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
    Bytes body(0xFA, 0);
    le16(body, 0, 0xFA);
    put_text(body, 2, dir);
    le32(body, 0x42, static_cast<std::uint32_t>(files.size() + 2));
    for (const Ps2File& f : files) {
        Bytes h(0xFA, 0);
        le16(h, 0, 0xFA);
        put_text(h, 2, f.name);
        le32(h, 0x42, static_cast<std::uint32_t>(f.data.size()));
        body.insert(body.end(), h.begin(), h.end());
        body.insert(body.end(), f.data.begin(), f.data.end());
    }
    append_le32(b, static_cast<std::uint32_t>(body.size()));
    b.insert(b.end(), body.begin(), body.end());
    append_le32(b, 0);  // the trailing four bytes
    return b;
}

Bytes psv_file(const std::string& dir, const std::vector<Ps2File>& files) {
    const std::size_t table = 0xA0 + files.size() * 0x3C;
    Bytes b(table, 0);
    std::memcpy(b.data(), "\0VSP", 4);
    le32(b, 0x38, 0x2C);
    le32(b, 0x3C, 2);
    le32(b, 0x64, static_cast<std::uint32_t>(files.size()));
    put_text(b, 0x80, dir);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::size_t e = 0xA0 + i * 0x3C;
        le32(b, e + 0x10, static_cast<std::uint32_t>(files[i].data.size()));
        put_text(b, e + 0x18, files[i].name);
        le32(b, e + 0x38, static_cast<std::uint32_t>(b.size()));
        b.insert(b.end(), files[i].data.begin(), files[i].data.end());
    }
    return b;
}

Bytes cbs_file(const std::string& dir, std::size_t payload) {
    Bytes b(0x128, 0);
    put_text(b, 0, std::string("CFU\0", 4));
    le32(b, 4, 0x1F40);
    le32(b, 8, 0x128);
    le32(b, 0x0C, 0x2552C);
    le32(b, 0x10, static_cast<std::uint32_t>(payload));
    put_text(b, 0x14, dir);
    b.resize(0x128 + payload);
    noise(b, 0x128, b.size(), 5);
    return b;
}

void expect_ps2(const sr::Identification& id, sr::Container c, sr::Region r, const char* product, const Bytes* data,
                const std::string& tag) {
    check(id.recognized && id.saves.size() == 1, tag + ": recognised");
    if (id.saves.size() != 1) return;
    const sr::Save& s = id.saves[0];
    check(s.platform == sr::Platform::PlayStation2 && s.container == c && s.region == r && s.product == product,
          tag + ": platform, container, region, product");
    if (data != nullptr) {
        check(s.valid && s.record_state == sr::RecordState::Encrypted && s.payload == *data, tag + ": the data file");
        sr::Record rec;
        std::string error;
        check(sr::extract_record(s, rec, &error) && rec.state == sr::RecordState::Encrypted &&
                  rec.bytes.size() == 0x1C40 && std::equal(rec.bytes.begin(), rec.bytes.end(), data->begin()),
              tag + ": the encrypted record");
    }
    check(!s.convertible, tag + ": not convertible");
    sf::Gci pc;
    std::string error;
    check(!sr::to_pc_gci(s, pc, &error), tag + ": to_pc_gci refuses it");
}

void ps2() {
    const auto us = ps2_dir("BASLUS-21261", 21), pal = ps2_dir("BESLES-53542", 22), jp = ps2_dir("BISLPM-66166", 23);
    Bytes max = max_file("BASLUS-21261", us);
    expect_ps2(sr::identify_bytes(max, "a.max"), sr::Container::ArMax, sr::Region::NtscU, "SLUS-21261", &us[0].data,
               "MAX");
    max[0x0C] ^= 1u;
    sr::Identification id = sr::identify_bytes(max, "a.max");
    check(id.recognized && !id.valid, "MAX: a wrong CRC-32 is not valid");
    // Reversed file order, as some writers store it.
    std::vector<Ps2File> reversed(jp.rbegin(), jp.rend());
    expect_ps2(sr::identify_bytes(max_file("BISLPM-66166", reversed), "b.max"), sr::Container::ArMax,
               sr::Region::NtscJ, "SLPM-66166", &jp[0].data, "MAX, data file last");
    id = sr::identify_bytes(max_file("BASLUS-20000", ps2_dir("BASLUS-20000", 1)), "c.max");
    check(!id.recognized && id.outer == sr::Container::ArMax, "MAX: another game's save");

    expect_ps2(sr::identify_bytes(xps_file("BESLES-53542", pal), "a.xps"), sr::Container::SharkPort, sr::Region::Pal,
               "SLES-53542", &pal[0].data, "XPS");
    expect_ps2(sr::identify_bytes(psv_file("BISLPM-66166", jp), "a.psv"), sr::Container::Psv, sr::Region::NtscJ,
               "SLPM-66166", &jp[0].data, "PSV");
    Bytes broken = psv_file("BISLPM-66166", jp);
    std::memset(broken.data() + 0xA0 + 0x3C + 0x18, 0xE4, 8);  // entry 2's name becomes unreadable
    id = sr::identify_bytes(broken, "b.psv");
    expect_ps2(id, sr::Container::Psv, sr::Region::NtscJ, "SLPM-66166", nullptr, "PSV, broken table");
    check(id.recognized && !id.valid && id.saves[0].payload.empty() &&
              id.saves[0].record_state == sr::RecordState::None,
          "PSV: a broken file table is not unwrapped");

    id = sr::identify_bytes(cbs_file("BESLES-53542", 0x4D9B), "a.cbs");
    expect_ps2(id, sr::Container::CodeBreaker, sr::Region::Pal, "SLES-53542", nullptr, "CBS");
    check(id.recognized && !id.valid && id.saves[0].record_state == sr::RecordState::None,
          "CBS: recognised from its header, not unwrapped");
}

// ---- Xbox and ZIP ----------------------------------------------------------------------------------------------------

Bytes xbox_save(std::uint32_t seed) {
    Bytes b(0x1F74, 0);
    le32(b, 0, 0x1F74);
    noise(b, 4, 0x1F70, seed);
    return b;
}

// A ZIP of stored entries.
Bytes zip(const std::vector<std::pair<std::string, Bytes>>& entries) {
    Bytes b, central;
    for (const auto& [name, data] : entries) {
        const std::uint32_t local = static_cast<std::uint32_t>(b.size());
        const std::uint32_t crc = sr::crc32(data);
        Bytes h(30, 0);
        put_text(h, 0, "PK\x03\x04");
        le16(h, 4, 10);
        le32(h, 14, crc);
        le32(h, 18, static_cast<std::uint32_t>(data.size()));
        le32(h, 22, static_cast<std::uint32_t>(data.size()));
        le16(h, 26, static_cast<std::uint16_t>(name.size()));
        b.insert(b.end(), h.begin(), h.end());
        b.insert(b.end(), name.begin(), name.end());
        b.insert(b.end(), data.begin(), data.end());
        Bytes c(46, 0);
        put_text(c, 0, "PK\x01\x02");
        le16(c, 4, 20);
        le16(c, 6, 10);
        le32(c, 16, crc);
        le32(c, 20, static_cast<std::uint32_t>(data.size()));
        le32(c, 24, static_cast<std::uint32_t>(data.size()));
        le16(c, 28, static_cast<std::uint16_t>(name.size()));
        le32(c, 42, local);
        central.insert(central.end(), c.begin(), c.end());
        central.insert(central.end(), name.begin(), name.end());
    }
    const std::uint32_t cd = static_cast<std::uint32_t>(b.size());
    b.insert(b.end(), central.begin(), central.end());
    Bytes e(22, 0);
    put_text(e, 0, "PK\x05\x06");
    le16(e, 8, static_cast<std::uint16_t>(entries.size()));
    le16(e, 10, static_cast<std::uint16_t>(entries.size()));
    le32(e, 12, static_cast<std::uint32_t>(central.size()));
    le32(e, 16, cd);
    b.insert(b.end(), e.begin(), e.end());
    return b;
}

void xbox_and_zip() {
    const Bytes save = xbox_save(31);
    const std::string path = "UDATA/53450086/25575D9B5D04/Save.dat";
    Bytes archive = zip({{"UDATA/", {}},
                         {"UDATA/53450086/TitleMeta.xbx", text_bytes("TitleName=Shadow The Hedgehog")},
                         {path, save}});
    sr::Identification id = sr::identify_bytes(archive, "x.zip");
    check(id.recognized && id.outer == sr::Container::Zip && id.saves.size() == 1, "Xbox: found in a ZIP");
    if (id.saves.size() == 1) {
        const sr::Save& s = id.saves[0];
        check(s.platform == sr::Platform::Xbox && s.container == sr::Container::XboxSaveDat && s.product == "53450086" &&
                  s.region == sr::Region::Unknown && s.valid && s.record_state == sr::RecordState::Encrypted &&
                  s.location == path && s.id == "x.zip|" + path,
              "Xbox: platform, title, region unknown, location");
        sr::Record rec;
        std::string error;
        check(sr::extract_record(s, rec, &error) && rec.bytes.size() == sf::kRecordBytes &&
                  std::equal(rec.bytes.begin(), rec.bytes.end(), save.begin() + 0x18),
              "Xbox: the encrypted record follows the 0x18-byte header");
        sf::Gci pc;
        check(!s.convertible && !sr::to_pc_gci(s, pc, &error), "Xbox: not converted");
    }
    id = sr::identify_bytes(zip({{"UDATA/4D530004/0123456789AB/Save.dat", save}}), "y.zip");
    check(!id.recognized, "Xbox: another title's Save.dat");
    id = sr::identify_bytes(save, "Save.dat");
    check(id.recognized && !id.valid, "Xbox: a Save.dat without its title folder is listed but not confirmed");
    Bytes wrong = save;
    wrong[0] = 0x70;
    id = sr::identify_bytes(wrong, "UDATA/53450086/25575D9B5D04/Save.dat");
    check(!id.recognized, "Xbox: a wrong length field");

    // Several platforms in one archive, a nested ZIP, and an entry whose CRC is wrong.
    const auto us = ps2_dir("BASLUS-21261", 41);
    Bytes inner = zip({{"save.gci", gci_bytes("GUPJ", gc_data(1, 42))}});
    Bytes mixed = zip({{"ps2/a.max", max_file("BASLUS-21261", us)}, {"inner.zip", inner}, {path, save}});
    id = sr::identify_bytes(mixed, "m.zip");
    check(id.saves.size() == 3, "ZIP: three saves across platforms and a nested archive");
    if (id.saves.size() == 3) {
        check(id.saves[0].platform == sr::Platform::PlayStation2 && id.saves[0].location == "ps2/a.max/BASLUS-21261",
              "ZIP: the PS2 save's location");
        check(id.saves[1].platform == sr::Platform::GameCube && id.saves[1].region == sr::Region::NtscJ &&
                  id.saves[1].location == "inner.zip/save.gci" && id.saves[1].convertible,
              "ZIP: the nested GCI");
        check(id.saves[2].platform == sr::Platform::Xbox, "ZIP: the Xbox save");
    }
    Bytes bad = zip({{"save.gci", gci_bytes("GUPE", gc_data(1, 43))}});
    bad[30 + 8 + 100] ^= 1u;  // inside the entry's data
    id = sr::identify_bytes(bad, "bad.zip");
    check(!id.recognized && id.reason.find("CRC-32") != std::string::npos, "ZIP: an entry with a wrong CRC is skipped");
    id = sr::identify_bytes(text_bytes("hello"), "readme.txt");
    check(!id.recognized && id.outer == sr::Container::Unknown, "not a save");
}

// ---- scan() ----------------------------------------------------------------------------------------------------------

void write_file(const std::filesystem::path& p, const Bytes& b) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

void folder_scan() {
    const std::filesystem::path dir = gekko::test_support::scratch_directory("save_recognize_tests");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    write_file(dir / "a.gci", gci_bytes("GUPE", gc_data(1, 51)));
    write_file(dir / "b.xps", xps_file("BESLES-53542", ps2_dir("BESLES-53542", 52)));
    write_file(dir / "UDATA" / "53450086" / "25575D9B5D04" / "Save.dat", xbox_save(53));
    write_file(dir / "UDATA" / "53450086" / "TitleMeta.xbx", text_bytes("TitleName=Shadow The Hedgehog"));
    write_file(dir / "notes.txt", text_bytes("not a save"));
    const std::vector<sr::ScanEntry> found = sr::scan(dir);
    check(found.size() == 3, "scan: three saves in the folder (got " + std::to_string(found.size()) + ")");
    int gc = 0, ps2 = 0, xbox = 0;
    for (const sr::ScanEntry& e : found) {
        for (const sr::Save& s : e.identification.saves) {
            gc += s.platform == sr::Platform::GameCube;
            ps2 += s.platform == sr::Platform::PlayStation2;
            xbox += s.platform == sr::Platform::Xbox && s.valid;
            check(s.id.rfind(e.path.generic_string(), 0) == 0, "scan: the id starts with the file's path");
        }
    }
    check(gc == 1 && ps2 == 1 && xbox == 1, "scan: one of each platform, the on-disk Xbox save confirmed by its folder");
    check(!sr::identify(dir).recognized, "identify() of a folder points to scan()");
    std::filesystem::remove_all(dir, ec);
}

// ---- The user's samples (arguments) ------------------------------------------------------------------------------------

struct Expected {
    sr::Platform platform;
    sr::Container container;
    sr::Region region;
    bool valid;
    sr::RecordState record;
    bool convertible;
};

// What docs/SAVE_FORMATS.md records for each of the 19 samples (2026-11-05), established by header inspection and
// an independent prototype; the ZIP ones by their single save.
const std::map<std::string, Expected>& expected_samples() {
    using P = sr::Platform;
    using C = sr::Container;
    using R = sr::Region;
    using S = sr::RecordState;
    static const std::map<std::string, Expected> m = {
        {"shadow-the-hedgehog.10128.gci", {P::GameCube, C::Gci, R::NtscU, true, S::Decoded, true}},
        {"shadow-the-hedgehog.10569.gci", {P::GameCube, C::Gci, R::Pal, true, S::Decoded, true}},
        {"shadow-the-hedgehog.10583.gci", {P::GameCube, C::Gci, R::NtscU, true, S::Decoded, true}},
        {"shadow-the-hedgehog.18165.gci", {P::GameCube, C::Gci, R::Pal, true, S::Decoded, true}},
        {"shadow-the-hedgehog.21761.gci", {P::GameCube, C::Gci, R::NtscJ, true, S::Decoded, true}},
        {"shadow-the-hedgehog.26078.gci", {P::GameCube, C::Gci, R::NtscU, true, S::Decoded, true}},
        {"shadow-the-hedgehog.34551.gci", {P::GameCube, C::Gci, R::Pal, true, S::Decoded, true}},
        {"shadow-the-hedgehog.34710.gci", {P::GameCube, C::Gci, R::Pal, true, S::Decoded, true}},
        {"shadow_the_hedgehog_usa.gci", {P::GameCube, C::Gci, R::NtscU, true, S::Decoded, true}},
        {"shadow-the-hedgehog.14395.max", {P::PlayStation2, C::ArMax, R::NtscU, true, S::Encrypted, false}},
        {"shadow-the-hedgehog.28240.max", {P::PlayStation2, C::ArMax, R::NtscU, true, S::Encrypted, false}},
        {"shadow-the-hedgehog.32061.max", {P::PlayStation2, C::ArMax, R::NtscJ, true, S::Encrypted, false}},
        {"shadow-the-hedgehog.32155.max", {P::PlayStation2, C::ArMax, R::NtscJ, true, S::Encrypted, false}},
        {"shadow-the-hedgehog.18297.cbs", {P::PlayStation2, C::CodeBreaker, R::Pal, false, S::None, false}},
        {"shadow-the-hedgehog.32290.xps", {P::PlayStation2, C::SharkPort, R::Pal, true, S::Encrypted, false}},
        {"shadow-the-hedgehog.10027.zip", {P::Xbox, C::XboxSaveDat, R::Unknown, true, S::Encrypted, false}},
        {"shadow-the-hedgehog.34727.zip", {P::Xbox, C::XboxSaveDat, R::Unknown, true, S::Encrypted, false}},
        {"shadow-the-hedgehog.32060.zip", {P::PlayStation2, C::Psv, R::NtscJ, false, S::None, false}},
        {"shadow-the-hedgehog.32156.zip", {P::PlayStation2, C::Psv, R::NtscJ, true, S::Encrypted, false}},
    };
    return m;
}

// Pairs of samples that hold the same game file (found by comparing the unwrapped payloads independently).
const std::vector<std::pair<std::string, std::string>>& same_payload_pairs() {
    static const std::vector<std::pair<std::string, std::string>> p = {
        {"shadow-the-hedgehog.32155.max", "shadow-the-hedgehog.32156.zip"},  // the same JP PS2 save, MAX and PSV
        {"shadow-the-hedgehog.26078.gci", "shadow_the_hedgehog_usa.gci"},    // byte-identical files
    };
    return p;
}

void samples(int argc, char** argv) {
    std::vector<std::filesystem::path> files;
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path p = argv[i];
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            for (const auto& e : std::filesystem::directory_iterator(p, ec))
                if (e.is_regular_file()) files.push_back(e.path());
        } else {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    std::printf("%-32s %-14s %-27s %-8s %-6s %-24s %-5s %s\n", "file", "platform", "container", "region", "valid",
                "record", "conv", "product / location / reason");
    std::map<std::string, Bytes> payloads;
    std::size_t matched = 0;
    for (const auto& path : files) {
        const sr::Identification id = sr::identify(path);
        const std::string name = path.filename().string();
        if (id.saves.empty()) {
            std::printf("%-32s (none) %s\n", name.c_str(), id.reason.c_str());
        }
        for (const sr::Save& s : id.saves) {
            std::printf("%-32s %-14s %-27s %-8s %-6s %-24s %-5s %s %s: %s\n", name.c_str(), sr::to_string(s.platform),
                        sr::to_string(s.container), sr::to_string(s.region), s.valid ? "yes" : "no",
                        sr::to_string(s.record_state), s.convertible ? "yes" : "no", s.product.c_str(),
                        s.location.c_str(), s.reason.c_str());
        }
        const auto it = expected_samples().find(name);
        if (it == expected_samples().end()) continue;
        ++matched;
        const Expected& x = it->second;
        check(id.recognized && id.saves.size() == 1, name + ": exactly one save");
        if (id.saves.size() != 1) continue;
        const sr::Save& s = id.saves[0];
        check(s.platform == x.platform && s.container == x.container && s.region == x.region && s.valid == x.valid &&
                  s.record_state == x.record && s.convertible == x.convertible,
              name + ": identified as docs/SAVE_FORMATS.md records it");
        payloads[name] = s.payload;
        sr::Record rec;
        std::string error;
        if (s.record_state != sr::RecordState::None)
            check(sr::extract_record(s, rec, &error) && rec.state == s.record_state, name + ": the record is extracted");
        if (s.platform == sr::Platform::GameCube) {
            check(rec.md5_ok, name + ": its MD5 matches");
            sf::Gci pc;
            check(sr::to_pc_gci(s, pc, &error) && pc.game_code() == "GUPP" && sf::check_record(pc.data).valid &&
                      std::equal(pc.data.begin(), pc.data.end(), s.payload.begin() + 0x40),
                  name + ": converted to the PC GCI in memory, data unchanged");
        }
        if (s.platform == sr::Platform::PlayStation2 && !s.payload.empty()) {
            const bool padded = std::all_of(s.payload.begin() + 0x1C40, s.payload.end(), [](std::uint8_t v) { return v == 0; });
            check(padded, name + ": the PS2 data file is zero after 0x1C40");
        }
    }
    for (const auto& [a, b] : same_payload_pairs()) {
        if (payloads.count(a) != 0 && payloads.count(b) != 0)
            check(payloads[a] == payloads[b] && !payloads[a].empty(), a + " and " + b + " hold the same game file");
    }
    std::printf("samples: %zu files, %zu of them known samples\n", files.size(), matched);
}

}  // namespace

int main(int argc, char** argv) {
    decoders();
    gamecube();
    ps2();
    xbox_and_zip();
    folder_scan();
    if (argc > 1) samples(argc, argv);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
