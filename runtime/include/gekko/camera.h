#pragma once

// The native camera model: the single source of truth for the renderable area.
//
// ARCHITECTURE
//
// The camera is a set of PHYSICAL parameters - an asymmetric frustum expressed
// as signed tangent extents, a near plane, a far plane, a projection type and a
// view-to-world transform. Everything else is derived from it:
//
//        Camera (canonical: extents, near, far, type, transform)
//           |                                  |
//           v                                  v
//   projection_matrix()                  view_frustum()
//   an OUTPUT FORMAT for the GPU         the culling volume
//                                              |
//                                              v
//                                        world_frustum()
//
// The projection matrix is a downstream encoding, not the model. The culling
// frustum is NEVER derived back from it: both come from the same extents, so
// projection coverage and culling coverage are identical by construction rather
// than by agreement. There is deliberately no function in this header that
// turns a matrix back into planes.
//
// GX AND RENDERWARE ARE INPUT DECODERS ONLY. The original's camera state
// (RenderWare `viewWindow` tangent pairs, GX `GXSetProjectionv` blocks) is
// decoded into the canonical model at the compatibility boundary in
// `namespace decode`, and GX semantics stop there. Nothing downstream of the
// decoder knows what a `viewWindow` or an XF 0x1020 block is.
//
// Before this header the renderable area had no owner. Hor+ was implemented
// four times - `display.cpp:102`, `renderer.cpp:618`, an inline
// `(4.0f/3.0f)/content_aspect` in `native_gpu.cpp:1860` on the DEFAULT path,
// and a dead pair in `render_geometry.h:405` with zero callers - each patching
// `m00` after the matrix already existed, and the native renderer carried no
// frustum, cull, near/far or distance counter at all.
//
// Specification and evidence: docs/visibility-2026-09-21/ORIGINAL_BEHAVIOUR.md
// and MEASUREMENTS.md. The behavioural facts that shaped this file:
//
//   * The original culls whole atomics against its own six planes BEFORE
//     emitting any GX draw (`_rwCameraFrustumBuild` 0x804862DC ->
//     `RwCamera+0x94`, tested by 0x80486718). Measured: it rejects the same
//     30,312 of 133,564 spheres at 4:3, 16:9, 21:9 and 32:9, so a host frustum
//     evaluated after submission can never recover what it dropped. That is why
//     `VisibilitySystem` below is a replacement for that decision, not a second
//     pass behind it.
//   * RenderWare stores coverage as `viewWindow = (tan(hFOV/2), tan(vFOV/2))`
//     with the FOV pair in RADIANS (`fn_8000F860`). That is a SYMMETRIC special
//     case of the canonical extents, and the decoder widens it to the general
//     form rather than the model narrowing to RenderWare's.
//   * The original's projection is `m00 = 1/viewWindow.x`, `m11 =
//     1/viewWindow.y`, `m02 = m12 = 0`, `m22 = -near/(far-near)`, `m23 =
//     far*m22` (CAMERABEGINUPDATE 0x804956F4, verified against 0x8039A158).
//     `RwCamera.viewOffset` is applied by shearing the VIEW matrix, never the
//     projection, so a decoded original camera always has symmetric extents.
//   * Hor+'s reference is the 4/3 DISPLAY aspect, not the game's own 1.3195877
//     PROJECTION aspect. Hor+ cancels a PRESENTATION stretch: horizontal
//     physical extent is (W'/2)*s*m00 with W' = H'*T, i.e. (H'/2)*m00*(4/3),
//     independent of T, with the projection aspect cancelling entirely.
//   * The far clip is NOT a constant: it is a per-BSP-zone f32 from
//     `stgNNNN.fog +0x14` (1500..30000), averaged and smoothed 0.98/0.02 onto
//     `RwCamera+0x84`, seeded from the camera's current value. Live in-stage it
//     is 20000.13, not the 30000.0 setup value. `FarClipFilter` reproduces it.
//   * There is NO geometry LOD system and NO world draw distance in this game
//     (proven four ways). The only real distance gate is the effect spawner's
//     launch-info limit, so `VisibilityBudget::world_distance` defaults to
//     disabled and nothing here invents an LOD ladder.

