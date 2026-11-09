// Adversarial mathematical audit of the native camera/visibility system.
//
// This suite exists to disprove, not to confirm. Every check is numeric; no
// expectation is taken from a comment.
//
// THE CENTREPIECE. `test_coverage_equals_culling` transforms sample points by
// the PROJECTION MATRIX and compares clip-space membership against the planes
// produced by `view_frustum()`. Those two are built by completely different
// routes from the same canonical extents - one by the encoder, one
// analytically - so agreement is a real proof that projection coverage and
// culling coverage are the same volume, not a tautology. It is run on nine
// policies including three deliberately off-centre ones, because a centred
// frustum has m02 = m12 = 0 and hides sign and indexing errors.
//
// The other three things it is built to catch:
//   * AABB classification that rejects a box because its SUPPORT vertex is
//     outside a plane. A box is outside a plane only when its MINIMUM
//     signed-distance corner is outside; rejecting on the support vertex culls
//     every box that straddles a plane, including one enclosing the frustum.
//   * A handedness error in the RenderWare LTM decode, which swaps near and far
//     and is invisible on a symmetric frustum.
//   * World-space planes that disagree with view-space planes under a
//     non-trivial camera transform.

#include "gekko/camera.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

using namespace gekko;
using namespace gekko::camera;

Camera measured_camera() {
    return decode::renderware_camera(decode::ViewWindow{0.615808f, 0.542110f}, 1.0f, 20000.13f,
                                     ProjectionType::Perspective, Transform{});
}

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// A point whose signed distance to `plane` is exactly `t`, found by sliding
// `origin` along the plane normal. Sliding along the normal keeps the point
// inside the OTHER five planes, so a test targeting one plane exercises only
// that plane.
Vec3 point_at(const Plane& p, Vec3 origin, float t) {
    const float d = p.signed_distance(origin.x, origin.y, origin.z);
    const float k = t - d;
    return Vec3{origin.x + p.nx * k, origin.y + p.ny * k, origin.z + p.nz * k};
}

Vec3 frustum_interior(const Camera& c) {
    return Vec3{0.0f, 0.0f, -(c.near_plane + c.far_plane) * 0.25f};
}

// "Exactly on the plane" is not representable: sliding to t = 0 from a point
// thousands of units away leaves float noise of order 1e-3. A probe radius
// above that noise is what makes the inclusive-boundary claim testable.
constexpr float kOnPlaneRadius = 1.0e-2f;

const char* visibility_name(Visibility v) {
    switch (v) {
        case Visibility::Inside: return "Inside";
        case Visibility::Intersecting: return "Intersecting";
        case Visibility::Outside: return "Outside";
    }
    return "?";
}

struct NamedPolicy {
    const char* name;
    CameraPolicy policy;
};

std::vector<NamedPolicy> policies() {
    std::vector<NamedPolicy> out;
    auto make = [](const char* name, float aspect, AspectPolicy mode, float ox, float oy) {
        CameraPolicy p;
        p.aspect_policy = mode;
        p.target_aspect = aspect;
        p.extent_offset_x = ox;
        p.extent_offset_y = oy;
        return NamedPolicy{name, p};
    };
    out.push_back(make("4:3 original", 4.0f / 3.0f, AspectPolicy::Original, 0.0f, 0.0f));
    out.push_back(make("4:3 Hor+", 4.0f / 3.0f, AspectPolicy::HorPlus, 0.0f, 0.0f));
    out.push_back(make("16:9 Hor+", 16.0f / 9.0f, AspectPolicy::HorPlus, 0.0f, 0.0f));
    out.push_back(make("21:9 Hor+", 21.0f / 9.0f, AspectPolicy::HorPlus, 0.0f, 0.0f));
    out.push_back(make("32:9 Hor+", 32.0f / 9.0f, AspectPolicy::HorPlus, 0.0f, 0.0f));
    out.push_back(make("16:9 Vert-", 16.0f / 9.0f, AspectPolicy::VertMinus, 0.0f, 0.0f));
    // The off-centre cases: these expose sign and indexing errors.
    out.push_back(make("4:3 offset x", 4.0f / 3.0f, AspectPolicy::Original, 0.21f, 0.0f));
    out.push_back(make("4:3 offset y", 4.0f / 3.0f, AspectPolicy::Original, 0.0f, -0.17f));
    out.push_back(make("16:9 offset xy", 16.0f / 9.0f, AspectPolicy::HorPlus, 0.13f, 0.09f));
    return out;
}

