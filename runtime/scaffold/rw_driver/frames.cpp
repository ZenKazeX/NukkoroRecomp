// SCAFFOLDING (R2): camera brackets, clears and the present as views, passes and frame packets - deleted by R5: the render tree's lift describes views and passes to the renderer itself
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "gekko/cpu.h"
#include "gekko/gc_addr.h"
#include "rw_driver.h"

namespace gekko::scaffold {

namespace render = shadow::render;

namespace {

constexpr u32 kRasterTypeCamera = 2;
constexpr u32 kClearImage = 1;
// The picture's shape as the game authored it: the television's 4:3, whatever the frame buffer's lines.
constexpr float kDisplayAspect = 4.0f / 3.0f;

// The guest word at `address`, or `fallback` when it is not mapped (an owner's memory may have been returned).
u32 peek32(Cpu& c, u32 address, u32 fallback) {
    const u8* p = c.guest_memory().peek(GcAddr(address), 4);
    if (p == nullptr)
        return fallback;
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}

bool same_camera(const render::Camera& a, const render::Camera& b) {
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k)
            if (a.frame.m[r][k] != b.frame.m[r][k])
                return false;
    return a.view_window_x == b.view_window_x && a.view_window_y == b.view_window_y && a.near_plane == b.near_plane &&
           a.far_plane == b.far_plane && a.parallel == b.parallel;
}

}  // namespace

void RwDriver::open_frame() {
    if (frame_open_)
        return;
    builder_.begin_frame(stats_.frames, render::Float4{0, 0, 0, 1});
    frame_open_ = true;
    tag_layers_ = render::effect_layer_switches().tagging();
}

// The game's camera (r2/SITES_DEVICE.md 4.2): the frame's local-to-world matrix as it is now (the begin wrapper
// synchronised dirty frames before the dispatch), the view window, near and far, the projection type.
render::Camera RwDriver::camera_of(Cpu& c, u32 camera) {
    render::Camera out;
    const u32 frame = c.mem_read32(camera + layout::kCameraFrame);
    float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, at[3] = {0, 0, 1}, pos[3] = {0, 0, 0};
    if (frame != 0u) {
        const u32 ltm = frame + layout::kFrameLtm;
        for (int k = 0; k < 3; ++k) {
            right[k] = read_f32(c, ltm + 0x00u + 4u * k);
            up[k] = read_f32(c, ltm + 0x10u + 4u * k);
            at[k] = read_f32(c, ltm + 0x20u + 4u * k);
            pos[k] = read_f32(c, ltm + 0x30u + 4u * k);
        }
    }
    // The renderer's frame names the screen's right: forward x up. RenderWare's frames are right-handed
    // (right x up = at), so the camera's `right` points to the left of the picture; the sign is measured, not
    // assumed, and kept in the stats.
    const float handed = (right[1] * up[2] - right[2] * up[1]) * at[0] + (right[2] * up[0] - right[0] * up[2]) * at[1] +
                         (right[0] * up[1] - right[1] * up[0]) * at[2];
    stats_.handedness = handed;
    const float flip = handed > 0.0f ? -1.0f : 1.0f;
    for (int k = 0; k < 3; ++k) {
        out.frame.m[k][0] = flip * right[k];
        out.frame.m[k][1] = up[k];
        out.frame.m[k][2] = at[k];
        out.frame.m[k][3] = pos[k];
    }
    out.view_window_x = read_f32(c, camera + layout::kCameraViewWindow);
    out.view_window_y = read_f32(c, camera + layout::kCameraViewWindow + 4u);
    out.near_plane = read_f32(c, camera + layout::kCameraNear);
    out.far_plane = read_f32(c, camera + layout::kCameraFar);
    out.parallel = c.mem_read32(camera + layout::kCameraProjection) == 2u;
    out.framing = framing_;  // render/view.h: what each view displays of this window
    if (read_f32(c, camera + layout::kCameraViewOffset) != 0.0f ||
        read_f32(c, camera + layout::kCameraViewOffset + 4u) != 0.0f)
        ++stats_.view_offsets;
    return out;
}

