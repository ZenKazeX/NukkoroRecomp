#pragma once

// The game's clock and its schedule of timed events.
//
// docs/native-time-20261005/DESIGN.md. Guest code runs in zero game time. The
// clock moves only where the game waits for time to pass - a wait point (the
// game's frame limiter, the OS idle loop) - and there it moves event by event:
// each scheduled event is delivered at its own time, in deadline order, before
// the clock passes it. Nothing here counts instructions and nothing here
// models a device: an event is a native deadline with a native consumer.
//
// The unit is one guest time-base tick, the unit OSGetTime and mftb report:
// 40.5 MHz, the bus clock the boot publishes at 0x800000F8 divided by four, so
// every conversion the game performs on the time base stays exact.

#include <array>
#include <cstddef>

#include "gekko/types.h"

namespace gekko {

constexpr u64 kGameTicksPerSecond = 40500000ull;
constexpr u64 kNoDeadline = ~0ull;

// Everything that can be scheduled. Equal deadlines are delivered in this
// order. The set is fixed and small; a deadline per kind is the whole queue.
enum class GameEvent : u8 {
    Frame = 0,    // the 5 ms host service (input, stop/savestate, pacing) and the 1/300 s pacing ticks
    Retrace,      // a video field ends and the next begins (NativeVideo)
    Audio,        // one audio frame of the native mixer
    Decrementer,  // the guest decrementer's 0 -> 1 sign edge (OS alarms)
    Devices,      // the next step of a device not yet native (SI, EXI)
    Count
};

const char* game_event_name(GameEvent event) noexcept;

class GameClock {
public:
    GameClock() noexcept { deadlines_.fill(kNoDeadline); }

    u64 now() const noexcept { return now_; }

    // Moves the clock. Only the scheduler's delivery loop advances it, and
    // never past a pending deadline; savestate load and reset set it outright.
    void set_now(u64 ticks) noexcept { now_ = ticks; }

    void schedule(GameEvent event, u64 when) noexcept {
        deadlines_[static_cast<std::size_t>(event)] = when;
    }
    void cancel(GameEvent event) noexcept {
        deadlines_[static_cast<std::size_t>(event)] = kNoDeadline;
    }
    u64 deadline(GameEvent event) const noexcept {
        return deadlines_[static_cast<std::size_t>(event)];
    }

    // The nearest deadline; ties resolve to the lowest event kind.
    u64 next(GameEvent& event) const noexcept {
        u64 best = kNoDeadline;
        event = GameEvent::Count;
        for (std::size_t i = 0; i < deadlines_.size(); ++i) {
            if (deadlines_[i] < best) {
                best = deadlines_[i];
                event = static_cast<GameEvent>(i);
            }
        }
        return best;
    }

    const std::array<u64, static_cast<std::size_t>(GameEvent::Count)>& deadlines() const noexcept {
        return deadlines_;
    }

private:
    u64 now_ = 0;
    std::array<u64, static_cast<std::size_t>(GameEvent::Count)> deadlines_{};
};

}  // namespace gekko
