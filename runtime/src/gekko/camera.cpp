#include "gekko/camera.h"

#include <algorithm>
#include <limits>

namespace gekko::camera {
namespace {

constexpr float kEpsilon = 1.0e-6f;

// The order planes are TESTED in. It does not change whether a renderable is
// rejected - a renderable outside any plane is outside, whatever order the
// planes are visited in - but it decides which plane gets BLAMED, because both
// classifiers stop at the first plane that rejects.
//
// Depth first, on purpose. A point behind the eye is outside all six planes
// (the side planes are infinite and meet at the apex), so testing left-to-right
// would attribute every behind-the-eye rejection to the left plane. Reporting
// "near" there is the specific, actionable answer; "left" is technically true
// and diagnostically useless. Beyond the far plane is the same story.
constexpr std::array<FrustumPlane, kFrustumPlaneCount> kEvaluationOrder = {
    FrustumPlane::Near,  FrustumPlane::Far,    FrustumPlane::Left,
    FrustumPlane::Right, FrustumPlane::Bottom, FrustumPlane::Top,
};

bool finite(float v) noexcept { return std::isfinite(v); }

// Normalise a plane so |normal| == 1, which is what makes `signed_distance`
// comparable against a radius in world units. The original's own frustum
// builder normalises its four side planes with a fast inverse square root for
// exactly this reason.
Plane normalised(float nx, float ny, float nz, float d) noexcept {
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(length > kEpsilon) || !finite(length)) {
        return Plane{nx, ny, nz, d};
    }
    const float inv = 1.0f / length;
    return Plane{nx * inv, ny * inv, nz * inv, d * inv};
}

}  // namespace

// ---------------------------------------------------------------------------
// The canonical camera
// ---------------------------------------------------------------------------

float Camera::horizontal_fov_radians() const noexcept {
    if (type != ProjectionType::Perspective) return 0.0f;
    return std::atan(extents.right) - std::atan(extents.left);
}

float Camera::vertical_fov_radians() const noexcept {
    if (type != ProjectionType::Perspective) return 0.0f;
    return std::atan(extents.top) - std::atan(extents.bottom);
}

// ---------------------------------------------------------------------------
// Derived output 1: the GPU projection matrix
// ---------------------------------------------------------------------------

std::array<float, 16> Projection::matrix() const noexcept {
    std::array<float, 16> m{};
    if (type == ProjectionType::Perspective) {
        // [ m00  0   m02  0 ] [ 0  m11 m12  0 ] [ 0 0 m22 m23 ] [ 0 0 -1 0 ]
        m[0] = m00;
        m[2] = m02;
        m[5] = m11;
        m[6] = m12;
        m[10] = m22;
        m[11] = m23;
        m[14] = -1.0f;
    } else {
        // [ m00  0   0  m02 ] [ 0  m11  0  m12 ] [ 0 0 m22 m23 ] [ 0 0 0 1 ]
        m[0] = m00;
        m[3] = m02;
        m[5] = m11;
        m[7] = m12;
        m[10] = m22;
        m[11] = m23;
        m[15] = 1.0f;
    }
    return m;
}

