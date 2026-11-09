#pragma once

// Tiled strips: decorative 2D layers built as a chain of congruent textured quads laid end to end along their own
// long axis - the stage loading screen's two rotating line chains (docs/LOADING_SCREEN_DECOMPOSITION.md 3: three
// copies of one 256-unit sprite cell, rigid children of one rotating parent). The game sized each chain to cross its
// 640 x 480 screen, whose edge clipped it; on a wider view the chain is CONTINUED to the view's edges by more copies of
// its end tiles, each translated by that end's own step - the artist's own construction, so angle, texel density,
// phase and animation carry over, nothing is stretched and no seam can appear (docs/2D_LAYOUT_ARCHITECTURE.md 3, the
// recognition rules kept exactly). On a view that shows only the canvas nothing is added.
//
// Pure arithmetic in canvas units: no device, no game state.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "render/view.h"

namespace shadow::ui {

// One candidate tile: a four-vertex canvas quad (its vertices in submission order) and the bounds of its texture
// coordinates.
struct StripTile {
    float x[4] = {0, 0, 0, 0};
    float y[4] = {0, 0, 0, 0};
    float u0 = 0, u1 = 0, v0 = 0, v1 = 0;
};

// A tile's shape: its long axis (unit vector) and its extent along it. A tile qualifies only if its texture
// coordinates span the whole texture width (an atlas cell that does not would tile its neighbours in).
struct TileShape {
    float ax = 1.0f, ay = 0.0f;
    float extent = 0.0f;     // along the axis
    float thickness = 0.0f;  // across it
};

inline bool tile_shape(const StripTile& t, TileShape* out) {
    if (!(t.u1 - t.u0 >= 0.99f))
        return false;
    // The two sides at vertex 0 are its two nearer neighbours; the farthest is the diagonal.
    float d[3];
    for (int i = 1; i < 4; ++i)
        d[i - 1] = std::hypot(t.x[i] - t.x[0], t.y[i] - t.y[0]);
    const int diagonal = static_cast<int>(std::max_element(d, d + 3) - d) + 1;
    int a = 0, b = 0;
    for (int i = 1; i < 4; ++i) {
        if (i == diagonal)
            continue;
        (a == 0 ? a : b) = i;
    }
    const float la = d[a - 1], lb = d[b - 1];
    const int along = la >= lb ? a : b;
    const float length = std::max(la, lb);
    if (!(length > 0.0f) || !std::isfinite(length))
        return false;
    out->ax = (t.x[along] - t.x[0]) / length;
    out->ay = (t.y[along] - t.y[0]) / length;
    out->extent = length;
    out->thickness = std::min(la, lb);
    return true;
}

// Whether `b` is `a` translated (every corner moved by one vector, to 0.05 units, the same texture coordinates);
// the vector in (dx, dy).
inline bool tile_translation(const StripTile& a, const StripTile& b, float* dx, float* dy) {
    constexpr float kUv = 1.0e-4f, kPosition = 0.05f;
    if (std::fabs(a.u0 - b.u0) > kUv || std::fabs(a.u1 - b.u1) > kUv || std::fabs(a.v0 - b.v0) > kUv ||
        std::fabs(a.v1 - b.v1) > kUv)
        return false;
    const float tx = b.x[0] - a.x[0], ty = b.y[0] - a.y[0];
    for (int i = 1; i < 4; ++i) {
        if (std::fabs(b.x[i] - a.x[i] - tx) > kPosition || std::fabs(b.y[i] - a.y[i] - ty) > kPosition)
            return false;
    }
    *dx = tx;
    *dy = ty;
    return true;
}

// Whether `next` continues a chain whose first tile is `first` (shape `shape`): a translation of it along the tile's
// own long axis (|cos| >= 0.95) - which is what rejects the title-screen logo, a column of wide quads stepping
// across their axis.
inline bool continues_chain(const StripTile& first, const TileShape& shape, const StripTile& next) {
    float dx = 0.0f, dy = 0.0f;
    if (!tile_translation(first, next, &dx, &dy))
        return false;
    const float length = std::hypot(dx, dy);
    if (!(length > 0.0f))
        return false;
    return std::fabs(dx * shape.ax + dy * shape.ay) / length >= 0.95f;
}

// A recognised strip's two ends: the tile at each end of the chain along the axis and the step that end repeats on
// (from the tile next to it to the end tile).
struct StripEnds {
    std::size_t low = 0, high = 0;
    float low_dx = 0, low_dy = 0;    // the low end continues by repeating this step (pointing outward)
    float high_dx = 0, high_dy = 0;  // the high end likewise
};

// A chain of at least two tiles (each a translation of chain[0] along its axis) is a strip when every gap between
// the tiles sorted along the axis is within [0.5, 1.05] of the tile's extent - the tiles abut, or overlap a little
// where a joint was hidden.
inline bool strip_ends(const std::vector<StripTile>& chain, const TileShape& shape, StripEnds* out) {
    const std::size_t n = chain.size();
    if (n < 2)
        return false;
    std::vector<std::size_t> order(n);
    std::vector<float> along(n);
    for (std::size_t i = 0; i < n; ++i) {
        order[i] = i;
        along[i] = (chain[i].x[0] - chain[0].x[0]) * shape.ax + (chain[i].y[0] - chain[0].y[0]) * shape.ay;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return along[a] < along[b]; });
    for (std::size_t k = 1; k < n; ++k) {
        const float gap = along[order[k]] - along[order[k - 1]];
        if (gap < 0.5f * shape.extent || gap > 1.05f * shape.extent)
            return false;
    }
    out->low = order[0];
    out->high = order[n - 1];
    out->low_dx = chain[order[0]].x[0] - chain[order[1]].x[0];
    out->low_dy = chain[order[0]].y[0] - chain[order[1]].y[0];
    out->high_dx = chain[order[n - 1]].x[0] - chain[order[n - 2]].x[0];
    out->high_dy = chain[order[n - 1]].y[0] - chain[order[n - 2]].y[0];
    return true;
}

