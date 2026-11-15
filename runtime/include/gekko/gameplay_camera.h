#pragma once

// Whether the game is showing a playable scene, read from the game's own camera manager: the mouse belongs to the camera
// only then. It decides nothing about gameplay; it only says when the cursor is taken and when mouse motion counts.

#include "gekko/types.h"
#include "peripheral/controls.h"

namespace gekko {

class Cpu;

// True while the game is not paused and camera unit 0 has a camera request (docs/camera-re-20261102/02_CAMERA_FRAMEWORK.md
// 2.6, 2.7). Menus and the title screen have none.
bool gameplay_camera_active(Cpu& cpu) noexcept;
// The game's pause flag.
bool gameplay_paused(Cpu& cpu) noexcept;

// Diagnostic: GEKKO_CAMERA_TRACE=<csv> appends, at each host service, the tick, the sample the game will read, the
// camera output (eye, look) and the camera target (position, velocity) of camera unit 0. Off (and free) otherwise.
void camera_trace_record(Cpu& cpu, u64 game_ticks, const shadow::peripheral::ControllerSample& sample);

}  // namespace gekko
