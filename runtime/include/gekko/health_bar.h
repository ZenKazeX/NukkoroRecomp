#pragma once

// The enemy health bar, rebuilt.
//
// ---------------------------------------------------------------------------
// WHAT THIS REPLACES
// ---------------------------------------------------------------------------
//
// The original layer (GiHPGaugeEnemyImpl, guest 0x800BF634 / 0x800BF9D8) did
// this, once per gauge per frame:
//
//     world anchor
//       -> a game-camera projection (0x8000F654) that is NOT the one the world
//          is rendered with
//       -> a 640x480 guest raster, via the camera-mode table rect at
//          0x8058516C + k*0x30 + 0x08
//       -> four predicates against that rectangle
//       -> a scale divided by a ratio of two rectangles that are always equal
//       -> a string-keyed lookup of four CSD cast names
//       -> eight separate RwIm2DRenderPrimitive calls, each re-establishing the
//          entire GX pipeline: ~148 GX commands per gauge of which 8 carry
//          information
//       -> the host's HUD affine, applied at layout time, then exempted again
//          by a global latch
//
// Everything after the first arrow is console machinery. The behaviour worth
// keeping is small and is listed under GAME BEHAVIOUR below.
//
// ---------------------------------------------------------------------------
// THE SHAPE HERE
// ---------------------------------------------------------------------------
//
//     Signal (what the enemy wrote)   Authored (what the designer set)
//              \                         /
//               +-----------+-----------+
//                           |
//                      place(): canonical camera -> NativeWorldAnchorProjection
//                           |          (visibility, depth, presentation pixels)
//                           |
//                      Placement { visible, reason, pixel, scale, fill, width }
//                           |
//                      build(): the authored composition -> quads
//                           |
//                      one batched native draw
//
// Five stages, and each one knows exactly one thing:
//
//   world anchor  the enemy owns it; this layer never looks up an enemy
//   visibility    only place() knows the camera exists
//   scaling       a pure function of depth and authored parameters
//   animation     a pure function of the HP rate; no time term, by design
//   presentation  only build() knows what a quad is; only the binding knows
//                 what a GPU is
//
// ---------------------------------------------------------------------------
// GAME BEHAVIOUR THIS PRESERVES (from docs/health-bar-2026-09-21/)
// ---------------------------------------------------------------------------
//
//  * Two gates before anything: the global HUD-visible flag, then the
//    per-gauge enable. Booleans, no fade, no hysteresis.
//  * Depth is the VIEW-AXIS distance dot(forward, world - eye). Never a
//    Euclidean range: the authored window is a band along the view axis, and a
//    range substitution widens it by 1/cos(theta) - about 19% at the edge of a
//    78.8 degree frustum.
//  * The authored depth window, evaluated on that depth, after the camera's own
//    near/far pair. Defaults 20 / 200, but they are PER INSTANCE: Devil Doom's
//    wing gauges author 500. Nothing here hard-codes them.
//  * scale = base_scale / depth. No clamp, no smoothing, no hysteresis.
//  * Fill is an instantaneous seek. The original writes the same value to both
//    the current and the target animation frame, which collapses the runtime's
//    own interpolation. Nothing in this bar tweens.
//  * rate is produced by the enemy; when its max HP reads near zero the enemy
//    forces rate to 0 and does NOT update max HP, so the bar shows EMPTY at the
//    previous width. (An earlier reading of this said the bar freezes. It does
//    not, and the difference is visible.)
//  * Width law: (max_hp < 0 ? 4 : max_hp) * 0.25, all-or-nothing across the
//    three bar pieces; the end caps are repositioned, never scaled. Four HP is
//    the authored reference width, so a 4 HP enemy is pixel-identical to the art.
//  * Rejection is total: no submission AND no animation step.
//  * The bar never occludes and is never occluded: depth compare ALWAYS, depth
//    write off, cull none. Order is pure painter's order.
//
// ---------------------------------------------------------------------------
// CONSOLE LIMITATION THIS DISCARDS
// ---------------------------------------------------------------------------
//
//  * The four screen-rect predicates. They look like a 640-pixel raster limit
//    and are not: the rect that bounds the test is the same rect that defines
//    the projection's output span, so the width cancels and the predicate is
//    exactly |ndc| <= 1 + 2/W in the guest's own 63.25 degree cone. Widening
//    the raster admits nothing; only widening the CONE does. That is why this
//    layer tests |ndc| <= 1 against the canonical camera and nothing else.
//  * The 640x480 rects, the camera-mode table as camera truth, the rectA/rectB
//    scale ratio (identically 1.0 and not even bit-exact through fmuls/fdivs),
//    the integer truncation onto a 640x480 lattice, the per-viewport FOV scale
//    pair with its 15/14 NTSC active-line factor, and the three-index
//    indirection behind it.
//  * Recovering a camera from six frustum planes.
//  * One draw per cast, and the four-vertex scratch buffer that made batching
//    structurally impossible.
//
// ---------------------------------------------------------------------------
// WHY A SECOND Hor+ IS UNREPRESENTABLE HERE
// ---------------------------------------------------------------------------
//
// This layer contains no aspect ratio, no coverage factor, no canvas width and
// no reference aspect - grep it. It receives a camera that has ALREADY been
// widened and a viewport rectangle that is ALREADY the presented image, and it
// asks NativeWorldAnchorProjection for pixels. There is no quantity here that
// a widescreen correction could be applied to, so applying one twice is not a
// mistake that can be made.
//
// The positions this produces are final. Nothing downstream may transform them:
// that is what DrawAnchorKind::WorldAnchored carries, stamped on the draw
// itself rather than latched in a global.

