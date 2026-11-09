#include "gekko/host_timing.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_fixed_simulation_clock_batches_then_sleeps() {
    gekko::FixedSimulationClock::Config config;
    config.sleep_slack = std::chrono::nanoseconds::zero();
    gekko::FixedSimulationClock clock(config);
    const Clock::time_point epoch{};
    clock.reset(0u, epoch);

    // A tenth of a quantum of game time (1.67 ms).
    const auto within = clock.observe(gekko::kGameTicksPerSecond / 600u,
                                      epoch + std::chrono::milliseconds(1));
    require(!within.boundary && !within.should_sleep,
            "a host-service call inside a fixed quantum must not pace or poll");

    const auto first = clock.observe(gekko::kGameTicksPerSecond / 60u,
                                     epoch + std::chrono::milliseconds(2));
    require(first.boundary && first.should_sleep && first.tick == 1u,
            "one guest quantum must produce one efficient host wait");
    require(first.deadline == epoch + std::chrono::nanoseconds(16666666),
            "the first fixed deadline must be derived from the guest clock exactly");
    clock.note_wake(first.deadline);

    for (gekko::u64 tick = 2; tick <= 60; ++tick) {
        const gekko::u64 cycles = tick * (gekko::kGameTicksPerSecond / 60u);
        const auto decision = clock.observe(cycles, epoch + std::chrono::milliseconds(2));
        require(decision.boundary && decision.should_sleep && decision.tick == tick,
                "every completed fixed quantum must retain its absolute deadline");
        clock.note_wake(decision.deadline);
    }
    require(clock.stats().boundaries == 60u && clock.stats().sleeps == 60u &&
                clock.stats().skipped_ticks == 0u,
            "one guest second must be sixty pacing boundaries, not thousands of polls");
}

void test_fixed_simulation_clock_bounds_catchup_and_debt() {
    gekko::FixedSimulationClock::Config config;
    config.sleep_slack = std::chrono::nanoseconds::zero();
    config.maximum_debt = std::chrono::milliseconds(100);
    config.maximum_catchup_rate = 1.05;
    // Left at the default credit. Zero credit is NOT a supported configuration:
    // with no credit the ladder is clamped to `now` every tick, so even an
    // on-time boundary is pushed a whole step into the future and sleeps. That
    // degenerate form is precisely the one-sided behaviour that made the
    // ceiling unusable in-stage.
    gekko::FixedSimulationClock clock(config);
    const Clock::time_point epoch{};
    const gekko::u64 quantum = gekko::kGameTicksPerSecond / 60u;
    clock.reset(0u, epoch);

    auto first = clock.observe(quantum, epoch + std::chrono::nanoseconds(16666666));
    require(first.boundary && !first.should_sleep, "an on-time boundary must not sleep again");

    const auto hitch_time = epoch + std::chrono::nanoseconds(83333332);
    auto second = clock.observe(quantum * 2u, hitch_time);
    require(second.boundary && !second.should_sleep && second.debt >= std::chrono::milliseconds(49),
            "a short hitch may consume one immediately available catch-up quantum");

    // Recovery may spend the credit first, so the ceiling does not necessarily
    // engage on the very next tick - but it must engage promptly once the
    // credit is gone, and when it does the wait must be a real one.
    bool engaged = false;
    auto probe = hitch_time;
    for (gekko::u64 tick = 3u; tick < 13u && !engaged; ++tick) {
        probe += std::chrono::milliseconds(1);
        const auto decision = clock.observe(quantum * tick, probe);
        require(decision.boundary, "recovery lost a fixed boundary");
        if (decision.should_sleep) {
            engaged = true;
            require(decision.deadline - probe >= std::chrono::milliseconds(10),
                    "the rate ceiling engaged but did not actually hold the guest back");
        }
    }
    require(engaged,
            "continued recovery must be rate-limited instead of sprinting at host speed");

    const auto severe = clock.observe(quantum * 13u, epoch + std::chrono::milliseconds(400));
    require(severe.boundary && severe.rebased && !severe.should_sleep &&
                clock.stats().rebases == 1u,
            "large debt must be forgiven once rather than creating runaway catch-up");

    // Persistent overload is one incident, not an endless series of rebases.
    // It must stop sleeping and clamp the diagnostic debt while the host does
    // all useful work it can.
    auto overloaded_now = epoch + std::chrono::milliseconds(430);
    for (gekko::u64 tick = 14u; tick <= 400u; ++tick) {
        overloaded_now += std::chrono::milliseconds(30);
        const auto overloaded = clock.observe(quantum * tick, overloaded_now);
        require(overloaded.boundary, "sustained overload lost a fixed boundary");
    }
    require(clock.stats().rebases == 1u && clock.stats().debt_clamps > 0u,
            "persistent overload must remain one hysteretic debt incident");
}

