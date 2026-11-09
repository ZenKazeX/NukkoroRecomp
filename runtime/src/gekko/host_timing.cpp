#include "gekko/host_timing.h"

#include "gekko/profiling.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gekko {

namespace {

std::chrono::nanoseconds nonnegative_ns(FixedSimulationClock::Duration duration) noexcept {
    if (duration <= FixedSimulationClock::Duration::zero()) {
        return std::chrono::nanoseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
}

}  // namespace

FixedSimulationClock::FixedSimulationClock() : FixedSimulationClock(Config{}) {}

FixedSimulationClock::FixedSimulationClock(Config config) : config_(config) {
    // Plot shapes, declared on the only constructor both overloads reach. The
    // millisecond curves interpolate - debt and sleep are continuous quantities
    // sampled once a boundary - while skipped ticks is a count and holds its
    // value between boundaries.
    GEKKO_PROFILE_PLOT_CONFIG("host/pacer debt ms", GEKKO_PLOT_NUMBER, false, true, 0xFF6060);
    GEKKO_PROFILE_PLOT_CONFIG("host/pacer sleep ms", GEKKO_PLOT_NUMBER, false, true, 0x60C0FF);
    GEKKO_PROFILE_PLOT_CONFIG("host/pacer wake overshoot ms", GEKKO_PLOT_NUMBER, false, true,
                              0xFFC060);
    GEKKO_PROFILE_PLOT_CONFIG("host/pacer skipped ticks", GEKKO_PLOT_NUMBER, true, true, 0xC060FF);
    if (config_.ticks_per_second == 0u) {
        config_.ticks_per_second = kGameTicksPerSecond;
    }
    if (config_.pacing_hz == 0u) {
        config_.pacing_hz = 60u;
    }
    if (!std::isfinite(config_.maximum_catchup_rate) ||
        config_.maximum_catchup_rate < 0.0) {
        config_.maximum_catchup_rate = 0.0;
    }
    if (config_.sleep_slack < std::chrono::nanoseconds::zero()) {
        config_.sleep_slack = std::chrono::nanoseconds::zero();
    }
    if (config_.maximum_debt < std::chrono::nanoseconds::zero()) {
        config_.maximum_debt = std::chrono::nanoseconds::zero();
    }
}

void FixedSimulationClock::reset(u64 game_ticks, TimePoint now) noexcept {
    initialized_ = true;
    // The pacing boundaries stay on the absolute grid of game time (every quantum a whole number of quanta from tick
    // 0), wherever the clock is re-anchored - a savestate load, a pause, host startup. Then each frame boundary of the
    // game (a whole number of quanta, docs/FRAME_PACING.md "Pacing on the frame grid") is one. `now` is the wall time
    // of `game_ticks`; the origin is the grid point at or before it.
    const u64 quantum = config_.ticks_per_second / config_.pacing_hz;
    const u64 into = config_.ticks_per_second % config_.pacing_hz == 0u ? game_ticks % quantum : 0u;
    guest_origin_ = game_ticks - into;
    serviced_tick_ = 0;
    wall_origin_ = now - std::chrono::nanoseconds(into * 1000000000ull / config_.ticks_per_second);
    // The catch-up ladder starts at the grid point too: seeded at `now`, the first boundary after a re-anchor waited a
    // whole limiter step from the anchor (3.27 ms at 300 Hz) instead of its grid distance, and the next few followed
    // it off the grid until the 2 % catch-up absorbed it (host_timing_tests: the absolute-grid anchor, 2026-11-06).
    last_release_ = wall_origin_;
    debt_incident_open_ = false;
    healthy_rearm_ticks_ = 0;
    previous_debt_ = std::chrono::nanoseconds::zero();
    have_previous_debt_ = false;
}

u64 FixedSimulationClock::tick_for_game_ticks(u64 cycles) const noexcept {
    // Quotient/remainder form avoids overflowing cycles * pacing_hz during a
    // long session while retaining the exact integer boundary.
    const u64 seconds = cycles / config_.ticks_per_second;
    const u64 remainder = cycles % config_.ticks_per_second;
    const u64 whole_ticks = seconds * config_.pacing_hz;
    const u64 partial_ticks = remainder * config_.pacing_hz /
                              config_.ticks_per_second;
    return whole_ticks + partial_ticks;
}

FixedSimulationClock::Duration FixedSimulationClock::duration_for_ticks(u64 ticks) const noexcept {
    const u64 seconds = ticks / config_.pacing_hz;
    const u64 remainder = ticks % config_.pacing_hz;
    const u64 nanoseconds = remainder * 1000000000ull / config_.pacing_hz;
    const auto max_seconds = static_cast<u64>(
        (std::numeric_limits<std::chrono::seconds::rep>::max)());
    const auto safe_seconds = static_cast<std::chrono::seconds::rep>(
        (std::min)(seconds, max_seconds));
    return std::chrono::seconds(safe_seconds) + std::chrono::nanoseconds(nanoseconds);
}

FixedSimulationClock::Decision FixedSimulationClock::observe(u64 game_ticks,
                                                             TimePoint now) noexcept {
    Decision decision;
    decision.deadline = now;
    if (!initialized_ || game_ticks < guest_origin_) {
        // Its own message: this path is the first observe of a run or guest
        // cycles going backwards - a savestate load - and letting it share the
        // debt rebase's literal would make a state load read as an overload
        // incident on the timeline.
        GEKKO_PROFILE_MESSAGE_LITERAL("host/pacer: anchor reset");
        reset(game_ticks, now);
        decision.rebased = true;
        return decision;
    }

    const u64 tick = tick_for_game_ticks(game_ticks - guest_origin_);
    if (tick <= serviced_tick_) {
        return decision;
    }

    decision.boundary = true;
    decision.tick = tick;
    decision.skipped_ticks = tick - serviced_tick_ - 1u;
    serviced_tick_ = tick;
    ++stats_.boundaries;
    stats_.skipped_ticks += decision.skipped_ticks;
    // Flat at zero in a healthy run. The lifetime sum cannot tell one 12-tick
    // stall from twelve one-tick stutters; this can.
    GEKKO_PROFILE_PLOT("host/pacer skipped ticks",
                       static_cast<double>(decision.skipped_ticks));

    TimePoint ideal = wall_origin_ + duration_for_ticks(tick);
    auto debt = nonnegative_ns(now - ideal);
    if (debt > stats_.maximum_observed_debt) {
        stats_.maximum_observed_debt = debt;
    }

    // Host hitch: forgive the growth of this single quantum down to the kept
    // debt. See Config::hitch_forgiveness_threshold for why this is not the
    // reverted debt bound.
    if (config_.hitch_forgiveness_threshold > std::chrono::nanoseconds::zero()) {
        if (have_previous_debt_ && debt > previous_debt_) {
            const auto growth = debt - previous_debt_;
            if (growth > config_.hitch_forgiveness_threshold &&
                growth > config_.hitch_kept_debt) {
                const auto forgive = growth - config_.hitch_kept_debt;
                wall_origin_ += forgive;
                ideal += forgive;
                debt -= forgive;
                ++stats_.hitches_forgiven;
                stats_.hitch_time_forgiven += forgive;
                GEKKO_PROFILE_MESSAGE_LITERAL("host/pacer: hitch forgiven");
            }
        }
        previous_debt_ = debt;
        have_previous_debt_ = true;
    }

    // A large discontinuity starts one debt incident. Re-anchoring on every
    // threshold crossing creates a sawtooth of sleep/sprint/resync; after the
    // first rebase, persistent overload is instead clamped and allowed to run
    // without sleeping. Five healthy seconds are required before another
    // unrelated hitch may arm a new rebase.
    if (debt > config_.maximum_debt) {
        if (debt_incident_open_) {
            const auto excess = debt - config_.maximum_debt;
            wall_origin_ += excess;
            decision.debt = config_.maximum_debt;
            decision.debt_clamped = true;
            decision.deadline = now;
            last_release_ = now;
            healthy_rearm_ticks_ = 0;
            ++stats_.behind;
            ++stats_.debt_clamps;
            GEKKO_PROFILE_MESSAGE_LITERAL("host/pacer: debt clamp");
            // The OBSERVED debt, not the clamped value handed back: the clamp
            // is what the pacer decided, the observation is how far behind the
            // host actually is, and only the second one is a curve.
            GEKKO_PROFILE_PLOT("host/pacer debt ms",
                               static_cast<double>(debt.count()) / 1.0e6);
            return decision;
        }
        wall_origin_ += now - ideal;
        last_release_ = now;
        debt_incident_open_ = true;
        healthy_rearm_ticks_ = 0;
        decision.rebased = true;
        decision.deadline = now;
        decision.debt = std::chrono::nanoseconds::zero();
        ++stats_.rebases;
        GEKKO_PROFILE_MESSAGE_LITERAL("host/pacer: rebase");
        GEKKO_PROFILE_PLOT("host/pacer debt ms",
                           static_cast<double>(debt.count()) / 1.0e6);
        return decision;
    }

    if (debt_incident_open_) {
        const auto healthy_threshold = config_.maximum_debt / 4;
        if (debt <= healthy_threshold) {
            ++healthy_rearm_ticks_;
            const u64 rearm_ticks = static_cast<u64>(config_.pacing_hz) * 5u;
            if (healthy_rearm_ticks_ >= rearm_ticks) {
                debt_incident_open_ = false;
                healthy_rearm_ticks_ = 0;
            }
        } else {
            healthy_rearm_ticks_ = 0;
        }
    }

    decision.debt = debt;
    if (debt > std::chrono::nanoseconds::zero()) {
        ++stats_.behind;
    }

    decision.deadline = ideal;
    const bool limiting = config_.maximum_catchup_rate > 1.0;
    Duration step = Duration::zero();
    if (limiting) {
        const auto nominal_period = duration_for_ticks(1u);
        const auto nominal_ns = std::chrono::duration<double, std::nano>(nominal_period).count();
        const auto minimum_ns = static_cast<long long>(
            std::llround(nominal_ns / config_.maximum_catchup_rate));
        step = std::chrono::nanoseconds((std::max)(1ll, minimum_ns));

        // The release ladder may not fall further than the credit behind the
        // present. This is what keeps the ceiling from being one-sided: when
        // the host is persistently slower than the ceiling the ladder would
        // otherwise sink arbitrarily far into the past, and every tick that
        // happened to be quick would be slept back to it while the slow ticks
        // earned no credit at all. Clamped, `last_release_ + step` stays behind
        // `now` in that regime, so the limiter never sleeps and is exactly as
        // inert as it must be.
        const TimePoint earliest = now - config_.maximum_catchup_credit;
        if (last_release_ < earliest) {
            last_release_ = earliest;
        }
        decision.deadline = (std::max)(ideal, last_release_ + step);
    }

    if (decision.deadline > now + config_.sleep_slack) {
        decision.should_sleep = true;
        // What the pacer ASKED for. The sleep itself happens in shadow_main, so
        // this plus the overshoot recorded in note_wake is how the limiter's
        // sleep becomes visible from inside this subsystem.
        GEKKO_PROFILE_PLOT("host/pacer sleep ms",
                           static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   decision.deadline - now)
                                                   .count()) /
                               1.0e6);
        last_release_ = decision.deadline;
        ++stats_.sleeps;
    } else {
        decision.deadline = now;
        // The ladder advances by one step per tick whether or not this tick
        // slept. Snapping it to `now` here was the other half of the one-sided
        // bug: it threw away the credit a slow tick had just earned, so the
        // next quick tick was capped as if nothing had been lost.
        last_release_ = limiting ? last_release_ + step : now;
    }
    // The normal boundary exit. maximum_observed_debt is a lifetime maximum
    // with no time axis, and the catch-up ratchet IS a debt curve - this is
    // that curve, sampled once per pacing tick.
    GEKKO_PROFILE_PLOT("host/pacer debt ms",
                       static_cast<double>(debt.count()) / 1.0e6);
    return decision;
}

