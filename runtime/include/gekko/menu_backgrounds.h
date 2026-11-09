#pragma once

// Full-height menu backgrounds (user request 2026-10-30; a deliberate deviation from the retail game).
//
// Original behaviour: five front-end screens author their background cast `bg` (group 0, cast 0) at scale 1.0 x 0.94,
// from y 0. Its cell is the top 480 rows of a 512-row texture (v 0 .. 0.9375), so the art is squeezed into 451.2 of
// the canvas's 480 lines and a 28.8-line band at the bottom shows the clear colour. The retail game shows that band.
// No motion of these scenes animates the cast's position or scale. Select Mode authors the same kind of cell at
// scale 1.0 x 1.0 and covers the whole canvas.
//
// Native change: these five casts are given Select Mode's scale (1.0), so each background is drawn 1:1 from its 480
// rows of art over the full 480 lines. Only the cast's authored scaleY word (property slot 5) changes.

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "gekko/types.h"

namespace gekko::menu_backgrounds {

struct Screen {
    std::string_view path;   // the project, as the game opens it
    std::string_view scene;  // the scene holding the cast ("group/scene" in a grouped project)
};
// The main menu, Story Mode, the story route, Battle Mode and Options.
constexpr std::array<Screen, 5> kScreens = {{
    {"csdFiles/honban.gncp", "Loop"},
    {"csdFiles/Story_Library.gncp", "Loop/Loop"},
    {"csdFiles/RouteDisplay.gncp", "StoryRoute/IconsBG"},
    {"csdFiles/BattleMode.gncp", "Kazari"},
    {"csdFiles/Options.gncp", "Loop"},
}};

// `gncp` with the background cast of `scene` at full height. Refused (with the reason) unless the cast is exactly the
// one described above: named "bg", group 0 cast 0, at x 0 / y 0, scale 1.0 x 0.94.
bool fill(const std::vector<u8>& gncp, std::string_view scene, std::vector<u8>& out, std::string& error);

// The background cast's authored scaleY, for the tests (false when the scene or cast is missing).
bool background_scale_y(const std::vector<u8>& gncp, std::string_view scene, float& scale_y);

}  // namespace gekko::menu_backgrounds