gekko::u64 count_render_frames(double rate, std::chrono::seconds span) {
    gekko::RenderClock clock(rate);
    const Clock::time_point epoch{};
    clock.reset(epoch);
    const auto end = epoch + span;
    while (clock.next_deadline() < end) {
        const auto now = clock.next_deadline();
        require(clock.due(now) && clock.consume(now), "a render deadline must be consumable");
    }
    return clock.stats().frames;
}

// Replays one pacing scenario and returns the wall time the clock allowed the
// guest to consume across the RECOVERY WINDOW - first release to last release,
// which spans `ticks - 1` pacing periods. The opening hitch is deliberately
// outside that span: it is host time the guest already lost, and including it
// would measure the hitch instead of the recovery policy. The host is modelled
// as finishing every tick's work instantly, so the only thing setting the pace
// is the clock. `wake_overshoot` is how late each sleep actually ends.
double replay_recovery(double catchup_rate, gekko::u64 ticks,
                       std::chrono::nanoseconds wake_overshoot,
                       std::chrono::nanoseconds initial_debt,
                       std::chrono::nanoseconds credit,
                       double* fastest_tick_ratio, gekko::FixedSimulationClock::Stats* out) {
    gekko::FixedSimulationClock::Config config;
    config.sleep_slack = std::chrono::nanoseconds::zero();
    config.maximum_catchup_rate = catchup_rate;
    config.maximum_catchup_credit = credit;
    // The shipped policy: debt is never forgiven, so no guest time is discarded
    // and the rate limiter is the only thing shaping recovery.
    config.maximum_debt = std::chrono::nanoseconds::max();
    gekko::FixedSimulationClock clock(config);

    const Clock::time_point epoch{};
    const gekko::u64 quantum = gekko::kGameTicksPerSecond / 60u;
    const auto period = std::chrono::nanoseconds(1000000000ull / 60ull);
    clock.reset(0u, epoch);

    // One hitch that puts the guest `initial_debt` behind the absolute ladder.
    auto now = epoch + initial_debt;
    auto first = clock.observe(quantum, now);
    require(first.boundary && !first.rebased,
            "an unbounded-debt clock must never rebase on a hitch");

    const auto first_release = now;
    auto previous_release = now;
    double fastest = 0.0;
    for (gekko::u64 tick = 2u; tick <= ticks; ++tick) {
        // The host finished this tick's work the instant it was released.
        const auto decision = clock.observe(quantum * tick, now);
        require(decision.boundary, "recovery lost a fixed boundary");
        if (decision.should_sleep) {
            now = decision.deadline;
            const auto released = now;
            now += wake_overshoot;
            clock.note_wake(now);
            const double ratio =
                std::chrono::duration<double>(period).count() /
                std::chrono::duration<double>(released - previous_release).count();
            if (ratio > fastest) fastest = ratio;
            previous_release = released;
        } else {
            const double ratio =
                std::chrono::duration<double>(period).count() /
                (std::max)(1.0e-12,
                           std::chrono::duration<double>(now - previous_release).count());
            if (ratio > fastest) fastest = ratio;
            previous_release = now;
        }
    }
    if (fastest_tick_ratio != nullptr) *fastest_tick_ratio = fastest;
    if (out != nullptr) *out = clock.stats();
    return std::chrono::duration<double>(previous_release - first_release).count();
}

