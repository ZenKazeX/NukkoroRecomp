#include "gekko/health_bar.h"

#include <cmath>

#include "gekko/profiling.h"

namespace gekko::health_bar {
namespace {

constexpr float kEpsilon = 1.0e-6f;

bool finite(float v) noexcept { return std::isfinite(v); }

constexpr std::array<u8, 4> rgba(u32 packed) noexcept {
    return {static_cast<u8>((packed >> 24) & 0xFFu), static_cast<u8>((packed >> 16) & 0xFFu),
            static_cast<u8>((packed >> 8) & 0xFFu), static_cast<u8>(packed & 0xFFu)};
}

constexpr u32 kWhite = 0xFFFFFFFFu;
constexpr u32 kBlack = 0x000000FFu;
// The meter's authored green. Not modulated at runtime: there is no low-HP
// flash, no fade and no recolour anywhere in this bar.
constexpr u32 kMeterGreen = 0x008200FFu;

// How the layer's plots are drawn. Declared once, from record(), which is the
// only function here that plots.
GEKKO_PROFILE_ONLY(void configure_health_bar_plots() {
    GEKKO_PROFILE_PLOT_CONFIG("gpu/health-bar depth", GEKKO_PLOT_NUMBER, false, true, 0x4FA8FFu);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/health-bar pixel scale", GEKKO_PLOT_NUMBER, false, true,
                              0x9FD84Fu);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/health-bar considered", GEKKO_PLOT_NUMBER, true, true,
                              0xFFB03Fu);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/health-bar drawn", GEKKO_PLOT_NUMBER, true, true, 0xFF4040u);
})

Rejection translate(world_anchor::Rejection r) noexcept {
    switch (r) {
        case world_anchor::Rejection::None: return Rejection::None;
        case world_anchor::Rejection::BehindCamera: return Rejection::BehindCamera;
        case world_anchor::Rejection::CameraNear: return Rejection::CameraNear;
        case world_anchor::Rejection::CameraFar: return Rejection::CameraFar;
        case world_anchor::Rejection::AuthoredMinDepth: return Rejection::AuthoredMinDepth;
        case world_anchor::Rejection::AuthoredMaxDepth: return Rejection::AuthoredMaxDepth;
        case world_anchor::Rejection::OffScreenLeft: return Rejection::OffScreenLeft;
        case world_anchor::Rejection::OffScreenRight: return Rejection::OffScreenRight;
        case world_anchor::Rejection::OffScreenBottom: return Rejection::OffScreenBottom;
        case world_anchor::Rejection::OffScreenTop: return Rejection::OffScreenTop;
    }
    return Rejection::None;
}

}  // namespace

