// SCAFFOLDING (R2): the renderer camera's window (the camera sync's lowered window step), the game's world-to-screen projection for the 2D it attaches to world points (the lowered calls of fn_8000F654), those draws' Projected anchor, their screen tests on the displayed screen (Weapon::LockonMark's draw moved to runtime/game, NE-RW1) - deleted by R4: the camera lift and the HUD consumers' lift take render/view.h directly
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gekko/cpu.h"
#include "rw_driver.h"

namespace gekko::scaffold {

namespace render = shadow::render;

// docs/boundary-20261007/ORIGINAL.md: the game's camera system as fn_8000F654 and the camera sync fn_80271F9C read it.
namespace {

constexpr u32 kCameraManager = 0x8056FED4;  // +0x08 count, +0x0C data: data[player] = {camera object, reference}
constexpr u32 kCameraHolder = 0x8056FF1C;   // +0x00 RwCamera*, +0x20 the screen layout mode
constexpr u32 kHolderLayoutMode = 0x20;
constexpr u32 kViewportTable = 0x8058516C;  // stride 0x30: +0x08 rect {x, y, w, h} (s32, 640 x 480 canvas)
constexpr u32 kViewportStride = 0x30;
constexpr u32 kViewportRect = 0x08;
constexpr u32 kViewportScale = 0x28;        // the FOV scale pair (f32 x, y)
constexpr u32 kLayoutMap = 0x8054DDC8;      // [[map + mode * 4] + player * 4]: the player's viewport (-1: none)
constexpr u32 kScaleGlobals = 0x805F1238;   // the FOV scale's global factors (f32 x, y)
constexpr u32 kCurrentPlayer = 0x805F1218;  // the player whose view is drawn (fn_8027211C)
constexpr u32 kCurrentViewport = 0x805F121C;  // ... and its viewport
constexpr float kSplitScale = 1.25f;        // fn_802731A4: every viewport but the whole screen's
constexpr u32 kCameraFov = 0x24;            // camera object: the FOV pair (radians)
constexpr u32 kCameraView = 0x3C;           // camera object: world -> view, 3 rows of 4 (right-handed, -z forward)
constexpr float kAuthoredScreenAspect = 4.0f / 3.0f;  // the whole screen's canvas (640 x 480) is seen 4:3
constexpr u32 kGlobalsNearScreenZ = 0x18;   // RwGlobals: dOpenDevice.zBufferNear (RwIm2DGetNearScreenZ)

// Weapon::LockonMark (type_info 0x805E4DA8; its draw 0x800620D4, ORIGINAL.md 3): the target, the alpha, the ring's
// scale, the inner and the ring textures (RwTexture*: +0 the raster); its constants.
constexpr u32 kMarkTarget = 0x14;
constexpr u32 kMarkAlpha = 0x20;
constexpr u32 kMarkRingScale = 0x24;
constexpr u32 kMarkInnerTexture = 0x28;
constexpr u32 kMarkRingTexture = 0x2C;
constexpr float kMarkHalfSize = 32.0f;      // [r2-0x76CC]: the quads' half size and the screen test's margin
constexpr float kMarkNearestDepth = 0.0f;   // [r2-0x76D0]

// RenderWare render states the mark sets (RwRenderState).
constexpr u32 kTextureRaster = 1, kZTestEnable = 6, kZWriteEnable = 8, kTextureFilter = 9, kSrcBlend = 10,
              kDestBlend = 11, kVertexAlphaEnable = 12, kFogEnable = 14;
constexpr u32 kBlendSrcAlpha = 5, kBlendOne = 2, kFilterNearest = 1;
constexpr u32 kTriangleStrip = 4;

// The world-following Chao::CSD widgets, by the return address of their Project::Render call (System's group id):
// their sprites sit where the game projected a world point, so they are placed Projected.
constexpr u32 kWorldAnchoredWidgets[] = {
    0x800BF770u,  // GiHPGaugeEnemyImpl: the enemy health bar (projected by its gate 0x800BF9D8)
    0x8033BC4Cu,  // VehicleArrowImpl::Render (fn_8033BB10): the hint arrow, its scenes placed by the 3D-anchored
                  // CSD helpers 0x8033BC6C / 0x8033BE80 (projected)
};

float as_float(u32 bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

u32 as_bits(float f) {
    u32 b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

// tan through the game's wrapper fn_8000F3D4: double tan, rounded to float.
float game_tan(float half_angle) {
    return static_cast<float>(std::tan(static_cast<double>(half_angle)));
}

}  // namespace

// ---- game::RwApi (NE-RW1): the game-level owners' view of RenderWare, served by this layer until the lifts ----------
game::RwApi::SavedStates RwDriver::saved_states() const {
    return {states_.z_test, states_.z_write, states_.fog_enable, states_.src_blend, states_.dst_blend};
}

void RwDriver::state_set(Cpu& c, u32 state, u32 value) {
    render_state_set(c, state, value);
}

void RwDriver::im2d_draw(Cpu& c, u32 type, const render::ImmediateVertex* vertices, u32 count, render::Anchor anchor) {
    im2d(c, type, 0u, count, 0u, 0u, &anchor, vertices);
}

bool RwDriver::project(Cpu& c, u32 world, u32 player, Projected* out) {
    ProjectedPoint p;
    if (!project_point(c, world, player, &p))
        return false;
    out->x = p.x;
    out->y = p.y;
    out->depth = p.depth;
    return true;
}

game::RwApi::ScreenRect RwDriver::viewport(Cpu& c, s32 index) {
    const ScreenRect r = viewport_rect(c, index);
    return {r.x, r.y, r.w, r.h};
}

render::CanvasRect RwDriver::shown_screen(const game::RwApi::ScreenRect& r) const {
    return visible_screen(ScreenRect{r.x, r.y, r.w, r.h});
}

bool RwDriver::is_world_anchored_widget(u32 group_id) noexcept {
    for (const u32 site : kWorldAnchoredWidgets) {
        if (site == group_id)
            return true;
    }
    return false;
}

void RwDriver::set_output_aspect(float aspect) noexcept {
    if (aspect > 0.0f && std::isfinite(aspect))
        output_aspect_bits_.store(as_bits(aspect), std::memory_order_release);
}

float RwDriver::output_aspect() const noexcept {
    if (event_framed_.load(std::memory_order_acquire))
        return kAuthoredScreenAspect;  // an in-engine event is shown 4:3 (sample_event_framing)
    return as_float(output_aspect_bits_.load(std::memory_order_acquire));
}

void RwDriver::sample_event_framing(Cpu& c) noexcept {
    const bool event = c.mem_read8(c.gpr[13] + layout::kSdaEventHoldsInterval) != 0u &&
                       c.mem_read8(layout::kSystemObject + layout::kSystemTwoPlayers) == 0u;
#if !defined(GEKKO_SHIPPING)
    if (event != event_framed_.load(std::memory_order_relaxed))
        std::fprintf(stderr, "[lifted-render] in-engine event %s at frame %llu\n", event ? "begins" : "ends",
                     static_cast<unsigned long long>(stats_.frames));
#endif
    event_framed_.store(event, std::memory_order_release);
}

// A viewport's rectangle of the game's 640 x 480 screen (the viewport table), and what the output shows of it: the
// visible canvas (render/view.h) of a canvas w x h seen at its own aspect (square units) on the view's share of the
// output - the screen the game's "is it on the screen" tests mean.
RwDriver::ScreenRect RwDriver::viewport_rect(Cpu& c, s32 viewport) {
    ScreenRect r;
    if (viewport >= 0) {
        const u32 entry = kViewportTable + kViewportStride * static_cast<u32>(viewport) + kViewportRect;
        r.x = static_cast<float>(static_cast<s32>(c.mem_read32(entry)));
        r.y = static_cast<float>(static_cast<s32>(c.mem_read32(entry + 4u)));
        r.w = static_cast<float>(static_cast<s32>(c.mem_read32(entry + 8u)));
        r.h = static_cast<float>(static_cast<s32>(c.mem_read32(entry + 12u)));
    }
    return r;
}

render::CanvasRect RwDriver::visible_screen(const ScreenRect& r) const {
    if (!(r.w > 0.0f && r.h > 0.0f))
        return render::CanvasRect{r.x, r.y, r.x + r.w, r.y + r.h};
    const float canvas_aspect = r.w / r.h;
    // The view's share of the output: (w / 640) of its width, (h / 480) of its height.
    const float view_aspect = output_aspect() * canvas_aspect / kAuthoredScreenAspect;
    const render::CanvasRect v = render::visible_canvas(r.w, r.h, canvas_aspect, view_aspect);
    return render::CanvasRect{r.x + v.left, r.y + v.top, r.x + v.right, r.y + v.bottom};
}

void RwDriver::viewport_fov_scale(Cpu& c, s32 viewport, float* scale_x, float* scale_y) {
    *scale_x = 1.0f;
    *scale_y = 1.0f;
    if (viewport < 0)
        return;
    const u32 entry = kViewportTable + kViewportStride * static_cast<u32>(viewport);
    *scale_x = as_float(c.mem_read32(entry + kViewportScale)) * as_float(c.mem_read32(kScaleGlobals));
    *scale_y = as_float(c.mem_read32(entry + kViewportScale + 4u)) * as_float(c.mem_read32(kScaleGlobals + 4u));
    if (viewport != 0) {
        *scale_x *= kSplitScale;
        *scale_y *= kSplitScale;
    }
}

// fn_8000F57C(out, camera state, world, viewport) at the Black Arms' shared on-screen test 0x8019DA1C, what it meant: is
// the world point on the screen. The game answered in its camera's own window - x / (-z tan(fov.x / 2)) / scale.x,
// y / (z tan(fov.y / 2)) / scale.y, then z (view space: x right, y up, z backward) - and tested |x|, |y| <= 1 and
// z <= 0: the 4:3 picture's edges, whatever the output shows. Here the same coordinates are measured in the DISPLAYED
// picture of that viewport (render/view.h displayed_window, the picture the renderer draws and the frustum culls), so
// +-1 is the edge of the screen the player sees on any aspect. With --framing game at 4:3 the displayed window is the
// game's own (tan of the half FOV, scaled) and the test is the original's. The camera state is the camera object's
// +0x08 (fn_80009370); its world -> view rows are the object's +0x3C, the matrix fn_8000F654 projected with.
u32 RwDriver::displayed_screen_point(Cpu& c, u32 out, u32 camera_state, u32 world, u32 viewport) {
    ++stats_.displayed_screen_points;
    const u32 object = camera_state - 8u;
    const s32 port = static_cast<s32>(viewport);
    float scale_x = 1.0f, scale_y = 1.0f;
    viewport_fov_scale(c, port, &scale_x, &scale_y);
    const ScreenRect rect = viewport_rect(c, port);
    const float fov_x = as_float(c.mem_read32(object + kCameraFov));
    const float fov_y = as_float(c.mem_read32(object + kCameraFov + 4u));
    const render::ViewWindow game{game_tan(0.5f * (fov_x * scale_x)), game_tan(0.5f * (fov_y * scale_y))};
    const float canvas_aspect = rect.h > 0.0f ? rect.w / rect.h : kAuthoredScreenAspect;
    const float view_aspect = output_aspect() * canvas_aspect / kAuthoredScreenAspect;
    const render::ViewWindow shown = render::displayed_window(game, canvas_aspect, view_aspect, framing_);
    float p[3];
    for (u32 i = 0; i < 3; ++i)
        p[i] = as_float(c.mem_read32(world + 4u * i));
    float v[3];
    for (u32 r = 0; r < 3; ++r) {
        const u32 m = object + kCameraView + 16u * r;
        v[r] = as_float(c.mem_read32(m)) * p[0] + as_float(c.mem_read32(m + 4u)) * p[1] +
               as_float(c.mem_read32(m + 8u)) * p[2] + as_float(c.mem_read32(m + 12u));
    }
    const float x = v[0] / -(v[2] * shown.x);
    const float y = v[1] / (v[2] * shown.y);
    // Evidence (docs/ASPECT_INVENTORY.md 6): the verdict the game's own window gave - its formula, tan(fov / 2) /
    // scale - against the displayed picture's, counted where they differ.
    const float game_x = v[0] / -(v[2] * game_tan(0.5f * fov_x)) / scale_x;
    const float game_y = v[1] / (v[2] * game_tan(0.5f * fov_y)) / scale_y;
    c.mem_write32(out, as_bits(game_window_screen_test_ ? game_x : x));
    c.mem_write32(out + 4u, as_bits(game_window_screen_test_ ? game_y : y));
    c.mem_write32(out + 8u, as_bits(v[2]));
    const bool in_front = v[2] <= 0.0f;
    const bool on_game = in_front && std::fabs(game_x) <= 1.0f && std::fabs(game_y) <= 1.0f;
    const bool on_shown = in_front && std::fabs(x) <= 1.0f && std::fabs(y) <= 1.0f;
    if (on_shown)
        ++stats_.displayed_screen_points_on;
    if (on_game != on_shown)
        ++(on_shown ? stats_.displayed_screen_points_gained : stats_.displayed_screen_points_lost);
    return out;
}

// fn_8000F654(out, world, player), what it meant: the screen position of a world point on `player`'s view - the
// canvas point the view's picture shows it at - and its forward distance. The game computed it with its own camera's
// horizontal extent and a tangent approximation, truncated to whole canvas units (ORIGINAL.md 3); here the point is
// put on the canvas by the boundary (render/view.h canvas_point) from the authored picture the renderer draws, so the
// 2D lands on the pixel the world point is drawn at, on any output.
bool RwDriver::project_point(Cpu& c, u32 world, u32 player, ProjectedPoint* out) {
    ++stats_.projections;
    // The player's camera object (fn_8000D0F0): the manager's vector of {object, reference}.
    const u32 count = c.mem_read32(kCameraManager + 0x08u);
    const u32 data = c.mem_read32(kCameraManager + 0x0Cu);
    const u32 object = player < count && data != 0u ? c.mem_read32(data + 8u * player) : 0u;
    if (object == 0u) {
        ++stats_.projections_without_camera;
        return false;
    }
    // The player's viewport (fn_80272180 -> fn_802732B4) and its FOV scale (fn_802731A4).
    const u32 mode = c.mem_read32(kCameraHolder + kHolderLayoutMode);
    const u32 row = c.mem_read32(kLayoutMap + 4u * mode);
    const s32 viewport = row != 0u ? static_cast<s32>(c.mem_read32(row + 4u * player)) : 0;
    float scale_x = 1.0f, scale_y = 1.0f;
    ScreenRect rect = viewport_rect(c, viewport);
    viewport_fov_scale(c, viewport, &scale_x, &scale_y);
    // The game's view window, step for step as the camera sync gives it to the renderer's camera (fn_80271F9C,
    // fn_8000F860): the scaled FOV, halved, its tangent - the same bits.
    const float fov_x = as_float(c.mem_read32(object + kCameraFov));
    const float fov_y = as_float(c.mem_read32(object + kCameraFov + 4u));
    const render::ViewWindow game{game_tan(0.5f * (fov_x * scale_x)), game_tan(0.5f * (fov_y * scale_y))};
    // The canvas's units are square (640 x 480 seen 4:3), so a viewport's authored aspect is its own.
    const float canvas_aspect = rect.h > 0.0f ? rect.w / rect.h : kAuthoredScreenAspect;
    const render::ViewWindow authored = render::authored_window(game, canvas_aspect, framing_);
    // The point in view space: the camera's rows give x right, y up and z backward.
    double p[3];
    for (u32 i = 0; i < 3; ++i)
        p[i] = static_cast<double>(as_float(c.mem_read32(world + 4u * i)));
    double v[3];
    for (u32 r = 0; r < 3; ++r) {
        const u32 m = object + kCameraView + 16u * r;
        v[r] = static_cast<double>(as_float(c.mem_read32(m))) * p[0] +
               static_cast<double>(as_float(c.mem_read32(m + 4u))) * p[1] +
               static_cast<double>(as_float(c.mem_read32(m + 8u))) * p[2] +
               static_cast<double>(as_float(c.mem_read32(m + 12u)));
    }
    const render::CanvasPoint point = render::canvas_point(v[0], v[1], -v[2], authored, rect.w, rect.h);
    out->x = rect.x + point.u;
    out->y = rect.y + point.v;
    out->depth = point.depth;
    out->viewport = viewport;
    // A point on the eye's plane has no place: the game's truncation turned it into the least integer, which every
    // consumer rejects; keep that answer rather than hand a consumer a NaN its comparisons would accept.
    if (!std::isfinite(out->x) || !std::isfinite(out->y)) {
        ++stats_.projections_degenerate;
        out->x = out->y = -2147483648.0f;
    }
    // The renderer's camera for the same view is synchronised from the same object and given the authored window of
    // the same FOV (camera_picture_window); a bracket on this viewport whose camera window differs means the two are
    // not the same camera - or a savestate from before the camera carried the authored window, until its FOV changes
    // (counted).
    if (bracket_valid_ && bracket_view_ < views_.size()) {
        const View& view = views_[bracket_view_];
        if (view.x == rect.x && view.y == rect.y && view.width == rect.w && view.height == rect.h &&
            (as_bits(bracket_camera_.view_window_x) != as_bits(authored.x) ||
             as_bits(bracket_camera_.view_window_y) != as_bits(authored.y)))
            ++stats_.projection_window_mismatches;
    }
    if (stats_.frames == trace_frame_ || trace_projections_ > 0) {
        if (trace_projections_ > 0)
            --trace_projections_;
        std::fprintf(stderr,
                     "[boundary] frame %llu lr %08X player %u viewport %d rect (%.0f %.0f %.0f %.0f) world (%.4f %.4f "
                     "%.4f) view (%.4f %.4f %.4f) game window (%.7f %.7f) authored (%.7f %.7f) ndc (%.6f %.6f) "
                     "screen (%.3f %.3f) depth %.4f\n",
                     static_cast<unsigned long long>(stats_.frames), c.lr, player, viewport, rect.x, rect.y, rect.w,
                     rect.h, p[0], p[1], p[2], v[0], v[1], -v[2], game.x, game.y, authored.x, authored.y,
                     v[0] / (-v[2] * authored.x), v[1] / (-v[2] * authored.y), out->x, out->y, out->depth);
    }
    return true;
}

// The lowered fn_8000F654 call: the projection in the game's form - x, y in its 640 x 480 canvas with the viewport's
// origin, then the depth.
u32 RwDriver::world_to_screen(Cpu& c, u32 out, u32 world, u32 player) {
    ProjectedPoint p;
    if (!project_point(c, world, player, &p)) {
        for (u32 i = 0; i < 3; ++i)
            c.mem_write32(out + 4u * i, 0u);
        return out;
    }
    c.mem_write32(out, as_bits(p.x));
    c.mem_write32(out + 4u, as_bits(p.y));
    c.mem_write32(out + 8u, as_bits(p.depth));
    return out;
}

// The screen tests of the 2D the game attaches to world points (the enemy gauge's gate 0x800BF9D8, the 3D-anchored CSD
// helpers 0x8033BC6C / 0x8033BE80): each read one edge of its viewport through an accessor (x + w, x, y + h, y). They
// mean "is the point on the screen", so they get the screen the output shows (visible_screen), widened outward to
// whole canvas units (an edge between two units admits the unit it touches); the rectangle's own words, which place
// the 2D, are untouched.
u32 RwDriver::visible_screen_edge(Cpu& c, u32 rect_address, ScreenEdge edge) {
    ++stats_.screen_edge_reads;
    ScreenRect r;
    r.x = static_cast<float>(static_cast<s32>(c.mem_read32(rect_address)));
    r.y = static_cast<float>(static_cast<s32>(c.mem_read32(rect_address + 4u)));
    r.w = static_cast<float>(static_cast<s32>(c.mem_read32(rect_address + 8u)));
    r.h = static_cast<float>(static_cast<s32>(c.mem_read32(rect_address + 12u)));
    const render::CanvasRect v = visible_screen(r);
    switch (edge) {
    case ScreenEdge::Right: return static_cast<u32>(static_cast<s32>(std::ceil(v.right)));
    case ScreenEdge::Left: return static_cast<u32>(static_cast<s32>(std::floor(v.left)));
    case ScreenEdge::Bottom: return static_cast<u32>(static_cast<s32>(std::ceil(v.bottom)));
    case ScreenEdge::Top: return static_cast<u32>(static_cast<s32>(std::floor(v.top)));
    }
    return 0u;
}

u32 RwDriver::im2d_primitive_projected(Cpu& c, u32 type, u32 vertices, u32 count) {
    const render::Anchor projected = render::Anchor::Projected;
    return im2d(c, type, vertices, count, 0u, 0u, &projected);
}

// The renderer camera's window. The camera sync (fn_80271F9C) gives RenderWare's camera the window of the game
// camera's FOV - fn_8000F860: the tangents of half the viewport-scaled FOV pair - whenever that FOV changes. On the
// console that window WAS the picture. Here the picture is the authored window (render/view.h): square pixels by
// default, the game's own window with --framing game. The lowered fn_8000F860 call gives the camera that window, so
// everything that maps the renderer's camera onto the canvas - RenderWare's view matrix, the lens flare, the
// refraction patches, the rain and watch-camera sheets - reads the picture that is drawn (docs/boundary-20261007/
// DESIGN.md 8); the camera's frustum is the displayed picture's (System::native_frustum_build, DESIGN.md 7).
// The tangents are fn_8000F860's (0.5 x the angle through the game's tanf: bit-identical); the canvas is the camera's
// raster (its viewport's, attached by fn_8027211C before the sync).
u32 RwDriver::camera_picture_window(Cpu& c, u32 out, u32 fov) {
    ++stats_.camera_windows;
    const render::ViewWindow game{game_tan(0.5f * read_f32(c, fov)), game_tan(0.5f * read_f32(c, fov + 4u))};
    // The canvas: the camera's raster as the renderer shows it (render/view.h raster_view - the rule the picture it is
    // drawn and culled with takes too, System::canonical_camera).
    float w = 0.0f, h = 0.0f, top_w = 0.0f, top_h = 0.0f;
    const u32 camera = c.mem_read32(kCameraHolder);
    const u32 raster = camera != 0u ? c.mem_read32(camera + layout::kCameraFrameBuffer) : 0u;
    if (raster != 0u) {
        u32 top = c.mem_read32(raster + layout::kRasterParent);
        if (top == 0u)
            top = raster;
        w = static_cast<float>(c.mem_read32(raster + layout::kRasterWidth));
        h = static_cast<float>(c.mem_read32(raster + layout::kRasterHeight));
        top_w = static_cast<float>(c.mem_read32(top + layout::kRasterWidth));
        top_h = static_cast<float>(c.mem_read32(top + layout::kRasterHeight));
    }
    const render::RasterView view = render::raster_view(w, h, top_w, top_h, output_aspect());
    // The camera's frustum - its visibility volume - is not this window's: System::native_frustum_build builds it from
    // the displayed picture (docs/boundary-20261007/DESIGN.md 7).
    const render::ViewWindow authored = render::authored_window(game, view.canvas_aspect, framing_);
    c.mem_write32(out, as_bits(authored.x));
    c.mem_write32(out + 4u, as_bits(authored.y));
    return out;
}

// ---- the generated sites' functions -----------------------------------------------------------------------------

namespace {

RwDriver* boundary_driver(Cpu& c, const char* entry) {
    RwDriver* d = RwDriver::installed();
    if (d == nullptr)
        c.request_stop(StopReason::ExecutionFault, std::string("lowered boundary site (scaffold::") + entry +
                                                       ") ran without the R2 scaffolding installed");
    return d;
}

}  // namespace

u32 world_to_screen(Cpu& c, u32 out, u32 world, u32 player) {
    RwDriver* d = boundary_driver(c, "world_to_screen");
    return d != nullptr ? d->world_to_screen(c, out, world, player) : out;
}

u32 rw_im2d_render_primitive_projected(Cpu& c, u32 type, u32 vertices, u32 count) {
    RwDriver* d = boundary_driver(c, "rw_im2d_render_primitive_projected");
    return d != nullptr ? d->im2d_primitive_projected(c, type, vertices, count) : 0u;
}

u32 camera_picture_window(Cpu& c, u32 out, u32 fov) {
    RwDriver* d = boundary_driver(c, "camera_picture_window");
    return d != nullptr ? d->camera_picture_window(c, out, fov) : out;
}

u32 displayed_screen_point(Cpu& c, u32 out, u32 camera_state, u32 world, u32 viewport) {
    RwDriver* d = boundary_driver(c, "displayed_screen_point");
    return d != nullptr ? d->displayed_screen_point(c, out, camera_state, world, viewport) : out;
}

u32 visible_screen_right(Cpu& c, u32 rect) {
    RwDriver* d = boundary_driver(c, "visible_screen_right");
    return d != nullptr ? d->visible_screen_edge(c, rect, RwDriver::ScreenEdge::Right) : 0u;
}

u32 visible_screen_left(Cpu& c, u32 rect) {
    RwDriver* d = boundary_driver(c, "visible_screen_left");
    return d != nullptr ? d->visible_screen_edge(c, rect, RwDriver::ScreenEdge::Left) : 0u;
}

u32 visible_screen_bottom(Cpu& c, u32 rect) {
    RwDriver* d = boundary_driver(c, "visible_screen_bottom");
    return d != nullptr ? d->visible_screen_edge(c, rect, RwDriver::ScreenEdge::Bottom) : 0u;
}

u32 visible_screen_top(Cpu& c, u32 rect) {
    RwDriver* d = boundary_driver(c, "visible_screen_top");
    return d != nullptr ? d->visible_screen_edge(c, rect, RwDriver::ScreenEdge::Top) : 0u;
}

}  // namespace gekko::scaffold