// THE REGRESSION THIS EXISTS FOR. Enabling the rate ceiling by default made
// the game run at 0.397 of real time in-stage - visible as a freeze - because a
// per-tick floor sleeps the quick ticks of a chronically-slow scene while the
// slow ticks they were compensating for earn no credit.
//
// Contract: a host that is PERSISTENTLY SLOWER than the ceiling must never be
// slept, no matter how much its per-tick time varies around that average.
void test_rate_ceiling_is_inert_when_the_host_cannot_keep_up() {
    gekko::FixedSimulationClock::Config config;
    config.sleep_slack = std::chrono::nanoseconds::zero();
    config.maximum_catchup_rate = 1.02;
    config.maximum_debt = std::chrono::nanoseconds::max();
    gekko::FixedSimulationClock clock(config);

    const Clock::time_point epoch{};
    const gekko::u64 quantum = gekko::kGameTicksPerSecond / 60u;
    clock.reset(0u, epoch);

    // 20 ms average against a 16.67 ms period - 0.83x, the measured in-stage
    // rate - with heavy alternating jitter so that half the ticks finish well
    // inside the 16.34 ms ceiling floor. Those are the ticks the broken version
    // slept.
    // Start behind. A guest that is AHEAD of the absolute ladder is paced by
    // `ideal`, which is correct and is not what this test is about; the
    // in-stage regime is one that fell behind and stayed behind.
    auto now = epoch + std::chrono::milliseconds(500);
    clock.observe(quantum, now);
    for (gekko::u64 tick = 2u; tick <= 600u; ++tick) {
        now += std::chrono::microseconds((tick % 2u) == 0u ? 32000 : 8000);
        const auto decision = clock.observe(quantum * tick, now);
        require(decision.boundary, "a slow host lost a fixed boundary");
        require(!decision.should_sleep,
                "the rate ceiling slept a host that is already slower than the ceiling");
    }
    require(clock.stats().sleeps == 0u,
            "a persistently slow host must never sleep under the rate ceiling");
}

// The measured defect, 2026-08-31: on the windowed boot route an 808 ms pacing
// debt was repaid at up to 4.79x real time, a whole half-second averaging 2.60x.
// Rate-limited recovery must make that impossible WITHOUT discarding guest time
// (no rebase, no debt clamp, no skipped boundary), which is what distinguishes
// it from the maximum_debt bound that was tried and reverted.
void test_recovery_is_rate_limited_and_discards_no_guest_time() {
    const gekko::u64 ticks = 600u;
    double fastest = 0.0;
    gekko::FixedSimulationClock::Stats stats{};
    const double wall = replay_recovery(1.02, ticks, std::chrono::nanoseconds::zero(),
                                        std::chrono::milliseconds(800),
                                        std::chrono::nanoseconds(1), &fastest, &stats);

    require(fastest <= 1.021,
            "rate-limited recovery let a single tick run faster than the ceiling");
    // The window spans ticks - 1 pacing periods of guest time. The clock's own
    // period is duration_for_ticks(1) = 16,666,666 ns and the floor it derives
    // is llround(period / rate), so the achievable floor sits under the exact
    // real-time expression by well under a nanosecond per tick. One nanosecond
    // per tick of slack covers that truncation and nothing larger; the
    // per-tick ceiling assertion above is the sharp one.
    const double periods = static_cast<double>(ticks - 1u);
    const double floor_seconds = periods / 60.0 / 1.02 - periods * 1.0e-9;
    require(wall >= floor_seconds,
            "the guest recovered faster than the ceiling over the whole window");
    require(stats.rebases == 0u && stats.debt_clamps == 0u && stats.skipped_ticks == 0u,
            "rate-limited recovery must not discard guest time or skip a boundary");
    require(stats.boundaries == ticks,
            "every pacing tick must still be serviced while recovering");

    // Control arm: the shipped-before behaviour. If this did not sprint, the
    // assertion above would be vacuous.
    double unlimited_fastest = 0.0;
    replay_recovery(0.0, ticks, std::chrono::nanoseconds::zero(),
                    std::chrono::milliseconds(800), std::chrono::nanoseconds(1),
                    &unlimited_fastest, nullptr);
    require(unlimited_fastest > 2.0,
            "the control arm did not reproduce unbounded catch-up, so this test proves nothing");
}

