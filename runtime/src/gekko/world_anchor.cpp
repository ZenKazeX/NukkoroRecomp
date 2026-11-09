#include "gekko/world_anchor.h"

#include <cmath>

namespace gekko::world_anchor {
namespace {

constexpr float kEpsilon = 1.0e-6f;

bool finite(float v) noexcept { return std::isfinite(v); }

}  // namespace

const char* rejection_name(Rejection r) noexcept {
    switch (r) {
        case Rejection::None: return "visible";
        case Rejection::BehindCamera: return "behind camera";
        case Rejection::CameraNear: return "camera near";
        case Rejection::CameraFar: return "camera far";
        case Rejection::AuthoredMinDepth: return "authored min depth";
        case Rejection::AuthoredMaxDepth: return "authored max depth";
        case Rejection::OffScreenLeft: return "off screen left";
        case Rejection::OffScreenRight: return "off screen right";
        case Rejection::OffScreenBottom: return "off screen bottom";
        case Rejection::OffScreenTop: return "off screen top";
    }
    return "?";
}

void Projector::update(const camera::Camera& camera, const ContentViewport& viewport) noexcept {
    camera_ = camera;
    // The SAME encoding the GPU receives. World-anchored UI and world geometry
    // are projected by one matrix, so they cannot disagree about where a point
    // on screen is.
    projection_ = camera::projection_matrix(camera);
    viewport_ = viewport.valid() ? viewport : ContentViewport{};
}

Result Projector::project(const Request& request) const noexcept {
    Result result;
    if (!finite(request.world_x) || !finite(request.world_y) || !finite(request.world_z)) {
        result.rejection = Rejection::BehindCamera;
        return result;
    }

    // ---- world -> view -------------------------------------------------
    // The basis columns are the world directions of view +x/+y/+z, and the
    // transform is rigid, so the inverse is a transpose: view coordinates are
    // the dot products with those columns.
    const camera::Transform& t = camera_.view_to_world;
    const float dx = request.world_x - t.origin_x;
    const float dy = request.world_y - t.origin_y;
    const float dz = request.world_z - t.origin_z;
    const std::array<float, 9>& b = t.basis;
    const float view_x = b[0] * dx + b[3] * dy + b[6] * dz;
    const float view_y = b[1] * dx + b[4] * dy + b[7] * dz;
    const float view_z = b[2] * dx + b[5] * dy + b[8] * dz;

    // The camera looks down -z, so a point in front has view_z < 0 and its
    // depth - the quantity every authored rule is written in - is -view_z.
    const float depth = -view_z;
    result.depth = depth;
    if (!(depth > kEpsilon)) {
        result.rejection = Rejection::BehindCamera;
        return result;
    }

    // ---- depth rules, in the order the original applies them ------------
    // Camera first, then authored. These are GAME BEHAVIOUR and are applied
    // exactly as recovered; nothing here widens or disables them.
    if (request.use_camera_near_far) {
        if (depth < camera_.near_plane) {
            result.rejection = Rejection::CameraNear;
            return result;
        }
        if (depth > camera_.far_plane) {
            result.rejection = Rejection::CameraFar;
            return result;
        }
    }
    if (request.min_depth > 0.0f && depth < request.min_depth) {
        result.rejection = Rejection::AuthoredMinDepth;
        return result;
    }
    if (request.max_depth > 0.0f && depth > request.max_depth) {
        result.rejection = Rejection::AuthoredMaxDepth;
        return result;
    }

    // ---- view -> canonical clip / NDC -----------------------------------
    // Straight through the canonical projection. w_clip = depth for the
    // perspective form, so the divide is by depth.
    const float clip_x = projection_.m00 * view_x + projection_.m02 * view_z;
    const float clip_y = projection_.m11 * view_y + projection_.m12 * view_z;
    result.ndc_x = clip_x / depth;
    result.ndc_y = clip_y / depth;

    // ---- NDC -> presentation pixels -------------------------------------
    // NDC y is up, pixels are down.
    result.pixel_x = viewport_.x + (result.ndc_x * 0.5f + 0.5f) * viewport_.width;
    result.pixel_y = viewport_.y + (0.5f - result.ndc_y * 0.5f) * viewport_.height;

    // ---- authored perspective scale --------------------------------------
    // scale = base_scale / depth, exactly the original law, times whatever
    // authored multiplier the consumer carries. Computed BEFORE the screen
    // test, because it is what converts the element's own extent into pixels.
    if (request.base_scale != 0.0f) {
        result.scale = (request.base_scale / depth) * request.scale_multiplier;
    }

    // ---- coverage --------------------------------------------------------
    // |ndc| <= 1 IS the displayed screen, because this is the displayed
    // camera. There is deliberately no 4:3 term, no canvas width and no
    // coverage factor anywhere in this test - the widening is already in the
    // camera the projection came from, so applying one here would be the
    // second correction this architecture exists to eliminate.
    //
    // The test is an INTERSECTION of the element's projected box with the
    // viewport, not a containment test on its anchor. An anchor is a point, and
    // a point test culls an element the moment its CENTRE leaves the screen,
    // with up to half of it still drawn.
    //
    // NO TOLERANCE IS ADDED, deliberately. The comparisons are inclusive, so an
    // element whose edge exactly touches the viewport edge is kept; anything
    // beyond that covers no pixel at all, and admitting it would be precisely
    // the arbitrary margin this design exists to avoid. The quantities are
    // continuous and the comparison is well conditioned, so there is no
    // floating-point instability for an epsilon to absorb - the only thing that
    // moves an element across this boundary is the camera actually moving.
    const float unit = (result.scale != 0.0f) ? result.scale : 1.0f;
    const float box_left = result.pixel_x - request.extent_left * unit;
    const float box_right = result.pixel_x + request.extent_right * unit;
    const float box_top = result.pixel_y - request.extent_up * unit;
    const float box_bottom = result.pixel_y + request.extent_down * unit;

    const float view_left = viewport_.x - request.screen_margin;
    const float view_right = viewport_.x + viewport_.width + request.screen_margin;
    const float view_top = viewport_.y - request.screen_margin;
    const float view_bottom = viewport_.y + viewport_.height + request.screen_margin;

    // How far the element's NEAREST edge still is from leaving, in presentation
    // pixels; negative once it has left. Consumers that fade near an edge use
    // this instead of re-deriving it, and it is now about the element rather
    // than about its anchor.
    result.edge_margin_x = std::fmin(box_right - view_left, view_right - box_left);
    result.edge_margin_y = std::fmin(box_bottom - view_top, view_bottom - box_top);

    if (box_right < view_left) {
        result.rejection = Rejection::OffScreenLeft;
        return result;
    }
    if (box_left > view_right) {
        result.rejection = Rejection::OffScreenRight;
        return result;
    }
    // Vertical is untouched by any widescreen policy and is tested the same way
    // at every aspect. Pixel y increases downward, so the TOP edge is the small
    // coordinate.
    if (box_bottom < view_top) {
        result.rejection = Rejection::OffScreenTop;
        return result;
    }
    if (box_top > view_bottom) {
        result.rejection = Rejection::OffScreenBottom;
        return result;
    }

    result.visible = true;
    result.rejection = Rejection::None;
    return result;
}

void Projector::Stats::record(const Result& r) noexcept {
    ++projected;
    if (r.visible) {
        ++visible;
        return;
    }
    switch (r.rejection) {
        case Rejection::BehindCamera: ++rejected_behind; break;
        case Rejection::CameraNear: ++rejected_camera_near; break;
        case Rejection::CameraFar: ++rejected_camera_far; break;
        case Rejection::AuthoredMinDepth: ++rejected_authored_min; break;
        case Rejection::AuthoredMaxDepth: ++rejected_authored_max; break;
        case Rejection::OffScreenLeft:
        case Rejection::OffScreenRight:
        case Rejection::OffScreenBottom:
        case Rejection::OffScreenTop: ++rejected_off_screen; break;
        case Rejection::None: break;
    }
}

}  // namespace gekko::world_anchor
