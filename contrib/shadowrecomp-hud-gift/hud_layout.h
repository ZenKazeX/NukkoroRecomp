#pragma once

// HUD layout policy for a game that draws its 2D on a fixed 4:3 canvas (640 x 480 in Shadow the Hedgehog), shown on
// an output of any aspect ratio.
//
// The problem: the canvas is stretched over the whole output, so on a wide screen every 2D draw has to be squeezed
// back to square pixels and moved to some point. That point should be the canvas's left edge, its centre, or its right
// edge, depending on where the art "belongs". The game never says where, so this file works it out from what is drawn:
//
//   1. Each draw's canvas bounds are classified into an anchor (an edge, a corner, the centre, or "covers the canvas").
//   2. Draws are grouped into logical widgets (one CSD project, one cast, one text run) using the hooks the caller
//      reports. A widget is anchored as a WHOLE, so its pieces never separate.
//   3. Anchors are remembered across frames, with hysteresis, so a widget that slides in or out does not snap from
//      one side to the other halfway through its animation.
//
// Placement (turning an anchor into a transform) is in hud_anchor_affine.h. Everything here uses plain values only:
// no guest memory, no renderer. It is meant to run on one thread (the thread that submits the 2D).
//
// Typical frame:
//
//     layout.on_group(HudGroupKind::CsdWidget, site, instance, 0.0f);   // a CSD project starts rendering
//     layout.on_scene(node, cast);                                      // optional: which cast is drawing
//     HudLayout::Decision d = layout.layout_draw(bounds, textured);     // for every quad
//     ...
//     layout.end_frame();                                               // after the frame's 2D pass
//
// No license, no conditions: use, change and share this however you like.

#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace shadow::ui {

// The game's 2D canvas. Every rule also accepts another canvas (for example 640 x 528 in PAL 50 Hz mode, or one
// player's 640 x 240 view in split screen), so these are only defaults.
inline constexpr float kHudCanvasWidth = 640.0f;
inline constexpr float kHudCanvasHeight = 480.0f;

// Where a draw is pinned on a wider output.
enum class HudAnchor : std::uint8_t {
    TopLeft = 0,
    Top,
    TopRight,
    Left,
    Center,
    Right,
    BottomLeft,
    Bottom,
    BottomRight,
    Canvas,  // A textured draw over the whole canvas (a menu background): kept 4:3 and centred.
    Fill,    // An untextured draw over the whole canvas (a fade, a letterbox band): stretched over the whole output.
    Cover,   // Reserved for a renderer that scales a background up to cover the output. Never returned by this file.
};

// What kind of logical group the next draws belong to. The caller reports this from its own hooks (on_group).
enum class HudGroupKind : std::uint8_t {
    // No group: screen-space effects, lock-on marks, off-screen arrows. These are positioned from a world-space
    // projection, so they stay on the centre anchor (the same squeeze the 3D gets).
    Ungrouped = 0,

    // A run of HUD glyphs. The caller passes the run's alignment X instead of bounds, because text reports one
    // reference point, not an extent.
    Text = 1,

    // One CSD project, which is one widget. It is anchored as a whole.
    CsdWidget = 2,

    // A curtain that tiles the screen. The fade is drawn as ten 64 x 480 columns, so no single quad looks
    // full-screen. Inside this group, every untextured full-height column fills the output.
    ScreenCover = 3,

    // One CSD project that draws several separate panels, so each cast is anchored on its own (report casts with
    // on_scene). Example: the mission objective window draws two panels, with counters at X = 24 and X = 545. As one
    // unit it straddles the middle and goes to Center while its counters go Left and Right, and they leave their
    // panels. A cast that itself straddles the middle still gets Center, so this only changes casts that lie wholly on
    // one side.
    CsdWidgetPerCast = 4,
};

