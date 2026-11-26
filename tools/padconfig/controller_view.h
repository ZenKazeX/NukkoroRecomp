#pragma once

// The controller pictures in the Settings program: the game's controller rendered from above and tilted, each of its buttons
// labelled with the button on the player's own controller that presses it. A button on the picture can be clicked.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "pad_config.h"

namespace padconfig {

struct ControllerView {
    GamepadBinding binding;
    std::uint16_t held = 0;      // the player's buttons down now
    bool left_trigger = false;   // past the mapping's trigger threshold
    bool right_trigger = false;
    int selected = -1;           // the kControls row being changed
};

bool operator==(const ControllerView& a, const ControllerView& b) noexcept;

// What presses control `index`: "A", "LB or LT", "RT". The analog triggers always work as L and R.
std::wstring control_label(const GamepadBinding& binding, std::size_t index);

// Paints both views inside `area` of `dc` (as large as fits, centred across it), with light labels when `dark`.
// `min_label`: the labels' smallest height in pixels (0: they scale with the pictures). When the pictures are drawn
// smaller than that allows, the label columns widen and the pictures shrink a little more, so the labels stay
// readable in a small area. Needs GDI+ started.
void paint_controller(HDC dc, const RECT& area, const ControllerView& view, bool dark = false, float min_label = 0.0f);

// The kControls row of the button at `point` on the pictures painted with the same `area` and `min_label`; -1 when
// none.
int control_at(const RECT& area, POINT point, float min_label = 0.0f);

}  // namespace padconfig