void FixedSimulationClock::note_wake(TimePoint actual_wake) noexcept {
    // A wake later than a whole pacing period means the host lost the thread -
    // an OS deschedule, not the timer. Re-anchor there so the pacer does not
    // then hand back the whole lost interval as one burst.
    //
    // A SMALLER overshoot must NOT move the floor. The recovery floor is
    // `last_release_ + period / rate`, so carrying an overshoot of e forward
    // adds e to every subsequent release: the guest would permanently run at
    // period / (period + e) of real time. At the measured overshoot this
    // silently cost more speed than the 2% of headroom the rate limiter is
    // there to provide. Absorbing it against the absolute ladder instead costs
    // nothing, because `deadline` is still max(ideal, floor) and `ideal` never
    // moves.
    const auto slip_limit = duration_for_ticks(1u);
    // The true scheduler overshoot: the should_sleep branch set last_release_
    // to the deadline it asked to be woken at, so this difference needs no
    // extra state. The comment above argues that carrying it forward costs a
    // permanent overshoot/period speed loss; nothing recorded its SIZE.
    GEKKO_PROFILE_PLOT("host/pacer wake overshoot ms",
                       static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               actual_wake - last_release_)
                                               .count()) /
                           1.0e6);
    if (actual_wake > last_release_ + slip_limit) {
        last_release_ = actual_wake;
    }
}

