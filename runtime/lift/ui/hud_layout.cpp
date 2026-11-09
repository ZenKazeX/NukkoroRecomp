#include "ui/hud_layout.h"

#include <algorithm>
#include <cmath>

// NE-RW2c: moved, value for value and in the same order, from runtime/src/gekko/display.cpp (the classifiers) and
// runtime/src/gekko/native_hud.cpp (the policy). Not moved: the legacy renderer's canvas affine (it stays in
// display.cpp) and the Tracy plots of the old end_frame (measurement only; a Tracy build of the lifted renderer no
// longer plots the HUD counters).

namespace shadow::ui {

HudAnchor classify_hud_anchor(const HudBounds& bounds, bool textured, bool blending, float canvas_width,
                              float canvas_height) noexcept {
    const float x0 = std::min(bounds.x0, bounds.x1);
    const float x1 = std::max(bounds.x0, bounds.x1);
    const float y0 = std::min(bounds.y0, bounds.y1);
    const float y1 = std::max(bounds.y0, bounds.y1);
    // Coverage is the same question here as in spans_canvas_width: does this
    // draw reach both edges of the canvas? A merely wide draw is content.
    if (spans_canvas_width(bounds, canvas_width) && spans_canvas_height(bounds, canvas_height)) {
        if (!textured) {
            return HudAnchor::Fill;
        }
        (void)blending;
        return HudAnchor::Canvas;
    }

    const float cx = (x0 + x1) * 0.5f;
    const float cy = (y0 + y1) * 0.5f;
    int horiz = 1;
    if (x1 <= canvas_width * 0.5f || cx < canvas_width / 3.0f) {
        horiz = 0;
    } else if (x0 >= canvas_width * 0.5f || cx > canvas_width * 2.0f / 3.0f) {
        horiz = 2;
    }
    int vert = 1;
    if (y1 <= canvas_height * 0.5f || cy < canvas_height / 3.0f) {
        vert = 0;
    } else if (y0 >= canvas_height * 0.5f || cy > canvas_height * 2.0f / 3.0f) {
        vert = 2;
    }

    static constexpr HudAnchor kGrid[3][3] = {
        {HudAnchor::TopLeft, HudAnchor::Top, HudAnchor::TopRight},
        {HudAnchor::Left, HudAnchor::Center, HudAnchor::Right},
        {HudAnchor::BottomLeft, HudAnchor::Bottom, HudAnchor::BottomRight},
    };
    return kGrid[vert][horiz];
}

bool spans_canvas_width(const HudBounds& bounds, float canvas_width) noexcept {
    // A draw covers the output only if it REACHES BOTH EDGES of the canvas.
    //
    // Width alone is not the same question. The autosave dialog's window body
    // is 588 canvas units wide (26.5..614.5) - 92% of the canvas - but it is
    // inset on both sides, and it is content, not a backdrop. Treating it as
    // coverage left it identity-mapped (stretched to the full output) while
    // its own four-unit-wide side borders, which are not wide, were anchored
    // and moved inwards to canvas 96 and 545: the window was pulled apart from
    // its own frame. Measured on the boot route, 16:9, frame 120.
    //
    // Everything that genuinely has to cover the output does reach the edges:
    // a fade column (0..64, with the ScreenCover group supplying the rest of
    // the tiling), a letterbox band, a full-frame backdrop, a menu's backing
    // plate. Everything that must not - a dialog, a banner with margins, a
    // wide HUD strip - does not.
    const float x0 = std::min(bounds.x0, bounds.x1);
    const float x1 = std::max(bounds.x0, bounds.x1);
    // The guest's own full-frame draws land on 0.5..640.5 after the viewport
    // mapping, and some art is authored one or two units inside the frame.
    constexpr float kEdgeTolerance = 8.0f;
    return x0 <= kEdgeTolerance && x1 >= canvas_width - kEdgeTolerance;
}

bool overflows_canvas_width(const HudBounds& bounds, float canvas_width) noexcept {
    // Past the coverage test's own slack on either side: art authored at the frame (0..640, 0.5..640.5, or a unit or
    // two outside it) never is. Either, not both: the title's after-images scale about the wordmark's centre
    // (x 287), not the canvas's, so one reaching both edges is far past the left one but can be only a few units past
    // the right (frame 2178 of the title entrance: -72.4..646.4).
    constexpr float kEdgeTolerance = 8.0f;
    const float x0 = std::min(bounds.x0, bounds.x1);
    const float x1 = std::max(bounds.x0, bounds.x1);
    return x0 < -kEdgeTolerance || x1 > canvas_width + kEdgeTolerance;
}

bool spans_canvas_height(const HudBounds& bounds, float canvas_height) noexcept {
    return std::fabs(bounds.y1 - bounds.y0) >= canvas_height * 0.90f;
}

HudAnchor classify_group_anchor(const HudBounds& bounds, HudAnchor previous, bool have_previous,
                                float canvas_width) noexcept {
    const float x0 = std::min(bounds.x0, bounds.x1);
    const float x1 = std::max(bounds.x0, bounds.x1);
    const float centre = (x0 + x1) * 0.5f;
    const float canvas_centre = canvas_width * 0.5f;

    HudAnchor candidate = HudAnchor::Center;
    const bool point_like = x1 - x0 < 1.0f;
    if (point_like) {
        // A run of HUD text reports one alignment reference point, not an
        // extent, so which half of the canvas it is "in" is meaningless and
        // thirds are the only sensible rule. The eight X values the gameplay
        // HUD uses - 54, 78, 84, 120 / 320 / 477, 520, 560 - clear both
        // boundaries by a wide margin.
        if (centre < canvas_width / 3.0f) {
            candidate = HudAnchor::Left;
        } else if (centre > canvas_width * 2.0f / 3.0f) {
            candidate = HudAnchor::Right;
        }
    } else if (x1 <= canvas_centre) {
        // A widget that lies wholly in one half of the canvas belongs to that
        // side of the HUD strip, however close to the middle it reaches. The
        // meter icons sit at the inner ends of the Hero and Dark gauges, and a
        // centroid rule puts one of them within half a unit of the left third
        // and detaches it from the gauge it caps.
        candidate = HudAnchor::Left;
    } else if (x0 >= canvas_centre) {
        candidate = HudAnchor::Right;
    }

    // A run of HUD text is classified from a fixed alignment point: every
    // gameplay glyph site reads its X from a fixed layout table and never
    // moves it (provenance: docs/NATIVE_SOURCE_PROVENANCE.md), so there is nothing to hold
    // steady and hysteresis would only make a settled run sticky in the band
    // between a third and the middle.
    if (!have_previous || point_like || candidate == previous) {
        return candidate;
    }
    // A quarter of the canvas of hysteresis. A group that has settled on one
    // side keeps it until the group has decisively moved elsewhere, so a
    // widget or a string that slides during an entrance animation cannot pop
    // from one anchor to another halfway through the move.
    const float hysteresis = canvas_width * 0.25f;
    if (candidate == HudAnchor::Left && centre < canvas_centre - hysteresis) {
        return HudAnchor::Left;
    }
    if (candidate == HudAnchor::Right && centre > canvas_centre + hysteresis) {
        return HudAnchor::Right;
    }
    if (candidate == HudAnchor::Center && std::fabs(centre - canvas_centre) < hysteresis) {
        return HudAnchor::Center;
    }
    return previous;
}

void HudLayout::close_group() noexcept {
    if (!group_open_) return;
    group_open_ = false;
    // The group's part in each player region settles that region's anchor, by the same rule in the region's own
    // coordinates and width.
    for (std::size_t r = 0; r < region_bounds_valid_.size(); ++r) {
        if (!region_bounds_valid_[r] || r >= regions_.size()) continue;
        region_bounds_valid_[r] = false;
        const auto key = std::make_tuple(group_key_.first, group_key_.second, static_cast<std::uint32_t>(r));
        const auto existing = region_memory_.find(key);
        if (existing == region_memory_.end() && region_memory_.size() >= 4096u) {
            ++stats_.group_memory_capped;
            continue;
        }
        Instance& instance = existing != region_memory_.end() ? existing->second : region_memory_[key];
        instance.anchor = classify_group_anchor(region_bounds_[r], instance.anchor, instance.have_anchor,
                                                regions_[r].x1 - regions_[r].x0);
        instance.have_anchor = true;
    }
    if (group_kind_ == HudGroupKind::Ungrouped || !group_bounds_valid_) return;
    const auto existing = group_memory_.find(group_key_);
    if (existing == group_memory_.end() && group_memory_.size() >= 4096u) {
        ++stats_.group_memory_capped;
        return;
    }
    Instance& instance =
        existing != group_memory_.end() ? existing->second : group_memory_[group_key_];
    // Only the anchor is a property of the whole group; coverage is decided per
    // draw (a widget's backdrop covers, its siblings do not).
    instance.anchor = classify_group_anchor(group_bounds_, instance.anchor, instance.have_anchor, canvas_width_);
    instance.have_anchor = true;
}

void HudLayout::close_cast() noexcept {
    if (!cast_open_) return;
    cast_open_ = false;
    if (!cast_bounds_valid_) return;
    const auto it = cast_memory_.find(cast_key_);
    const bool remembered = it != cast_memory_.end() && it->second.have_anchor;
    const HudAnchor settled = classify_group_anchor(
        cast_bounds_, remembered ? it->second.anchor : HudAnchor::Center, remembered, canvas_width_);
    if (remembered) {
        it->second.anchor = settled;
    } else if (cast_memory_.size() < 4096u) {
        Instance& instance = cast_memory_[cast_key_];
        instance.anchor = settled;
        instance.have_anchor = true;
    }
}

void HudLayout::on_group(HudGroupKind kind, std::uint32_t group_id, std::uint32_t instance,
                               float canvas_x) noexcept {
    close_group();
    group_kind_ = kind;
    group_id_ = group_id;
    // A group instance is (draw site, the guest's own object). The ordinal is
    // the fallback only for marks that carry no instance (glyph runs), counted
    // per displayed frame.
    const std::uint32_t key_instance = instance != 0u ? instance : group_ordinals_[group_id]++;
    group_key_ = std::make_pair(group_id, key_instance);
    group_bounds_ = HudBounds{};
    group_bounds_valid_ = false;
    group_text_x_ = canvas_x;
    std::fill(region_bounds_valid_.begin(), region_bounds_valid_.end(), false);
    group_open_ = kind != HudGroupKind::Ungrouped;
    const auto it = group_memory_.find(group_key_);
    const bool remembered = it != group_memory_.end() && it->second.have_anchor;
    if (kind == HudGroupKind::Text) {
        HudBounds run;
        run.x0 = canvas_x;
        run.x1 = canvas_x;
        group_anchor_ = classify_group_anchor(run, remembered ? it->second.anchor : HudAnchor::Center, remembered,
                                              canvas_width_);
        group_anchor_decided_ = true;
    } else if (remembered) {
        group_anchor_ = it->second.anchor;
        group_anchor_decided_ = true;
    } else {
        group_anchor_decided_ = false;
    }
}

void HudLayout::on_scene(std::uint32_t node, std::uint32_t cast) noexcept {
    (void)node;
    if (cast != cast_id_) close_cast();
    cast_id_ = cast;
}

namespace {

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool get_u32(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint32_t& value) {
    if (end - cursor < 4) return false;
    value = 0;
    for (int shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(*cursor++) << shift;
    return true;
}

}  // namespace

void HudLayout::set_regions(const Region* regions, std::size_t count, float canvas_width,
                                  float canvas_height) {
    const bool same = count == regions_.size() && canvas_width == region_canvas_width_ &&
                      canvas_height == region_canvas_height_ &&
                      std::equal(regions, regions + count, regions_.begin(), [](const Region& a, const Region& b) {
                          return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
                      });
    if (same) return;
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

// The region a draw lies in: wholly inside it - up to the same few units of slack the coverage test allows at the
// canvas's own edges (art authored one or two units outside the frame), but none at an edge two regions share, so a
// draw across the split (the 2P timer) belongs to the whole canvas.
int HudLayout::region_of(const HudBounds& bounds) const noexcept {
    constexpr float kEdgeTolerance = 8.0f;
    const float x0 = std::min(bounds.x0, bounds.x1), x1 = std::max(bounds.x0, bounds.x1);
    const float y0 = std::min(bounds.y0, bounds.y1), y1 = std::max(bounds.y0, bounds.y1);
    for (std::size_t r = 0; r < regions_.size(); ++r) {
        const Region& g = regions_[r];
        const float left = g.x0 <= 0.0f ? g.x0 - kEdgeTolerance : g.x0;
        const float right = g.x1 >= region_canvas_width_ ? g.x1 + kEdgeTolerance : g.x1;
        const float top = g.y0 <= 0.0f ? g.y0 - kEdgeTolerance : g.y0;
        const float bottom = g.y1 >= region_canvas_height_ ? g.y1 + kEdgeTolerance : g.y1;
        if (x0 >= left && x1 <= right && y0 >= top && y1 <= bottom) return static_cast<int>(r);
    }
    return -1;
}

// A glyph run's region: the one whose span holds its alignment point strictly inside (a point on an edge two
// regions share - the 2P timer at 320 - belongs to the whole canvas). Only regions as tall as the canvas qualify: a
// run reports no vertical position.
int HudLayout::region_of_point(float x) const noexcept {
    for (std::size_t r = 0; r < regions_.size(); ++r) {
        const Region& g = regions_[r];
        if (g.y0 <= 0.0f && g.y1 >= region_canvas_height_ && x > g.x0 && x < g.x1) return static_cast<int>(r);
    }
    return -1;
}

// A draw's anchor inside its region. A widget: the anchor its group's part in this region settled on (classified at
// the group's close, as a whole canvas's widget is), Center until then. A glyph run: the half of the region its
// alignment point lies in - a player region holds no run centred on it, and the thirds the whole canvas uses would
// put the 2P ammo counters (printed centred on their command windows at canvas x 120 and 520, both windows on the
// outer edges) in their regions' middle thirds, away from the windows they are printed on.
HudAnchor HudLayout::region_anchor(int region, const HudBounds& bounds, bool shapes_anchor) noexcept {
    const std::size_t r = static_cast<std::size_t>(region);
    const Region& g = regions_[r];
    if (group_kind_ == HudGroupKind::Text) {
        const float local = group_text_x_ - g.x0;
        const float half = 0.5f * (g.x1 - g.x0);
        return local < half ? HudAnchor::Left : local > half ? HudAnchor::Right : HudAnchor::Center;
    }
    if (group_open_ && shapes_anchor) {
        HudBounds local;
        local.x0 = bounds.x0 - g.x0;
        local.x1 = bounds.x1 - g.x0;
        local.y0 = bounds.y0 - g.y0;
        local.y1 = bounds.y1 - g.y0;
        HudBounds& b = region_bounds_[r];
        if (!region_bounds_valid_[r]) {
            b = local;
            region_bounds_valid_[r] = true;
        } else {
            b.x0 = std::min(b.x0, local.x0);
            b.y0 = std::min(b.y0, local.y0);
            b.x1 = std::max(b.x1, local.x1);
            b.y1 = std::max(b.y1, local.y1);
        }
    }
    const auto it = region_memory_.find(std::make_tuple(group_key_.first, group_key_.second, static_cast<std::uint32_t>(r)));
    return it != region_memory_.end() && it->second.have_anchor ? it->second.anchor : HudAnchor::Center;
}

void HudLayout::save_region_memory(std::vector<std::uint8_t>& out) const {
    put_u32(out, static_cast<std::uint32_t>(region_memory_.size()));
    for (const auto& [key, instance] : region_memory_) {
        put_u32(out, std::get<0>(key));
        put_u32(out, std::get<1>(key));
        put_u32(out, std::get<2>(key));
        out.push_back(static_cast<std::uint8_t>(instance.anchor));
        out.push_back(instance.have_anchor ? 1u : 0u);
    }
}

bool HudLayout::load_region_memory(const std::uint8_t*& cursor, const std::uint8_t* end) {
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, Instance> restored;
    std::uint32_t count = 0;
    if (!get_u32(cursor, end, count) || count > (1u << 20)) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t a = 0, b = 0, c = 0;
        if (!get_u32(cursor, end, a) || !get_u32(cursor, end, b) || !get_u32(cursor, end, c) || end - cursor < 2)
            return false;
        Instance instance;
        instance.anchor = static_cast<HudAnchor>(*cursor++);
        instance.have_anchor = *cursor++ != 0u;
        restored[std::make_tuple(a, b, c)] = instance;
    }
    region_memory_ = std::move(restored);
    return true;
}

void HudLayout::save_memory(std::vector<std::uint8_t>& out) const {
    for (const auto* memory : {&group_memory_, &cast_memory_}) {
        put_u32(out, static_cast<std::uint32_t>(memory->size()));
        for (const auto& [key, instance] : *memory) {
            put_u32(out, key.first);
            put_u32(out, key.second);
            out.push_back(static_cast<std::uint8_t>(instance.anchor));
            out.push_back(instance.have_anchor ? 1u : 0u);
        }
    }
}

bool HudLayout::load_memory(const std::uint8_t*& cursor, const std::uint8_t* end) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, Instance> restored[2];
    for (auto& memory : restored) {
        std::uint32_t count = 0;
        if (!get_u32(cursor, end, count) || count > (1u << 20)) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t first = 0, second = 0;
            if (!get_u32(cursor, end, first) || !get_u32(cursor, end, second) || end - cursor < 2) return false;
            Instance instance;
            instance.anchor = static_cast<HudAnchor>(*cursor++);
            instance.have_anchor = *cursor++ != 0u;
            memory[{first, second}] = instance;
        }
    }
    group_memory_ = std::move(restored[0]);
    cast_memory_ = std::move(restored[1]);
    return true;
}

