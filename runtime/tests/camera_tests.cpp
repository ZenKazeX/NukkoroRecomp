// Tests for the native camera model: fidelity to the original.
//
// Every expectation here is traceable to docs/visibility-2026-09-21/
// ORIGINAL_BEHAVIOUR.md and MEASUREMENTS.md and, through those, either to an
// instruction in the PAL DOL or to a number produced by a measured run.
//
// What this suite holds:
//   1. 4:3 is bit-exactly the original. The Hor+ extent scale must be EXACTLY
//      1.0f at and below the reference aspect, and the encoded matrix must be
//      coefficient-identical to the one the original programs.
//   2. GX and RenderWare are DECODERS. Decoding a legacy camera and re-encoding
//      it must round-trip, and a decoded original camera must always come out
//      symmetric because the original shears its view matrix, never its
//      projection.
//   3. The seven visibility systems stay separate. Internal scale never reaches
//      a camera; far clip and draw distance are never the same number.

#include "gekko/camera.h"

#include <cmath>
#include <cstdio>
#include <string>

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
        std::printf("FAIL: %s (got %.9f, expected %.9f +/- %g)\n", what.c_str(), actual, expected,
                    tolerance);
        ++g_failures;
    }
}

void check_exact(float actual, float expected, const std::string& what) {
    if (!(actual == expected)) {
        std::printf("FAIL: %s (got %.9g, expected %.9g exactly)\n", what.c_str(),
                    static_cast<double>(actual), static_cast<double>(expected));
        ++g_failures;
    }
}

using namespace gekko;
using namespace gekko::camera;

constexpr double kPi = 3.14159265358979323846;

// The measured in-stage gameplay camera: viewWindow read by --visibility-probe
// on the Final Haunt route, near 1.0 and far 20000.13 from RENDERER_RE/01:619.
Camera measured_camera() {
    return decode::renderware_camera(decode::ViewWindow{0.615808f, 0.542110f}, 1.0f, 20000.13f,
                                     ProjectionType::Perspective, Transform{});
}

// What the camera SHOULD be once the stuck hFOV is fixed: the original's live
// horizontal FOV of 72.1447 degrees, tan(half) = 0.7284736.
Camera original_camera() {
    return decode::renderware_camera(decode::ViewWindow{0.7284736f, 0.542110f}, 1.0f, 20000.13f,
                                     ProjectionType::Perspective, Transform{});
}

// ---------------------------------------------------------------------------

void test_reference_aspect_is_the_display_aspect() {
    check_exact(kReferenceAspect, 4.0f / 3.0f, "the reference aspect is the 4:3 DISPLAY aspect");
    // 1.3195877 is the game's own PROJECTION aspect (guest float at 0x805EBA30)
    // and must never be substituted for the display aspect: it would make the
    // 4:3 extent scale 1.0104 instead of exactly 1.0.
    check(horizontal_extent_scale(kReferenceAspect, 1.3195877f) != 1.0f,
          "using the projection aspect as the reference would NOT be neutral at 4:3");
}

void test_extent_scale_is_exactly_neutral_at_and_below_reference() {
    check_exact(horizontal_extent_scale(4.0f / 3.0f, kReferenceAspect), 1.0f,
                "the Hor+ extent scale is EXACTLY 1.0 at 4:3");
    // Hor+ only ever widens: a window narrower than 4:3 keeps the original
    // frustum and is pillarboxed rather than having its horizontal FOV cropped.
    check_exact(horizontal_extent_scale(1.0f, kReferenceAspect), 1.0f,
                "exactly 1.0 at 1:1 (narrower than reference: never narrows)");
    check_exact(horizontal_extent_scale(5.0f / 4.0f, kReferenceAspect), 1.0f, "exactly 1.0 at 5:4");
    // Tolerances are float32-relative: the scale is computed in float, whose
    // precision at 2.67 is about 3e-7 absolute. A tighter bound would be
    // testing the arithmetic of the test, not the behaviour of the code.
    check_near(horizontal_extent_scale(16.0f / 9.0f, kReferenceAspect), (16.0 / 9.0) / (4.0 / 3.0),
               1e-6, "at 16:9 the extents widen by (16/9)/(4/3)");
    check_near(horizontal_extent_scale(32.0f / 9.0f, kReferenceAspect), (32.0 / 9.0) / (4.0 / 3.0),
               1e-6, "at 32:9 the extents widen by (32/9)/(4/3)");
    check_exact(horizontal_extent_scale(0.0f, kReferenceAspect), 1.0f, "zero aspect is neutral");
    check_exact(horizontal_extent_scale(std::nanf(""), kReferenceAspect), 1.0f,
                "NaN aspect is neutral");
}

