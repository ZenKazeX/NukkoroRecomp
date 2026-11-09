#pragma once

// Host timing policy.
//
// This file deliberately contains no guest logic. Game time is the GameClock
// (gekko/game_clock.h), moved only by System::advance_game_time. These classes
// only decide when the host should become idle and when a completed
// presentation snapshot should be shown.

#include <chrono>
#include <cstddef>
#include <vector>

#include "gekko/game_clock.h"
#include "gekko/types.h"

namespace gekko {

class FixedSimulationClock {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    struct Config {
        u64 ticks_per_second = kGameTicksPerSecond;  // the game clock
        // The host is allowed to run a fixed quantum of guest time quickly and
        // sleep once at its boundary. This is pacing granularity only; it does
        // not replace or quantise any guest timer or interrupt.
        u32 pacing_hz = 60;
        std::chrono::nanoseconds sleep_slack{200000};       // 0.2 ms
        // Normal execution never rebases: a costly level transition is still
        // guest time and must be recovered, not converted into slow motion.
        // A 33 ms bound was tried once and reverted because rebasing DISCARDS
        // guest time and reads as permanent slow motion; that decision stands.
        std::chrono::nanoseconds maximum_debt = std::chrono::nanoseconds::max();
        // Ceiling on how fast recovery may run, as a multiple of real time.
        // Zero or one disables the limiter and lets the guest sprint at whatever
        // rate the host can manage until the absolute deadline ladder is caught.
        //
        // MEASURED, 2026-08-31, windowed boot route: with the limiter off, an
        // 808 ms debt built during disc load was repaid at up to 4.79x real
        // time, with a whole 0.5 s window sustained at 2.60x. That is the
        // reported "the game suddenly speeds up". It is a different mechanism
        // from maximum_debt: rate limiting NEVER discards guest time, it only
        // refuses to deliver it faster than this multiple of real time, so the
        // reverted-bound reasoning above does not apply to it.
        //
        // 1.02 is deliberately just above unity. In this port guest time IS
        // retired instructions, so a slow patch costs the player nothing to
        // "make up" - the game's own clock stayed consistent and audio and
        // video both derive from it. The only reason to exceed 1.0 at all is to
        // absorb sleep-timer overshoot without the guest losing ground; 2% is
        // an order of magnitude more than the measured overshoot and is far
        // below the threshold at which a player can see or hear it.
        // Enabled at 1.02, but ONLY with the credit bound below.
        //
        // History, because the failure mode is not obvious. 1.02 was first
        // shipped WITHOUT a credit, i.e. as a per-tick floor, and that is
        // one-sided: in a scene where the guest is chronically behind, a tick
        // that happens to finish quickly is slept back to the floor while the
        // slow ticks it was compensating for earn nothing. Measured in-stage,
        // same binary, alternating runs: 0.887/0.829 of real time with the
        // limiter off against 0.790/0.397 with it on. The 0.397 run was
        // visible as a freeze. test_rate_ceiling_is_inert_when_the_host_cannot
        // _keep_up pins that regression.
        //
        // With the credit, measured over five alternating runs each:
        //   in-stage  off  median 0.807  mean 0.808   (paced 0 every run)
        //   in-stage  on   median 0.810  mean 0.817   - indistinguishable
        //   boot      off  worst 0.5 s bucket 2.54x and 1.67x
        //   boot      on   worst 0.5 s bucket 1.09x and 1.08x
        //
        // --catchup-rate off restores the previous behaviour exactly.
        double maximum_catchup_rate = 1.02;
        // How much accumulated slowness the guest may spend at full host speed
        // before the rate ceiling starts to apply.
        //
        // Without this the ceiling is a PER-TICK floor, and a per-tick floor is
        // one-sided: in a scene the host cannot sustain, a tick that happens to
        // finish quickly is slept back to the floor, while the slow ticks it
        // was compensating for earn nothing. The mean therefore falls BELOW
        // what the host could manage. Measured in-stage at rate 1.02 with zero
        // credit: 0.790 and 0.397 of real time against 0.887 and 0.829 with the
        // limiter off - the 0.397 run was visible as a freeze.
        //
        // With a credit, the release ladder is not allowed to fall further than
        // this behind the present, so a host that is persistently slower than
        // the ceiling never sleeps at all and the limiter is inert exactly
        // where it must be. Two pacing periods is enough to absorb ordinary
        // tick-to-tick jitter and small enough that a sustained sprint is
        // capped almost immediately.
        std::chrono::nanoseconds maximum_catchup_credit{33333333};  // 2 periods
        // A HITCH is one quantum during which the debt grows by more than
        // this - a stage load, a device or shader stall - as opposed to a
        // SUSTAINED shortfall, where every quantum is a few ms slow. A hitch's
        // wall time was already spent on screen as a frozen frame; repaying it
        // afterwards at the catch-up rate does not give the player anything
        // back. What it does do is overproduce guest-slaved audio for
        // debt/0.02 seconds, while the device has already played that stretch
        // as inserted silence - so the queue ratchets up and never drains
        // (docs/audio-2026-09-16/latency-mechanism.md: a 900 ms load left
        // ~820 ms of permanent audio latency plus cap-drop clicks).
        //
        // So the growth of a hitch is forgiven down to hitch_kept_debt, which is
        // exactly the audio prime the device drained during the hitch and must
        // get back. This is NOT the reverted 33 ms debt bound: that bound fired on
        // every slow quantum and turned a sustained 0.81x scene into permanent
        // slow motion. A sustained shortfall grows the debt by ~4 ms per quantum
        // and never crosses this threshold, so it is still repaid exactly as
        // before. No guest cycles are discarded either way - guest time is
        // retired instructions; only the wall anchor moves. Zero disables.
        // DEFAULT OFF until measured and approved; --hitch-forgiveness-ms 100 enables it.
        std::chrono::nanoseconds hitch_forgiveness_threshold{0};
        std::chrono::nanoseconds hitch_kept_debt{80000000};  // the 80 ms audio prime
    };

