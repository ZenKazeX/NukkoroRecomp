#pragma once

// Where the game's save file lives and the one-time import of existing progress into it (docs/SAVE_REROUTE_PLAN.md,
// the user's decisions of 2026-11-05). The save is one GCI (gekko/save_file.h); the memory-card images and GCIs the
// PC port used until now are read once, never written.
//
// The save file follows the card the launcher used to pick, so each launch keeps the progress it had:
//   card source (--memory-card, else <root>/GC/EUR/Card A/8P-GUPP-SHADOWTHEHEDGEHOG.gci when present, else none)
//     .gci   -> <save-data>/<stem>.gci, imported from the working image the game played on
//               (<save-data>/<stem>.pc.raw) when it holds a valid save, else from the GCI itself
//     .raw   -> <its directory>/<stem>.gci, imported from that card image
//     none   -> <save-data>/ShadowTheHedgehog.gci (a new game: the game creates its file)
//   --save-file <path> names the file directly (its import source is still the card above, if any).
// A derived path that would be the source itself gets "-pc" before its extension: a source is never written.
//
// Why not "the newest save by its own stamp": the record's stamp (+0x1774) is the console's clock on a GCI made on a
// GameCube or in Dolphin, but a count from boot on the PC port, so the two do not compare (2026-11-05, every save in
// save-data checked) - the oldest GCI would win over months of PC progress.

#include <filesystem>
#include <optional>
#include <string>

namespace gekko::save_import {

struct Paths {
    std::filesystem::path save;              // the GCI the game reads and writes
    std::optional<std::filesystem::path> card_source;  // the old card source, if any
    std::filesystem::path working_image;     // its .pc.raw working image (GCI sources), else empty
};

// The paths for a launch (see the top of this file). `save_data` is <exe>/save-data.
Paths resolve(const std::optional<std::filesystem::path>& save_file_option,
              const std::optional<std::filesystem::path>& memory_card_option, const std::filesystem::path& root,
              const std::filesystem::path& save_data);

struct Result {
    bool imported = false;     // a save file was written now
    bool existed = false;      // the save file was already there (nothing read, nothing written)
    std::string source;        // what it was imported from
    std::string message;       // a line for the log (also the reason when nothing could be imported)
    // The imported record's options (gekko/save_file.h RecordCheck), for the first fill of the PC settings.
    bool has_options = false;
    unsigned text_language = 0, speaker_mode = 0;
    bool vibration = false;
};

// When `paths.save` does not exist: finds the game's file in the working image, then in the card source, checks its
// record, and writes it to `paths.save` atomically together with "<save>.source" (one line: where it came from).
// Sources are only read. A source without a valid save is skipped with the reason in `message`.
Result import_if_missing(const Paths& paths);

}  // namespace gekko::save_import
