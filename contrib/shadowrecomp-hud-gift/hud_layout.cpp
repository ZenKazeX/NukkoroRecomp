#include "hud_layout.h"

#include <algorithm>
#include <cmath>

// See hud_layout.h for the overview. Every number below was measured from the game's own 640 x 480 HUD.
// No license, no conditions: use, change and share this however you like.

namespace shadow::ui {

namespace {

// Art authored "at the frame" lands a few units off it: full-frame draws come out at 0.5..640.5 after the viewport
// mapping, and some art is drawn one or two units inside or outside the frame.
constexpr float kEdgeSlack = 8.0f;

// A draw counts as full height at 90% of the canvas.
constexpr float kFullHeightShare = 0.90f;

// Widgets change side only once their centre is this share of the canvas width past the middle.
constexpr float kHysteresisShare = 0.25f;

// The bounds with x0 <= x1 and y0 <= y1.
HudBounds normalized(const HudBounds& b) noexcept {
    return {std::min(b.x0, b.x1), std::min(b.y0, b.y1), std::max(b.x0, b.x1), std::max(b.y0, b.y1)};
}

// Which third of [0, extent) a span falls in: 0, 1 or 2. A span that lies wholly in one half takes that half's
// outer third, even if its centre is in the middle third.
int grid_cell(float lo, float hi, float extent) noexcept {
    const float centre = (lo + hi) * 0.5f;
    if (hi <= extent * 0.5f || centre < extent / 3.0f) return 0;
    if (lo >= extent * 0.5f || centre > extent * 2.0f / 3.0f) return 2;
    return 1;
}

// ---- Little-endian serialization for savestates --------------------------------------------------------------------

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool get_u32(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint32_t& value) {
    if (end - cursor < 4) return false;
    value = 0;
    for (int shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(*cursor++) << shift;
    return true;
}

// A record count larger than this is treated as corrupt data.
constexpr std::uint32_t kMaxRecords = 1u << 20;

}  // namespace

// ---- Stateless classifiers ------------------------------------------------------------------------------------------

HudAnchor classify_hud_anchor(const HudBounds& bounds, bool textured, float canvas_width,
                              float canvas_height) noexcept {
    if (spans_canvas_width(bounds, canvas_width) && spans_canvas_height(bounds, canvas_height)) {
        return textured ? HudAnchor::Canvas : HudAnchor::Fill;
    }
    static constexpr HudAnchor kGrid[3][3] = {
        {HudAnchor::TopLeft, HudAnchor::Top, HudAnchor::TopRight},
        {HudAnchor::Left, HudAnchor::Center, HudAnchor::Right},
        {HudAnchor::BottomLeft, HudAnchor::Bottom, HudAnchor::BottomRight},
    };
    const HudBounds b = normalized(bounds);
    return kGrid[grid_cell(b.y0, b.y1, canvas_height)][grid_cell(b.x0, b.x1, canvas_width)];
}

bool spans_canvas_width(const HudBounds& bounds, float canvas_width) noexcept {
    // Both edges must be reached. Width alone is not enough: an inset dialog 92% wide must keep its proportions, or its
    // body is stretched apart from its own narrow side borders, which are anchored normally.
    const HudBounds b = normalized(bounds);
    return b.x0 <= kEdgeSlack && b.x1 >= canvas_width - kEdgeSlack;
}

bool spans_canvas_height(const HudBounds& bounds, float canvas_height) noexcept {
    return std::fabs(bounds.y1 - bounds.y0) >= canvas_height * kFullHeightShare;
}

bool overflows_canvas_width(const HudBounds& bounds, float canvas_width) noexcept {
    // Either side is enough. The title's after-images scale about the wordmark's centre (x 287), not the canvas
    // centre, so one can be far past the left edge but only a few units past the right (-72.4..646.4).
    const HudBounds b = normalized(bounds);
    return b.x0 < -kEdgeSlack || b.x1 > canvas_width + kEdgeSlack;
}

HudAnchor classify_group_anchor(const HudBounds& bounds, HudAnchor previous, bool have_previous,
                                float canvas_width) noexcept {
    const HudBounds b = normalized(bounds);
    const float centre = (b.x0 + b.x1) * 0.5f;
    const float middle = canvas_width * 0.5f;
    const bool point_like = b.x1 - b.x0 < 1.0f;

    HudAnchor candidate = HudAnchor::Center;
    if (point_like) {
        // A text run: thirds. The HUD's text X values (54, 78, 84, 120 / 320 / 477, 520, 560) clear both boundaries
        // easily.
        if (centre < canvas_width / 3.0f) candidate = HudAnchor::Left;
        else if (centre > canvas_width * 2.0f / 3.0f) candidate = HudAnchor::Right;
    } else if (b.x1 <= middle) {
        // Wholly in the left half: left, however close to the middle. A centroid rule would detach the meter icon
        // (x 197.5..229.5) that caps the Hero gauge from the gauge itself.
        candidate = HudAnchor::Left;
    } else if (b.x0 >= middle) {
        candidate = HudAnchor::Right;
    }

    // Text runs never move (their X comes from a fixed layout table), so hysteresis would only make them sticky.
    if (!have_previous || point_like || candidate == previous) return candidate;

    // A widget that has settled keeps its side until it has decisively moved, so an entrance animation that slides
    // across a boundary does not snap halfway through.
    const float hysteresis = canvas_width * kHysteresisShare;
    const bool decisive = (candidate == HudAnchor::Left && centre < middle - hysteresis) ||
                          (candidate == HudAnchor::Right && centre > middle + hysteresis) ||
                          (candidate == HudAnchor::Center && std::fabs(centre - middle) < hysteresis);
    return decisive ? candidate : previous;
}

// ---- HudLayout: groups and casts ------------------------------------------------------------------------------------

void HudLayout::accumulate(HudBounds& into, bool& valid, const HudBounds& bounds) noexcept {
    if (!valid) {
        into = bounds;
        valid = true;
        return;
    }
    into.x0 = std::min(into.x0, bounds.x0);
    into.y0 = std::min(into.y0, bounds.y0);
    into.x1 = std::max(into.x1, bounds.x1);
    into.y1 = std::max(into.y1, bounds.y1);
}

void HudLayout::on_group(HudGroupKind kind, std::uint32_t site, std::uint32_t instance, float text_x) noexcept {
    close_group();

    // Without an instance (glyph runs), the n-th group from this site in this frame is instance n.
    const std::uint32_t key_instance = instance != 0u ? instance : group_ordinals_[site]++;
    group_kind_ = kind;
    group_key_ = {site, key_instance};
    group_open_ = kind != HudGroupKind::Ungrouped;
    group_text_x_ = text_x;
    group_bounds_valid_ = false;
    std::fill(region_bounds_valid_.begin(), region_bounds_valid_.end(), false);

    // The anchor this group's draws take this frame: the one it settled on before, or Center the first time. A text
    // run is decided right now from its alignment point.
    const auto it = group_memory_.find(group_key_);
    const bool remembered = it != group_memory_.end() && it->second.known;
    if (kind == HudGroupKind::Text) {
        group_anchor_ = classify_group_anchor({text_x, 0.0f, text_x, 0.0f}, HudAnchor::Center, false, canvas_width_);
    } else {
        group_anchor_ = remembered ? it->second.anchor : HudAnchor::Center;
    }
}

void HudLayout::on_scene(std::uint32_t node, std::uint32_t cast) noexcept {
    (void)node;
    if (cast != cast_id_) close_cast();
    cast_id_ = cast;
}

void HudLayout::close_group() noexcept {
    if (!group_open_) return;
    group_open_ = false;

    // Each region's part of the group settles that region's anchor, by the same rule in the region's own width.
    for (std::size_t r = 0; r < region_bounds_valid_.size() && r < regions_.size(); ++r) {
        if (!region_bounds_valid_[r]) continue;
        region_bounds_valid_[r] = false;
        const RegionKey key{group_key_.first, group_key_.second, static_cast<std::uint32_t>(r)};
        auto it = region_memory_.find(key);
        if (it == region_memory_.end()) {
            if (region_memory_.size() >= kMemoryCap) {
                ++stats_.memory_capped;
                continue;
            }
            it = region_memory_.emplace(key, Memory{}).first;
        }
        Memory& m = it->second;
        m.anchor = classify_group_anchor(region_bounds_[r], m.anchor, m.known, regions_[r].x1 - regions_[r].x0);
        m.known = true;
    }

    if (group_kind_ == HudGroupKind::Ungrouped || !group_bounds_valid_) return;
    auto it = group_memory_.find(group_key_);
    if (it == group_memory_.end()) {
        if (group_memory_.size() >= kMemoryCap) {
            ++stats_.memory_capped;
            return;
        }
        it = group_memory_.emplace(group_key_, Memory{}).first;
    }
    // Only the anchor belongs to the whole group. Whether a draw covers the canvas is decided per draw: a widget's
    // backdrop can cover while its other parts do not.
    Memory& m = it->second;
    m.anchor = classify_group_anchor(group_bounds_, m.anchor, m.known, canvas_width_);
    m.known = true;
}

void HudLayout::close_cast() noexcept {
    if (!cast_open_) return;
    cast_open_ = false;
    if (!cast_bounds_valid_) return;
    auto it = cast_memory_.find(cast_key_);
    if (it == cast_memory_.end()) {
        if (cast_memory_.size() >= kMemoryCap) {
            ++stats_.memory_capped;
            return;
        }
        it = cast_memory_.emplace(cast_key_, Memory{}).first;
    }
    Memory& m = it->second;
    m.anchor = classify_group_anchor(cast_bounds_, m.known ? m.anchor : HudAnchor::Center, m.known, canvas_width_);
    m.known = true;
}

void HudLayout::reset_group() noexcept {
    group_kind_ = HudGroupKind::Ungrouped;
    group_key_ = {};
    group_open_ = false;
    group_bounds_valid_ = false;
}

void HudLayout::end_frame() noexcept {
    if (group_open_) ++stats_.groups_open_at_frame_end;
    close_group();
    reset_group();
    group_ordinals_.clear();
    std::fill(region_bounds_valid_.begin(), region_bounds_valid_.end(), false);
    close_cast();
    cast_id_ = 0;
    cast_bounds_valid_ = false;
}

// ---- HudLayout: regions (split screen) ------------------------------------------------------------------------------

void HudLayout::set_regions(const Region* regions, std::size_t count, float canvas_width, float canvas_height) {
    const auto same_region = [](const Region& a, const Region& b) {
        return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
    };
    if (count == regions_.size() && canvas_width == region_canvas_width_ && canvas_height == region_canvas_height_ &&
        std::equal(regions, regions + count, regions_.begin(), same_region)) {
        return;
    }
    regions_.assign(regions, regions + count);
    region_canvas_width_ = canvas_width;
    region_canvas_height_ = canvas_height;
    if (canvas_width > 0.0f && canvas_height > 0.0f) {
        canvas_width_ = canvas_width;
        canvas_height_ = canvas_height;
    }
    region_bounds_.assign(count, HudBounds{});
    region_bounds_valid_.assign(count, false);
}

int HudLayout::region_of(const HudBounds& bounds) const noexcept {
    // The region that wholly contains the draw. Edges on the canvas border get the usual slack. Edges shared between
    // two regions get none, so a draw across the split (the 2P timer) belongs to the whole canvas.
    const HudBounds b = normalized(bounds);
    for (std::size_t r = 0; r < regions_.size(); ++r) {
        const Region& g = regions_[r];
        const float left = g.x0 <= 0.0f ? g.x0 - kEdgeSlack : g.x0;
        const float right = g.x1 >= region_canvas_width_ ? g.x1 + kEdgeSlack : g.x1;
        const float top = g.y0 <= 0.0f ? g.y0 - kEdgeSlack : g.y0;
        const float bottom = g.y1 >= region_canvas_height_ ? g.y1 + kEdgeSlack : g.y1;
        if (b.x0 >= left && b.x1 <= right && b.y0 >= top && b.y1 <= bottom) return static_cast<int>(r);
    }
    return -1;
}

int HudLayout::region_of_point(float x) const noexcept {
    // A text run reports no vertical position, so only full-height regions qualify. A point exactly on a shared edge
    // (the 2P timer at x 320) belongs to the whole canvas.
    for (std::size_t r = 0; r < regions_.size(); ++r) {
        const Region& g = regions_[r];
        if (g.y0 <= 0.0f && g.y1 >= region_canvas_height_ && x > g.x0 && x < g.x1) return static_cast<int>(r);
    }
    return -1;
}

HudAnchor HudLayout::region_anchor(int region, const HudBounds& bounds, bool shapes_anchor) noexcept {
    const std::size_t r = static_cast<std::size_t>(region);
    const Region& g = regions_[r];

    // Text inside a region uses halves, not thirds. The 2P ammo counters are printed at x 120 and 520, centred on
    // command windows at the outer edges, and thirds would put them in their regions' middles.
    if (group_kind_ == HudGroupKind::Text) {
        const float local = group_text_x_ - g.x0;
        const float half = 0.5f * (g.x1 - g.x0);
        return local < half ? HudAnchor::Left : local > half ? HudAnchor::Right : HudAnchor::Center;
    }

    // A widget: collect its bounds in the region's own coordinates (decided at close_group) and use the anchor it
    // settled on before, or Center the first time.
    if (group_open_ && shapes_anchor) {
        const HudBounds local{bounds.x0 - g.x0, bounds.y0 - g.y0, bounds.x1 - g.x0, bounds.y1 - g.y0};
        bool valid = region_bounds_valid_[r];
        accumulate(region_bounds_[r], valid, local);
        region_bounds_valid_[r] = valid;
    }
    const auto it = region_memory_.find({group_key_.first, group_key_.second, static_cast<std::uint32_t>(r)});
    return it != region_memory_.end() && it->second.known ? it->second.anchor : HudAnchor::Center;
}

// ---- HudLayout: one draw --------------------------------------------------------------------------------------------

HudAnchor HudLayout::group_draw_anchor() noexcept {
    switch (group_kind_) {
        case HudGroupKind::Text:
        case HudGroupKind::CsdWidget:
            return group_anchor_;
        case HudGroupKind::CsdWidgetPerCast: {
            const auto it = cast_memory_.find({group_key_.first, cast_id_});
            return it != cast_memory_.end() && it->second.known ? it->second.anchor : HudAnchor::Center;
        }
        case HudGroupKind::ScreenCover:
        case HudGroupKind::Ungrouped:
        default:
            // World-projected 2D (lock-on marks, arrows): the centre anchor applies the same squeeze as the 3D.
            return HudAnchor::Center;
    }
}

HudLayout::Decision HudLayout::layout_draw(const HudBounds& bounds, bool textured, bool shapes_anchor) noexcept {
    Decision decision;

    // 1. Grow the open cast's and group's bounds. Invisible draws do not count.
    if (shapes_anchor && cast_id_ != 0u && group_kind_ == HudGroupKind::CsdWidgetPerCast) {
        const Key key{group_key_.first, cast_id_};
        if (!cast_open_ || key != cast_key_) {
            close_cast();
            cast_key_ = key;
            cast_bounds_valid_ = false;
            cast_open_ = true;
        }
        accumulate(cast_bounds_, cast_bounds_valid_, bounds);
    }
    if (group_open_ && shapes_anchor) accumulate(group_bounds_, group_bounds_valid_, bounds);

    // 2. Parked outside the canvas on purpose (a widget's hidden state): leave it where it is.
    if (bounds.x1 < 0.0f || bounds.x0 > canvas_width_) {
        ++stats_.off_canvas;
        return decision;
    }
    ++stats_.laid_out;
    decision.laid_out = true;

    // 3. Does it cover the canvas? Full-width draws do, except a textured sprite that is zoomed past an edge and is not
    //    full height. That is content larger than the screen (the title's wordmark after-images), so it stays with its
    //    widget instead of being stretched to the output's width.
    bool covers = spans_canvas_width(bounds, canvas_width_);
    if (covers && textured && overflows_canvas_width(bounds, canvas_width_) &&
        !spans_canvas_height(bounds, canvas_height_)) {
        covers = false;
        decision.beyond_canvas = true;
        ++stats_.zoomed_sprites;
    }

    // 4. Inside a curtain group, every untextured full-height column covers. The first draw that is not a column (the
    //    loading graphic drawn on top) ends the curtain.
    if (group_kind_ == HudGroupKind::ScreenCover) {
        if (!textured && spans_canvas_height(bounds, canvas_height_)) {
            covers = true;
        } else {
            close_group();
            reset_group();
        }
    }

    // 5. Covering draws: a textured full-canvas draw is a background and stays the centred 4:3 canvas, as the
    //    original shows it. Fades, bands and curtain columns fill the output.
    if (covers) {
        const bool background = textured && spans_canvas_height(bounds, canvas_height_) &&
                                group_kind_ != HudGroupKind::ScreenCover;
        decision.anchor = background ? HudAnchor::Canvas : HudAnchor::Fill;
        return decision;
    }

    // 6. Everything else takes its group's anchor, and in split screen, the anchor inside its player's region.
    decision.anchor = group_draw_anchor();
    if (!regions_.empty() && (group_kind_ == HudGroupKind::CsdWidget || group_kind_ == HudGroupKind::Text)) {
        const int region = group_kind_ == HudGroupKind::Text ? region_of_point(group_text_x_) : region_of(bounds);
        if (region >= 0) {
            decision.anchor = region_anchor(region, bounds, shapes_anchor);
            decision.region = region;
            ++stats_.in_regions;
        }
    }
    return decision;
}

// ---- Savestates -----------------------------------------------------------------------------------------------------
// Format: for each map, a u32 count, then per record its key as u32s, the anchor (u8) and known (u8).

void HudLayout::save_memory(std::vector<std::uint8_t>& out) const {
    for (const auto* memory : {&group_memory_, &cast_memory_}) {
        put_u32(out, static_cast<std::uint32_t>(memory->size()));
        for (const auto& [key, m] : *memory) {
            put_u32(out, key.first);
            put_u32(out, key.second);
            out.push_back(static_cast<std::uint8_t>(m.anchor));
            out.push_back(m.known ? 1u : 0u);
        }
    }
}

bool HudLayout::load_memory(const std::uint8_t*& cursor, const std::uint8_t* end) {
    std::map<Key, Memory> restored[2];
    for (auto& memory : restored) {
        std::uint32_t count = 0;
        if (!get_u32(cursor, end, count) || count > kMaxRecords) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            Key key;
            if (!get_u32(cursor, end, key.first) || !get_u32(cursor, end, key.second) || end - cursor < 2) return false;
            Memory m;
            m.anchor = static_cast<HudAnchor>(*cursor++);
            m.known = *cursor++ != 0u;
            memory[key] = m;
        }
    }
    group_memory_ = std::move(restored[0]);
    cast_memory_ = std::move(restored[1]);
    return true;
}

void HudLayout::save_region_memory(std::vector<std::uint8_t>& out) const {
    put_u32(out, static_cast<std::uint32_t>(region_memory_.size()));
    for (const auto& [key, m] : region_memory_) {
        put_u32(out, std::get<0>(key));
        put_u32(out, std::get<1>(key));
        put_u32(out, std::get<2>(key));
        out.push_back(static_cast<std::uint8_t>(m.anchor));
        out.push_back(m.known ? 1u : 0u);
    }
}

bool HudLayout::load_region_memory(const std::uint8_t*& cursor, const std::uint8_t* end) {
    std::map<RegionKey, Memory> restored;
    std::uint32_t count = 0;
    if (!get_u32(cursor, end, count) || count > kMaxRecords) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t site = 0, instance = 0, region = 0;
        if (!get_u32(cursor, end, site) || !get_u32(cursor, end, instance) || !get_u32(cursor, end, region) ||
            end - cursor < 2) {
            return false;
        }
        Memory m;
        m.anchor = static_cast<HudAnchor>(*cursor++);
        m.known = *cursor++ != 0u;
        restored[{site, instance, region}] = m;
    }
    region_memory_ = std::move(restored);
    return true;
}

}  // namespace shadow::ui