// An axis-aligned rectangle in canvas units. The corners may come in either order.
struct HudBounds {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

// ---- Stateless classifiers ------------------------------------------------------------------------------------------

// The anchor of one draw taken on its own: a 3 x 3 grid of the canvas, or Canvas / Fill when it covers the canvas.
HudAnchor classify_hud_anchor(const HudBounds& bounds, bool textured, float canvas_width = kHudCanvasWidth,
                              float canvas_height = kHudCanvasHeight) noexcept;

// True when a draw reaches BOTH side edges of the canvas (with a few units of slack). Such a draw is a fade, an
// overlay, a background or a band, and must not be moved inwards, whatever its height. Being wide is not enough: the
// autosave dialog is 588 units wide (92% of the canvas), but it is inset on both sides and is content.
bool spans_canvas_width(const HudBounds& bounds, float canvas_width = kHudCanvasWidth) noexcept;

// True when a draw covers at least 90% of the canvas height. This tells one column of a fade curtain apart from the
// content drawn on top of it.
bool spans_canvas_height(const HudBounds& bounds, float canvas_height = kHudCanvasHeight) noexcept;

// True when a draw reaches past either side edge by more than the slack. Together with spans_canvas_width, this is
// a sprite zoomed larger than the screen (the title logo's after-images), not a band authored at the frame.
bool overflows_canvas_width(const HudBounds& bounds, float canvas_width = kHudCanvasWidth) noexcept;

// The horizontal anchor of a whole group (Left, Center or Right) from the group's combined bounds.
//   - A group that lies wholly in one half of the canvas takes that side, however close to the middle it reaches.
//   - A point (a text run's alignment X, x0 == x1) uses thirds of the canvas.
//   - With a `previous` anchor, a group changes side only after its centre is a quarter of the canvas past the
//     middle (hysteresis). Text runs skip this, because their X is fixed.
HudAnchor classify_group_anchor(const HudBounds& bounds, HudAnchor previous, bool have_previous,
                                float canvas_width = kHudCanvasWidth) noexcept;

// ---- Stateful layout ------------------------------------------------------------------------------------------------

class HudLayout {
public:
    // A rectangle of the canvas that has its own HUD, such as one player's viewport in split screen.
    struct Region {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    };

    // What layout_draw decided for one draw.
    struct Decision {
        bool laid_out = false;               // false: the draw is parked outside the canvas on purpose. Leave it alone.
        HudAnchor anchor = HudAnchor::Center;
        int region = -1;                     // The region (set_regions) the anchor is relative to, or -1 for the canvas.
        bool beyond_canvas = false;          // A sprite zoomed past the canvas edge: clip it at the output's edge,
                                             // not at the edge of the centred 4:3 box.
    };

    struct Stats {
        std::uint64_t laid_out = 0;
        std::uint64_t off_canvas = 0;
        std::uint64_t groups_open_at_frame_end = 0;
        std::uint64_t memory_capped = 0;     // groups not remembered because a memory map was full
        std::uint64_t in_regions = 0;        // draws laid out inside a region
        std::uint64_t zoomed_sprites = 0;    // draws flagged beyond_canvas
    };

    // A new group starts. `site` identifies the code that draws it (for example a hash of the CSD file path).
    // `instance` identifies the object (for example the project pointer). 0 means "none", and then the n-th group
    // from the same site in a frame is treated as instance n. `text_x` is a Text group's alignment X; other kinds
    // ignore it. Starting a group closes the previous one.
    void on_group(HudGroupKind kind, std::uint32_t site, std::uint32_t instance, float text_x) noexcept;

    // The cast that the next draws come from. This only matters inside a CsdWidgetPerCast group.
    void on_scene(std::uint32_t node, std::uint32_t cast) noexcept;