// The sleep ends a little after the deadline it asked for. Folding that
// overshoot into the recovery floor adds it to EVERY subsequent release, so the
// guest would settle at period/(period+overshoot) of real time - a silent
// permanent slowdown larger than the headroom the limiter exists to provide.
void test_wake_overshoot_does_not_compound_into_slow_motion() {
    const gekko::u64 ticks = 600u;
    double fastest = 0.0;
    // The shipping-shaped credit. With a near-zero credit the ladder is clamped
    // to the present every tick, which folds the overshoot back in and
    // reintroduces exactly the compounding this test exists to catch - so the
    // credit bound and the note_wake rule are two halves of one fix.
    const double wall = replay_recovery(1.02, ticks, std::chrono::microseconds(500),
                                        std::chrono::milliseconds(800),
                                        std::chrono::nanoseconds(33333333), &fastest, nullptr);
    const double guest_seconds = static_cast<double>(ticks - 1u) / 60.0;
    const double achieved = guest_seconds / wall;
    require(achieved >= 1.015,
            "sleep-timer overshoot compounded into the recovery floor and cost guest speed");
    // No per-tick bound, for the same reason as the credited test above: the
    // one-off credit spend releases several ticks with no wall gap.
    (void)fastest;
}

// The credited form: the credit is a ONE-OFF burst allowance, so a few ticks
// after a hitch may exceed the ceiling while it is spent, but the sustained
// rate over any long window must still be the ceiling.
void test_catchup_credit_is_a_one_off_burst_not_a_sustained_rate() {
    const gekko::u64 ticks = 600u;
    double fastest = 0.0;
    const auto credit = std::chrono::nanoseconds(33333333);
    const double wall = replay_recovery(1.02, ticks, std::chrono::nanoseconds::zero(),
                                        std::chrono::milliseconds(800), credit,
                                        &fastest, nullptr);
    const double periods = static_cast<double>(ticks - 1u);
    // Everything the credit can buy, once, over the whole window.
    const double credit_seconds = std::chrono::duration<double>(credit).count();
    const double floor_seconds = periods / 60.0 / 1.02 - credit_seconds - periods * 1.0e-9;
    require(wall >= floor_seconds,
            "credited recovery sustained a rate above the ceiling");
    // Deliberately NO per-tick bound here. While the credit is being spent the
    // ticks release with no wall gap between them, so a per-tick ratio is
    // unbounded by construction and says nothing. That is what a burst
    // allowance IS; the aggregate above is the contract that matters. The
    // strict per-tick ceiling is pinned by the near-zero-credit test above.
    (void)fastest;
}

// AUD-1 (docs/audio-2026-09-16/latency-mechanism.md). A host HITCH - one quantum
// whose debt grows by more than the threshold, e.g. a stage load - is forgiven
// down to the kept debt (the audio prime the device drained). A SUSTAINED
// shortfall is repaid exactly as before. Both arms are pinned here.
namespace {

gekko::FixedSimulationClock::Config hitch_config(std::chrono::nanoseconds threshold) {
    gekko::FixedSimulationClock::Config config;
    config.sleep_slack = std::chrono::nanoseconds::zero();
    config.maximum_catchup_rate = 1.02;
    config.maximum_debt = std::chrono::nanoseconds::max();
    config.hitch_forgiveness_threshold = threshold;
    config.hitch_kept_debt = std::chrono::milliseconds(80);
    return config;
}

Clock::time_point on_schedule(Clock::time_point epoch, gekko::u64 tick) {
    // Exactly FixedSimulationClock::duration_for_ticks for pacing_hz 60.
    return epoch + std::chrono::nanoseconds(static_cast<long long>(tick) * 1000000000ll / 60ll);
}

double ms(std::chrono::nanoseconds value) {
    return std::chrono::duration<double, std::milli>(value).count();
}

}  // namespace

