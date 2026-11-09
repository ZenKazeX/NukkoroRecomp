// The lens flare, natively: where the glow, the ghosts and the flash of a sun's lens flare go on a view, and whether
// they are shown. The game's effect library keeps the flare's data (the sun, the element table, the intensity); this
// is its layout - fn_80432360 reconstructed (docs/boundary-20261007/FLARE.md), with the screen it tests being the view
// the output shows instead of the console's 640 x 480 raster. Pure arithmetic, no guest memory.
//
// Coordinates are the camera raster's canvas units (x right, y down; the raster's top-left is 0, 0), the space the
// game's 2D lives in; render/view.h places them on the view (the glow and the ghosts Projected, the flash Fill).
#pragma once

#include <cmath>
#include <cstdint>

namespace shadow::effects {

// One element of a flare (the game's table: 0x24 bytes each). Where it sits on the line from the view's centre (0)
// through the sun (1, beyond: further out; negative: past the centre), its size in canvas units, its colour in the
// order the draw submits it; the alpha is the strength the fade scales (the glow's is used as it is).
struct FlareElement {
    float position = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
};

// The camera the flare is drawn with, as the layout needs it.
struct FlareView {
    float raster_width = 640.0f;   // the camera's raster (its viewport) in canvas units
    float raster_height = 480.0f;
    float shown_width = 640.0f;    // the part of the canvas plane the output shows of it (render/view.h
    float shown_height = 480.0f;   // visible_canvas): the raster at its own aspect, wider / taller otherwise
    float near_plane = 1.0f;
    float far_plane = 1000.0f;
    float depth_near = 0.0f;       // the screen depth range of the 2D draws (RwIm2D's near / far screen z)
    float depth_far = 1.0f;
};

struct FlareVertex {
    float x = 0.0f, y = 0.0f;
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
};

// One frame's flare. The quads' corners in the draw's strip order: (left, top), (left, bottom), (right, top),
// (right, bottom).
struct FlareFrame {
    bool in_front = false;    // the sun is not behind the near plane: the flash quad covers the raster
    bool glow = false;        // the glow (element 0, at the sun, depth tested) is drawn
    bool ghosts = false;      // the ghosts (elements 1 ..) and the flash are drawn
    FlareVertex glow_quad[4];
    float glow_depth = 0.0f;  // the glow's screen depth: the sun's
    std::uint8_t flash_level = 0;  // the flash's grey (additive; alpha 255)
    float fade = 0.0f;        // the ghosts' and the flash's strength, 0 .. 1
};

// The screen the flare tests: the view the output shows (never smaller than the raster).
inline float flare_screen_width(const FlareView& v) noexcept {
    return v.shown_width > v.raster_width ? v.shown_width : v.raster_width;
}
inline float flare_screen_height(const FlareView& v) noexcept {
    return v.shown_height > v.raster_height ? v.shown_height : v.raster_height;
}

namespace detail {

// One quad of an element about (cx, cy): the corners as the game computes them (each a single-rounded fused step).
inline void flare_quad(float cx, float cy, float width, float height, FlareVertex* q) noexcept {
    const float left = -std::fma(0.5f, width, -cx);
    const float right = std::fma(0.5f, width, cx);
    const float top = -std::fma(0.5f, height, -cy);
    const float bottom = std::fma(0.5f, height, cy);
    q[0].x = left;  q[0].y = top;
    q[1].x = left;  q[1].y = bottom;
    q[2].x = right; q[2].y = top;
    q[3].x = right; q[3].y = bottom;
}

// fctiwz: toward zero; the game stores the low byte.
inline std::uint8_t truncated_byte(float v) noexcept {
    if (!(v == v) || v >= 2147483647.0f || v <= -2147483648.0f)
        return 0;
    return static_cast<std::uint8_t>(static_cast<std::int32_t>(v) & 0xFF);
}

}  // namespace detail

// The flare for one frame. `sun` is the sun through the camera's view matrix (RenderWare's: x and y carry the raster
// fractions times z, z the forward distance). `ghosts` receives 4 vertices per element 1 .. count - 1 (positions and
// colours). Returns the frame; nothing is drawn when the sun is behind the near plane.
//
// The console's rules and what became of them (FLARE.md):
//  - Ghosts (and the flash, which shares their fade): the console showed them while the sun lay within the square of
//    half the raster's larger side about its centre, fading with the distance to 0 at that radius - a field laid over
//    its picture (at 640 x 480: 0 at the left / right edges, 80 units past the top / bottom ones). Here that field is
//    laid over the view the output shows: each axis of the sun's offset is measured in the raster's units of that
//    axis's shown extent (x * W / shown W, y * H / shown H), then the console's square and fade apply. A wider picture
//    stretches the field sideways only - the flare follows the sun into the side bands and fades out at the real
//    edges, while above and below the picture it reaches exactly as far as the console's did (FLARE.md 6, 10: the
//    first rule took half the SHOWN larger side on both axes, which at 16:9 lit the flash for a sun 186 units above
//    the picture and, with the stage-start flash ramp, drove the glare into a whole-screen blink at the start of Sky
//    Troops). At the raster's aspect the scales are exactly 1: the console's arithmetic, bit for bit.
//  - Glow: the console kept it while x lay in [-0.5 W, 1.5 W] and y in [-0.5 H, 1.5 H] - half a screen beyond each edge
//    in raster coordinates - but compared x, y measured from the centre: the glow vanished as soon as the sun crossed
//    the left or top edge and lingered a screen beyond the right or bottom one. The bounds' intent over the shown
//    screen: half a screen beyond each edge, |x| <= width, |y| <= height.
//  - The distance is the true length; the console took RenderWare's 12-bit table square root (RwV2dLength).
inline FlareFrame layout_lens_flare(const float sun[3], const FlareView& view, const FlareElement* elements,
                                    std::int32_t count, std::uint8_t intensity, std::uint8_t flash,
                                    FlareVertex* ghosts) noexcept {
    FlareFrame f;
    // Behind the near plane: nothing (a NaN depth passes, as the console's ordered compare let it).
    if (sun[2] < view.near_plane)
        return f;
    f.in_front = true;
    const float width = view.raster_width;
    const float height = view.raster_height;
    const float u = sun[0] / sun[2];
    const float v = sun[1] / sun[2];
    const float centre_x = 0.5f * width;
    const float centre_y = 0.5f * height;
    // The sun from the raster's centre, in canvas units.
    const float x = std::fma(width, u, -centre_x);
    const float y = std::fma(height, v, -centre_y);
    const float screen_w = flare_screen_width(view);
    const float screen_h = flare_screen_height(view);

    // (Each rounding step its own statement: the build contracts nothing, and the game rounded every one.)
    // The console's field: half the raster's larger side. The sun's offset in the raster's units of each shown axis
    // (a scale of exactly 1 where the output shows the raster's own extent, so x and y pass unchanged).
    const float radius = 0.5f * (width > height ? width : height);
    const float scale_x = width / screen_w;
    const float scale_y = height / screen_h;
    const float field_x = x * scale_x;
    const float field_y = y * scale_y;
    f.ghosts = !(field_x > radius || field_x < -radius || field_y > radius || field_y < -radius);
    if (f.ghosts) {
        const float xx = field_x * field_x;
        const float yy = field_y * field_y;
        const float squared = xx + yy;
        const float length = std::sqrt(squared);
        const float room = radius - length;
        float fade = room / radius;
        if (!(fade >= 0.0f))
            fade = 0.0f;
        const float strength = static_cast<float>(intensity) / 255.0f;
        fade = fade * strength;
        f.fade = fade;
        for (std::int32_t i = 1; i < count; ++i) {
            const FlareElement& e = elements[i];
            FlareVertex* q = ghosts + 4 * (i - 1);
            detail::flare_quad(std::fma(x, e.position, centre_x), std::fma(y, e.position, centre_y), e.width,
                               e.height, q);
            const std::uint8_t alpha = detail::truncated_byte(fade * static_cast<float>(e.a));
            for (int k = 0; k < 4; ++k) {
                q[k].r = e.r;
                q[k].g = e.g;
                q[k].b = e.b;
                q[k].a = alpha;
            }
        }
        f.flash_level = detail::truncated_byte(static_cast<float>(flash) * fade);
    }

    f.glow = !(x > screen_w || x < -screen_w || y > screen_h || y < -screen_h);
    if (f.glow && count > 0) {
        const FlareElement& e = elements[0];
        // The sun's screen depth (RenderWare's perspective depth over the 2D draws' range).
        const float span = view.far_plane - view.near_plane;
        const float ratio = view.far_plane / sun[2];
        const float depth_span = view.depth_far - view.depth_near;
        const float scale = depth_span / span;
        const float ahead = sun[2] - view.near_plane;
        const float scaled = scale * ahead;
        const float depth = ratio * scaled;
        f.glow_depth = view.depth_near + depth;
        detail::flare_quad(std::fma(e.position, x, centre_x), std::fma(e.position, y, centre_y), e.width, e.height,
                           f.glow_quad);
        for (FlareVertex& q : f.glow_quad) {
            q.r = e.r;
            q.g = e.g;
            q.b = e.b;
            q.a = e.a;
        }
    }
    return f;
}

}  // namespace shadow::effects
