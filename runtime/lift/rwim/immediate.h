#pragma once

// RENDERWARE'S IMMEDIATE MODES, NATIVE (NE-RW2, HW_REPORT Y.3; NO_EMULATION_PLAN.md 3.4). What RwIm2DRenderPrimitive /
// RwIm2DRenderIndexedPrimitive and RwIm3DTransform + RwIm3DRender(Indexed)Primitive mean, as a library over plain
// values: a primitive's vertices (position, depth, RGBA8, texture coordinates), its vertex order, the texture bound,
// the render states in force (a render::Recipe) and the 2D layout policy's placement become immediate draws of the
// frame packet (render/frame.h). No guest memory, no console structs or byte orders, no RenderWare object layouts: the
// caller hands over values. Callers supply decoded vertex records through the same API. Historical caller
// provenance is recorded in docs/NATIVE_SOURCE_PROVENANCE.md.
//
// The camera / frame layer (which view and pass a draw goes into) and the 2D layout policy are the caller's, reached
// through two small interfaces, so this library decides nothing about either.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/frame.h"
#include "render/view.h"
#include "ui/tiled_strip.h"

namespace shadow::rwim {

// RenderWare's primitive types (both immediate modes): 1 lines, 2 polyline, 3 triangles, 4 strip, 5 fan. A fan is
// drawn as a triangle list; points (6) and none (0) are refused.
// The topology `primitive` is drawn as; false: refused.
bool topology_of(std::uint32_t primitive, render::Topology* topology, bool* fan);

// The order a primitive's vertices are drawn in as a list: its indices (null: the vertices in order), a fan
// (v0, v1, v2, ...) as the triangles (v0, vi, vi+1). `out` stays empty for vertices drawn in order as given. False
// when an index names no vertex.
bool list_order(const std::uint16_t* indices, std::uint32_t index_count, std::uint32_t vertex_count, bool fan,
                std::vector<std::uint32_t>* out);

// A canvas draw's extent in canvas units.
struct Bounds {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

// The camera / frame layer the draws go into (implemented by the caller).
class Host {
public:
    struct View {
        float width = 0.0f, height = 0.0f;                // the view's canvas (its raster's size)
        float screen_width = 0.0f, screen_height = 0.0f;  // the whole picture it is a part of
    };
    // The pass the next draw goes into, opened or continued; false: there is no screen view to draw into.
    virtual bool ensure_pass() = 0;
    virtual bool pass_open() const = 0;
    // The view the current draws are made for, and a view of this frame by index (false: no such view).
    virtual std::size_t current_view() const = 0;
    virtual bool view(std::size_t index, View* out) const = 0;
    // The output's width / height.
    virtual float output_aspect() const = 0;
    // The frame being described (diagnostics).
    virtual std::uint64_t frame() const = 0;

protected:
    ~Host() = default;
};

// The 2D layout policy's placement of one canvas draw (implemented by the caller).
struct Placement {
    bool laid_out = true;  // false: off the canvas (stretched with it)
    render::Anchor anchor = render::Anchor::Center;
    int region = -1;                              // the policy's region (a player's viewport); -1: the whole canvas
    std::uint32_t region_id = render::kNoRegion;  // that region's id in the frame packet
};
class LayoutPolicy {
public:
    // `textured`: a texture is bound; `shows`: the draw puts something on screen (not every vertex fully transparent
    // under a source-alpha blend).
    virtual Placement place(const Bounds& bounds, bool textured, bool shows) = 0;

protected:
    ~LayoutPolicy() = default;
};

// The texture bound for a draw.
struct Binding {
    bool textured = false;          // a texture is bound (it may still have no renderer handle)
    render::TextureHandle texture;  // none: the vertex colours alone
    render::Sampler sampler;
};

class Immediate {
public:
    struct Stats {
        std::uint64_t im2d_draws = 0, im2d_no_pass = 0, im2d_off_canvas = 0, im2d_fog_armed = 0, im2d_in_regions = 0;
        std::uint64_t im2d_anchors[render::kAnchorCount] = {};
        // Tiled strips (ui/tiled_strip.h): continued, the copies drawn, runs that did not qualify, runs whose pass
        // closed before they ended.
        std::uint64_t strips_continued = 0, strip_copies = 0, strips_rejected = 0, strips_unfinished = 0;
        std::uint64_t im3d_draws = 0;
    };
    // What became of an Im2D draw (the caller's diagnostics).
    struct Result2D {
        bool drawn = false;  // false: no pass to draw into
        Bounds bounds;
        Placement placement;  // the policy's (default when the caller placed the draw)
        render::Anchor anchor = render::Anchor::Center;
        render::Recipe recipe;
        bool fog_armed = false;
    };

    Immediate(render::FrameBuilder& builder, Host& host, LayoutPolicy& layout) noexcept
        : builder_(builder), host_(host), layout_(layout) {}

    // An Im2D primitive on the current view's canvas: `vertices` x, y in canvas units, z the depth itself (0
    // nearest), RGBA8, u, v; drawn as `topology` in `order` (empty: in order; list_order; `indexed`: the caller
    // gave indices); `recipe` the render states
    // in force with the alpha cutoff resolved (canvas draws are not fogged: counted); `anchor` null: the layout
    // policy places the draw. A tiled strip is collected as it is drawn and continued to the view's edges when it
    // ends (flush_strip).
    Result2D draw2d(const render::ImmediateVertex* vertices, std::uint32_t count, render::Topology topology,
                    const std::vector<std::uint32_t>& order, bool indexed, const Binding& binding,
                    render::Recipe recipe, const render::Anchor* anchor);
    // An Im3D primitive into the pass the caller opened (Host::ensure_pass): `vertices` in object space placed by
    // `world`, drawn as `topology` in `order` (list_order) when `listed`, else in order.
    void draw3d(const render::ImmediateVertex* vertices, std::uint32_t count, render::Topology topology,
                const std::vector<std::uint32_t>& order, bool listed, const render::Float3x4& world,
                const Binding& binding, const render::Recipe& recipe);
    // The strip being collected has ended (its group, pass, bracket or frame ended; another draw came).
    void flush_strip();
    // A restored state: the strip being collected belonged to the replaced timeline.
    void reset() { strip_.clear(); }

    const Stats& stats() const noexcept { return stats_; }

private:
    struct StripTileDraw {
        ui::StripTile tile;
        render::ImmediateVertex vertices[4];
        std::size_t immediate = 0;
    };

    render::FrameBuilder& builder_;
    Host& host_;
    LayoutPolicy& layout_;
    std::vector<StripTileDraw> strip_;
    ui::TileShape strip_shape_;
    render::ImmediateDraw strip_draw_;
    render::Recipe strip_recipe_;
    std::size_t strip_pass_ = 0;
    std::size_t strip_view_ = 0;
    bool reported_strip_ = false;
    Stats stats_;
};

}  // namespace shadow::rwim
