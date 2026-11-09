// SCAFFOLDING (R2): the adapter from RenderWare's GameCube immediate-mode records to the native immediate modes (runtime/lift/rwim, NE-RW2: values, the bound texture, the render states, the 2D layout policy's placement), the screen passes' remaining Im2D quads and camera-texture copies accepted and counted - deleted by R4 / R6: RenderWare's 2D layer and texture objects lifted (Im3D and the copies go with the screen passes)
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "assets/texture_dictionary.h"
#include "gekko/cpu.h"
#include "im_vertex.h"
#include "rw_driver.h"
#include "rwim/immediate.h"

// r2/SITES_DEVICE.md 2 (Im2D: 50 primitive + 1 indexed dispatch, every one returning 1), r2/SITES_PIPELINES.md 1.1,
// 2.8, 4.6 (Im3D: the transform records the pool, the render draws it; both succeed) and r2/SITES_PRESENT_EFFECTS.md 2
// (the camera-texture copies). The copies keep their RenderWare-visible results exact; the screen passes' own sites
// are in screens.cpp.

namespace gekko::scaffold {

namespace render = shadow::render;

using im::be_float;
using im::rgba8;

namespace {

constexpr u32 kRasterTypeCameraTexture = 5;
constexpr u32 kIm2DVertexSize = im::k2DVertexSize;
constexpr u32 kIm2DVertexColour = im::k2DVertexColour;
constexpr u32 kIm2DVertexUv = im::k2DVertexUv;

// The layout policy's anchors as the renderer's: the canvas's left edge, centre or right edge stays on the view's;
// a draw that covers the canvas stretches over the view.
render::Anchor anchor_of(shadow::ui::HudAnchor anchor) {
    switch (anchor) {
    case shadow::ui::HudAnchor::TopLeft:
    case shadow::ui::HudAnchor::Left:
    case shadow::ui::HudAnchor::BottomLeft: return render::Anchor::Left;
    case shadow::ui::HudAnchor::TopRight:
    case shadow::ui::HudAnchor::Right:
    case shadow::ui::HudAnchor::BottomRight: return render::Anchor::Right;
    case shadow::ui::HudAnchor::Fill: return render::Anchor::Fill;
    case shadow::ui::HudAnchor::Cover: return render::Anchor::Cover;
    default: return render::Anchor::Center;
    }
}

const char* anchor_name(render::Anchor anchor) {
    switch (anchor) {
    case render::Anchor::Left: return "left";
    case render::Anchor::Center: return "centre";
    case render::Anchor::Right: return "right";
    case render::Anchor::Fill: return "fill";
    case render::Anchor::Projected: return "projected";
    case render::Anchor::Continued: return "continued";
    case render::Anchor::Cover: return "cover";
    }
    return "?";
}

}  // namespace

u32 RwDriver::im2d_primitive(Cpu& c, u32 type, u32 vertices, u32 count) {
    return im2d(c, type, vertices, count, 0u, 0u);
}

// The bind every Im2D primitive makes (0x80498854): the Im texture's raster, and the alpha mode from its has-alpha
// bit; untextured, the mode in force stays.
u32 RwDriver::im2d_bind(Cpu& c, bool* camera_texture) {
    *camera_texture = false;
    const u32 im = im_texture(c);
    const u32 raster = im != 0u ? c.mem_read32(im + layout::kTextureRaster) : 0u;
    if (raster == 0u)
        return 0u;
    u32 parent = c.mem_read32(raster + layout::kRasterParent);
    if (parent == 0u)
        parent = raster;
    *camera_texture = (c.mem_read8(raster + layout::kRasterType) & 7u) == kRasterTypeCameraTexture;
    const bool has_alpha = (c.mem_read32(raster_ext(c, parent) + layout::kRasterExtFlags) & 1u) != 0u;
    states_.alpha_test_off = has_alpha ? 0u : 1u;
    return raster;
}

u32 RwDriver::im2d_indexed(Cpu& c, u32 type, u32 vertices, u32 count, u32 indices, u32 index_count) {
    return im2d(c, type, vertices, count, indices, index_count);
}

// The movie player's frame blit (0x80036570, sites in the story / event players): the bound raster is the movie's
// current picture buffer, which the Sofdec decoder rewrites in place (a RAW write lock; r2/SITES_DEVICE.md 4.6). Its
// texture is rebuilt when the texels changed since the last build - a fingerprint of the whole picture, so a frame
// drawn twice is not imported twice - and the quad is placed keeping the movie's 4:3 picture whole on a wider output
// (pillarboxed: the default build stretched it).
u32 RwDriver::movie_frame(Cpu& c, u32 type, u32 vertices, u32 count) {
    const u32 im = im_texture(c);
    const u32 raster = im != 0u ? c.mem_read32(im + layout::kTextureRaster) : 0u;
    if (raster != 0u) {
        u32 parent = c.mem_read32(raster + layout::kRasterParent);
        if (parent == 0u)
            parent = raster;
        const u32 ext = raster_ext(c, parent);
        const u32 pixels = c.mem_read32(ext + layout::kRasterExtPixels);
        const auto encoding =
            static_cast<shadow::assets::TexelEncoding>(c.mem_read32(ext + layout::kRasterExtGxFormat) & 0xFFu);
        const std::size_t bytes =
            shadow::assets::level_bytes(encoding, c.mem_read32(parent + layout::kRasterWidth),
                                        c.mem_read32(parent + layout::kRasterHeight));
        const u8* texels = pixels != 0u && bytes != 0u ? c.guest_memory().peek(GcAddr(pixels), bytes) : nullptr;
        if (texels != nullptr) {
            // 64-bit words mixed with a multiply-rotate: fast enough for a 1 MiB picture per frame.
            std::uint64_t h = 0x9E3779B97F4A7C15ull ^ bytes;
            std::size_t i = 0;
            for (; i + 8 <= bytes; i += 8) {
                std::uint64_t word;
                std::memcpy(&word, texels + i, sizeof(word));
                h = (h ^ word) * 0xFF51AFD7ED558CCDull;
                h ^= h >> 29;
            }
            for (; i < bytes; ++i)
                h = (h ^ texels[i]) * 0x100000001B3ull;
            std::uint64_t& last = movie_texels_[parent];
            if (last != h) {
                last = h;
                render::TextureHandle handle;
                if (build_texture(c, parent, ext, &handle, false))
                    ++stats_.movie_pictures;
            }
        }
    }
    ++stats_.movie_draws;
    stats_.movie_frames[0] = std::min(stats_.movie_frames[0], stats_.frames);
    stats_.movie_frames[1] = std::max(stats_.movie_frames[1], stats_.frames);
    const render::Anchor whole = render::Anchor::Center;
    return im2d(c, type, vertices, count, 0u, 0u, &whole);
}

void RwDriver::on_hud_group(HudGroupKind kind, u32 group_id, float canvas_x, u32 instance) {
    strip_flush();  // a strip is one widget's: it ends with its group
    if (stats_.frames == trace_frame_)
        std::fprintf(stderr, "[r2-trace] hud group kind %u site %08X instance %08X x %.1f\n",
                     static_cast<unsigned>(kind), group_id, instance, canvas_x);
    hud_.on_group(kind, group_id, instance, canvas_x);
    // A world-following widget's sprites sit where the game projected its world point (projection.cpp): placed as
    // projected, never re-anchored or parked; the next announcement (its project's release among them) ends it.
    projected_widget_ = is_world_anchored_widget(group_id);
}

void RwDriver::on_hud_scene(u32 node, u32 cast) {
    hud_.on_scene(node, cast);
}

// An Im2D primitive (r2/SITES_DEVICE.md 2): the client's 24-byte vertices - x, y in pixels of the current camera's
// raster, z the depth itself (0 nearest), RGBA8, u, v - as the primitive named, in order or through its u16 indices,
// coloured by the vertices and, when the Im texture has a raster, textured by it with the alpha mode re-selected from
// that raster as the driver's bind does (without one the mode in force stays). The render states in force apply
// (blend, depth, cull); the driver's orthographic projection is the view's canvas (the camera raster's size), and the
// anchor the 2D layout policy gives the draw keeps it in place on any output aspect. The canvas convention is the
// renderer's: an authored coordinate is the canvas position (the console's half-pixel shift to its pixel centres is
// not carried, so full-screen quads cover the view exactly at every resolution). Canvas draws are not fogged (the
// console's fog left armed by 3D draws reaches 2D at its screen z; at the HUD's z of 0 it adds nothing - counted).
// Every entry returns 1, refused primitives and draws not described included.
u32 RwDriver::im2d(Cpu& c, u32 type, u32 vertex_array, u32 count, u32 index_array, u32 index_count,
                   const render::Anchor* anchor, const render::ImmediateVertex* native_vertices) {
    ++stats_.im2d_primitives;
    tag_draws(c);
    render::Topology topology = render::Topology::Triangles;
    bool fan = false;
    if (!shadow::rwim::topology_of(type, &topology, &fan)) {
        ++stats_.im2d_refused;
        return 1u;
    }
    bool camera_texture = false;
    const u32 raster = im2d_bind(c, &camera_texture);
    const u32 im = im_texture(c);
    // Only draws into a screen view are described: the screen passes' own quads into camera textures, and their
    // composites textured with a camera texture, wait for those passes to be native.
    if (!bracket_valid_) {
        ++stats_.im2d_outside_views;
        return 1u;
    }
    if (camera_texture) {
        ++stats_.im2d_effect_composites;
        ++effect_composite_sites_[c.lr];
        return 1u;
    }
    // NE-RW2 adapter: the game's records (GameCube byte order) become values here, once per array.
    const u8* vertices = native_vertices != nullptr ? nullptr
                         : vertex_array != 0u && count != 0u
                             ? c.guest_memory().peek(GcAddr(vertex_array), std::size_t{count} * kIm2DVertexSize)
                             : nullptr;
    const u8* indices = index_array != 0u && index_count != 0u
                            ? c.guest_memory().peek(GcAddr(index_array), std::size_t{index_count} * 2u)
                            : nullptr;
    if ((native_vertices == nullptr && vertices == nullptr) || (index_array != 0u && indices == nullptr) ||
        !shadow::rwim::list_order(im::host_indices(indices, index_count, &immediate_index_values_), index_count,
                                  count, fan, &immediate_indices_)) {
        ++stats_.im2d_unreadable;
        return 1u;
    }
    shadow::rwim::Binding binding;
    bool has_alpha = false;
    if (raster != 0u) {
        binding.textured = true;
        binding.texture = texture_of(c, im, &has_alpha);
        binding.sampler = sampler_of(c.mem_read32(im + layout::kTextureFilterAddressing));
    }
    if (native_vertices == nullptr) {
        immediate_vertices_.resize(count);
        for (u32 i = 0; i < count; ++i) {
            const u8* v = vertices + std::size_t{kIm2DVertexSize} * i;
            render::ImmediateVertex& out = immediate_vertices_[i];
            out.position = render::Float3{be_float(v), be_float(v + 4), be_float(v + 8)};
            out.colour = rgba8(v + kIm2DVertexColour);
            out.uv[0] = be_float(v + kIm2DVertexUv);
            out.uv[1] = be_float(v + kIm2DVertexUv + 4);
        }
        native_vertices = immediate_vertices_.data();
    }
    // The anchor: the caller's, Projected inside a world-following widget, or the layout policy's decision (place)
    // for this draw in its announced group / cast.
    const render::Anchor projected = render::Anchor::Projected;
    if (anchor == nullptr && projected_widget_)
        anchor = &projected;
    // Split screen: the players' viewports the bracket's view covers are the regions the HUD is laid out in (none -
    // the whole canvas - in single player).
    const View& bracket = views_[bracket_view_];
    hud_.set_regions(bracket.hud_regions.data(), bracket.hud_regions.size(), bracket.width, bracket.height);
    render::Recipe r = recipe();
    r.alpha_cutoff = states_.alpha_test_off == 0u ? alpha_cutoff() : 0.0f;
    const shadow::rwim::Immediate::Result2D drawn =
        imm_.draw2d(native_vertices, count, topology, immediate_indices_, index_array != 0u, binding, r, anchor);
    if (anchor == nullptr && drawn.placement.region >= 0 && !reported_regions_) {
        reported_regions_ = true;
        std::fprintf(stderr,
                     "[r2-2d] frame %llu: a HUD draw (lr %08X, canvas x %.1f..%.1f) laid out in player region %d "
                     "(%s)\n",
                     static_cast<unsigned long long>(stats_.frames), c.lr, drawn.bounds.x0, drawn.bounds.x1,
                     drawn.placement.region, anchor_name(drawn.anchor));
    }
    if (drawn.drawn && stats_.frames == trace_frame_) {
        const render::Recipe& rr = drawn.recipe;
        std::fprintf(stderr,
                     "[r2-trace] im2d lr %08X prim %u vertices %u indices %u bounds (%.1f %.1f)-(%.1f %.1f) z %.3f "
                     "anchor %s%s colour %08X texture %08X (alpha %d) blend %u/%u z %d/%d cull %u fog %d cutoff "
                     "%.3f%s\n",
                     c.lr, type, count, index_count, drawn.bounds.x0, drawn.bounds.y0, drawn.bounds.x1,
                     drawn.bounds.y1, native_vertices[0].position.z, anchor_name(drawn.anchor),
                     drawn.placement.laid_out ? "" : " (off canvas)", native_vertices[0].colour,
                     binding.textured ? binding.texture.value : 0u, has_alpha ? 1 : 0, static_cast<unsigned>(rr.src),
                     static_cast<unsigned>(rr.dst), rr.depth_test ? 1 : 0, rr.depth_write ? 1 : 0,
                     static_cast<unsigned>(rr.cull), drawn.fog_armed ? 1 : 0, rr.alpha_cutoff,
                     glare_window_ ? " [glare source]" : "");
    }
    return 1u;
}

// ClearQuad (0x80499E58), the raster-level clear the original draws as one screen quad: colour replaced (blend ONE /
// ZERO), depth set to the farthest (z test always, z write on) - or only the depth (the colour kept: blend ZERO / ONE),
// whatever the scene had drawn there. The rectangle is in raster pixels like an Im2D vertex; one that covers the whole
// view stretches over it. Like RwCameraClear it leaves the alpha-test mode at 1 and (outside a bracket, with nothing to
// clear or an empty rectangle) draws nothing.
u32 RwDriver::raster_clear(Cpu& c, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, u32 rgba, bool colour,
                           bool depth) {
    (void)c;
    ++stats_.raster_clears;
    states_.alpha_test_off = 1u;
    if (!bracket_valid_ || (!colour && !depth) || w <= 0 || h <= 0) {
        ++stats_.raster_clears_ignored;
        return 1u;
    }
    const View& view = views_[bracket_view_];
    constexpr float kFarthest = 0.99999994f;   // the original's vertex z (binary32 0x3F7FFFFF)
    render::ImmediateVertex vertices[4];
    const float x0 = static_cast<float>(x), y0 = static_cast<float>(y);
    const float x1 = static_cast<float>(x + w), y1 = static_cast<float>(y + h);
    const float corner[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
    for (int i = 0; i < 4; ++i) {
        vertices[i].position = render::Float3{corner[i][0], corner[i][1], kFarthest};
        vertices[i].colour = colour ? rgba : 0xFF000000u;
        vertices[i].uv[0] = vertices[i].uv[1] = 0.0f;
    }
    immediate_indices_.assign({0u, 1u, 2u, 2u, 1u, 3u});
    render::Recipe r;
    r.src = colour ? render::Blend::One : render::Blend::Zero;
    r.dst = colour ? render::Blend::Zero : render::Blend::One;
    r.depth_test = false;
    r.depth_write = depth;
    r.cull = render::Cull::None;
    r.fog = false;
    r.alpha_cutoff = 0.0f;
    const bool whole = x0 <= view.x + 0.5f && y0 <= view.y + 0.5f && x1 >= view.x + view.width - 0.5f &&
                       y1 >= view.y + view.height - 0.5f;
    const render::Anchor anchor = whole ? render::Anchor::Fill : render::Anchor::Center;
    shadow::rwim::Binding binding;
    imm_.draw2d(vertices, 4u, render::Topology::Triangles, immediate_indices_, false, binding, r, &anchor);
    return 1u;
}

// NE-RW2: the strip is collected and continued by the immediate modes (runtime/lift/rwim); this layer tells them when
// it has ended (its group, pass, bracket or frame).
void RwDriver::strip_flush() {
    imm_.flush_strip();
}

// shadow::rwim::Host: a view of this frame, as the strip continuation measures it.
bool RwDriver::view(std::size_t index, shadow::rwim::Host::View* out) const {
    if (index >= views_.size())
        return false;
    const View& v = views_[index];
    out->width = v.width;
    out->height = v.height;
    out->screen_width = v.screen_width;
    out->screen_height = v.screen_height;
    return true;
}

// shadow::rwim::LayoutPolicy: the 2D layout policy's decision for one canvas draw, its anchor as the renderer's (the
// canvas's left edge, centre or right edge stays on the view's; a draw that covers the canvas stretches over the view)
// and its region as the frame packet's.
shadow::rwim::Placement RwDriver::place(const shadow::rwim::Bounds& bounds, bool textured, bool shows) {
    const shadow::ui::HudLayout::Decision decision =
        hud_.layout_draw(shadow::ui::HudBounds{bounds.x0, bounds.y0, bounds.x1, bounds.y1}, textured, shows);
    shadow::rwim::Placement placement;
    placement.laid_out = decision.laid_out;
    placement.anchor = anchor_of(decision.anchor);
    // A centred sprite zoomed past the canvas's side (the title logo's after-images): placed as Center, cut at the
    // view's edge as the console's screen edge cut it, not at the 4:3 box's.
    if (decision.beyond_canvas && placement.anchor == render::Anchor::Center && decision.region < 0)
        placement.anchor = render::Anchor::Continued;
    const View& bracket = views_[bracket_view_];
    if (decision.region >= 0 && static_cast<std::size_t>(decision.region) < bracket.region_ids.size()) {
        placement.region = decision.region;
        placement.region_id = bracket.region_ids[static_cast<std::size_t>(decision.region)];
    }
    return placement;
}

RwDriver::Stats RwDriver::stats() const noexcept {
    Stats s = stats_;
    const shadow::rwim::Immediate::Stats& m = imm_.stats();
    s.im2d_draws = m.im2d_draws;
    s.im2d_outside_views += m.im2d_no_pass;
    s.im2d_off_canvas = m.im2d_off_canvas;
    s.im2d_fog_armed = m.im2d_fog_armed;
    s.im2d_in_regions = m.im2d_in_regions;
    for (std::size_t i = 0; i < render::kAnchorCount; ++i)
        s.im2d_anchors[i] = m.im2d_anchors[i];
    s.strips_continued = m.strips_continued;
    s.strip_copies = m.strip_copies;
    s.strips_rejected = m.strips_rejected;
    s.strips_unfinished = m.strips_unfinished;
    s.im3d_draws = m.im3d_draws;
    return s;
}

// RwIm3DTransform has recorded the pool before this execution (count, vertices, stride, flags, matrix); nothing is
// instanced natively until the render.
u32 RwDriver::im3d_transform(Cpu&, u32 pipeline, u32) {
    return pipeline;
}

// An Im3D render (SubmitNoLight.csl 0x804A0B68, r2/SITES_PIPELINES.md 2.8): the pool's vertices as the primitive the
// render named, in order or through its u16 indices, positioned by the pool's matrix (world space without one),
// coloured by the vertices and - with VERTEXUV - textured by the Im texture, whose bind (0x80498854) sets the alpha
// mode from its raster; without VERTEXUV the alpha mode of the previous draw stays. The client owns its arrays only
// until the call returns: the geometry is copied here. A fan becomes a triangle list.
u32 RwDriver::im3d_render(Cpu& c, u32 pipeline, u32 stash) {
    strip_flush();
    tag_draws(c);
    ++stats_.im3d_primitives;
    if (pipeline == 0u)
        return 0u;  // the render named no primitive the driver knows (RW error 0x25): RxPipelineExecute fails
    const u32 pool = stash - layout::kIm3DStashInPool;
    const u32 count = c.mem_read16(pool + layout::kIm3DCount);
    const u32 vertex_array = c.mem_read32(pool + layout::kIm3DVertices);
    const u32 stride = c.mem_read32(pool + layout::kIm3DStride);
    const u32 flags = c.mem_read32(stash + layout::kIm3DFlags);
    const u32 matrix = c.mem_read32(stash + layout::kIm3DMatrix);
    const u32 primitive = c.mem_read32(stash + layout::kIm3DPrimitive);
    const u32 index_array = c.mem_read32(stash + layout::kIm3DIndices);
    const u32 index_count = c.mem_read32(stash + layout::kIm3DIndexCount);
    shadow::rwim::Binding binding;
    bool has_alpha = false;
    const bool textured = (flags & 1u) != 0u;
    if (textured) {
        const u32 im = im_texture(c);
        if (im != 0u && c.mem_read32(im) != 0u) {
            binding.textured = true;
            binding.texture = texture_of(c, im, &has_alpha);
            binding.sampler = sampler_of(c.mem_read32(im + layout::kTextureFilterAddressing));
            states_.alpha_test_off = has_alpha ? 0u : 1u;
        }
    }
    const u8* vertices = vertex_array != 0u && count != 0u && stride >= 0x24u
                             ? c.guest_memory().peek(GcAddr(vertex_array), std::size_t{stride} * (count - 1u) + 0x24u)
                             : nullptr;
    const u8* indices = index_array != 0u && index_count != 0u
                            ? c.guest_memory().peek(GcAddr(index_array), std::size_t{index_count} * 2u)
                            : nullptr;
    render::Topology topology = render::Topology::Triangles;
    bool fan = false;
    if (!shadow::rwim::topology_of(primitive, &topology, &fan))
        vertices = nullptr;
    if (vertices == nullptr || (index_array != 0u && indices == nullptr) || !ensure_pass()) {
        ++stats_.im3d_skipped;
        return pipeline;
    }
    // NE-RW2 adapter: the pool's records (GameCube byte order) become values here, once per array.
    immediate_vertices_.resize(count);
    for (u32 i = 0; i < count; ++i) {
        const u8* v = vertices + std::size_t{stride} * i;
        render::ImmediateVertex& out = immediate_vertices_[i];
        out.position = render::Float3{be_float(v), be_float(v + 4), be_float(v + 8)};
        out.colour = rgba8(v + layout::kIm3DVertexColour);
        out.uv[0] = be_float(v + layout::kIm3DVertexUv);
        out.uv[1] = be_float(v + layout::kIm3DVertexUv + 4);
    }
    if (!shadow::rwim::list_order(im::host_indices(indices, index_count, &immediate_index_values_), index_count, count,
                                  fan, &immediate_indices_)) {
        ++stats_.im3d_skipped;
        return pipeline;
    }
    const bool listed = fan || indices != nullptr;
    render::Float3x4 world;
    if (matrix != 0u)
        world = frame_matrix(c, matrix);
    render::Recipe r = recipe();
    r.alpha_cutoff = states_.alpha_test_off == 0u ? alpha_cutoff() : 0.0f;
    imm_.draw3d(immediate_vertices_.data(), count, topology, immediate_indices_, listed, world, binding, r);
    if (stats_.frames == trace_frame_) {
        std::fprintf(stderr,
                     "[r2-trace] im3d prim %u vertices %u indices %u flags %X matrix %08X texture %08X (alpha %d) "
                     "first (%.1f %.1f %.1f) colour %08X blend %u/%u z %d/%d cull %u fog %d cutoff %.3f%s; camera "
                     "window %.4f x %.4f parallel %d near %.3f\n",
                     primitive, count, index_count, flags, matrix, binding.textured ? binding.texture.value : 0u,
                     has_alpha ? 1 : 0, immediate_vertices_[0].position.x, immediate_vertices_[0].position.y,
                     immediate_vertices_[0].position.z, immediate_vertices_[0].colour, static_cast<unsigned>(r.src),
                     static_cast<unsigned>(r.dst), r.depth_test ? 1 : 0, r.depth_write ? 1 : 0,
                     static_cast<unsigned>(r.cull), r.fog ? 1 : 0, r.alpha_cutoff,
                     glare_window_ ? " [glare source]" : "", pass_camera_.view_window_x, pass_camera_.view_window_y,
                     pass_camera_.parallel ? 1 : 0, pass_camera_.near_plane);
    }
    return pipeline;
}

void RwDriver::camera_texture_copy(Cpu& c, u32, u32) {
    ++stats_.copies;
    ++copy_sites_[c.lr];
}

}  // namespace gekko::scaffold
