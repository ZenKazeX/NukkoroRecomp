// What a mod changes, in the player's terms: which voice lines, which stages and menus, which region-specific files.
// Built from the player's own game files (subtitle tables, voice archive) and from maps measured on the game
// (test_data/mods/*.json, shipped beside the manager as mods-data/). Used by the mod manager only: the game's loader
// never reads any of this.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "shadow/mods/mods.h"

namespace shadow::mods {

// A spoken line: where its subtitle sits and what it says (the game's own English text, read from the disc tree).
struct VoiceLine {
    std::string table;   // the subtitle table's disc path, e.g. "fonts/stg0100/stg0100_EN.fnt"
    std::uint32_t key = 0;
    std::string text;    // UTF-8
};

// PRS_VOICE_E.afs entries by name, with the subtitle lines that play them (an entry can serve several tables).
struct VoiceIndex {
    std::map<std::string, std::uint32_t> entry_index;              // lower-case entry name -> AFS index
    std::map<std::uint32_t, std::vector<VoiceLine>> lines;          // AFS index -> lines
    bool build(const std::filesystem::path& files_root, std::string& error);
    const std::vector<VoiceLine>* find(std::string_view entry_name) const;
};

// Reads one FNT subtitle table: (key, audio id, UTF-8 text) per entry. Little-endian, 20-byte records, UTF-16LE
// strings (the layout is the disc's; see docs/mods/map/VOICE_LINES.md).
struct FntEntry {
    std::uint32_t key = 0;
    std::int32_t audio_id = -1;
    std::uint32_t type = 0;
    std::uint32_t active_time = 0;
    std::string text;
};
bool read_fnt(const std::filesystem::path& file, std::vector<FntEntry>& out, std::string& error);

}  // namespace shadow::mods

namespace shadow::mods {

// The disc files PAL and NTSC-U differ in (test_data/mods/region_diff.json, docs/mods/map/REGION_DIFF.md), so a mod
// built from NTSC-U files can be told apart from one that keeps PAL's later revision.
struct RegionFile {
    std::string pal_md5, usa_md5, kind, risk, summary, advice, stage_title;
};
struct RegionData {
    std::map<std::string, RegionFile> paths;  // lower-case disc path
    bool load(const std::filesystem::path& json_file, std::string& error);
    bool empty() const noexcept { return paths.empty(); }
};

// Lower-case hex MD5 of a file ("" when it cannot be read).
std::string md5_file(const std::filesystem::path& file);

// Notes on a mod's disc files that differ between the regions: a stage layout from NTSC-U (gameplay: PAL's added
// collision volumes, enemies, rings are gone), font files from the other disc, region-specific artwork. Covers the
// base content and every option folder. Also flags save / front-end message tables that lack messages the PAL
// table has (their PC wording is then not applied).
std::vector<Issue> region_notes(const InstalledMod& mod, const RegionData& region, const std::filesystem::path& files_root);

}  // namespace shadow::mods