    // Lays out one draw. `bounds` are its canvas coordinates as the game placed it. Pass `shapes_anchor` = false for a
    // draw that puts nothing on screen (every vertex fully transparent): it follows its group's anchor but does not
    // widen the group's bounds. The Egg Dealer slot display draws a hidden panel across the middle every frame, and
    // counting it pulled the visible panel off its edge.
    Decision layout_draw(const HudBounds& bounds, bool textured, bool shapes_anchor = true) noexcept;

    // Closes the frame's open group and cast. Their anchors are decided now and used from the next frame on.
    void end_frame() noexcept;

    // Sets the canvas the next draws are measured in, and the regions they are laid out in. No regions (the default,
    // and always the case in single player) means the whole canvas.
    void set_regions(const Region* regions, std::size_t count, float canvas_width, float canvas_height);

    // Saves or restores the remembered anchors, for savestates. The open group and cast are not included.
    // load_* returns false, and changes nothing, if the data is truncated or malformed.
    void save_memory(std::vector<std::uint8_t>& out) const;
    bool load_memory(const std::uint8_t*& cursor, const std::uint8_t* end);
    void save_region_memory(std::vector<std::uint8_t>& out) const;
    bool load_region_memory(const std::uint8_t*& cursor, const std::uint8_t* end);
    bool has_region_memory() const noexcept { return !region_memory_.empty(); }
    void clear_region_memory() noexcept { region_memory_.clear(); }

    const Stats& stats() const noexcept { return stats_; }
    std::size_t group_memory_size() const noexcept { return group_memory_.size(); }
    std::size_t cast_memory_size() const noexcept { return cast_memory_.size(); }

private:
    // A remembered anchor. `known` is false until a group has been seen closing at least once.
    struct Memory {
        HudAnchor anchor = HudAnchor::Center;
        bool known = false;
    };
    using Key = std::pair<std::uint32_t, std::uint32_t>;                       // (site, instance) or (site, cast)
    using RegionKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;  // (site, instance, region)

    // Each map is capped so a game that keeps making new instances cannot grow it without bound.
    static constexpr std::size_t kMemoryCap = 4096;

    // Grows a bounds accumulator to include `bounds`. The first call (valid == false) just copies.
    static void accumulate(HudBounds& into, bool& valid, const HudBounds& bounds) noexcept;

    void close_group() noexcept;
    void close_cast() noexcept;
    void reset_group() noexcept;
    int region_of(const HudBounds& bounds) const noexcept;
    int region_of_point(float x) const noexcept;
    HudAnchor region_anchor(int region, const HudBounds& bounds, bool shapes_anchor) noexcept;
    HudAnchor group_draw_anchor() noexcept;

    // The canvas the draws are measured in.
    float canvas_width_ = kHudCanvasWidth;
    float canvas_height_ = kHudCanvasHeight;

    // The open group.
    HudGroupKind group_kind_ = HudGroupKind::Ungrouped;
    bool group_open_ = false;
    Key group_key_{};
    float group_text_x_ = 0.0f;
    HudBounds group_bounds_{};
    bool group_bounds_valid_ = false;
    HudAnchor group_anchor_ = HudAnchor::Center;  // the anchor its draws take this frame
    std::map<Key, Memory> group_memory_;
    std::map<std::uint32_t, std::uint32_t> group_ordinals_;  // per site: groups seen this frame without an instance

    // The open cast (CsdWidgetPerCast only).
    std::uint32_t cast_id_ = 0;
    bool cast_open_ = false;
    Key cast_key_{};
    HudBounds cast_bounds_{};
    bool cast_bounds_valid_ = false;
    std::map<Key, Memory> cast_memory_;

    // Regions (split screen). Each region's group bounds are kept in that region's own coordinates.
    std::vector<Region> regions_;
    float region_canvas_width_ = 0.0f, region_canvas_height_ = 0.0f;
    std::vector<HudBounds> region_bounds_;
    std::vector<bool> region_bounds_valid_;
    std::map<RegionKey, Memory> region_memory_;

    Stats stats_{};
};

}  // namespace shadow::ui
