// Tiled strips (runtime/lift/ui/tiled_strip.h; docs/LOADING_SCREEN_DECOMPOSITION.md 3): the stage loading screen's two
// rotating line chains, rebuilt from their authored data (tile 256 x 9 at x 1.05 / 256 x 5; local offsets
// -256 / 0 / +256 and -128 / +128 / +342 along the parent's rotation; pivots (372, 222) and (226, 306)), recognised as
// strips and continued to the edge of the part of the canvas a view shows - none on a 4:3 view at any angle of the
// animation, exactly as many copies as the decomposition measured at 16:9 and 21:9 - and the false positives the rules
// exist for rejected. Pure arithmetic.
#include "ui/tiled_strip.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace shadow;

int failures = 0;
int checks = 0;

void check(bool value, const char* what) {
    ++checks;
    if (!value) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr double kPi = 3.14159265358979323846;

// A tile of `length` x `thickness` centred at (cx, cy), its long axis at `degrees` (y down: a positive angle turns
// the axis upward on screen, as the CSD rotation does), vertices in strip order, the full texture width.
ui::StripTile tile(double cx, double cy, double degrees, double length, double thickness) {
    const double r = degrees * kPi / 180.0;
    const double ax = std::cos(r), ay = -std::sin(r);   // along
    const double nx = -ay, ny = ax;                     // across
    const double h = 0.5 * length, w = 0.5 * thickness;
    const double sx[4] = {-h, h, -h, h}, sy[4] = {-w, -w, w, w};
    ui::StripTile t;
    for (int i = 0; i < 4; ++i) {
        t.x[i] = static_cast<float>(cx + sx[i] * ax + sy[i] * nx);
        t.y[i] = static_cast<float>(cy + sx[i] * ay + sy[i] * ny);
    }
    t.u0 = 0.0f;
    t.u1 = 1.0f;
    t.v0 = 0.7031f;
    t.v1 = 0.7422f;
    return t;
}

std::vector<ui::StripTile> chain(double px, double py, double degrees, double length, double thickness,
                                 std::initializer_list<double> offsets) {
    const double r = degrees * kPi / 180.0;
    std::vector<ui::StripTile> out;
    for (const double o : offsets)
        out.push_back(tile(px + o * std::cos(r), py - o * std::sin(r), degrees, length, thickness));
    return out;
}

struct Continued {
    bool strip = false;
    unsigned low = 0, high = 0;
    bool covers = false;  // the strip with its copies reaches the visible edge at both ends it continues
};

Continued continue_strip(const std::vector<ui::StripTile>& tiles, float view_aspect) {
    Continued c;
    ui::TileShape shape;
    if (!ui::tile_shape(tiles[0], &shape))
        return c;
    for (const ui::StripTile& t : tiles)
        if (&t != &tiles[0] && !ui::continues_chain(tiles[0], shape, t))
            return c;
    ui::StripEnds ends;
    if (!ui::strip_ends(tiles, shape, &ends))
        return c;
    c.strip = true;
    const render::CanvasRect canvas{0.0f, 0.0f, 640.0f, 480.0f};
    const render::CanvasRect visible = render::visible_canvas(640.0f, 480.0f, 4.0f / 3.0f, view_aspect);
    c.low = ui::continuation_count(tiles[ends.low], shape, ends.low_dx, ends.low_dy, canvas, visible);
    c.high = ui::continuation_count(tiles[ends.high], shape, ends.high_dx, ends.high_dy, canvas, visible);
    // Coverage: where an end is continued, its last copy reaches the visible edge along the line.
    c.covers = true;
    const auto reaches = [&](const ui::StripTile& t, float dx, float dy, unsigned n) {
        const float step = std::hypot(dx, dy);
        const float ux = dx / step, uy = dy / step;
        float cx = 0.0f, cy = 0.0f;
        for (int i = 0; i < 4; ++i) {
            cx += 0.25f * t.x[i];
            cy += 0.25f * t.y[i];
        }
        float at_canvas = 0.0f, at_view = 0.0f;
        const float m = 0.5f * shape.thickness;
        const float reach = 0.5f * shape.extent * std::fabs(ux * shape.ax + uy * shape.ay);
        if (!ui::line_exit(cx, cy, ux, uy, canvas, m, &at_canvas) || at_canvas > reach)
            return n == 0u;  // not continued: nothing to reach
        if (!ui::line_exit(cx, cy, ux, uy, visible, m, &at_view))
            return n == 0u;
        return reach + static_cast<float>(n) * step >= at_view &&
               (n == 0u || reach + static_cast<float>(n - 1u) * step < at_view);  // and not one copy more
    };
    c.covers = reaches(tiles[ends.low], ends.low_dx, ends.low_dy, c.low) &&
               reaches(tiles[ends.high], ends.high_dx, ends.high_dy, c.high);
    return c;
}

}  // namespace

