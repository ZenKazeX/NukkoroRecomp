// SCAFFOLDING (R2): the screen passes' lowered sites - the motion trail, the heat haze and refraction distortions, the screen colour's multiply - described as the renderer's snapshots, view history and canvas draws - deleted by R5 / R6: the effect library's lift describes its passes to the renderer itself
#include <algorithm>
#include <cstdio>

#include "gekko/cpu.h"
#include "im_vertex.h"
#include "rw_driver.h"

// r2/SITES_PRESENT_EFFECTS.md 3.2 (DrawBlur + SaveScreen), 3.3 (DrawScreenColor), 3.5 (the full-screen heat shimmer),
// 3.7 (refraction elements); R2_DRIVER.md 4.9. The workers stay translated - their decisions, their render-state saves
// and restores, every guest write - and only what they hand the GPU is lowered here: their copies of the screen and
// the Im2D primitives that sample those copies (or, for the multiply, compute a product a blend state gives directly).
// The console copied the screen into a camera texture because it could not sample its frame buffer; natively a
// distortion samples a snapshot of the view's scene, the trail the view's history.

namespace gekko::scaffold {

namespace render = shadow::render;

void RwDriver::skip_screen_pass(Cpu& c, ScreenSkip why) {
    ++stats_.screen_pass_skipped;
    ++stats_.screen_skips[static_cast<u32>(why)];
    ++screen_skip_sites_[(u64{static_cast<u32>(why)} << 32) | c.lr];
    stats_.screen_skip_frames[0] = std::min(stats_.screen_skip_frames[0], stats_.frames);
    stats_.screen_skip_frames[1] = std::max(stats_.screen_skip_frames[1], stats_.frames);
    if (stats_.screen_pass_skipped <= 40u) {
        const bool snapshot = bracket_valid_ && bracket_view_ < views_.size() && views_[bracket_view_].has_snapshot;
        std::fprintf(stderr,
                     "[r2-skip] frame %llu site %08X reason %u: brackets this frame %u on screens, %u on other rasters; "
                     "latest bracket raster type 0x%X; views %zu; snapshot of the bracket's view %d\n",
                     static_cast<unsigned long long>(stats_.frames), c.lr - 4u, static_cast<unsigned>(why),
                     frame_screen_brackets_, frame_other_brackets_, last_bracket_type_, views_.size(),
                     snapshot ? 1 : 0);
    }
}

// The copy function (CameraTextureCopy 0x80496208) reads the frame buffer at the destination raster's own offset and
// size - doubled for a half-resolution copy, which no shipped site makes - so a copy into a parent raster reads the
// whole picture. The views of the frame (camera-raster rectangles in the same frame-buffer pixels) it overlaps are
// what it captured. GXCopyTex reads the frame buffer as it is: a copy needs no camera bracket (the heat haze copies
// the screen before the frame's first bracket in the frames after a respawn, when the view holds only its clear).
template <typename F>
std::size_t RwDriver::for_each_view_copied(Cpu& c, u32 raster, u32 half_resolution, F&& f) {
    if (raster == 0u)
        return 0;
    const float scale = half_resolution != 0u ? 2.0f : 1.0f;
    const float x = scale * static_cast<float>(static_cast<s16>(c.mem_read16(raster + layout::kRasterOffsetX)));
    const float y = scale * static_cast<float>(static_cast<s16>(c.mem_read16(raster + layout::kRasterOffsetY)));
    const float w = scale * static_cast<float>(c.mem_read32(raster + layout::kRasterWidth));
    const float h = scale * static_cast<float>(c.mem_read32(raster + layout::kRasterHeight));
    std::size_t covered = 0;
    for (View& v : views_) {
        if (v.x < x + w && x < v.x + v.width && v.y < y + h && y < v.y + v.height) {
            f(v);
            ++covered;
        }
    }
    return covered;
}

void RwDriver::screen_snapshot(Cpu& c, u32 raster, u32 half_resolution) {
    const std::size_t taken = for_each_view_copied(c, raster, half_resolution, [&](View& v) {
        v.snapshot = builder_.snapshot(v.id);
        v.has_snapshot = true;
        if (stats_.frames == trace_frame_) {
            // When in the frame (the draws recorded so far) and of which view: what a distortion will sample.
            std::fprintf(stderr,
                         "[r2-trace] snapshot lr %08X raster %08X half %u view %u rect (%.0f %.0f %.0f %.0f) snapshot %u "
                         "bracket %d (view %u) atomics so far %llu im2d %llu\n",
                         c.lr, raster, half_resolution, static_cast<unsigned>(v.id), v.x, v.y, v.width, v.height,
                         static_cast<unsigned>(v.snapshot), bracket_valid_ ? 1 : 0,
                         bracket_valid_ && bracket_view_ < views_.size() ? static_cast<unsigned>(views_[bracket_view_].id)
                                                                         : 0u,
                         static_cast<unsigned long long>(stats_.atomics),
                         static_cast<unsigned long long>(stats_.im2d_primitives));
        }
    });
    if (taken == 0) {
        skip_screen_pass(c, ScreenSkip::NoView);
        return;
    }
    pass_open_ = false;  // the snapshot closed it
    stats_.screen_snapshots += taken;
    if (!bracket_valid_)
        ++stats_.screen_copies_unbracketed;
}

// SaveScreen's first copy (the whole frame, after the 3D and the world-anchored 2D, before the screen colour and
// the HUD): the history the next frame's DrawBlur draws. Its downsampling into 320 x 240 (the stomp quad, the second
// copy, the restore quad) is the console's way to a half-size history and is not reproduced: the history is the view
// at full resolution.
void RwDriver::trail_save(Cpu& c, u32 raster, u32 half_resolution) {
    const std::size_t saved =
        for_each_view_copied(c, raster, half_resolution, [&](View& v) { builder_.save_history(v.id); });
    if (saved == 0) {
        skip_screen_pass(c, ScreenSkip::NoView);
        return;
    }
    pass_open_ = false;  // the save closed it
    stats_.trail_saves += saved;
    if (!bracket_valid_)
        ++stats_.screen_copies_unbracketed;
}

u32 RwDriver::trail_draw(Cpu& c, u32 type, u32 vertices, u32 count) {
    // The quad spans the view zoomed about its centre (B+0x04) and samples the whole history: laid out over the view.
    tag_draws(c, render::EffectLayer::MotionBlur);
    return screen_pass_draw(c, type, vertices, count, render::ImageSource::History, render::Anchor::Fill, false,
                            "trail");
}

u32 RwDriver::shimmer_draw(Cpu& c, u32 type, u32 vertices, u32 count) {
    // The grid covers fractions of the view (S+0x24 .. +0x40): laid out over the view.
    tag_draws(c, render::EffectLayer::HeatHaze);
    return screen_pass_draw(c, type, vertices, count, render::ImageSource::Snapshot, render::Anchor::Fill, false,
                            "heat haze");
}

u32 RwDriver::refraction_draw(Cpu& c, u32 type, u32 vertices, u32 count) {
    // The patch sits where the element projects through the renderer's camera, whose window is the authored picture
    // (the camera sync's, projection.cpp camera_picture_window): its canvas units are the picture's - placed as the
    // canvas is (Center), sampling the picture at the same places.
    tag_draws(c, render::EffectLayer::Refraction);
    return screen_pass_draw(c, type, vertices, count, render::ImageSource::Snapshot, render::Anchor::Center, false,
                            "refraction");
}

u32 RwDriver::screen_multiply(Cpu& c, u32 type, u32 vertices, u32 count) {
    // ONE / ZERO with a copy of the screen times the vertex colour is the screen times the colour: a blend.
    tag_draws(c, render::EffectLayer::ScreenColour);
    return screen_pass_draw(c, type, vertices, count, render::ImageSource::Texture, render::Anchor::Fill, true,
                            "screen multiply");
}

// The primitive's vertices as a canvas draw. A screen image's texture coordinates are the worker's: normalised by the
// screen raster and offset by half a texel, matching the driver's half-pixel position shift on the console (the
// canvas has neither) - they become the canvas points they address, x = u W - 1/2 - the view's left, so an undisplaced
// vertex samples exactly what lies under it.
u32 RwDriver::screen_pass_draw(Cpu& c, u32 type, u32 vertex_array, u32 count, render::ImageSource source,
                               render::Anchor anchor, bool multiply, const char* what) {
    ++stats_.im2d_primitives;
    render::ImmediateDraw draw;
    bool fan = false;
    if (!shadow::rwim::topology_of(type, &draw.topology, &fan)) {
        ++stats_.im2d_refused;
        return 1u;
    }
    bool camera_texture = false;
    im2d_bind(c, &camera_texture);  // the driver's bind re-selects the alpha mode whatever is drawn
    if (!bracket_valid_) {
        skip_screen_pass(c, ScreenSkip::NoBracket);
        return 1u;
    }
    if (source == render::ImageSource::Snapshot && no_distortion_) {
        skip_screen_pass(c, ScreenSkip::Disabled);
        return 1u;
    }
    if (source == render::ImageSource::Snapshot && !views_[bracket_view_].has_snapshot) {
        skip_screen_pass(c, ScreenSkip::NoSnapshot);
        return 1u;
    }
    const u8* vertices = vertex_array != 0u && count != 0u
                             ? c.guest_memory().peek(GcAddr(vertex_array), std::size_t{count} * im::k2DVertexSize)
                             : nullptr;
    if (vertices == nullptr || !shadow::rwim::list_order(nullptr, 0u, count, fan, &immediate_indices_)) {
        skip_screen_pass(c, ScreenSkip::Unreadable);
        return 1u;
    }
    const View& view = views_[bracket_view_];
    immediate_vertices_.resize(count);
    float u_min = 1.0e9f, u_max = -1.0e9f, v_min = 1.0e9f, v_max = -1.0e9f;
    float x_min = 1.0e9f, x_max = -1.0e9f;
    for (u32 i = 0; i < count; ++i) {
        const u8* v = vertices + std::size_t{im::k2DVertexSize} * i;
        render::ImmediateVertex& out = immediate_vertices_[i];
        out.position = render::Float3{im::be_float(v), im::be_float(v + 4), im::be_float(v + 8)};
        out.colour = im::rgba8(v + im::k2DVertexColour);
        out.uv[0] = out.uv[1] = 0.0f;
        if (source != render::ImageSource::Texture) {
            out.uv[0] = im::be_float(v + im::k2DVertexUv) * view.screen_width - 0.5f - view.x;
            out.uv[1] = im::be_float(v + im::k2DVertexUv + 4) * view.screen_height - 0.5f - view.y;
            u_min = std::min(u_min, out.uv[0]);
            u_max = std::max(u_max, out.uv[0]);
            v_min = std::min(v_min, out.uv[1]);
            v_max = std::max(v_max, out.uv[1]);
        }
        x_min = std::min(x_min, out.position.x);
        x_max = std::max(x_max, out.position.x);
    }
    // A grid placed as the canvas is (Center: a refraction patch at its place in the world) that spans its view's
    // whole canvas width is a screen cover (the Chaos Control warp's full-screen refraction, CH 10:3x frame 558): on
    // the console it covered the whole picture, so it is laid out over the view as the heat haze is - positions and
    // the screen points it samples placed alike, so an undisplaced vertex still samples what lies under it (renderer
    // canvas_image_rows) - instead of stopping at the 4:3 box (HW_REPORT M / O; DESIGN.md section 7).
    bool widened = false;
    if (anchor == render::Anchor::Center && view.width > 0.0f && x_min <= view.x + 0.5f &&
        x_max >= view.x + view.width - 0.5f) {
        anchor = render::Anchor::Fill;
        widened = true;
        ++stats_.screen_covers_widened;
    }
    draw.space = render::Space::Canvas;
    draw.anchor = anchor;
    draw.source = source;
    draw.snapshot = view.snapshot;
    draw.sampler.filter = render::Filter::Linear;
    draw.sampler.u = draw.sampler.v = render::Address::Clamp;
    render::Recipe r = recipe();
    r.alpha_cutoff = states_.alpha_test_off == 0u ? alpha_cutoff() : 0.0f;
    r.fog = false;
    if (multiply) {
        r.src = render::Blend::DestColor;
        r.dst = render::Blend::Zero;
    }
    if (!ensure_pass()) {
        skip_screen_pass(c, ScreenSkip::NoPass);
        return 1u;
    }
    const bool listed = !immediate_indices_.empty();
    builder_.immediate(draw, immediate_vertices_.data(), count, listed ? immediate_indices_.data() : nullptr,
                       listed ? static_cast<std::uint32_t>(immediate_indices_.size()) : 0u, render::Float3x4{}, r);
    u64* frames = nullptr;
    if (source == render::ImageSource::History) {
        ++stats_.trail_draws;
        frames = stats_.trail_frames;
    } else if (source == render::ImageSource::Snapshot) {
        ++stats_.distortion_draws;
        frames = stats_.distortion_frames;
    } else {
        ++stats_.screen_multiplies;
        frames = stats_.multiply_frames;
    }
    frames[0] = std::min(frames[0], stats_.frames);
    frames[1] = std::max(frames[1], stats_.frames);
    if (stats_.frames == trace_frame_) {
        std::fprintf(stderr,
                     "[r2-trace] %s lr %08X prim %u vertices %u first (%.1f %.1f z %.3f) colour %08X canvas u "
                     "%.1f..%.1f v %.1f..%.1f x %.1f..%.1f view %u snapshot %u%s blend %u/%u z %d/%d cull %u "
                     "cutoff %.3f%s\n",
                     what, c.lr, type, count, immediate_vertices_[0].position.x, immediate_vertices_[0].position.y,
                     immediate_vertices_[0].position.z, immediate_vertices_[0].colour,
                     source != render::ImageSource::Texture ? u_min : 0.0f,
                     source != render::ImageSource::Texture ? u_max : 0.0f,
                     source != render::ImageSource::Texture ? v_min : 0.0f,
                     source != render::ImageSource::Texture ? v_max : 0.0f, x_min, x_max,
                     static_cast<unsigned>(view.id), static_cast<unsigned>(view.snapshot),
                     widened ? " widened to the view" : "", static_cast<unsigned>(r.src),
                     static_cast<unsigned>(r.dst), r.depth_test ? 1 : 0, r.depth_write ? 1 : 0,
                     static_cast<unsigned>(r.cull), r.alpha_cutoff, glare_window_ ? " [glare source]" : "");
    }
    return 1u;
}

}  // namespace gekko::scaffold
