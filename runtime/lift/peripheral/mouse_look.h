#pragma once

// Mouse motion as the game's camera input (docs/controls/CAMERA_CONTROL_RECONSTRUCTION.md).
//
// The game's camera has a rate input, not an angle: CameraModeGeneral's analog step rotates the eye about
// the player by 150 degrees per second times a normalised input in -1..1. The camera manager decides from that same
// input whether the player is steering at all, and refuses it where a camera volume forbids manual control, so the mouse
// goes through that input and every constraint the game applies to a stick applies to the mouse with no extra code.
//
// Each engine read consumes the counts since the previous read and turns them into the rate that yields exactly the
// requested rotation over one game frame. That makes the rotation a function of the counts alone: not of how many
// frames were rendered, of the display, or of when within a frame the counts arrived.
//
// Counts are never lost to rounding. Motion too small to pass the game's own "is the stick moving" test (squared
// magnitude >= 1e-4) waits in the accumulator until it adds up to enough. Motion beyond the camera's maximum
// rate is dropped rather than owed, so a fast flick ends when the hand stops instead of the camera drifting on.
//
// peek() and commit() are separate because a latched sample may be replaced by a newer one before the engine reads it
// (the 5 ms latch grid of a recording): only what the engine actually read is consumed.

#include <cstdint>

#include "peripheral/controls.h"

namespace shadow::peripheral {

struct MouseLookSettings {
    double sensitivity_x = 1.0;
    double sensitivity_y = 1.0;
    bool invert_x = false;
    bool invert_y = false;
};

class MouseLook {
public:
    // The camera's maximum rotation at full input, degrees per second of game time (the doc names the game's constant).
    static constexpr double kFullRateDegreesPerSecond = 150.0;
    // Degrees of yaw for one mouse count at sensitivity 1.0.
    static constexpr double kDegreesPerCount = 0.06;
    // The game frame the rate is held for.
    static constexpr double kFrameSeconds = 1.0 / 60.0;
    // Smallest input the game counts as steering: |n|^2 >= 1e-4, with margin for float rounding.
    static constexpr double kMinimumRate = 0.0101;

    struct Output {
        double yaw_rate = 0.0;     // normalised camera input, -1..1
        double height_rate = 0.0;  // normalised camera input, -1..1
        bool any() const noexcept { return yaw_rate != 0.0 || height_rate != 0.0; }
    };

    void set_settings(const MouseLookSettings& settings) noexcept { settings_ = settings; }

    // Forgets every count up to these totals: the mouse was taken from the camera (focus, pause, a menu).
    void reset(std::int64_t total_x, std::int64_t total_y) noexcept;

    // The rates for the counts up to these running totals, without consuming them.
    Output peek(std::int64_t total_x, std::int64_t total_y) noexcept;
    // The engine read what the last peek() produced: those counts are spent.
    void commit() noexcept;

private:
    MouseLookSettings settings_{};
    std::int64_t consumed_x_ = 0, consumed_y_ = 0;
    std::int64_t pending_x_ = 0, pending_y_ = 0;
    bool have_pending_x_ = false, have_pending_y_ = false;
};

// Writes the rates onto the camera stick of `sample`, in the stick units that the engine's dead zone (25) and live range
// (39) normalise back to those rates. Any non-zero rate lands outside the dead zone, so the game sees the input.
void apply_look_to_stick(ControllerSample& sample, const MouseLook::Output& look) noexcept;

}  // namespace shadow::peripheral