#include <array>

#include "gekko/types.h"
#include "gekko/world_anchor.h"

namespace gekko::health_bar {

// ---------------------------------------------------------------------------
// Stage 1 - the world anchor, and what else the enemy produces
// ---------------------------------------------------------------------------

// Everything the enemy's display component writes each frame. This layer is a
// pure consumer: it never reads an enemy, a bone, a status block or an attach
// matrix. The anchor's composition - frame position, plus the attach transform
// and the half-height lift WHEN an attach matrix resolves - belongs to the
// enemy and stays there.
struct Signal {
    float world_x = 0.0f, world_y = 0.0f, world_z = 0.0f;

    // current HP / max HP, computed by the enemy. Forced to 0 by the enemy when
    // max HP reads near zero, which is how a degenerate enemy shows an empty
    // bar rather than a stale one.
    float rate = 1.0f;

    // The enemy's max HP, which is what the bar's WIDTH means. Negative is the
    // unset sentinel and selects the authored reference width.
    float max_hp = -1.0f;

    // The per-enemy-type show/hide policy, already evaluated. Seven enemy types
    // always show; twelve gate on a per-type state; three of those additionally
    // run a countdown. That policy is enemy behaviour - this layer consumes the
    // boolean it produces.
    bool enabled = false;
};

// What the designer set on this instance. Defaults are the constructor's, not
// the rule: max_depth and base_scale have setters and are overridden in the
// game, so a replacement that bakes 20/200/50 in clips the final boss's gauges
// 300 units early and draws them at half size.
struct Authored {
    float min_depth = 20.0f;
    float max_depth = 200.0f;
    float base_scale = 50.0f;

    // A consumer's own screen margin, in presentation pixels, composed with
    // canonical coverage rather than competing with it. The enemy bar authors
    // none; LockonMark authors 32. This layer never invents one.
    float screen_margin = 0.0f;

    constexpr bool sane() const noexcept {
        return min_depth >= 0.0f && max_depth > min_depth && base_scale > 0.0f;
    }
};

struct Gauge {
    Signal signal{};
    Authored authored{};
};

// ---------------------------------------------------------------------------
// Stage 2/3/4 - visibility, scale, animation state
// ---------------------------------------------------------------------------

// One named reason per gauge per frame. The original could only ever answer
// "it is not there"; every disappearance here can name its cause, and every
// outcome is counted AND printed.
enum class Rejection : u32 {
    None = 0,
    HudHidden,          // the shared global HUD flag
    Disabled,           // the enemy's own per-type policy
    NotAuthored,        // authored parameters are degenerate
    InvalidAnchor,      // non-finite world position
    BehindCamera,
    CameraNear,
    CameraFar,
    AuthoredMinDepth,   // GAME BEHAVIOUR
    AuthoredMaxDepth,   // GAME BEHAVIOUR
    OffScreenLeft,
    OffScreenRight,
    OffScreenBottom,
    OffScreenTop,
    Count,
};

const char* rejection_name(Rejection r) noexcept;

struct Placement {
    bool visible = false;
    Rejection rejection = Rejection::None;