// ---------------------------------------------------------------------------
// The canonical coverage primitive: the ONE source every 3D-anchored consumer
// derives its horizontal visibility decision from.
// ---------------------------------------------------------------------------

void test_coverage_is_exactly_neutral_at_4_3() {
    const Coverage c = Coverage::from_aspect(4.0f / 3.0f);
    check(c.native(), "4:3 coverage reports native");
    check_exact(c.extent_scale, 1.0f, "4:3 extent scale is EXACTLY 1");
    check_exact(c.affine_scale(), 1.0f, "4:3 affine scale is EXACTLY 1");
    // The framebuffer bounds must be bit-exactly the original rectangle, or
    // every consumer's 4:3 behaviour changes.
    check_exact(c.framebuffer_left(640.0f), 0.0f, "4:3 visible left is EXACTLY 0");
    check_exact(c.framebuffer_right(640.0f), 640.0f, "4:3 visible right is EXACTLY 640");
    check_exact(c.ndc_extent(), 1.0f, "4:3 NDC extent is EXACTLY 1");
    // Narrower than 4:3 must also be neutral: Hor+ only ever widens.
    check_exact(Coverage::from_aspect(1.0f).framebuffer_right(640.0f), 640.0f,
                "a 1:1 target still clips at exactly 640");
}

void test_coverage_framebuffer_bounds_match_the_hud_affine() {
    // The bounds must be the displayed screen inverse-mapped through the very
    // affine layout_hud applies, x' = 320 + ax*(x - 320). Anything else would
    // be a second, independent widescreen derivation.
    for (const float aspect : {16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f, 2.37f}) {
        const Coverage c = Coverage::from_aspect(aspect);
        const float ax = c.affine_scale();
        const float left = c.framebuffer_left(640.0f);
        const float right = c.framebuffer_right(640.0f);
        // Mapping the bounds forward must land exactly on the screen edges.
        check_near(320.0f + ax * (left - 320.0f), 0.0, 1e-3,
                   "the left visible bound maps to displayed x = 0");
        check_near(320.0f + ax * (right - 320.0f), 640.0, 1e-3,
                   "the right visible bound maps to displayed x = 640");
        check(left < 0.0f && right > 640.0f,
              "at a wider aspect the visible rectangle EXPANDS in guest space");
        // And the NDC form is the same number, not a parallel derivation.
        check_near(c.ndc_extent(), 1.0 / ax, 1e-5,
                   "the NDC extent is the reciprocal of the same affine scale");
    }
}

void test_coverage_reproduces_the_captured_gauge_fixtures() {
    // Regression fixtures from the live session, GAUGE_POPPING_EVIDENCE.md.
    // Each is a gauge that was DRAWING and then vanished while its enemy was
    // still well inside the displayed image. guest tanX 0.615808, native
    // 0.821077 -> coverage 1.3333.
    const Coverage c = Coverage::from_aspect(16.0f / 9.0f);
    check_near(c.extent_scale, 0.821077 / 0.615808, 1e-4,
               "16:9 coverage equals the measured native/guest tangent ratio");

    const float left = c.framebuffer_left(640.0f);
    const float right = c.framebuffer_right(640.0f);
    // The three captured FIRST-GONE frames must now be ACCEPTED.
    check(644.0f <= right, "fixture 0x813A48E8: screen X 644 is inside the new right bound");
    check(650.0f <= right, "fixture 0x8139CA40: screen X 650 is inside the new right bound");
    check(-3.0f >= left, "fixture 0x813A82E8: screen X -3 is inside the new left bound");
    // And they must map back onto the screen where the enemy actually was.
    const float ax = c.affine_scale();
    check_near(320.0f + ax * (644.0f - 320.0f), 563.0, 1.0,
               "fixture 0x813A48E8 lands at displayed pixel 563, as measured");
    check_near(320.0f + ax * (-3.0f - 320.0f), 77.7, 1.0,
               "fixture 0x813A82E8 lands at displayed pixel 77.7, as measured");

    // A point genuinely off the displayed screen must STILL be rejected.
    check(760.0f > right, "a point past the displayed right edge is still rejected");
    check(-120.0f < left, "a point past the displayed left edge is still rejected");

    // At 4:3 every one of those fixtures must reject exactly as the original did.
    const Coverage native = Coverage::from_aspect(4.0f / 3.0f);
    check(644.0f > native.framebuffer_right(640.0f),
          "at 4:3 the same fixture is rejected, exactly as the original");
    check(-3.0f < native.framebuffer_left(640.0f),
          "at 4:3 the left fixture is rejected, exactly as the original");
}

