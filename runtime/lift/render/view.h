#pragma once

// The boundary between the 3D world and the 2D screen: ONE definition of how a view's camera becomes its picture and
// where the view's 2D canvas sits on that picture (docs/boundary-20261007/DESIGN.md, COORDINATE_SPACES.md). The
// renderer draws the world with displayed_window() and places canvas draws with canvas_fit(); the world culling cone
// takes displayed_window() too; 2D the game attaches to a world point (lock-on marks, enemy gauges, markers) is put on
// the canvas by canvas_point(). The three agree by construction: the authored picture is exactly the canvas's box, so
// the Center fit puts canvas_point()'s result on the very pixel displayed_window() draws the world point at - on
// every output size and aspect.
//
// Spaces (all continuous; a pixel (i, j) covers [i, i + 1) x [j, j + 1)):
//   view space  origin the eye; x screen right, y screen up, z forward (the direction the camera looks); world units.
//   window      the half extents of a picture at forward distance 1: a view-space point is in the picture when
//               |x| <= z * window.x and |y| <= z * window.y.
//   ndc         x / (z window.x), y / (z window.y): [-1, 1] spans the picture, y up.
//   canvas      a view's 2D space: (0, 0) its top-left corner, x right, y down, width x height authored units seen at
//               the authored aspect (the units need not be square: 640 x 528 seen 4:3).
//   view pixels the view's rectangle in the output: (0, 0) its top-left corner, x right, y down; ndc -1 / +1 are its
//               left / right (bottom / top) edges.
//
// Pure arithmetic: no device, no game state.

#include <cstdint>