#include <array>
#include <cmath>
#include <cstdint>

#include "gekko/types.h"

namespace gekko::camera {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// The DISPLAY aspect the original composes for. Deliberately not the game's own
// projection aspect 1.3195877 (guest float at 0x805EBA30).
inline constexpr float kReferenceAspect = 4.0f / 3.0f;

// The original's compiled default FOV pair, radians, as parked at 0x805EF048 /
// 0x805EF04C by the deg2rad of 63.25014877 / 53.13009644 degrees.
inline constexpr float kDefaultGuestHFovRad = 1.1039233f;
inline constexpr float kDefaultGuestVFovRad = 0.9272951f;

// The original's live horizontal FOV, measured from a Dolphin memory dump at a
// matched camera pose. Exactly kDefaultGuestHFovRad * 1.140625 to the bit.
inline constexpr float kOriginalLiveHFovRad = 1.2591625f;

// The camera raster the original creates and never changes (`fn_80272FA8`
// stores the immediates 640 and 480; one writer, three readers in the whole
// DOL). A guest constant: it must never scale.
inline constexpr u32 kCameraRasterWidth = 640;
inline constexpr u32 kCameraRasterHeight = 480;

enum class ProjectionType : u32 { Perspective = 0, Orthographic = 1 };

// ---------------------------------------------------------------------------
// The canonical camera
// ---------------------------------------------------------------------------

// The frustum's physical shape, as SIGNED extents on the near-plane-normalised
// image plane.
//
// Perspective: tangents at unit distance, so at depth d the visible x range is
// [left*d, right*d]. `left` and `bottom` are normally negative.
// Orthographic: world units directly.
//
// Asymmetric by construction. The original only ever produces symmetric
// extents, but storing the general form is what lets an off-centre projection,
// a tiled render or a stereo pair exist without a second code path - and it is
// what makes Hor+ a change to a physical extent rather than a patch applied to
// a finished matrix.
struct FrustumExtents {
    float left = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float top = 0.0f;

    static constexpr FrustumExtents symmetric(float horizontal, float vertical) noexcept {
        return FrustumExtents{-horizontal, horizontal, -vertical, vertical};
    }

    constexpr float width() const noexcept { return right - left; }
    constexpr float height() const noexcept { return top - bottom; }
    constexpr float horizontal_tangent() const noexcept { return 0.5f * width(); }
    constexpr float vertical_tangent() const noexcept { return 0.5f * height(); }
    constexpr float centre_x() const noexcept { return 0.5f * (right + left); }
    constexpr float centre_y() const noexcept { return 0.5f * (top + bottom); }
    constexpr bool symmetric_shape() const noexcept {
        return centre_x() == 0.0f && centre_y() == 0.0f;
    }
    constexpr bool valid() const noexcept { return width() > 0.0f && height() > 0.0f; }
    // The shape's own aspect. NOT the display aspect, and not interchangeable
    // with it: in-stage the original's is ~1.3438 and the port's is 1.135946.
    constexpr float aspect() const noexcept { return height() > 0.0f ? width() / height() : 0.0f; }
};

// A rigid view-to-world transform: an orthonormal basis and an origin.
// `basis` is column-major - columns 0, 1, 2 are the world-space directions of
// view-space +x, +y and +z.
struct Transform {
    std::array<float, 9> basis{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    float origin_x = 0.0f, origin_y = 0.0f, origin_z = 0.0f;
};

// THE camera model. Everything that decides the renderable area lives here and
// nowhere else.
struct Camera {
    ProjectionType type = ProjectionType::Perspective;
    FrustumExtents extents{};
    float near_plane = 1.0f;
    float far_plane = 20000.13f;
    Transform view_to_world{};

