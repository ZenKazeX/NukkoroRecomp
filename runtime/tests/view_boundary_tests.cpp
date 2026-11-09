// The 2D <-> 3D boundary (runtime/lift/render/view.h, docs/boundary-20261007/DESIGN.md): the authored picture, the
// displayed picture, the canvas fit and world-anchored canvas points. The invariant under test: a 2D element the game
// attaches to a world point lands on the very pixel the world point is drawn at - every output size and aspect, every
// canvas (full view, split-screen halves, a non-square-unit canvas), both framings. Pure arithmetic.
#include "render/view.h"
#include "effects/watch_camera_sheet.h"
#include "../include/gekko/display_geometry.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

using namespace shadow::render;

int failures = 0;
int checks = 0;

void check(bool value, const char* what) {
    ++checks;
    if (!value) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::uint32_t bits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

// A deterministic stream of points in front of the eye, spread over and beyond the picture.
struct Lcg {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    double next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(s >> 11) / static_cast<double>(1ull << 53);
    }
};

struct CanvasCase {
    const char* name;
    float width, height, aspect;
};

}  // namespace

int main() {
    // The display rule (gekko/display_geometry.h, docs/DISPLAY_SETTINGS_AUDIT.md section 6): output + ratio -> picture;
    // picture x resolution scale -> render.
    {
        using gekko::display_geometry;
        const gekko::DisplayGeometry a = display_geometry({800, 600}, 4.0 / 3.0, 50.0);
        check(a.picture.width == 800 && a.picture.height == 600 && a.render.width == 400 && a.render.height == 300,
              "800x600 4:3 at 50%: a 800x600 picture, 400x300 rendered");
        const gekko::DisplayGeometry b = display_geometry({800, 600}, 16.0 / 9.0, 50.0);
        check(b.picture.width == 800 && b.picture.height == 450 && b.picture_x == 0 && b.picture_y == 75 &&
                  b.render.width == 400 && b.render.height == 225,
              "800x600 16:9 at 50%: a letterboxed 800x450 picture, 400x225 rendered");
        const gekko::DisplayGeometry c = display_geometry({1920, 1080}, 4.0 / 3.0, 100.0);
        check(c.picture.width == 1440 && c.picture_x == 240 && c.render.width == 1440 && c.render.height == 1080,
              "4:3 on 1920x1080 at 100%: pillarboxed 1440x1080, rendered 1:1");
        const gekko::DisplayGeometry d2 = display_geometry({3840, 2160}, 0.0, 200.0);
        check(d2.render.width == 7680 && d2.render.height == 4320, "200% is twice each side (four times the pixels)");
        const gekko::DisplayGeometry e = display_geometry({1765, 993}, 0.0, 75.0);
        check(e.render.width == 1324 && e.render.height == 745, "75% rounds each side half away from zero");
        // The user's own setup: fullscreen 1920x1080, 32:10, 200% - unchanged.
        const gekko::DisplayGeometry u = display_geometry({1920, 1080}, 3.2, 200.0);
        check(u.picture.width == 1920 && u.picture.height == 600 && u.picture_y == 240 && u.render.width == 3840 &&
                  u.render.height == 1200 && !u.scale_capped,
              "32:10 on 1920x1080 at 200%: a 1920x600 picture, 3840x1200 rendered");
        // The 3 % snap: a window dragged to almost the ratio, a 21:9-sold panel with 21:9 chosen - no hairline bars.
        const gekko::DisplayGeometry s1 = display_geometry({1923, 600}, 3.2, 100.0);
        check(s1.picture.width == 1923 && s1.picture.height == 600 && s1.picture_x == 0,
              "a window 3 pixels off 32:10: filled, no bars");
        const gekko::DisplayGeometry s2 = display_geometry({3440, 1440}, 21.0 / 9.0, 100.0);
        check(s2.picture.width == 3440 && s2.picture.height == 1440, "21:9 on a 3440x1440 (2.39:1) panel: filled");
        const gekko::DisplayGeometry s3 = display_geometry({1920, 1080}, 16.0 / 10.0, 100.0);
        check(s3.picture.width == 1728 && s3.picture.height == 1080, "16:10 on 16:9 is 10 % off: real bars");
        // A free window with a fixed ratio: letterboxed / pillarboxed inside whatever shape it has.
        const gekko::DisplayGeometry w1 = display_geometry({1000, 1000}, 3.2, 100.0);
        check(w1.picture.width == 1000 && w1.picture.height == 313 && w1.picture_y == 343, "32:10 in a square window");
        const gekko::DisplayGeometry w2 = display_geometry({2000, 500}, 4.0 / 3.0, 100.0);
        check(w2.picture.width == 667 && w2.picture.height == 500 && w2.picture_x == 666, "4:3 in a wide window");
        // Picking a ratio while windowed shapes the window once: the same width, the ratio's height, inside the desktop.
        const gekko::PixelSize k1 = gekko::window_for_ratio({1600, 900}, 3.2, {1904, 1001});
        check(k1.width == 1600 && k1.height == 500, "32:10 picked in a 1600x900 window: 1600x500");
        const gekko::PixelSize k2 = gekko::window_for_ratio({1600, 900}, 4.0 / 3.0, {1904, 1001});
        check(k2.width == 1335 && k2.height == 1001, "4:3 picked: 1600x1200 does not fit, 1335x1001 does");
        const gekko::PixelSize k3 = gekko::window_for_ratio({1600, 900}, 0.0, {1904, 1001});
        check(k3.width == 1600 && k3.height == 900, "Auto picked: the window as it is");
        // The texture limit lowers the scale on both sides (one side clamped alone before).
        const gekko::DisplayGeometry big = display_geometry({10240, 3200}, 0.0, 200.0);
        check(big.scale_capped && std::abs(big.effective_scale - 160.0) < 1e-9 && big.render.width == 16384 &&
                  big.render.height == 5120 && big.asked_scale == 200.0,
              "10240x3200 at 200%: 160% on both sides (16384x5120), reported");
        // The scale never changes the picture, at any size.
        const double scales[] = {25, 50, 75, 100, 125, 150, 175, 200};
        const gekko::PixelSize outputs[] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {5120, 2880}, {5120, 1600},
                                            {10240, 3200}, {1000, 777}};
        for (const gekko::PixelSize o : outputs) {
            for (const double ratio : {0.0, 3.2, 4.0 / 3.0}) {
                const gekko::DisplayGeometry ref = display_geometry(o, ratio, 100.0);
                for (const double sc : scales) {
                    const gekko::DisplayGeometry q = display_geometry(o, ratio, sc);
                    check(q.picture.width == ref.picture.width && q.picture.height == ref.picture.height &&
                              q.aspect == ref.aspect && q.asked_scale == sc,
                          "a scale never changes the picture");
                    const double used = q.effective_scale;
                    check(std::abs(static_cast<double>(q.render.width) - q.picture.width * used / 100.0) <= 0.5 &&
                              std::abs(static_cast<double>(q.render.height) - q.picture.height * used / 100.0) <= 0.5 &&
                              q.render.width <= gekko::kMaxRenderSide && q.render.height <= gekko::kMaxRenderSide,
                          "render = picture x the scale in use, within the texture limit");
                }
            }
        }
        check(gekko::render_target_bytes({16384, 16384}, 8u) > (1ull << 32), "the target estimate does not overflow");
        // One aspect parser for display.cfg and the command line (--aspect refused 32:10 before).
        double ratio = -1.0;
        check(gekko::parse_aspect_value("32:10", &ratio) && ratio == 3.2, "32:10 parses exactly");
        check(gekko::parse_aspect_value("auto", &ratio) && ratio == 0.0, "auto");
        check(gekko::parse_aspect_value("64:27", &ratio) && std::abs(ratio - 64.0 / 27.0) < 1e-12, "any W:H");
        check(gekko::parse_aspect_value("1.777778", &ratio) && ratio == 16.0 / 9.0, "a number near 16:9 is 16:9");
        check(!gekko::parse_aspect_value("wide", &ratio) && !gekko::parse_aspect_value("9:0", &ratio) &&
                  !gekko::parse_aspect_value("20", &ratio),
              "nonsense and out-of-range ratios are refused");
        check(gekko::aspect_label(3440.0 / 1440.0) == "2.39:1" && gekko::aspect_label(3.2) == "32:10",
              "ratios are labelled exactly");
    }
    // The game's camera: the default camera's view window (the game's own numbers, docs/boundary-20261007/ORIGINAL.md
    // 2) and two others; the canvases a view can have.
    const ViewWindow game_windows[] = {{0.6158075f, 0.5421097f}, {0.7284736f, 0.5421097f}, {0.3594f, 0.7154f}};
    const CanvasCase canvases[] = {
        {"full view 640x480 seen 4:3", 640.0f, 480.0f, 4.0f / 3.0f},
        {"split-screen left half 320x480", 320.0f, 480.0f, 2.0f / 3.0f},
        {"split-screen top half 640x240", 640.0f, 240.0f, 8.0f / 3.0f},
        {"640x528 seen 4:3 (non-square units)", 640.0f, 528.0f, 4.0f / 3.0f},
    };
    const struct {
        double w, h;
    } outputs[] = {{640, 480}, {1280, 720}, {1920, 1080}, {2560, 1080}, {3840, 1080}, {1080, 1080}, {1080, 1920},
                   {1280, 1024}, {928, 522}, {7680, 4320}};
    const Framing framings[] = {Framing::SquarePixels, Framing::GameWindow};

    // 1. The invariant: canvas_point + the Center fit == the displayed picture's pixel; Projected is Center.
    double worst = 0.0;
    Lcg rng;
    for (const ViewWindow game : game_windows) {
        for (const CanvasCase& canvas : canvases) {
            for (const auto& out : outputs) {
                const float view_aspect = static_cast<float>(out.w / out.h);
                for (const Framing framing : framings) {
                    const ViewWindow authored = authored_window(game, canvas.aspect, framing);
                    const ViewWindow displayed = displayed_window(game, canvas.aspect, view_aspect, framing);
                    const CanvasFit centre = canvas_fit(canvas.width, canvas.height, canvas.aspect, Anchor::Center,
                                                        view_aspect);
                    const CanvasFit projected = canvas_fit(canvas.width, canvas.height, canvas.aspect,
                                                           Anchor::Projected, view_aspect);
                    check(bits(centre.sx) == bits(projected.sx) && bits(centre.ox) == bits(projected.ox) &&
                              bits(centre.sy) == bits(projected.sy) && bits(centre.oy) == bits(projected.oy),
                          "Projected is placed as Center");
                    for (int i = 0; i < 400; ++i) {
                        const double z = 1.0 + 4000.0 * rng.next();
                        const double x = (rng.next() * 6.0 - 3.0) * z * displayed.x;
                        const double y = (rng.next() * 6.0 - 3.0) * z * displayed.y;
                        const CanvasPoint p = canvas_point(x, y, z, authored, canvas.width, canvas.height);
                        const ViewPixel a = canvas_pixel(p.u, p.v, centre, out.w, out.h);
                        const ViewPixel b = world_pixel(x, y, z, displayed, out.w, out.h);
                        // Error relative to the picture's own size: a point 3 pictures off screen keeps the same
                        // relative precision as one in the middle.
                        const double ex = std::fabs(a.x - b.x) / out.w;
                        const double ey = std::fabs(a.y - b.y) / out.h;
                        const double e = ex > ey ? ex : ey;
                        if (e > worst)
                            worst = e;
                    }
                }
            }
        }
    }
    std::printf("worst 2D-vs-world disagreement: %.3g of the view (x 7680 px = %.4f px)\n", worst, worst * 7680.0);
    check(worst < 2e-6, "a world-anchored canvas point lands on its world point's pixel (every view, both framings)");

    // 2. The canvas point does not depend on the output: it is computed from the authored picture alone (the game
    // computes it without knowing the window), and the authored picture spans the canvas exactly.
    for (const CanvasCase& canvas : canvases) {
        const ViewWindow authored = authored_window(game_windows[0], canvas.aspect, Framing::SquarePixels);
        const CanvasPoint left = canvas_point(-authored.x * 10.0, 0.0, 10.0, authored, canvas.width, canvas.height);
        const CanvasPoint right = canvas_point(authored.x * 10.0, 0.0, 10.0, authored, canvas.width, canvas.height);
        const CanvasPoint top = canvas_point(0.0, authored.y * 10.0, 10.0, authored, canvas.width, canvas.height);
        const CanvasPoint bottom = canvas_point(0.0, -authored.y * 10.0, 10.0, authored, canvas.width, canvas.height);
        check(std::fabs(left.u) < 1e-4f && std::fabs(right.u - canvas.width) < 1e-3f, "authored width = canvas width");
        check(std::fabs(top.v) < 1e-4f && std::fabs(bottom.v - canvas.height) < 1e-3f,
              "authored height = canvas height");
        check(left.depth == 10.0f, "depth is the forward distance");
    }

    // 3. Square pixels: the displayed picture's aspect is the view's on every output; the vertical view is the game's
    // on views at least as wide as the canvas (Hor+), widened vertically on narrower ones (Vert+).
    for (const CanvasCase& canvas : canvases) {
        for (const auto& out : outputs) {
            const float view_aspect = static_cast<float>(out.w / out.h);
            const ViewWindow d = displayed_window(game_windows[0], canvas.aspect, view_aspect, Framing::SquarePixels);
            check(std::fabs(d.x / d.y - view_aspect) < 1e-5f * view_aspect, "square pixels on every view");
            if (view_aspect >= canvas.aspect)
                check(d.y == game_windows[0].y, "Hor+: the game's vertical view is kept");
            else
                check(d.y > game_windows[0].y && d.x == game_windows[0].y * canvas.aspect,
                      "Vert+: the authored width is kept, the height grows");
        }
    }

    // 4. The game's own framing: at the canvas's aspect the displayed picture IS the game's window (its framing kept
    // exactly), widened only by the fit elsewhere.
    for (const CanvasCase& canvas : canvases) {
        const ViewWindow g = game_windows[0];
        const ViewWindow d = displayed_window(g, canvas.aspect, canvas.aspect, Framing::GameWindow);
        check(d.x == g.x && d.y == g.y, "GameWindow at the canvas aspect is the game's window");
        const ViewWindow wide = displayed_window(g, canvas.aspect, canvas.aspect * 2.0f, Framing::GameWindow);
        check(std::fabs(wide.x - 2.0f * g.x) < 1e-6f && wide.y == g.y, "GameWindow: Hor+ widens the game's window");
    }

    // 5. The fit at the canvas's own aspect is the identity placement for every anchor but Fill's equal: Center, Left,
    // Right and Fill coincide; on a wider view Left keeps the canvas's left edge on the view's, Right its right edge.
    {
        const CanvasCase& c = canvases[0];
        const CanvasFit fill = canvas_fit(c.width, c.height, c.aspect, Anchor::Fill, c.aspect);
        for (const Anchor a : {Anchor::Left, Anchor::Center, Anchor::Right, Anchor::Projected}) {
            const CanvasFit f = canvas_fit(c.width, c.height, c.aspect, a, c.aspect);
            check(std::fabs(f.sx - fill.sx) < 1e-9f && std::fabs(f.ox - fill.ox) < 1e-6f, "same aspect: fit = fill");
        }
        const float wide = 16.0f / 9.0f;
        const CanvasFit left = canvas_fit(c.width, c.height, c.aspect, Anchor::Left, wide);
        const CanvasFit right = canvas_fit(c.width, c.height, c.aspect, Anchor::Right, wide);
        check(std::fabs(canvas_pixel(0.0f, 0.0f, left, 1920, 1080).x) < 1e-6, "Left keeps the left edge");
        check(std::fabs(canvas_pixel(c.width, 0.0f, right, 1920, 1080).x - 1920.0) < 1e-3, "Right keeps the right edge");
        const CanvasFit centre = canvas_fit(c.width, c.height, c.aspect, Anchor::Center, wide);
        check(std::fabs(canvas_pixel(320.0f, 240.0f, centre, 1920, 1080).x - 960.0) < 1e-3 &&
                  std::fabs(canvas_pixel(320.0f, 240.0f, centre, 1920, 1080).y - 540.0) < 1e-3,
              "Center keeps the canvas centre on the view's");
        check(std::fabs(canvas_pixel(0.0f, 0.0f, centre, 1920, 1080).x - 240.0) < 1e-3,
              "16:9: the 4:3 box starts 240 px in at 1080p");
        // A narrower view: the box fills the width, vertically centred (the canvas is contained, never cut).
        const CanvasFit tall = canvas_fit(c.width, c.height, c.aspect, Anchor::Center, 9.0f / 16.0f);
        const ViewPixel tl = canvas_pixel(0.0f, 0.0f, tall, 1080, 1920);
        const ViewPixel br = canvas_pixel(c.width, c.height, tall, 1080, 1920);
        check(std::fabs(tl.x) < 1e-6 && std::fabs(br.x - 1080.0) < 1e-3, "narrow: the box fills the width");
        check(std::fabs((br.y - tl.y) - 1080.0 * 3.0 / 4.0) < 1e-2 && std::fabs(tl.y + br.y - 1920.0) < 1e-2,
              "narrow: the box keeps its aspect, vertically centred");

        // Cover: the box keeps its aspect and covers the view, centred, the overflow cut - never stretched.
        const CanvasFit cover = canvas_fit(c.width, c.height, c.aspect, Anchor::Cover, wide);
        const ViewPixel ctl = canvas_pixel(0.0f, 0.0f, cover, 1920, 1080);
        const ViewPixel cbr = canvas_pixel(c.width, c.height, cover, 1920, 1080);
        check(std::fabs(ctl.x) < 1e-3 && std::fabs(cbr.x - 1920.0) < 1e-3, "cover, wide: the box fills the width");
        check(std::fabs((cbr.y - ctl.y) - 1920.0 * 3.0 / 4.0) < 1e-2 && std::fabs(ctl.y + cbr.y - 1080.0) < 1e-2,
              "cover, wide: the box keeps its aspect, its overflow cut evenly at the top and bottom");
        const CanvasFit cover_tall = canvas_fit(c.width, c.height, c.aspect, Anchor::Cover, 9.0f / 16.0f);
        const ViewPixel ttl = canvas_pixel(0.0f, 0.0f, cover_tall, 1080, 1920);
        const ViewPixel tbr = canvas_pixel(c.width, c.height, cover_tall, 1080, 1920);
        check(std::fabs(ttl.y) < 1e-3 && std::fabs(tbr.y - 1920.0) < 1e-3, "cover, narrow: the box fills the height");
        check(std::fabs((tbr.x - ttl.x) - 1920.0 * 4.0 / 3.0) < 1e-2 && std::fabs(ttl.x + tbr.x - 1080.0) < 1e-2,
              "cover, narrow: the box keeps its aspect, its overflow cut evenly at the sides");
        const CanvasFit cover_same = canvas_fit(c.width, c.height, c.aspect, Anchor::Cover, c.aspect);
        check(std::fabs(cover_same.sx - fill.sx) < 1e-9f && std::fabs(cover_same.sy - fill.sy) < 1e-9f &&
                  std::fabs(cover_same.oy - fill.oy) < 1e-6f,
              "cover at the canvas aspect is the fill");
    }

    // 6. The renderer's numbers are unchanged where they were right: square pixels on views at least as wide as the
    // canvas are the vertical window times the view's aspect, bit for bit, and the canvas fit is the formula the
    // renderer used (s = canvas aspect / view aspect, x' = a + s (x - a)).
    {
        const ViewWindow g = game_windows[0];
        for (const auto& out : outputs) {
            const float view_aspect = static_cast<float>(out.w) / static_cast<float>(out.h);
            if (view_aspect < 4.0f / 3.0f)
                continue;
            const ViewWindow d = displayed_window(g, 4.0f / 3.0f, view_aspect, Framing::SquarePixels);
            check(bits(d.x) == bits(g.y * view_aspect) && bits(d.y) == bits(g.y),
                  "square pixels: window_x = window_y x aspect, bit for bit");
            const float s = (4.0f / 3.0f) / view_aspect;
            for (const Anchor a : {Anchor::Left, Anchor::Center, Anchor::Right}) {
                const float point = a == Anchor::Left ? 0.0f : a == Anchor::Right ? 640.0f : 320.0f;
                const CanvasFit f = canvas_fit(640.0f, 480.0f, 4.0f / 3.0f, a, view_aspect);
                check(bits(f.sx) == bits(2.0f * s / 640.0f) &&
                          bits(f.ox) == bits(2.0f * point * (1.0f - s) / 640.0f - 1.0f) &&
                          bits(f.sy) == bits(-2.0f / 480.0f) && bits(f.oy) == bits(1.0f),
                      "the canvas fit is the renderer's formula, bit for bit");
            }
        }
    }

    // 7. The crosshair defect, as measured (docs/boundary-20261007/DIVERGENCE.md): the game placed the enemy gauge on
    // its canvas with its own window (0.6158, 0.5421) while the world was drawn with square pixels - 16.84 px apart
    // at 1280x720 for the enemy at view slope (-0.13953, -0.40175). With the canvas point from the authored picture
    // the two meet.
    {
        const ViewWindow g = game_windows[0];
        const double z = 250.42700195, x = -0.13952778 * z, y = 0.40174838 * z;  // view space, y up
        const ViewWindow displayed = displayed_window(g, 4.0f / 3.0f, 16.0f / 9.0f, Framing::SquarePixels);
        const CanvasFit centre = canvas_fit(640.0f, 480.0f, 4.0f / 3.0f, Anchor::Center, 16.0f / 9.0f);
        const ViewPixel world = world_pixel(x, y, z, displayed, 1280, 720);
        const CanvasPoint as_game_did = canvas_point(x, y, z, g, 640.0f, 480.0f);  // GameWindow framing
        const CanvasPoint authored = canvas_point(x, y, z, authored_window(g, 4.0f / 3.0f, Framing::SquarePixels),
                                                  640.0f, 480.0f);
        const ViewPixel before = canvas_pixel(as_game_did.u, as_game_did.v, centre, 1280, 720);
        const ViewPixel after = canvas_pixel(authored.u, authored.v, centre, 1280, 720);
        std::printf("crosshair regression at 1280x720: world x %.2f, the game's canvas point x %.2f (%+.2f px), "
                    "the authored canvas point x %.2f (%+.4f px)\n",
                    world.x, before.x, before.x - world.x, after.x, after.x - world.x);
        check(std::fabs((before.x - world.x) - (-16.10)) < 0.3,
              "the measured defect is reproduced (-16.84 px with the game's truncation, -16.10 without)");
        check(std::fabs(after.x - world.x) < 1e-3 && std::fabs(after.y - world.y) < 1e-3,
              "the authored canvas point sits on the world point");
    }

    // 8. The visible part of the canvas is the view's edges through the Center fit (every canvas, every view), and the
    // whole canvas is always part of it.
    for (const CanvasCase& canvas : canvases) {
        for (const auto& out : outputs) {
            const float view_aspect = static_cast<float>(out.w / out.h);
            const CanvasRect r = visible_canvas(canvas.width, canvas.height, canvas.aspect, view_aspect);
            const CanvasFit centre = canvas_fit(canvas.width, canvas.height, canvas.aspect, Anchor::Center, view_aspect);
            const ViewPixel tl = canvas_pixel(r.left, r.top, centre, out.w, out.h);
            const ViewPixel br = canvas_pixel(r.right, r.bottom, centre, out.w, out.h);
            check(std::fabs(tl.x) < 2e-3 && std::fabs(tl.y) < 2e-3 && std::fabs(br.x - out.w) < 2e-3 &&
                      std::fabs(br.y - out.h) < 2e-3,
                  "the visible canvas is the view's edges");
            check(r.left <= 0.0f && r.top <= 0.0f && r.right >= canvas.width && r.bottom >= canvas.height,
                  "the whole canvas is always visible");
        }
    }
    {
        const CanvasRect wide = visible_canvas(640.0f, 480.0f, 4.0f / 3.0f, 16.0f / 9.0f);
        check(std::fabs(wide.left - (-320.0f / 3.0f)) < 1e-3f && std::fabs(wide.right - (640.0f + 320.0f / 3.0f)) < 1e-3f &&
                  wide.top == 0.0f && wide.bottom == 480.0f,
              "16:9 shows canvas -106.67 .. 746.67 of the 640 x 480 view");
        const CanvasRect same = visible_canvas(640.0f, 480.0f, 4.0f / 3.0f, 4.0f / 3.0f);
        check(same.left == 0.0f && same.right == 640.0f && same.top == 0.0f && same.bottom == 480.0f,
              "4:3 shows exactly the canvas");
    }

    // The renderer's camera may carry the authored window already (the lifted build's camera sync): the boundary
    // applied to it changes nothing, bit for bit - the picture, the cull cone and the canvas points stay the same.
    for (const ViewWindow& g : game_windows) {
        for (const CanvasCase& canvas : canvases) {
            for (const Framing framing : {Framing::SquarePixels, Framing::GameWindow}) {
                const ViewWindow a = authored_window(g, canvas.aspect, framing);
                const ViewWindow aa = authored_window(a, canvas.aspect, framing);
                check(bits(aa.x) == bits(a.x) && bits(aa.y) == bits(a.y), "authored_window is idempotent");
                for (const float view_aspect : {0.5f, 1.0f, 4.0f / 3.0f, 16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f,
                                                canvas.aspect, 0.0f}) {
                    const ViewWindow d = displayed_window(g, canvas.aspect, view_aspect, framing);
                    const ViewWindow da = displayed_window(a, canvas.aspect, view_aspect, framing);
                    check(bits(d.x) == bits(da.x) && bits(d.y) == bits(da.y),
                          "displayed_window of the authored window == of the game's");
                }
            }
        }
    }

    // A camera raster's canvas and view (raster_view): the whole 640 x 480 screen is 4:3 on the output's aspect; a
    // split-screen half keeps its share of both. Its canvas aspect is the raster's own w / h for every raster of the
    // 640 x 480 screen, bit for bit - the authored window a camera carries did not move when the rule became one.
    {
        const RasterView whole = raster_view(640.0f, 480.0f, 640.0f, 480.0f, 16.0f / 9.0f);
        check(bits(whole.canvas_aspect) == bits(4.0f / 3.0f) && bits(whole.view_aspect) == bits(16.0f / 9.0f),
              "the whole screen: 4:3 on the output's aspect");
        const RasterView top_half = raster_view(640.0f, 240.0f, 640.0f, 480.0f, 16.0f / 9.0f);
        check(std::fabs(top_half.canvas_aspect - 8.0f / 3.0f) < 1e-6f &&
                  std::fabs(top_half.view_aspect - 32.0f / 9.0f) < 1e-5f,
              "a horizontal half: 8:3 on twice the output's aspect");
        const RasterView left_half = raster_view(320.0f, 480.0f, 640.0f, 480.0f, 16.0f / 9.0f);
        check(std::fabs(left_half.canvas_aspect - 2.0f / 3.0f) < 1e-6f &&
                  std::fabs(left_half.view_aspect - 8.0f / 9.0f) < 1e-6f,
              "a vertical half: 2:3 on half the output's aspect");
        const RasterView unknown = raster_view(0.0f, 480.0f, 640.0f, 480.0f, 21.0f / 9.0f);
        check(bits(unknown.canvas_aspect) == bits(4.0f / 3.0f) && bits(unknown.view_aspect) == bits(21.0f / 9.0f),
              "an unreadable raster counts as the whole screen");
        for (const float w : {640.0f, 320.0f, 160.0f, 480.0f, 213.0f, 427.0f}) {
            for (const float h : {480.0f, 240.0f, 120.0f, 160.0f, 320.0f}) {
                const RasterView v = raster_view(w, h, 640.0f, 480.0f, 16.0f / 9.0f);
                check(std::fabs(v.canvas_aspect - w / h) <= 2e-7f * (w / h), "canvas aspect == the raster's w / h");
                if ((w == 640.0f || w == 320.0f || w == 160.0f) && (h == 480.0f || h == 240.0f || h == 120.0f))
                    check(bits(v.canvas_aspect) == bits(w / h), "a power-of-two share: canvas aspect bit for bit");
            }
        }
    }

    // 6. Canvas regions (split screen: the players' viewports inside the full-screen canvas). A region covering the
    // whole canvas is the canvas's own fit; a half is laid out on its half of the view exactly as a half-size canvas
    // on a half-size view; Continued is placed as Center and clipped to the view.
    {
        for (const auto& out : outputs) {
            const float view_aspect = static_cast<float>(out.w / out.h);
            for (const Anchor a : {Anchor::Left, Anchor::Center, Anchor::Right, Anchor::Fill}) {
                const CanvasFit whole = canvas_fit(640.0f, 480.0f, 4.0f / 3.0f, a, view_aspect);
                const CanvasFit region =
                    region_fit(640.0f, 480.0f, 4.0f / 3.0f, CanvasRegion{0.0f, 0.0f, 640.0f, 480.0f}, a, view_aspect);
                for (const float u : {-50.0f, 0.0f, 62.0f, 320.0f, 578.0f, 640.0f})
                    check(std::fabs((whole.sx * u + whole.ox) - (region.sx * u + region.ox)) < 1e-5f &&
                              std::fabs(whole.oy - region.oy) < 1e-5f && std::fabs(whole.sy - region.sy) < 1e-9f,
                          "a whole-canvas region is the canvas's own fit");
                // The right half: its own fit on the right half of the view.
                const CanvasFit half = canvas_fit(320.0f, 480.0f, 2.0f / 3.0f, a, view_aspect * 0.5f);
                const CanvasFit right =
                    region_fit(640.0f, 480.0f, 4.0f / 3.0f, CanvasRegion{320.0f, 0.0f, 320.0f, 480.0f}, a, view_aspect);
                for (const float u : {320.0f, 380.0f, 538.0f, 640.0f}) {
                    const double expected = 0.5 * out.w + 0.25 * out.w * (1.0 + (half.sx * (u - 320.0f) + half.ox));
                    const double got = 0.5 * out.w * (1.0 + (right.sx * u + right.ox));
                    check(std::fabs(expected - got) < 1e-3 * out.w, "the right half: its own fit on its half view");
                }
            }
            const CanvasFit centre = canvas_fit(640.0f, 480.0f, 4.0f / 3.0f, Anchor::Center, view_aspect);
            const CanvasFit continued = canvas_fit(640.0f, 480.0f, 4.0f / 3.0f, Anchor::Continued, view_aspect);
            check(bits(centre.sx) == bits(continued.sx) && bits(centre.ox) == bits(continued.ox),
                  "Continued is placed as Center");
        }
        check(clipped_to_view(Anchor::Continued) && clipped_to_view(Anchor::Projected) &&
                  !clipped_to_view(Anchor::Center) && !clipped_to_view(Anchor::Left) && !clipped_to_view(Anchor::Fill),
              "Continued and Projected are clipped to the view");
        // The 2P ring icons at 16:9 (1920 x 1080), measured placements of docs: Ring1P canvas 62..102 Left in the
        // left player's region -> 139.5..229.5 px; Ring2P 538..578 Right in the right one -> 1690.5..1780.5 px.
        const CanvasFit p1 =
            region_fit(640.0f, 480.0f, 4.0f / 3.0f, CanvasRegion{0.0f, 0.0f, 320.0f, 480.0f}, Anchor::Left, 16.0f / 9.0f);
        const CanvasFit p2 = region_fit(640.0f, 480.0f, 4.0f / 3.0f, CanvasRegion{320.0f, 0.0f, 320.0f, 480.0f},
                                        Anchor::Right, 16.0f / 9.0f);
        const auto px = [](const CanvasFit& f, float u) { return 960.0 * (1.0 + (f.sx * u + f.ox)); };
        check(std::fabs(px(p1, 62.0f) - 139.5) < 0.01 && std::fabs(px(p1, 102.0f) - 229.5) < 0.01,
              "Ring1P at the left player's left edge");
        check(std::fabs(px(p2, 538.0f) - 1690.5) < 0.01 && std::fabs(px(p2, 578.0f) - 1780.5) < 0.01,
              "Ring2P at the right player's right edge");
        // At 4:3 every region fit is the whole canvas's (the console's composition).
        for (const Anchor a : {Anchor::Left, Anchor::Center, Anchor::Right}) {
            const CanvasFit whole = canvas_fit(640.0f, 480.0f, 4.0f / 3.0f, a, 4.0f / 3.0f);
            for (const CanvasRegion r : {CanvasRegion{0.0f, 0.0f, 320.0f, 480.0f}, CanvasRegion{320.0f, 0.0f, 320.0f, 480.0f}}) {
                const CanvasFit f = region_fit(640.0f, 480.0f, 4.0f / 3.0f, r, a, 4.0f / 3.0f);
                for (const float u : {0.0f, 84.0f, 320.0f, 560.0f, 640.0f})
                    check(std::fabs((whole.sx * u + whole.ox) - (f.sx * u + f.ox)) < 1e-5f,
                          "at the canvas's own aspect a region is laid out as the whole canvas");
            }
        }
    }

    // The physical render size (render_size_for, docs/RESOLUTION_AUDIT.md): both asked -> exactly that, the host
    // making the output the asked size; no size is a multiple of the console's framebuffer.
    {
        const struct {
            std::uint32_t w, h;
        } asked[] = {{640, 480}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}, {2560, 1600},
                     {1366, 768}, {1000, 777}, {16384, 9216}};
        for (const auto a : asked) {
            const RenderSize s = render_size_for(a.w, a.h, a.w, a.h);
            check(s.width == a.w && s.height == a.h, "both asked, output of that size: rendered exactly as asked");
        }
        // An output of another aspect (an input tape's recorded output): the largest rectangle of its aspect inside the
        // asked size - never larger than asked, never an invented size.
        const RenderSize tape = render_size_for(2560, 1600, 1920, 1080);
        check(tape.width == 2560 && tape.height == 1440, "16:9 output inside 2560x1600: 2560x1440 (letterboxed)");
        const RenderSize wide = render_size_for(3440, 1440, 1920, 1080);
        check(wide.width == 2560 && wide.height == 1440, "16:9 output inside 3440x1440: 2560x1440 (pillarboxed)");
        const RenderSize square = render_size_for(1920, 1080, 640, 480);
        check(square.width == 1440 && square.height == 1080, "4:3 output inside 1920x1080: 1440x1080");
        for (const auto a : asked) {
            for (const auto o : asked) {
                const RenderSize s = render_size_for(a.w, a.h, o.w, o.h);
                check(s.width <= a.w && s.height <= a.h && (s.width == a.w || s.height == a.h),
                      "never larger than asked; one side always exactly as asked");
            }
        }
        // A windowed client that is not exactly the asked shape no longer changes an exact request (the host makes
        // the output the asked size); a height alone still follows the window.
        const RenderSize client = render_size_for(0, 1440, 1920, 1057);
        check(client.width == 2616 && client.height == 1440, "a height alone follows the output's aspect");
        const RenderSize width_only = render_size_for(3840, 0, 1920, 1080);
        check(width_only.width == 3840 && width_only.height == 2160, "a width alone follows the output's aspect");
        const RenderSize none = render_size_for(0, 0, 1920, 1080);
        check(none.width == 0 && none.height == 0, "nothing asked: the window's size");
        const RenderSize no_output = render_size_for(2560, 1440, 0, 0);
        check(no_output.width == 0 && no_output.height == 0, "no output yet: nothing decided");
    }

    // The watch camera's noise sheet (effects/watch_camera_sheet.h, docs/ASPECT_INVENTORY.md E3): sized on the window
    // the view displays, its edges 1.1 in front of the eye are the drawn picture's edges at every aspect and in both
    // framings; at the authored aspect it is the game's own size of the camera's window, bit for bit. The console's
    // size (the camera's authored window) falls short of a wider view's sides and a narrower view's top and bottom.
    {
        using shadow::effects::kWatchSheetDistance;
        using shadow::effects::SheetScale;
        using shadow::effects::watch_camera_sheet_scale;
        for (const Framing framing : {Framing::SquarePixels, Framing::GameWindow}) {
            for (const float tangent : {0.4142136f, 0.5f, 0.7002075f}) {
                const ViewWindow game{tangent * 1.3195877f, tangent};
                const ViewWindow camera = authored_window(game, kScreenAspect, framing);  // the camera sync's window
                const SheetScale console{2.0f * (camera.x * 1.1f), 2.0f * (camera.y * 1.1f)};  // fn_80268660's
                const SheetScale at_authored =
                    watch_camera_sheet_scale(displayed_window(camera, kScreenAspect, kScreenAspect, framing));
                check(bits(at_authored.x) == bits(console.x) && bits(at_authored.y) == bits(console.y),
                      "watch sheet at the authored aspect: the game's size, bit for bit");
                for (const float aspect :
                     {1.0f, 1.25f, 4.0f / 3.0f, 1.6f, 16.0f / 9.0f, 2.3703704f, 3.2f, 32.0f / 9.0f}) {
                    const ViewWindow shown = displayed_window(camera, kScreenAspect, aspect, framing);
                    const SheetScale sheet = watch_camera_sheet_scale(shown);
                    // The sheet's corner (0.5 sx, 0.5 sy) at its distance, in the drawn picture's ndc (frame_math.h
                    // projection_of: x / (z window.x)).
                    const float edge_x = (0.5f * sheet.x / kWatchSheetDistance) / shown.x;
                    const float edge_y = (0.5f * sheet.y / kWatchSheetDistance) / shown.y;
                    check(std::fabs(edge_x - 1.0f) < 1e-6f && std::fabs(edge_y - 1.0f) < 1e-6f,
                          "watch sheet: its edges are the displayed picture's");
                    const float console_x = (0.5f * console.x / kWatchSheetDistance) / shown.x;
                    const float console_y = (0.5f * console.y / kWatchSheetDistance) / shown.y;
                    if (aspect > kScreenAspect * 1.001f)
                        check(console_x < 0.999f && std::fabs(console_y - 1.0f) < 1e-6f,
                              "watch sheet, console size: a wider view's sides left bare");
                    if (aspect < kScreenAspect * 0.999f)
                        check(console_y < 0.999f && std::fabs(console_x - 1.0f) < 1e-6f,
                              "watch sheet, console size: a narrower view's top and bottom left bare");
                }
            }
        }
    }

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
