// Tests for the rebuilt enemy health bar layer.
//
// What this suite defends, in one sentence each:
//
//   * the authored rules (20/200 depth window, base_scale/depth, the width law,
//     the instantaneous fill) survive the rewrite EXACTLY, and are identical at
//     every display aspect;
//   * the screen-visibility test contains no aspect, coverage or canvas term,
//     so a second Hor+ cannot be introduced;
//   * a camera that was not built is REFUSED rather than used - the defect that
//     shipped, where a default-constructed camera turned depth into -world_z
//     and silently stopped every bar from drawing.
//
// The last one is why every camera in this file is deliberately NON-IDENTITY,
// with a rotated basis and an origin far from the world origin. A suite built
// on Transform{} cannot tell a correct camera from a default-constructed one,
// because both compute depth = -world_z.

#include "gekko/health_bar.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

void check_near(double actual, double expected, double tolerance, const std::string& what) {
    if (!(std::fabs(actual - expected) <= tolerance)) {
        std::printf("FAIL: %s (got %.6f, expected %.6f +/- %g)\n", what.c_str(), actual, expected,
                    tolerance);
        ++g_failures;
    }
}

using namespace gekko;
using namespace gekko::health_bar;

// The original's live coverage: hFOV 63.2501 deg, and the vertical it pairs with.
constexpr float kGuestTanX = 0.615808f;
constexpr float kGuestTanY = 0.542110f;

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalise(const Vec3& v) {
    const float len = std::sqrt(dot(v, v));
    return Vec3{v.x / len, v.y / len, v.z / len};
}

// A canonical view-to-world transform for a camera at `eye` looking at `target`.
// Columns are the world directions of view +x, +y, +z, and the canonical camera
// looks down -z, so column 2 is BACKWARD.
camera::Transform look_at(const Vec3& eye, const Vec3& target, const Vec3& world_up) {
    const Vec3 forward = normalise(sub(target, eye));
    const Vec3 right = normalise(cross(forward, world_up));
    const Vec3 up = cross(right, forward);
    camera::Transform t;
    t.basis = {right.x, up.x, -forward.x, right.y, up.y, -forward.y, right.z, up.z, -forward.z};
    t.origin_x = eye.x;
    t.origin_y = eye.y;
    t.origin_z = eye.z;
    return t;
}

// Deliberately awkward: an eye thousands of units from the world origin, on a
// rotated, non-axis-aligned basis. Every fixture below is expressed relative to
// it, so nothing in this suite can pass by accident on an identity camera.
const Vec3 kEye{1234.5f, -567.25f, -3410.75f};
const Vec3 kTarget{1100.0f, -520.0f, -3900.0f};
const Vec3 kWorldUp{0.0f, 1.0f, 0.0f};

camera::Transform view_transform() { return look_at(kEye, kTarget, kWorldUp); }

Vec3 camera_forward() { return normalise(sub(kTarget, kEye)); }
Vec3 camera_right() { return normalise(cross(camera_forward(), kWorldUp)); }
Vec3 camera_up() { return cross(camera_right(), camera_forward()); }

camera::Camera camera_at(float aspect) {
    const camera::Camera decoded = camera::decode::renderware_camera(
        camera::decode::ViewWindow{kGuestTanX, kGuestTanY}, 1.0f, 20000.13f,
        camera::ProjectionType::Perspective, view_transform());
    camera::CameraPolicy policy;
    policy.target_aspect = aspect;
    return camera::apply_policy(decoded, policy);
}

// A second fixture for the tests that must land on an authored bound EXACTLY.
// The basis is a quarter turn - genuinely non-identity, so it still catches a
// default-constructed camera - but every component is 0 or +/-1 and the eye is
// a power of two, so `eye + forward * d` is exact in binary32 and a test of an
// inclusive bound is testing the bound and not the rounding.
const Vec3 kExactEye{1024.0f, -512.0f, -2048.0f};
const Vec3 kExactForward{-1.0f, 0.0f, 0.0f};
const Vec3 kExactRight{0.0f, 0.0f, -1.0f};
const Vec3 kExactUp{0.0f, 1.0f, 0.0f};

camera::Camera exact_camera_at(float aspect) {
    camera::Transform t;
    t.basis = {kExactRight.x, kExactUp.x, -kExactForward.x,
               kExactRight.y, kExactUp.y, -kExactForward.y,
               kExactRight.z, kExactUp.z, -kExactForward.z};
    t.origin_x = kExactEye.x;
    t.origin_y = kExactEye.y;
    t.origin_z = kExactEye.z;
    const camera::Camera decoded = camera::decode::renderware_camera(
        camera::decode::ViewWindow{kGuestTanX, kGuestTanY}, 1.0f, 20000.13f,
        camera::ProjectionType::Perspective, t);
    camera::CameraPolicy policy;
    policy.target_aspect = aspect;
    return camera::apply_policy(decoded, policy);
}

Vec3 exact_point(float depth, float right_offset = 0.0f, float up_offset = 0.0f) {
    return Vec3{kExactEye.x + kExactForward.x * depth + kExactRight.x * right_offset +
                    kExactUp.x * up_offset,
                kExactEye.y + kExactForward.y * depth + kExactRight.y * right_offset +
                    kExactUp.y * up_offset,
                kExactEye.z + kExactForward.z * depth + kExactRight.z * right_offset +
                    kExactUp.z * up_offset};
}

