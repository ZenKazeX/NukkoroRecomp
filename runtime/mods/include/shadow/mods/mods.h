// The mod loader's model: installed mods, their manifests, the persisted load order, dependency and conflict
// resolution, and the plan the game applies at start-up. Shared by the game (runtime/src/shadow_main.cpp) and the
// mod manager (tools/mod_manager), so both always agree on what a set of mods means. Design and rationale:
// docs/mods/DESIGN.md; what the Reloaded ecosystem ships and how each kind maps here: docs/mods/RELOADED_RESEARCH.md.
//
// A mods directory (<executable dir>/mods by default):
//
//   mods/
//     mods.json                  the state: load order (highest priority first), enable flags, chosen options
//     <folder>/                  one installed mod
//       mod.json                 its manifest (id, name, version, author, description, dependencies, ...)
//       files/...                disc files: replaces the file of that path, or adds a new one (paths are the
//                                extracted disc's files/ tree, matched without regard to case)
//       afs/<ARCHIVE>/<entry>    entries of an AFS archive replaced by entry name (PAFS layout): afs/PRS_VOICE_E/
//                                act04_g149_sl.adx replaces that entry of PRS_VOICE_E.afs
//       textures/...             a Dolphin texture pack (tex1_<w>x<h>_..._<fmt>.png|dds, any sub-folders)
//       options/<option>/<choice>/{files,afs,textures}   content of one choice of a configurable option
//     .cache/                    archives the loader rebuilt (AFS with replaced entries), keyed by their inputs
//
// Nothing here touches guest memory or game code: a mod changes which bytes the game's file reads return and which
// texels the renderer samples. With no enabled mod the plan is empty and the game takes exactly the path it takes
// without a loader.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "shadow/mods/json.h"

