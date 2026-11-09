#include "native_bootstrap.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace shadow {
namespace {

std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("Unexpected end of big-endian data");
    }
    return (static_cast<std::uint32_t>(bytes[offset]) << 24u) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8u) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

bool range_fits(std::size_t offset, std::size_t size, std::size_t container_size) {
    return offset <= container_size && size <= container_size - offset;
}

bool is_extracted_root(const std::filesystem::path& root) {
    std::error_code error;
    return std::filesystem::is_regular_file(root / "sys" / "boot.bin", error) &&
           std::filesystem::is_regular_file(root / "sys" / "main.dol", error) &&
           std::filesystem::is_directory(root / "files", error);
}

bool is_asset_root(const std::filesystem::path& root) {
    std::error_code error;
    return std::filesystem::is_directory(root / "files", error);
}

std::optional<std::filesystem::path> find_root(
    const std::optional<std::filesystem::path>& explicit_root,
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& working_directory,
    bool require_system_files) {
    std::vector<std::filesystem::path> candidates;
    if (explicit_root) {
        candidates.push_back(*explicit_root);
    } else {
        candidates.push_back(executable_directory / "GameData");
        candidates.push_back(executable_directory);
        candidates.push_back(working_directory / "GameData");
        candidates.push_back(working_directory);
    }

    std::unordered_set<std::string> visited;
    for (const auto& candidate : candidates) {
        std::error_code error;
        const auto normalized = std::filesystem::weakly_canonical(candidate, error);
        const auto& path = error ? candidate : normalized;
        const std::string key = path.lexically_normal().generic_string();
        if (!visited.insert(key).second) {
            continue;
        }
        if (require_system_files ? is_extracted_root(path) : is_asset_root(path)) {
            return path;
        }
    }
    return std::nullopt;
}

std::string read_c_string(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::size_t limit) {
    if (offset > bytes.size()) {
        throw std::runtime_error("String offset is outside the input");
    }
    const std::size_t end = std::min(bytes.size(), offset + limit);
    std::string result;
    for (std::size_t index = offset; index < end && bytes[index] != 0; ++index) {
        result.push_back(static_cast<char>(bytes[index]));
    }
    return result;
}

}  // namespace

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    const auto end = stream.tellg();
    if (end < 0) {
        throw std::runtime_error("Cannot determine file size: " + path.string());
    }
    const auto unsigned_size = static_cast<std::uintmax_t>(end);
    if (unsigned_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("File is too large for this host: " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(unsigned_size));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw std::runtime_error("Cannot read complete file: " + path.string());
        }
    }
    return bytes;
}

DiscHeader parse_disc_header(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 0x440) {
        throw std::runtime_error("boot.bin is shorter than the GameCube disc header");
    }
    DiscHeader header;
    header.game_id.assign(reinterpret_cast<const char*>(bytes.data()), 6);
    header.title = read_c_string(bytes, 0x20, 0x3e0);
    header.magic = read_be32(bytes, 0x1c);
    if (header.magic != kGameCubeDiscMagic) {
        throw std::runtime_error("boot.bin does not contain the GameCube disc magic");
    }
    return header;
}