    struct Decision {
        bool boundary = false;
        bool should_sleep = false;
        bool rebased = false;
        bool debt_clamped = false;
        u64 tick = 0;
        u64 skipped_ticks = 0;
        TimePoint deadline{};
        std::chrono::nanoseconds debt{};
    };

    struct Stats {
        u64 boundaries = 0;
        u64 sleeps = 0;
        u64 behind = 0;
        u64 rebases = 0;
        u64 debt_clamps = 0;
        u64 skipped_ticks = 0;
        std::chrono::nanoseconds maximum_observed_debt{};
        u64 hitches_forgiven = 0;
        std::chrono::nanoseconds hitch_time_forgiven{};
    };

    FixedSimulationClock();
    explicit FixedSimulationClock(Config config);

    void reset(u64 game_ticks, TimePoint now) noexcept;
    Decision observe(u64 game_ticks, TimePoint now) noexcept;
    // Reports where the sleep actually ended. Only a wake late by more than a
    // whole pacing period moves the recovery floor; anything smaller is timer
    // overshoot, and carrying that forward would compound into a permanent
    // speed loss of overshoot/period on every single tick.
    void note_wake(TimePoint actual_wake) noexcept;

    const Config& config() const noexcept { return config_; }
    const Stats& stats() const noexcept { return stats_; }
    bool initialized() const noexcept { return initialized_; }

private:
    u64 tick_for_game_ticks(u64 game_ticks) const noexcept;
    Duration duration_for_ticks(u64 ticks) const noexcept;

    Config config_{};
    Stats stats_{};
    bool initialized_ = false;
    bool debt_incident_open_ = false;
    u64 healthy_rearm_ticks_ = 0;
    u64 guest_origin_ = 0;
    u64 serviced_tick_ = 0;
    TimePoint wall_origin_{};
    TimePoint last_release_{};
    std::chrono::nanoseconds previous_debt_{};
    bool have_previous_debt_ = false;
};

class RenderClock {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    struct Stats {
        u64 frames = 0;
        u64 dropped_deadlines = 0;
        u64 rebases = 0;
    };

    // target_hz == 0 is unlocked. With DXGI vsync enabled, Present supplies the
    // physical display clock; with vsync disabled the caller runs unrestricted.
    explicit RenderClock(double target_hz = 60.0) noexcept;

    void set_target_hz(double target_hz) noexcept;
    void reset(TimePoint now) noexcept;
    bool due(TimePoint now) const noexcept;
    bool consume(TimePoint now) noexcept;
    Duration time_until_deadline(TimePoint now) const noexcept;
    TimePoint next_deadline() const noexcept { return next_deadline_; }
    double target_hz() const noexcept { return target_hz_; }
    bool unlocked() const noexcept { return target_hz_ <= 0.0; }
    const Stats& stats() const noexcept { return stats_; }

private:
    void refresh_period() noexcept;

    double target_hz_ = 60.0;
    std::chrono::nanoseconds period_{16666667};
    TimePoint next_deadline_{};
    bool initialized_ = false;
    Stats stats_{};
};

// The audio device owns a real clock and consumes at its configured sample
// rate. This counter records the source timeline only; it is intentionally not
// connected to FixedSimulationClock and can never advance gameplay.
class AudioClock {
public:
    explicit AudioClock(u32 sample_rate = 32000u) noexcept;
    void reset() noexcept { produced_samples_ = 0; }
    void advance(u64 samples) noexcept { produced_samples_ += samples; }
    u32 sample_rate() const noexcept { return sample_rate_; }
    u64 produced_samples() const noexcept { return produced_samples_; }
    double produced_seconds() const noexcept;

private:
    u32 sample_rate_ = 32000u;
    u64 produced_samples_ = 0;
};

// Causal presentation interpolation. `older` and `newer` are completed XFB
// snapshots, so this can smooth delivery without reading or mutating guest
// state. It deliberately adds one source-frame of presentation latency.
void interpolate_xrgb8888(const std::vector<u32>& older,
                          const std::vector<u32>& newer,
                          double alpha,
                          std::vector<u32>& output);

}  // namespace gekko