namespace shadow::mods {

inline constexpr int kManifestSchema = 1;
inline constexpr int kStateSchema = 1;
inline constexpr std::string_view kManifestName = "mod.json";
inline constexpr std::string_view kStateName = "mods.json";
inline constexpr std::string_view kCacheName = ".cache";

// ---- issues ------------------------------------------------------------------------------------------------------

enum class Severity { Info, Warning, Error };
struct Issue {
    Severity severity = Severity::Info;
    std::string mod_id;  // empty: about the mods directory as a whole
    std::string message;
};
const char* severity_name(Severity s) noexcept;
bool has_errors(const std::vector<Issue>& issues, std::string_view mod_id = {});

// ---- versions ----------------------------------------------------------------------------------------------------

// "1.2", "1.2.3", "v1.2.3-beta": up to four numeric parts (missing parts are 0) and an optional suffix after '-' or
// '+' that only breaks ties (a version with a suffix sorts before the same numbers without one).
struct Version {
    std::uint32_t parts[4] = {0, 0, 0, 0};
    std::string suffix;
    static std::optional<Version> parse(std::string_view text);
    int compare(const Version& other) const noexcept;
};

// ---- manifest ----------------------------------------------------------------------------------------------------

struct Dependency {
    std::string id;
    std::string min_version;  // empty: any version
    bool optional = false;    // optional: only orders the two mods when both are enabled
};

struct OptionChoice {
    std::string id;
    std::string name;
    std::string folder;  // relative to the mod folder; empty = this choice adds nothing
};

struct ModOption {
    std::string id;
    std::string name;
    std::string description;
    std::vector<OptionChoice> choices;
    std::string default_choice;  // a choice id; empty = the first choice
};

struct Manifest {
    int schema = kManifestSchema;
    std::string id;           // unique; lower-case letters, digits, '.', '_', '-'
    std::string name;
    std::string version;      // Version::parse must accept it
    std::string author;
    std::string description;
    std::string website;
    std::string game_id;      // the disc the content was made from (GUPE8P, GUPP8P, GUPR8P...): information only
    std::string preview;      // relative path of a preview image (png/jpg/bmp)
    std::vector<Dependency> dependencies;
    std::vector<std::string> incompatible;  // ids that must not be enabled together with this mod
    std::vector<std::string> load_after;    // soft order: these load first (lower priority) when also enabled
    std::vector<ModOption> options;
    json::Value extra;  // members this version does not know, kept so a rewrite does not drop them
};

bool valid_mod_id(std::string_view id) noexcept;
// A lower-case id made from free text ("Maria Better Eyes!" -> "maria-better-eyes"); never empty ("mod").
std::string make_mod_id(std::string_view text);
// Manifest from JSON; problems go to `issues` (errors make the manifest unusable).
bool parse_manifest(const json::Value& value, Manifest& out, std::vector<Issue>& issues);
json::Value manifest_to_json(const Manifest& manifest);
bool read_manifest(const std::filesystem::path& file, Manifest& out, std::vector<Issue>& issues);
bool write_manifest(const std::filesystem::path& file, const Manifest& manifest, std::string& error);

// ---- installed mods ----------------------------------------------------------------------------------------------

// What one content root (the mod folder itself or an option's choice folder) provides.
struct ContentSummary {
    std::size_t files = 0;          // files/ entries
    std::size_t afs_entries = 0;    // afs/<archive>/<entry>
    std::size_t textures = 0;       // textures/ entries named tex1_*
    std::size_t other_textures = 0; // other images under textures/ (previews, notes)
    std::vector<std::string> unsupported;  // content a recompiled executable cannot take (code patches)
};

struct InstalledMod {
    std::filesystem::path folder;
    std::string folder_name;
    Manifest manifest;
    bool usable = false;  // the manifest parsed and its id is valid and unique
    std::vector<Issue> issues;
    ContentSummary content;  // the base content (options not included)
};

// Every sub-folder of `mods_dir` except dot-folders; each with or without a usable manifest (a folder without
// mod.json is reported, not guessed at: the installer writes manifests). Duplicate ids: all but the first folder in
// name order are unusable. Symbolic links and junctions are not followed.
std::vector<InstalledMod> discover(const std::filesystem::path& mods_dir, std::vector<Issue>& issues);
ContentSummary summarize_content(const std::filesystem::path& root);

// ---- state -------------------------------------------------------------------------------------------------------

struct StateEntry {
    std::string id;
    bool enabled = false;
    std::map<std::string, std::string> options;  // option id -> choice id
    std::map<std::string, bool> codes;           // code item id -> chosen (docs/mods/codemods/README.md); default off
};

struct State {
    int schema = kStateSchema;
    bool mods_enabled = true;          // the master switch: false = the game loads no mod at all
    std::vector<StateEntry> order;     // highest priority first: its files win conflicts
};

// A missing file reads as the empty state (true). A damaged file is an error (false, `error` set) and the caller
// decides: the game runs without mods, the manager offers to rebuild it.
bool load_state(const std::filesystem::path& file, State& out, std::string& error);
// Written atomically (temporary + rename), so a crash never leaves half a file.
bool save_state(const std::filesystem::path& file, const State& state, std::string& error);
// Drops entries whose mod is gone or unusable, appends newly found usable mods (disabled, at the bottom), and fills
// option choices that are missing or no longer exist with the default. True when something changed.
bool reconcile(State& state, const std::vector<InstalledMod>& mods);

// ---- resolution --------------------------------------------------------------------------------------------------

struct Resolution {
    // The mods that load, lowest priority first (later ones win): the user's order, adjusted only where a dependency
    // or load_after needs another order (a mod always loads after - above - what it depends on).
    std::vector<const InstalledMod*> load_order;
    std::vector<Issue> issues;
    std::set<std::string> skipped;  // enabled mods that do not load, with the reason in `issues`
};

// Enabled mods whose dependencies are missing, disabled, too old, or skipped themselves are skipped; of two enabled
// mods that declare each other incompatible, the lower-priority one is skipped; a dependency cycle skips every mod
// on it. Nothing is enabled behind the user's back: the manager offers to enable dependencies, the game reports.
Resolution resolve(const std::vector<InstalledMod>& mods, const State& state);

// ---- game index --------------------------------------------------------------------------------------------------

// The extracted disc's files/ tree: every path ('/'-separated, as on the disc) by its lower-case spelling.
struct GameIndex {
    std::map<std::string, std::string> paths;                    // lower-case -> disc spelling
    std::map<std::string, std::vector<std::string>> by_name;     // lower-case file name -> disc spellings
    bool build(const std::filesystem::path& files_root, std::string& error);
    const std::string* find(std::string_view path) const;        // any case; '\' accepted
    bool empty() const noexcept { return paths.empty(); }
};

// ---- the plan ----------------------------------------------------------------------------------------------------

struct FileOverlay {
    std::string game_path;                  // disc spelling when the disc has it, else the mod's own ('/'-separated)
    std::filesystem::path host;             // the file served instead
    std::string mod_id;                     // the winner
    std::vector<std::string> overridden;    // lower-priority mods that also provide it (highest first)
    bool added = false;                     // a file the disc does not have
};

struct AfsEntryOverlay {
    std::string entry_name;
    std::filesystem::path host;
    std::string mod_id;
    std::vector<std::string> overridden;
};

struct AfsOverlay {
    std::string archive_path;                // disc spelling, e.g. "PRS_VOICE_E.afs"
    std::vector<AfsEntryOverlay> entries;    // sorted by entry name
};

struct Plan {
    std::vector<FileOverlay> files;                      // sorted by lower-case game path
    std::vector<AfsOverlay> afs;
    std::vector<std::filesystem::path> texture_dirs;     // highest priority first
    std::vector<std::string> texture_mods;               // the mod of each texture_dirs entry
    std::vector<std::string> active;                     // mod ids that contribute, lowest priority first
    std::vector<Issue> issues;
    bool empty() const noexcept { return files.empty() && afs.empty() && texture_dirs.empty(); }
};

// The content of every resolved mod (base + chosen options) merged by priority. Content the port cannot take
// (code patches, AFS entries the archive does not have, names outside the disc's name rule) is reported and left
// out. `game` may be empty (tests): then every file counts as added and AFS entries are not checked.
Plan build_plan(const Resolution& resolution, const State& state, const GameIndex& game,
                const std::filesystem::path& files_root);

// ---- format checks -----------------------------------------------------------------------------------------------

// A structural check of the containers the game parses without checking (a truncated or damaged file crashes the
// game's own loader): .one (RenderWare chunk header whose length is the file's, "One Ver" signature, entry table
// inside the file), .txd / .dff (the first chunk fits the file), .afs (table of contents inside the file), .adx
// (CRI header). Other kinds pass. True when the file looks whole; otherwise `why` says what is wrong.
bool check_file_format(const std::filesystem::path& file, std::string_view game_path, std::string& why);

// Whole files this game cannot use as they are (made for another version of the game): a movie whose picture is
// larger than the 512x480 every disc movie has (the Xbox version's 640x480 movies play scrambled: the game decodes
// them into its 512x480 picture). False with `why` set for such a file; check_file_format includes this check. The
// installer leaves such a file out (the rest of the download installs); the loader skips it at start-up.
bool check_file_compatible(const std::filesystem::path& file, std::string_view game_path, std::string& why);

// ---- AFS ---------------------------------------------------------------------------------------------------------

struct AfsEntry {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::string name;  // from the name table; empty when the archive has none
};

struct AfsArchive {
    std::vector<AfsEntry> entries;
    std::uint64_t name_table_offset = 0;
    std::uint64_t name_table_size = 0;
    std::uint64_t alignment = 2048;
    std::vector<std::uint8_t> name_records;  // the raw 0x30-byte records, kept for rebuilding
};

bool read_afs(const std::filesystem::path& file, AfsArchive& out, std::string& error);

// The archive with the overlay's entries replaced (same entry order, names and record dates; sizes and offsets
// recomputed, every entry aligned as the original). Written to `cache_dir`/<stem>-<digest>.afs, where the digest
// covers the original's size and time and every replacement's path, size and time; an existing file of that name is
// reused. `out` is the file to serve.
bool materialize_afs(const std::filesystem::path& original, const AfsOverlay& overlay,
                     const std::filesystem::path& cache_dir, std::filesystem::path& out, std::string& error);

// ---- the game's start-up ------------------------------------------------------------------------------------------

// One data code for the in-game code engine (gekko/code_engine.h), as the code-mod pipeline wrote it for the chosen
// codes (mods/.variants/<signature>.json "engine_codes": PAL Gecko lines).
struct RuntimeCode {
    std::string name;                                   // "<mod>:<item>#<feature>"
    std::vector<std::pair<std::uint32_t, std::uint32_t>> lines;
    bool needs_variant = false;  // its feature also changes code: it runs only in the variant built for `code_signature`
};

struct RuntimeSetup {
    bool active = false;                                                // anything to apply
    std::vector<std::pair<std::string, std::filesystem::path>> files;  // game path -> host file (AFS rebuilt included)
    std::vector<std::filesystem::path> texture_dirs;                   // highest priority first
    std::vector<std::string> active_mods;                              // "name version (id)", lowest priority first
    std::string code_signature;  // code_selection_signature() of the chosen codes; empty when none is chosen
    std::vector<RuntimeCode> codes;  // the built selection's data codes (empty when not built or none)
    std::vector<Issue> issues;
};

// Everything the game needs, from `mods_dir` (missing directory or state, or the master switch off: inactive and no
// issues). Never throws; a damaged state file or unreadable directory leaves the game running without mods and says
// why in `issues`.
RuntimeSetup prepare_runtime(const std::filesystem::path& mods_dir, const std::filesystem::path& files_root);

// ---- code mods ---------------------------------------------------------------------------------------------------

// What a mod offers as code (docs/mods/codemods/README.md), from its codes/ folder:
//   "dol"            a replacement main.dol: every change it makes to its base executable
//   "mcm"            Melee Code Manager libraries (*.txt with "-==-" entries), when there is no main.dol
//   "gecko:<name>"   one named code of a Dolphin game ini ([Gecko] / [ActionReplay] "$name" blocks)
//   "text:<file>"    a text file of code lines ("Code: 044978f0 60000000", 8+8 hex digits per line)
// Code is applied only when the user chooses an item (StateEntry::codes) and only through the code-mod pipeline
// (tools/mods/codemods/build_variant.py): code patches need a recompiled variant executable, data codes the
// runtime code engine. Nothing here runs by itself.
struct CodeItem {
    std::string id;
    std::string label;
    std::string input;          // the pipeline's input spec, e.g. "gecko-ini:<path>", "dol-diff:<path>"
    std::filesystem::path file;
};
std::vector<CodeItem> list_code_items(const InstalledMod& mod);

// A digest of every chosen code item of the enabled mods (mod id, item id and the bytes of its input file), in load
// order. Empty when nothing is chosen. A prepared code plan or built variant records the signature it was made for;
// the game and the manager use it only when it matches.
std::string code_selection_signature(const std::vector<InstalledMod>& mods, const State& state);

// ---- profiles ----------------------------------------------------------------------------------------------------

// Named sets of enable flags, order and option choices: <mods dir>/.profiles/<name>.json, the state file's format.
// A profile names mods by id; loading one reconciles it with what is installed (missing mods dropped, new ones off).
bool valid_profile_name(std::string_view name) noexcept;  // 1-40 characters: letters, digits, space, - _ ( ) .
std::vector<std::string> list_profiles(const std::filesystem::path& mods_dir);
bool save_profile(const std::filesystem::path& mods_dir, std::string_view name, const State& state, std::string& error);
bool load_profile(const std::filesystem::path& mods_dir, std::string_view name, State& out, std::string& error);
bool delete_profile(const std::filesystem::path& mods_dir, std::string_view name, std::string& error);

// ---- texture packs ------------------------------------------------------------------------------------------------

// Textures that more than one active pack provides (the same Dolphin name, any case): the first pack in
// plan.texture_dirs wins. One entry per (winner, loser) pair.
struct TextureConflict {
    std::string winner, loser;  // mod ids
    std::size_t count = 0;
    std::vector<std::string> examples;  // a few file names
};
std::vector<TextureConflict> texture_conflicts(const Plan& plan);

// An absolute path Windows can use past MAX_PATH (the "\\?\" form; unchanged elsewhere). Mod downloads nest deep
// folders with long names, so every walk and write of mod content goes through it.
std::filesystem::path long_path(const std::filesystem::path& path);

// UTF-8 text of a path (for messages and JSON), without a long-path prefix.
std::string utf8(const std::filesystem::path& path);
std::filesystem::path from_utf8(std::string_view text);

}  // namespace shadow::mods
