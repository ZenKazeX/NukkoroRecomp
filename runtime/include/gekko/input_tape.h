#pragma once

// A recorded run of controller input, in game time (docs/comparator/README.md "Checkpoints").
//
// The peripheral layer latches each port's sample at the host service, every 5 ms of game time
// (System::kHostServiceTicks, peripheral/manager_imp_pc.h), so what the game reads is decided by the samples latched
// at those ticks and by nothing else. A tape is that sequence: for every host service at which a port's latched
// sample changed, the tick, the port and the new sample. Played back - the same samples latched at the same ticks -
// it reproduces the run exactly in any build whose game time is the game's own; the before / after workstation uses
// it to bring two different builds to the same logical state from the same starting state.
//
// The file is text, one record per line:
//
//   shadow-input-tape 2                  (1: a tape without output records - still read)
//   start <tick>                         the game tick the recording started at (the starting state's)
//   display <width>x<height>             the configured window size (informational since version 2)
//   output <tick> <width>x<height>       version 2: the game-visible output from this host service on - the aspect
//                                        the game's screen tests and the native frustum use (HW_REPORT J); the
//                                        first one is at the start tick, then one per change (a window resize)
//   sample <tick> <port> <connected> <controls hex> <stick x> <stick y> <substick x> <substick y>
//   stop <tick>                          optional: the host-service tick a boundary was requested at
//   saved <tick>                         optional: the game tick the boundary (the checkpoint's savestate) fell at
//   end
//
// Ticks are decimal game ticks (kGameTicksPerSecond per second); sticks are signed stick units (-100..100).
//
// The output is part of what a run is replayed from: the game's state depends on its aspect (the widescreen screen
// tests and culling), so a version 2 tape's output records are applied at their ticks whatever the replaying run's
// window or headless size is; a version 1 tape keeps that run's own output (and the replay matches the recording only
// when the aspect is the same).

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "peripheral/controls.h"

namespace gekko {

class InputTape {
public:
    static constexpr std::size_t kPorts = 4;
    using Samples = std::array<shadow::peripheral::ControllerSample, kPorts>;

    struct Entry {
        std::uint64_t tick = 0;
        std::uint8_t port = 0;
        shadow::peripheral::ControllerSample sample;
    };

    // ---- recording ----
    // Starts a new recording at `start_tick` (the run's starting state); `width` x `height` is noted (the display
    // line). An output noted before (note_output) carries over as the new recording's first, at `start_tick`.
    void begin_recording(std::uint64_t start_tick, int width, int height);
    // At the start and at every host service where the game-visible output changes: an output record.
    void note_output(std::uint64_t tick, int width, int height);
    void stop_recording() noexcept { recording_ = false; }
    bool recording() const noexcept { return recording_; }
    // At every host service, after the samples were latched: records each port whose sample changed.
    void observe(std::uint64_t tick, const Samples& latched);
    // Writes the recording so far; `stop_tick` / `saved_tick` 0 leave the lines out.
    bool write(const std::filesystem::path& path, std::uint64_t stop_tick, std::uint64_t saved_tick,
               std::string* error) const;

    // ---- playback ----
    bool load(const std::filesystem::path& path, std::string* error);
    bool playing() const noexcept { return playing_; }
    void stop_playing() noexcept { playing_ = false; }
    // At every host service: the samples a run latches at `tick` - for each port the newest entry at or before it
    // (ports without one keep `latched`'s value). Counts entries applied after their own tick (a host service grid
    // that is not the recording's: the replay is then not exact).
    void samples_at(std::uint64_t tick, Samples& latched);
    // True once every entry has been applied.
    bool exhausted() const noexcept { return cursor_ >= entries_.size(); }
    // Version 2: the tape names the game-visible output (output records).
    bool has_outputs() const noexcept { return !outputs_.empty(); }
    // At every host service (and once before the game runs, at the start tick): true when an output record at or
    // before `tick` is due that was not applied yet; `width` x `height` is the newest of them.
    bool output_at(std::uint64_t tick, int* width, int* height);

    // ---- what the file says ----
    std::uint64_t start_tick() const noexcept { return start_tick_; }
    std::uint64_t stop_tick() const noexcept { return stop_tick_; }
    std::uint64_t saved_tick() const noexcept { return saved_tick_; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    std::size_t size() const noexcept { return entries_.size(); }
    // Playback: the samples applied so far (the next one's index).
    std::size_t cursor() const noexcept { return cursor_; }
    std::uint64_t late_entries() const noexcept { return late_entries_; }

private:
    struct Output {
        std::uint64_t tick = 0;
        int width = 0;
        int height = 0;
    };
    std::vector<Entry> entries_;
    std::vector<Output> outputs_;
    std::size_t output_cursor_ = 0;
    std::uint64_t start_tick_ = 0;
    std::uint64_t stop_tick_ = 0;
    std::uint64_t saved_tick_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool recording_ = false;
    bool playing_ = false;
    bool have_last_ = false;
    Samples last_{};
    std::size_t cursor_ = 0;
    Samples current_{};
    std::uint64_t late_entries_ = 0;
};

}  // namespace gekko
