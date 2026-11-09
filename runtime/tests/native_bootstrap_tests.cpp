#include "native_bootstrap.h"
#include "shadow/backends/storage_backends.h"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gekko/asset_mount.h"

namespace {

void write_be32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value >> 24u);
    bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 16u);
    bytes.at(offset + 2) = static_cast<std::uint8_t>(value >> 8u);
    bytes.at(offset + 3) = static_cast<std::uint8_t>(value);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_disc_header() {
    std::vector<std::uint8_t> bytes(0x440, 0);
    const std::string id = "GUPP8P";
    std::copy(id.begin(), id.end(), bytes.begin());
    const std::string title = "SHADOW THE HEDGEHOG";
    std::copy(title.begin(), title.end(), bytes.begin() + 0x20);
    write_be32(bytes, 0x1c, shadow::kGameCubeDiscMagic);
    const auto header = shadow::parse_disc_header(bytes);
    require(header.game_id == id, "disc ID was not parsed");
    require(header.title == title, "disc title was not parsed");
}

void test_dol_and_memory() {
    std::vector<std::uint8_t> bytes(0x104, 0);
    write_be32(bytes, 0x00, 0x100);
    write_be32(bytes, 0x48, 0x80003100);
    write_be32(bytes, 0x90, 4);
    write_be32(bytes, 0xd8, 0x80004000);
    write_be32(bytes, 0xdc, 8);
    write_be32(bytes, 0xe0, 0x80003100);
    bytes[0x100] = 0x12;
    bytes[0x101] = 0x34;
    bytes[0x102] = 0x56;
    bytes[0x103] = 0x78;
    const auto dol = shadow::parse_dol(std::move(bytes));
    require(dol.sections.size() == 1 && dol.sections[0].executable, "DOL text section is wrong");
    shadow::ProgramImage memory;
    memory.load_dol(dol);
    require(memory.read_u32_be(0x80003100) == 0x12345678u, "DOL bytes were not loaded big-endian");
    require(memory.read_u32_be(0x80004000) == 0, "DOL BSS was not cleared");
    require(memory.bytes().size() == shadow::kProgramImageEnd - shadow::kProgramImageBase,
            "the program image is not the image region's size");
}

void test_dol_bss_does_not_wipe_overlapping_data() {
    // Shadow's DOL BSS header spans later .sdata2; π for the particle angle
    // wrap lives in that overlap. BSS must be cleared before sections land.
    std::vector<std::uint8_t> bytes(0x108, 0);
    write_be32(bytes, 0x1c, 0x100);          // data[0] file offset
    write_be32(bytes, 0x64, 0x80005000);     // data[0] address
    write_be32(bytes, 0xac, 4);              // data[0] size
    write_be32(bytes, 0xd8, 0x80004000);     // BSS
    write_be32(bytes, 0xdc, 0x2000);
    write_be32(bytes, 0xe0, 0x80005000);
    bytes[0x100] = 0x40;
    bytes[0x101] = 0x49;
    bytes[0x102] = 0x0f;
    bytes[0x103] = 0xdb;
    const auto dol = shadow::parse_dol(std::move(bytes));
    shadow::ProgramImage memory;
    memory.load_dol(dol);
    require(memory.read_u32_be(0x80004000) == 0, "BSS below the data section must still be zero");
    require(memory.read_u32_be(0x80005000) == 0x40490fdBu,
            "initialised data that sits inside the BSS header range must survive");
}

void test_embedded_system_image_with_assets_only_root() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("shadow-assets-only-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "files" / "dir");
    {
        std::ofstream file(root / "files" / "dir" / "file.bin", std::ios::binary);
        const char payload[4] = {0x12, 0x34, 0x56, 0x78};
        file.write(payload, sizeof(payload));
    }

    std::vector<std::uint8_t> boot(0x440, 0);
    std::copy_n("GUPP8P", 6, boot.begin());
    std::copy_n("SHADOW THE HEDGEHOG", 19, boot.begin() + 0x20);
    write_be32(boot, 0x1c, shadow::kGameCubeDiscMagic);

    std::vector<std::uint8_t> dol(0x104, 0);
    write_be32(dol, 0x00, 0x100);
    write_be32(dol, 0x48, 0x80003100);
    write_be32(dol, 0x90, 4);
    write_be32(dol, 0xe0, 0x80003100);

    const auto found = shadow::find_extracted_game_assets(
        root, root / "unused", root / "unused");
    require(found.has_value(), "an embedded build must accept a root containing only files/");
    require(!std::filesystem::exists(root / "sys"),
            "the assets-only fixture must not contain a sys directory");
    const auto mount = shadow::identify_game_data(root, boot);
    require(mount.disc.game_id == "GUPP8P" && mount.disc.title == "SHADOW THE HEDGEHOG",
            "the embedded title block must identify the adjacent asset tree");
    require(mount.root == std::filesystem::weakly_canonical(root),
            "identification must report the canonical root");
    bool refused = false;
    try {
        (void)shadow::identify_game_data(root / "files" / "dir", boot);
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused, "a root without files/ must be refused");

    {
        // The adjacent tree mounts by itself: the namespace is the host tree
        // (no table is read). The locator is not the 0x3000 the table carried -
        // that was a disc offset - it is the token the registry issued.
        gekko::assets::AssetSystem assets;
        std::string error;
        require(shadow::backends::mount_host_directory(assets, root / "files", std::vector<gekko::u8>(boot.begin(), boot.begin() + 0x20),
                             error),
                "the adjacent asset tree must mount: " + error);
        const gekko::assets::AssetId id = assets.registry().find("dir/file.bin");
        require(id.valid(), "the embedded directory must name the asset");
        std::uint8_t output[4]{};
        gekko::u64 got = 0;
        require(assets.read(assets.space().base_of(id), sizeof(output), output, got) ==
                    gekko::assets::AssetSystem::ReadStatus::Ok,
                "the published base must serve the asset");
        require(output[0] == 0x12u && output[1] == 0x34u && output[2] == 0x56u &&
                    output[3] == 0x78u,
                "the assets-only path must return the original file bytes");
    }

    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    try {
        test_disc_header();
        test_dol_and_memory();
        test_dol_bss_does_not_wipe_overlapping_data();
        test_embedded_system_image_with_assets_only_root();
        std::cout << "native bootstrap tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native bootstrap tests failed: " << error.what() << "\n";
        return 1;
    }
}
