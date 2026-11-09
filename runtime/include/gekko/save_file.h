#pragma once

// The game's save file on the host (docs/SAVE_REROUTE_PLAN.md, the user's decisions of 2026-11-05): one GCI -
// the 64-byte directory entry the GameCube CARD library keeps for a file, then the file's data, as Dolphin and
// GameCube save tools exchange it - written atomically, plus the read-only import of an existing save from a GCI or
// from a raw memory-card image (the PC port's .pc.raw working images).
//
// The game's file (TMemoryCardCtrl, vtable 0x805539E0): "SHADOWTHEHEDGEHOG", game GUPP / maker 8P on PAL, 2 blocks
// (16 KiB):
//   0x0000 comments (2 x 32 bytes)   0x0040 banner 96 x 32 RGB5A3   0x1840 icon 32 x 32 RGB5A3
//   0x2040 the record, 0x1F5C bytes: DES-ECB (key EE B0 6C AE A4 78 49 90) over its first 0x1F58 bytes; an MD5 of
//          record[0x10 .. 0x1F5C) at record[0 .. 0x10) - the game's MD5 pads with 0x00 where MD5 pads with 0x80
//          (0x802983E8). Verified on the user's own GCI and every card image in save-data (2026-11-05).
//   The record's options: +0x18 text language, +0x1C voice language, +0x20 speaker mode, +0x24 (byte) vibration.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gekko::save_file {

constexpr std::size_t kGciHeaderBytes = 0x40;
constexpr std::size_t kBlockBytes = 0x2000;
constexpr std::size_t kRecordOffset = 0x2040;
constexpr std::size_t kRecordBytes = 0x1F5C;

// A GCI: the directory entry (big-endian, as on the card) and the file's data (a whole number of 8 KiB blocks).
struct Gci {
    std::array<std::uint8_t, kGciHeaderBytes> header{};
    std::vector<std::uint8_t> data;

    std::string game_code() const;   // "GUPP"
    std::string maker_code() const;  // "8P"
    std::string file_name() const;   // "SHADOWTHEHEDGEHOG"
    std::uint16_t block_count() const;
};

// Reads a GCI; false (and why) when the size does not match its block count.
bool read_gci(const std::filesystem::path& path, Gci& out, std::string* error);
// Writes `gci` to `path` durably: a temporary file beside it, flushed, then renamed over it. The old file stays
// whole until the rename. False (and why) on any failure; the target is then as it was.
bool write_gci(const std::filesystem::path& path, const Gci& gci, std::string* error);

// Makes a GameCube save of any region the canonical PC file: game GUPP, maker 8P, "SHADOWTHEHEDGEHOG", the directory
// entry the save store creates, the source's modified time. The 16 KiB of data stay byte for byte: the record's key and
// MD5 do not depend on the region (docs/SAVE_FORMATS.md 2.2). False (and why) unless it is a 2-block file with a valid record.
bool to_canonical_pc_gci(Gci& gci, std::string* error);

// Finds a file in a raw memory-card image (block 0 header, blocks 1-2 the directory copies, 3-4 the allocation table
// copies; the valid copy with the higher update counter is current) and returns it as a GCI (its directory entry
// and its blocks in chain order). False (and why) when the image is not a card, or the file is absent or broken.
bool extract_from_card(const std::vector<std::uint8_t>& card, const std::string& game, const std::string& maker,
                       const std::string& name, Gci& out, std::string* error);

// The record inside the game's file, decrypted and checked.
struct RecordCheck {
    bool valid = false;            // the MD5 matches
    std::uint32_t text_language = 0, voice_language = 0, speaker_mode = 0;
    std::uint8_t vibration = 0;
    std::uint64_t stamp = 0;       // record + 0x1774 (the game's own save stamp; not a calendar time on the PC port)
};
RecordCheck check_record(const std::vector<std::uint8_t>& file_data);

// The primitives, for tests.
std::array<std::uint8_t, 16> md5(const std::uint8_t* data, std::size_t size, std::uint8_t pad_byte = 0x80);
void des_ecb_decrypt(const std::uint8_t key[8], std::uint8_t* data, std::size_t size);  // whole 8-byte blocks
void des_ecb_encrypt(const std::uint8_t key[8], std::uint8_t* data, std::size_t size);

}  // namespace gekko::save_file
