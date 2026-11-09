#pragma once

// The one authoritative description of every rectangle in the renderer.
//
// Before this header the same numbers lived in four places that were kept equal
// by convention alone:
//
//   * `kEfbWidth` / `kEfbHeight`   (gekko/renderer.h)      640 x 528
//   * `kWidth` / `kHeight`         (plume_gx_raster.cpp)   640 x 528, a private
//                                                          duplicate; that file
//                                                          does not include
//                                                          renderer.h and no
//                                                          static_assert tied
//                                                          the two together
//   * `kVirtualHudWidth/Height`    (gekko/display.h)       640 x 480
//   * the guest's own display raster, recovered only from the BP scissor
//
// Everything below is pure arithmetic over explicitly named coordinate spaces.
// Nothing here allocates, touches a device, or reads global state, so all of it
// is unit-testable and none of it can drift out of step with the resources it
// describes.
//
// ---------------------------------------------------------------------------
// The coordinate spaces, in the order a pixel travels through them
// ---------------------------------------------------------------------------
//
//   guest clip space          what the guest's XF stage emits
//        | XF viewport (guest programs it; scale/offset in guest EFB units)
//        v
//   GUEST EFB SPACE           integer pixels, origin top-left. The console's
//                             embedded framebuffer. `GuestRaster` names the
//                             sub-rectangle the guest actually draws into
//                             (640x480 for this game); the surrounding area up
//                             to the EFB bound exists but is never written.
//        | RenderScale        internal resolution multiplier, 1 = console
//        v
//   NATIVE EFB SPACE          the host colour/depth targets. Identical to guest
//                             EFB space when scale == 1, which is why adopting
//                             this header is bit-identical by construction.
//        | resolve / EFB copy (source rect in GUEST units, always)
//        v
//   COPY DESTINATION SPACE    guest units. Texture copies MUST land back at
//                             guest dimensions or the game's own UVs break.
//        |
//        v
//   VIRTUAL HUD CANVAS        640x480. The space the game authors 2D in, and
//                             the space `apply_hud_layout` remaps. It is NOT
//                             the EFB: it is 48 rows shorter, and comparing an
//                             EFB Y against a canvas threshold is a bug.
//        | AspectMapping      Hor+ widening, anchors
//        v
//   OUTPUT / LAYOUT SPACE     DisplayLayout::output_width/height
//        | fit_content_rect
//        v
//   SWAPCHAIN SPACE           the back buffer, letterboxed/pillarboxed
//
// A conversion that is not expressed here is a conversion nobody can audit.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "gekko/types.h"

namespace gekko {

// ---------------------------------------------------------------------------
// Console-side constants. These describe the GameCube, so they are the only
// numbers in this header that are genuinely fixed.
// ---------------------------------------------------------------------------

// The embedded framebuffer the Flipper provides. 640x528 is the largest region
// addressable in the 24-bit colour + 24-bit depth configuration this game uses.
constexpr u32 kGuestEfbWidth = 640;
constexpr u32 kGuestEfbHeight = 528;

// The raster the guest actually programs, recovered from its BP scissor rather
// than assumed. Westopolis reports exactly one scissor state for every one of
// its 7,621 pixel snapshots: TL 0x156156 BR 0x3D5335 OFF 0x02ACAB -> [0,0]-[639,479].
// Rows 480..527 of the EFB are therefore never written by any draw, and the
// guest issues zero copy-clears, so they hold whatever the one startup clear
// left. They are initialised (opaque black) but they are not image data, and
// any copy rectangle that reaches them is a bug.
constexpr u32 kGuestRasterWidth = 640;
constexpr u32 kGuestRasterHeight = 480;

// The 2D authoring canvas. Chao::CSD is initialised to exactly this by
// `Chao::CSD::Initialize` (fn_80364A38); authored coordinates are normalised
// 0..1 and become pixels as u*640, v*480.
constexpr float kHudCanvasWidth = 640.0f;
constexpr float kHudCanvasHeight = 480.0f;

// The aspect the original composes for. This is the DISPLAY aspect, not the
// game's projection aspect (the guest float 1.3195877 at 0x805EBA30). Two
// earlier audits conflated the two; using 1.3195877 here would make the Hor+
// scale 0.98969 instead of exactly 1.0 and 640x480 would stop being
// bit-identical. Do not change it.
constexpr float kReferenceAspect = 4.0f / 3.0f;

// ---------------------------------------------------------------------------
// Rectangles. One type, used for every space, with the space named at the use
// site rather than encoded in a dozen bare int pairs.
// ---------------------------------------------------------------------------

struct PixelRect {
    u32 left = 0;
    u32 top = 0;
    u32 width = 0;
    u32 height = 0;