const char* rejection_name(Rejection r) noexcept {
    switch (r) {
        case Rejection::None: return "visible";
        case Rejection::HudHidden: return "hud hidden";
        case Rejection::Disabled: return "disabled by enemy";
        case Rejection::NotAuthored: return "authored parameters degenerate";
        case Rejection::InvalidAnchor: return "anchor not finite";
        case Rejection::BehindCamera: return "behind camera";
        case Rejection::CameraNear: return "camera near";
        case Rejection::CameraFar: return "camera far";
        case Rejection::AuthoredMinDepth: return "authored min depth";
        case Rejection::AuthoredMaxDepth: return "authored max depth";
        case Rejection::OffScreenLeft: return "off screen left";
        case Rejection::OffScreenRight: return "off screen right";
        case Rejection::OffScreenBottom: return "off screen bottom";
        case Rejection::OffScreenTop: return "off screen top";
        case Rejection::Count: break;
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The authored composition
// ---------------------------------------------------------------------------
//
// From files/csdFiles/HPBar.gncp. Nine casts, of which `bar` is a null root
// that emits nothing and `dammy_texture` is a 2x2 offscreen quad whose only
// purpose is to keep the CSD runtime's texture list non-empty - it costs one
// primitive, one texture bind and the whole textured TEV path per gauge and
// contributes no pixels, so it is not reproduced. Seven quads remain.
//
// Coordinates are bar-local units, +x right and +y DOWN, matching both the
// authoring canvas and presentation pixels, so no axis is flipped anywhere in
// this layer.
//
// The parent/child tree is FLATTENED into an anchor plus a local rect. The
// guest's width pass scales exactly one cast (`outline`) and repositions
// exactly three (`outline`, `outlinel`, `outliner`), and because every other
// cast is a child of one of those, the whole tree reduces to:
//
//   anchor_x        multiplied by the width law, always - every anchor here
//                   descends from one of the three repositioned casts.
//   local rect      multiplied by the width law only when `body_relative`,
//                   i.e. only for the parts under `outline`, the one cast the
//                   guest scales. This is what keeps the two end caps their
//                   authored 2 units wide while their anchors travel outward.
//   drains          the fill moves the right edge and pins the left. Only the
//                   meter, which is why the bar empties right-to-left.
//
// The all-or-nothing rule from the original - if the body or either cap is
// missing, apply no width change at all - is preserved by construction: this
// table is a single unit, so the three pieces are always present together.
Composition Composition::authored() noexcept {
    Composition c;
    auto add = [&c](float anchor_x, float anchor_y, float left, float top, float right,
                    float bottom, u32 colour, bool body_relative, bool drains) {
        if (c.count >= kMaxParts) return;
        Part& p = c.parts[c.count++];
        p.anchor_x = anchor_x;
        p.anchor_y = anchor_y;
        p.left = left;
        p.top = top;
        p.right = right;
        p.bottom = bottom;
        p.rgba = rgba(colour);
        p.body_relative = body_relative;
        p.drains = drains;
        return;
    };
    // ORDER IS SUBMISSION ORDER, and submission order is the asset's FLAT cast
    // array - not the parent/child tree's pre-order. The scene renderer walks
    // the cast pointer array by index with no depth key, so this is the whole
    // of the ordering rule: the vertices carry no z and the depth compare is
    // ALWAYS, making the layer pure painter's order. (An earlier decode listed
    // the tree's DFS order here, which differs and would put the drop shadow
    // on top of the bar.)
    //
    // Anchors are the accumulated translations: `shadow`, `meter_bg` and
    // `meter` sit on `outline` at -22, `shadow_side` sits on `outliner` at +22.
    add(-22.0f, 5.0f, 0.0f, -4.0f, 45.0f, 4.0f, kBlack, true, false);        // shadow
    add(22.0f, -2.0f, 0.0f, 0.0f, 4.0f, 11.0f, kBlack, false, false);        // shadow_side
    add(22.0f, 0.0f, 0.0f, -6.0f, 2.0f, 6.0f, kWhite, false, false);         // outliner  (right cap)
    add(-22.0f, 0.0f, 0.0f, -6.0f, 44.0f, 6.0f, kWhite, true, false);        // outline   (body)
    add(-22.0f, 0.0f, 0.0f, -4.0f, 44.0f, 4.0f, kBlack, true, false);        // meter_bg
    add(-22.0f, 0.0f, 0.0f, -4.0f, 44.0f, 4.0f, kMeterGreen, true, true);    // meter
    add(-22.0f, 0.0f, -2.0f, -6.0f, 0.0f, 6.0f, kWhite, false, false);       // outlinel  (left cap)

    // NOT reproduced: `dammy_texture`, a 2x2 quad the asset parks at bar-local
    // (-24, +302) in its own second group. It exists to keep the CSD runtime's
    // texture list non-empty, and it costs one primitive, one texture bind and
    // the entire textured TEV path per gauge.
    //
    // Be clear about what dropping it changes. It is NOT invisible: its palette
    // is GX_TL_RGB565, so its single entry 0x0000 is OPAQUE BLACK, not
    // transparent - an earlier reading had this backwards. If the CSD runtime
    // does submit it (that is genuinely unresolved - its two gating fields are
    // runtime state), the original draws a 2x2 black dot roughly 302 scaled
    // units below every health bar, which at the far end of the authored depth
    // window lands on screen. Removing it is therefore a deliberate, visible
    // change, made because a texture-list keeper carries no design intent.
    // It is called out here rather than buried so it can be reversed knowingly.

    // The canvas the art was authored against. A property of the ASSET, not of
    // any screen: CSD is initialised with 640 x 480 and the asset's own texture
    // table confirms that scale (0.0041666669 * 480 = 2 px). It is what lets
    // the bar keep its authored proportions in a viewport of any size, which is
    // the one thing the original's rectA/rectB ratio was for. Expressed once,
    // here, and never as an aspect.
    c.reference_height = 480.0f;
    return c;
}

// ---------------------------------------------------------------------------
// The layer
// ---------------------------------------------------------------------------

bool Layer::begin_frame(const camera::Camera& camera,
                        const world_anchor::ContentViewport& viewport, bool hud_visible) noexcept {
    // Despite the name this is NOT once per frame: the native binding calls it
    // once per GAUGE, so the orthonormality check below and the whole
    // projector_.update re-run for every enemy bar against the same camera.
    // This zone measures that repetition directly, which is the evidence
    // anyone would need before arguing to hoist it out.
    GEKKO_PROFILE_ZONE("gpu/health-bar-begin-frame");
    ++stats_.camera_updates;
    hud_visible_ = hud_visible;
    ready_ = false;

    // A camera that was not built must fail here, loudly, and never be usable.
    // The defect this guard exists for is exact and was shipped: a default
    // constructed camera has an identity basis and a zero origin, which turns
    // `depth` into `-world_z` - on Final Haunt that is 3,400 instead of 195,
    // so every gauge fails its authored 200-unit rule and not one bar draws,
    // with no error anywhere. An identity basis is a legal rotation, so only
    // the extents can distinguish it, and they are zero.
    if (!camera.extents.valid()) {
        ++stats_.camera_refused;
        return false;
    }
    if (!(camera.near_plane > 0.0f) || !(camera.far_plane > camera.near_plane)) {
        ++stats_.camera_refused;
        return false;
    }
    if (!viewport.valid()) {
        ++stats_.camera_refused;
        return false;
    }
    // The basis has to be a rotation, or `depth` is not a world distance and
    // every authored rule downstream is being applied to the wrong quantity.
    const std::array<float, 9>& b = camera.view_to_world.basis;
    for (int j = 0; j < 3; ++j) {
        for (int k = j; k < 3; ++k) {
            const float dot = b[static_cast<std::size_t>(j)] * b[static_cast<std::size_t>(k)] +
                              b[static_cast<std::size_t>(j) + 3u] * b[static_cast<std::size_t>(k) + 3u] +
                              b[static_cast<std::size_t>(j) + 6u] * b[static_cast<std::size_t>(k) + 6u];
            const float expected = (j == k) ? 1.0f : 0.0f;
            if (!(std::fabs(dot - expected) <= 1.0e-3f)) {
                ++stats_.camera_refused;
                return false;
            }
        }
    }

    projector_.update(camera, viewport);
    ready_ = true;
    return true;
}

Composition::Extent Composition::extent_at(float width) const noexcept {
    Extent e;
    bool any = false;
    for (u32 i = 0; i < count; ++i) {
        const Part& p = parts[i];
        const float anchor_x = p.anchor_x * width;
        const float extent = p.body_relative ? width : 1.0f;
        // Full fill: the frame, the caps and the backing plate are drawn at
        // every HP value, so the drawn extent does not shrink as the meter
        // drains.
        const float left = anchor_x + p.left * extent;
        const float right = anchor_x + p.right * extent;
        const float top = p.anchor_y + p.top;
        const float bottom = p.anchor_y + p.bottom;
        if (!any) {
            e.left = left;
            e.right = right;
            e.up = top;
            e.down = bottom;
            any = true;
            continue;
        }
        e.left = left < e.left ? left : e.left;
        e.right = right > e.right ? right : e.right;
        e.up = top < e.up ? top : e.up;
        e.down = bottom > e.down ? bottom : e.down;
    }
    if (!any) return Extent{};
    // As distances FROM the anchor, which is what the projection service wants.
    return Extent{-e.left, e.right, -e.up, e.down};
}

Placement Layer::place(const Gauge& gauge, const Composition& composition) const noexcept {
    // The layout decision for one gauge: the gates, the 7-part extent fold and
    // the world-anchor projection. Kept apart from build() so "slow to decide"
    // and "slow to emit" have separate answers. projector_.project lives in
    // world_anchor.cpp, so until that file is instrumented its cost shows up as
    // this zone's unattributed interior.
    GEKKO_PROFILE_ZONE("gpu/health-bar-place");
    Placement placement;

    // The two original gates, in the original order, before anything is
    // projected. Both are plain booleans: there is no fade, no timer and no
    // hysteresis on either.
    if (!hud_visible_) {
        placement.rejection = Rejection::HudHidden;
        return placement;
    }
    if (!gauge.signal.enabled) {
        placement.rejection = Rejection::Disabled;
        return placement;
    }
    if (!ready_ || !gauge.authored.sane()) {
        placement.rejection = Rejection::NotAuthored;
        return placement;
    }
    // The original accepts a non-finite anchor: `fcmpo` leaves CR0 unordered
    // and every one of its eight branches is bge/ble/bgt/blt, so a NaN passes
    // all of them. Rejecting it is a strict improvement - it cannot change the
    // outcome for any finite input - and it stops a single bad anchor from
    // putting a quad at an undefined place on screen.
    if (!finite(gauge.signal.world_x) || !finite(gauge.signal.world_y) ||
        !finite(gauge.signal.world_z)) {
        placement.rejection = Rejection::InvalidAnchor;
        return placement;
    }

    world_anchor::Request request;
    request.world_x = gauge.signal.world_x;
    request.world_y = gauge.signal.world_y;
    request.world_z = gauge.signal.world_z;
    // The camera's own pair first, then the authored pair - the original's
    // order, and it matters only for which reason is reported, because the
    // authored window sits strictly inside the camera's.
    request.use_camera_near_far = true;
    request.min_depth = gauge.authored.min_depth;
    request.max_depth = gauge.authored.max_depth;
    request.base_scale = gauge.authored.base_scale;
    request.screen_margin = gauge.authored.screen_margin;
    // The viewport term rides on the authored scale, so the projection service
    // converts the bar's own extent into pixels with exactly the factor that
    // draws it. Vertical on purpose - see Composition::reference_height.
    request.scale_multiplier = (composition.reference_height > kEpsilon)
                                   ? projector_.viewport().height / composition.reference_height
                                   : 1.0f;

    // The element's own projected extent, so visibility is an intersection of
    // the BAR with the screen. Without this the bar is culled when its centre
    // leaves, which at the near end of the authored depth window discards it
    // with tens of pixels still drawn - and asymmetrically, because the
    // composition runs -24..+26.
    const float width_now = width_from_max_hp(gauge.signal.max_hp);
    const Composition::Extent extent = composition.extent_at(width_now);
    request.extent_left = extent.left;
    request.extent_right = extent.right;
    request.extent_up = extent.up;
    request.extent_down = extent.down;

    const world_anchor::Result result = projector_.project(request);

    placement.depth = result.depth;
    placement.pixel_x = result.pixel_x;
    placement.pixel_y = result.pixel_y;
    placement.edge_margin_x = result.edge_margin_x;
    placement.edge_margin_y = result.edge_margin_y;
    placement.rejection = translate(result.rejection);
    placement.visible = result.visible;
    if (!placement.visible) return placement;

    // `base_scale / depth` is the authored law, unclamped and unsmoothed: 2.5
    // at the near end of the default window, 0.25 at the far end. Kept apart
    // from the pixel scale so the authored quantity stays testable on its own.
    placement.scale = (result.depth > kEpsilon) ? gauge.authored.base_scale / result.depth : 0.0f;
    placement.pixel_scale = result.scale;
    placement.fill = fill_from_rate(gauge.signal.rate);
    placement.width = width_now;
    return placement;
}

u32 Layer::build(const Placement& placement, const Composition& composition, Vertex* out,
                 u32 capacity) const noexcept {
    // Opened above ALL four guards, including the degenerate-scale one further
    // down: a gauge that passed placement and then emitted nothing is a real
    // refusal, and a zone opened after the guards would erase it. Here the zone
    // count is the number of times build was asked, and a zero-vertex zone IS
    // the refusal.
    GEKKO_PROFILE_ZONE("gpu/health-bar-build");
    GEKKO_PROFILE_VALUE(composition.count);
    if (!placement.visible || out == nullptr) return 0;
    if (composition.count == 0 || !(composition.reference_height > kEpsilon)) return 0;
    if (capacity < composition.count * kVerticesPerQuad) return 0;

    // The pixel scale already carries the viewport term: it is the same value
    // the visibility test converted the bar's extent with, so what is drawn and
    // what was tested cannot disagree.
    const float scale = placement.pixel_scale;
    if (!finite(scale) || !(std::fabs(scale) > kEpsilon)) return 0;

    const float width = placement.width;
    const float fill = placement.fill;

    u32 written = 0;
    for (u32 i = 0; i < composition.count; ++i) {
        const Part& part = composition.parts[i];

        // The width law. It moves every anchor, because every anchor in this
        // asset descends from one of the three casts the guest repositions;
        // it stretches the local rect only under the body, which is the one
        // cast the guest scales. That separation is what keeps the end caps
        // their authored size while they travel outward.
        const float anchor_x = part.anchor_x * width;
        const float extent = part.body_relative ? width : 1.0f;
        float left = part.left * extent;
        float right = part.right * extent;

        if (part.drains) {
            // Pinned at the piece's own left edge; only the right edge moves.
            right = left + (right - left) * fill;
        }

        // Bar-local units into presentation pixels. One multiply each; there is
        // no canvas, no normalised round trip and no authoring-aspect fixup.
        const float x0 = placement.pixel_x + (anchor_x + left) * scale;
        const float x1 = placement.pixel_x + (anchor_x + right) * scale;
        const float y0 = placement.pixel_y + (part.anchor_y + part.top) * scale;
        const float y1 = placement.pixel_y + (part.anchor_y + part.bottom) * scale;

        // A fully drained meter is a zero-area quad. Skip it rather than submit
        // it: it is the one part whose extent reaches zero in normal play, and
        // a degenerate quad is work that produces no pixels.
        if (!(std::fabs(x1 - x0) > 0.0f) || !(std::fabs(y1 - y0) > 0.0f)) continue;

        Vertex* v = out + written;
        // Quads, wound so one batched GX_QUADS draw covers the whole layer
        // instead of the original's eight separate pipeline brackets.
        v[0] = Vertex{x0, y0, part.rgba};
        v[1] = Vertex{x1, y0, part.rgba};
        v[2] = Vertex{x1, y1, part.rgba};
        v[3] = Vertex{x0, y1, part.rgba};
        written += kVerticesPerQuad;
    }
    return written;
}

void Layer::record(const Placement& placement) noexcept {
    GEKKO_PROFILE_ONLY(static const bool plots_configured =
                           (configure_health_bar_plots(), true);
                       (void)plots_configured;)
    ++stats_.considered;
    // Two monotone ramps, so the SLOPE is the per-gauge rate and the divergence
    // between them is the refusal rate over time. The validated ratio is 10.5%
    // drawn; a session where that moves is the signature of a camera or window
    // regression, and today it is only visible as one aggregate at end of run.
    GEKKO_PROFILE_PLOT("gpu/health-bar considered", static_cast<double>(stats_.considered));
    if (placement.visible) {
        ++stats_.drawn;
        GEKKO_PROFILE_PLOT("gpu/health-bar drawn", static_cast<double>(stats_.drawn));
        // Depth is the quantity behind the shipped defect the comment in
        // begin_frame documents: a default-constructed camera collapsed it to
        // -world_z, 3,400 instead of 195, and not one bar drew with no error
        // anywhere. Plotted with pixel_scale it also draws the authored scale
        // law - 2.5 at the near end of the window down to 0.25 at the far end -
        // so a wrong viewport height or reference_height shows as a departure
        // from that curve. Visible placements only: an invisible one carries 0
        // or a stale value and would flatten both plots to noise.
        GEKKO_PROFILE_PLOT("gpu/health-bar depth", static_cast<double>(placement.depth));
        GEKKO_PROFILE_PLOT("gpu/health-bar pixel scale",
                           static_cast<double>(placement.pixel_scale));
        return;
    }
    const u32 index = static_cast<u32>(placement.rejection);
    if (index < stats_.rejected.size()) ++stats_.rejected[index];
}

}  // namespace gekko::health_bar