world_anchor::ContentViewport screen(float w = 640.0f, float h = 480.0f) {
    return world_anchor::ContentViewport{0.0f, 0.0f, w, h};
}

// A world point at an exact view-axis depth and an exact offset across the view
// plane. Built from the camera basis, so "depth" here means exactly what every
// authored rule means by it.
Vec3 world_point(float depth, float right_offset, float up_offset) {
    const Vec3 f = camera_forward(), r = camera_right(), u = camera_up();
    return Vec3{kEye.x + f.x * depth + r.x * right_offset + u.x * up_offset,
                kEye.y + f.y * depth + r.y * right_offset + u.y * up_offset,
                kEye.z + f.z * depth + r.z * right_offset + u.z * up_offset};
}

const Composition& composition() {
    static const Composition c = Composition::authored();
    return c;
}

Gauge gauge_at(const Vec3& world, float rate = 1.0f, float max_hp = 4.0f) {
    Gauge g;
    g.signal.world_x = world.x;
    g.signal.world_y = world.y;
    g.signal.world_z = world.z;
    g.signal.rate = rate;
    g.signal.max_hp = max_hp;
    g.signal.enabled = true;
    return g;
}

Layer layer_at(float aspect, bool hud_visible = true, float vw = 640.0f, float vh = 480.0f) {
    Layer layer;
    const bool ok = layer.begin_frame(camera_at(aspect), screen(vw, vh), hud_visible);
    check(ok, "begin_frame accepts a well-formed camera");
    return layer;
}

// ---------------------------------------------------------------------------

// THE SHIPPED DEFECT. A default-constructed camera has an identity basis and a
// zero origin, so `depth` collapses to -world_z: on Final Haunt that is ~3,400
// where the true axial depth is ~195, every gauge fails its authored 200-unit
// rule, and not one bar draws with no error reported anywhere. The identity
// basis is a legal rotation, so only the extents can catch it.
void test_an_unbuilt_camera_is_refused() {
    Layer layer;
    check(!layer.begin_frame(camera::Camera{}, screen(), true),
          "a default-constructed camera is refused");
    check(!layer.ready(), "the layer does not become ready on a refused camera");
    check(layer.stats().camera_refused == 1, "the refusal is counted");

    // And nothing projects through it.
    const Placement p = layer.place(gauge_at(world_point(100.0f, 0.0f, 0.0f)), composition());
    check(!p.visible, "no gauge is placed without a camera");
    check(p.rejection == Rejection::NotAuthored, "the reason names the missing camera state");

    // A basis that is not a rotation is refused too: a scaled basis still
    // projects, but its depth is no longer in world units and every authored
    // rule is then applied to the wrong quantity.
    camera::Camera scaled = camera_at(4.0f / 3.0f);
    for (float& v : scaled.view_to_world.basis) v *= 2.0f;
    Layer other;
    check(!other.begin_frame(scaled, screen(), true), "a scaled (non-rotation) basis is refused");
}

// Depth is the view-axis distance, never a Euclidean range. The two differ by
// 1/cos(theta) - about 19% at the edge of a 78.8 degree frustum - and that
// difference decides which enemies show a bar.
void test_depth_is_view_axis_not_range() {
    Layer layer = layer_at(16.0f / 9.0f);
    // 120 units along the view axis, but pushed 90 units across it: the range
    // is 150, the depth is 120.
    const Vec3 world = world_point(120.0f, 90.0f, 0.0f);
    const Placement p = layer.place(gauge_at(world), composition());
    check_near(p.depth, 120.0, 1.0e-2, "depth is the view-axis distance");
    const Vec3 d = sub(world, kEye);
    check_near(std::sqrt(dot(d, d)), 150.0, 1.0e-2, "the Euclidean range really is 150");
    check(p.visible, "a point inside the authored window and on screen is visible");
}

// The authored window is game behaviour and must reject at exactly the same
// depths at every aspect. If these counts move, an authored rule moved.
void test_authored_window_is_exact_and_aspect_independent() {
    const float aspects[] = {4.0f / 3.0f, 16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f};
    for (float aspect : aspects) {
        Layer layer;
        check(layer.begin_frame(exact_camera_at(aspect), screen(), true), "camera accepted");
        const std::string at = " at aspect " + std::to_string(aspect);

        check(layer.place(gauge_at(exact_point(19.9f)), composition()).rejection == Rejection::AuthoredMinDepth,
              "just inside the authored minimum is rejected" + at);
        // Inclusive: every one of the original's eight branches is
        // bge/ble/bgt/blt, so equality is ACCEPTED. This is the edge the
        // previous suite never pinned.
        check(layer.place(gauge_at(exact_point(20.0f)), composition()).visible,
              "exactly the authored minimum is ACCEPTED" + at);
        check(layer.place(gauge_at(exact_point(200.0f)), composition()).visible,
              "exactly the authored maximum is ACCEPTED" + at);
        check(layer.place(gauge_at(exact_point(200.1f)), composition()).rejection == Rejection::AuthoredMaxDepth,
              "just outside the authored maximum is rejected" + at);

        // The depth at which each rule fires is bit-identical across aspects:
        // that is what "an authored rule did not move" means.
        check_near(layer.place(gauge_at(exact_point(137.5f)), composition()).depth, 137.5, 0.0,
                   "depth is exact and aspect-independent" + at);
    }
}