    constexpr u32 right() const noexcept { return left + width; }
    constexpr u32 bottom() const noexcept { return top + height; }
    constexpr bool empty() const noexcept { return width == 0u || height == 0u; }
    constexpr bool operator==(const PixelRect& other) const noexcept {
        return left == other.left && top == other.top && width == other.width &&
               height == other.height;
    }
    constexpr bool operator!=(const PixelRect& other) const noexcept {
        return !(*this == other);
    }
};

// Inclusive-bound form, because that is how BP programs the scissor and how the
// renderer's own `scissor_right`/`scissor_bottom` members are stored. Kept
// distinct from PixelRect so an off-by-one cannot be introduced by assignment.
struct InclusiveRect {
    int left = 0;
    int top = 0;
    int right = 0;   // inclusive
    int bottom = 0;  // inclusive

    constexpr bool empty() const noexcept { return right < left || bottom < top; }
};

constexpr PixelRect to_half_open(const InclusiveRect& rect) noexcept {
    if (rect.empty()) return PixelRect{};
    return PixelRect{static_cast<u32>(rect.left < 0 ? 0 : rect.left),
                     static_cast<u32>(rect.top < 0 ? 0 : rect.top),
                     static_cast<u32>(rect.right - (rect.left < 0 ? 0 : rect.left) + 1),
                     static_cast<u32>(rect.bottom - (rect.top < 0 ? 0 : rect.top) + 1)};
}

// Clamp a rectangle into a bound, keeping the origin and shrinking the extent.
// Returns an empty rect when the origin is already outside, rather than
// silently sliding it back inside - the old `min(width, kEfbWidth)` form kept
// the width and let the origin drift, which is how a source and a destination
// rectangle come to disagree.
constexpr PixelRect clamp_into(const PixelRect& rect, u32 bound_width,
                               u32 bound_height) noexcept {
    if (rect.left >= bound_width || rect.top >= bound_height) return PixelRect{};
    PixelRect out = rect;
    out.width = std::min(rect.width, bound_width - rect.left);
    out.height = std::min(rect.height, bound_height - rect.top);
    return out;
}

// ---------------------------------------------------------------------------
// Internal resolution
// ---------------------------------------------------------------------------

// The multiplier between guest EFB space and the native render targets. It is
// an integer on purpose: a non-integer factor makes the EFB-copy downsample
// back to guest dimensions non-exact, and the game performs 477 texture copies
// per Westopolis route whose UVs depend on that result being the guest's own
// size. 1 is the console.
// The ceiling on the internal scale. 6x of 640x528 is 3840x3168, which is
// already a supersample of any display the fitted raster is presented into;
// beyond it the EFB alone costs more VRAM than the whole rest of the renderer.
// It is one constant because BOTH renderers take their scale from it: the
// legacy GX raster through --internal-scale and the native renderer through
// the same option. There is no second scaling control.
constexpr u32 kMaxInternalScale = 6;

struct RenderScale {
    u32 factor = 1;

    constexpr bool native() const noexcept { return factor <= 1u; }
    constexpr u32 apply(u32 guest_pixels) const noexcept {
        return guest_pixels * (factor == 0u ? 1u : factor);
    }
    // Guest units from native units, rounding down. Used for the resolve back
    // to guest dimensions.
    constexpr u32 unapply(u32 native_pixels) const noexcept {
        return native_pixels / (factor == 0u ? 1u : factor);
    }
    constexpr PixelRect apply(const PixelRect& guest) const noexcept {
        const u32 f = factor == 0u ? 1u : factor;
        return PixelRect{guest.left * f, guest.top * f, guest.width * f, guest.height * f};
    }
};

// Everything the host needs to size and address its render targets. One value
// of this describes the whole EFB-side of the pipeline; nothing downstream
// should ever recompute a dimension from a literal.
struct EfbGeometry {
    RenderScale scale{};