void test_coverage_leaves_vertical_alone() {
    for (const float aspect : {4.0f / 3.0f, 16.0f / 9.0f, 32.0f / 9.0f}) {
        const Coverage c = Coverage::from_aspect(aspect);
        check_exact(c.framebuffer_top(480.0f), 0.0f, "vertical top bound is always 0");
        check_exact(c.framebuffer_bottom(480.0f), 480.0f, "vertical bottom bound is always 480");
        check_exact(c.ndc_extent_vertical(), 1.0f, "vertical NDC extent is always 1");
    }
}

void test_coverage_round_trips_through_the_affine_scale() {
    for (const float aspect : {4.0f / 3.0f, 16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f}) {
        const Coverage a = Coverage::from_aspect(aspect);
        const Coverage b = Coverage::from_affine_scale(a.affine_scale());
        check_near(b.extent_scale, a.extent_scale, 1e-5,
                   "coverage round-trips through its affine scale");
    }
    // Degenerate affine inputs must be neutral, never narrowing.
    check_exact(Coverage::from_affine_scale(0.0f).extent_scale, 1.0f, "zero affine is neutral");
    check_exact(Coverage::from_affine_scale(2.0f).extent_scale, 1.0f, "affine > 1 is neutral");
    check_exact(Coverage::from_affine_scale(std::nanf("")).extent_scale, 1.0f, "NaN affine is neutral");
}

void test_policy_and_coverage_agree() {
    // apply_policy must widen the world frustum by exactly the same number the
    // screen-space consumers clip against. One camera model for everything.
    const Camera decoded = measured_camera();
    for (const float aspect : {16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f}) {
        CameraPolicy policy;
        policy.target_aspect = aspect;
        const Camera widened = apply_policy(decoded, policy);
        const Coverage c = Coverage::from_aspect(aspect);
        check_near(widened.extents.right / decoded.extents.right, c.extent_scale, 1e-5,
                   "the world frustum widens by exactly the canonical coverage");
    }
}

void test_4_3_is_coefficient_identical_to_the_original() {
    const Camera decoded = measured_camera();
    const Projection reference = projection_matrix(decoded);

    CameraPolicy policy;  // defaults: HorPlus, target 4:3, reference 4:3
    const Projection at_4_3 = projection_matrix(apply_policy(decoded, policy));
    check(at_4_3.m00 == reference.m00 && at_4_3.m02 == reference.m02 &&
              at_4_3.m11 == reference.m11 && at_4_3.m12 == reference.m12 &&
              at_4_3.m22 == reference.m22 && at_4_3.m23 == reference.m23,
          "Hor+ at 4:3 is coefficient-identical to the decoded camera");

    policy.aspect_policy = AspectPolicy::Original;
    const Camera untouched = apply_policy(decoded, policy);
    check(untouched.extents.left == decoded.extents.left &&
              untouched.extents.right == decoded.extents.right &&
              untouched.extents.bottom == decoded.extents.bottom &&
              untouched.extents.top == decoded.extents.top,
          "AspectPolicy::Original leaves every extent untouched");
}

void test_encoded_matrix_matches_the_measured_gx_block() {
    // CAMERABEGINUPDATE 0x804956F4 programs m00 = recipViewWindow.x, m11 =
    // recipViewWindow.y, m02 = m12 = 0, m22 = -near/(far-near), m23 = far*m22.
    // The canonical encoding must collapse to exactly that for the symmetric
    // extents a decoded original camera always has.
    const Projection p = projection_matrix(measured_camera());
    check_near(p.m00, 1.0 / 0.615808, 1e-4, "m00 collapses to the reciprocal view window x");
    check_near(p.m11, 1.0 / 0.542110, 1e-4, "m11 collapses to the reciprocal view window y");
    check_exact(p.m02, 0.0f, "m02 is exactly zero for symmetric extents");
    check_exact(p.m12, 0.0f, "m12 is exactly zero for symmetric extents");
    check_near(p.m22, -5.00022e-5, 1e-9, "m22 = -near/(far-near) matches the captured block");
    check_near(p.m23, -1.00005, 1e-5, "m23 = far*m22 matches the captured block");
}