void test_single_host_hitch_is_forgiven_down_to_the_kept_debt() {
    gekko::FixedSimulationClock clock(hitch_config(std::chrono::milliseconds(100)));
    const Clock::time_point epoch{};
    const gekko::u64 quantum = gekko::kGameTicksPerSecond / 60u;
    clock.reset(0u, epoch);
    for (gekko::u64 tick = 1u; tick <= 60u; ++tick) {
        clock.observe(quantum * tick, on_schedule(epoch, tick));
    }
    // One quantum arrives 900 ms late: a stage load.
    const auto decision =
        clock.observe(quantum * 61u, on_schedule(epoch, 61u) + std::chrono::milliseconds(900));
    require(decision.boundary, "the hitch quantum lost its boundary");
    require(clock.stats().hitches_forgiven == 1u, "a 900 ms hitch must be forgiven once");
    require(std::abs(ms(decision.debt) - 80.0) < 1.0,
            "a forgiven hitch must keep exactly the 80 ms the audio prime needs back");
    require(std::abs(ms(clock.stats().hitch_time_forgiven) - 820.0) < 1.0,
            "a 900 ms hitch must forgive 820 ms");
    require(ms(clock.stats().maximum_observed_debt) > 899.0,
            "the report must still show the real hitch, not the forgiven remainder");
}

void test_sustained_shortfall_is_never_forgiven() {
    gekko::FixedSimulationClock clock(hitch_config(std::chrono::milliseconds(100)));
    const Clock::time_point epoch{};
    const gekko::u64 quantum = gekko::kGameTicksPerSecond / 60u;
    clock.reset(0u, epoch);
    // 20 ms per quantum against a 16.67 ms period: the measured 0.83x in-stage
    // regime the reverted 33 ms debt bound turned into permanent slow motion.
    auto now = epoch;
    gekko::FixedSimulationClock::Decision last;
    for (gekko::u64 tick = 1u; tick <= 600u; ++tick) {
        now += std::chrono::milliseconds(20);
        last = clock.observe(quantum * tick, now);
    }
    require(clock.stats().hitches_forgiven == 0u,
            "a sustained shortfall must never be treated as a hitch");
    require(ms(last.debt) > 1900.0,
            "a sustained shortfall must keep its whole debt so it is repaid, not discarded");
}

void test_zero_threshold_repays_every_hitch_as_before() {
    gekko::FixedSimulationClock clock(hitch_config(std::chrono::nanoseconds::zero()));
    const Clock::time_point epoch{};
    const gekko::u64 quantum = gekko::kGameTicksPerSecond / 60u;
    clock.reset(0u, epoch);
    for (gekko::u64 tick = 1u; tick <= 60u; ++tick) {
        clock.observe(quantum * tick, on_schedule(epoch, tick));
    }
    const auto decision =
        clock.observe(quantum * 61u, on_schedule(epoch, 61u) + std::chrono::milliseconds(900));
    require(clock.stats().hitches_forgiven == 0u, "threshold 0 must disable forgiveness");
    require(ms(decision.debt) > 899.0, "threshold 0 must keep the whole hitch as debt");
}

void test_render_clock_supports_requested_rates() {
    const std::array<double, 7> rates{30.0, 60.0, 120.0, 144.0, 165.0, 240.0, 800.0};
    for (const double rate : rates) {
        const auto frames = count_render_frames(rate, std::chrono::seconds(10));
        const auto expected = static_cast<gekko::u64>(rate * 10.0);
        require(frames >= expected - 1u && frames <= expected + 1u,
                "render clock drifted outside one frame over ten seconds");
    }

    gekko::RenderClock unlocked(0.0);
    const Clock::time_point epoch{};
    unlocked.reset(epoch);
    for (int index = 0; index < 1000; ++index) {
        require(unlocked.due(epoch) && unlocked.consume(epoch),
                "unlocked rendering must never wait on a synthetic deadline");
    }
    require(unlocked.stats().frames == 1000u, "unlocked render accounting is incorrect");
}