// ---------------------------------------------------------------------------
// The centrepiece
// ---------------------------------------------------------------------------

// Clip-space membership straight from the encoded matrix, with no reference to
// the frustum. GX normalises depth to z_clip/w_clip in [-1, 0].
bool inside_clip_space(const Projection& p, float x, float y, float z) {
    const std::array<float, 16> m = p.matrix();
    const float xc = m[0] * x + m[1] * y + m[2] * z + m[3];
    const float yc = m[4] * x + m[5] * y + m[6] * z + m[7];
    const float zc = m[8] * x + m[9] * y + m[10] * z + m[11];
    const float wc = m[12] * x + m[13] * y + m[14] * z + m[15];
    if (!(wc > 0.0f)) return false;
    return xc >= -wc && xc <= wc && yc >= -wc && yc <= wc && zc >= -wc && zc <= 0.0f;
}

void test_coverage_equals_culling() {
    const Camera decoded = measured_camera();
    for (const NamedPolicy& np : policies()) {
        const Camera c = apply_policy(decoded, np.policy);
        const Projection p = projection_matrix(c);   // the encoder's route
        const Frustum f = view_frustum(c);           // the analytic route
        std::size_t disagreements = 0;
        std::size_t inside_count = 0;
        for (int ix = -14; ix <= 14; ++ix) {
            for (int iy = -10; iy <= 10; ++iy) {
                for (int iz = -2; iz <= 22; ++iz) {
                    const float z = -(static_cast<float>(iz) * 1200.0f);  // +2400 .. -24000
                    const float span = std::fabs(z) + 1.0f;
                    const float x = static_cast<float>(ix) * span * 0.075f;
                    const float y = static_cast<float>(iy) * span * 0.075f;
                    const bool by_clip = inside_clip_space(p, x, y, z);
                    const bool by_frustum = classify_sphere(f, Sphere{x, y, z, 0.0f}).visible();
                    if (by_clip) ++inside_count;
                    if (by_clip != by_frustum) ++disagreements;
                }
            }
        }
        check(inside_count > 200,
              std::string("the lattice reaches inside the frustum for ") + np.name);
        check(disagreements == 0,
              std::string("PROJECTION COVERAGE == CULLING COVERAGE for ") + np.name + " (" +
                  std::to_string(disagreements) + " disagreements)");
    }
}

// ---------------------------------------------------------------------------
// Per-plane geometry
// ---------------------------------------------------------------------------

void test_points_exactly_on_every_plane() {
    const Camera decoded = measured_camera();
    for (const NamedPolicy& np : policies()) {
        const Camera c = apply_policy(decoded, np.policy);
        const Frustum f = view_frustum(c);
        for (u32 i = 0; i < kFrustumPlaneCount; ++i) {
            const Plane& plane = f.planes[i];
            const FrustumPlane which = static_cast<FrustumPlane>(i);
            const Vec3 on = point_at(plane, frustum_interior(c), 0.0f);
            check(std::fabs(plane.signed_distance(on.x, on.y, on.z)) < kOnPlaneRadius,
                  std::string("the probe lands on the ") + plane_name(which) + " plane for " +
                      np.name);
            const VisibilityResult r = classify_sphere(f, Sphere{on.x, on.y, on.z, kOnPlaneRadius});
            const bool rejected_here =
                !r.visible() && r.reason == RejectReason::FrustumPlane && r.plane == which;
            check(!rejected_here, std::string("a point on the ") + plane_name(which) +
                                      " plane is not rejected by it for " + np.name);
            const Vec3 past = point_at(plane, frustum_interior(c), 1.0f);
            const VisibilityResult out =
                classify_sphere(f, Sphere{past.x, past.y, past.z, kOnPlaneRadius});
            check(!out.visible() && out.plane == which,
                  std::string("a point just past the ") + plane_name(which) +
                      " plane is rejected by it for " + np.name);
        }
    }
}