RenderClock::RenderClock(double target_hz) noexcept : target_hz_(target_hz) {
    // Configured here rather than alongside the pacer's plots: this is the only
    // constructor of the only class that can emit this point, so a RenderClock
    // built without a FixedSimulationClock still draws it as a count.
    GEKKO_PROFILE_PLOT_CONFIG("host/render dropped deadlines", GEKKO_PLOT_NUMBER, true, true,
                              0xFF4040);
    refresh_period();
}

void RenderClock::refresh_period() noexcept {
    if (!std::isfinite(target_hz_) || target_hz_ < 0.0) {
        target_hz_ = 60.0;
    }
    if (target_hz_ <= 0.0) {
        period_ = std::chrono::nanoseconds::zero();
        return;
    }
    const auto ns = static_cast<long long>(std::llround(1.0e9 / target_hz_));
    period_ = std::chrono::nanoseconds((std::max)(1ll, ns));
}

void RenderClock::set_target_hz(double target_hz) noexcept {
    target_hz_ = target_hz;
    refresh_period();
    initialized_ = false;
}

void RenderClock::reset(TimePoint now) noexcept {
    initialized_ = true;
    next_deadline_ = now;
}

bool RenderClock::due(TimePoint now) const noexcept {
    return !initialized_ || unlocked() || now >= next_deadline_;
}

