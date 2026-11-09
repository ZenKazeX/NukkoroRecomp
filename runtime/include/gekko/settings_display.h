#pragma once

// The Display and Graphics categories of the settings registry (gekko/settings_registry.h), over the display settings
// service (gekko/display_settings.h) and the one display rule of gekko/display_geometry.h. Four settings in all, each
// with one job (docs/DISPLAY_SETTINGS_AUDIT.md section 6):
//
// Display ("display.*"):
//   mode          windowed | borderless ("Fullscreen")      needs_confirm (15 s trial)
//   aspect        auto | 4:3 | 5:4 | 16:10 | 16:9 | 21:9 | 32:10 | 32:9 (a custom ratio in effect is listed). Picked
//                 while windowed, a fixed ratio gives the window its shape once; afterwards the window is free and the
//                 picture is letterboxed inside it.
//   vsync         off | on
// Graphics ("graphics.*"):
//   render_scale  the resolution scale, "25" .. "200" (percent per side of the picture)
//
// The window size is the window's own (dragged, remembered). MSAA, filtering, the FPS counter and exclusive
// fullscreen are display.cfg / command-line keys for tests, not menu rows. Each provider applies only its own fields
// onto the settings in effect, so a Display change never undoes a Graphics one made meanwhile, and the reverse.

#include <memory>

#include "gekko/display_settings.h"
#include "gekko/settings_registry.h"

namespace gekko::settings {

std::unique_ptr<Provider> make_display_provider();
std::unique_ptr<Provider> make_graphics_provider();

// The values "reset to defaults" puts back (the launcher's defaults without a display.cfg).
DisplaySettings default_display_settings();

}  // namespace gekko::settings