void test_spheres_cross_every_plane() {
    const Camera decoded = measured_camera();
    for (const NamedPolicy& np : policies()) {
        const Camera c = apply_policy(decoded, np.policy);
        const Frustum f = view_frustum(c);
        for (u32 i = 0; i < kFrustumPlaneCount; ++i) {
            const Plane& plane = f.planes[i];
            const FrustumPlane which = static_cast<FrustumPlane>(i);
            const float radius = 1.0f;
            const auto sphere_at = [&](float t) {
                const Vec3 p = point_at(plane, frustum_interior(c), t);
                return Sphere{p.x, p.y, p.z, radius};
            };
            check(!classify_sphere(f, sphere_at(radius + 0.5f)).visible(),
                  std::string("a sphere fully past the ") + plane_name(which) +
                      " plane is rejected for " + np.name);
            const VisibilityResult cross = classify_sphere(f, sphere_at(0.0f));
            check(cross.visible(), std::string("a sphere straddling the ") + plane_name(which) +
                                       " plane is NOT rejected for " + np.name);
            check(cross.visibility == Visibility::Intersecting,
                  std::string("a sphere straddling the ") + plane_name(which) +
                      " plane reports Intersecting for " + np.name);
        }
    }
}

Aabb box_around(float x, float y, float z, float half) {
    return Aabb{x - half, y - half, z - half, x + half, y + half, z + half};
}

void test_aabbs_cross_every_plane() {
    const Camera decoded = measured_camera();
    for (const NamedPolicy& np : policies()) {
        const Camera c = apply_policy(decoded, np.policy);
        const Frustum f = view_frustum(c);
        for (u32 i = 0; i < kFrustumPlaneCount; ++i) {
            const Plane& plane = f.planes[i];
            const FrustumPlane which = static_cast<FrustumPlane>(i);
            const float half = 1.0f;
            const Vec3 on = point_at(plane, frustum_interior(c), 0.0f);
            const VisibilityResult cross = classify_aabb(f, box_around(on.x, on.y, on.z, half));
            check(cross.visible(),
                  std::string("an AABB straddling the ") + plane_name(which) +
                      " plane is NOT rejected for " + np.name + " (got " +
                      visibility_name(cross.visibility) + " via " + plane_name(cross.plane) + ")");
            check(cross.visibility == Visibility::Intersecting,
                  std::string("an AABB straddling the ") + plane_name(which) +
                      " plane reports Intersecting for " + np.name);
            // Past the plane by more than the half diagonal (sqrt(3) * half).
            const Vec3 past = point_at(plane, frustum_interior(c), 4.0f * half);
            check(!classify_aabb(f, box_around(past.x, past.y, past.z, half)).visible(),
                  std::string("an AABB fully past the ") + plane_name(which) +
                      " plane IS rejected for " + np.name);
        }
    }
}

void test_aabb_fully_inside_and_outside() {
    const Camera decoded = measured_camera();
    for (const NamedPolicy& np : policies()) {
        const Camera c = apply_policy(decoded, np.policy);
        const Frustum f = view_frustum(c);
        check(classify_aabb(f, box_around(0.0f, 0.0f, -300.0f, 2.0f)).visibility ==
                  Visibility::Inside,
              std::string("a small axial box is fully Inside for ") + np.name);
        check(!classify_aabb(f, box_around(0.0f, 0.0f, 5000.0f, 2.0f)).visible(),
              std::string("a box far behind the eye is Outside for ") + np.name);
        check(!classify_aabb(f, box_around(0.0f, 0.0f, -500000.0f, 2.0f)).visible(),
              std::string("a box far past the far clip is Outside for ") + np.name);
    }
}

