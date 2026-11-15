#pragma once

// Dolphin-style custom texture packs, served natively by the lifted renderer's texture import.
//
// A pack is a directory tree of files named the way Dolphin names its hires textures
// (docs/mods/TEXTURE_PACKS.md has the rule and the proof against the disc):
//
//     tex1_{W}x{H}[_m]_{texhash:016x}[_{tluthash:016x}|_$]_{fmt}.{png|dds}
//
// texhash is XXH64 (seed 0) over the raster's base-level texel blocks as the game stores them; for the
// paletted formats (C4 = 8, C8 = 9, C14X2 = 10) tluthash is XXH64 over the palette entries from the
// smallest to the largest index the base level uses. `_m` says the sampler had mip filtering on;
// `_$` matches any palette.
//
// Contract:
//   * Off unless configure_texture_packs() received at least one directory. While off, the import pays one
//     relaxed atomic load per texture build: no hashing, no lookups, no allocation.
//   * A replacement only changes what the GPU samples. The guest raster, its texels, its palette and every
//     guest-visible size stay exactly what the game loaded, so gameplay is unchanged by construction.
//   * Files are indexed once (recursive, case-insensitive names, first directory in the list wins); a file is
//     decoded on its first hit and cached. A file that fails to decode is counted once and then skipped.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "gekko/texture_overrides.h"

namespace gekko::texture_packs {

// ---- configuration -----------------------------------------------------------------------------------------------

// Replaces the pack list and rebuilds the index (directory scan only; nothing is decoded). An empty list turns the
// feature off and drops every cached replacement. Directories that do not exist are reported and skipped.
void configure_texture_packs(std::vector<std::filesystem::path> dirs_highest_priority_first);

// Appends one directory at the lowest priority (the --texture-pack flag: repeatable, first given wins). The index is
// built once, at the first lookup or stats query after the last call, so N flags cost one scan.
void add_texture_pack(const std::filesystem::path& dir);

// The configured directories, highest priority first.
std::vector<std::filesystem::path> texture_pack_directories();

namespace detail {
extern std::atomic<bool> g_enabled;
}
// True when at least one indexed file exists (or directories were added and are not indexed yet). The import's only
// cost while packs are off.
inline bool texture_packs_enabled() noexcept { return detail::g_enabled.load(std::memory_order_relaxed); }

// ---- the lookup the renderer's texture import makes ----------------------------------------------------------------

// `texels` is the raster's base level as the game stores it (at least base_level_bytes(format, width, height)
// bytes); `palette` the raster's whole palette, big-endian 16-bit entries as stored (null / 0 for direct formats).
// `mipmapped` is the Dolphin `_m` flag the drawing sampler implies (exact match preferred, the other spelling
// accepted). Returns the replacement in the GX RGBA8 tile layout texture_overrides uses, or null.
std::shared_ptr<const texture_overrides::Override> find_replacement(std::uint32_t format, std::uint32_t width,
                                                                     std::uint32_t height, const std::uint8_t* texels,
                                                                     std::size_t texel_bytes,
                                                                     const std::uint8_t* palette,
                                                                     std::size_t palette_bytes, bool mipmapped);

struct Stats {
    std::uint64_t directories = 0;
    std::uint64_t files_indexed = 0;       // tex1_ names that parsed and will be served
    std::uint64_t files_shadowed = 0;      // same name already provided by a higher-priority file
    std::uint64_t files_ignored = 0;       // tex1_ files not served: custom mip levels (_mipN), unknown extension
    std::uint64_t lookups = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t mip_flag_fallbacks = 0;  // hit through the other `_m` spelling
    std::uint64_t wildcard_hits = 0;       // hit through a `_$` palette wildcard
    std::uint64_t load_failures = 0;       // indexed files that could not be decoded
    std::uint64_t files_loaded = 0;
    std::uint64_t cache_drops = 0;         // cache cleared because it passed its byte budget
    std::uint64_t resident_bytes = 0;
    double decode_ms = 0.0;
};
Stats texture_pack_stats();
void print_texture_pack_stats(std::ostream& out);

// ---- the naming rule, exposed for the tests and tools --------------------------------------------------------------

std::uint64_t xxh64(const void* data, std::size_t size, std::uint64_t seed = 0) noexcept;

// The base level's stored size: the GX block-padded extent of a WxH image in `format` (0 for an unknown format).
std::size_t base_level_bytes(std::uint32_t format, std::uint32_t width, std::uint32_t height) noexcept;

struct Key {
    std::uint32_t width = 0, height = 0, format = 0;
    std::uint64_t texel_hash = 0;
    std::uint64_t palette_hash = 0;
    bool has_palette = false;
    bool operator==(const Key&) const = default;
};

// The Dolphin key of a raster (see the file comment). Returns nullopt for an unknown format or short data.
std::optional<Key> compute_key(std::uint32_t format, std::uint32_t width, std::uint32_t height,
                               const std::uint8_t* texels, std::size_t texel_bytes, const std::uint8_t* palette,
                               std::size_t palette_bytes) noexcept;

// "tex1_128x128_m_0123456789abcdef_fedcba9876543210_8" (no extension).
std::string dolphin_name(const Key& key, bool mipmapped);

struct ParsedName {
    Key key;
    bool mipmapped = false;
    bool palette_wildcard = false;  // "_$" in place of the palette hash
    int mip_level = -1;             // "_mipN" custom mip level file (not served), -1 for a base image
    bool arbitrary_mipmaps = false; // "_arb"
    bool dds = false;               // otherwise png
};
// Parses a file name (no directory); case-insensitive. nullopt when it is not a tex1_ name.
std::optional<ParsedName> parse_name(std::string_view filename);

// ---- decoders (exposed for the tests) ------------------------------------------------------------------------------

struct Image {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba8;  // linear rows, R G B A
};
// DDS base level: uncompressed 32/24-bit RGB(A) with masks (legacy header or DX10 R8G8B8A8 / B8G8R8A8 / B8G8R8X8),
// BC1 (DXT1), BC2 (DXT2/DXT3), BC3 (DXT4/DXT5). Other encodings (BC7 among them) return false with `error` set.
bool decode_dds(const std::uint8_t* data, std::size_t size, Image& out, std::string* error);
bool decode_png(const std::uint8_t* data, std::size_t size, Image& out, std::string* error);

}  // namespace gekko::texture_packs
