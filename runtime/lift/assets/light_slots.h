#pragma once

// The blinking corridor lights laid on the painted slots of their walls - a DELIBERATE DEVIATION from the retail game
// (docs/RENDER_FLICKER.md "Corridor lights"). GUN Fortress's corridor walls carry a black slot painted on every
// texture tile, and each slot's blinking light is a separate quad in the stage's glare worlds, laid over it by hand:
// on the disc 418 of them sit off their slot along the wall (up to 12 units), 19 lie on or behind the wall they light
// (up to 0.094 units - the wall's own depth hides them, wholly or in part, as in the retail game) and 8 slots have
// none. The table (light_slots_table.inc, generated from the disc by tools/assets/light_slots.py) moves every vertex
// of a misplaced light onto its slot, sets a light at or behind its wall at the offset its sound neighbours on the
// same wall have (most lie 0.25..0.35 units clear), and adds a copy of the nearest sound light to every empty slot.
// Positions are the disc's floats, so a glare sector's mesh is matched exactly; nothing else in the game reads these
// quads (they are drawn, never collided with).

#include <cstdint>
#include <vector>

#include "assets/geometry_import.h"

namespace shadow::assets {

struct LightSlotMove {
    float from[3];
    float to[3];
};

struct LightSlotAdd {
    std::uint32_t vertex_count;
    float from[8][3];  // the template light's vertices on the disc (its attributes are copied)
    float to[8][3];    // the new light's
    std::uint32_t triangle_count;
    std::uint32_t triangles[6][3];  // into from / to
};

struct LightSlotResult {
    std::uint32_t moved_vertices = 0;
    std::uint32_t added_lights = 0;
};

// Applies the table to `mesh` (a glare world sector's imported mesh), only to the submeshes `glow[s]` marks (the
// lights' material). Lights are added before any vertex moves, from the templates' disc positions.
LightSlotResult fix_light_slots(MeshData& mesh, const std::vector<bool>& glow);

// The same with a table of the caller's (tests).
LightSlotResult fix_light_slots(MeshData& mesh, const std::vector<bool>& glow, const LightSlotMove* moves,
                                std::size_t move_count, const LightSlotAdd* adds, std::size_t add_count);

}  // namespace shadow::assets
