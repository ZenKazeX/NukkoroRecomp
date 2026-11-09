#pragma once

// The XInput device layer: polls Xbox-protocol gamepads and drives their rumble motors.
//
// What a gamepad means to the game - which buttons raise which of its controls, how the sticks map to stick
// units - belongs to the lifted peripheral layer (peripheral/gamepad_binding.h,
// docs/lift-20261006/L1_INPUT.md); this file only talks to the device.

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

}  // namespace gekko