    // Final presentation pixels. Already correct; nothing downstream may apply
    // a widescreen or HUD correction to these.
    float pixel_x = 0.0f;
    float pixel_y = 0.0f;

    // View-axis distance from the eye. The quantity every authored rule is
    // expressed in.
    float depth = 0.0f;

    // base_scale / depth, the authored law alone. Isotropic: the original
    // stored two components, but derived them from a ratio of two rectangles
    // that are equal in all five camera modes, so the second carried no
    // information (it did carry a 1-ULP difference, which is not behaviour).
    float scale = 0.0f;

    // The authored law times the viewport term - what actually converts
    // bar-local units into presentation pixels. Kept separate from `scale` so
    // the authored quantity stays testable on its own.
    float pixel_scale = 0.0f;

    // Animation state. Instantaneous by design - there is no time term and no
    // interpolation anywhere in this bar.
    float fill = 0.0f;        // 0 = empty, 1 = full

    // The bar's length, in authored reference widths. 1.0 is the authored art.
    float width = 1.0f;

    // Distance to the nearest screen edge in presentation pixels, negative when
    // outside. Consumers that fade near an edge use this instead of re-deriving
    // it from a canvas size.
    float edge_margin_x = 0.0f;
    float edge_margin_y = 0.0f;
};

// The animation stage, isolated so it can be tested without a camera.
//
// The original seeks a CSD animation frame, `20 * (1 - rate)`, and writes it to
// both the current and the target slot. The 20 is the asset's authored motion
// length and is an undeclared coupling: express the state as a fraction and let
// the composition own its own span.
//
// The original never clamps here. The only thing keeping the frame inside the
// authored range is that the enemy's heal routine clamps HP to max HP, so a
// mod or an unexpected value walks straight off the end of the motion. Clamping
// is a strict improvement that cannot change behaviour for in-range input.
constexpr float fill_from_rate(float rate) noexcept {
    return rate < 0.0f ? 0.0f : (rate > 1.0f ? 1.0f : rate);
}

// The width stage. `4` is the authored reference: a 4 HP enemy must be
// pixel-identical to the art, so the law is expressed as a ratio to it rather
// than as the original's `max_hp * 0.25` against a 640-wide canvas.
constexpr float kAuthoredReferenceHp = 4.0f;
constexpr float width_from_max_hp(float max_hp) noexcept {
    return (max_hp < 0.0f) ? 1.0f : (max_hp / kAuthoredReferenceHp);
}

// ---------------------------------------------------------------------------
// Stage 5 - the authored composition
// ---------------------------------------------------------------------------

// One drawable piece of the bar, in bar-local units where 1.0 is one authored
// pixel at unit scale. This is the asset's content, not its container: the CSD
// scene graph, its string-keyed lookups, its canvas round trip and its
// authoring-aspect fixup are all gone, and what survives is the picture.
//
// The original's parent/child tree is FLATTENED here. It had exactly two jobs -
// carry the width law down from the body, and carry the two end caps outward -
// and both are expressed as an ANCHOR plus a LOCAL RECT:
//
//   anchor   the accumulated translation. The width law multiplies anchor_x,
//            because every anchor in this asset descends from one of the three
//            casts the guest repositions (`outline`, `outlinel`, `outliner`).
//   local    the quad in the part's own frame. Its X extent is multiplied by
//            the width law only for the parts under `outline`, which is the
//            one cast the guest scales.
//
// Keeping those separate is what makes the end caps behave: they are
// REPOSITIONED AND NOT SCALED, so their anchor moves to 22*width while their
// 2-unit local rect stays 2 units. Collapsing the two into a centre and a half
// extent silently scales the caps as well.
struct Part {
    float anchor_x = 0.0f, anchor_y = 0.0f;
    float left = 0.0f, right = 0.0f;    // local rect, +x right
    float top = 0.0f, bottom = 0.0f;    // local rect, +y DOWN, as authored
    std::array<u8, 4> rgba{};           // authored, never modulated at runtime