// A view is a camera frame-buffer rectangle of a screen raster: the whole of a top-level camera raster (the main
// screen, 640 x 480; the PAL 50 / 60 Hz question's own 640 x 528 one), or a sub-raster of one (split screen swaps the
// one camera's raster per half). Cameras rendering into camera textures (the screen effects) have none until those
// passes are native.
//
// The game draws all of them into ONE picture, in order (docs/renderer-re-20261007/01_FRAME.md 3.5): in split screen
// RenderStart clears the whole screen, RenderDiv draws viewport 1 and then viewport 2, and RenderStatic selects the
// whole screen again for the overlay - the HUD, the fades - drawn over both. So a bracket on a rectangle that later
// views were opened over (and drawn into) is not the old view continued: it is a new view that draws over them
// (render/frame.h ViewRecord::over_views), and the players' viewports it covers are the regions its HUD is laid out
// in (render/view.h CanvasRegion; the game composes each player's HUD inside its own viewport).
RwDriver::View* RwDriver::view_for(Cpu& c, u32 camera) {
    const u32 raster = c.mem_read32(camera + layout::kCameraFrameBuffer);
    if (raster == 0u)
        return nullptr;
    const u32 type = c.mem_read8(raster + layout::kRasterType) & 7u;
    if (type != kRasterTypeCamera)
        return nullptr;
    u32 top = c.mem_read32(raster + layout::kRasterParent);
    if (top == 0u)
        top = raster;
    const float x = static_cast<float>(static_cast<s16>(c.mem_read16(raster + layout::kRasterOffsetX)));
    const float y = static_cast<float>(static_cast<s16>(c.mem_read16(raster + layout::kRasterOffsetY)));
    const float w = static_cast<float>(c.mem_read32(raster + layout::kRasterWidth));
    const float h = static_cast<float>(c.mem_read32(raster + layout::kRasterHeight));
    const float screen_w = static_cast<float>(c.mem_read32(top + layout::kRasterWidth));
    const float screen_h = static_cast<float>(c.mem_read32(top + layout::kRasterHeight));
    if (!(w > 0.0f && h > 0.0f && screen_w > 0.0f && screen_h > 0.0f))
        return nullptr;
    open_frame();
    bool over_views = false;
    for (std::size_t i = views_.size(); i-- > 0;) {
        const View& v = views_[i];
        if (v.x == x && v.y == y && v.width == w && v.height == h && v.screen_width == screen_w &&
            v.screen_height == screen_h) {
            for (std::size_t j = i + 1; j < views_.size(); ++j) {
                const View& later = views_[j];
                if (later.screen_width == screen_w && later.screen_height == screen_h && later.x < x + w &&
                    later.x + later.width > x && later.y < y + h && later.y + later.height > y)
                    over_views = true;
            }
            if (!over_views)
                return &views_[i];
            break;
        }
    }
    View v;
    v.x = x;
    v.y = y;
    v.width = w;
    v.height = h;
    v.screen_width = screen_w;
    v.screen_height = screen_h;
    v.camera = camera_of(c, camera);
    // The top-level raster is the whole picture, which the console showed 4:3 whatever its lines (640 x 528 on PAL
    // 50 Hz); a view is its share of the output, and its 2D canvas is the camera raster (Im2D positions are pixels of
    // that raster, from its origin) at its share of that 4:3 picture.
    render::ViewRect rect;
    rect.x = x / screen_w;
    rect.y = y / screen_h;
    rect.width = w / screen_w;
    rect.height = h / screen_h;
    if (over_views) {
        // The players' viewports this view covers: the views opened before it that lie inside it, smaller than it.
        for (const View& under : views_) {
            if (under.screen_width == screen_w && under.screen_height == screen_h && under.x >= x && under.y >= y &&
                under.x + under.width <= x + w && under.y + under.height <= y + h && under.width * under.height < w * h) {
                shadow::ui::HudLayout::Region region;
                region.x0 = under.x - x;
                region.y0 = under.y - y;
                region.x1 = region.x0 + under.width;
                region.y1 = region.y0 + under.height;
                v.hud_regions.push_back(region);
                v.region_ids.push_back(builder_.canvas_region(
                    render::CanvasRegion{region.x0, region.y0, under.width, under.height}));
            }
        }
        v.over_views = true;
        ++stats_.views_over_views;
        if (!reported_over_views_) {
            reported_over_views_ = true;
            std::fprintf(stderr,
                         "[r2-2d] frame %llu: a %.0f x %.0f view drawn over %zu earlier view(s) of the frame (split "
                         "screen overlay), %zu player region(s)\n",
                         static_cast<unsigned long long>(stats_.frames), w, h, views_.size(),
                         v.hud_regions.size());
        }
    }
    v.id = builder_.view(v.camera, rect, render::Canvas{w, h, kDisplayAspect * rect.width / rect.height}, over_views);
    views_.push_back(v);
    return &views_.back();
}