void test_render_rate_cannot_change_simulation_state() {
    const std::array<double, 8> rates{30.0, 60.0, 120.0, 144.0, 165.0, 240.0, 800.0, 0.0};
    gekko::u64 reference_hash = 0u;
    for (const double rate : rates) {
        gekko::FixedSimulationClock simulation;
        gekko::RenderClock render(rate);
        const Clock::time_point epoch{};
        simulation.reset(0u, epoch);
        render.reset(epoch);

        std::int64_t player_position = 0;
        gekko::u64 animation_tick = 0;
        gekko::u64 mission_tick = 0;
        bool script_fired = false;
        gekko::u64 hash = 1469598103934665603ull;
        for (gekko::u64 tick = 1; tick <= 600u; ++tick) {
            const auto now = epoch + std::chrono::nanoseconds(tick * 1000000000ull / 60u);
            const auto decision = simulation.observe(
                tick * (gekko::kGameTicksPerSecond / 60u), now);
            require(decision.boundary, "fixed simulation lost a gameplay tick");

            // Integer stand-ins for the invariants the opaque guest owns. Their
            // only input is the fixed simulation tick; render decisions are
            // deliberately made after the state has been committed.
            player_position += 7;
            ++animation_tick;
            ++mission_tick;
            if (mission_tick == 243u) script_fired = true;
            hash ^= static_cast<gekko::u64>(player_position) + animation_tick * 17u +
                    mission_tick * 31u + (script_fired ? 1u : 0u);
            hash *= 1099511628211ull;

            if (render.unlocked()) {
                render.consume(now);
            } else {
                while (render.due(now)) {
                    render.consume(now);
                    if (!render.due(now)) break;
                }
            }
        }
        require(player_position == 4200 && animation_tick == 600u && mission_tick == 600u &&
                    script_fired,
                "gameplay-duration proxy changed under a presentation rate");
        if (reference_hash == 0u) reference_hash = hash;
        require(hash == reference_hash,
                "render cadence fed back into movement, animation, mission or script time");
    }
}

void test_presentation_interpolation_is_causal() {
    const std::vector<gekko::u32> older{0xff000000u, 0xffffffffu};
    const std::vector<gekko::u32> newer{0xffffffffu, 0xff000000u};
    std::vector<gekko::u32> output;
    gekko::interpolate_xrgb8888(older, newer, 0.0, output);
    require(output == older, "interpolation alpha zero must reproduce the older completed state");
    gekko::interpolate_xrgb8888(older, newer, 1.0, output);
    require(output == newer, "interpolation alpha one must reproduce the newer completed state");
    gekko::interpolate_xrgb8888(older, newer, 0.5, output);
    require(output[0] == 0xff808080u && output[1] == 0xff808080u,
            "half-frame interpolation must blend presentation pixels deterministically");
}

void test_audio_clock_is_independent() {
    gekko::AudioClock audio(32000u);
    audio.advance(320u);
    require(audio.produced_samples() == 320u &&
                std::abs(audio.produced_seconds() - 0.01) < 1.0e-12,
            "audio source clock must count samples in its own domain");
    audio.reset();
    require(audio.produced_samples() == 0u, "audio discontinuity reset failed");
}

}  // namespace