bool RenderClock::consume(TimePoint now) noexcept {
    if (!initialized_) {
        reset(now);
    }
    if (!due(now)) {
        return false;
    }
    ++stats_.frames;
    if (unlocked()) {
        next_deadline_ = now;
        return true;
    }

    next_deadline_ += period_;
    if (next_deadline_ <= now) {
        const auto late = now - next_deadline_;
        const auto missed = static_cast<u64>(late / period_) + 1u;
        stats_.dropped_deadlines += missed;
        // RenderClock deliberately never bursts through missed deadlines, so
        // this is the only record that a presentation slot was abandoned.
        GEKKO_PROFILE_PLOT("host/render dropped deadlines", static_cast<double>(missed));
        ++stats_.rebases;
        // Never burst through missed render frames. Rendering has no authority
        // over simulation, so a late presentation deadline is simply dropped.
        next_deadline_ = now + period_;
    }
    return true;
}

RenderClock::Duration RenderClock::time_until_deadline(TimePoint now) const noexcept {
    if (!initialized_ || unlocked() || now >= next_deadline_) {
        return Duration::zero();
    }
    return next_deadline_ - now;
}

AudioClock::AudioClock(u32 sample_rate) noexcept
    : sample_rate_(sample_rate == 0u ? 32000u : sample_rate) {}

double AudioClock::produced_seconds() const noexcept {
    return static_cast<double>(produced_samples_) / static_cast<double>(sample_rate_);
}

void interpolate_xrgb8888(const std::vector<u32>& older,
                          const std::vector<u32>& newer,
                          double alpha,
                          std::vector<u32>& output) {
    if (newer.empty()) {
        output.clear();
        return;
    }
    if (older.size() != newer.size() || alpha >= 1.0) {
        output = newer;
        return;
    }
    if (alpha <= 0.0) {
        output = older;
        return;
    }

    // Opened below the three early-outs so it covers only the work: the resize
    // and a serial four-channel lerp over the whole XFB, on the presenting
    // thread. No counter of any kind exists for it, and it is off entirely
    // unless frame interpolation was asked for.
    GEKKO_PROFILE_ZONE("host/xfb-interpolate");
    GEKKO_PROFILE_VALUE(newer.size());
    const u32 weight = static_cast<u32>(std::lround(std::clamp(alpha, 0.0, 1.0) * 256.0));
    const u32 inverse = 256u - weight;
    output.resize(newer.size());
    for (std::size_t index = 0; index < newer.size(); ++index) {
        const u32 a = older[index];
        const u32 b = newer[index];
        u32 mixed = 0;
        for (u32 shift = 0; shift < 32u; shift += 8u) {
            const u32 av = (a >> shift) & 0xffu;
            const u32 bv = (b >> shift) & 0xffu;
            mixed |= ((av * inverse + bv * weight + 128u) >> 8u) << shift;
        }
        output[index] = mixed;
    }
}

}  // namespace gekko