    constexpr u32 guest_width() const noexcept { return kGuestEfbWidth; }
    constexpr u32 guest_height() const noexcept { return kGuestEfbHeight; }
    constexpr u32 native_width() const noexcept { return scale.apply(kGuestEfbWidth); }
    constexpr u32 native_height() const noexcept { return scale.apply(kGuestEfbHeight); }
    constexpr u64 native_texels() const noexcept {
        return static_cast<u64>(native_width()) * native_height();
    }
    constexpr u64 guest_texels() const noexcept {
        return static_cast<u64>(kGuestEfbWidth) * kGuestEfbHeight;
    }
    // The region the guest actually rasterises into, in guest units. Anything
    // outside it is initialised but is not image data.
    constexpr PixelRect guest_raster() const noexcept {
        return PixelRect{0u, 0u, kGuestRasterWidth, kGuestRasterHeight};
    }
    // The same region in native units - what a full-target clear must cover if
    // it is to leave no undefined pixel anywhere the presenter can reach.
    constexpr PixelRect native_raster() const noexcept {
        return scale.apply(guest_raster());
    }
    // The part of the native target that no draw will ever touch. Non-empty by
    // construction (48 guest rows), which is why the target must be cleared in
    // full at creation and not merely where the guest draws.
    constexpr u32 unwritten_guest_rows() const noexcept {
        return kGuestEfbHeight - kGuestRasterHeight;
    }
};

// ---------------------------------------------------------------------------
// Copies
// ---------------------------------------------------------------------------

// A GX copy, resolved into every space it touches at once, so a source and a
// destination can never be clamped independently.
//
// `source_guest` is what the guest asked for, clamped into the EFB.
// `source_native` is where those texels physically live.
// `destination` is where they must land - ALWAYS in guest units, because the
// guest's own texture coordinates address it.
struct ResolvedCopy {
    PixelRect source_guest{};
    PixelRect source_native{};
    PixelRect destination{};
    bool clamped = false;  // the request did not fit the EFB and was reduced
    bool to_xfb = false;
    u32 format = 0;

    constexpr bool consistent() const noexcept {
        return source_guest.width == destination.width &&
               source_guest.height == destination.height;
    }
};

// Build a copy from the guest's request. Both rectangles are derived from ONE
// clamp, which is the invariant the old code could not state: `perform_copy`
// clamped width and height with min() while leaving the origin alone, and the
// destination rectangle was reconstructed separately from the same raw request.
constexpr ResolvedCopy resolve_copy(const EfbGeometry& efb, const PixelRect& requested,
                                    bool to_xfb, u32 format) noexcept {
    ResolvedCopy copy;
    copy.source_guest = clamp_into(requested, efb.guest_width(), efb.guest_height());
    copy.clamped = copy.source_guest != requested;
    copy.source_native = efb.scale.apply(copy.source_guest);
    // The destination is the source's size in guest units, at the origin the
    // destination surface defines. It is never the native size: a raised
    // internal resolution must resolve DOWN here or every UV the guest computed
    // against this surface is wrong.
    copy.destination = PixelRect{0u, 0u, copy.source_guest.width, copy.source_guest.height};
    copy.to_xfb = to_xfb;
    copy.format = format;
    return copy;
}

// ---------------------------------------------------------------------------
// The GX copy filter (BP 0x53 / 0x54)
// ---------------------------------------------------------------------------
//
// Seven 6-bit taps summing to 64, spanning three EFB rows. Grouping, per GX:
// taps 0-1 are row y-1, taps 2-4 are row y, taps 5-6 are row y+1.
//
// MEASURED on the Westopolis route (see the "EFB copy semantics census"):
//   texture copies (477 of 526): [0,0,21,22,21,0,0] -> rows [0, 64, 0] = identity
//   XFB copies      (49 of 526): [8,8,10,12,10,8,8] -> rows [16, 32, 16]
//                                                    = exactly 1/4, 1/2, 1/4
//
// So ignoring the filter is CORRECT for every texture copy and WRONG for every
// XFB copy: the console applies a 1-2-1 vertical low-pass to the image it
// scans out, and the host does not.
struct CopyFilter {
    u32 row_above = 0;    // sum of taps 0..1
    u32 row_current = 64; // sum of taps 2..4
    u32 row_below = 0;    // sum of taps 5..6