// A bracket only names the view and the camera; the pass opens at its first draw (ensure_pass), so brackets that
// draw nothing natively yet (2D, the screen effects) leave no empty pass behind.
u32 RwDriver::camera_begin(Cpu& c, u32 camera) {
    ++stats_.cameras;
    if (!frame_open_)
        sample_event_framing(c);  // the frame's views are shown as the game's state is when it starts drawing
    current_camera_ = camera;
    const u32 raster = c.mem_read32(camera + layout::kCameraFrameBuffer);
    last_bracket_type_ = raster != 0u ? (c.mem_read8(raster + layout::kRasterType) & 7u) : 0xFEu;
    View* view = view_for(c, camera);
    if (view == nullptr) {
        ++frame_other_brackets_;
        bracket_valid_ = false;
        return 1u;
    }
    ++frame_screen_brackets_;
    bracket_camera_ = camera_of(c, camera);
    if (!same_camera(view->camera, bracket_camera_))
        ++stats_.camera_mismatches;
    bracket_view_ = static_cast<std::size_t>(view - views_.data());
    bracket_valid_ = true;
    return 1u;
}

bool RwDriver::ensure_pass() {
    if (!bracket_valid_)
        return false;
    // Inside the glare window the view's draws are its glare sources.
    const render::PassKind kind = glare_window_ && bracket_view_ == glare_view_ ? render::PassKind::GlareSources
                                                                                : render::PassKind::Scene;
    if (pass_open_ && pass_view_ == bracket_view_ && pass_kind_ == kind && same_camera(pass_camera_, bracket_camera_))
        return true;
    strip_flush();  // a strip ends with its pass
    if (pass_open_)
        builder_.end_pass();
    render::Clear clear;
    clear.colour = clear.depth = true;
    builder_.begin_pass(views_[bracket_view_].id, kind, clear, &bracket_camera_);
    pass_open_ = true;
    pass_view_ = bracket_view_;
    pass_kind_ = kind;
    pass_camera_ = bracket_camera_;
    ++stats_.passes;
    return true;
}

u32 RwDriver::camera_end(Cpu& c, u32 camera) {
    // The in-stage camera's bracket is ending: the far-object proxies join its pass.
    if (camera != 0u && camera == c.mem_read32(layout::kInStageCameraHolder))
        draw_far_proxies(c);
    strip_flush();
    current_camera_ = 0u;
    return 1u;
}

// RwCameraClear: a full-raster quad (ClearQuad 0x80499E58) that leaves the alpha-test mode at 1. The frame's first
// clear of a screen view is the scene's background; the later ones belong to the screen passes (PreGlare clears the
// scene raster to black after copying it out) and wait for them.
u32 RwDriver::camera_clear(Cpu& c, u32 camera, u32 colour, u32 mode) {
    ++stats_.clears;
    states_.alpha_test_off = 1u;
    View* view = camera != 0u ? view_for(c, camera) : nullptr;
    if (view == nullptr || background_set_ || (mode & kClearImage) == 0u) {
        ++stats_.clears_ignored;
        return 1u;
    }
    const u32 rgba = c.mem_read32(colour);
    background_ = render::Float4{static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f,
                                 static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
                                 static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f, 1.0f};
    background_set_ = true;
    return 1u;
}

