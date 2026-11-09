#pragma once

// The game's saves on the host: a native save store in place of the memory card (docs/SAVE_REROUTE_PLAN.md, the
// user's decisions of 2026-11-05). The game's save screens (SaveCycle) and its save manager (TMemoryCardCtrl:
// banner, icon, the record's MD5 and DES, the stamp check, every message) run unchanged; below them, the card job
// runner and the CARD library are replaced by one host GCI (gekko/save_file.h).
//
// THE BOUNDARY (static RE 2026-11-05; only TMemoryCardCtrl calls into the runner, through these):
//   0x8029BF84 start(R, job)   owned: busy, job, result 1 ("in progress"), card present; 1 = started, 0 = busy.
//                              The job entries around it (0x8029BF30 format, 0x8029BEAC create(name, len, buf,
//                              banner), 0x8029BE40 delete, 0x8029BDC4 read(name, len, buf), 0x8029BD48
//                              overwrite(name, len, buf), 0x8029BCC8 free blocks, 0x8029BC74 scan) stay guest code:
//                              they store the name (ctx + 8), the length (R + 0x24) and the buffer (R + 0x28).
//   0x8029BA8C tick(R)         owned: runs the started job against the host file, then ends it as the runner's
//                              finish paths do (busy 0, job 0, flags 0, result at R + 0x1C); idle: card present
//                              (R + 8 = 0). Returns busy. The manager reads only R + 8, R + 0x14, R + 0x1C, R + 0x30 /
//                              R + 0x34 (free bytes / directory entries) and ctx + 0xAC (scan count), through its
//                              getters; a job may end on the first tick (the manager waits for busy == 0).
//   Results (R + 0x1C): 0 done; 2 generic (LIMIT ...); 5 I/O error; 7 no space; 8 an interrupted create
//   (iconAddr 0xFFFFFFFF); 9 no file; 10 exists. The record's MD5 / stamp checks are the manager's (8 / 11).
//   CARDProbeEx 0x80390D90 (CheckModule): READY. CARDGetResultCode 0x8038E044 (the reset handler): READY.
//   CARDMountAsync / CARDUnmount / __CARDRead / __CARDWrite / __CARDEraseSector: owned so that nothing reaches the
//   removed card; only the runner called them (counted if anything else ever does).
//
// THE OPTIONS (the user's decision C): the game's text language, speaker mode and vibration live in the PC settings
// (gekko/pc_settings.h options.cfg language / speaker / vibration; -1 = not set yet):
//   * at boot, the language setter 0x801DFA98 (owned) takes the PC language when set - Japanese included, which the
//     console's SRAM cannot hold - so the screens before the save loads are in it too;
//   * after every load (0x8035AA28 at boot, 0x8035B414 from Options' Load) the values are written over the loaded
//     record's (GameData + 0x18 / + 0x20 / + 0x24) before the game applies them; a value not set yet is taken from
//     the record instead, once, and saved to options.cfg (the user's save carries their choices over);
//   * leaving Options does not write the save: Options' exit (state 5, 0x802B90B0) reads "save enabled" (G + 0x7D)
//     as clear for that one decision, so it fades out without the card save; the flag is restored at the next frame;
//   * Autosave off (PcSettings autosave 0) keeps "save enabled" clear every frame, whatever the game sets; on / not
//     set leaves the game's own value (its boot check sets it when a save exists or was created).
//
// Writes are atomic (gekko/save_file.h write_gci) and finished before the job reports success. A host failure is
// the game's own error: no space (7, when the volume is full) or an I/O error (5) - shown by the game's messages.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "gekko/save_file.h"
#include "gekko/types.h"

namespace gekko {

class Cpu;
class GuestMemory;

class SaveStore final {
public:
    SaveStore(Cpu& cpu, GuestMemory& memory) noexcept;

    // The save file (may not exist: the game creates it). Reads it when present.
    void set_path(std::filesystem::path path);
    const std::filesystem::path& path() const noexcept { return path_; }
    bool has_file() const noexcept { return file_.has_value(); }

    // Installs the owners and observers. Before the guest boots.
    void install();

    // The game thread, every frame (from the runner tick): restores Options' "save enabled" flag after its exit.
    struct Stats {
        u64 starts = 0, refused_starts = 0, ticks = 0;
        u64 creates = 0, reads = 0, overwrites = 0, deletes = 0, formats = 0, free_queries = 0, scans = 0;
        u64 failures = 0, writes = 0, write_failures = 0;
        u64 options_applied = 0, options_taken = 0, options_exit_saves_skipped = 0, boot_languages = 0;
        u64 stray_card_calls = 0;  // CARD calls that only the replaced runner made
        s32 last_result = 0;
    };
    const Stats& stats() const noexcept { return stats_; }
    const std::string& last_error() const noexcept { return last_error_; }

    // The guest entries the generator routes to this store for good (extract/native-owners.txt, the saves.card block;
    // CTest native_file_api_tests compares): the card job runner's start and tick and the CARD library's two
    // entries the game still calls. The CARD library's other bodies lose every caller and are not compiled.
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    // The runner's work, also for tests: runs job `job` on the runner object `runner` (guest address) now.
    s32 run_job(u32 runner, u32 job);

private:
    void start(Cpu& c);
    void tick(Cpu& c);
    void after_load(Cpu& c);
    void boot_language(Cpu& c);
    void options_exit(Cpu& c);
    void restore_save_flag();
    s32 write_file(const save_file::Gci& gci);
    std::string guest_text(u32 address, std::size_t max) const;

    Cpu& cpu_;
    GuestMemory& memory_;
    std::filesystem::path path_;
    std::optional<save_file::Gci> file_;
    Stats stats_{};
    std::string last_error_;
    bool save_flag_cleared_ = false;
    u8 save_flag_ = 0;
};

}  // namespace gekko
