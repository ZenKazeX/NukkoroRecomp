#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace shadow {

// The program image: the addresses the game's code, data and BSS (and the OS
// globals and main stack around them) were linked at - the range the game's
// address window maps as its image region (gekko::GuestMemory::kImageBase /
// kImageEnd, which system.cpp asserts equal).
constexpr std::uint32_t kProgramImageBase = 0x80000000u;
constexpr std::uint32_t kProgramImageEnd = 0x80610000u;
constexpr std::uint32_t kGameCubeDiscMagic = 0xC2339F3Du;

struct DiscHeader {
    std::string game_id;
    std::string title;
    std::uint32_t magic = 0;
};

struct DolSection {
    std::uint32_t file_offset = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    bool executable = false;
};

struct DolImage {
    std::vector<std::uint8_t> bytes;
    std::vector<DolSection> sections;
    std::uint32_t bss_address = 0;
    std::uint32_t bss_size = 0;
    std::uint32_t entry_point = 0;
};

// The located game data: its root and the title it carries. The file tree is
// not described here - it is the native namespace System builds from
// files/ (gekko/asset_namespace.h); no directory table is read.
struct ExtractedGameData {
    std::filesystem::path root;
    DiscHeader disc;
};

// The loaded program image (tests): the DOL's sections and BSS at their link
// addresses inside [kProgramImageBase, kProgramImageEnd).
class ProgramImage {
public:
    ProgramImage();

    void load_dol(const DolImage& dol);
    std::uint32_t read_u32_be(std::uint32_t address) const;
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path);
DiscHeader parse_disc_header(const std::vector<std::uint8_t>& bytes);
DolImage parse_dol(std::vector<std::uint8_t> bytes);

std::optional<std::filesystem::path> find_extracted_game_data(
    const std::optional<std::filesystem::path>& explicit_root,
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& working_directory);

// Shipping executables carry the exact sys/ image internally and therefore
// need only an adjacent files/ asset directory.
std::optional<std::filesystem::path> find_extracted_game_assets(
    const std::optional<std::filesystem::path>& explicit_root,
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& working_directory);

// Identifies the game data at `root` from the title block `boot` (the first
// 0x440 bytes of boot.bin). `root` must contain files/.
ExtractedGameData identify_game_data(const std::filesystem::path& root,
                                     const std::vector<std::uint8_t>& boot);

}  // namespace shadow