    float horizontal_fov_radians() const noexcept;
    float vertical_fov_radians() const noexcept;
};

// ---------------------------------------------------------------------------
// Derived output 1: the GPU projection matrix
// ---------------------------------------------------------------------------

// The matrix, in the packing the GX block and the native D3D12 constant buffer
// both use. This is an OUTPUT FORMAT. Nothing derives a camera or a frustum
// from it.
//
//   perspective   [ m00  0   m02  0  ][ 0  m11 m12  0 ][ 0 0 m22 m23 ][ 0 0 -1 0 ]
//   orthographic  [ m00  0    0  m02 ][ 0  m11  0  m12 ][ 0 0 m22 m23 ][ 0 0  0 1 ]
struct Projection {
    ProjectionType type = ProjectionType::Perspective;
    float m00 = 1.0f;
    float m02 = 0.0f;
    float m11 = 1.0f;
    float m12 = 0.0f;
    float m22 = 0.0f;
    float m23 = 0.0f;

    std::array<float, 6> coefficients() const noexcept { return {m00, m02, m11, m12, m22, m23}; }
    std::array<float, 16> matrix() const noexcept;
};

// Encode the camera for the GPU.
//   perspective:  m00 = 2/(r-l), m02 =  (r+l)/(r-l)
//                 m11 = 2/(t-b), m12 =  (t+b)/(t-b)
//                 m22 = -near/(far-near), m23 = far*m22
//   orthographic: m00 = 2/(r-l), m02 = -(r+l)/(r-l)
//                 m11 = 2/(t-b), m12 = -(t+b)/(t-b)
//                 m22 = -1/(far-near),    m23 = far*m22
// For the symmetric extents the original always produces, m00 collapses to
// 1/tan(hFOV/2) and m02 to exactly 0 - which is what keeps 4:3 byte-identical.
Projection projection_matrix(const Camera& camera) noexcept;

// ---------------------------------------------------------------------------
// Derived output 2: the culling frustum
// ---------------------------------------------------------------------------

// Plane in the convention the original uses: a point is OUTSIDE when
// dot(normal, p) - distance > radius.
struct Plane {
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    float d = 0.0f;

    constexpr float signed_distance(float x, float y, float z) const noexcept {
        return nx * x + ny * y + nz * z - d;
    }
};

enum class FrustumPlane : u32 { Left = 0, Right = 1, Bottom = 2, Top = 3, Near = 4, Far = 5 };
inline constexpr u32 kFrustumPlaneCount = 6;
const char* plane_name(FrustumPlane plane) noexcept;

struct Frustum {
    std::array<Plane, kFrustumPlaneCount> planes{};
};

// The six planes, ANALYTICALLY, from the same extents the matrix was built
// from. For a perspective camera the four side planes pass through the apex
// (d = 0) and their normals come straight from the extents:
//
//   left   n = (-1, 0, -left  )        right  n = ( 1, 0,  right )
//   bottom n = ( 0,-1, -bottom)        top    n = ( 0, 1,  top   )
//   near   n = ( 0, 0,  1), d = -near  far    n = ( 0, 0, -1), d = far
//
// all normalised. For an orthographic camera the side planes are axis-aligned
// slabs instead. Derived from the camera, never from `Projection`.
Frustum view_frustum(const Camera& camera) noexcept;

// The same planes in world space, so world-space bounds can be tested without
// transforming every bound into view space. Six planes once per camera sync
// instead of one transform per renderable.
//
//   n_world = R * n_view ,  d_world = d_view + dot(n_world, t)
Frustum world_frustum(const Camera& camera) noexcept;
Frustum to_world(const Frustum& view_space, const Transform& view_to_world) noexcept;

// The frustum's cross-section at a forward distance, in world space: its corners top-left, top-right, bottom-right,
// bottom-left as the picture shows them (x right, y up). From the same extents as the planes, so each corner lies on
// two side planes. Perspective: the extents are tangents (the section scales with the distance); orthographic: world
// units.
using Corner = std::array<float, 3>;
std::array<Corner, 4> frustum_section(const Camera& camera, float distance) noexcept;

// ---------------------------------------------------------------------------
// Host policy
// ---------------------------------------------------------------------------

enum class AspectPolicy : u32 {
    // The decoded camera, untouched. The 4:3 validation reference.
    Original = 0,
    // Widen the horizontal extents by exactly what presentation stretches.
    // Vertical composition untouched. Only ever widens.
    HorPlus = 1,
    // Narrow the vertical extents instead: preserves horizontal framing and
    // crops the original's top and bottom.
    VertMinus = 2,
    // Leave the extents alone and let presentation stretch them. Geometrically
    // wrong; offered only so the defect can be reproduced for comparison.
    Anamorphic = 3,
};

struct CameraPolicy {
    AspectPolicy aspect_policy = AspectPolicy::HorPlus;
    float target_aspect = kReferenceAspect;     // the DISPLAY aspect presented into
    float reference_aspect = kReferenceAspect;  // what the content was composed for