DolImage parse_dol(std::vector<std::uint8_t> bytes) {
    if (bytes.size() < 0x100) {
        throw std::runtime_error("main.dol is shorter than its header");
    }
    DolImage dol;
    dol.bytes = std::move(bytes);

    const auto add_section = [&dol](std::size_t offset_field,
                                    std::size_t address_field,
                                    std::size_t size_field,
                                    bool executable) {
        const std::uint32_t file_offset = read_be32(dol.bytes, offset_field);
        const std::uint32_t address = read_be32(dol.bytes, address_field);
        const std::uint32_t size = read_be32(dol.bytes, size_field);
        if (size == 0) {
            return;
        }
        if (!range_fits(file_offset, size, dol.bytes.size())) {
            throw std::runtime_error("main.dol section extends past the file");
        }
        const std::uint64_t memory_begin = address;
        const std::uint64_t memory_end = memory_begin + size;
        if (memory_begin < kProgramImageBase || memory_end > kProgramImageEnd) {
            throw std::runtime_error("main.dol section is outside the program image");
        }
        dol.sections.push_back(DolSection{file_offset, address, size, executable});
    };

    for (std::size_t index = 0; index < 7; ++index) {
        add_section(index * 4u, 0x48u + index * 4u, 0x90u + index * 4u, true);
    }
    for (std::size_t index = 0; index < 11; ++index) {
        add_section(0x1cu + index * 4u, 0x64u + index * 4u, 0xacu + index * 4u, false);
    }
    dol.bss_address = read_be32(dol.bytes, 0xd8);
    dol.bss_size = read_be32(dol.bytes, 0xdc);
    dol.entry_point = read_be32(dol.bytes, 0xe0);

    const std::uint64_t bss_end = static_cast<std::uint64_t>(dol.bss_address) + dol.bss_size;
    if (dol.bss_size != 0 && (dol.bss_address < kProgramImageBase || bss_end > kProgramImageEnd)) {
        throw std::runtime_error("main.dol BSS is outside the program image");
    }
    if (dol.entry_point < kProgramImageBase || dol.entry_point >= kProgramImageEnd) {
        throw std::runtime_error("main.dol entry point is outside the program image");
    }
    return dol;
}

std::optional<std::filesystem::path> find_extracted_game_data(
    const std::optional<std::filesystem::path>& explicit_root,
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& working_directory) {
    return find_root(explicit_root, executable_directory, working_directory, true);
}

std::optional<std::filesystem::path> find_extracted_game_assets(
    const std::optional<std::filesystem::path>& explicit_root,
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& working_directory) {
    return find_root(explicit_root, executable_directory, working_directory, false);
}

ExtractedGameData identify_game_data(const std::filesystem::path& root,
                                     const std::vector<std::uint8_t>& boot) {
    if (!is_asset_root(root)) {
        throw std::runtime_error("Game-data root must contain files/");
    }
    ExtractedGameData data;
    data.root = std::filesystem::weakly_canonical(root);
    data.disc = parse_disc_header(boot);
    return data;
}

ProgramImage::ProgramImage() : bytes_(kProgramImageEnd - kProgramImageBase, 0) {}

void ProgramImage::load_dol(const DolImage& dol) {
    std::fill(bytes_.begin(), bytes_.end(), std::uint8_t{0});
    // Same order as System::load_dol / the IPL: BSS first, then sections.
    // Shadow's DOL BSS header spans later .sdata/.sdata2 (π for particle
    // angle wrap lives there). Zeroing afterwards would wipe those words.
    if (dol.bss_size != 0) {
        const std::size_t bss_offset = static_cast<std::size_t>(dol.bss_address - kProgramImageBase);
        std::fill_n(bytes_.begin() + bss_offset, dol.bss_size, std::uint8_t{0});
    }
    for (const auto& section : dol.sections) {
        const std::size_t destination = static_cast<std::size_t>(section.address - kProgramImageBase);
        std::copy_n(dol.bytes.begin() + section.file_offset, section.size, bytes_.begin() + destination);
    }
}

std::uint32_t ProgramImage::read_u32_be(std::uint32_t address) const {
    if (address < kProgramImageBase) {
        throw std::runtime_error("Guest read is below the program image");
    }
    const std::size_t offset = static_cast<std::size_t>(address - kProgramImageBase);
    if (!range_fits(offset, 4, bytes_.size())) {
        throw std::runtime_error("Guest read is outside the program image");
    }
    return (static_cast<std::uint32_t>(bytes_[offset]) << 24u) |
           (static_cast<std::uint32_t>(bytes_[offset + 1]) << 16u) |
           (static_cast<std::uint32_t>(bytes_[offset + 2]) << 8u) |
           static_cast<std::uint32_t>(bytes_[offset + 3]);
}

}  // namespace shadow
