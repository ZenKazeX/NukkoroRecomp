#pragma once

// The phase lock (default ON since 2026-11-04, docs/PERF_60FPS_2026-11-04.md; SHADOW_PHASE_LOCK=0 or --no-phase-lock
// turns it off; docs/TIMING_ARCHITECTURE.md 7-8): the game clock's phase follows the display.
// The render thread measures, per recorded packet, how long the packet waited between the game's push and the start
// of its recording (the swap chain's credit, with the credit-first order), and publishes the median of the last
// frames. The game thread's pacer moves its wall anchor by a bounded step per pacing tick so that this hold settles at
// kTargetHold: the frame is built just before the vblank that will start its GPU work. Game time keeps its own steps;
// only where wall time is mapped onto it moves (at most kMaxSlewPpm, and on average the display's few ppm against QPC).
// Pair with the audio rate trim (default on; SHADOW_AUDIO_RATE_TRIM=0 turns it off) so that the ppm the game clock gains
// is absorbed by the device-side resampler.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace gekko::phase_lock {

inline constexpr std::chrono::nanoseconds kTargetHold{2'000'000};  // push jitter p99 ~0.6 ms + render-thread wake
inline constexpr double kGainPerTick = 0.001;                       // of the hold error, per 1/300 s pacing tick
inline constexpr double kMaxSlewPpm = 1000.0;                       // of wall time, i.e. 3.33 us per pacing tick

inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_PHASE_LOCK");
        return !(v != nullptr && v[0] == '0');
    }();
    return on;
}

// Whether the display paces the frames (V-Sync on): written by the render settings (LiftedRender::set_vsync), read by
// the render thread (it samples holds only then) and the game thread (it steers only then). With V-Sync off a frame is
// presented as soon as it is popped: there is no vblank to lock to, the hold reads ~0, and the controller used to chase
// its 2 ms target for ever - the game clock ran 1000 ppm fast (73 ms per minute) and the audio trim, capped at 300 ppm,
// let the device queue grow ~0.7 ms per second (measured 2026-11-06, docs/FRAME_PACING.md "V-Sync off").
inline std::atomic<bool>& display_paced() {
    static std::atomic<bool> value{true};
    return value;
}

// published_hold_ns() before anything was measured. Any other value is a hold - negative too: with SHADOW_R2_JIT the
// hold is measured against the frame's planned start (render/jit_pacer.h), and a packet pushed after that start holds
// a negative time, which moves the game earlier.
inline constexpr std::int64_t kNothingMeasured = INT64_MIN;

// Render thread -> game thread: the filtered hold in ns (kNothingMeasured: nothing yet).
inline std::atomic<std::int64_t>& published_hold_ns() {
    static std::atomic<std::int64_t> value{kNothingMeasured};
    return value;
}

// Render thread: the median of the last kWindow holds (a late frame or one jittered push does not steer the clock).
class HoldFilter {
public:
    static constexpr std::size_t kWindow = 31;
    void add(std::int64_t hold_ns) {
        ring_[next_++ % kWindow] = hold_ns;
        if (count_ < kWindow)
            ++count_;
        if (count_ < kWindow)
            return;
        std::array<std::int64_t, kWindow> sorted = ring_;
        std::nth_element(sorted.begin(), sorted.begin() + kWindow / 2, sorted.end());
        published_hold_ns().store(sorted[kWindow / 2], std::memory_order_relaxed);
    }

    // Forget every sample (the display pacing changed: the next ones measure against another reference).
    void reset() noexcept { next_ = count_ = 0; }

private:
    std::array<std::int64_t, kWindow> ring_{};
    std::size_t next_ = 0, count_ = 0;
};

// The shared phase controller (2026-11-04, Astra's recommendation; docs/PERF_60FPS_2026-11-04.md). The proportional
// law below (step = 0.001 x hold error per tick) has a ~3.3 s small-signal time constant and chases an error that lags
// the shift it already applied, so after a step in the render lead it took 15-20 s to settle. Instead each measured
// hold is turned into the anchor shift that would have made THAT packet's hold the target:
//     target_shift = shift applied when the packet was pushed + (hold - kTargetHold)
// The median of the last kWindow such targets is independent of the shift applied since, so the game thread can move
// straight towards it at the slew limit (kMaxSlewPpm), with a deadband - no asymptotic tail, no lag overshoot.
// SHADOW_PHASE_LOCK_LEGACY=1: the proportional law (A/B).
inline constexpr std::int64_t kDeadbandNs = 100'000;  // 100 us

inline bool legacy() {
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_PHASE_LOCK_LEGACY");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}

// Render thread -> game thread: the filtered target shift in ns (kNothingMeasured: nothing yet).
inline std::atomic<std::int64_t>& published_target_shift_ns() {
    static std::atomic<std::int64_t> value{kNothingMeasured};
    return value;
}

// Render thread: the median of the last kWindow target shifts.
class TargetFilter {
public:
    static constexpr std::size_t kWindow = 31;
    void add(std::int64_t hold_ns, std::int64_t shift_at_packet_ns) {
        ring_[next_++ % kWindow] = shift_at_packet_ns + (hold_ns - kTargetHold.count());
        if (count_ < kWindow)
            ++count_;
        if (count_ < kWindow)
            return;
        std::array<std::int64_t, kWindow> sorted = ring_;
        std::nth_element(sorted.begin(), sorted.begin() + kWindow / 2, sorted.end());
        published_target_shift_ns().store(sorted[kWindow / 2], std::memory_order_relaxed);
    }

    // Forget every sample (the display pacing changed: the next ones measure against another reference).
    void reset() noexcept { next_ = count_ = 0; }

private:
    std::array<std::int64_t, kWindow> ring_{};
    std::size_t next_ = 0, count_ = 0;
};

// Game thread, at a pacing tick: towards the target at the slew limit (positive: the frames start later).
inline std::chrono::nanoseconds step_to_target(std::int64_t target_ns, std::int64_t applied_ns,
                                               std::chrono::nanoseconds tick_period) {
    if (target_ns == kNothingMeasured)
        return std::chrono::nanoseconds::zero();
    const std::int64_t error = target_ns - applied_ns;
    if (error <= kDeadbandNs && error >= -kDeadbandNs)
        return std::chrono::nanoseconds::zero();
    const auto limit = static_cast<std::int64_t>(static_cast<double>(tick_period.count()) * kMaxSlewPpm * 1e-6);
    return std::chrono::nanoseconds(std::clamp(error, -limit, limit));
}

// Game thread, at a pacing tick: the anchor shift for this tick (positive: the frames start later in wall time).
// The proportional law (SHADOW_PHASE_LOCK_LEGACY=1).
inline std::chrono::nanoseconds step_for(std::int64_t hold_ns, std::chrono::nanoseconds tick_period) {
    if (hold_ns == kNothingMeasured)
        return std::chrono::nanoseconds::zero();
    const double error = static_cast<double>(hold_ns - kTargetHold.count());
    const double limit = static_cast<double>(tick_period.count()) * kMaxSlewPpm * 1e-6;
    return std::chrono::nanoseconds(static_cast<std::int64_t>(std::clamp(error * kGainPerTick, -limit, limit)));
}

}  // namespace gekko::phase_lock