    // Independent field-of-view control, radians. 0 = keep the decoded value.
    // Applied to the canonical extents before the aspect policy, so an override
    // and a policy compose instead of fighting.
    float hfov_override_rad = 0.0f;
    float vfov_override_rad = 0.0f;

    // Independent clip control, world units. 0 = keep the decoded value.
    float near_override = 0.0f;
    float far_override = 0.0f;

    // Shift the frustum off-centre, in the same tangent units as the extents.
    // A NEW capability: the original always shears its VIEW matrix instead, so
    // a decoded original camera is always symmetric. Default 0 reproduces it.
    float extent_offset_x = 0.0f;
    float extent_offset_y = 0.0f;

    constexpr bool overrides_fov() const noexcept {
        return hfov_override_rad > 0.0f || vfov_override_rad > 0.0f;
    }
};

// ---------------------------------------------------------------------------
// Canonical camera coverage
// ---------------------------------------------------------------------------

// THE single source of truth for "how much wider than 4:3 is the displayed
// camera", for every consumer that depends on it.
//
//                        Coverage
//                           |
//         +-----------------+-----------------+------------------+
//         |                 |                 |                  |
//   framebuffer        NDC visible        world frustum      HUD affine
//   visible bounds     extent             extents            parameters
//
// Before this, five guest subsystems and three host sites each decided
// horizontal coverage for themselves. The guest ones all project a world point
// and then test it against a FIXED 4:3 rectangle or a fixed +/-1 NDC bound,
// while the displayed image is Hor+ widened - so a 3D-anchored sprite vanishes
// while the object it belongs to is still on screen. Measured on Final Haunt at
// 16:9: 2,518 of 8,724 enemy HP gauges that passed every authored depth rule
// were then rejected by a 640-wide clip, with the enemy still 72-78 px from the
// edge of a 640-wide display.
//
// No subsystem may contain its own 4:3-to-widescreen arithmetic. Everything
// comes from here.
struct Coverage {
    // The factor applied to the horizontal tangent extents. Always >= 1: Hor+
    // only widens, so a target narrower than the reference keeps the original
    // frustum and is pillarboxed. EXACTLY 1.0f at and below the reference,
    // which is what keeps 4:3 byte-identical everywhere downstream.
    float extent_scale = 1.0f;

    static Coverage from_aspect(float target_aspect,
                                float reference_aspect = kReferenceAspect) noexcept;
    // From the presentation affine scale (what layout_hud and the legacy
    // projection patch call `ax` / `horplus`). ax == 1/extent_scale.
    static Coverage from_affine_scale(float ax) noexcept;

    constexpr bool native() const noexcept { return extent_scale == 1.0f; }

    // The horizontal affine scale the HUD remap and the legacy m00 patch use.
    // Exactly 1.0f when native.
    constexpr float affine_scale() const noexcept {
        return extent_scale > 0.0f ? 1.0f / extent_scale : 1.0f;
    }

    // FRAMEBUFFER-SPACE horizontal visible bounds, in the guest's own canvas
    // coordinates. This is what a subsystem that projects to guest pixels and
    // then clips must test against, INSTEAD of the fixed [0, width] rectangle.
    //
    //   left  = c - c*extent_scale ,  right = c + c*extent_scale   (c = width/2)
    //
    // which is the displayed screen inverse-mapped through the very affine
    // layout_hud already applies, x' = c + ax*(x - c). At 4:3 extent_scale is
    // exactly 1 and these are exactly [0, width] - the original rectangle, bit
    // for bit.
    //
    // The position is NOT corrected here and must not be: layout_hud applies
    // the Hor+ affine exactly once already. This only moves the rejection
    // boundary onto the coverage that is actually displayed.
    constexpr float framebuffer_left(float canvas_width) const noexcept {
        return 0.5f * canvas_width * (1.0f - extent_scale);
    }
    constexpr float framebuffer_right(float canvas_width) const noexcept {
        return 0.5f * canvas_width * (1.0f + extent_scale);
    }