    constexpr u32 total() const noexcept { return row_above + row_current + row_below; }
    constexpr bool identity() const noexcept {
        return row_above == 0u && row_below == 0u;
    }
};

constexpr CopyFilter decode_copy_filter(u32 filter0, u32 filter1) noexcept {
    const u32 w0 = filter0 & 0x3fu;
    const u32 w1 = (filter0 >> 6) & 0x3fu;
    const u32 w2 = (filter0 >> 12) & 0x3fu;
    const u32 w3 = (filter0 >> 18) & 0x3fu;
    const u32 w4 = filter1 & 0x3fu;
    const u32 w5 = (filter1 >> 6) & 0x3fu;
    const u32 w6 = (filter1 >> 12) & 0x3fu;
    return CopyFilter{w0 + w1, w2 + w3 + w4, w5 + w6};
}

// Every field of the copy trigger, named. The host consumes three of them; the
// rest are recorded so that "we ignore this" is a measured decision with a
// place to assert it, rather than an omission nobody can see.
struct CopyControl {
    bool clamp_top = false;      // bit 0  - filter clamps instead of reading past the edge
    bool clamp_bottom = false;   // bit 1
    bool yuv = false;            // bit 2
    u32 format_nibble = 0;       // bits 3-6
    u32 gamma = 0;               // bits 7-8   (0 = 1.0, 1 = 1.7, 2 = 2.2)
    bool half_scale = false;     // bit 9
    bool scale_invert = false;   // bit 10
    bool clear = false;          // bit 11
    u32 frame_to_field = 0;      // bits 12-13
    bool to_xfb = false;         // bit 14
    bool intensity = false;      // bit 15
    bool auto_conv = false;      // bit 16