void test_gx_decode_round_trips() {
    // GX is an input decoder. Encoding a camera and decoding it back must
    // return the same camera, or the boundary is lossy.
    for (const Camera& source : {measured_camera(), original_camera()}) {
        const Projection p = projection_matrix(source);
        const std::array<float, 6> c = p.coefficients();
        Camera back;
        check(decode::gx_projection(0u, c.data(), Transform{}, back),
              "a perspective GX block decodes");
        check_near(back.extents.left, source.extents.left, 1e-4, "decode recovers left");
        check_near(back.extents.right, source.extents.right, 1e-4, "decode recovers right");
        check_near(back.extents.bottom, source.extents.bottom, 1e-4, "decode recovers bottom");
        check_near(back.extents.top, source.extents.top, 1e-4, "decode recovers top");
        check_near(back.near_plane, source.near_plane, 1e-2, "decode recovers near");
        check_near(back.far_plane, source.far_plane, 2.0, "decode recovers far");
    }

    // An orthographic block uses the opposite shear sign and a different depth
    // law; it must round-trip too.
    Camera ortho;
    ortho.type = ProjectionType::Orthographic;
    ortho.extents = FrustumExtents{-320.0f, 320.0f, -240.0f, 240.0f};
    ortho.near_plane = 1.0f;
    ortho.far_plane = 1000.0f;
    const std::array<float, 6> oc = projection_matrix(ortho).coefficients();
    Camera oback;
    check(decode::gx_projection(1u, oc.data(), Transform{}, oback),
          "an orthographic GX block decodes");
    check_near(oback.extents.right, 320.0, 1e-2, "ortho decode recovers right");
    check_near(oback.extents.top, 240.0, 1e-2, "ortho decode recovers top");
    check_near(oback.far_plane, 1000.0, 1e-1, "ortho decode recovers far");

    // Garbage in must be refused, not silently turned into a camera.
    const float degenerate[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    Camera ignored;
    check(!decode::gx_projection(0u, degenerate, Transform{}, ignored),
          "a degenerate GX block is refused");
}

void test_decoded_original_camera_is_symmetric() {
    // RenderWare applies viewOffset by shearing the VIEW matrix, so a decoded
    // original camera is always centred. An earlier draft of this file copied
    // the view offset into the projection shear, which double-applied it.
    const Camera c = measured_camera();
    check(c.extents.symmetric_shape(), "a decoded RenderWare camera has symmetric extents");
    check_exact(c.extents.centre_x(), 0.0f, "its horizontal centre is exactly zero");
    check_exact(c.extents.centre_y(), 0.0f, "its vertical centre is exactly zero");
}

void test_fov_and_the_stuck_hfov() {
    const Camera c = measured_camera();
    check_near(c.horizontal_fov_radians() * 180.0 / kPi, 63.2501, 1e-3,
               "the port's live hFOV is the compiled default 63.2501 deg");
    check_near(c.vertical_fov_radians() * 180.0 / kPi, 56.9251, 1e-3,
               "the port's live vFOV is 56.9251 deg (53.130096 x 15/14)");
    check_near(c.extents.aspect(), 1.135946, 1e-5,
               "the port's frustum aspect is 1.135946 - narrower than 4:3");

    const Camera original = original_camera();
    check_near(original.horizontal_fov_radians() * 180.0 / kPi, 72.1447, 1e-3,
               "the original's live hFOV is 72.1447 deg");
    check_near(original.extents.aspect(), 1.3437745, 1e-5,
               "the original's frustum aspect is 1.3438, close to 4:3");
}

void test_fov_override_is_independent_of_aspect_policy() {
    const Camera c = measured_camera();
    CameraPolicy policy;
    policy.aspect_policy = AspectPolicy::Original;
    policy.hfov_override_rad = kOriginalLiveHFovRad;  // 72.1447 deg

    const Camera fixed = apply_policy(c, policy);
    check_near(fixed.extents.right, 0.7284736, 1e-5, "an hFOV override sets the horizontal extent");
    check_near(fixed.extents.top, 0.542110, 1e-5, "an hFOV override leaves the vertical alone");

    // Overrides and policy compose: the override sets the base coverage, then
    // Hor+ widens it by the presentation stretch.
    policy.aspect_policy = AspectPolicy::HorPlus;
    policy.target_aspect = 16.0f / 9.0f;
    const Camera widened = apply_policy(c, policy);
    check_near(widened.extents.right, 0.7284736 * (16.0 / 9.0) / (4.0 / 3.0), 1e-4,
               "Hor+ widens the overridden coverage by exactly the presentation stretch");
    check_near(widened.extents.top, 0.542110, 1e-5, "Hor+ still leaves the vertical alone");
}

void test_near_and_far_are_independently_controllable() {
    const Camera c = measured_camera();
    CameraPolicy policy;
    policy.near_override = 5.0f;
    const Camera near_only = apply_policy(c, policy);
    check_exact(near_only.near_plane, 5.0f, "the near override takes effect");
    check_exact(near_only.far_plane, c.far_plane, "the near override leaves far alone");

    policy = CameraPolicy{};
    policy.far_override = 4000.0f;
    const Camera far_only = apply_policy(c, policy);
    check_exact(far_only.far_plane, 4000.0f, "the far override takes effect");
    check_exact(far_only.near_plane, c.near_plane, "the far override leaves near alone");
    check_exact(far_only.extents.right, c.extents.right,
                "a clip override does not move the horizontal coverage");
    check_exact(far_only.extents.top, c.extents.top,
                "a clip override does not move the vertical coverage");
}

void test_widening_recovers_content_at_the_edge() {
    const Camera c = measured_camera();
    const float z = -100.0f;
    const float edge = c.extents.right * 100.0f;  // half-width at |z| = 100
    const Sphere probe{edge * 1.20f, 0.0f, z, 1.0f};

    CameraPolicy at_4_3;
    check(!classify_sphere(view_frustum(apply_policy(c, at_4_3)), probe).visible(),
          "the probe sphere is outside the 4:3 frustum");

    CameraPolicy at_16_9;
    at_16_9.target_aspect = 16.0f / 9.0f;
    check(classify_sphere(view_frustum(apply_policy(c, at_16_9)), probe).visible(),
          "Hor+ at 16:9 brings the same sphere inside the frustum");

    // Vert- must NOT widen horizontally: a different policy with a different
    // answer, and collapsing the two is what this suite prevents.
    CameraPolicy vert;
    vert.aspect_policy = AspectPolicy::VertMinus;
    vert.target_aspect = 16.0f / 9.0f;
    check(!classify_sphere(view_frustum(apply_policy(c, vert)), probe).visible(),
          "Vert- does NOT widen the horizontal frustum");
}

void test_internal_scale_never_reaches_the_camera() {
    const Camera c = measured_camera();
    CameraPolicy policy;
    policy.target_aspect = 16.0f / 9.0f;
    const Camera solved = apply_policy(c, policy);
    const Projection p = projection_matrix(solved);
    const Frustum f = view_frustum(solved);

    // There is deliberately no overload anywhere that takes a scale alongside a
    // camera: the scale multiplies pixel rectangles and nothing else.
    const PixelViewport guest_vp{0, 0, 640, 480, 0.0f, 1.0f};
    for (u32 scale = 1; scale <= 6; ++scale) {
        const PixelViewport scaled = scale_viewport(guest_vp, scale);
        check(scaled.width == static_cast<s32>(640u * scale) &&
                  scaled.height == static_cast<s32>(480u * scale),
              "the viewport scales by exactly the integer factor");
        check(scaled.min_depth == guest_vp.min_depth && scaled.max_depth == guest_vp.max_depth,
              "the internal scale does not touch the depth range");
        check(scale_scissor(PixelScissor{0, 0, 640, 480}, scale).width ==
                  static_cast<s32>(640u * scale),
              "the scissor scales by exactly the integer factor");

        const Projection again = projection_matrix(apply_policy(c, policy));
        check(again.m00 == p.m00 && again.m11 == p.m11 && again.m22 == p.m22,
              "the projection is independent of the internal scale");
        const Frustum f2 = view_frustum(apply_policy(c, policy));
        check(f2.planes[0].nx == f.planes[0].nx && f2.planes[0].d == f.planes[0].d,
              "the frustum is independent of the internal scale");
    }
}

void test_arbitrary_aspects_are_monotone_and_stable() {
    const Camera c = measured_camera();
    float previous = 0.0f;
    for (const float aspect : {4.0f / 3.0f, 1.5f, 16.0f / 10.0f, 16.0f / 9.0f, 2.0f, 21.0f / 9.0f,
                               2.37f, 32.0f / 9.0f, 5.0f}) {
        CameraPolicy policy;
        policy.target_aspect = aspect;
        const Camera solved = apply_policy(c, policy);
        check(solved.extents.right >= previous - 1e-6f,
              "horizontal coverage never shrinks as the target widens");
        previous = solved.extents.right;
        check_exact(solved.extents.top, c.extents.top,
                    "vertical coverage is bit-stable at every aspect (this is what Hor+ means)");
        const Camera again = apply_policy(c, policy);
        check(again.extents.right == solved.extents.right,
              "policy application is deterministic");
    }
}

void test_far_clip_and_draw_distance_are_separate() {
    FarClipFilter filter;
    filter.reset();
    check_exact(filter.far_plane, 30000.0f, "the filter reset seeds far = 30000.0");
    check_exact(filter.near_plane, 1.0f, "the filter reset seeds near = 1.0");

    const float before = filter.far_plane;
    filter.advance(1.0f, 20000.0f);
    check(filter.far_plane < before && filter.far_plane > 20000.0f,
          "one frame moves the far clip towards the zone mean without reaching it");
    for (int i = 0; i < 2000; ++i) filter.advance(1.0f, 20000.0f);
    check_near(filter.far_plane, 20000.0, 1.0, "the far clip converges on the zone mean");

    check_exact(VisibilityBudget::effect_limit_from_launch_info(0), 400.0f,
                "launch-info 0 means a 400.0 unit effect limit");
    check_exact(VisibilityBudget::effect_limit_from_launch_info(15), 1500.0f,
                "launch-info N means N x 100 units");
    check(VisibilityBudget::effect_limit_from_launch_info(15) != filter.far_plane,
          "the effect draw distance is not the far clip");
}

void test_effect_ramp_matches_the_original() {
    VisibilityBudget budget;
    budget.effect_distance = 400.0f;
    budget.effect_ramp_begin = 60.0f;
    const float base = 30.0f;

    check_near(effect_level_ramp(0.0f, budget, base), base, 1e-5,
               "inside the ramp start the level is the base");
    check_near(effect_level_ramp(60.0f, budget, base), base, 1e-5,
               "at the ramp start the level is still the base");
    check_near(effect_level_ramp(400.0f, budget, base), -127.0, 1e-4,
               "at the limit the level is exactly -127");
    check_near(effect_level_ramp(1000.0f, budget, base), -127.0, 1e-4,
               "beyond the limit the level stays -127");
    check_near(effect_level_ramp(399.9f, budget, base), -127.0, 0.1,
               "the ramp is continuous with the hard reject at the limit");
}

void test_world_distance_is_off_by_default() {
    const Camera c = measured_camera();
    const Frustum f = view_frustum(c);
    const VisibilityBudget budget;
    check_exact(budget.world_distance, 0.0f,
                "there is NO world draw distance by default - the original has none");

    const Sphere distant{0.0f, 0.0f, -15000.0f, 10.0f};
    check(classify(f, distant, budget, 0.0f, 0.0f, 0.0f).visible(),
          "with no world distance a far-but-in-frustum sphere is visible");

    VisibilityBudget limited;
    limited.world_distance = 1000.0f;
    const VisibilityResult capped = classify(f, distant, limited, 0.0f, 0.0f, 0.0f);
    check(!capped.visible() && capped.reason == RejectReason::WorldDistance,
          "an explicit world distance rejects with RejectReason::WorldDistance");
    check(capped.margin > 0.0f, "the world-distance rejection reports how far past the limit");
}

void test_stats_account_for_every_rejection() {
    const Camera c = measured_camera();
    const Frustum f = view_frustum(c);
    VisibilityStats stats;

    stats.record(classify_sphere(f, Sphere{0.0f, 0.0f, -100.0f, 1.0f}));        // inside
    stats.record(classify_sphere(f, Sphere{0.0f, 0.0f, 10.0f, 1.0f}));          // near
    stats.record(classify_sphere(f, Sphere{0.0f, 0.0f, -50000.0f, 1.0f}));      // far
    stats.record(classify_sphere(f, Sphere{-500.0f, 0.0f, -100.0f, 1.0f}));     // a side
    stats.record(classify_sphere(f, Sphere{0.0f, 0.0f, 0.0f, std::nanf("")}));  // degenerate

    check(stats.tested == 5, "every classification is counted");
    check(stats.inside == 1, "the inside sphere is counted as inside");
    check(stats.rejected == 4, "four rejections are counted");
    check(stats.rejected_by_plane[static_cast<u32>(FrustumPlane::Near)] == 1,
          "the near-plane rejection is attributed to the near plane");
    check(stats.rejected_by_plane[static_cast<u32>(FrustumPlane::Far)] == 1,
          "the far-plane rejection is attributed to the far plane");
    check(stats.rejected_degenerate == 1, "the degenerate sphere is counted separately");
    check(stats.closest_rejection_margin[static_cast<u32>(FrustumPlane::Near)] > 0.0f,
          "the closest rejection margin records the boundary in world units");
}

void test_visibility_system_shares_one_camera() {
    const Camera decoded = measured_camera();
    CameraPolicy policy;
    policy.target_aspect = 21.0f / 9.0f;

    VisibilitySystem system;
    system.update(decoded, policy);

    const Camera expected = apply_policy(decoded, policy);
    const Projection expected_matrix = projection_matrix(expected);
    check(system.projection().m00 == expected_matrix.m00 &&
              system.projection().m02 == expected_matrix.m02 &&
              system.projection().m11 == expected_matrix.m11 &&
              system.projection().m22 == expected_matrix.m22,
          "the system uploads the matrix encoded from its own camera");

    const Frustum expected_planes = view_frustum(expected);
    bool same = true;
    for (u32 i = 0; i < kFrustumPlaneCount; ++i) {
        same = same && system.view_planes().planes[i].nx == expected_planes.planes[i].nx &&
               system.view_planes().planes[i].d == expected_planes.planes[i].d;
    }
    check(same, "the system culls against planes built from that same camera's extents");
    check(system.camera().extents.right == expected.extents.right,
          "the system's camera is the policy-applied camera");
}

void test_anamorphic_is_available_but_not_the_default() {
    const Camera c = measured_camera();
    CameraPolicy policy;
    check(policy.aspect_policy == AspectPolicy::HorPlus, "Hor+ is the default policy");

    policy.aspect_policy = AspectPolicy::Anamorphic;
    policy.target_aspect = 16.0f / 9.0f;
    check_exact(apply_policy(c, policy).extents.right, c.extents.right,
                "Anamorphic leaves the extents alone (presentation does the stretching)");
}

}  // namespace

