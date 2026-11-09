// Tests for NativeWorldAnchorProjection.
//
// The central claim this suite defends: a world-anchored element is visible
// exactly when its anchor is inside the DISPLAYED camera, at every aspect, with
// no widescreen term anywhere in the visibility test. The old architecture
// projected into a 640x480 guest canvas, clipped against that canvas, and then
// had the host repair the position; this one projects through the canonical
// camera straight to presentation pixels, so there is nothing to repair.
//
// The regression fixtures are the ACCEPT -> REJECT transitions captured live on
// Final Haunt (docs/visibility-2026-09-21/GAUGE_POPPING_EVIDENCE.md): enemy HP
// gauges that vanished at guest X 644 / 650 / -3 while their enemy was still
// 72-78 px from the edge of a 640-wide display.

#include "gekko/world_anchor.h"

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
        std::printf("FAIL: %s (got %.6f, expected %.6f +/- %g)\n", what.c_str(), actual, expected,
                    tolerance);
        ++g_failures;
    }
}

using namespace gekko;
using namespace gekko::world_anchor;

constexpr float kGuestTanX = 0.615808f;  // the original's live hFOV, 63.2501 deg
constexpr float kGuestTanY = 0.542110f;  // 56.9251 deg

// The canonical camera at a given display aspect: the original's coverage,
// widened by the canonical policy. Identity at 4:3.
camera::Camera camera_at(float aspect) {
    const camera::Camera decoded = camera::decode::renderware_camera(
        camera::decode::ViewWindow{kGuestTanX, kGuestTanY}, 1.0f, 20000.13f,
        camera::ProjectionType::Perspective, camera::Transform{});
    camera::CameraPolicy policy;
    policy.target_aspect = aspect;
    return camera::apply_policy(decoded, policy);
}

ContentViewport screen() { return ContentViewport{0.0f, 0.0f, 640.0f, 480.0f}; }

// A world point that the ORIGINAL's 63.2501 deg projection would place at a
// given guest canvas X, at a given depth. Inverting the legacy chain lets the
// captured fixtures be expressed as world anchors, which is what the new system
// consumes.
Request anchor_from_guest_x(float guest_x, float depth) {
    // guest ndc = (x - 320) / 320 ; view_x = ndc * tan * depth
    const float ndc = (guest_x - 320.0f) / 320.0f;
    Request r;
    r.world_x = ndc * kGuestTanX * depth;
    r.world_y = 0.0f;
    r.world_z = -depth;  // camera at the origin looking down -z
    return r;
}

// ---------------------------------------------------------------------------

void test_4_3_is_the_original_boundary() {
    Projector p;
    p.update(camera_at(4.0f / 3.0f), screen());

    // At 4:3 the canonical camera IS the original camera, so the accept/reject
    // boundary must land exactly on the guest canvas edges.
    check(p.project(anchor_from_guest_x(639.0f, 200.0f)).visible,
          "4:3: guest X 639 is visible");
    check(!p.project(anchor_from_guest_x(644.0f, 200.0f)).visible,
          "4:3: guest X 644 is REJECTED, exactly as the original");
    check(!p.project(anchor_from_guest_x(650.0f, 120.0f)).visible,
          "4:3: guest X 650 is REJECTED, exactly as the original");
    check(!p.project(anchor_from_guest_x(-3.0f, 108.0f)).visible,
          "4:3: guest X -3 is REJECTED, exactly as the original");
    check(p.project(anchor_from_guest_x(1.0f, 108.0f)).visible, "4:3: guest X 1 is visible");

    // And the projected pixel must be the guest pixel, because the camera is
    // the guest camera and the viewport is the guest raster.
    const Result centre = p.project(anchor_from_guest_x(320.0f, 200.0f));
    check_near(centre.pixel_x, 320.0, 0.01, "4:3: the centre projects to pixel 320");
    const Result edge = p.project(anchor_from_guest_x(639.0f, 200.0f));
    check_near(edge.pixel_x, 639.0, 0.05, "4:3: guest X 639 projects to pixel 639");
}