int main() {
    const float aspects[] = {4.0f / 3.0f, 16.0f / 10.0f, 16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f};

    // 1. The captured frame (docs/LOADING_SCREEN_DECOMPOSITION.md 3.2: chain B at 24.94 degrees, steps (232.1,
    // -107.9) and (194.05, -90.25)): 4:3 none; 16:9 one low + one high; 21:9 one low + two high.
    {
        const double angle = std::atan2(107.9, 232.1) * 180.0 / kPi;
        const auto b = chain(226.0, 306.0, angle, 256.0, 5.0, {-128.0, 128.0, 342.0});
        const Continued c43 = continue_strip(b, 4.0f / 3.0f);
        check(c43.strip && c43.low == 0u && c43.high == 0u, "chain B at 4:3: a strip, nothing added");
        const Continued c169 = continue_strip(b, 16.0f / 9.0f);
        check(c169.strip && c169.low == 1u && c169.high == 1u && c169.covers, "chain B at 16:9: one low, one high");
        const Continued c219 = continue_strip(b, 21.0f / 9.0f);
        check(c219.strip && c219.low == 1u && c219.high == 2u && c219.covers, "chain B at 21:9: one low, two high");
    }

    // 2. The whole animation (chain A 80..140 degrees, chain B 25..-5), every aspect: a strip in every frame; 4:3
    // never adds a copy; wider views add exactly the copies that reach the visible edge.
    for (double t = 0.0; t <= 1.0; t += 1.0 / 64.0) {
        const auto a = chain(372.0, 222.0, 80.0 + 60.0 * t, 268.8, 9.45, {-268.8, 0.0, 268.8});
        const auto b = chain(226.0, 306.0, 25.0 - 30.0 * t, 256.0, 5.0, {-128.0, 128.0, 342.0});
        for (const float aspect : aspects) {
            const Continued ca = continue_strip(a, aspect);
            const Continued cb = continue_strip(b, aspect);
            check(ca.strip && cb.strip, "both chains are strips in every frame");
            check(ca.covers && cb.covers, "a continued end reaches the visible edge, with no copy to spare");
            if (aspect == 4.0f / 3.0f)
                check(ca.low + ca.high + cb.low + cb.high == 0u, "a 4:3 view never gets a copy");
        }
    }

    // 3. What must not be continued.
    {
        // The title logo: a column of identical wide quads stepping ACROSS their long axis.
        std::vector<ui::StripTile> column;
        for (int i = 0; i < 4; ++i)
            column.push_back(tile(320.0, 100.0 + 12.0 * i, 0.0, 400.0, 40.0));
        ui::TileShape shape;
        check(ui::tile_shape(column[0], &shape) && !ui::continues_chain(column[0], shape, column[1]),
              "a column stepping across the tiles' axis is not a chain");
        // An atlas cell that does not span the texture width.
        ui::StripTile cell = tile(100.0, 100.0, 0.0, 128.0, 16.0);
        cell.u1 = 0.5f;
        check(!ui::tile_shape(cell, &shape), "a cell narrower than its texture is not a tile");
        // Tiles twice their length apart: not abutting.
        const auto sparse = chain(320.0, 240.0, 10.0, 100.0, 4.0, {-200.0, 0.0, 200.0});
        ui::TileShape s2;
        ui::StripEnds ends;
        check(ui::tile_shape(sparse[0], &s2) && !ui::strip_ends(sparse, s2, &ends), "gaps of two tiles: no strip");
        // Other texture coordinates: not a translation.
        ui::StripTile other = sparse[1];
        other.v0 = 0.1f;
        check(!ui::continues_chain(sparse[0], s2, other), "different texture coordinates: not the same tile");
        // A strip ending inside its canvas stays as it is.
        const auto short_line = chain(320.0, 240.0, 0.0, 100.0, 4.0, {-100.0, 0.0, 100.0});
        const Continued cs = continue_strip(short_line, 32.0f / 9.0f);
        check(cs.strip && cs.low == 0u && cs.high == 0u, "a strip the artist ended inside the picture is not continued");
    }

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
