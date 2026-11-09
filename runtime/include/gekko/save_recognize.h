#pragma once

// The universal save recognizer (docs/SAVE_FORMATS.md): finds Shadow the Hedgehog saves in a file or a folder,
// whatever the console, region or container, unwraps them, and decodes the game's record as far as it is proven.
//
// What is proven (2026-11-05, the 19 user samples; docs/SAVE_FORMATS.md has the evidence):
//   GameCube (GUPE / GUPP / GUPJ): a GCI or a raw memory-card image. The record (gekko/save_file.h) is decoded and
//     its MD5 checked; every region converts to the canonical PC GCI the save store uses (game GUPP, maker 8P).
//   PlayStation 2 (SLUS-21261 / SLES-53542 / SLPM-66166): Action Replay MAX (.max, LZARI), PS3 PSV, SharkPort /
//     X-Port (.xps), CodeBreaker (.cbs: the header only - its payload cipher key is not available offline). The
//     game's 0x2000-byte data file is unwrapped; its record (0x1C40 bytes) is encrypted with a key that is not known
//     and has a different layout from the GameCube record, so it is never converted.
//   Xbox (title 53450086): UDATA/53450086/<save>/Save.dat (0x1F74 bytes: a 0x18-byte header, then a record with the
//     GameCube record's block layout, encrypted with a key that is not known), so it is never converted.
// ZIP archives (stored / deflate) are searched entry by entry, nested containers included. Nothing is ever written
// to the source; every function here only reads.
//
// Hook points for the in-game "convert these saves into slots?" prompt (not part of this module): scan() lists every
// save in a folder with a stable `id` per save; a caller shows the convertible ones (Save::convertible) and calls
// to_pc_gci() on the one the user picks, then hands the GCI to the save store / slot code.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "gekko/save_file.h"

namespace gekko::save_recognize {

enum class Platform { Unknown, GameCube, PlayStation2, Xbox };
enum class Region { Unknown, NtscU, Pal, NtscJ };
enum class Container {
    Unknown,
    Gci,             // GameCube: 64-byte directory entry + the file's blocks
    GcCardImage,     // GameCube: raw memory-card image (Dolphin .raw, the PC port's .pc.raw)
    ArMax,           // PS2: Action Replay MAX "Ps2PowerSave", LZARI
    CodeBreaker,     // PS2: CodeBreaker "CFU", RC4-encrypted zlib
    SharkPort,       // PS2: SharkPort / X-Port "SharkPortSave"
    Psv,             // PS2 save exported by a PS3 ("\0VSP")
    XboxSaveDat,     // Xbox: the game's Save.dat
    Zip,             // only as Identification::outer
};

// How far the game's record could be decoded.
enum class RecordState {
    None,        // no record could be reached (the container cannot be unwrapped)
    Decoded,     // decrypted and its MD5 checked (valid says whether it matched)
    Encrypted,   // the record's bytes are reached, but the console's cipher key is not known
};

const char* to_string(Platform p);
const char* to_string(Region r);
const char* to_string(Container c);
const char* to_string(RecordState s);

// One save found in a source.
struct Save {
    Platform platform = Platform::Unknown;
    Container container = Container::Unknown;
    Region region = Region::Unknown;
    std::string product;    // "GUPE", "SLUS-21261", "53450086"
    std::string location;   // where in the source: "" for the file itself, else a ZIP entry / container member
    std::string id;         // source path + "|" + location: stable within a scan, for a prompt's selection
    bool valid = false;     // the container's own checks and the record's (where checkable) pass
    std::string reason;     // why it is (in)valid, one line
    RecordState record_state = RecordState::None;
    bool convertible = false;  // to_pc_gci() will succeed
    // The game's file as the console stored it: GC = the GCI (header + data); PS2 = the game's data file; Xbox =
    // Save.dat. Empty when the container cannot be unwrapped.
    std::vector<std::uint8_t> payload;
};

struct Identification {
    bool recognized = false;    // at least one Shadow the Hedgehog save was found
    Container outer = Container::Unknown;  // the file's own container (Zip for an archive)
    // The first save's fields (the request's {platform, container, region, valid, reason}); `reason` explains
    // a source with no save.
    Platform platform = Platform::Unknown;
    Container container = Container::Unknown;
    Region region = Region::Unknown;
    bool valid = false;
    std::string reason;
    std::vector<Save> saves;
};

// Identifies one file (by its content; the extension is only a hint).
Identification identify(const std::filesystem::path& path);
// The same over bytes already in memory; `name` is the file / entry name (for Save.dat and messages).
Identification identify_bytes(std::span<const std::uint8_t> bytes, const std::string& name);

// Every regular file under `folder` (recursively, at most `max_files` files, files over 64 MiB skipped): the ones
// with at least one save, in path order. An Xbox UDATA tree on disk is found through its Save.dat files.
struct ScanEntry {
    std::filesystem::path path;
    Identification identification;
};
std::vector<ScanEntry> scan(const std::filesystem::path& folder, std::size_t max_files = 4096);

// The game's record as far as it is decoded.
struct Record {
    RecordState state = RecordState::None;
    bool md5_ok = false;                 // Decoded only
    std::vector<std::uint8_t> bytes;     // Decoded: the plaintext record (0x1F5C); Encrypted: the ciphertext as
                                         // stored (GC layout 0x1F5C on Xbox, 0x1C40 on PS2)
    save_file::RecordCheck options;      // Decoded only: language, voice, speaker, vibration, stamp
};
bool extract_record(const Save& save, Record& out, std::string* error);

// A GameCube save of any region as the canonical PC GCI (game GUPP, maker 8P, "SHADOWTHEHEDGEHOG", the directory
// entry the save store creates; the data - comments, banner, icon, record - byte for byte). The record is not
// re-encrypted: its key and MD5 do not depend on the region. False (and why) for anything else.
bool to_pc_gci(const Save& save, save_file::Gci& out, std::string* error);

// The decoders, for tests.
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);
bool inflate_raw(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t expected_size,
                 std::string* error);
bool lzari_decode(std::span<const std::uint8_t> in, std::size_t out_size, std::vector<std::uint8_t>& out,
                  std::string* error);
std::vector<std::uint8_t> lzari_encode(std::span<const std::uint8_t> in);  // for round-trip tests

}  // namespace gekko::save_recognize
