#pragma once

// The controls Shadow's peripheral layer reads, and one controller's sample.
//
// The engine identifies its controls by bit, and the game's prompts and tuning refer to them by the names
// below. A sample is what a device - or a script - reports for one player port at one moment: whether a
// controller is there, which controls are held, and the two analog sticks in stick units (full deflection
// is 100; right and up are positive). docs/lift-20261006/L1_INPUT.md.

#include <cstdint>

namespace shadow::peripheral {

enum Control : std::uint32_t {
    kControlA = 0x0001u,
    kControlB = 0x0002u,
    kControlX = 0x0008u,
    kControlY = 0x0010u,
    kControlZ = 0x0020u,
    kControlUp = 0x0040u,
    kControlDown = 0x0080u,
    kControlLeft = 0x0100u,
    kControlRight = 0x0200u,
    kControlStart = 0x0400u,
    kControlL = 0x1000u,
    kControlR = 0x2000u,
};

// Every control the game reads. Bits outside this set are never reported.
inline constexpr std::uint32_t kAllControls = kControlA | kControlB | kControlX | kControlY | kControlZ |
                                              kControlUp | kControlDown | kControlLeft | kControlRight |
                                              kControlStart | kControlL | kControlR;

// A stick's full deflection, in stick units.
inline constexpr int kStickFull = 100;

struct ControllerSample {
    bool connected = false;
    std::uint32_t controls = 0;  // Control bits held
    std::int8_t stick_x = 0;     // movement stick
    std::int8_t stick_y = 0;
    std::int8_t substick_x = 0;  // camera stick
    std::int8_t substick_y = 0;
};

inline bool operator==(const ControllerSample& a, const ControllerSample& b) noexcept {
    return a.connected == b.connected && a.controls == b.controls && a.stick_x == b.stick_x &&
           a.stick_y == b.stick_y && a.substick_x == b.substick_x && a.substick_y == b.substick_y;
}
inline bool operator!=(const ControllerSample& a, const ControllerSample& b) noexcept { return !(a == b); }

// The control a name stands for in scripts and bindings ("A", "B", "X", "Y", "Z", "L", "R", "START", "UP",
// "DOWN", "LEFT", "RIGHT"; upper case). Returns false for any other name.
bool control_from_name(const char* name, Control& out) noexcept;

// The name of one control (nullptr for a value that is not exactly one control).
const char* control_name(std::uint32_t control) noexcept;

}  // namespace shadow::peripheral