Projection projection_matrix(const Camera& camera) noexcept {
    Projection p;
    p.type = camera.type;
    const FrustumExtents& e = camera.extents;
    const float width = e.width();
    const float height = e.height();

    if (std::fabs(width) > kEpsilon) {
        p.m00 = 2.0f / width;
        // For symmetric extents (right == -left) this is exactly 0.0f, and m00
        // collapses to 1/right = 1/tan(hFOV/2) - the original's own
        // coefficient, bit for bit.
        const float shear_x = (e.right + e.left) / width;
        p.m02 = camera.type == ProjectionType::Perspective ? shear_x : -shear_x;
    }
    if (std::fabs(height) > kEpsilon) {
        p.m11 = 2.0f / height;
        const float shear_y = (e.top + e.bottom) / height;
        p.m12 = camera.type == ProjectionType::Perspective ? shear_y : -shear_y;
    }

    const float span = camera.far_plane - camera.near_plane;
    if (std::fabs(span) > kEpsilon) {
        p.m22 = camera.type == ProjectionType::Perspective ? -camera.near_plane / span
                                                           : -1.0f / span;
        p.m23 = camera.far_plane * p.m22;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Derived output 2: the culling frustum
// ---------------------------------------------------------------------------

const char* plane_name(FrustumPlane plane) noexcept {
    switch (plane) {
        case FrustumPlane::Left: return "left";
        case FrustumPlane::Right: return "right";
        case FrustumPlane::Bottom: return "bottom";
        case FrustumPlane::Top: return "top";
        case FrustumPlane::Near: return "near";
        case FrustumPlane::Far: return "far";
    }
    return "?";
}

Frustum view_frustum(const Camera& camera) noexcept {
    // Straight from the extents. Never from `Projection`: the matrix is an
    // output encoding, and re-deriving the culling volume from it would make
    // the two agree only as long as the encoding stays invertible.
    const FrustumExtents& e = camera.extents;
    Frustum f;
    const auto set = [&f](FrustumPlane which, Plane p) {
        f.planes[static_cast<u32>(which)] = p;
    };

    if (camera.type == ProjectionType::Perspective) {
        // The four side planes pass through the apex, so d = 0. A point on the
        // left boundary at depth d is (left*d, y, -d); the plane through the
        // origin containing that edge has normal (-1, 0, -left) once oriented
        // outward (check: the interior point (0, 0, -1) gives left < 0).
        set(FrustumPlane::Left, normalised(-1.0f, 0.0f, -e.left, 0.0f));
        set(FrustumPlane::Right, normalised(1.0f, 0.0f, e.right, 0.0f));
        set(FrustumPlane::Bottom, normalised(0.0f, -1.0f, -e.bottom, 0.0f));
        set(FrustumPlane::Top, normalised(0.0f, 1.0f, e.top, 0.0f));
    } else {
        // Orthographic: the sides are an axis-aligned slab in view space, and
        // the extents are world units rather than tangents.
        set(FrustumPlane::Left, Plane{-1.0f, 0.0f, 0.0f, -e.left});
        set(FrustumPlane::Right, Plane{1.0f, 0.0f, 0.0f, e.right});
        set(FrustumPlane::Bottom, Plane{0.0f, -1.0f, 0.0f, -e.bottom});
        set(FrustumPlane::Top, Plane{0.0f, 1.0f, 0.0f, e.top});
    }

    // The camera looks down -z, so the near plane is at z = -near with the
    // outward normal pointing back towards the eye.
    set(FrustumPlane::Near, Plane{0.0f, 0.0f, 1.0f, -camera.near_plane});
    set(FrustumPlane::Far, Plane{0.0f, 0.0f, -1.0f, camera.far_plane});
    return f;
}

Frustum to_world(const Frustum& view_space, const Transform& view_to_world) noexcept {
    const std::array<float, 9>& b = view_to_world.basis;
    Frustum out;
    for (u32 i = 0; i < kFrustumPlaneCount; ++i) {
        const Plane& p = view_space.planes[i];
        // n_world = R * n_view, with R column-major as documented.
        const float nx = b[0] * p.nx + b[1] * p.ny + b[2] * p.nz;
        const float ny = b[3] * p.nx + b[4] * p.ny + b[5] * p.nz;
        const float nz = b[6] * p.nx + b[7] * p.ny + b[8] * p.nz;
        const float d = p.d + (nx * view_to_world.origin_x + ny * view_to_world.origin_y +
                               nz * view_to_world.origin_z);
        out.planes[i] = Plane{nx, ny, nz, d};
    }
    return out;
}

Frustum world_frustum(const Camera& camera) noexcept {
    return to_world(view_frustum(camera), camera.view_to_world);
}

std::array<Corner, 4> frustum_section(const Camera& camera, float distance) noexcept {
    const FrustumExtents& e = camera.extents;
    const bool perspective = camera.type == ProjectionType::Perspective;
    const float scale = perspective ? distance : 1.0f;
    // The view-space corners (x right, y up; the camera looks down -z), then world = R v + origin.
    const float xs[4] = {e.left, e.right, e.right, e.left};
    const float ys[4] = {e.top, e.top, e.bottom, e.bottom};
    const std::array<float, 9>& b = camera.view_to_world.basis;
    const Transform& t = camera.view_to_world;
    std::array<Corner, 4> out{};
    for (u32 i = 0; i < 4u; ++i) {
        const float vx = xs[i] * scale, vy = ys[i] * scale, vz = -distance;
        out[i] = Corner{b[0] * vx + b[1] * vy + b[2] * vz + t.origin_x, b[3] * vx + b[4] * vy + b[5] * vz + t.origin_y,
                        b[6] * vx + b[7] * vy + b[8] * vz + t.origin_z};
    }
    return out;
}

// ---------------------------------------------------------------------------
// Host policy
// ---------------------------------------------------------------------------

float horizontal_extent_scale(float target_aspect, float reference_aspect) noexcept {
    if (!(reference_aspect > 0.1f) || !finite(reference_aspect)) return 1.0f;
    if (!(target_aspect > 0.1f) || !finite(target_aspect)) return 1.0f;
    // Hor+ only ever widens. A target narrower than the reference keeps the
    // original frustum and is pillarboxed by the presenter, because narrowing
    // the horizontal field of view would crop composition the original relies
    // on. The max() is what makes this exactly 1.0f at and below 4:3, and that
    // exactness is what keeps 640x480 byte-identical.
    return std::max(target_aspect, reference_aspect) / reference_aspect;
}

float vertical_extent_scale(float target_aspect, float reference_aspect) noexcept {
    const float widen = horizontal_extent_scale(target_aspect, reference_aspect);
    return widen > 0.0f ? 1.0f / widen : 1.0f;
}

Coverage Coverage::from_aspect(float target_aspect, float reference_aspect) noexcept {
    Coverage c;
    c.extent_scale = horizontal_extent_scale(target_aspect, reference_aspect);
    return c;
}

Coverage Coverage::from_affine_scale(float ax) noexcept {
    Coverage c;
    // ax is the presentation affine scale, <= 1. Its reciprocal is the extent
    // scale. Guard the degenerate cases to exactly 1 so a bad input can never
    // narrow coverage.
    c.extent_scale = (ax > 0.05f && ax <= 1.0f && std::isfinite(ax)) ? 1.0f / ax : 1.0f;
    return c;
}

Camera apply_policy(const Camera& decoded, const CameraPolicy& policy) noexcept {
    Camera c = decoded;

    // 1. Field of view. Applied to the canonical extents first, so "set the
    //    horizontal FOV to X" means X whatever the aspect policy then does.
    //    An override re-centres that axis: it specifies a total angle, which
    //    only has a symmetric reading.
    if (policy.hfov_override_rad > 0.0f) {
        const float half = std::tan(policy.hfov_override_rad * 0.5f);
        c.extents.left = -half;
        c.extents.right = half;
    }
    if (policy.vfov_override_rad > 0.0f) {
        const float half = std::tan(policy.vfov_override_rad * 0.5f);
        c.extents.bottom = -half;
        c.extents.top = half;
    }

    // 2. Clip. Independent of extents and of aspect.
    if (policy.near_override > 0.0f) c.near_plane = policy.near_override;
    if (policy.far_override > 0.0f) c.far_plane = policy.far_override;

    // 3. Aspect, as a change to a PHYSICAL EXTENT. Orthographic cameras are
    //    never touched: the 2D canvas is remapped by the HUD anchor system,
    //    which is a different problem with a different correct answer.
    if (c.type == ProjectionType::Perspective) {
        switch (policy.aspect_policy) {
            case AspectPolicy::Original:
            case AspectPolicy::Anamorphic:
                break;
            case AspectPolicy::HorPlus: {
                const float k =
                    Coverage::from_aspect(policy.target_aspect, policy.reference_aspect)
                        .extent_scale;
                if (k != 1.0f) {
                    c.extents.left *= k;
                    c.extents.right *= k;
                }
                break;
            }
            case AspectPolicy::VertMinus: {
                const float k =
                    vertical_extent_scale(policy.target_aspect, policy.reference_aspect);
                if (k != 1.0f) {
                    c.extents.bottom *= k;
                    c.extents.top *= k;
                }
                break;
            }
        }
    }

    // 4. The host's own off-centre shift, in the same tangent units. Applied
    //    after the aspect policy so it means the same thing at every aspect.
    if (policy.extent_offset_x != 0.0f) {
        c.extents.left += policy.extent_offset_x;
        c.extents.right += policy.extent_offset_x;
    }
    if (policy.extent_offset_y != 0.0f) {
        c.extents.bottom += policy.extent_offset_y;
        c.extents.top += policy.extent_offset_y;
    }
    return c;
}

// ---------------------------------------------------------------------------
// The compatibility boundary
// ---------------------------------------------------------------------------

namespace decode {

Camera renderware_camera(const ViewWindow& view_window, float near_plane, float far_plane,
                         ProjectionType type, const Transform& view_to_world) noexcept {
    Camera c;
    c.type = type;
    // RenderWare's viewWindow is a half-extent pair, so the canonical extents
    // are symmetric. The original never produces anything else: its viewOffset
    // shears the view matrix instead, and that arrives in `view_to_world`.
    c.extents = FrustumExtents::symmetric(view_window.x, view_window.y);
    c.near_plane = near_plane;
    c.far_plane = far_plane;
    c.view_to_world = view_to_world;
    return c;
}

bool gx_projection(u32 type, const float p[6], const Transform& view_to_world,
                   Camera& out) noexcept {
    if (p == nullptr) return false;
    const float m00 = p[0], m02 = p[1], m11 = p[2], m12 = p[3], m22 = p[4], m23 = p[5];
    if (!finite(m00) || !finite(m11) || !finite(m22) || !finite(m23)) return false;
    if (!(std::fabs(m00) > kEpsilon) || !(std::fabs(m11) > kEpsilon)) return false;

    Camera c;
    c.type = type == 0u ? ProjectionType::Perspective : ProjectionType::Orthographic;
    c.view_to_world = view_to_world;

    // Invert the encoding in projection_matrix(). width = 2/m00, and the shear
    // gives the centre: centre = shear * width / 2 = shear / m00.
    const float width = 2.0f / m00;
    const float height = 2.0f / m11;
    const float shear_x = c.type == ProjectionType::Perspective ? m02 : -m02;
    const float shear_y = c.type == ProjectionType::Perspective ? m12 : -m12;
    const float centre_x = shear_x * width * 0.5f;
    const float centre_y = shear_y * height * 0.5f;
    c.extents.left = centre_x - width * 0.5f;
    c.extents.right = centre_x + width * 0.5f;
    c.extents.bottom = centre_y - height * 0.5f;
    c.extents.top = centre_y + height * 0.5f;

    float near_plane = 0.0f, far_plane = 0.0f;
    if (c.type == ProjectionType::Perspective) {
        if (!clip_from_depth_coefficients(m22, m23, near_plane, far_plane)) return false;
    } else {
        // m22 = -1/(far-near), m23 = far*m22.
        if (!(std::fabs(m22) > std::numeric_limits<float>::min())) return false;
        far_plane = m23 / m22;
        near_plane = far_plane + 1.0f / m22;
        if (!finite(near_plane) || !finite(far_plane)) return false;
    }
    c.near_plane = near_plane;
    c.far_plane = far_plane;
    if (!c.extents.valid()) return false;
    out = c;
    return true;
}

Transform transform_from_renderware_ltm(const float right[3], const float up[3], const float at[3],
                                        const float pos[3]) noexcept {
    Transform t;
    // Columns are the world-space directions of view-space +x, +y, +z. The
    // third column is -at, not at: RenderWare's camera looks down +z and the
    // canonical camera looks down -z.
    t.basis[0] = right[0];
    t.basis[3] = right[1];
    t.basis[6] = right[2];
    t.basis[1] = up[0];
    t.basis[4] = up[1];
    t.basis[7] = up[2];
    t.basis[2] = -at[0];
    t.basis[5] = -at[1];
    t.basis[8] = -at[2];
    t.origin_x = pos[0];
    t.origin_y = pos[1];
    t.origin_z = pos[2];
    return t;
}

bool transform_from_world_to_view(const float m[12], Transform& out, float tolerance) noexcept {
    if (m == nullptr || !(tolerance > 0.0f)) return false;
    for (int i = 0; i < 12; ++i) {
        if (!finite(m[i])) return false;
    }
    // Row j of the world-to-view matrix is the world direction of view axis j,
    // so it is column j of the view-to-world basis.
    const float rows[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
    // Refuse anything that is not a rotation. A scaled or sheared matrix still
    // projects, but its `depth` is no longer a world distance, and every
    // authored depth rule downstream is written in world units.
    for (int j = 0; j < 3; ++j) {
        for (int k = j; k < 3; ++k) {
            const float dot =
                rows[j][0] * rows[k][0] + rows[j][1] * rows[k][1] + rows[j][2] * rows[k][2];
            const float expected = (j == k) ? 1.0f : 0.0f;
            if (!(std::fabs(dot - expected) <= tolerance)) return false;
        }
    }
    Transform t;
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            t.basis[static_cast<std::size_t>(i) * 3u + static_cast<std::size_t>(j)] = rows[j][i];
        }
    }
    // view = R*world + translation, so the eye - the world point whose view
    // coordinates are zero - is -R^T * translation.
    const float translation[3] = {m[3], m[7], m[11]};
    float eye[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3; ++i) {
        eye[i] = -(rows[0][i] * translation[0] + rows[1][i] * translation[1] +
                   rows[2][i] * translation[2]);
        if (!finite(eye[i])) return false;
    }
    t.origin_x = eye[0];
    t.origin_y = eye[1];
    t.origin_z = eye[2];
    out = t;
    return true;
}

bool clip_from_depth_coefficients(float m22, float m23, float& near_out,
                                  float& far_out) noexcept {
    if (!(std::fabs(m22) > std::numeric_limits<float>::min()) || !finite(m22)) return false;
    if (!finite(m23)) return false;
    // m22 = -near/(far-near), m23 = far*m22.
    const float far_plane = m23 / m22;
    const float denominator = 1.0f - m22;
    if (!(std::fabs(denominator) > kEpsilon)) return false;
    const float near_plane = -m23 / denominator;
    if (!finite(near_plane) || !finite(far_plane)) return false;
    near_out = near_plane;
    far_out = far_plane;
    return true;
}

}  // namespace decode

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

VisibilityResult classify_sphere(const Frustum& f, const Sphere& s) noexcept {
    VisibilityResult r;
    if (!finite(s.x) || !finite(s.y) || !finite(s.z) || !finite(s.radius) || s.radius < 0.0f) {
        r.visibility = Visibility::Outside;
        r.reason = RejectReason::Degenerate;
        return r;
    }
    bool touching = false;
    for (const FrustumPlane which : kEvaluationOrder) {
        const float distance = f.planes[static_cast<u32>(which)].signed_distance(s.x, s.y, s.z);
        if (distance > s.radius) {
            r.visibility = Visibility::Outside;
            r.reason = RejectReason::FrustumPlane;
            r.plane = which;
            r.margin = distance - s.radius;
            return r;
        }
        if (distance > -s.radius) touching = true;
    }
    r.visibility = touching ? Visibility::Intersecting : Visibility::Inside;
    return r;
}

VisibilityResult classify_aabb(const Frustum& f, const Aabb& b) noexcept {
    VisibilityResult r;
    const bool ok = finite(b.min_x) && finite(b.min_y) && finite(b.min_z) && finite(b.max_x) &&
                    finite(b.max_y) && finite(b.max_z);
    if (!ok || b.max_x < b.min_x || b.max_y < b.min_y || b.max_z < b.min_z) {
        r.visibility = Visibility::Outside;
        r.reason = RejectReason::Degenerate;
        return r;
    }
    bool touching = false;
    for (const FrustumPlane which : kEvaluationOrder) {
        const Plane& p = f.planes[static_cast<u32>(which)];
        // Outside == dot(n, v) - d > 0, so a box is outside a plane only when
        // EVERY corner is, i.e. when the corner that MINIMISES the signed
        // distance is still positive. Rejecting on the maximising corner
        // instead culls every box that merely straddles a plane - including one
        // that encloses the whole frustum.
        const float min_x = p.nx >= 0.0f ? b.min_x : b.max_x;
        const float min_y = p.ny >= 0.0f ? b.min_y : b.max_y;
        const float min_z = p.nz >= 0.0f ? b.min_z : b.max_z;
        const float nearest = p.signed_distance(min_x, min_y, min_z);
        if (nearest > 0.0f) {
            r.visibility = Visibility::Outside;
            r.reason = RejectReason::FrustumPlane;
            r.plane = which;
            r.margin = nearest;
            return r;
        }
        // The maximising corner decides Inside vs Intersecting.
        const float max_x = p.nx >= 0.0f ? b.max_x : b.min_x;
        const float max_y = p.ny >= 0.0f ? b.max_y : b.min_y;
        const float max_z = p.nz >= 0.0f ? b.max_z : b.min_z;
        if (p.signed_distance(max_x, max_y, max_z) > 0.0f) touching = true;
    }
    r.visibility = touching ? Visibility::Intersecting : Visibility::Inside;
    return r;
}

float effect_level_ramp(float distance, const VisibilityBudget& budget, float base) noexcept {
    const float limit = budget.effect_distance;
    if (!(limit > budget.effect_ramp_begin)) return base;
    if (distance <= budget.effect_ramp_begin) return base;
    if (distance >= limit) return -127.0f;
    const float t = (distance - budget.effect_ramp_begin) / (limit - budget.effect_ramp_begin);
    // Runs from `base` at ramp_begin down to exactly -127 at the limit, so the
    // ramp is continuous with the hard reject rather than stepping into it.
    return base - t * (base + 127.0f);
}

VisibilityResult classify(const Frustum& f, const Sphere& s, const VisibilityBudget& budget,
                          float camera_x, float camera_y, float camera_z) noexcept {
    VisibilityResult r = classify_sphere(f, s);
    if (!r.visible()) return r;

    // The distance gates are SEPARATE systems, evaluated after the frustum, so
    // the reported reason is the first subsystem that would have rejected the
    // renderable. Both default to disabled: the original has no world draw
    // distance at all, and the effect limit only applies to effects.
    const bool need_distance = budget.world_distance > 0.0f || budget.effect_distance > 0.0f;
    if (!need_distance) return r;

    const float dx = s.x - camera_x;
    const float dy = s.y - camera_y;
    const float dz = s.z - camera_z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - s.radius;

    if (budget.world_distance > 0.0f && distance > budget.world_distance) {
        r.visibility = Visibility::Outside;
        r.reason = RejectReason::WorldDistance;
        r.margin = distance - budget.world_distance;
        return r;
    }
    if (budget.effect_distance > 0.0f && distance > budget.effect_distance) {
        r.visibility = Visibility::Outside;
        r.reason = RejectReason::EffectDistance;
        r.margin = distance - budget.effect_distance;
        return r;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void VisibilityStats::record(const VisibilityResult& r) noexcept {
    ++tested;
    switch (r.visibility) {
        case Visibility::Inside: ++inside; return;
        case Visibility::Intersecting: ++intersecting; return;
        case Visibility::Outside: break;
    }
    ++rejected;
    switch (r.reason) {
        case RejectReason::FrustumPlane: {
            const u32 index = static_cast<u32>(r.plane);
            if (index < kFrustumPlaneCount) {
                ++rejected_by_plane[index];
                // The smallest margin is how close the nearest rejected
                // renderable came to surviving: the boundary itself, in world
                // units, rather than an inferred distance.
                if (closest_rejection_margin[index] <= 0.0f ||
                    r.margin < closest_rejection_margin[index]) {
                    closest_rejection_margin[index] = r.margin;
                }
            }
            break;
        }
        case RejectReason::WorldDistance: ++rejected_world_distance; break;
        case RejectReason::EffectDistance: ++rejected_effect_distance; break;
        case RejectReason::Degenerate: ++rejected_degenerate; break;
        case RejectReason::None: break;
    }
}

// ---------------------------------------------------------------------------
// The native visibility system
// ---------------------------------------------------------------------------

void VisibilitySystem::update(const Camera& decoded, const CameraPolicy& policy) noexcept {
    // One camera. The matrix that will be uploaded and the planes that will
    // decide visibility are both derived from its extents, so they cannot
    // describe different volumes.
    camera_ = apply_policy(decoded, policy);
    projection_ = projection_matrix(camera_);
    view_frustum_ = view_frustum(camera_);
    world_frustum_ = to_world(view_frustum_, camera_.view_to_world);
}

VisibilityResult VisibilitySystem::test_sphere(const Sphere& world_sphere) noexcept {
    const VisibilityResult r = classify_sphere(world_frustum_, world_sphere);
    stats_.record(r);
    return r;
}

VisibilityResult VisibilitySystem::test_aabb(const Aabb& world_box) noexcept {
    const VisibilityResult r = classify_aabb(world_frustum_, world_box);
    stats_.record(r);
    return r;
}

VisibilityResult VisibilitySystem::test_sphere(const Sphere& world_sphere,
                                               const VisibilityBudget& budget) noexcept {
    const VisibilityResult r =
        classify(world_frustum_, world_sphere, budget, camera_.view_to_world.origin_x,
                 camera_.view_to_world.origin_y, camera_.view_to_world.origin_z);
    stats_.record(r);
    return r;
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

PixelViewport scale_viewport(const PixelViewport& guest, u32 scale) noexcept {
    const s32 f = static_cast<s32>(scale == 0u ? 1u : scale);
    PixelViewport out = guest;
    out.x = guest.x * f;
    out.y = guest.y * f;
    out.width = guest.width * f;
    out.height = guest.height * f;
    // Depth range is NOT scaled: it is normalised already, and an internal
    // scale is a density multiplier on pixels only.
    return out;
}

PixelScissor scale_scissor(const PixelScissor& guest, u32 scale) noexcept {
    const s32 f = static_cast<s32>(scale == 0u ? 1u : scale);
    PixelScissor out;
    // An integer scale maps a guest pixel boundary onto a native pixel
    // boundary, so no scissor edge lands mid-pixel and no two adjacent guest
    // regions can round onto the same native column.
    out.x = guest.x * f;
    out.y = guest.y * f;
    out.width = guest.width * f;
    out.height = guest.height * f;
    return out;
}

}  // namespace gekko::camera