// The cheapest check that catches support-vertex rejection: a box enclosing the
// whole frustum is outside NO plane yet straddles all six.
void test_enormous_enclosing_box_is_never_culled() {
    const Camera decoded = measured_camera();
    for (const NamedPolicy& np : policies()) {
        const Camera c = apply_policy(decoded, np.policy);
        const VisibilityResult r =
            classify_aabb(view_frustum(c), Aabb{-1.0e6f, -1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f, 1.0e6f});
        check(r.visible(), std::string("a box enclosing the whole frustum is visible for ") +
                               np.name + " (got " + visibility_name(r.visibility) + " via " +
                               plane_name(r.plane) + ")");
    }
}

void test_near_far_boundaries() {
    const Camera decoded = measured_camera();
    for (const NamedPolicy& np : policies()) {
        const Camera c = apply_policy(decoded, np.policy);
        const Frustum f = view_frustum(c);
        check(c.near_plane == decoded.near_plane,
              std::string("near survives the policy for ") + np.name);
        check(c.far_plane == decoded.far_plane,
              std::string("far survives the policy for ") + np.name);
        check(classify_sphere(f, Sphere{0.0f, 0.0f, -(c.near_plane + 0.05f), 0.0f}).visible(),
              std::string("a point just past the near plane is visible for ") + np.name);
        check(!classify_sphere(f, Sphere{0.0f, 0.0f, -(c.near_plane * 0.5f), 0.0f}).visible(),
              std::string("a point in front of the near plane is rejected for ") + np.name);
        check(classify_sphere(f, Sphere{0.0f, 0.0f, -(c.far_plane - 1.0f), 0.0f}).visible(),
              std::string("a point just inside the far plane is visible for ") + np.name);
        check(!classify_sphere(f, Sphere{0.0f, 0.0f, -(c.far_plane + 50.0f), 0.0f}).visible(),
              std::string("a point past the far plane is rejected for ") + np.name);
    }
}

// ---------------------------------------------------------------------------
// World space and handedness
// ---------------------------------------------------------------------------

void test_renderware_ltm_handedness() {
    // A camera at the origin looking down world +x, with world +z as up.
    const float right[3] = {0.0f, -1.0f, 0.0f};
    const float up[3] = {0.0f, 0.0f, 1.0f};
    const float at[3] = {1.0f, 0.0f, 0.0f};
    const float pos[3] = {0.0f, 0.0f, 0.0f};
    Camera c = measured_camera();
    c.view_to_world = decode::transform_from_renderware_ltm(right, up, at, pos);

    const Frustum world = world_frustum(c);
    // Something 100 units along +at must be VISIBLE; the same distance behind
    // must not. A handedness error swaps exactly these two.
    check(classify_sphere(world, Sphere{100.0f, 0.0f, 0.0f, 1.0f}).visible(),
          "a point along the RenderWare 'at' vector is in front of the camera");
    const VisibilityResult behind = classify_sphere(world, Sphere{-100.0f, 0.0f, 0.0f, 1.0f});
    check(!behind.visible() && behind.plane == FrustumPlane::Near,
          "a point opposite 'at' is rejected by the NEAR plane, not the far one");
    // And the far plane is genuinely far away along +at.
    check(!classify_sphere(world, Sphere{50000.0f, 0.0f, 0.0f, 1.0f}).visible(),
          "a point past the far clip along 'at' is rejected");
}

