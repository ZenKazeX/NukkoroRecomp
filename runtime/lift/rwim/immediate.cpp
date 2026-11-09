#include "rwim/immediate.h"

#include <algorithm>
#include <cstdio>

// NE-RW2 (HW_REPORT Y.3): moved here from the scaffold's immediate.cpp / im_vertex.h with the same decisions in the
// same order, so the frame packets are the same record for record (GLE frame digests, HW_REPORT W.4).

namespace shadow::rwim {

bool topology_of(std::uint32_t primitive, render::Topology* topology, bool* fan) {
    *fan = false;
    switch (primitive) {
    case 1u: *topology = render::Topology::Lines; return true;
    case 2u: *topology = render::Topology::LineStrip; return true;
    case 3u: *topology = render::Topology::Triangles; return true;
    case 4u: *topology = render::Topology::TriangleStrip; return true;
    case 5u:
        *topology = render::Topology::Triangles;
        *fan = true;
        return true;
    default: return false;
    }
}

bool list_order(const std::uint16_t* indices, std::uint32_t index_count, std::uint32_t vertex_count, bool fan,
                std::vector<std::uint32_t>* out) {
    out->clear();
    const std::uint32_t order_count = indices != nullptr ? index_count : vertex_count;
    const auto order = [&](std::uint32_t i) { return indices != nullptr ? std::uint32_t{indices[i]} : i; };
    if (fan) {
        for (std::uint32_t i = 1; i + 1u < order_count; ++i)
            out->insert(out->end(), {order(0), order(i), order(i + 1u)});
    } else if (indices != nullptr) {
        for (std::uint32_t i = 0; i < order_count; ++i)
            out->push_back(order(i));
    }
    for (std::uint32_t index : *out) {
        if (index >= vertex_count)
            return false;
    }
    return true;
}

Immediate::Result2D Immediate::draw2d(const render::ImmediateVertex* vertices, std::uint32_t count,
                                      render::Topology topology, const std::vector<std::uint32_t>& order,
                                      bool indexed, const Binding& binding, render::Recipe recipe,
                                      const render::Anchor* anchor) {
    Result2D result;
    render::ImmediateDraw draw;
    draw.topology = topology;
    if (binding.textured) {
        draw.texture = binding.texture;
        draw.sampler = binding.sampler;
    }
    Bounds bounds{1.0e9f, 1.0e9f, -1.0e9f, -1.0e9f};
    for (std::uint32_t i = 0; i < count; ++i) {
        bounds.x0 = std::min(bounds.x0, vertices[i].position.x);
        bounds.y0 = std::min(bounds.y0, vertices[i].position.y);
        bounds.x1 = std::max(bounds.x1, vertices[i].position.x);
        bounds.y1 = std::max(bounds.y1, vertices[i].position.y);
    }
    result.bounds = bounds;
    // A draw whose every vertex is fully transparent under a source-alpha blend puts nothing on screen (a widget's
    // hidden part - the Egg Dealer's closed big slot panel, drawn mid-canvas at alpha 0 every frame): it takes its
    // widget's anchor but does not shape it.
    bool shows = recipe.src != render::Blend::SrcAlpha;
    for (std::uint32_t i = 0; i < count && !shows; ++i)
        shows = (vertices[i].colour >> 24) != 0u;
    Placement placement;
    if (anchor == nullptr)
        placement = layout_.place(bounds, binding.textured, shows);
    result.placement = placement;
    draw.space = render::Space::Canvas;
    draw.anchor = anchor != nullptr ? *anchor : placement.laid_out ? placement.anchor : render::Anchor::Fill;
    result.anchor = draw.anchor;
    if (!placement.laid_out)
        ++stats_.im2d_off_canvas;
    if (anchor == nullptr && placement.region >= 0) {
        draw.region = placement.region_id;
        ++stats_.im2d_in_regions;
    }
    result.fog_armed = recipe.fog;
    if (recipe.fog)
        ++stats_.im2d_fog_armed;
    recipe.fog = false;
    result.recipe = recipe;
    if (!host_.ensure_pass()) {
        ++stats_.im2d_no_pass;
        return result;
    }
    const bool listed = !order.empty();
    // A tiled strip (ui/tiled_strip.h): a run of four-vertex textured quads, each a translation of the first along
    // its long axis, laid out as content. The run is collected as it is drawn and continued to the view's edges when
    // it ends (flush_strip); anything else ends it first, so the copies are drawn right after it. Only a centred
    // layer: a strip is continued about the canvas's centre (Continued is placed as Center), so a draw the layout
    // holds at an edge is never one.
    bool strip_candidate = anchor == nullptr && placement.laid_out && binding.textured && count == 4u && !indexed &&
                           draw.anchor == render::Anchor::Center;
    ui::StripTile tile;
    ui::TileShape shape;
    if (strip_candidate) {
        tile.u0 = tile.v0 = 1.0e30f;
        tile.u1 = tile.v1 = -1.0e30f;
        for (std::uint32_t i = 0; i < 4u; ++i) {
            const render::ImmediateVertex& vx = vertices[i];
            tile.x[i] = vx.position.x;
            tile.y[i] = vx.position.y;
            tile.u0 = std::min(tile.u0, vx.uv[0]);
            tile.u1 = std::max(tile.u1, vx.uv[0]);
            tile.v0 = std::min(tile.v0, vx.uv[1]);
            tile.v1 = std::max(tile.v1, vx.uv[1]);
        }
        strip_candidate = ui::tile_shape(tile, &shape);
    }
    const std::size_t view = host_.current_view();
    const bool continues =
        strip_candidate && !strip_.empty() && strip_pass_ == builder_.passes() && strip_view_ == view &&
        strip_draw_.texture.value == draw.texture.value && strip_draw_.topology == draw.topology &&
        strip_draw_.sampler.filter == draw.sampler.filter && strip_draw_.sampler.u == draw.sampler.u &&
        strip_draw_.sampler.v == draw.sampler.v && strip_draw_.anchor == draw.anchor &&
        strip_draw_.region == draw.region && strip_recipe_.src == recipe.src && strip_recipe_.dst == recipe.dst &&
        ui::continues_chain(strip_.front().tile, strip_shape_, tile);
    if (!continues) {
        flush_strip();
        if (strip_candidate) {
            strip_shape_ = shape;
            strip_draw_ = draw;
            strip_recipe_ = recipe;
            strip_pass_ = builder_.passes();
            strip_view_ = view;
        }
    }
    if (strip_candidate) {
        StripTileDraw t;
        t.tile = tile;
        for (std::uint32_t i = 0; i < 4u; ++i)
            t.vertices[i] = vertices[i];
        t.immediate = builder_.immediates();
        strip_.push_back(t);
    }
    builder_.immediate(draw, vertices, count, listed ? order.data() : nullptr,
                       listed ? static_cast<std::uint32_t>(order.size()) : 0u, render::Float3x4{}, recipe);
    ++stats_.im2d_draws;
    ++stats_.im2d_anchors[static_cast<std::size_t>(draw.anchor)];
    result.drawn = true;
    return result;
}

void Immediate::draw3d(const render::ImmediateVertex* vertices, std::uint32_t count, render::Topology topology,
                       const std::vector<std::uint32_t>& order, bool listed, const render::Float3x4& world,
                       const Binding& binding, const render::Recipe& recipe) {
    render::ImmediateDraw draw;
    draw.topology = topology;
    if (binding.textured) {
        draw.texture = binding.texture;
        draw.sampler = binding.sampler;
    }
    builder_.immediate(draw, vertices, count, listed ? order.data() : nullptr,
                       listed ? static_cast<std::uint32_t>(order.size()) : 0u, world, recipe);
    ++stats_.im3d_draws;
}

// The strip being collected has ended. A run of two or more tiles whose gaps along the axis are all within
// [0.5, 1.05] of the tile's extent is a strip: its tiles are clipped to the view rather than to the canvas's box (as
// the console's raster edge clipped them), and each end that reaches its canvas's edge is repeated by that end's own
// step until it reaches the edge of the part of the canvas the view shows (render/view.h visible_canvas: the actual
// view, any aspect, a split-screen half included; tiled_strip.h continuation_count). A view that shows only the
// canvas gets no copy. Copies are the end tile's own vertices translated, drawn right after the strip with its
// texture, sampler and render states.
void Immediate::flush_strip() {
    if (strip_.empty())
        return;
    std::vector<StripTileDraw> run;
    run.swap(strip_);
    if (run.size() < 2u)
        return;
    Host::View view;
    if (!host_.pass_open() || strip_pass_ != builder_.passes() || !host_.view(strip_view_, &view)) {
        ++stats_.strips_unfinished;  // its pass closed before it ended: the copies would land in another pass
        return;
    }
    std::vector<ui::StripTile> tiles;
    tiles.reserve(run.size());
    for (const StripTileDraw& t : run)
        tiles.push_back(t.tile);
    ui::StripEnds ends;
    if (!ui::strip_ends(tiles, strip_shape_, &ends)) {
        ++stats_.strips_rejected;
        return;
    }
    for (const StripTileDraw& t : run)
        builder_.set_immediate_anchor(t.immediate, render::Anchor::Continued);
    // What the view shows of its canvas: the view is its raster's share of the output.
    const float share = (view.width / view.screen_width) / (view.height / view.screen_height);
    const render::CanvasRect visible = render::visible_canvas(view.width, view.height, render::kScreenAspect * share,
                                                              host_.output_aspect() * share);
    const render::CanvasRect canvas{0.0f, 0.0f, view.width, view.height};
    render::ImmediateDraw copy = strip_draw_;
    copy.anchor = render::Anchor::Continued;
    unsigned copies = 0;
    const auto repeat = [&](const StripTileDraw& end, float dx, float dy) {
        const unsigned n = ui::continuation_count(end.tile, strip_shape_, dx, dy, canvas, visible);
        for (unsigned k = 1; k <= n; ++k) {
            render::ImmediateVertex v[4];
            for (int i = 0; i < 4; ++i) {
                v[i] = end.vertices[i];
                v[i].position.x += static_cast<float>(k) * dx;
                v[i].position.y += static_cast<float>(k) * dy;
            }
            builder_.immediate(copy, v, 4u, nullptr, 0u, render::Float3x4{}, strip_recipe_);
        }
        copies += n;
    };
    repeat(run[ends.low], ends.low_dx, ends.low_dy);
    repeat(run[ends.high], ends.high_dx, ends.high_dy);
    ++stats_.strips_continued;
    stats_.strip_copies += copies;
    if (!reported_strip_) {
        reported_strip_ = true;
        std::fprintf(stderr,
                     "[r2-2d] frame %llu: a tiled strip of %zu tiles continued to the view's edges with %u copies "
                     "(visible canvas x %.1f..%.1f)\n",
                     static_cast<unsigned long long>(host_.frame()), run.size(), copies, visible.left, visible.right);
    }
}

}  // namespace shadow::rwim