    // Under `outline`, the one cast the guest applies a scale to.
    bool body_relative = false;
    // The meter, whose right edge follows the fill while its left edge is
    // pinned - which is what makes the bar empty right-to-left.
    bool drains = false;
};

// The bar's picture, as data. Held in one place so the asset's numbers have
// exactly one home, and so a test can substitute a synthetic composition.
struct Composition {
    static constexpr u32 kMaxParts = 8;
    std::array<Part, kMaxParts> parts{};
    u32 count = 0;

    // The canvas height the art was authored against. A property of the ASSET,
    // which is why it lives here and not in the layer: it is what lets the bar
    // keep its authored proportions in a viewport of any size. The original
    // carried the same idea as a ratio of two camera-mode rectangles, which is
    // why the bar shrinks with the viewport in split screen.
    //
    // VERTICAL on purpose. A widescreen viewport is wider, not taller, so no
    // aspect ratio can reach the bar's size through this term and a second
    // Hor+ has nowhere to enter.
    float reference_height = 0.0f;

    // The authored HPBar composition. Every number here comes from the asset,
    // not from a screen dimension.
    static Composition authored() noexcept;

    // The union of every part's rect at a given width, as extents from the
    // anchor in bar-local units. This is what makes the visibility test an
    // intersection of the BAR with the screen rather than a containment test on
    // its centre - a centre test culls the bar with up to half of it still
    // drawn, and because this composition is asymmetric (-24..+26) it would
    // also pop earlier on one side than the other.
    //
    // Computed at full fill on purpose: the frame, the caps and the backing
    // plate are drawn whatever the HP is, so the drawn extent does not shrink
    // as the meter drains.
    struct Extent {
        float left = 0.0f, right = 0.0f, up = 0.0f, down = 0.0f;
    };
    Extent extent_at(float width) const noexcept;
};

// A vertex of the rebuilt bar. Position is in presentation pixels; colour is
// the authored colour. There is deliberately no z: depth is genuinely absent
// from this layer, and the intent - compare ALWAYS, write off - belongs to the
// pipeline state, never to a value.
struct Vertex {
    float x = 0.0f, y = 0.0f;
    std::array<u8, 4> rgba{};
};

constexpr u32 kVerticesPerQuad = 4;
constexpr u32 kMaxVerticesPerGauge = Composition::kMaxParts * kVerticesPerQuad;

// ---------------------------------------------------------------------------
// The layer
// ---------------------------------------------------------------------------

class Layer {
public:
    // Once per frame per viewport, before any gauge. The camera is taken as a
    // first-class input and is never recovered, cached or defaulted here: a
    // camera that was not built must fail at its source, not arrive as an
    // identity basis that silently turns depth into -world_z.
    //
    // `viewport` is the rectangle the image is presented into. Because the
    // camera is already the displayed camera, |ndc| <= 1 maps exactly onto it.
    bool begin_frame(const camera::Camera& camera, const world_anchor::ContentViewport& viewport,
                     bool hud_visible) noexcept;

    bool ready() const noexcept { return ready_; }

    // The whole decision for one gauge. Pure: same inputs, same output.
    //
    // It takes the composition because visibility depends on the bar's SIZE:
    // the element is culled when the bar leaves the screen, not when its anchor
    // does.
    Placement place(const Gauge& gauge, const Composition& composition) const noexcept;

    // The picture, in presentation pixels. Returns the vertex count written, 4
    // per quad, or 0 when the placement is not visible. Quads, not strips: one
    // batched draw for the layer instead of the original's eight brackets per
    // gauge.
    u32 build(const Placement& placement, const Composition& composition, Vertex* out,
              u32 capacity) const noexcept;

    struct Stats {
        u64 considered = 0;
        u64 drawn = 0;
        u64 quads = 0;
        std::array<u64, static_cast<u32>(Rejection::Count)> rejected{};
        // Named for what they count. begin_frame is called once per gauge
        // per viewport (the camera is cached, so it is cheap), not once
        // per frame, and calling it "frames" would have been a lie in the
        // report.
        u64 camera_updates = 0;
        u64 camera_refused = 0;
        void reset() noexcept { *this = Stats{}; }
    };

    const Stats& stats() const noexcept { return stats_; }
    Stats& stats() noexcept { return stats_; }
    void record(const Placement& placement) noexcept;

private:
    world_anchor::Projector projector_{};
    bool hud_visible_ = false;
    bool ready_ = false;
    Stats stats_{};
};

}  // namespace gekko::health_bar
