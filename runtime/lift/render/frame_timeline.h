#pragma once

// Independent frame (docs/re/independent-frame/): where between two game frames a picture started now belongs.
//
// The game finishes its frames on an exact grid of game time (675,000 ticks of the 40.5 MHz game clock at 60 Hz) and
// hands each to the renderer when it is done; the hand-over's wall time jitters by the game thread's wake (tenths of
// a millisecond). The timeline maps game time to wall time with one filtered offset, so the spacing of the frames it
// places pictures between is the game's own, exact, and the jitter does not reach the pictures.
//
// A picture shows the game as it was one game frame before the picture is started: the newest frame, B, is reached
// exactly when the frame after it is due. That one frame is the cost of showing only what the game has decided
// (nothing is predicted). alpha is the position from A (0) to B (1).
//
// No device, no thread, no clock of its own: the caller supplies every time, so tests drive it with synthetic ones.

#include <cstdint>

namespace shadow::render {

class FrameTimeline {
public:
    static constexpr double kTicksPerSecond = 40500000.0;  // gekko/game_clock.h kGameTicksPerSecond
    static constexpr double kNsPerTick = 1e9 / kTicksPerSecond;
    // A hand-over this far off the filtered offset is not jitter: the mapping starts again there (a pause, a load, a
    // stall, the pacer's re-anchor) and the pair it ends is shown as B.
    static constexpr double kResyncNs = 4.0e6;
    // The most game frames between A and B that are still one motion (the renderer skipped the ones between).
    static constexpr std::uint64_t kMaxFramesApart = 3;

    // The newest frame the game handed over: its frame number, the game clock when it was finished, and the wall time
    // (ns) of the hand-over.
    void arrive(std::uint64_t frame, std::uint64_t game_ticks, std::int64_t pushed_ns) noexcept {
        const double offset = static_cast<double>(pushed_ns) - static_cast<double>(game_ticks) * kNsPerTick;
        bool resynced = false;
        if (!have_cur_ || offset - offset_ > kResyncNs || offset - offset_ < -kResyncNs) {
            offset_ = offset;
            resynced = have_cur_;
            if (resynced)
                ++resyncs_;
        } else {
            offset_ += (offset - offset_) / 16.0;
        }
        prev_ = cur_;
        have_prev_ = have_cur_;
        cur_ = Frame{frame, game_ticks};
        have_cur_ = true;
        continuous_ = false;
        period_ns_ = 0.0;
        if (!have_prev_ || resynced || cur_.frame <= prev_.frame || cur_.ticks <= prev_.ticks)
            return;
        const std::uint64_t frames = cur_.frame - prev_.frame;
        if (frames > kMaxFramesApart)
            return;
        const double period =
            static_cast<double>(cur_.ticks - prev_.ticks) * kNsPerTick / static_cast<double>(frames);
        // One game frame: 1/60 s, or 1/50 s on the 50 Hz screens; anything far from those is not a frame step.
        if (period < 1e9 / 130.0 || period > 1e9 / 20.0)
            return;
        period_ns_ = period;
        continuous_ = true;
    }

    void reset() noexcept { *this = FrameTimeline{}; }

    // B follows A as one motion (else every picture of the pair is B).
    bool continuous() const noexcept { return continuous_; }
    // One game frame of the current pair, in ns of wall time (0: not continuous).
    double period_ns() const noexcept { return period_ns_; }
    std::uint64_t resyncs() const noexcept { return resyncs_; }

    // The wall time game time `ticks` maps to.
    double wall_ns(std::uint64_t ticks) const noexcept { return static_cast<double>(ticks) * kNsPerTick + offset_; }

    // The position between A and B of a picture started at wall time now_ns: 0 at or before A, 1 at or after B, and
    // 1 whenever the pair is not continuous.
    float alpha(std::int64_t now_ns) const noexcept {
        if (!continuous_)
            return 1.0f;
        const double a = wall_ns(prev_.ticks), b = wall_ns(cur_.ticks);
        const double shown = static_cast<double>(now_ns) - period_ns_;
        if (shown >= b)
            return 1.0f;
        if (shown <= a)
            return 0.0f;
        return static_cast<float>((shown - a) / (b - a));
    }

    // How old the newest frame is at now_ns (ns since its mapped hand-over).
    double age_ns(std::int64_t now_ns) const noexcept {
        return have_cur_ ? static_cast<double>(now_ns) - wall_ns(cur_.ticks) : 0.0;
    }

private:
    struct Frame {
        std::uint64_t frame = 0, ticks = 0;
    };
    Frame prev_, cur_;
    bool have_prev_ = false, have_cur_ = false, continuous_ = false;
    double offset_ = 0.0, period_ns_ = 0.0;
    std::uint64_t resyncs_ = 0;
};

}  // namespace shadow::render
