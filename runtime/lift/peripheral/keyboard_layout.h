#pragma once

// The keyboard as player 1's controller, and how the keyboard and player 1's gamepad combine.
//
// Layout (Windows virtual keys): X or Enter = A, Z = B, S = X, A = Y, Space or Shift+Enter = Start,
// Q = L, E = R, C = Z, arrow keys = d-pad and movement stick, I J K L = movement stick.
// docs/lift-20261006/L1_INPUT.md.

#include "peripheral/controls.h"

namespace shadow::peripheral {

// Answers whether a Windows virtual key is held (the caller decides focus and the source).
using KeyHeld = bool (*)(int virtual_key, void* context);

// The keyboard's sample: always connected (a PC always has one).
ControllerSample sample_from_keyboard(KeyHeld key_held, void* context) noexcept;

// Player 1 with both a keyboard and a gamepad: controls from either; the gamepad's movement stick wins
// where it is deflected, the camera stick is the gamepad's.
ControllerSample combine(const ControllerSample& keyboard, const ControllerSample& gamepad) noexcept;

}  // namespace shadow::peripheral