// The limits are PER INSTANCE. Baking the constructor defaults in clips the
// final boss's gauges 300 units early and draws them at half size.
void test_authored_limits_are_per_instance() {
    Layer layer = layer_at(16.0f / 9.0f);

    Gauge devil = gauge_at(world_point(400.0f, 0.0f, 0.0f), 1.0f, 32.0f);
    devil.authored.max_depth = 500.0f;   // DevilDoomCtrlUnit_WingControl
    devil.authored.base_scale = 100.0f;
    check(layer.place(devil, composition()).visible, "a 500-unit gauge is visible at depth 400");

    Gauge ordinary = gauge_at(world_point(400.0f, 0.0f, 0.0f));
    check(layer.place(ordinary, composition()).rejection == Rejection::AuthoredMaxDepth,
          "the same depth is rejected for a default 200-unit gauge");

    check_near(layer.place(devil, composition()).scale, 100.0 / 400.0, 1.0e-6,
               "the per-instance base scale is used");
}

void test_scale_law_is_the_raw_quotient() {
    Layer layer;
    check(layer.begin_frame(exact_camera_at(16.0f / 9.0f), screen(), true), "camera accepted");
    check_near(layer.place(gauge_at(exact_point(20.0f)), composition()).scale, 2.5, 0.0,
               "scale at the near end of the authored window");
    check_near(layer.place(gauge_at(exact_point(200.0f)), composition()).scale, 0.25, 0.0,
               "scale at the far end of the authored window");
    check_near(layer.place(gauge_at(exact_point(50.0f)), composition()).scale, 1.0, 0.0,
               "scale is base_scale / depth exactly, with no clamp or easing");
}

// No time term, no interpolation, no lag bar, no damage trail. The original
// writes the same value to the current AND the target animation slot, which
// collapses the runtime's own tween; the behaviour is a seek.
void test_fill_is_instantaneous_and_clamped() {
    check_near(fill_from_rate(1.0f), 1.0, 0.0, "full");
    check_near(fill_from_rate(0.5f), 0.5, 0.0, "half");
    check_near(fill_from_rate(0.0f), 0.0, 0.0, "empty");
    // The original never clamps here; only the enemy's heal routine keeps the
    // value in range. Clamping cannot change any in-range outcome.
    check_near(fill_from_rate(-0.25f), 0.0, 0.0, "below empty clamps to empty");
    check_near(fill_from_rate(1.75f), 1.0, 0.0, "above full clamps to full");

    // Placing the same gauge twice with the same rate gives the same fill:
    // there is no state carried between frames.
    Layer layer = layer_at(16.0f / 9.0f);
    const Gauge g = gauge_at(world_point(100.0f, 0.0f, 0.0f), 0.375f);
    check_near(layer.place(g, composition()).fill, 0.375, 1.0e-6, "fill follows rate directly");
    check_near(layer.place(g, composition()).fill, 0.375, 1.0e-6, "and does not drift on a second frame");
}

// Four HP is the authored reference: a 4 HP enemy must be the authored art.
void test_width_law() {
    check_near(width_from_max_hp(4.0f), 1.0, 0.0, "4 HP is the authored reference width");
    check_near(width_from_max_hp(8.0f), 2.0, 0.0, "8 HP is twice as long");
    check_near(width_from_max_hp(2.0f), 0.5, 0.0, "2 HP is half as long");
    // The unset sentinel selects the authored reference, not a zero-width bar.
    check_near(width_from_max_hp(-1.0f), 1.0, 0.0, "the unset sentinel is the reference width");
}

// The whole point of the rewrite: the visibility test is |ndc| <= 1 against the
// displayed camera, so widening the display admits MORE, and the vertical rules
// do not move at all.
void test_widening_admits_more_horizontally_and_nothing_vertically() {
    // A point outside the original's 63.25 deg cone horizontally.
    const float outside_x = kGuestTanX * 100.0f * 1.15f;
    const Vec3 world = world_point(100.0f, outside_x, 0.0f);

    Layer narrow = layer_at(4.0f / 3.0f);
    Layer wide = layer_at(16.0f / 9.0f);
    check(!narrow.place(gauge_at(world), composition()).visible, "outside the 4:3 cone it is rejected");
    check(narrow.place(gauge_at(world), composition()).rejection == Rejection::OffScreenRight,
          "and the reason names the correct edge");
    check(wide.place(gauge_at(world), composition()).visible, "the widened camera admits it");

    // Vertical is untouched by any widescreen policy.
    const float outside_y = kGuestTanY * 100.0f * 1.05f;
    for (float aspect : {4.0f / 3.0f, 16.0f / 9.0f, 32.0f / 9.0f}) {
        Layer layer = layer_at(aspect);
        const Placement above = layer.place(gauge_at(world_point(100.0f, 0.0f, outside_y)), composition());
        const Placement below = layer.place(gauge_at(world_point(100.0f, 0.0f, -outside_y)), composition());
        check(above.rejection == Rejection::OffScreenTop,
              "a point above the frustum reports a TOP reason at every aspect");
        check(below.rejection == Rejection::OffScreenBottom,
              "a point below the frustum reports a BOTTOM reason at every aspect");
        const Placement inside = layer.place(gauge_at(world_point(100.0f, 0.0f, outside_y * 0.9f)), composition());
        check(inside.visible, "the vertical boundary itself does not move with aspect");
    }
}

