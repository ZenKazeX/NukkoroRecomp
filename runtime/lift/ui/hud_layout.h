#pragma once

// THE GAME'S 2D LAYOUT POLICY (NE-RW2c, HW_REPORT Y; docs/2D_LAYOUT_ARCHITECTURE.md). Where each screen-space draw of
// the game's 640 x 480 canvas stays on an output of another aspect: the game's own group / cast / glyph-run
// announcements (System's recovered layout hooks) open and close groups, every canvas draw is classified into an
// anchor (an edge, the centre, or covering the canvas), and anchors are remembered per (draw site, instance), per
// authored cast and per player region across frames, with hysteresis. Plain values only: no guest memory, no console
// structs, no renderer. Moved here from include/gekko/native_hud.h (NativeHudLayout) and display.{h,cpp} (the
// classifiers), where it sat beside the legacy GX/EFB renderer's display model; the legacy renderer keeps its own
// canvas affine (gekko::hud_affine_for_anchor) on top of this.
//
// One thread (the game thread in the lifted renderer).

#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace shadow::ui {

// The game's 2D canvas in 60 Hz mode: the default until a bracket states its own (HudLayout::set_regions). The canvas
// is the camera raster the 2D is drawn on - 640 x 528 in PAL 50 Hz mode - so every rule below takes the canvas it
// classifies in rather than assuming these.
inline constexpr float kHudCanvasWidth = 640.0f;
inline constexpr float kHudCanvasHeight = 480.0f;

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
    Canvas,  // full virtual frame, kept 4:3 and centred (menus, results)
    Fill,    // full virtual frame, covers the output (fades)
    Cover,   // a textured full-frame draw (a background): its own aspect, scaled to cover the output, overflow cut
};

// Which logical 2D group the game announced (the recovered layout hooks in system.cpp); an announcement describes the
// draws that follow it.
enum class HudGroupKind : std::uint8_t {
    // Not inside any announced 2D group: screen-space effects, lens flare,
    // lock-on marks, off-screen arrows - everything the game positions from a
    // world-space projection.
    Ungrouped = 0,
    // A run of HUD glyphs whose anchor the guest stated outright.
    Text = 1,
    // One Chao::CSD project - one widget. Anchored as a whole.
    CsdWidget = 2,
    // A draw that tiles the screen. The recovered fade submits its curtain as
    // ten separate 64x480 columns, so no one primitive looks full-screen and
    // the whole curtain would otherwise be anchored inwards, leaving the
    // outer eighth of a widescreen frame unfaded.
    ScreenCover = 3,
    // One Chao::CSD project that draws SEVERAL spatially separate panels, so
    // the project is the wrong unit to anchor.
    //
    // The mission objective window is the
    // case: one Project::Render draws both objective panels, and their
    // counters are drawn by a two-entry loop whose X comes from the recovered
    // layout table: entry 0 at X = 24, entry 1 at X = 545. Original locations
    // and record stride are documented in docs/NATIVE_SOURCE_PROVENANCE.md.
    // Those two counters take the Left and the Right anchor by canvas thirds,
    // while the art's union straddles the middle and takes Center, so a panel
    // and the counter printed on it separate. Anchoring per authored cast puts
    // each panel on the side its own counter is on.
    //
    // Degrades gracefully: a cast that straddles the middle still classifies
    // Center, which is exactly the per-project behaviour, so this can only
    // change a cast that lies wholly on one side - the case that is wrong.
    CsdWidgetPerCast = 4,
};

struct HudBounds {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

HudAnchor classify_hud_anchor(const HudBounds& bounds, bool textured, bool blending,
                              float canvas_width = kHudCanvasWidth, float canvas_height = kHudCanvasHeight) noexcept;
// True when a draw spans essentially the whole canvas WIDTH. Such a draw is a
// fade, a full-screen overlay, a background or a letterbox band, and its X must
// be left alone whatever its height: anchoring it would pull its edges inwards
// and leave uncovered strips at the sides. Height deliberately does not
// participate - a full-width band 200 units tall covers the screen horizontally
// just as much as one 480 units tall.
bool spans_canvas_width(const HudBounds& bounds, float canvas_width = kHudCanvasWidth) noexcept;
// True when a draw spans essentially the whole canvas HEIGHT, which is what
// distinguishes one column of a screen-covering curtain from the content drawn
// on top of it.
bool spans_canvas_height(const HudBounds& bounds, float canvas_height = kHudCanvasHeight) noexcept;
// True when a draw reaches past a side edge of the canvas by more than the coverage test's slack: with
// spans_canvas_width, a sprite zoomed larger than the screen (the title logo's after-images), where a band or a
// backdrop is authored at the frame.
bool overflows_canvas_width(const HudBounds& bounds, float canvas_width = kHudCanvasWidth) noexcept;
// Which side of the canvas a logical group belongs to, from the group's own
// bounds. `previous` is the anchor the group had on the last frame, which is
// held on to unless the group has moved decisively past the centre; without
// that, a group animating across a classification boundary snaps.
// `canvas_width`: the width of what the group is classified in - the whole
// canvas, or a player's viewport region of it in split screen (the bounds then
// in that region's own coordinates).
HudAnchor classify_group_anchor(const HudBounds& bounds, HudAnchor previous, bool have_previous,
                                float canvas_width = kHudCanvasWidth) noexcept;

class HudLayout {
public:
    void on_group(HudGroupKind kind, std::uint32_t group_id, std::uint32_t instance, float canvas_x) noexcept;
    void on_scene(std::uint32_t node, std::uint32_t cast) noexcept;
    void end_frame() noexcept;