// A re-anchor (savestate load, pause, host startup) at any game time keeps the pacing boundaries on the absolute
// grid, so every frame boundary stays one (docs/FRAME_PACING.md "Pacing on the frame grid"): at 300 Hz the 1/60 s
// frame 675,000 is a boundary, slept to the wall time its game time is from the anchor.
void test_pacing_boundaries_stay_on_the_absolute_grid() {
    gekko::FixedSimulationClock::Config config;
    config.pacing_hz = 300u;
    config.sleep_slack = std::chrono::nanoseconds::zero();
    gekko::FixedSimulationClock clock(config);
    const Clock::time_point epoch{};
    constexpr gekko::u64 kQuantum = gekko::kGameTicksPerSecond / 300u;  // 135,000
    const gekko::u64 anchor = 600000u;                                  // 4.44 quanta: between two boundaries
    clock.reset(anchor, epoch);
    require(!clock.observe(anchor + 1u, epoch).boundary, "no boundary before the next grid point");
    const auto grid = clock.observe(5u * kQuantum, epoch);
    require(grid.boundary && grid.should_sleep, "the next grid point (675,000) is a boundary");
    const auto expected = epoch + std::chrono::nanoseconds((5u * kQuantum - anchor) * 1000000000ull /
                                                           gekko::kGameTicksPerSecond);
    // The clock floors twice (the grid point's 3,333,333 ns minus the anchor's offset into its quantum, 1,481,481 ns),
    // the expectation floors once (1,851,851.85 ns): the same instant to within 1 ns of integer rounding, so compare
    // with a 2 ns tolerance. A boundary off the grid would be off by a whole offset (microseconds or more).
    const auto error = grid.deadline > expected ? grid.deadline - expected : expected - grid.deadline;
    require(error <= std::chrono::nanoseconds(2), "its deadline is its game time's distance from the anchor");
}

void test_wake_guard_follows_the_timer_lateness() {
    using std::chrono::nanoseconds;
    gekko::WakeGuard guard;
    require(guard.guard() == gekko::WakeGuard::kInitial, "nothing measured: the initial guard");
    guard.note_lateness(nanoseconds(-50'000));  // an early wake counts as on time
    require(guard.guard() == gekko::WakeGuard::kMinimum, "a punctual timer keeps the minimum guard");
    guard.note_lateness(nanoseconds(700'000));
    require(guard.guard() == gekko::WakeGuard::kMinimum, "one late wake is an outlier: the minimum guard stays");
    guard.note_lateness(nanoseconds(600'000));
    require(guard.guard() == nanoseconds(700'000), "two late wakes: the second largest (0.6 ms) plus the margin");
    guard.note_lateness(nanoseconds(5'000'000));
    require(guard.guard() == nanoseconds(800'000), "a single stall does not pin a long spin");
    guard.note_lateness(nanoseconds(4'000'000));
    require(guard.guard() == gekko::WakeGuard::kMaximum, "two stalls: the guard rises, but never past the maximum");
    // The stall and the 0.7 ms wake age out after kWindow punctual wakes.
    for (std::size_t i = 0; i < gekko::WakeGuard::kWindow; ++i) guard.note_lateness(nanoseconds(200'000));
    require(guard.guard() == nanoseconds(300'000), "old wakes leave the window");
}

int main() {
    try {
        test_wake_guard_follows_the_timer_lateness();
        test_pacing_boundaries_stay_on_the_absolute_grid();
        test_fixed_simulation_clock_batches_then_sleeps();
        test_fixed_simulation_clock_bounds_catchup_and_debt();
        test_rate_ceiling_is_inert_when_the_host_cannot_keep_up();
        test_catchup_credit_is_a_one_off_burst_not_a_sustained_rate();
        test_recovery_is_rate_limited_and_discards_no_guest_time();
        test_wake_overshoot_does_not_compound_into_slow_motion();
        test_single_host_hitch_is_forgiven_down_to_the_kept_debt();
        test_sustained_shortfall_is_never_forgiven();
        test_zero_threshold_repays_every_hitch_as_before();
        test_render_clock_supports_requested_rates();
        test_render_rate_cannot_change_simulation_state();
        test_presentation_interpolation_is_causal();
        test_audio_clock_is_independent();
        std::cout << "host timing tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "host timing test failed: " << error.what() << '\n';
        return 1;
    }
}