// The display copy of the present (GXCopyDisp's site): the frame is complete. The packet goes to the render thread;
// the game never waits for it (a full queue drops the older unrendered packet).
void RwDriver::submit_frame(Cpu& c) {
    open_frame();
    strip_flush();
    if (pass_open_)
        builder_.end_pass();
    if (glare_window_) {
        // PreGlare copied the scene out and PostGlare never ran: no shipped path does this (counted).
        ++stats_.glares_unclosed;
        glare_window_ = false;
    }
    render::FramePacket packet = builder_.finish();
    packet.background = background_;
    packet.game_ticks = c.game_clock().now();
    if (event_framed_.load(std::memory_order_acquire)) {
        packet.content_aspect = 4.0f / 3.0f;
        ++stats_.event_framed;
    }
    // A video-mode switch (the top-level raster changes size: 640x528 in 50 Hz mode, 640x480 in 60 Hz mode) is shown
    // as one black frame, as a TV blanks while it resyncs. Otherwise the first picture of the new mode - the PAL
    // 50/60 Hz prompt redrawn once at 480 lines as it closes - pops visibly against the 528-line one before it
    // (user report 2026-10-26). Presentation only: the game's frame and state are untouched.
    if (!views_.empty()) {
        const float screen_w = views_.front().screen_width, screen_h = views_.front().screen_height;
        const bool switched = last_screen_width_ > 0.0f &&
                              (screen_w != last_screen_width_ || screen_h != last_screen_height_);
        last_screen_width_ = screen_w;
        last_screen_height_ = screen_h;
        if (switched) {
            render::FramePacket blank;
            blank.tick = packet.tick;
            blank.game_ticks = packet.game_ticks;
            blank.background = render::Float4{0.0f, 0.0f, 0.0f, 1.0f};
            packet = std::move(blank);
            ++stats_.mode_switch_blanks;
        }
    }
    // VISetBlack(TRUE): the console's VI shows black while the flag is in force, whatever the frame buffer holds (the
    // game keeps drawing and flipping). The frame is shown as one black frame (presentation only: the game's frame and
    // state are untouched), like the mode switch's.
    const bool display_black = display_blank_ && display_blank_();
    if (display_black) {
        render::FramePacket blank;
        blank.tick = packet.tick;
        blank.game_ticks = packet.game_ticks;
        blank.background = render::Float4{0.0f, 0.0f, 0.0f, 1.0f};
        packet = std::move(blank);
        ++stats_.display_blanks;
    }
    static const bool trace_black = std::getenv("SHADOW_TRACE_VI_BLACK") != nullptr;
    if (trace_black)
        std::fprintf(stderr, "[vi-black] present %llu tick %llu black %d\n",
                     static_cast<unsigned long long>(stats_.frames + 1u),
                     static_cast<unsigned long long>(packet.game_ticks), display_black ? 1 : 0);
    if (frame_observer_)
        frame_observer_(packet);
    material_probe_.frame(packet);
    // The fade-in's previous view: the camera the world was drawn with this frame - the perspective pass camera that
    // drew the most (the view's own camera is the frame's fixed one; the game opens a camera bracket per land class,
    // the sky and the effects).
    const render::PassRecord* world_pass = nullptr;
    for (const render::PassRecord& p : packet.passes)
        if (p.has_camera && !p.camera.parallel && (world_pass == nullptr || p.draw_count > world_pass->draw_count))
            world_pass = &p;
    if (world_pass != nullptr) {
        const render::Camera& cam = world_pass->camera;
        AppearFade::View view;
        for (int r = 0; r < 3; ++r) {
            view.right[r] = cam.frame.m[r][0];
            view.up[r] = cam.frame.m[r][1];
            view.forward[r] = cam.frame.m[r][2];
            view.position[r] = cam.frame.m[r][3];
        }
        view.window_x = cam.view_window_x;
        view.window_y = cam.view_window_y;
        view.near_plane = cam.near_plane;
        view.far_plane = cam.far_plane;
        appear_.end_frame(&view);
    } else {
        appear_.end_frame(nullptr);
    }
    const u64 dropped_before = frames_.stats().dropped;
    frames_.push(std::move(packet));
    stats_.frames_dropped += frames_.stats().dropped - dropped_before;
    ++stats_.frames;
    uv_stage_check(c);
    if (skin_hash_ && frame_skins_ != 0u)
        std::fprintf(stderr, "[skinhash] %llu %u %016llX\n", static_cast<unsigned long long>(stats_.frames), frame_skins_,
                     static_cast<unsigned long long>(frame_skin_hash_));
    frame_skin_hash_ = 0xCBF29CE484222325ull;
    frame_skins_ = 0u;
    far_drawn_this_frame_ = false;
    game_lights_.clear();
    world_recipe_valid_ = false;
    frame_open_ = false;
    pass_open_ = false;
    bracket_valid_ = false;
    views_.clear();
    background_set_ = false;
    frame_screen_brackets_ = frame_other_brackets_ = 0u;
    last_bracket_type_ = 0xFFu;
    hud_.end_frame();  // the 2D layout's per-frame groups close; its remembered anchors stay
    reconcile(c);
    sample_event_framing(c);  // the next frame's update sees the output it will be shown on
}

