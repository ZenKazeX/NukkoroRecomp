// Installing mods: from an archive (.zip natively; .7z, .rar and others through the libarchive-based tar.exe that
// ships with Windows 10 1803 and later) or from a folder, in whatever layout the community distributes them, into
// the loader's own layout (shadow/mods/mods.h). See docs/mods/DESIGN.md ("Installing").
//
// How a download is turned into a mod, file by file (the first rule that applies wins):
//   1. a folder containing mod.json at the top (or under one wrapping folder): already a mod, copied as it is
//   2. tex1_*.png / tex1_*.dds anywhere                     -> textures/ (a Dolphin texture pack)
//      programs (.exe, .dll, scripts...) and GameCube saves (.gci/.gcs/.sav, memory-card images) -> left out, named:
//      the manager never installs or runs a program and never writes a save; a download that is only a program or a
//      save is refused with what it is (and, for this game's own save, how the player can use it). Programs are also
//      removed from a download that is a mod already (rule 1).
//   3. under a folder named code, codes, gecko or cheats; *.gct; sys/main.dol; *.xdelta and other disc-image
//      patches                                             -> recorded as code / image patches the port cannot apply
//   4. other sys/ files (boot.bin, bi2.bin, apploader, fst) -> ignored: the port has no disc header
//   5. <ARCHIVE>/<entry> where the disc has <ARCHIVE>.afs   -> afs/<ARCHIVE>/<entry>
//   6. the longest tail of the path that is a disc path     -> files/<that path>  (any wrapping folders)
//   7. below a folder named files or root (Dolphin's and GameCube Rebuilder's extracted layouts)
//                                                          -> files/<the rest>   (a file the disc does not have)
//   8. a file name that exactly one disc file has           -> files/<that path>  (loose "shadow.one" packs)
//   9. text, images and documents                          -> docs/
//  10. anything else                                       -> left out and listed
// Files of one download that place the same disc file twice come from alternative versions (a widescreen and a
// 4:3 set, a "Dolphin" and a "GameCube" folder...): the larger set is the base, every other set that repeats one
// of its files becomes an option, off by default, choosable in the manager.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "shadow/mods/mods.h"

namespace shadow::mods {

struct InstallRequest {
    std::filesystem::path source;      // an archive file or a folder
    std::filesystem::path mods_dir;
    const GameIndex* game = nullptr;   // the disc's files (required: placement is decided against it)
    bool replace_existing = false;     // an installed mod with the same id is replaced (an update)
    std::string name_hint;             // the mod's display name when the download has none of its own
};

struct InstallResult {
    bool ok = false;
    std::string id;
    std::filesystem::path folder;
    Manifest manifest;
    ContentSummary content;
    std::vector<Issue> issues;
    std::vector<std::string> left_out;  // files of the download that the mod does not use, with the reason
};

InstallResult install(const InstallRequest& request);

// The archive's files written below `dest` (created). Entry names are checked: no absolute paths, drive letters or
// "..", no links; at most 200,000 entries and 16 GiB. False with `error` set on any problem (dest may then hold
// part of the archive; the caller removes it).
bool extract_archive(const std::filesystem::path& archive, const std::filesystem::path& dest, std::string& error);
bool is_archive(const std::filesystem::path& file);

// Removes an installed mod's folder (only a direct sub-folder of mods_dir). False with `error` set otherwise.
bool remove_installed(const std::filesystem::path& mods_dir, const std::filesystem::path& folder, std::string& error);

}  // namespace shadow::mods