void test_captured_fixtures_are_visible_at_16_9() {
    Projector p;
    p.update(camera_at(16.0f / 9.0f), screen());

    // The three captured FIRST-GONE frames. Each must now be VISIBLE, and must
    // land on the displayed pixel that was measured for it.
    struct Fixture {
        const char* gauge;
        float guest_x;
        float depth;
        double expected_pixel;
    };
    const Fixture fixtures[] = {
        {"0x813A48E8", 644.0f, 195.81f, 563.0},
        {"0x8139CA40", 650.0f, 120.41f, 567.5},
        {"0x813A82E8", -3.0f, 107.88f, 77.7},
    };
    for (const Fixture& f : fixtures) {
        const Result r = p.project(anchor_from_guest_x(f.guest_x, f.depth));
        check(r.visible, std::string("16:9: fixture ") + f.gauge + " (guest X " +
                             std::to_string(static_cast<int>(f.guest_x)) + ") is VISIBLE");
        check_near(r.pixel_x, f.expected_pixel, 1.0,
                   std::string("16:9: fixture ") + f.gauge + " lands on the measured pixel");
        check_near(r.depth, f.depth, 0.01,
                   std::string("16:9: fixture ") + f.gauge + " depth is unchanged");
        check(r.edge_margin_x > 60.0f,
              std::string("16:9: fixture ") + f.gauge + " still has >60 px of edge margin");
    }

    // A point genuinely off the displayed screen must STILL be rejected - the
    // fix moves the boundary onto the display, it does not remove it.
    check(!p.project(anchor_from_guest_x(780.0f, 195.0f)).visible,
          "16:9: a point past the displayed right edge is still rejected");
    check(!p.project(anchor_from_guest_x(-140.0f, 108.0f)).visible,
          "16:9: a point past the displayed left edge is still rejected");
}

void test_no_widescreen_term_in_the_visibility_test() {
    // The same world anchor, at a sequence of aspects. As the display widens,
    // an anchor can only become MORE visible, never less, and the vertical
    // result must never change.
    const Request probe = anchor_from_guest_x(700.0f, 150.0f);
    bool previously_visible = false;
    for (const float aspect : {4.0f / 3.0f, 16.0f / 10.0f, 16.0f / 9.0f, 21.0f / 9.0f,
                               32.0f / 9.0f}) {
        Projector p;
        p.update(camera_at(aspect), screen());
        const Result r = p.project(probe);
        if (previously_visible) {
            check(r.visible, "widening the display never hides a world-anchored element");
        }
        previously_visible = r.visible || previously_visible;
        // Vertical NDC must be identical at every aspect: Hor+ leaves vertical
        // coverage alone, so a vertical decision cannot depend on the target.
        check_near(r.ndc_y, 0.0, 1e-6, "vertical NDC is unaffected by the display aspect");
    }
    check(previously_visible, "the probe becomes visible at some aspect");
}

void test_authored_depth_rules_are_exact_and_aspect_independent() {
    for (const float aspect : {4.0f / 3.0f, 16.0f / 9.0f, 32.0f / 9.0f}) {
        Projector p;
        p.update(camera_at(aspect), screen());
        Request r = anchor_from_guest_x(320.0f, 0.0f);
        r.min_depth = 20.0f;   // the gauge's authored minimum
        r.max_depth = 200.0f;  // the gauge's authored maximum

        const auto at_depth = [&](float depth) {
            Request q = r;
            const Request a = anchor_from_guest_x(320.0f, depth);
            q.world_x = a.world_x;
            q.world_y = a.world_y;
            q.world_z = a.world_z;
            return p.project(q);
        };
        check(at_depth(19.0f).rejection == Rejection::AuthoredMinDepth,
              "depth below the authored minimum rejects with AuthoredMinDepth");
        check(at_depth(21.0f).visible, "depth just inside the authored minimum is visible");
        check(at_depth(199.0f).visible, "depth just inside the authored maximum is visible");
        check(at_depth(201.0f).rejection == Rejection::AuthoredMaxDepth,
              "depth beyond the authored maximum rejects with AuthoredMaxDepth");
        // The 20-200 window must be byte-identical at every aspect: it is game
        // behaviour, not a widescreen quantity.
        check(at_depth(200.0f).visible, "exactly at the authored maximum is visible");
    }
}