// The two original gates, in order, before anything is projected.
void test_gate_order() {
    const Vec3 world = world_point(100.0f, 0.0f, 0.0f);

    Layer hidden = layer_at(16.0f / 9.0f, /*hud_visible=*/false);
    check(hidden.place(gauge_at(world), composition()).rejection == Rejection::HudHidden,
          "the global HUD flag is checked first");

    Layer layer = layer_at(16.0f / 9.0f);
    Gauge off = gauge_at(world);
    off.signal.enabled = false;
    check(layer.place(off, composition()).rejection == Rejection::Disabled,
          "the per-gauge enable is checked before any projection");

    // A hidden HUD wins over a disabled gauge: the order is observable.
    Layer both = layer_at(16.0f / 9.0f, /*hud_visible=*/false);
    check(both.place(off, composition()).rejection == Rejection::HudHidden, "the HUD flag takes precedence");
}

// The original accepts a NaN anchor through all eight of its predicates,
// because every branch is bge/ble/bgt/blt and fcmpo leaves CR0 unordered.
void test_non_finite_anchor_is_refused() {
    Layer layer = layer_at(16.0f / 9.0f);
    Gauge g = gauge_at(world_point(100.0f, 0.0f, 0.0f));
    g.signal.world_y = std::nanf("");
    check(layer.place(g, composition()).rejection == Rejection::InvalidAnchor, "a NaN anchor is refused");
    Gauge inf = gauge_at(world_point(100.0f, 0.0f, 0.0f));
    inf.signal.world_z = HUGE_VALF;
    check(layer.place(inf, composition()).rejection == Rejection::InvalidAnchor, "an infinite anchor is refused");
}