// frustum_section: the corners RenderWare's camera stores (System::native_frustum_build writes them with the
// world_frustum planes) lie on the planes they bound - top-left on Left and Top, top-right on Right and Top,
// bottom-right on Right and Bottom, bottom-left on Left and Bottom - and on the near / far plane of their distance,
// under a rotated, translated camera and every policy (off-centre ones included).
void test_frustum_section_lies_on_the_planes() {
    const Camera decoded = measured_camera();
    const float ca = std::cos(0.7f), sa = std::sin(0.7f);
    const float cb = std::cos(-0.4f), sb = std::sin(-0.4f);
    const float r00 = ca, r01 = -sa * cb, r02 = sa * sb;
    const float r10 = sa, r11 = ca * cb, r12 = -ca * sb;
    const float r20 = 0.0f, r21 = sb, r22 = cb;
    const FrustumPlane sides[4][2] = {{FrustumPlane::Left, FrustumPlane::Top},
                                      {FrustumPlane::Right, FrustumPlane::Top},
                                      {FrustumPlane::Right, FrustumPlane::Bottom},
                                      {FrustumPlane::Left, FrustumPlane::Bottom}};
    for (const NamedPolicy& np : policies()) {
        Camera c = apply_policy(decoded, np.policy);
        c.view_to_world.basis = {r00, r01, r02, r10, r11, r12, r20, r21, r22};
        c.view_to_world.origin_x = 1234.5f;
        c.view_to_world.origin_y = -678.25f;
        c.view_to_world.origin_z = 90.125f;
        const Frustum world = world_frustum(c);
        for (const float distance : {c.near_plane, c.far_plane}) {
            const std::array<Corner, 4> corners = frustum_section(c, distance);
            const float tolerance = 4.0e-6f * (distance * 2.0f + 1500.0f);
            const Plane& depth = world.planes[static_cast<u32>(distance == c.near_plane ? FrustumPlane::Near
                                                                                         : FrustumPlane::Far)];
            for (u32 i = 0; i < 4u; ++i) {
                const Corner& p = corners[i];
                for (const FrustumPlane side : sides[i]) {
                    const Plane& s = world.planes[static_cast<u32>(side)];
                    check(std::fabs(s.signed_distance(p[0], p[1], p[2])) <= tolerance,
                          std::string("frustum_section corner ") + std::to_string(i) + " on " + plane_name(side) +
                              " (" + np.name + ", distance " + std::to_string(distance) + ")");
                }
                check(std::fabs(depth.signed_distance(p[0], p[1], p[2])) <= tolerance,
                      std::string("frustum_section corner ") + std::to_string(i) + " on its depth plane (" +
                          np.name + ")");
            }
        }
    }
}

void test_world_frustum_agrees_with_view_frustum() {
    const Camera decoded = measured_camera();
    // A deliberately non-trivial camera: rotated about all three axes and
    // translated far from the origin.
    const float ca = std::cos(0.7f), sa = std::sin(0.7f);
    const float cb = std::cos(-0.4f), sb = std::sin(-0.4f);
    // R = Rz(0.7) * Rx(-0.4), stored column-major.
    const float r00 = ca, r01 = -sa * cb, r02 = sa * sb;
    const float r10 = sa, r11 = ca * cb, r12 = -ca * sb;
    const float r20 = 0.0f, r21 = sb, r22 = cb;

    for (const NamedPolicy& np : policies()) {
        Camera c = apply_policy(decoded, np.policy);
        c.view_to_world.basis = {r00, r01, r02, r10, r11, r12, r20, r21, r22};
        c.view_to_world.origin_x = 1234.5f;
        c.view_to_world.origin_y = -678.25f;
        c.view_to_world.origin_z = 90.125f;

        const Frustum view = view_frustum(c);
        const Frustum world = world_frustum(c);
        std::size_t disagreements = 0;
        for (int ix = -8; ix <= 8; ++ix) {
            for (int iy = -6; iy <= 6; ++iy) {
                for (int iz = 0; iz <= 14; ++iz) {
                    const float z = -(static_cast<float>(iz) * 1500.0f + 0.5f);
                    const float span = std::fabs(z) + 1.0f;
                    const float x = static_cast<float>(ix) * span * 0.11f;
                    const float y = static_cast<float>(iy) * span * 0.11f;
                    // The same point, expressed in world space.
                    const float wx = r00 * x + r01 * y + r02 * z + c.view_to_world.origin_x;
                    const float wy = r10 * x + r11 * y + r12 * z + c.view_to_world.origin_y;
                    const float wz = r20 * x + r21 * y + r22 * z + c.view_to_world.origin_z;
                    const bool in_view = classify_sphere(view, Sphere{x, y, z, 0.0f}).visible();
                    const bool in_world = classify_sphere(world, Sphere{wx, wy, wz, 0.0f}).visible();
                    if (in_view != in_world) ++disagreements;
                }
            }
        }
        check(disagreements == 0,
              std::string("the world frustum agrees with the view frustum for ") + np.name + " (" +
                  std::to_string(disagreements) + " disagreements)");
    }
}

