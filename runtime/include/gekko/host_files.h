#pragma once

// Host files of the PC's save storage: the memory card's working image and its
// GCI source marker (gekko/native_card.h), the console settings image
// (gekko/native_ipl.h). Whole-file reads, and durable atomic replacement: a
// write goes to <path>.tmp, is flushed and synced, and is renamed over <path>
// (MoveFileExW with REPLACE_EXISTING | WRITE_THROUGH on Windows), so an
// interrupted write leaves the old file or the new one, never a torn one.
//
// The save-pipeline trace below is diagnostic only (inert unless
// GEKKO_AUDIO_AUDIT is set, gekko/audio_audit.h): request -> serialization ->
// open -> write -> flush -> close -> fsync -> rename. Each record: a = start ns
// (audit clock), b = duration ns, c = bytes, d = guest LR of the request (0
// when not applicable). It records only on the game thread (the audit lanes are
// owner-thread buffers; an exit-time persist from another thread must not race
// them).

#include <filesystem>
#include <vector>

#include "gekko/types.h"

namespace gekko {

// Throws std::runtime_error naming the path when the file cannot be opened,
// sized or read.
std::vector<u8> read_host_file(const std::filesystem::path& path);

// Throws std::runtime_error naming the path on a failed create, write, flush
// or rename (the temporary is removed after a failed rename).
void write_host_file_atomically(const std::filesystem::path& path, const std::vector<u8>& bytes);

namespace save_trace {

// The game thread claims the trace the first time it reaches a save path.
void claim_owner() noexcept;
void mark(const char* stage, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0) noexcept;

// One timed stage, recorded when it ends (on the owner thread, audit enabled).
class Stage {
public:
    explicit Stage(const char* stage, u64 size = 0, u64 origin = 0) noexcept;
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

private:
    const char* name_;
    u64 start_ = 0;
    u64 bytes_ = 0;
    u64 origin_ = 0;
};

}  // namespace save_trace
}  // namespace gekko
