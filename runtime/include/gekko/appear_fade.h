#pragma once

// The fade-in: a model that starts being drawn while it is already inside the
// camera's view dissolves in over a short time instead of appearing in one
// frame - a layout object spawning at its range (docs/DRAW_DISTANCE.md), an
// enemy switching its model on, any pop.
//
// The rule needs no knowledge of who owns a model: an atomic drawn this frame
// that was not drawn for kAbsentFrames frames (or never), whose position was
// inside the PREVIOUS frame's view and at least kMinDistance from its eye,
// appeared for a reason other than the camera. One that comes into view at the
// screen's edge was outside the previous view and is drawn as the game drew
// it; so is one that the game switches on and off more often than every
// kAbsentFrames (a damage flash, a blinking marker), and anything near the eye
// - the player's own models and effects, which the game switches deliberately.
// Measured on the Westopolis replay (docs/DRAW_DISTANCE.md "Fade-in"): of 961
// first-draw-in-view events at a 1 s absence, 311 were within 100 units of the
// eye (Shadow's models and effects); the smallest authored layout range is 200.
//
// The renderer takes the result as a dissolve (the share of pixels still
// hidden, an ordered 4x4 dither in the mesh shader): no sorting, opaque
// surfaces stay opaque and depth-correct.

#include <iterator>
#include <unordered_map>

#include "gekko/types.h"

namespace gekko {

class AppearFade {
public:
    static constexpr u64 kDefaultTicks = 40500000ull * 3ull / 10ull;  // 0.3 s of game time
    static constexpr u64 kAbsentFrames = 240;                         // 4 s at 60 Hz
    static constexpr float kMinDistance = 200.0f;                     // the smallest layout range (2 x 100)

    // The fade's length in game ticks (40.5 MHz); 0 turns the fade off.
    void set_duration(u64 ticks) noexcept { duration_ = ticks; }
    u64 duration() const noexcept { return duration_; }

    // A camera's view: its frame (right, up, forward, position in world space)
    // and the half extents of its view at distance 1.
    struct View {
        float right[3]{1, 0, 0}, up[3]{0, 1, 0}, forward[3]{0, 0, 1}, position[3]{};
        float window_x = 0.5f, window_y = 0.375f, near_plane = 1.0f, far_plane = 10000.0f;
    };

    // A frame was finished; `view` is the view it showed the world through
    // (null: none, e.g. a 2D-only frame).
    void end_frame(const View* view) {
        previous_valid_ = view != nullptr;
        if (view != nullptr) previous_ = *view;
        ++frame_;
        // Forget atomics not drawn for a long time (their pointers may be reused).
        if ((frame_ & 1023u) == 0u) {
            for (auto it = atomics_.begin(); it != atomics_.end();) {
                it = frame_ - it->second.last_frame > kAbsentFrames ? atomics_.erase(it) : std::next(it);
            }
        }
    }

    static bool inside(const View& v, const float p[3]) noexcept {
        const float d[3] = {p[0] - v.position[0], p[1] - v.position[1], p[2] - v.position[2]};
        const float z = d[0] * v.forward[0] + d[1] * v.forward[1] + d[2] * v.forward[2];
        if (!(z > v.near_plane && z < v.far_plane)) return false;
        const float x = d[0] * v.right[0] + d[1] * v.right[1] + d[2] * v.right[2];
        const float y = d[0] * v.up[0] + d[1] * v.up[1] + d[2] * v.up[2];
        return x <= z * v.window_x && -x <= z * v.window_x && y <= z * v.window_y && -y <= z * v.window_y;
    }

    static bool far_enough(const View& v, const float p[3]) noexcept {
        const float d[3] = {p[0] - v.position[0], p[1] - v.position[1], p[2] - v.position[2]};
        return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] >= kMinDistance * kMinDistance;
    }

    // `atomic` is drawn at `position` (world) at game tick `now`: the share of
    // its pixels still hidden (0 = drawn as the game drew it).
    float draw(u32 atomic, const float position[3], u64 now) {
        if (duration_ == 0u) return 0.0f;
        const auto [it, inserted] = atomics_.try_emplace(atomic);
        State& s = it->second;
        if (inserted || s.last_frame != frame_) {  // its first draw this frame
            const bool absent = inserted || frame_ - s.last_frame > kAbsentFrames;
            if (absent && previous_valid_ && inside(previous_, position) && far_enough(previous_, position)) {
                s.fade_start = now != 0u ? now : 1u;
                ++stats_.fades;
            }
            s.last_frame = frame_;
        }
        if (s.fade_start == 0u) return 0.0f;
        const u64 elapsed = now > s.fade_start ? now - s.fade_start : 0u;
        if (elapsed >= duration_) {
            s.fade_start = 0u;
            return 0.0f;
        }
        return 1.0f - static_cast<float>(static_cast<double>(elapsed) / static_cast<double>(duration_));
    }

    // A savestate load: nothing carries over.
    void clear() {
        atomics_.clear();
        previous_valid_ = false;
    }

    struct Stats {
        u64 fades = 0;  // atomics that appeared inside the view and faded in
    };
    const Stats& stats() const noexcept { return stats_; }
    const View& previous_view() const noexcept { return previous_; }

private:
    struct State {
        u64 last_frame = 0;  // frame_ when last drawn
        u64 fade_start = 0;  // game tick its fade began (0: not fading)
    };
    u64 duration_ = kDefaultTicks;
    u64 frame_ = 1;
    View previous_{};
    bool previous_valid_ = false;
    std::unordered_map<u32, State> atomics_;
    Stats stats_{};
};

}  // namespace gekko
