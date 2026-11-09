#pragma once

// NativeWorldAnchorProjection: the one service for everything whose position
// originates in 3D world space but whose presentation is 2D.
//
// THE ARCHITECTURE IT REPLACES
//
// Five guest subsystems each did this, independently:
//
//     world position
//       -> a game-camera projection (0x8000F654 / 0x8000F57C, FOV from the game
//          camera object at 0x8000D0F0(global,k)+0x24 - NOT the RwCamera)
//       -> 640x480 guest framebuffer pixels, via the camera-mode table rect at
//          0x8058516C + k*48 + 8
//       -> a clip against that fixed rectangle, or against +/-1 NDC
//       -> CSD / Sprite3D positioning
//       -> the host's layout_hud affine, repairing the position afterwards
//
// Every step after the first is legacy baggage. The projection used a 63.2501
// deg cone while the display showed 78.777 deg at 16:9, so the clip rejected
// elements that were still on screen: MEASURED on Final Haunt, 2,518 of 8,724
// enemy HP gauges that had passed every authored depth rule, with the enemy
// still 72-78 px from the edge of a 640-wide display.
//
// THE ARCHITECTURE HERE
//
//     world position
//       -> the canonical native camera (gekko/camera.h) - the SAME one that
//          drives the GPU projection and world visibility
//       -> canonical clip / NDC
//       -> the content rectangle
//       -> final presentation pixels
//
// There is no guest canvas in that chain, so there is nothing to repair
// afterwards. Crucially, because the camera is ALREADY the widened one, the
// visible region is simply |ndc| <= 1: there is no separate coverage formula
// for world-anchored UI at all, and no possibility of a second Hor+.
//
// WHAT IS PRESERVED
//
// The authored game rules, because they are intent rather than implementation:
// per-consumer minimum and maximum depth, the camera near/far behaviour, the
// perspective scale law scale = base_scale / depth, and a consumer's own
// screen margin. A consumer owns its margin; this layer owns camera coverage.
//
// WHAT IS NOT
//
// The 640x480 clip rectangle, the camera-mode table as camera truth, the
// duplicated world-to-screen projections, the separate 4:3 coverage
// calculations, and the host-side repair of an already-projected anchor.

#include "gekko/camera.h"
#include "gekko/types.h"

namespace gekko::world_anchor {

// Where the displayed image actually lives, in output pixels. This is the
// presentation rectangle the content is fitted into - not the guest raster and
// not the window. Positions come out directly in this space.
struct ContentViewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 640.0f;
    float height = 480.0f;

    constexpr bool valid() const noexcept { return width > 0.0f && height > 0.0f; }
};

// What a consumer authors. Everything optional is disabled at zero, so a
// default-constructed request is "project it, reject it only if it leaves the
// camera".
struct Request {
    float world_x = 0.0f, world_y = 0.0f, world_z = 0.0f;

    // Authored visibility distances, in world units along the view axis.
    // 0 = not authored. These are GAME RULES and are applied exactly.
    float min_depth = 0.0f;
    float max_depth = 0.0f;

    // Apply the camera's own near/far planes as well. The gauge does; a
    // consumer that only has authored limits does not have to.
    bool use_camera_near_far = true;

    // scale = base_scale / depth. 0 = no scale computed.
    float base_scale = 0.0f;
    // A further authored multiplier on the computed scale, for consumers that
    // carry one (the gauge multiplies by a ratio of two authored rectangles).
    float scale_multiplier = 1.0f;

    // The consumer's OWN screen margin, in presentation pixels, applied on top
    // of canonical coverage. LockonMark authors 32 px. Positive widens the
    // accepted region; negative tightens it. This layer never invents one.
    float screen_margin = 0.0f;

    // THE ELEMENT'S OWN PROJECTED EXTENT around the anchor, in the same units
    // as `base_scale` - so the computed scale converts it to pixels exactly the
    // way it converts the element's geometry. With no scale law the units ARE
    // pixels.
    //
    // Why this exists: an anchor is a POINT, and a point test culls the element
    // the moment its CENTRE leaves the screen - with up to half the element
    // still drawn. The original had this defect too (it tested the projected
    // anchor X against [0, 640], also a point), and it is the residual
    // "pops out early" that survives fixing the camera cone.
    //
    // Asymmetric on purpose. Real elements are not centred on their anchors:
    // the enemy health bar spans -24..+26 in its own units, so a symmetric
    // extent would pop early on one side and late on the other.
    //
    // All zero is a point, and the test then degenerates EXACTLY to the
    // anchor-point test - so a consumer that has not been given a footprint
    // behaves precisely as before.
    float extent_left = 0.0f;
    float extent_right = 0.0f;
    float extent_up = 0.0f;
    float extent_down = 0.0f;
};

enum class Rejection : u32 {
    None = 0,
    BehindCamera,   // at or behind the eye; no projection is meaningful
    CameraNear,
    CameraFar,
    AuthoredMinDepth,
    AuthoredMaxDepth,
    OffScreenLeft,
    OffScreenRight,
    OffScreenBottom,
    OffScreenTop,
};

const char* rejection_name(Rejection r) noexcept;

struct Result {
    bool visible = false;
    Rejection rejection = Rejection::None;

    // Canonical NDC from the native camera. |x| <= 1 and |y| <= 1 is exactly
    // the displayed screen, because the camera is the displayed camera.
    float ndc_x = 0.0f;
    float ndc_y = 0.0f;

    // View-space distance along the view axis. Positive in front of the eye.
    // This is the quantity every authored depth rule is expressed in.
    float depth = 0.0f;

    // Final presentation pixels, inside the content rectangle. Already correct:
    // nothing downstream may apply a widescreen correction to these.
    float pixel_x = 0.0f;
    float pixel_y = 0.0f;

    // base_scale / depth, times any authored multiplier. 0 when not requested.
    float scale = 0.0f;

    // How far the anchor is from the nearest horizontal screen edge, in
    // presentation pixels. Negative when off screen. Consumers that fade near
    // an edge use this instead of re-deriving it.
    float edge_margin_x = 0.0f;
    float edge_margin_y = 0.0f;
};

// The service. One instance per camera; update once per frame, project many
// times. No allocation, no globals, no device.
class Projector {
public:
    // The canonical camera and the rectangle its image is presented into.
    void update(const camera::Camera& camera, const ContentViewport& viewport) noexcept;

    const camera::Camera& camera() const noexcept { return camera_; }
    const ContentViewport& viewport() const noexcept { return viewport_; }

    Result project(const Request& request) const noexcept;

    // Diagnostics, so a disappearing element can always name its cause.
    struct Stats {
        u64 projected = 0;
        u64 visible = 0;
        u64 rejected_behind = 0;
        u64 rejected_camera_near = 0;
        u64 rejected_camera_far = 0;
        u64 rejected_authored_min = 0;
        u64 rejected_authored_max = 0;
        u64 rejected_off_screen = 0;
        void reset() noexcept { *this = Stats{}; }
        void record(const Result& r) noexcept;
    };

private:
    camera::Camera camera_{};
    camera::Projection projection_{};
    ContentViewport viewport_{};
};

}  // namespace gekko::world_anchor