// Where the line from (cx, cy) along the unit vector (ux, uy) leaves `r` grown by `margin`: the parameter in `exit`;
// false when the line misses it.
inline bool line_exit(float cx, float cy, float ux, float uy, const render::CanvasRect& r, float margin, float* exit) {
    float t0 = -1.0e30f, t1 = 1.0e30f;
    const float p[4] = {-ux, ux, -uy, uy};
    const float q[4] = {cx - (r.left - margin), (r.right + margin) - cx, cy - (r.top - margin),
                        (r.bottom + margin) - cy};
    for (int k = 0; k < 4; ++k) {
        if (p[k] == 0.0f) {
            if (q[k] < 0.0f)
                return false;  // parallel to that edge and outside it
            continue;
        }
        const float t = q[k] / p[k];
        if (p[k] < 0.0f)
            t0 = std::max(t0, t);
        else
            t1 = std::min(t1, t);
    }
    if (t0 > t1)
        return false;
    *exit = t1;
    return true;
}

// How many copies of an end tile, translated by k (dx, dy), k = 1, 2, ..., the view needs. An end is continued only
// when the authored strip already reaches the edge of its canvas there (the console's screen edge cut it; a strip
// the artist ended inside the picture stays as it is), and then just far enough to reach the edge of the part of the
// canvas the view shows (render/view.h visible_canvas) along its own line, its band of `shape.thickness` included.
// So a view that shows only the canvas never gets a copy. `cap` bounds a degenerate step.
inline unsigned continuation_count(const StripTile& tile, const TileShape& shape, float dx, float dy,
                                   const render::CanvasRect& canvas, const render::CanvasRect& visible,
                                   unsigned cap = 64u) {
    const float step = std::hypot(dx, dy);
    if (!(step > 0.0f))
        return 0u;
    const float ux = dx / step, uy = dy / step;
    float cx = 0.0f, cy = 0.0f;
    for (int i = 0; i < 4; ++i) {
        cx += 0.25f * tile.x[i];
        cy += 0.25f * tile.y[i];
    }
    const float margin = 0.5f * shape.thickness;
    // The tile reaches half its extent along the step from its centre (the step runs along the axis).
    const float reach = 0.5f * shape.extent * std::fabs(ux * shape.ax + uy * shape.ay);
    float at_canvas = 0.0f, at_view = 0.0f;
    if (!line_exit(cx, cy, ux, uy, canvas, margin, &at_canvas) || at_canvas > reach)
        return 0u;  // the strip ends inside its canvas here, or does not cross it
    if (!line_exit(cx, cy, ux, uy, visible, margin, &at_view) || at_view <= reach)
        return 0u;
    const float needed = std::ceil((at_view - reach) / step);
    return needed >= static_cast<float>(cap) ? cap : static_cast<unsigned>(needed);
}

}  // namespace shadow::ui