    constexpr u32 format() const noexcept {
        return format_nibble / 2u + (format_nibble & 1u) * 8u;
    }
    // True when every field the host ignores is at its inert value, so the
    // copy can be serviced by the modern path with no loss of guest-visible
    // behaviour. `gamma`, `half_scale`, `scale_invert`, `frame_to_field`,
    // `intensity` and `yuv` are all zero on both of this game's copies.
    constexpr bool ignorable_fields_are_inert() const noexcept {
        return !yuv && gamma == 0u && !half_scale && !scale_invert &&
               frame_to_field == 0u && !intensity;
    }
};

constexpr CopyControl decode_copy_control(u32 execute) noexcept {
    CopyControl control;
    control.clamp_top = (execute & 0x1u) != 0u;
    control.clamp_bottom = (execute & 0x2u) != 0u;
    control.yuv = (execute & 0x4u) != 0u;
    control.format_nibble = (execute >> 3) & 0x0fu;
    control.gamma = (execute >> 7) & 0x3u;
    control.half_scale = (execute & 0x200u) != 0u;
    control.scale_invert = (execute & 0x400u) != 0u;
    control.clear = (execute & 0x800u) != 0u;
    control.frame_to_field = (execute >> 12) & 0x3u;
    control.to_xfb = (execute & 0x4000u) != 0u;
    control.intensity = (execute & 0x8000u) != 0u;
    control.auto_conv = (execute & 0x10000u) != 0u;
    return control;
}

// BP 0x4E, PE_COPY_YSCALE: 1.8 fixed point. 0x100 is 1.0. Measured 0x000100 on
// every copy this game issues, so the host's unconditional 1:1 resolve is
// correct - but it is correct by measurement, and this is where that is
// checked rather than assumed.
constexpr bool copy_y_scale_is_unity(u32 y_scale) noexcept {
    return (y_scale & 0x1ffu) == 0x100u;
}

// ---------------------------------------------------------------------------
// Texel footprint of a draw
// ---------------------------------------------------------------------------

// Texels of a width x height texture that one guest pixel spans, along the
// LESS minified screen axis, for an affine triangle given in guest screen
// pixels with normalised UVs. 1.0 is a 1:1 draw, 5.0 the glare ladder's 5:1
// downsample, 0.2 its 1:5 bloom composite. 0 for a degenerate triangle.
// Consumed by the resident-copy footprint reconstruction, which must not
// apply to magnifying draws.
inline float texel_footprint(float x0, float y0, float u0, float v0, float x1, float y1,
                             float u1, float v1, float x2, float y2, float u2, float v2,
                             u32 width, u32 height) noexcept {
    const float dx1 = x1 - x0;
    const float dy1 = y1 - y0;
    const float dx2 = x2 - x0;
    const float dy2 = y2 - y0;
    const float det = dx1 * dy2 - dx2 * dy1;
    if (!(std::fabs(det) > 0.0f) || !std::isfinite(det)) return 0.0f;
    const float du1 = u1 - u0;
    const float dv1 = v1 - v0;
    const float du2 = u2 - u0;
    const float dv2 = v2 - v0;
    const float dudx = (du1 * dy2 - du2 * dy1) / det;
    const float dvdx = (dv1 * dy2 - dv2 * dy1) / det;
    const float dudy = (du2 * dx1 - du1 * dx2) / det;
    const float dvdy = (dv2 * dx1 - dv1 * dx2) / det;
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float along_x = std::max(std::fabs(dudx) * w, std::fabs(dvdx) * h);
    const float along_y = std::max(std::fabs(dudy) * w, std::fabs(dvdy) * h);
    const float footprint = std::min(along_x, along_y);
    return std::isfinite(footprint) ? footprint : 0.0f;
}

// ---------------------------------------------------------------------------
// Aspect mapping
// ---------------------------------------------------------------------------

// The Hor+ aspect mapping that used to live here had ZERO callers while three
// other copies of the same arithmetic were live. It is now in gekko/camera.h as
// camera::horizontal_extent_scale(), where it changes a physical frustum extent
// rather than patching a finished matrix, and where the culling frustum is
// derived from the same extent. Do not re-add a copy here.

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

struct SurfaceRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    constexpr bool covers(int surface_width, int surface_height) const noexcept {
        return x == 0 && y == 0 && width == surface_width && height == surface_height;
    }
};

// Centre `content_aspect` inside the surface. Split so the letterbox bars can
// never differ by more than one pixel and the total always fills the surface
// exactly - the caller is then responsible for clearing exactly
// `surface - content`, with no rounding gap for a stale pixel to live in.
struct PresentGeometry {
    SurfaceRect content{};
    int surface_width = 0;
    int surface_height = 0;

    // Columns/rows outside the content rect, which MUST be written every
    // present. Non-zero here is the only reason a clear is needed at all.
    constexpr int bar_left() const noexcept { return content.x; }
    constexpr int bar_right() const noexcept {
        return surface_width - (content.x + content.width);
    }
    constexpr int bar_top() const noexcept { return content.y; }
    constexpr int bar_bottom() const noexcept {
        return surface_height - (content.y + content.height);
    }
    constexpr bool needs_clear() const noexcept {
        return bar_left() > 0 || bar_right() > 0 || bar_top() > 0 || bar_bottom() > 0;
    }
    // Every surface pixel is either content or bar. If this is ever false the
    // presenter has a region it neither draws nor clears.
    constexpr bool partitions_surface() const noexcept {
        return bar_left() >= 0 && bar_right() >= 0 && bar_top() >= 0 && bar_bottom() >= 0 &&
               bar_left() + content.width + bar_right() == surface_width &&
               bar_top() + content.height + bar_bottom() == surface_height;
    }
};

PresentGeometry fit_present_geometry(int surface_width, int surface_height,
                                     float content_aspect) noexcept;

}  // namespace gekko