void test_scale_law_is_preserved() {
    Projector p;
    p.update(camera_at(16.0f / 9.0f), screen());
    Request r = anchor_from_guest_x(320.0f, 100.0f);
    r.base_scale = 50.0f;
    const Result a = p.project(r);
    check_near(a.scale, 0.5, 1e-5, "scale = base_scale / depth");

    // The authored multiplier composes on top, exactly.
    r.scale_multiplier = 2.0f;
    check_near(p.project(r).scale, 1.0, 1e-5, "the authored scale multiplier composes");

    // Scale must not depend on the display aspect.
    Projector wide;
    wide.update(camera_at(32.0f / 9.0f), screen());
    r.scale_multiplier = 1.0f;
    check_near(wide.project(r).scale, a.scale, 1e-6,
               "the scale law is independent of the display aspect");

    // And no scale is computed when none is authored.
    Request none = anchor_from_guest_x(320.0f, 100.0f);
    check(p.project(none).scale == 0.0f, "no scale is produced when none is authored");
}

void test_consumer_margin_is_owned_by_the_consumer() {
    Projector p;
    p.update(camera_at(4.0f / 3.0f), screen());
    // LockonMark authors a 32 px margin. The common layer must not know that
    // number; it must apply whatever the consumer passes, on top of canonical
    // coverage.
    const Request just_outside = anchor_from_guest_x(655.0f, 150.0f);
    check(!p.project(just_outside).visible, "without a margin the anchor is rejected");

    Request with_margin = just_outside;
    with_margin.screen_margin = 32.0f;
    check(p.project(with_margin).visible, "a 32 px consumer margin admits it");

    Request tighter = just_outside;
    tighter.screen_margin = -32.0f;
    check(!p.project(tighter).visible, "a negative margin tightens the boundary");

    // The margin is in presentation pixels and must mean the same thing at
    // every aspect.
    Projector wide;
    wide.update(camera_at(16.0f / 9.0f), screen());
    Request edge = anchor_from_guest_x(760.0f, 150.0f);
    const bool without = wide.project(edge).visible;
    edge.screen_margin = 32.0f;
    const bool with = wide.project(edge).visible;
    check(!without && with, "the consumer margin behaves identically at 16:9");
}

void test_rejection_reasons_are_specific() {
    Projector p;
    p.update(camera_at(4.0f / 3.0f), screen());

    Request behind = anchor_from_guest_x(320.0f, 100.0f);
    behind.world_z = 100.0f;  // behind the eye
    check(p.project(behind).rejection == Rejection::BehindCamera,
          "a point behind the eye reports BehindCamera");

    Request near_reject = anchor_from_guest_x(320.0f, 0.5f);
    check(p.project(near_reject).rejection == Rejection::CameraNear,
          "a point inside the near plane reports CameraNear");

    Request far_reject = anchor_from_guest_x(320.0f, 50000.0f);
    check(p.project(far_reject).rejection == Rejection::CameraFar,
          "a point past the far plane reports CameraFar");

    check(p.project(anchor_from_guest_x(700.0f, 150.0f)).rejection == Rejection::OffScreenRight,
          "an anchor past the right edge reports OffScreenRight");
    check(p.project(anchor_from_guest_x(-60.0f, 150.0f)).rejection == Rejection::OffScreenLeft,
          "an anchor past the left edge reports OffScreenLeft");

    // use_camera_near_far off means the camera planes stop applying, but the
    // authored ones still do.
    Request no_camera = anchor_from_guest_x(320.0f, 0.5f);
    no_camera.use_camera_near_far = false;
    check(p.project(no_camera).visible, "with camera near/far disabled a close anchor is visible");
}

