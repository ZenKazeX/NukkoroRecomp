#pragma once

// The enabled mods' file overlay as host-side readers outside the storage backend see it (shadow/mods/mods.h).
//
// The game's own reads go through the storage backend, which applies the overlay to its tree once at mount
// (shadow/backends/storage_backends.h, StorageOptions::overlays). A few host-side services open disc files by path on
// their own (the native asset prefetcher, the switchable voice archive); they ask here so they read the same bytes
// the game does. Set once at start-up before any reader runs, read-only afterwards. Empty = no mod loaded, and every
// lookup is a single empty-map test.

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gekko::mod_overlay {

void set(const std::vector<std::pair<std::string, std::filesystem::path>>& files);
bool active() noexcept;
// The host file that replaces the disc path `game_path` (any case, '/'-separated), or nullptr.
const std::filesystem::path* find(std::string_view game_path) noexcept;

}  // namespace gekko::mod_overlay
