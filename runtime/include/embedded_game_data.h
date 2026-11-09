#pragma once

// Exact GameCube program inputs embedded into the final Windows EXE: the title
// block (boot.bin), the BI2 block the OS reads, and the executable (main.dol).
// These bytes are copied from the user's extracted disc during the build; the
// runtime does not reconstruct or reinterpret them. No directory table and no
// apploader is embedded: the file tree is the native namespace built from the
// adjacent files/ directory (gekko/asset_namespace.h), and the native boot
// never runs a disc loader.

#include <cstdint>
#include <vector>

namespace shadow {

struct EmbeddedGameData {
    std::vector<std::uint8_t> boot;
    std::vector<std::uint8_t> bi2;
    std::vector<std::uint8_t> dol;

    bool complete() const noexcept {
        return !boot.empty() && !dol.empty();
    }
};

// Returns an empty structure for development/test executables that were built
// without embedded disc data.
EmbeddedGameData load_embedded_game_data();

}  // namespace shadow
