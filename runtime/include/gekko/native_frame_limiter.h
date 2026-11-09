#pragma once

// The game's frame limiter (fn_801E1F2C), served natively.
//
// Current contract and original-code evidence: docs/FRAME_PACING.md.
// The original spun on OSGetTime until the frame's microsecond grid boundary
// passed - 43% of every instruction the console version retired. Here the
// game waits for time to pass, which is a wait point: the owner advances the
// game clock to the boundary (System::advance_game_time), delivering every
// event on the way. The console's multi-period waiting policy is removed;
// published elapsed-time and game speed controls keep their existing math.
//
// What the game observes, kept:
//  * P = trunc(single(1e6f * single(1.0f / hz))) us (hz at 0x8057E760+0x50),
//    the period the game's elapsed time counts in;
//    us = (time base * 8) / ((bus / 4) / 125000) (OSGetTime -> 0x80051414);
//  * the first call (r13+0x4576 == 0) arms r13+0x4575/0x4576 and samples
//    "last" (r13+0x4578/0x457C) from the clock;
//  * elapsed = P x the frames begun between last and now (u64); movie mode
//    (r13+0x2C50 != 0) first calls 0x804040AC (one display-refresh wait);
//    bypass (+0x14) skips the wait; otherwise the frame waits until
//    elapsed >= P. The old interval (+0x10) can only disable waiting (zero,
//    reset/disc-error mode); every nonzero value waits at most one period;
//  * dt = single(elapsed) * 1e-6f to r13+0x4558, clamped to [P, P * cap]
//    (cap +0x34) after the r13+0x4560 override; raw dt (now - last) to
//    r13+0x455C; last = now; then 0x80203D20(obj + 0xC, n = low word of
//    elapsed / P, f1 = dt) and 0x80337DD4(obj + 0x88, byte r13+0x2D7C).
// The waits are game time; the host paces them (GameEvent::Frame).
//
// Replaced: the console's microsecond grid (multiples of P, 16,666 us - 0.04%
// short of 1/60 s). Frame k begins at time base k * (bus / 4) / hz, exactly
// 1/60 s (1/50 s) apart: the one time base the video retrace and the audio
// passes share (NativeVideo, NativeAudio), so the retrace the game waits on is
// its own frame boundary instead of beating against it every 1,001 frames.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class NativeFrameLimiter {
public:
    // Advances the game clock towards `target` (game ticks), event by event.
    // Returns true once the clock is at target; false when the guest has to
    // run first (an interrupt became deliverable, or a stop was requested).
    using Advance = std::function<bool(u64 target)>;
    // Advances the game clock to its next event and delivers every event due
    // then (System::advance_to_next_event); same return contract.
    using AdvanceEvent = std::function<bool()>;

    NativeFrameLimiter(Cpu& cpu, Advance advance, AdvanceEvent advance_event);
    // Writes the per-frame work log (GEKKO_FRAME_WORK), when one was asked for.
    ~NativeFrameLimiter();

    void install();

    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    struct Stats {
        u64 frames = 0;         // limiter calls completed
        u64 waits = 0;          // frames that had to wait for the grid
        u64 yields = 0;         // waits interrupted to let the guest take an interrupt
        u64 movie_waits = 0;    // movie-mode display-refresh calls
        u64 waited_ticks = 0;   // game time the waits covered
        u64 bypassed = 0;       // frames the object's bypass byte published at once
        u64 immediate = 0;      // no-wait mode or elapsed time already met one period
        u32 last_interval = 0;  // the frame object's interval at the last frame (as requested)
        u64 zero_time = 0;      // no-wait frames that had taken no game time: held to the next event
        // Frames that requested an interval above 1 (the in-engine events'
        // 2) and were paced at one period. Diagnostic only; never sets cadence.
        u64 paced_at_one_period = 0;
    };
    const Stats& stats() const noexcept { return stats_; }

private:
    void enter(Cpu& c);
    void after_movie_wait(Cpu& c);
    void wait(Cpu& c);
    // A frame that would publish at once (bypass, or already due): publishes,
    // or - when no game time passed since the previous frame - lasts until the
    // next event first.
    void publish_now(Cpu& c);
    void resample(Cpu& c, u32 frame);
    static u64 frame_rate(Cpu& c, u32 frame);
    void publish(Cpu& c);
    void after_setter(Cpu& c);

    Cpu& cpu_;
    Advance advance_;
    AdvanceEvent advance_event_;
    Stats stats_;
    // Diagnostic (GEKKO_FRAME_WORK=path): host steady-clock nanoseconds and game ticks at every limiter entry. Headless,
    // the game thread never sleeps, so the host time between two entries is that frame's whole game-thread work.
    std::string work_log_path_;
    std::vector<u64> work_log_;
};

}  // namespace gekko
