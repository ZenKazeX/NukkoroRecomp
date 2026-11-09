#pragma once

// The XInput device layer: polls Xbox-protocol gamepads and drives their rumble motors.
//
// What a gamepad means to the game - which buttons raise which of its controls, how the sticks map to stick
// units - belongs to the lifted peripheral layer (peripheral/gamepad_binding.h,
// docs/lift-20261006/L1_INPUT.md); this file only talks to the device.

#include <cstdint>

#include "peripheral/controls.h"
#include "peripheral/gamepad_binding.h"

namespace gekko {

// The binding every later poll_xinput_controller uses (pad.cfg; the default binding until one is set).
void set_gamepad_binding(const shadow::peripheral::GamepadBinding& binding);
const shadow::peripheral::GamepadBinding& gamepad_binding() noexcept;

// One XInput user's gamepad; false when it is absent or XInput is unavailable.
bool poll_xinput_gamepad(unsigned user_index, shadow::peripheral::GamepadState& state);

// That gamepad as a controller sample through the current binding; a disconnected sample when absent.
shadow::peripheral::ControllerSample poll_xinput_controller(unsigned user_index);

// A port's rumble request on the physical controller. A no-op without XInput or a controller.
void set_xinput_rumble(unsigned user_index, bool rumble);
// The player's preferences (Options, gekko/pc_settings.h), any thread: the rumble motors' strength (percent of the
// original duty; 100 = original) and the camera stick's inversion per axis (false = original).
void set_xinput_preferences(unsigned rumble_percent, bool invert_camera_x, bool invert_camera_y) noexcept;

// The pure parts of the two preferences (unit-tested: runtime/tests/options_input_tests.cpp).
// One stick axis inverted: -v, saturating, so an int8 -128 becomes +127 (it must not wrap back to -128).
std::int8_t invert_stick_axis(std::int8_t value) noexcept;
// The camera stick (substick) of a sample with the requested axes inverted; everything else is untouched.
shadow::peripheral::ControllerSample apply_camera_inversion(shadow::peripheral::ControllerSample sample, bool invert_x,
                                                            bool invert_y) noexcept;
// The motor speed (both motors) for a rumble request at a strength: 0 when off, else 0xC000 * percent / 100 (percent
// clamped to 100; 100 = the original duty).
unsigned short xinput_rumble_speed(bool rumble, unsigned percent) noexcept;

}  // namespace gekko