// ---------------------------------------------------------------------------
// Hor+ is a change to a physical extent
// ---------------------------------------------------------------------------

void test_horplus_changes_extents_not_coefficients() {
    const Camera decoded = measured_camera();
    CameraPolicy policy;
    policy.target_aspect = 16.0f / 9.0f;
    const Camera widened = apply_policy(decoded, policy);
    const float k = horizontal_extent_scale(policy.target_aspect, policy.reference_aspect);

    check(std::fabs(widened.extents.right - decoded.extents.right * k) < 1.0e-6f,
          "Hor+ scales the right extent");
    check(std::fabs(widened.extents.left - decoded.extents.left * k) < 1.0e-6f,
          "Hor+ scales the left extent");
    check(widened.extents.top == decoded.extents.top, "Hor+ leaves the top extent bit-identical");
    check(widened.extents.bottom == decoded.extents.bottom,
          "Hor+ leaves the bottom extent bit-identical");
    check(widened.extents.symmetric_shape(),
          "Hor+ of a symmetric frustum stays symmetric, so m02 stays exactly zero");
    check(projection_matrix(widened).m02 == 0.0f, "and the encoded m02 is exactly zero");

    // The coefficient change is a CONSEQUENCE of the extent change, not a patch.
    const Projection before = projection_matrix(decoded);
    const Projection after = projection_matrix(widened);
    check(std::fabs(after.m00 - before.m00 / k) < 1.0e-6f,
          "m00 falls out as the reciprocal of the widened extent");
    check(after.m11 == before.m11, "m11 is untouched");
}

void test_offset_frustum_is_genuinely_off_centre() {
    const Camera decoded = measured_camera();
    CameraPolicy policy;
    policy.aspect_policy = AspectPolicy::Original;
    policy.extent_offset_x = 0.21f;
    const Camera c = apply_policy(decoded, policy);
    check(!c.extents.symmetric_shape(), "an extent offset makes the frustum asymmetric");
    const Projection p = projection_matrix(c);
    check(p.m02 != 0.0f, "an asymmetric frustum encodes a non-zero m02");
    // Round-trip through the decoder to prove the shear sign is self-consistent.
    const std::array<float, 6> coefficients = p.coefficients();
    Camera back;
    check(decode::gx_projection(0u, coefficients.data(), Transform{}, back),
          "an off-centre block decodes");
    check(std::fabs(back.extents.left - c.extents.left) < 1.0e-4f,
          "the off-centre left extent round-trips");
    check(std::fabs(back.extents.right - c.extents.right) < 1.0e-4f,
          "the off-centre right extent round-trips");
}

}  // namespace

int main() {
    test_coverage_equals_culling();
    test_points_exactly_on_every_plane();
    test_spheres_cross_every_plane();
    test_aabbs_cross_every_plane();
    test_aabb_fully_inside_and_outside();
    test_enormous_enclosing_box_is_never_culled();
    test_near_far_boundaries();
    test_renderware_ltm_handedness();
    test_world_frustum_agrees_with_view_frustum();
    test_frustum_section_lies_on_the_planes();
    test_horplus_changes_extents_not_coefficients();
    test_offset_frustum_is_genuinely_off_centre();

    if (g_failures != 0) {
        std::printf("camera_frustum_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("camera_frustum_tests: all checks passed\n");
    return 0;
}