int main() {
    test_reference_aspect_is_the_display_aspect();
    test_extent_scale_is_exactly_neutral_at_and_below_reference();
    test_coverage_is_exactly_neutral_at_4_3();
    test_coverage_framebuffer_bounds_match_the_hud_affine();
    test_coverage_reproduces_the_captured_gauge_fixtures();
    test_coverage_leaves_vertical_alone();
    test_coverage_round_trips_through_the_affine_scale();
    test_policy_and_coverage_agree();
    test_4_3_is_coefficient_identical_to_the_original();
    test_encoded_matrix_matches_the_measured_gx_block();
    test_gx_decode_round_trips();
    test_decoded_original_camera_is_symmetric();
    test_fov_and_the_stuck_hfov();
    test_fov_override_is_independent_of_aspect_policy();
    test_near_and_far_are_independently_controllable();
    test_widening_recovers_content_at_the_edge();
    test_internal_scale_never_reaches_the_camera();
    test_arbitrary_aspects_are_monotone_and_stable();
    test_far_clip_and_draw_distance_are_separate();
    test_effect_ramp_matches_the_original();
    test_world_distance_is_off_by_default();
    test_stats_account_for_every_rejection();
    test_visibility_system_shares_one_camera();
    test_anamorphic_is_available_but_not_the_default();

    if (g_failures != 0) {
        std::printf("camera_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("camera_tests: all checks passed\n");
    return 0;
}