    // NDC-space horizontal acceptance: a consumer that tests |x_ndc| <= 1.0f
    // must test |x_ndc| <= ndc_extent() instead. Same number, different space -
    // NOT a second derivation.
    constexpr float ndc_extent() const noexcept { return extent_scale; }

    // VERTICAL coverage is untouched by Hor+ and must stay untouched. Present
    // so a consumer can say so explicitly rather than by omission.
    constexpr float framebuffer_top(float) const noexcept { return 0.0f; }
    constexpr float framebuffer_bottom(float canvas_height) const noexcept {
        return canvas_height;
    }
    constexpr float ndc_extent_vertical() const noexcept { return 1.0f; }
};

// The factor Hor+ applies to the horizontal EXTENTS. Always >= 1: Hor+ only
// widens, so a target narrower than the reference keeps the original frustum
// and is pillarboxed by the presenter. Exactly 1.0f at and below the reference,
// which is what keeps 4:3 byte-identical.
//
// Prefer Coverage::from_aspect(). This remains the primitive it is built on.
float horizontal_extent_scale(float target_aspect, float reference_aspect) noexcept;
// The Vert- dual, applied to the vertical extents. Always <= 1.
float vertical_extent_scale(float target_aspect, float reference_aspect) noexcept;

// Apply the policy to the canonical camera. This is where Hor+ lives: it
// changes a physical extent, and the matrix and the frustum both follow from
// the changed extent. Nothing patches a coefficient afterwards.
Camera apply_policy(const Camera& decoded, const CameraPolicy& policy) noexcept;

// ---------------------------------------------------------------------------
// The compatibility boundary: legacy camera state in, canonical camera out
// ---------------------------------------------------------------------------

namespace decode {

// RenderWare's coverage representation: tan(hFOV/2), tan(vFOV/2). A SYMMETRIC
// special case of FrustumExtents.
struct ViewWindow {
    float x = 0.0f;
    float y = 0.0f;
};

// `RwCamera+0x68/+0x6C` plus near/far plus the frame's local-to-world.
//
// `RwCamera.viewOffset` is deliberately absent: RenderWare applies it by
// shearing the VIEW matrix (the S concatenated into V at CAMERABEGINUPDATE),
// so it arrives inside `view_to_world` and must not also become an extent
// offset. Passing it twice was a real defect in an earlier draft of this file.
Camera renderware_camera(const ViewWindow& view_window, float near_plane, float far_plane,
                         ProjectionType type, const Transform& view_to_world) noexcept;

// A raw `GXSetProjectionv` block, as the guest programs XF 0x1020 - the last
// place in this codebase that knows what those six floats mean.
// Returns false if the block does not describe a usable frustum.
bool gx_projection(u32 type, const float p[6], const Transform& view_to_world,
                   Camera& out) noexcept;

// The view-to-world transform from a RenderWare frame's local-to-world vectors.
//
// HANDEDNESS: RenderWare's camera looks along its frame's `at` vector, down +z
// in camera space. The canonical camera looks down -z (its matrix's bottom row
// is [0 0 -1 0], so w = -z). The two differ by a z flip, so view-space +z maps
// to world -at. Getting this wrong swaps near and far and is invisible on a
// symmetric frustum, which is why it is a named function with its own test.
Transform transform_from_renderware_ltm(const float right[3], const float up[3],
                                        const float at[3], const float pos[3]) noexcept;

// A row-major 3x4 world-to-view matrix, the form the game's own camera object
// stores and the form its world-to-screen projection consumes:
//
//     view.x = m[0]*w.x + m[1]*w.y + m[2]*w.z  + m[3]
//     view.y = m[4]*w.x + m[5]*w.y + m[6]*w.z  + m[7]
//     view.z = m[8]*w.x + m[9]*w.y + m[10]*w.z + m[11]
//
// This is the EXACT route, and it exists so that nothing has to recover a
// camera archaeologically. The rows are the view basis and the translation
// pins the eye (`eye = -R^T t`), so there is no search, no tolerance, no
// handedness guess and no sign convention to get wrong - the three questions
// that a frustum-plane reconstruction has to answer by inference, this answers
// by reading.
//
// The rows must be orthonormal to within `tolerance`; a matrix carrying a scale
// or a shear is refused rather than silently producing a camera whose depth is
// off by that factor. Returns false on a refusal, leaving `out` untouched.
bool transform_from_world_to_view(const float m[12], Transform& out,
                                  float tolerance = 1.0e-3f) noexcept;

// Recover near and far from a perspective block's depth coefficients, for
// checking a captured projection against a camera without trusting either:
//   far = m23/m22 , near = -m23/(1 - m22)
bool clip_from_depth_coefficients(float m22, float m23, float& near_out,
                                  float& far_out) noexcept;

}  // namespace decode

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

enum class Visibility : u32 { Inside = 0, Intersecting = 1, Outside = 2 };

enum class RejectReason : u32 {
    None = 0,
    FrustumPlane,   // a specific plane; see `plane`
    WorldDistance,  // host world visibility distance
    EffectDistance, // the original's effect launch-info limit, reproduced
    Degenerate,     // non-finite or negative-radius bounds
};

struct VisibilityResult {
    Visibility visibility = Visibility::Inside;
    RejectReason reason = RejectReason::None;
    FrustumPlane plane = FrustumPlane::Left;  // meaningful iff reason == FrustumPlane
    // How far outside the rejecting plane, in world units. Zero when accepted.
    // This is what makes "the exact point where the object becomes rejected"
    // measurable rather than bisected.
    float margin = 0.0f;

