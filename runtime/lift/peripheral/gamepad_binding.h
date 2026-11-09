#pragma once

// A PC gamepad as one of Shadow's controllers: which gamepad buttons raise which of the game's controls,
// and how the analog sticks map to stick units.
//
// The binding is the player's (pad.cfg, written by tools/launcher.py: key = gamepad button mask). The stick
// mapping is not a preference: the game's movement and camera code compares stick magnitudes against fixed
// thresholds, so the dead zone (8000 of 32767) and the scale (full deflection = 100 stick units) are part of
// how the game plays and stay as they are. docs/lift-20261006/L1_INPUT.md.

#include <cstdint>
#include <string>

#include "peripheral/controls.h"

namespace shadow::peripheral {

// The standard gamepad layout's buttons (the values of the Windows XInput masks, which pad.cfg stores).
enum GamepadButton : std::uint16_t {
    kGamepadDpadUp = 0x0001u,
    kGamepadDpadDown = 0x0002u,
    kGamepadDpadLeft = 0x0004u,
    kGamepadDpadRight = 0x0008u,
    kGamepadStart = 0x0010u,
    kGamepadBack = 0x0020u,
    kGamepadLeftStick = 0x0040u,
    kGamepadRightStick = 0x0080u,
    kGamepadLeftShoulder = 0x0100u,
    kGamepadRightShoulder = 0x0200u,
    kGamepadSouth = 0x1000u,  // A on an Xbox pad
    kGamepadEast = 0x2000u,   // B
    kGamepadWest = 0x4000u,   // X
    kGamepadNorth = 0x8000u,  // Y
};

// The buttons the game's prompts draw: the disc's own art (original), or a modern controller's. Auto follows the
// controller that is connected.
enum class ButtonStyle : std::uint8_t { Auto, Original, Xbox, PlayStation, Switch };
// "auto", "original", "xbox", "playstation", "switch"; false for anything else.
bool parse_button_style(const std::string& text, ButtonStyle& style);
const char* button_style_name(ButtonStyle style) noexcept;

// One gamepad reading: buttons held, triggers 0..255, sticks -32768..32767 with up positive.
struct GamepadState {
    std::uint16_t buttons = 0;
    std::uint8_t left_trigger = 0;
    std::uint8_t right_trigger = 0;
    std::int16_t left_x = 0;
    std::int16_t left_y = 0;
    std::int16_t right_x = 0;
    std::int16_t right_y = 0;
};

struct GamepadBinding {
    // The gamepad buttons (a mask; 0 = unbound) that raise each control.
    std::uint16_t a = kGamepadSouth;
    std::uint16_t b = kGamepadEast;
    std::uint16_t x = kGamepadWest;
    std::uint16_t y = kGamepadNorth;
    std::uint16_t start = kGamepadStart;
    std::uint16_t l = kGamepadLeftShoulder;
    std::uint16_t z = kGamepadRightShoulder;
    std::uint16_t r = 0;  // unbound: the right analog trigger raises R
    std::uint16_t up = kGamepadDpadUp;
    std::uint16_t down = kGamepadDpadDown;
    std::uint16_t left = kGamepadDpadLeft;
    std::uint16_t right = kGamepadDpadRight;
    // Analog trigger travel (0..255) beyond which a trigger also holds L / R.
    std::uint8_t trigger_threshold = 40;
    // Movement on the right stick, camera on the left.
    bool swap_sticks = false;
    // Which controller's buttons the game's prompts show (gekko/button_glyphs.h).
    ButtonStyle button_style = ButtonStyle::Auto;
};

// Parses pad.cfg: `key = value` lines, '#' comments. Keys: a b x y start l r z dpad_up dpad_down dpad_left
// dpad_right (gamepad button masks, 0 to 0xFFFF), trigger_threshold (0 to 255), swap_sticks (0 / 1),
// button_style (a parse_button_style name). A value
// that is not wholly a number, out of range, or an unknown key is an error (`error` says which line), so a
// typo never silently keeps a default.
bool parse_gamepad_binding(const std::string& text, GamepadBinding& binding, std::string* error);

// One gamepad reading as a connected controller sample.
ControllerSample sample_from_gamepad(const GamepadState& state, const GamepadBinding& binding) noexcept;

// A stick axis in stick units: 0 inside the dead zone, then value * 100 / 32767, clamped to +-100.
std::int8_t stick_units(std::int16_t axis) noexcept;

}  // namespace shadow::peripheral
