#pragma once

// The PC implementation of Shadow's peripheral platform interface.
//
// Shadow's engine keeps its controller logic platform-neutral. Once per frame its peripheral manager asks
// the platform implementation how many player ports exist, what each port reads, and how each port's two
// analog sticks are calibrated; from that it derives held / pressed / released controls and normalised
// sticks itself, so the platform decides nothing about gameplay. This class is that implementation for a
// PC (docs/lift-20261006/L1_INPUT.md):
//
//   * a port reads whatever its controller sample says - a keyboard, a gamepad or a script fills it;
//   * samples are latched at points of game time chosen by the host (every 5 ms of game time), never read
//     from a device in the middle of a frame, so a recorded or scripted run replays exactly;
//   * a controller is present or absent; there is no probing, settling or transfer error to report;
//   * rumble is a per-port on / off request that the device layer carries out.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "peripheral/controls.h"

namespace shadow::peripheral {

class ManagerImpPc {
public:
    static constexpr std::size_t kPorts = 4;

    enum class PortState : std::uint32_t {
        Ready = 0,
        Absent = 1,
    };

    // One port, in the engine's convention: the controls held and both sticks in stick units with y
    // positive DOWN. A negative deflection reads one unit closer to centre than the same positive one (the
    // shipped game's reported values, which its stick tuning was made against).
    struct PortReading {
        PortState state = PortState::Absent;
        std::uint32_t controls = 0;
        std::int16_t left_x = 0;
        std::int16_t left_y = 0;
        std::int16_t right_x = 0;
        std::int16_t right_y = 0;
    };

    // Each stick's dead zone and live range in stick units. The engine zeroes an axis inside the dead
    // zone, clamps what remains to a circle of the live range and divides by it.
    struct StickRange {
        std::int16_t left_dead = 15;
        std::int16_t right_dead = 25;
        std::int16_t left_range = 42;
        std::int16_t right_range = 39;
    };

    ManagerImpPc() noexcept;

    // ---- sources ----------------------------------------------------------------------------------------
    // The host's sample for a port, latched at the current point of game time.
    void latch(std::size_t port, const ControllerSample& sample) noexcept;
    const ControllerSample& sample(std::size_t port) const noexcept;

    // Live play only (docs/PERF_60FPS_2026-11-04.md, input grid): called at the start of every read() so the host
    // can latch its newest samples at the moment the engine reads them, instead of the last 5 ms latch of game time
    // (which, in wall time, can be up to ~12 ms older: the game's frame work runs game time in bursts). The host
    // installs it only for live input and must leave the latches alone while a tape plays or records - a replay
    // stays exact because tapes keep the 5 ms grid.
    using LiveSource = std::function<void(ManagerImpPc&)>;
    void set_live_source(LiveSource source) { live_source_ = std::move(source); }

    // ---- what the engine asks ---------------------------------------------------------------------------
    std::uint32_t port_count() const noexcept { return static_cast<std::uint32_t>(kPorts); }
    // Reads ports 0 .. count-1 into `out` (one frame's read); a port past kPorts reads absent.
    void read(PortReading* out, std::size_t count) noexcept;
    StickRange stick_range(std::size_t port) const noexcept;

    // ---- rumble -----------------------------------------------------------------------------------------
    // Told about every change of a port's rumble request, on the thread that made it.
    using RumbleListener = std::function<void(std::size_t port, bool on)>;
    void set_rumble_listener(RumbleListener listener);
    void set_rumble(std::size_t port, bool on);
    bool rumble(std::size_t port) const noexcept;

    // ---- persistence (savestates) -------------------------------------------------------------------------
    // The latched samples and the rumble requests. A restored rumble request that differs from the current
    // one is reported to the listener.
    void save(std::vector<std::uint8_t>& out) const;
    bool load(const std::uint8_t*& cursor, const std::uint8_t* end);
    // Back to the start state: port 0 holds a connected, idle controller (the keyboard is always there),
    // the other ports are empty, no rumble.
    void reset() noexcept;

    struct Stats {
        std::uint64_t frames_read = 0;
        std::uint64_t rumble_changes = 0;
    };
    const Stats& stats() const noexcept { return stats_; }
    std::uint32_t connected_mask() const noexcept;  // bit p = port p has a controller

private:
    std::array<ControllerSample, kPorts> samples_{};
    std::array<bool, kPorts> rumble_{};
    RumbleListener rumble_listener_;
    LiveSource live_source_;
    Stats stats_;
};

}  // namespace shadow::peripheral