    constexpr bool visible() const noexcept { return visibility != Visibility::Outside; }
};

struct Sphere {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float radius = 0.0f;
};

struct Aabb {
    float min_x = 0.0f, min_y = 0.0f, min_z = 0.0f;
    float max_x = 0.0f, max_y = 0.0f, max_z = 0.0f;
};

// Reject on the first plane for which dot(n, centre) - d > radius.
VisibilityResult classify_sphere(const Frustum& f, const Sphere& s) noexcept;

// A box is outside a plane only when EVERY corner is, i.e. when the corner that
// MINIMISES the signed distance is still positive. Rejecting on the maximising
// corner instead would cull every box that merely straddles a plane - including
// one that encloses the whole frustum.
VisibilityResult classify_aabb(const Frustum& f, const Aabb& b) noexcept;

// Independent visibility distances. Each is separately switchable because the
// original keeps them separate; `world_distance` does not exist in the original
// at all and defaults to disabled so the default configuration adds no
// boundary.
struct VisibilityBudget {
    float world_distance = 0.0f;
    // limit = (launch_info_0x0C == 0 ? 400.0 : launch_info_0x0C * 100.0)
    float effect_distance = 0.0f;
    float effect_ramp_begin = 60.0f;

    static constexpr float effect_limit_from_launch_info(u32 byte_0x0C) noexcept {
        return byte_0x0C == 0u ? 400.0f : static_cast<float>(byte_0x0C) * 100.0f;
    }
};

// The original's per-frame effect level ramp (`fn_801D979C`): the signed level
// runs from `base` at `ramp_begin` down to exactly -127 at `limit`.
float effect_level_ramp(float distance, const VisibilityBudget& budget, float base) noexcept;

// Frustum first, then the independent distance gates, so the reported reason is
// the FIRST subsystem that would have rejected the renderable.
VisibilityResult classify(const Frustum& f, const Sphere& s, const VisibilityBudget& budget,
                          float camera_x, float camera_y, float camera_z) noexcept;

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

struct VisibilityStats {
    u64 tested = 0;
    u64 inside = 0;
    u64 intersecting = 0;
    u64 rejected = 0;
    std::array<u64, kFrustumPlaneCount> rejected_by_plane{};
    u64 rejected_world_distance = 0;
    u64 rejected_effect_distance = 0;
    u64 rejected_degenerate = 0;
    // The closest any rejected renderable came to surviving, per plane, in
    // world units: the boundary itself, measured rather than inferred.
    std::array<float, kFrustumPlaneCount> closest_rejection_margin{};