namespace shadow::render {

// The half extents of a picture at forward distance 1 (view space, x right, y up).
struct ViewWindow {
    float x = 0.5f;
    float y = 0.375f;
};

// How a canvas draw keeps its place on a view whose aspect is not the canvas's. Left / Center / Right: the largest box
// of the canvas's aspect inside the view (the authored picture's box), at the view's left edge, centred, at its right
// edge - horizontally on a wider view, the box filling the width (vertically centred) on a narrower one; Fill: the
// canvas stretched over the whole view (draws that cover the screen). Projected: a canvas point the game derived from a
// world point (canvas_point()): placed as Center, and not clipped to the canvas's box - the world it marks extends to
// the view's edges. Continued: a decorative pattern the 2D layout continued to the view's edges (a tiled strip -
// runtime/lift/ui/tiled_strip.h): placed as Center and, as the console's raster edge clipped it, clipped to the view
// rather than to the canvas's box.
// Cover: the canvas box at its own aspect, scaled until it covers the view and centred - the overflow is cut, nothing is
// stretched (a full-screen background on a wider or narrower view; user's choice 2026-10-26).
enum class Anchor : std::uint8_t { Left, Center, Right, Fill, Projected, Continued, Cover };
inline constexpr std::uint32_t kAnchorCount = 7;

// Whether a canvas draw at `anchor` is clipped to the view (the world it marks or the pattern it continues extends to
// the view's edges) rather than to the canvas's anchored box.
inline bool clipped_to_view(Anchor anchor) noexcept {
    return anchor == Anchor::Projected || anchor == Anchor::Continued;
}

// The horizontal extent of the authored picture.
enum class Framing : std::uint8_t {
    // The game's vertical view; the width the canvas's aspect gives it: the picture is undistorted (square pixels).
    SquarePixels,
    // The game's own window as the game framed it: its horizontal extent need not agree with the canvas's aspect
    // (the game's default camera frames 0.852 of the undistorted width - see docs/boundary-20261007/ORIGINAL.md).
    GameWindow,
};

// The authored picture of a view: what the game's camera frames on the canvas's box.
inline ViewWindow authored_window(ViewWindow game, float canvas_aspect, Framing framing) noexcept {
    if (framing == Framing::GameWindow)
        return game;
    return ViewWindow{game.y * canvas_aspect, game.y};
}

// The displayed picture on a view of `view_aspect` (width / height in output pixels): the authored picture, whole and
// undistorted, widened to a wider view (Hor+) or heightened to a narrower one (Vert+).
inline ViewWindow displayed_window(ViewWindow game, float canvas_aspect, float view_aspect, Framing framing) noexcept {
    if (!(canvas_aspect > 0.0f) || !(view_aspect > 0.0f))
        return authored_window(game, canvas_aspect, framing);
    if (view_aspect >= canvas_aspect) {
        if (framing == Framing::SquarePixels)
            return ViewWindow{game.y * view_aspect, game.y};
        return ViewWindow{game.x * (view_aspect / canvas_aspect), game.y};
    }
    const ViewWindow authored = authored_window(game, canvas_aspect, framing);
    return ViewWindow{authored.x, authored.y * (canvas_aspect / view_aspect)};
}

// Both accept the authored window in the game window's place and give the same answer, bit for bit:
// authored_window(authored_window(g)) == authored_window(g), displayed_window(authored_window(g), ...) ==
// displayed_window(g, ...). The lifted build's camera sync gives the renderer's camera the authored window
// (runtime/scaffold/rw_driver/projection.cpp camera_picture_window); the legacy build's camera keeps the game's.

// The game's whole screen: its camera raster (640 x 480) seen at 4:3.
inline constexpr float kScreenAspect = 4.0f / 3.0f;

// The physical render size for the asked render_width x render_height (0 = not asked) and the game-visible output
// (docs/RESOLUTION_AUDIT.md). Both asked and the output has their aspect - the host makes the output the asked size -
// exactly as asked. Both asked but the output has another aspect (an input tape's recorded output): the largest
// rectangle of the output's aspect inside the asked size (2560x1600 asked, 16:9 output: 2560x1440, presented 1:1 with
// bars) - never a size larger than asked. One asked: that side, the other following the output's aspect so pixels stay
// square. No multiple of the console's framebuffer is involved. {0, 0}: nothing asked (render at the window's size).
struct RenderSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

inline RenderSize render_size_for(std::uint32_t asked_width, std::uint32_t asked_height, std::uint32_t output_width,
                                  std::uint32_t output_height) noexcept {
    if ((asked_width == 0u && asked_height == 0u) || output_width == 0u || output_height == 0u)
        return {};
    RenderSize size;
    if (asked_width != 0u && asked_height != 0u &&
        static_cast<std::uint64_t>(asked_width) * output_height ==
            static_cast<std::uint64_t>(asked_height) * output_width) {
        size = {asked_width, asked_height};
    } else if (asked_width != 0u && asked_height != 0u) {
        // Fit inside: the side that limits is kept exactly, the other is the output's aspect of it.
        if (static_cast<std::uint64_t>(asked_width) * output_height <
            static_cast<std::uint64_t>(asked_height) * output_width) {
            size.width = asked_width;
            size.height = static_cast<std::uint32_t>(
                static_cast<double>(asked_width) * output_height / output_width + 0.5);
        } else {
            size.height = asked_height;
            size.width = static_cast<std::uint32_t>(
                static_cast<double>(asked_height) * output_width / output_height + 0.5);
        }
    } else if (asked_height != 0u) {
        size.height = asked_height;
        size.width = static_cast<std::uint32_t>(
            static_cast<double>(asked_height) * output_width / output_height + 0.5);
    } else {
        size.width = asked_width;
        size.height = static_cast<std::uint32_t>(
            static_cast<double>(asked_width) * output_height / output_width + 0.5);
    }
    if (size.width == 0u)
        size.width = 1u;
    if (size.height == 0u)
        size.height = 1u;
    return size;
}

// A camera raster's canvas and view: the raster's share of its top-level raster - (w / top w) of the width, (h / top h)
// of the height - is its share of the screen's canvas (the canvas aspect: the screen's 4:3 times the share) and of the
// output (the view aspect: the output's aspect times the share). The whole screen: 4:3 and the output's aspect; a
// split-screen half has its own shape. THE rule for both the authored window a camera carries and the displayed
// picture it is drawn and culled with.
struct RasterView {
    float canvas_aspect = kScreenAspect;
    float view_aspect = kScreenAspect;
};

inline RasterView raster_view(float width, float height, float top_width, float top_height,
                              float output_aspect) noexcept {
    float share = 1.0f;
    if (width > 0.0f && height > 0.0f && top_width > 0.0f && top_height > 0.0f)
        share = (width / top_width) / (height / top_height);
    return RasterView{kScreenAspect * share, output_aspect * share};
}

// The canvas fit: canvas units -> the view's ndc, ndc_x = sx u + ox, ndc_y = sy v + oy (the canvas is y down).
struct CanvasFit {
    float sx = 1.0f, ox = 0.0f;
    float sy = -1.0f, oy = 1.0f;
};

inline CanvasFit canvas_fit(float canvas_width, float canvas_height, float canvas_aspect, Anchor anchor,
                            float view_aspect) noexcept {
    const float w = canvas_width;
    const float h = canvas_height;
    CanvasFit fit;
    fit.sx = 2.0f / w;
    fit.ox = -1.0f;
    fit.sy = -2.0f / h;
    fit.oy = 1.0f;
    if (anchor == Anchor::Fill || !(canvas_aspect > 0.0f) || !(view_aspect > 0.0f))
        return fit;
    if (anchor == Anchor::Cover) {
        if (view_aspect >= canvas_aspect) {
            // A wider view: the box fills the width and overflows the height, vertically centred.
            const float t = view_aspect / canvas_aspect;
            fit.sy = -2.0f * t / h;
            fit.oy = t;
        } else {
            // A narrower view: the box fills the height and overflows the width, horizontally centred.
            const float s = canvas_aspect / view_aspect;
            fit.sx = 2.0f * s / w;
            fit.ox = -s;
        }
        return fit;
    }
    if (view_aspect >= canvas_aspect) {
        // The box fills the view's height; s is its share of the width, `a` the canvas point kept on the view's.
        const float s = canvas_aspect / view_aspect;
        const float a = anchor == Anchor::Left ? 0.0f : anchor == Anchor::Right ? w : 0.5f * w;
        fit.sx = 2.0f * s / w;
        fit.ox = 2.0f * a * (1.0f - s) / w - 1.0f;
        return fit;
    }
    // A narrower view: the box fills the width, vertically centred; t is its share of the height.
    const float t = view_aspect / canvas_aspect;
    fit.sy = -2.0f * t / h;
    fit.oy = t;
    return fit;
}

// A region of a view's canvas laid out on its own: a player's viewport inside the full-screen canvas of split screen.
// The game composes each player's 2D (its rings, its command window, its item icons) inside that player's viewport
// rectangle of its 640 x 480 screen (docs/renderer-re-20261007/07_2D.md 3.11), and the viewport is the same share of
// the output as of the canvas; so a region is laid out against the part of the view it covers, exactly as a whole
// canvas is against its view. In canvas units of the view's canvas: x, y its top-left corner, width x height its size.
struct CanvasRegion {
    float x = 0.0f, y = 0.0f;
    float width = 0.0f, height = 0.0f;
};

// The fit of a canvas region: canvas units of the VIEW's canvas -> the view's ndc - the region's own canvas_fit placed
// on the region's share of the view. `canvas_aspect` / `view_aspect` are the whole canvas's and view's; the region's
// are those times its share ((width / canvas width) / (height / canvas height)).
inline CanvasFit region_fit(float canvas_width, float canvas_height, float canvas_aspect, const CanvasRegion& region,
                            Anchor anchor, float view_aspect) noexcept {
    const float fw = region.width / canvas_width;
    const float fh = region.height / canvas_height;
    const float fx = region.x / canvas_width;
    const float fy = region.y / canvas_height;
    const float share = fw / fh;
    const CanvasFit local =
        canvas_fit(region.width, region.height, canvas_aspect * share, anchor, view_aspect * share);
    // Region ndc -> the region's fraction of the view -> view ndc, in x (right) and y (up; the canvas is y down).
    CanvasFit out;
    out.sx = fw * local.sx;
    out.ox = 2.0f * fx + fw * (local.ox + 1.0f - local.sx * region.x) - 1.0f;
    out.sy = fh * local.sy;
    out.oy = 1.0f - 2.0f * fy - fh + fh * (local.oy - local.sy * region.y);
    return out;
}

// A view-space point's place on the canvas: the canvas point the Center fit puts on the pixel the displayed picture
// draws the point at (any view aspect), from the authored picture alone - so it does not depend on the output.
// `depth` is the point's forward distance (z); a point at or behind the eye (depth <= 0) has no place.
struct CanvasPoint {
    float u = 0.0f, v = 0.0f;
    float depth = 0.0f;
};

inline CanvasPoint canvas_point(double x, double y, double z, ViewWindow authored, float canvas_width,
                                float canvas_height) noexcept {
    CanvasPoint p;
    p.depth = static_cast<float>(z);
    const double ndc_x = x / (z * static_cast<double>(authored.x));
    const double ndc_y = y / (z * static_cast<double>(authored.y));
    p.u = static_cast<float>(0.5 * static_cast<double>(canvas_width) * (1.0 + ndc_x));
    p.v = static_cast<float>(0.5 * static_cast<double>(canvas_height) * (1.0 - ndc_y));
    return p;
}

// The part of the canvas a view shows: the view's edges through the Center fit, in canvas units - the canvas itself
// when the view has the canvas's aspect, wider on a wider view (Hor+), taller on a narrower one. What "on the screen"
// means for 2D placed at a projected point.
struct CanvasRect {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
};

inline CanvasRect visible_canvas(float canvas_width, float canvas_height, float canvas_aspect,
                                 float view_aspect) noexcept {
    CanvasRect r{0.0f, 0.0f, canvas_width, canvas_height};
    if (!(canvas_aspect > 0.0f) || !(view_aspect > 0.0f))
        return r;
    if (view_aspect >= canvas_aspect) {
        const float half = 0.5f * canvas_width * (view_aspect / canvas_aspect);
        r.left = 0.5f * canvas_width - half;
        r.right = 0.5f * canvas_width + half;
    } else {
        const float half = 0.5f * canvas_height * (canvas_aspect / view_aspect);
        r.top = 0.5f * canvas_height - half;
        r.bottom = 0.5f * canvas_height + half;
    }
    return r;
}

// The view-pixel position of a view-space point drawn with the displayed picture (the renderer's projection, for
// checks and tools), and of a canvas point placed by a fit: the two meet for canvas_point()'s result and Center.
struct ViewPixel {
    double x = 0.0, y = 0.0;
};

inline ViewPixel world_pixel(double x, double y, double z, ViewWindow displayed, double view_width,
                             double view_height) noexcept {
    const double ndc_x = x / (z * static_cast<double>(displayed.x));
    const double ndc_y = y / (z * static_cast<double>(displayed.y));
    return ViewPixel{0.5 * view_width * (1.0 + ndc_x), 0.5 * view_height * (1.0 - ndc_y)};
}

inline ViewPixel canvas_pixel(float u, float v, const CanvasFit& fit, double view_width, double view_height) noexcept {
    const double ndc_x = static_cast<double>(fit.sx) * u + fit.ox;
    const double ndc_y = static_cast<double>(fit.sy) * v + fit.oy;
    return ViewPixel{0.5 * view_width * (1.0 + ndc_x), 0.5 * view_height * (1.0 - ndc_y)};
}

}  // namespace shadow::render