void RwDriver::on_state_loaded() {
    for (Owned& o : textures_) {
        if (o.handle != 0u) {
            renderer_.release(render::TextureHandle{o.handle}, stats_.frames);
            ++stats_.textures_released;
        }
    }
    for (Owned& o : meshes_) {
        if (o.handle != 0u) {
            renderer_.release(render::MeshHandle{o.handle}, stats_.frames);
            ++stats_.meshes_released;
        }
    }
    textures_.clear();
    texture_index_dirty_ = true;
    meshes_.clear();
    movie_texels_.clear();
    imm_.reset();  // NE-RW2: the strip being collected belonged to the replaced timeline
    // The frame being described belonged to the replaced timeline.
    builder_.discard();  // C3: the builder holds a mutex now (not assignable); the same reset
    frame_open_ = false;
    pass_open_ = false;
    glare_window_ = false;
    bracket_valid_ = false;
    views_.clear();
    background_set_ = false;
    frame_screen_brackets_ = frame_other_brackets_ = 0u;
    last_bracket_type_ = 0xFFu;
    current_camera_ = 0u;
    hud_.end_frame();
}

namespace {

// 2: + the 2D layout's player-region anchors (split screen). A state with none (every single-player state) is written
// as format 1, byte for byte what it was before them; both load.
constexpr u8 kDriverStateFormat = 2;
constexpr u8 kDriverStateFormatWithoutRegions = 1;

void put_u32(std::vector<u8>& out, u32 value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<u8>(value >> shift));
}