    void reset() noexcept { *this = VisibilityStats{}; }
    void record(const VisibilityResult& r) noexcept;
};

// ---------------------------------------------------------------------------
// The native visibility system
// ---------------------------------------------------------------------------

// The one authoritative visibility decision.
//
// A REPLACEMENT for the original's per-atomic frustum test, not an observer of
// it and not a second pass behind it. `update()` solves the camera once;
// `projection()` is what gets uploaded and `test_sphere()` is what decides
// visibility. Both read the one `Camera`, and the frustum is built from its
// extents rather than from the matrix, so there is no path by which coverage
// and culling can describe different volumes.
//
// No allocation, no globals, no device. One instance per camera.
class VisibilitySystem {
public:
    // Re-solve for a new camera state or a new policy. Once per camera sync,
    // never per renderable.
    void update(const Camera& decoded, const CameraPolicy& policy) noexcept;

    const Camera& camera() const noexcept { return camera_; }
    const Projection& projection() const noexcept { return projection_; }
    const Frustum& view_planes() const noexcept { return view_frustum_; }
    const Frustum& world_planes() const noexcept { return world_frustum_; }

    // THE decision, in world space.
    VisibilityResult test_sphere(const Sphere& world_sphere) noexcept;
    VisibilityResult test_aabb(const Aabb& world_box) noexcept;
    VisibilityResult test_sphere(const Sphere& world_sphere,
                                 const VisibilityBudget& budget) noexcept;

    void set_budget(const VisibilityBudget& budget) noexcept { budget_ = budget; }
    const VisibilityBudget& budget() const noexcept { return budget_; }

    const VisibilityStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_.reset(); }

    float eye_x() const noexcept { return camera_.view_to_world.origin_x; }
    float eye_y() const noexcept { return camera_.view_to_world.origin_y; }
    float eye_z() const noexcept { return camera_.view_to_world.origin_z; }

private:
    Camera camera_{};
    Projection projection_{};
    Frustum view_frustum_{};
    Frustum world_frustum_{};
    VisibilityBudget budget_{};
    VisibilityStats stats_{};
};

// ---------------------------------------------------------------------------
// Far-clip filter (the original's fog-driven law)
// ---------------------------------------------------------------------------

// `stgNNNN.fog` carries a per-BSP-zone far value at +0x14 (measured 1500..30000).
// The active zones are averaged and smoothed onto the camera with a 0.98/0.02
// per-frame filter SEEDED FROM THE CAMERA'S CURRENT VALUE, not from zero.
struct FarClipFilter {
    static constexpr float kRetain = 0.98f;
    static constexpr float kAdmit = 0.02f;
    // The reset seeds, from [0x805F8F24] / [0x805F8F20].
    static constexpr float kResetNear = 1.0f;
    static constexpr float kResetFar = 30000.0f;

    float near_plane = kResetNear;
    float far_plane = kResetFar;

    void reset() noexcept {
        near_plane = kResetNear;
        far_plane = kResetFar;
    }
    void advance(float zone_mean_near, float zone_mean_far) noexcept {
        near_plane = kRetain * near_plane + kAdmit * zone_mean_near;
        far_plane = kRetain * far_plane + kAdmit * zone_mean_far;
    }
};

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

struct PixelViewport {
    s32 x = 0, y = 0, width = 0, height = 0;
    float min_depth = 0.0f, max_depth = 1.0f;
};

struct PixelScissor {
    s32 x = 0, y = 0, width = 0, height = 0;
};

// The original's own rectangles scaled to the native targets. `scale` is the
// integer internal render scale and appears NOWHERE else in this header: it is
// a pure density multiplier and must never reach a camera, an extent, a plane
// or a world-space distance.
PixelViewport scale_viewport(const PixelViewport& guest, u32 scale) noexcept;
PixelScissor scale_scissor(const PixelScissor& guest, u32 scale) noexcept;

}  // namespace gekko::camera