void HudLayout::end_frame() noexcept {
    if (group_open_) ++stats_.groups_open_at_frame_end;
    close_group();
    group_kind_ = HudGroupKind::Ungrouped;
    group_id_ = 0;
    group_anchor_decided_ = false;
    group_bounds_valid_ = false;
    group_open_ = false;
    group_ordinals_.clear();
    std::fill(region_bounds_valid_.begin(), region_bounds_valid_.end(), false);
    close_cast();
    cast_id_ = 0;
    cast_bounds_valid_ = false;
}

HudLayout::Decision HudLayout::layout_draw(const HudBounds& bounds,
                                                       bool textured,
                                                       bool shapes_anchor) noexcept {
    Decision decision;
    // A draw that puts nothing on screen does not decide where its widget
    // belongs: the Egg Dealer's slot display draws its hidden big panel at
    // vertex alpha 0 across the middle of the canvas every frame, and counting
    // it held the visible small panel on the right in the centred 4:3 box.
    if (shapes_anchor && cast_id_ != 0u && group_kind_ == HudGroupKind::CsdWidgetPerCast) {
        const std::pair<std::uint32_t, std::uint32_t> key{group_id_, cast_id_};
        if (!cast_open_ || key != cast_key_) {
            close_cast();
            cast_key_ = key;
            cast_bounds_ = bounds;
            cast_bounds_valid_ = true;
            cast_open_ = true;
        } else {
            cast_bounds_.x0 = std::min(cast_bounds_.x0, bounds.x0);
            cast_bounds_.y0 = std::min(cast_bounds_.y0, bounds.y0);
            cast_bounds_.x1 = std::max(cast_bounds_.x1, bounds.x1);
            cast_bounds_.y1 = std::max(cast_bounds_.y1, bounds.y1);
        }
    }
    if (group_open_ && shapes_anchor) {
        if (!group_bounds_valid_) {
            group_bounds_ = bounds;
            group_bounds_valid_ = true;
        } else {
            group_bounds_.x0 = std::min(group_bounds_.x0, bounds.x0);
            group_bounds_.y0 = std::min(group_bounds_.y0, bounds.y0);
            group_bounds_.x1 = std::max(group_bounds_.x1, bounds.x1);
            group_bounds_.y1 = std::max(group_bounds_.y1, bounds.y1);
        }
    }
    // Parked outside the canvas on purpose: leave it hidden where it is.
    if (bounds.x1 < 0.0f || bounds.x0 > canvas_width_) {
        ++stats_.off_canvas;
        return decision;
    }
    ++stats_.laid_out;
    decision.laid_out = true;

    HudAnchor anchor = classify_hud_anchor(bounds, textured, false, canvas_width_, canvas_height_);
    bool covers = spans_canvas_width(bounds, canvas_width_);
    // A textured sprite across the canvas width and zoomed past a side edge, not as tall as the canvas, is content larger than
    // the screen, not a band: the title screen's after-images of the SHADOW wordmark (honban.gncp scene "Title",
    // casts title2..title6, 512 x 128 scaled up about their centre). Filled, it was stretched to the view's width
    // and left the wordmark it echoes; it is laid out with its widget, as the wordmark is.
    if (covers && textured && !zoomed_sprites_fill_ && overflows_canvas_width(bounds, canvas_width_) &&
        !spans_canvas_height(bounds, canvas_height_)) {
        covers = false;
        decision.beyond_canvas = true;
        ++stats_.zoomed_sprites;
    }
    if (group_kind_ == HudGroupKind::ScreenCover) {
        const bool curtain_column = !textured && spans_canvas_height(bounds, canvas_height_);
        if (curtain_column) {
            covers = true;
        } else {
            close_group();
            group_kind_ = HudGroupKind::Ungrouped;
            group_id_ = 0;
        }
    }
    if (covers) {
        // A textured draw over the whole canvas is a background: it stays the 4:3 canvas, centred, as the original
        // shows it (user 2026-10-28: menus in their original form; the 2026-10-26 cover rule is gone). Fades, bands
        // and curtain columns fill.
        anchor = textured && spans_canvas_height(bounds, canvas_height_) && group_kind_ != HudGroupKind::ScreenCover
                     ? HudAnchor::Canvas
                     : HudAnchor::Fill;
    } else {
        switch (group_kind_) {
            case HudGroupKind::Text:
                anchor = group_anchor_;
                break;
            case HudGroupKind::CsdWidgetPerCast: {
                const auto it = cast_memory_.find({group_id_, cast_id_});
                anchor = (it != cast_memory_.end() && it->second.have_anchor) ? it->second.anchor
                                                                            : HudAnchor::Center;
                break;
            }
            case HudGroupKind::CsdWidget:
                if (!group_anchor_decided_) {
                    group_anchor_ = HudAnchor::Center;
                    group_anchor_decided_ = true;
                }
                anchor = group_anchor_;
                break;
            case HudGroupKind::ScreenCover:
            case HudGroupKind::Ungrouped:
            default:
                anchor = HudAnchor::Center;
                break;
        }
        // In split screen a gameplay-HUD draw that lies in one player's viewport is laid out in that viewport.
        // Everything else - a draw across the split, a menu, a banner, a fade - keeps the whole canvas.
        if (!regions_.empty() &&
            (group_kind_ == HudGroupKind::CsdWidget || group_kind_ == HudGroupKind::Text)) {
            const int region =
                group_kind_ == HudGroupKind::Text ? region_of_point(group_text_x_) : region_of(bounds);
            if (region >= 0) {
                anchor = region_anchor(region, bounds, shapes_anchor);
                decision.region = region;
                ++stats_.in_regions;
            }
        }
    }
    decision.anchor = anchor;
    return decision;
}

}  // namespace shadow::ui