    // The anchors remembered across frames, for a savestate (the lifted renderer's block): written and restored as
    // they are; a frame's open group and cast are not part of it.
    void save_memory(std::vector<std::uint8_t>& out) const;
    bool load_memory(const std::uint8_t*& cursor, const std::uint8_t* end);
    // The anchors remembered per player region (set_regions), kept apart so the block above keeps its format.
    void save_region_memory(std::vector<std::uint8_t>& out) const;
    bool has_region_memory() const noexcept { return !region_memory_.empty(); }
    void clear_region_memory() noexcept { region_memory_.clear(); }
    bool load_region_memory(const std::uint8_t*& cursor, const std::uint8_t* end);

    // The canvas the next draws are classified in (`canvas_width` x `canvas_height`: the bracket's camera raster,
    // 640 x 528 in PAL 50 Hz mode) and its regions the next draws are laid out in: in split screen the players'
    // viewports inside the full-screen canvas the overlay is drawn on (the game composes each player's HUD inside its own viewport
    // rectangle - docs/renderer-re-20261007/07_2D.md 3.11; the 2P ring icons and counters, the command windows, the
    // item icons). None (the default, and always in single player): the whole canvas, as before. Canvas units of the
    // view's canvas, which is `canvas_width` x `canvas_height`.
    struct Region {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    };
    void set_regions(const Region* regions, std::size_t count, float canvas_width, float canvas_height);

    // One screen-space draw. `bounds` are the draw's canvas coordinates as the
    // game placed it. Returns its anchor and whether the draw was laid out at
    // all. `shapes_anchor` false: the draw puts nothing on screen (every vertex
    // fully transparent under a source-alpha blend - a widget's hidden part), so
    // it takes its group's anchor but does not widen the bounds that decide it.
    struct Decision {
        bool laid_out = false;
        HudAnchor anchor = HudAnchor::Center;
        // The region (set_regions) the draw is laid out in, its anchor that region's; -1: the whole canvas.
        int region = -1;
        // A sprite zoomed past the canvas's side (overflows_canvas_width): the console's screen edge cut it, so on a
        // wider view it is cut at the view's edge rather than the canvas box's (render::Anchor::Continued when
        // centred).
        bool beyond_canvas = false;
    };
    Decision layout_draw(const HudBounds& bounds, bool textured, bool shapes_anchor = true) noexcept;

    struct Stats {
        std::uint64_t laid_out = 0;
        std::uint64_t off_canvas = 0;
        std::uint64_t groups_open_at_frame_end = 0;
        std::uint64_t group_memory_capped = 0;
        std::uint64_t in_regions = 0;  // draws laid out in a player region
        std::uint64_t zoomed_sprites = 0;  // full-width textured draws kept with their widget (overflows_canvas_width)
    };
    // Diagnostics: true restores the earlier rule (a zoomed sprite across the canvas width fills the view), for a
    // same-frame A/B (SHADOW_2D_ZOOM_FILL=1, read by the renderer).
    void set_zoomed_sprites_fill(bool fill) noexcept { zoomed_sprites_fill_ = fill; }
    const Stats& stats() const noexcept { return stats_; }
    std::size_t group_memory_size() const noexcept { return group_memory_.size(); }
    std::size_t cast_memory_size() const noexcept { return cast_memory_.size(); }

private:
    struct Instance {
        HudAnchor anchor = HudAnchor::Center;
        bool have_anchor = false;
    };
    void close_group() noexcept;
    void close_cast() noexcept;
    int region_of(const HudBounds& bounds) const noexcept;
    int region_of_point(float x) const noexcept;
    HudAnchor region_anchor(int region, const HudBounds& bounds, bool shapes_anchor) noexcept;

    HudGroupKind group_kind_ = HudGroupKind::Ungrouped;
    std::uint32_t group_id_ = 0;
    std::pair<std::uint32_t, std::uint32_t> group_key_{};
    HudBounds group_bounds_{};
    bool group_bounds_valid_ = false;
    bool group_open_ = false;
    HudAnchor group_anchor_ = HudAnchor::Center;
    bool group_anchor_decided_ = false;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Instance> group_memory_;
    std::map<std::uint32_t, std::uint32_t> group_ordinals_;

    std::uint32_t cast_id_ = 0;
    std::pair<std::uint32_t, std::uint32_t> cast_key_{};
    HudBounds cast_bounds_{};
    bool cast_bounds_valid_ = false;
    bool cast_open_ = false;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Instance> cast_memory_;

    std::vector<Region> regions_;
    float region_canvas_width_ = 0.0f, region_canvas_height_ = 0.0f;
    // The canvas the draws are classified in (set_regions); the 60 Hz canvas until a bracket states one.
    float canvas_width_ = kHudCanvasWidth, canvas_height_ = kHudCanvasHeight;
    float group_text_x_ = 0.0f;  // a glyph run's alignment point
    // The open group's draws in each region, in that region's own coordinates (its top-left corner the origin).
    std::vector<HudBounds> region_bounds_;
    std::vector<bool> region_bounds_valid_;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, Instance> region_memory_;  // (site, instance, region)

    bool zoomed_sprites_fill_ = false;

    Stats stats_{};
};

}  // namespace shadow::ui