void put_f32(std::vector<u8>& out, float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

bool get_u32(const u8*& cursor, const u8* end, u32& value) {
    if (end - cursor < 4) return false;
    value = 0;
    for (int shift = 0; shift < 32; shift += 8) value |= static_cast<u32>(*cursor++) << shift;
    return true;
}

bool get_f32(const u8*& cursor, const u8* end, float& value) {
    u32 bits = 0;
    if (!get_u32(cursor, end, bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

bool get_u8(const u8*& cursor, const u8* end, u8& value) {
    if (end - cursor < 1) return false;
    value = *cursor++;
    return true;
}

}  // namespace

void RwDriver::save_state(std::vector<u8>& out) const {
    const bool regions = hud_.has_region_memory();
    out.push_back(regions ? kDriverStateFormat : kDriverStateFormatWithoutRegions);
    const RenderStates& s = states_;
    for (const u32 v : {s.z_write, s.z_test, s.cull, s.fog_enable, s.fog_type, s.fog_colour})
        put_u32(out, v);
    for (const float v : {s.fog_start, s.fog_end, s.fog_near, s.fog_far})
        put_f32(out, v);
    for (const u32 v : {s.src_blend, s.dst_blend, s.alpha_test_off, s.cmp0, s.cmp1, s.alpha_op})
        put_u32(out, v);
    out.push_back(s.ref0);
    out.push_back(s.ref1);
    out.push_back(s.full_compare ? 1u : 0u);
    out.push_back(s.gx_fog ? 1u : 0u);
    put_f32(out, s.gx_fog_start);
    put_f32(out, s.gx_fog_end);
    out.push_back(frame_open_ ? 1u : 0u);
    hud_.save_memory(out);
    if (regions)
        hud_.save_region_memory(out);
}

bool RwDriver::load_state(const u8*& cursor, const u8* end) {
    u8 format = 0;
    if (!get_u8(cursor, end, format) || format < kDriverStateFormatWithoutRegions || format > kDriverStateFormat)
        return false;
    RenderStates s;
    for (u32* v : {&s.z_write, &s.z_test, &s.cull, &s.fog_enable, &s.fog_type, &s.fog_colour}) {
        if (!get_u32(cursor, end, *v))
            return false;
    }
    for (float* v : {&s.fog_start, &s.fog_end, &s.fog_near, &s.fog_far}) {
        if (!get_f32(cursor, end, *v))
            return false;
    }
    for (u32* v : {&s.src_blend, &s.dst_blend, &s.alpha_test_off, &s.cmp0, &s.cmp1, &s.alpha_op}) {
        if (!get_u32(cursor, end, *v))
            return false;
    }
    u8 full = 0, fog = 0, open = 0;
    if (!get_u8(cursor, end, s.ref0) || !get_u8(cursor, end, s.ref1) || !get_u8(cursor, end, full) ||
        !get_u8(cursor, end, fog) || !get_f32(cursor, end, s.gx_fog_start) || !get_f32(cursor, end, s.gx_fog_end) ||
        !get_u8(cursor, end, open))
        return false;
    s.full_compare = full != 0u;
    s.gx_fog = fog != 0u;
    if (!hud_.load_memory(cursor, end))
        return false;
    if (format >= kDriverStateFormat) {
        if (!hud_.load_region_memory(cursor, end))
            return false;
    } else {
        hud_.clear_region_memory();  // a state from a moment without them
    }
    states_ = s;
    return true;
}

void RwDriver::own_words(std::vector<u32>& out) const {
    for (const Owned& o : textures_) {
        if (o.handle != 0u)
            out.push_back(o.word);
    }
    for (const Owned& o : meshes_) {
        if (o.handle != 0u)
            out.push_back(o.word);
    }
}

// Once per frame: a handle whose RenderWare word no longer holds it lost its owner (translated RenderWare destroyed
// the raster or freed the sector's data, and the memory may already hold something else): it is released.
void RwDriver::reconcile(Cpu& c) {
    const auto sweep = [&](std::vector<Owned>& owned, bool textures) {
        std::size_t kept = 0;
        for (Owned& o : owned) {
            if (o.handle != 0u && peek32(c, o.word, 0u) == o.handle) {
                owned[kept++] = o;
                continue;
            }
            if (o.handle != 0u) {
                if (textures) {
                    renderer_.release(render::TextureHandle{o.handle}, stats_.frames);
                    ++stats_.textures_released;
                } else {
                    renderer_.release(render::MeshHandle{o.handle}, stats_.frames);
                    ++stats_.meshes_released;
                }
            }
        }
        if (textures && kept != owned.size())
            texture_index_dirty_ = true;
        owned.resize(kept);
    };
    sweep(textures_, true);
    sweep(meshes_, false);
}

}  // namespace gekko::scaffold
