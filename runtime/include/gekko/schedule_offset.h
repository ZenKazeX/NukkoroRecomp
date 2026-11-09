#pragma once

// How far ahead of its neutral schedule the game runs, shared by the renderer, the game thread's pacer and the audio
// device thread (2026-11-05, docs/PERF_60FPS_2026-11-04.md "Burst scheduling").
//
// The render thread's burst modes (render/burst_ahead.h) ask the game to run a lead earlier than the phase lock's
// schedule: a whole frame (render-ahead, two frames in flight) or a few ms (burst lead, eager start). The game thread
// applies the difference between the desired and the applied lead to its pacer's wall anchor at a pacing boundary,
// and re-applies it after anything that re-anchors the pacer (a pause, a savestate load, a pacer reset) - so the
// applied lead is always what the anchor carries.
//
// A lead moves audio too, as it must for audio and picture to stay together: the game produces its audio that much
// earlier, so the device queue holds that much more. The audio thread therefore primes and trims against the queue
// MINUS the applied lead (the neutral queue): a prime or a re-prime during a lead ends with the neutral queue at the
// prime, and leaving the lead takes back exactly what entering added. The render thread leaves a lead only when the
// queue holds it (audio_queued_ns), so a lead is never left into an underrun.

#include <atomic>
#include <cstdint>

namespace gekko::schedule_offset {

// Render thread: the lead it wants (ns, >= 0; 0 = the neutral schedule).
inline std::atomic<std::int64_t>& desired_lead_ns() {
    static std::atomic<std::int64_t> value{0};
    return value;
}

// Game thread: the lead its pacer's anchor carries now (ns, >= 0).
inline std::atomic<std::int64_t>& applied_lead_ns() {
    static std::atomic<std::int64_t> value{0};
    return value;
}

// Audio device thread: the queued game audio at its last wake (ns of source audio; -1 = no audio device running).
inline std::atomic<std::int64_t>& audio_queued_ns() {
    static std::atomic<std::int64_t> value{-1};
    return value;
}

// The applied lead in source frames of a `rate_num / rate_den` Hz stream (audio thread).
inline std::uint64_t applied_lead_frames(std::uint64_t rate_num, std::uint64_t rate_den) {
    const std::int64_t lead = applied_lead_ns().load(std::memory_order_acquire);
    if (lead <= 0)
        return 0;
    return static_cast<std::uint64_t>(lead) * rate_num / (rate_den * 1'000'000'000ull);
}

}  // namespace gekko::schedule_offset
