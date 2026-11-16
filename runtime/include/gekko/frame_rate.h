#pragma once

// The game's frame rate above the console's 60 Hz (docs/HIGH_FRAME_RATE.md).
//
// The game already runs at the rate in its frame object (0x8057E760 + 0x50): 50 or 60 on the console. Its frame
// period, dt and per-step scaling (for example the effects' 60 / hz) all follow that word. A high frame rate publishes
// a multiple of 60 there instead, and the parts of the platform that are tied to the frame follow it:
//   - the frame limiter waits one period of that rate (NativeFrameLimiter::set_frame_rate);
//   - the 60 Hz video field becomes one frame of that rate, so the retrace the game also waits on is its own frame
//     boundary (NativeVideo::set_frame_rate);
//   - the host's real-time pacing ticks come five times per frame, so every frame boundary is one (System).
// 60 is the console's rate and changes nothing: every word, event and tick is as before.
//
// Code that counts frames instead of time still runs faster at a higher rate. Those sites are listed, and fixed one by
// one, in docs/HIGH_FRAME_RATE.md.

#include "gekko/game_clock.h"
#include "gekko/types.h"

namespace gekko::frame_rate {

inline constexpr u32 kConsole = 60u;
inline constexpr u32 kSupported[] = {60u, 120u, 180u, 240u};

constexpr bool supported(u32 hz) noexcept {
    for (const u32 rate : kSupported)
        if (rate == hz) return true;
    return false;
}

// A rate of the 60 Hz family: the console's 60 or a supported multiple of it.
constexpr bool sixty_family(u32 hz) noexcept { return supported(hz); }

// The game's own rate, from the frame object's word. A supported high rate there was published by the limiter on top of
// the game's 60 (the game itself only ever writes 50 or 60), so it reads as 60 - also after a savestate made at a high
// rate is loaded into a session at another one. Any other value is the game's own and is returned unchanged.
constexpr u32 game_rate(u32 word) noexcept { return sixty_family(word) ? kConsole : word; }

// Real-time pacing ticks per second at a frame rate: 300 for the console's 50 and 60 Hz (1/300 s divides both frame
// periods), five per frame above that. Every value divides the game clock's 40.5 MHz, the 1/60 s and 1/50 s frame
// and the rate's own frame (static_asserts below).
constexpr u32 pacing_hz(u32 hz) noexcept { return hz <= kConsole ? 300u : 5u * hz; }

// The frame step cap at `hz` that keeps the game's slowdown point where the console had it. The game caps a frame's
// dt at `cap` periods (frame object + 0x34): `cap` / 60 s on the console (50 ms for its 3). At a higher rate the same
// number of periods would be a shorter time, and a frame of ordinary length would already be slowed down, so the cap
// grows with the rate: cap * hz / 60 periods are the same time.
constexpr u32 scaled_cap(u32 cap, u32 game_hz, u32 hz) noexcept {
    if (game_hz == 0u || hz <= game_hz || hz % game_hz != 0u) return cap;
    return cap * (hz / game_hz);
}

namespace detail {
constexpr bool grid_ok(u32 hz) noexcept {
    const u32 pacing = pacing_hz(hz);
    if (kGameTicksPerSecond % pacing != 0u) return false;
    const u64 tick = kGameTicksPerSecond / pacing;
    return (kGameTicksPerSecond / 60u) % tick == 0u && (kGameTicksPerSecond / 50u) % tick == 0u &&
           kGameTicksPerSecond % hz == 0u && (kGameTicksPerSecond / hz) % tick == 0u;
}
constexpr bool all_grids_ok() noexcept {
    for (const u32 rate : kSupported)
        if (!grid_ok(rate)) return false;
    return grid_ok(50u);
}
}  // namespace detail
static_assert(detail::all_grids_ok(), "every pacing grid holds every frame boundary of the 50 Hz, 60 Hz and its own rate");
static_assert(pacing_hz(60u) == 300u && pacing_hz(50u) == 300u, "the console rates keep the 1/300 s grid");

}  // namespace gekko::frame_rate