void test_content_viewport_is_honoured() {
    // A letterboxed presentation: the content does not start at 0,0.
    Projector p;
    p.update(camera_at(4.0f / 3.0f), ContentViewport{160.0f, 90.0f, 1280.0f, 960.0f});
    const Result centre = p.project(anchor_from_guest_x(320.0f, 200.0f));
    check_near(centre.pixel_x, 160.0 + 640.0, 0.1, "the centre lands in the middle of the content");
    check_near(centre.pixel_y, 90.0 + 480.0, 0.1, "the vertical centre likewise");

    const Result right = p.project(anchor_from_guest_x(639.0f, 200.0f));
    check(right.visible, "the content rectangle does not change visibility");
    check_near(right.pixel_x, 160.0 + 1280.0 * (639.0 / 640.0), 2.0,
               "the right edge scales with the content rectangle");
}

void test_ndc_agrees_with_the_gpu_projection() {
    // The anchor must land where the GPU would put the same world point, or
    // world-anchored UI and world geometry disagree about the screen.
    for (const float aspect : {4.0f / 3.0f, 16.0f / 9.0f, 21.0f / 9.0f}) {
        const camera::Camera cam = camera_at(aspect);
        Projector p;
        p.update(cam, screen());
        const camera::Projection gpu = camera::projection_matrix(cam);

        for (const float guest_x : {100.0f, 320.0f, 500.0f, 639.0f}) {
            const Request a = anchor_from_guest_x(guest_x, 250.0f);
            const Result r = p.project(a);
            // Independently push the same point through the GPU matrix.
            const float view_x = a.world_x, view_z = a.world_z;
            const float w = -view_z;
            const float ndc = (gpu.m00 * view_x + gpu.m02 * view_z) / w;
            check_near(r.ndc_x, ndc, 1e-5,
                       "the anchor NDC equals what the GPU projection produces");
        }
    }
}

void test_stats_attribute_every_outcome() {
    Projector p;
    p.update(camera_at(4.0f / 3.0f), screen());
    Projector::Stats stats;

    stats.record(p.project(anchor_from_guest_x(320.0f, 200.0f)));   // visible
    stats.record(p.project(anchor_from_guest_x(700.0f, 200.0f)));   // off screen
    Request deep = anchor_from_guest_x(320.0f, 50000.0f);
    stats.record(p.project(deep));                                   // camera far
    Request authored = anchor_from_guest_x(320.0f, 300.0f);
    authored.max_depth = 200.0f;
    stats.record(p.project(authored));                               // authored max

    check(stats.projected == 4, "every projection is counted");
    check(stats.visible == 1, "the visible one is counted");
    check(stats.rejected_off_screen == 1, "the off-screen rejection is attributed");
    check(stats.rejected_camera_far == 1, "the far rejection is attributed");
    check(stats.rejected_authored_max == 1, "the authored rejection is attributed separately");
}

}  // namespace

int main() {
    test_4_3_is_the_original_boundary();
    test_captured_fixtures_are_visible_at_16_9();
    test_no_widescreen_term_in_the_visibility_test();
    test_authored_depth_rules_are_exact_and_aspect_independent();
    test_scale_law_is_preserved();
    test_consumer_margin_is_owned_by_the_consumer();
    test_rejection_reasons_are_specific();
    test_content_viewport_is_honoured();
    test_ndc_agrees_with_the_gpu_projection();
    test_stats_attribute_every_outcome();

    if (g_failures != 0) {
        std::printf("world_anchor_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("world_anchor_tests: all checks passed\n");
    return 0;
}