// Rejection is total: no submission AND no animation step.
void test_rejection_is_total() {
    Layer layer = layer_at(16.0f / 9.0f);
    const Composition& parts = composition();
    Vertex vertices[kMaxVerticesPerGauge]{};

    const Placement rejected = layer.place(gauge_at(world_point(1000.0f, 0.0f, 0.0f)), composition());
    check(!rejected.visible, "a gauge beyond the authored window is not visible");
    check(layer.build(rejected, parts, vertices, kMaxVerticesPerGauge) == 0,
          "a rejected gauge submits nothing");
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

void test_geometry_follows_the_authored_laws() {
    Layer layer = layer_at(16.0f / 9.0f);
    const Composition& parts = composition();
    check(parts.count > 0, "the authored composition is populated");
    check(parts.reference_height > 0.0f, "the composition owns its authoring reference");

    Vertex full[kMaxVerticesPerGauge]{};
    Vertex half[kMaxVerticesPerGauge]{};
    Vertex wide[kMaxVerticesPerGauge]{};

    const Vec3 world = world_point(100.0f, 0.0f, 0.0f);
    const Placement p_full = layer.place(gauge_at(world, 1.0f, 4.0f), composition());
    const Placement p_half = layer.place(gauge_at(world, 0.5f, 4.0f), composition());
    const Placement p_wide = layer.place(gauge_at(world, 1.0f, 8.0f), composition());

    const u32 n_full = layer.build(p_full, parts, full, kMaxVerticesPerGauge);
    const u32 n_half = layer.build(p_half, parts, half, kMaxVerticesPerGauge);
    const u32 n_wide = layer.build(p_wide, parts, wide, kMaxVerticesPerGauge);

    check(n_full == parts.count * kVerticesPerQuad, "one quad per authored part");
    check(n_half == n_full, "a half-full bar still draws every part");
    check(n_wide == n_full, "a wider bar still draws every part");

    // Per-quad, because the authored law is per-part: the BODY widens, the end
    // caps are REPOSITIONED AND NOT SCALED. Asserting on the bar's overall
    // span would hide exactly that distinction - the caps keep their 2-unit
    // width, so the whole bar grows by 1.94x when the body grows by 2x.
    auto quad_left = [](const Vertex* v, u32 part) { return v[part * 4].x; };
    auto quad_right = [](const Vertex* v, u32 part) { return v[part * 4 + 1].x; };
    auto quad_width = [&](const Vertex* v, u32 part) {
        return quad_right(v, part) - quad_left(v, part);
    };
    auto quad_centre = [&](const Vertex* v, u32 part) {
        return 0.5f * (quad_left(v, part) + quad_right(v, part));
    };
    // Submission order is the asset's FLAT cast array, not the tree's DFS:
    // 0 shadow, 1 side shadow, 2 right cap, 3 body, 4 backing plate, 5 meter,
    // 6 left cap.
    constexpr u32 kRightCap = 2, kBody = 3, kMeter = 5, kLeftCap = 6;

    check_near(quad_width(wide, kBody) / quad_width(full, kBody), 2.0, 1.0e-4,
               "the body's width tracks the width law exactly");
    check_near(quad_width(wide, kRightCap), quad_width(full, kRightCap), 1.0e-4,
               "the right cap is REPOSITIONED, not scaled");
    check_near(quad_width(wide, kLeftCap), quad_width(full, kLeftCap), 1.0e-4,
               "the left cap is REPOSITIONED, not scaled");
    check_near(quad_centre(full, kBody), p_full.pixel_x, 1.0e-3,
               "the body stays centred on the anchor");
    check_near(quad_centre(wide, kBody), p_wide.pixel_x, 1.0e-3,
               "and stays centred as it lengthens");
    // The ANCHOR travels with the width law - the cap's inner edge sits at
    // 22*width - while the cap's own 2-unit rect is untouched. Testing the
    // centre instead would conflate the two and pass on a wrong model.
    const float inner_full = (quad_left(full, kRightCap) - p_full.pixel_x);
    const float inner_wide = (quad_left(wide, kRightCap) - p_wide.pixel_x);
    check_near(inner_wide / inner_full, 2.0, 1.0e-4,
               "the cap's anchor travels outward with the width law");
    check_near(quad_right(full, kBody) - p_full.pixel_x, inner_full, 1.0e-3,
               "the cap sits exactly at the body's outer edge");

    // Draining is anchored at the meter's own LEFT edge, which is what makes
    // the bar empty right-to-left.
    const float meter_left_full = full[kMeter * 4].x;
    const float meter_left_half = half[kMeter * 4].x;
    check_near(meter_left_half, meter_left_full, 1.0e-4,
               "draining does not move the meter's left edge");
    check_near(quad_width(half, kMeter) / quad_width(full, kMeter), 0.5, 1.0e-4,
               "a half-full meter is half as long");

    // A fully drained meter is a zero-area quad and is not submitted.
    Vertex empty[kMaxVerticesPerGauge]{};
    const Placement p_empty = layer.place(gauge_at(world, 0.0f, 4.0f), composition());
    const u32 n_empty = layer.build(p_empty, parts, empty, kMaxVerticesPerGauge);
    check(n_empty == n_full - kVerticesPerQuad, "a fully drained meter is not submitted");
}

// Positions are final presentation pixels, in whatever rectangle the layer was
// given. There is no 640x480 anywhere in this layer.
void test_no_canvas_assumption() {
    const Composition& parts = composition();
    const Vec3 centre = world_point(100.0f, 0.0f, 0.0f);

    Layer small = layer_at(16.0f / 9.0f, true, 640.0f, 480.0f);
    Layer large = layer_at(16.0f / 9.0f, true, 2560.0f, 1920.0f);

    const Placement a = small.place(gauge_at(centre), composition());
    const Placement b = large.place(gauge_at(centre), composition());
    check_near(a.pixel_x, 320.0, 1.0e-3, "a centred anchor lands at the centre of a 640 viewport");
    check_near(b.pixel_x, 1280.0, 1.0e-2, "and at the centre of a 2560 viewport");
    check_near(a.depth, b.depth, 1.0e-4, "the viewport does not affect depth");
    check_near(a.scale, b.scale, 1.0e-6, "the viewport does not affect the authored scale law");

    // The drawn bar scales with the viewport - the original's split-screen
    // behaviour - and does so on the VERTICAL extent, so aspect cannot reach it.
    Vertex va[kMaxVerticesPerGauge]{};
    Vertex vb[kMaxVerticesPerGauge]{};
    const u32 na = small.build(a, parts, va, kMaxVerticesPerGauge);
    const u32 nb = large.build(b, parts, vb, kMaxVerticesPerGauge);
    check(na == nb && na > 0, "the same parts are drawn in both viewports");
    auto width_of = [](const Vertex* v, u32 n) {
        float lo = v[0].x, hi = v[0].x;
        for (u32 i = 1; i < n; ++i) {
            lo = v[i].x < lo ? v[i].x : lo;
            hi = v[i].x > hi ? v[i].x : hi;
        }
        return hi - lo;
    };
    check_near(width_of(vb, nb) / width_of(va, na), 4.0, 1.0e-3,
               "the bar scales with the viewport's HEIGHT, not its aspect");

    // A half-height viewport - the split-screen case - halves the bar.
    Layer split = layer_at(16.0f / 9.0f, true, 640.0f, 240.0f);
    Vertex vs[kMaxVerticesPerGauge]{};
    const Placement s = split.place(gauge_at(centre), composition());
    const u32 ns = split.build(s, parts, vs, kMaxVerticesPerGauge);
    check_near(width_of(vs, ns) / width_of(va, na), 0.5, 1.0e-3,
               "the bar shrinks with the viewport in split screen");
}

// A 16:9 window is wider, not taller, so widening must not change the bar's
// SIZE - only which anchors are admitted. If a coverage term leaked into the
// scale, this is where it would show.
void test_widening_does_not_resize_the_bar() {
    const Composition& parts = composition();
    const Vec3 world = world_point(100.0f, 0.0f, 0.0f);
    Vertex narrow[kMaxVerticesPerGauge]{};
    Vertex wide[kMaxVerticesPerGauge]{};

    Layer l43 = layer_at(4.0f / 3.0f);
    Layer l169 = layer_at(16.0f / 9.0f);
    const Placement a = l43.place(gauge_at(world), composition());
    const Placement b = l169.place(gauge_at(world), composition());
    const u32 na = l43.build(a, parts, narrow, kMaxVerticesPerGauge);
    const u32 nb = l169.build(b, parts, wide, kMaxVerticesPerGauge);
    check(na == nb, "the same parts are drawn at both aspects");
    check_near(a.scale, b.scale, 1.0e-6, "the authored scale is aspect-independent");
    for (u32 i = 0; i < na; ++i) {
        check_near(narrow[i].y, wide[i].y, 1.0e-3, "no vertex moves vertically with aspect");
    }
    check_near(a.pixel_y, b.pixel_y, 1.0e-3, "a centred anchor's Y is aspect-independent");
}

// ---------------------------------------------------------------------------
// Edge behaviour: the bar is culled when the BAR leaves, not when its anchor
// does. These are the cases a centre-point test gets wrong.
// ---------------------------------------------------------------------------

// A world point that projects to an exact presentation pixel X on the exact
// camera, at an exact view-axis depth. Inverting the projection is what lets a
// test name a cull threshold in pixels instead of guessing at it.
Vec3 exact_point_at_pixel(float pixel_x, float depth, float aspect) {
    const float tan_x = kGuestTanX * (aspect > 4.0f / 3.0f ? aspect / (4.0f / 3.0f) : 1.0f);
    const float ndc_x = pixel_x / 320.0f - 1.0f;
    return exact_point(depth, ndc_x * tan_x * depth, 0.0f);
}

void test_the_bar_is_culled_when_the_BAR_leaves_not_its_anchor() {
    const float aspect = 16.0f / 9.0f;
    Layer layer;
    check(layer.begin_frame(exact_camera_at(aspect), screen(), true), "camera accepted");
    const Composition::Extent e = composition().extent_at(1.0f);

    // The authored composition, as extents from the anchor. Asymmetric, and
    // that asymmetry is the point: a symmetric margin would be wrong on one
    // side whatever value it took.
    check_near(e.left, 24.0, 1.0e-4, "the bar reaches 24 units left of its anchor");
    check_near(e.right, 26.0, 1.0e-4, "and 26 units right");
    check_near(e.up, 6.0, 1.0e-4, "6 units up");
    check_near(e.down, 9.0, 1.0e-4, "9 units down");

    // THE RELATIONSHIP THAT GETS IMPLEMENTED BACKWARDS. How far the anchor may
    // travel past the RIGHT border is governed by the bar's LEFT extent: it is
    // the bar's left arm that is still hanging on screen. And vice versa.
    //
    // At depth 100 with base_scale 50 and a 480-tall viewport the pixel scale
    // is exactly 0.5, so the bar reaches 12 px left and 13 px right of its
    // anchor.
    const float depth = 100.0f;
    const float scale = 50.0f / depth;  // viewport term is 1.0 at height 480
    const float past_right_border = e.left * scale;   // the LEFT arm holds it on
    const float past_left_border = e.right * scale;   // the RIGHT arm holds it on
    check_near(past_right_border, 12.0, 1.0e-4, "12 px of travel past the right border");
    check_near(past_left_border, 13.0, 1.0e-4, "13 px past the left border");

    // THE CASE A POINT TEST GETS WRONG: the anchor is off screen, the bar is
    // not. A centre test culls every one of these.
    for (float past = 1.0f; past < past_right_border; past += 3.0f) {
        const Placement p =
            layer.place(gauge_at(exact_point_at_pixel(640.0f + past, depth, aspect)), composition());
        check(p.visible, "the bar is kept while its left edge is still on screen (right side)");
    }
    for (float past = 1.0f; past < past_left_border; past += 3.0f) {
        const Placement p =
            layer.place(gauge_at(exact_point_at_pixel(-past, depth, aspect)), composition());
        check(p.visible, "the bar is kept while its right edge is still on screen (left side)");
    }

    // And it IS culled once the whole bar has left - not a pixel later.
    const Placement gone_right =
        layer.place(gauge_at(exact_point_at_pixel(640.0f + past_right_border + 1.0f, depth, aspect)),
                    composition());
    check(gone_right.rejection == Rejection::OffScreenRight,
          "once the bar's left edge passes the right border it is culled");
    const Placement gone_left = layer.place(
        gauge_at(exact_point_at_pixel(-(past_left_border + 1.0f), depth, aspect)), composition());
    check(gone_left.rejection == Rejection::OffScreenLeft,
          "and on the left at that side's own reach - the two differ, as the art does");
}

// The reach is the bar's real size, so a near (large) bar survives further off
// screen than a far (small) one. That is the physically correct behaviour and
// it is what a fixed margin cannot express.
void test_cull_threshold_tracks_the_bar_size() {
    const float aspect = 16.0f / 9.0f;
    Layer layer;
    check(layer.begin_frame(exact_camera_at(aspect), screen(), true), "camera accepted");
    const Composition::Extent e = composition().extent_at(1.0f);

    for (const float depth : {25.0f, 50.0f, 100.0f, 200.0f}) {
        // Past the RIGHT border, the bar's LEFT extent is what keeps it on.
        const float reach = e.left * (50.0f / depth);
        const Placement inside = layer.place(
            gauge_at(exact_point_at_pixel(640.0f + reach - 1.0f, depth, aspect)), composition());
        const Placement outside = layer.place(
            gauge_at(exact_point_at_pixel(640.0f + reach + 1.0f, depth, aspect)), composition());
        check(inside.visible, "just inside the bar's own reach is visible at every depth");
        check(outside.rejection == Rejection::OffScreenRight,
              "just outside it is culled at every depth");
        // A near bar is bigger, so it genuinely survives further out. That is
        // the behaviour no fixed margin can express.
        if (depth < 200.0f) {
            check(reach > e.left * (50.0f / 200.0f),
                  "a nearer (larger) bar reaches further past the border");
        }
    }

    // A longer bar (more max HP) reaches further, so it survives further out.
    const float depth = 50.0f;
    const Composition::Extent wide = composition().extent_at(2.0f);
    check(wide.left > e.left, "8 HP reaches further left of its anchor than 4 HP");
    const float reach_wide = wide.left * (50.0f / depth);
    const Placement p = layer.place(
        gauge_at(exact_point_at_pixel(640.0f + reach_wide - 1.0f, depth, aspect), 1.0f, 8.0f),
        composition());
    check(p.visible, "a longer bar is kept further off screen, because it IS further on screen");
}

// A zero-size element must behave exactly as before: this is what guarantees
// the change cannot alter any consumer that has not been given a footprint.
void test_zero_footprint_is_exactly_the_anchor_test() {
    const float aspect = 16.0f / 9.0f;
    Layer layer;
    check(layer.begin_frame(exact_camera_at(aspect), screen(), true), "camera accepted");
    Composition point{};
    point.count = 1;
    point.reference_height = 480.0f;  // parts[0] is all zeros

    check(layer.place(gauge_at(exact_point_at_pixel(639.0f, 100.0f, aspect)), point).visible,
          "a point element just inside the right border is visible");
    check(layer.place(gauge_at(exact_point_at_pixel(641.0f, 100.0f, aspect)), point).rejection ==
              Rejection::OffScreenRight,
          "and just outside it is culled - the bound is the border itself");
    check(layer.place(gauge_at(exact_point_at_pixel(1.0f, 100.0f, aspect)), point).visible,
          "just inside the left border is visible");
    check(layer.place(gauge_at(exact_point_at_pixel(-1.0f, 100.0f, aspect)), point).rejection ==
              Rejection::OffScreenLeft,
          "just outside the left border is culled");
}

// Vertical must behave the same way, on the bar's own asymmetric reach, and
// must not move with the display aspect.
void test_vertical_edges_use_the_bar_extent_too() {
    const Composition::Extent e = composition().extent_at(1.0f);
    for (const float aspect : {4.0f / 3.0f, 16.0f / 9.0f, 32.0f / 9.0f}) {
        Layer layer;
        check(layer.begin_frame(exact_camera_at(aspect), screen(), true), "camera accepted");
        const float depth = 100.0f;
        const float scale = 50.0f / depth;
        // Place the anchor just above the top border by less than the bar's
        // downward reach: part of the bar hangs into view.
        const float tan_y = kGuestTanY;
        const auto at_pixel_y = [&](float pixel_y) {
            const float ndc_y = 1.0f - pixel_y / 240.0f;
            return exact_point(depth, 0.0f, ndc_y * tan_y * depth);
        };
        check(layer.place(gauge_at(at_pixel_y(-(e.down * scale) + 1.0f)), composition()).visible,
              "a bar hanging down into view from above the top border is kept");
        check(layer.place(gauge_at(at_pixel_y(-(e.down * scale) - 1.0f)), composition())
                      .rejection == Rejection::OffScreenTop,
              "and is culled once even its lowest edge is above the border");
        check(layer.place(gauge_at(at_pixel_y(480.0f + e.up * scale - 1.0f)), composition())
                  .visible,
              "likewise reaching up into view from below the bottom border");
        check(layer.place(gauge_at(at_pixel_y(480.0f + e.up * scale + 1.0f)), composition())
                      .rejection == Rejection::OffScreenBottom,
              "and culled once its highest edge is below it");
    }
}

void test_stats_attribute_every_outcome() {
    Layer layer = layer_at(16.0f / 9.0f);
    layer.record(layer.place(gauge_at(world_point(100.0f, 0.0f, 0.0f)), composition()));
    layer.record(layer.place(gauge_at(world_point(1000.0f, 0.0f, 0.0f)), composition()));
    layer.record(layer.place(gauge_at(world_point(5.0f, 0.0f, 0.0f)), composition()));
    const Layer::Stats& s = layer.stats();
    check(s.considered == 3, "every gauge is counted");
    check(s.drawn == 1, "the visible one is counted");
    check(s.rejected[static_cast<u32>(Rejection::AuthoredMaxDepth)] == 1,
          "the authored maximum is attributed");
    check(s.rejected[static_cast<u32>(Rejection::AuthoredMinDepth)] == 1,
          "the authored minimum is attributed separately");
    u64 total = s.drawn;
    for (u64 n : s.rejected) total += n;
    check(total == s.considered, "no outcome is unattributed");
}

// ---------------------------------------------------------------------------
// The exact camera route that replaces frustum-plane archaeology
// ---------------------------------------------------------------------------

void test_world_to_view_decode_is_exact() {
    const camera::Transform expected = view_transform();
    const Vec3 f = camera_forward(), r = camera_right(), u = camera_up();

    // Build the guest's own form: a row-major 3x4 world-to-view matrix whose
    // rows are (right, up, backward) and whose translation pins the eye.
    const Vec3 back{-f.x, -f.y, -f.z};
    const float m[12] = {r.x,    r.y,    r.z,    -dot(r, kEye),
                         u.x,    u.y,    u.z,    -dot(u, kEye),
                         back.x, back.y, back.z, -dot(back, kEye)};

    camera::Transform decoded;
    check(camera::decode::transform_from_world_to_view(m, decoded),
          "a rotation matrix decodes");
    check_near(decoded.origin_x, expected.origin_x, 1.0e-2, "the eye is recovered (x)");
    check_near(decoded.origin_y, expected.origin_y, 1.0e-2, "the eye is recovered (y)");
    check_near(decoded.origin_z, expected.origin_z, 1.0e-2, "the eye is recovered (z)");
    for (std::size_t i = 0; i < 9; ++i) {
        check_near(decoded.basis[i], expected.basis[i], 1.0e-5, "the basis is recovered");
    }

    // A matrix carrying a scale still projects, but its depth is no longer a
    // world distance - so it is refused rather than silently believed.
    float scaled[12];
    for (int i = 0; i < 12; ++i) scaled[i] = m[i] * 2.0f;
    camera::Transform ignored;
    check(!camera::decode::transform_from_world_to_view(scaled, ignored),
          "a scaled matrix is refused");
    float sheared[12];
    for (int i = 0; i < 12; ++i) sheared[i] = m[i];
    sheared[1] += 0.25f;
    check(!camera::decode::transform_from_world_to_view(sheared, ignored),
          "a sheared matrix is refused");
}

// The decoded transform and the layer must agree about depth, which is the
// quantity every authored rule is written in.
void test_decoded_camera_reproduces_the_authored_rules() {
    const Vec3 f = camera_forward(), r = camera_right(), u = camera_up();
    const Vec3 back{-f.x, -f.y, -f.z};
    const float m[12] = {r.x,    r.y,    r.z,    -dot(r, kEye),
                         u.x,    u.y,    u.z,    -dot(u, kEye),
                         back.x, back.y, back.z, -dot(back, kEye)};
    camera::Transform decoded;
    check(camera::decode::transform_from_world_to_view(m, decoded), "decode succeeds");

    camera::Camera cam = camera::decode::renderware_camera(
        camera::decode::ViewWindow{kGuestTanX, kGuestTanY}, 1.0f, 20000.13f,
        camera::ProjectionType::Perspective, decoded);
    camera::CameraPolicy policy;
    policy.target_aspect = 16.0f / 9.0f;
    cam = camera::apply_policy(cam, policy);

    Layer layer;
    check(layer.begin_frame(cam, screen(), true), "the decoded camera is accepted");
    check_near(layer.place(gauge_at(world_point(137.5f, 0.0f, 0.0f)), composition()).depth, 137.5, 1.0e-2,
               "depth through the decoded camera is the view-axis distance");
    check(layer.place(gauge_at(world_point(200.1f, 0.0f, 0.0f)), composition()).rejection ==
              Rejection::AuthoredMaxDepth,
          "the authored maximum rejects at the same depth through the decoded camera");
}

}  // namespace

int main() {
    test_an_unbuilt_camera_is_refused();
    test_depth_is_view_axis_not_range();
    test_authored_window_is_exact_and_aspect_independent();
    test_authored_limits_are_per_instance();
    test_scale_law_is_the_raw_quotient();
    test_fill_is_instantaneous_and_clamped();
    test_width_law();
    test_widening_admits_more_horizontally_and_nothing_vertically();
    test_gate_order();
    test_non_finite_anchor_is_refused();
    test_rejection_is_total();
    test_geometry_follows_the_authored_laws();
    test_no_canvas_assumption();
    test_widening_does_not_resize_the_bar();
    test_the_bar_is_culled_when_the_BAR_leaves_not_its_anchor();
    test_cull_threshold_tracks_the_bar_size();
    test_zero_footprint_is_exactly_the_anchor_test();
    test_vertical_edges_use_the_bar_extent_too();
    test_stats_attribute_every_outcome();
    test_world_to_view_decode_is_exact();
    test_decoded_camera_reproduces_the_authored_rules();

    if (g_failures != 0) {
        std::printf("health_bar_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("health_bar_tests: all checks passed\n");
    return 0;
}
