// GAME-LEVEL OWNERS (game/game_owners.h). Moved unchanged in behaviour out of the RenderWare GameCube driver scaffold
// (scaffold/rw_driver/projection.cpp and lens_flare.cpp, NE-RW1): the same reads, the same writes, the same RenderWare
// calls in the same order.
#include "game/game_owners.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gekko/cpu.h"

namespace gekko::game {

namespace effects = shadow::effects;
namespace render = shadow::render;

namespace {

GameOwners* g_installed = nullptr;

// ---- the game's objects these owners read (the game's own, and the RenderWare objects its code reads) -------------
constexpr u32 kRwGlobals = 0x805F265C;        // RwGlobals*
constexpr u32 kGlobalsNearScreenZ = 0x18;     // RwGlobals: the 2D draws' screen depth range (RwIm2DGetNearScreenZ)
constexpr u32 kGlobalsFarScreenZ = 0x1C;
constexpr u32 kCameraFrameBuffer = 0x60;      // RwCamera: its raster
constexpr u32 kCameraNear = 0x80;
constexpr u32 kCameraFar = 0x84;
constexpr u32 kCameraViewMatrix = 0x20;       // RwMatrix: right, up, at, pos rows 0x10 apart
constexpr u32 kRasterParent = 0x00;           // RwRaster: its top-level raster (0: itself)
constexpr u32 kRasterWidth = 0x0C;            // RwRaster
constexpr u32 kRasterHeight = 0x10;

// Weapon::LockonMark (docs/boundary-20261007/ORIGINAL.md 3)
constexpr u32 kCurrentPlayer = 0x805F1218;    // the player whose view is drawn (fn_8027211C)
constexpr u32 kCurrentViewport = 0x805F121C;  // ... and its viewport
constexpr u32 kMarkTarget = 0x14;
constexpr u32 kMarkAlpha = 0x20;
constexpr u32 kMarkRingScale = 0x24;
constexpr u32 kMarkInnerTexture = 0x28;
constexpr u32 kMarkRingTexture = 0x2C;
constexpr float kMarkHalfSize = 32.0f;        // [r2-0x76CC]: the quads' half size and the screen test's margin
constexpr float kMarkNearestDepth = 0.0f;     // [r2-0x76D0]
// RenderWare's render states, blend modes, filter and primitive type as the game's code passes them.
constexpr u32 kTextureRaster = 1, kZTestEnable = 6, kZWriteEnable = 8, kTextureFilter = 9, kSrcBlend = 10,
              kDestBlend = 11, kVertexAlphaEnable = 12, kFogEnable = 14;
constexpr u32 kBlendSrcAlpha = 5, kBlendOne = 2, kFilterNearest = 1;
constexpr u32 kTriangleStrip = 4;

// The lens flare (docs/boundary-20261007/FLARE.md): the task EffectDrawGlobalLensFlareTask (vf1 0x801D2928) builds the
// effect manager's flare (manager [0x805EF2D8] + 0x50) through fn_8043329C -> this builder, then draws it
// (fn_80433270 -> fn_80431E38: the glow and ghost quads Projected, the flash Fill), once per frame, with the camera of
// the installed effect system.
constexpr u32 kCurrentEffectSystem = 0x805E25C8;    // fn_8042B620: the installed Effect::System::EffectSystem
constexpr u32 kEffectSystemPjsVtable = 0x8053E93C;  // Effect::PJS::EffectSystemPJS (the builder's dynamic_cast target)
constexpr u32 kCameraHolder = 0x8056FF1C;           // +0: the RwCamera
constexpr u32 kFlareEnabled = 0x04;      // u8
constexpr u32 kFlareIntensity = 0x05;    // u8: the ghosts' and the flash's strength, 255 = full
constexpr u32 kFlareFlash = 0x06;        // u8: the flash's grey at full strength
constexpr u32 kFlareSun = 0x08;          // RwV3d, world
constexpr u32 kFlareElements = 0x18;     // the element table
constexpr u32 kFlareCount = 0x24;        // s32
constexpr u32 kFlareVertices = 0x28;     // RwIm2DVertex[4 x count]: element i's quad at 0x60 i
constexpr u32 kFlareFlashQuad = 0x2C;    // RwIm2DVertex[4]: the flash
constexpr u32 kFlareGlowHidden = 0x8C;   // u8
constexpr u32 kFlareGhostsHidden = 0x8D; // u8
constexpr u32 kElementStride = 0x24;
constexpr u32 kElementHeight = 0x04;     // s16, canvas units
constexpr u32 kElementWidth = 0x06;      // s16
constexpr u32 kElementPosition = 0x08;   // f32
constexpr u32 kElementColour = 0x1C;     // u8 x 4: the draw takes +0x1C, +0x1E, +0x1D and +0x1F (alpha) in that order
constexpr u32 kVertexStride = 0x18;      // RwIm2DVertex: x, y, z, RGBA, u, v
constexpr u32 kVertexZ = 0x08;
constexpr u32 kVertexColour = 0x0C;
constexpr u32 kQuadStride = 4 * kVertexStride;
constexpr s32 kMostElements = 64;        // the shipped table has 7

u32 as_bits(float f) {
    u32 b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

// fctiwz: truncation toward zero, saturating (the game's float -> int).
s32 truncate(float f) {
    if (!(f == f))
        return static_cast<s32>(0x80000000u);
    if (f >= 2147483647.0f)
        return 0x7FFFFFFF;
    if (f <= -2147483648.0f)
        return static_cast<s32>(0x80000000u);
    return static_cast<s32>(f);
}

GameOwners* owners(Cpu& c, const char* entry) {
    if (g_installed == nullptr)
        c.request_stop(StopReason::ExecutionFault,
                       std::string("game owner game::") + entry + " ran without the game owners installed");
    return g_installed;
}

}  // namespace

GameOwners::GameOwners(RwApi& rw) : rw_(rw) {
#if !defined(GEKKO_SHIPPING)
    // SHADOW_FLARE_TRACE=<n>: the first n lens flare builds - the sun from the raster's centre, the screen shown, the
    // decisions (docs/boundary-20261007/FLARE.md). SHADOW_FLARE_CONSOLE_SCREEN=1: the flare tests the camera raster,
    // as the console did (comparison baseline).
    if (const char* trace = std::getenv("SHADOW_FLARE_TRACE"))
        trace_flare_ = std::strtoull(trace, nullptr, 10);
    if (const char* console = std::getenv("SHADOW_FLARE_CONSOLE_SCREEN"))
        flare_console_screen_ = console[0] == '1';
#endif
}

void GameOwners::install() noexcept {
    g_installed = this;
}

void GameOwners::uninstall() noexcept {
    if (g_installed == this)
        g_installed = nullptr;
}

GameOwners* GameOwners::installed() noexcept {
    return g_installed;
}

float GameOwners::read_f32(Cpu& c, u32 address) {
    const u32 bits = c.mem_read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Weapon::LockonMark's draw (0x800620D4, ORIGINAL.md 3).
bool GameOwners::lockon_mark_draw(Cpu& c) {
    const u32 mark = c.gpr[3];
    ++stats_.lockon_marks;
    const u32 globals = c.mem_read32(kRwGlobals);
    const float near_z = read_f32(c, globals + kGlobalsNearScreenZ);
    const u32 player = c.mem_read32(kCurrentPlayer);
    const s32 viewport = static_cast<s32>(c.mem_read32(kCurrentViewport));
    RwApi::Projected p;
    if (!rw_.project(c, mark + kMarkTarget, player, &p)) {
        ++stats_.lockon_marks_hidden;
        return true;
    }
    const RwApi::ScreenRect rect = rw_.viewport(c, viewport);
    const float x = p.x - rect.x;
    const float y = p.y - rect.y;
    const render::CanvasRect shown = rw_.shown_screen(rect);
    const float left = shown.left - rect.x, right = shown.right - rect.x;
    const float top = shown.top - rect.y, bottom = shown.bottom - rect.y;
    if (p.depth < kMarkNearestDepth || x > kMarkHalfSize + right || x < left - kMarkHalfSize ||
        y > kMarkHalfSize + bottom || y < top - kMarkHalfSize) {
        ++stats_.lockon_marks_hidden;
        return true;
    }
    // The game's state changes and their restore (RwRenderStateGet / Set, as its lowered sites would make them).
    const RwApi::SavedStates saved = rw_.saved_states();
    rw_.state_set(c, kZTestEnable, 0u);
    rw_.state_set(c, kZWriteEnable, 0u);
    rw_.state_set(c, kFogEnable, 0u);
    rw_.state_set(c, kSrcBlend, kBlendSrcAlpha);
    rw_.state_set(c, kDestBlend, kBlendOne);
    rw_.state_set(c, kVertexAlphaEnable, 1u);
    rw_.state_set(c, kTextureFilter, kFilterNearest);
    // fn_800624D8: four vertices (x - s, y - s), (x - s, y + s), (x + s, y - s), (x + s, y + s), white at the mark's
    // alpha, u v the corners, z the near screen z.
    const u32 alpha = static_cast<u32>(truncate(255.0f * read_f32(c, mark + kMarkAlpha))) & 0xFFu;
    const auto quad = [&](u32 texture_field, float half) {
        const u32 texture = c.mem_read32(mark + texture_field);
        rw_.state_set(c, kTextureRaster, texture != 0u ? c.mem_read32(texture) : 0u);
        render::ImmediateVertex vertices[4];
        const float corner[4][4] = {{x - half, y - half, 0.0f, 0.0f},
                                    {x - half, y + half, 0.0f, 1.0f},
                                    {x + half, y - half, 1.0f, 0.0f},
                                    {x + half, y + half, 1.0f, 1.0f}};
        for (u32 i = 0; i < 4; ++i) {
            vertices[i].position = render::Float3{corner[i][0], corner[i][1], near_z};
            vertices[i].colour = 0x00FFFFFFu | (alpha << 24);  // white, R in the lowest byte
            vertices[i].uv[0] = corner[i][2];
            vertices[i].uv[1] = corner[i][3];
        }
        rw_.im2d_draw(c, kTriangleStrip, vertices, 4u, render::Anchor::Projected);
    };
    quad(kMarkInnerTexture, kMarkHalfSize);
    quad(kMarkRingTexture, kMarkHalfSize * read_f32(c, mark + kMarkRingScale));
    rw_.state_set(c, kZTestEnable, saved.z_test);
    rw_.state_set(c, kZWriteEnable, saved.z_write);
    rw_.state_set(c, kFogEnable, saved.fog_enable);
    rw_.state_set(c, kSrcBlend, saved.src_blend);
    rw_.state_set(c, kDestBlend, saved.dst_blend);
    ++stats_.lockon_marks_drawn;
    return true;
}

// The lens flare's builder (fn_80432360).
bool GameOwners::lens_flare_build(Cpu& c) {
    const u32 flare = c.gpr[3];
    ++stats_.flare_builds;
    c.mem_write8(flare + kFlareGlowHidden, 1u);
    c.mem_write8(flare + kFlareGhostsHidden, 1u);
    // The game's gates: an installed RenderWare effect system, the flare enabled, its vertices, a count.
    const u32 system = c.mem_read32(kCurrentEffectSystem);
    if (system == 0u)
        return true;
    if (c.mem_read32(system) != kEffectSystemPjsVtable) {
        ++stats_.flare_unknown_system;  // no other effect system ships; counted, not guessed at
        return true;
    }
    if (c.mem_read8(flare + kFlareEnabled) == 0u)
        return true;
    const u32 vertices = c.mem_read32(flare + kFlareVertices);
    const s32 count = static_cast<s32>(c.mem_read32(flare + kFlareCount));
    if (vertices == 0u || count < 0)
        return true;
    if (count > kMostElements) {
        ++stats_.flare_unknown_system;
        return true;
    }
    const u32 camera = c.mem_read32(kCameraHolder);
    const u32 raster = camera != 0u ? c.mem_read32(camera + kCameraFrameBuffer) : 0u;
    if (camera == 0u || raster == 0u)
        return true;

    // The sun through the camera's view matrix, as RwV3dTransformPoint (0x804834A8) gives it: pos + right x + up y +
    // at z, one fused single-precision step each. The camera carries the authored picture's window (the camera sync's
    // window step), so the result places the flare where the renderer draws the sun.
    const u32 m = camera + kCameraViewMatrix;
    const float p[3] = {read_f32(c, flare + kFlareSun), read_f32(c, flare + kFlareSun + 4u),
                        read_f32(c, flare + kFlareSun + 8u)};
    float sun[3];
    for (u32 i = 0; i < 3; ++i) {
        float r = std::fma(read_f32(c, m + 0x00u + 4u * i), p[0], read_f32(c, m + 0x30u + 4u * i));
        r = std::fma(read_f32(c, m + 0x10u + 4u * i), p[1], r);
        sun[i] = std::fma(read_f32(c, m + 0x20u + 4u * i), p[2], r);
    }

    // The view: the camera's raster, and what the output shows of it - the raster's share of its top-level raster (the
    // game's screen, 640 x 480 or 640 x 528) seen at the output's aspect, by the one rule the camera's window and its
    // frustum take (render/view.h raster_view; a split-screen viewport has its own share).
    effects::FlareView view;
    view.raster_width = static_cast<float>(static_cast<s32>(c.mem_read32(raster + kRasterWidth)));
    view.raster_height = static_cast<float>(static_cast<s32>(c.mem_read32(raster + kRasterHeight)));
    view.shown_width = view.raster_width;
    view.shown_height = view.raster_height;
    if (view.raster_width > 0.0f && view.raster_height > 0.0f && !flare_console_screen_) {
        u32 top = c.mem_read32(raster + kRasterParent);
        if (top == 0u)
            top = raster;
        const float top_width = static_cast<float>(static_cast<s32>(c.mem_read32(top + kRasterWidth)));
        const float top_height = static_cast<float>(static_cast<s32>(c.mem_read32(top + kRasterHeight)));
        const render::RasterView shares = render::raster_view(view.raster_width, view.raster_height, top_width,
                                                              top_height, rw_.presented_aspect());
        const render::CanvasRect shown = render::visible_canvas(view.raster_width, view.raster_height,
                                                                shares.canvas_aspect, shares.view_aspect);
        view.shown_width = shown.right - shown.left;
        view.shown_height = shown.bottom - shown.top;
    }
    view.near_plane = read_f32(c, camera + kCameraNear);
    view.far_plane = read_f32(c, camera + kCameraFar);
    const u32 globals = c.mem_read32(kRwGlobals);
    view.depth_near = read_f32(c, globals + kGlobalsNearScreenZ);
    view.depth_far = read_f32(c, globals + kGlobalsFarScreenZ);

    // The element table.
    const u32 table = c.mem_read32(flare + kFlareElements);
    flare_elements_.resize(static_cast<std::size_t>(count));
    for (s32 i = 0; i < count; ++i) {
        const u32 e = table + kElementStride * static_cast<u32>(i);
        effects::FlareElement& out = flare_elements_[static_cast<std::size_t>(i)];
        out.position = read_f32(c, e + kElementPosition);
        out.width = static_cast<float>(static_cast<s16>(c.mem_read16(e + kElementWidth)));
        out.height = static_cast<float>(static_cast<s16>(c.mem_read16(e + kElementHeight)));
        out.r = c.mem_read8(e + kElementColour + 0u);
        out.g = c.mem_read8(e + kElementColour + 2u);
        out.b = c.mem_read8(e + kElementColour + 1u);
        out.a = c.mem_read8(e + kElementColour + 3u);
    }
    flare_ghosts_.resize(count > 1 ? 4u * static_cast<std::size_t>(count - 1) : 0u);
    const effects::FlareFrame f = effects::layout_lens_flare(
        sun, view, flare_elements_.data(), count, c.mem_read8(flare + kFlareIntensity), c.mem_read8(flare + kFlareFlash),
        flare_ghosts_.data());
    if (!f.in_front)
        return true;
    ++stats_.flare_in_front;

    const auto put_colour = [&c](u32 vertex, const effects::FlareVertex& q) {
        c.mem_write8(vertex + kVertexColour + 0u, q.r);
        c.mem_write8(vertex + kVertexColour + 1u, q.g);
        c.mem_write8(vertex + kVertexColour + 2u, q.b);
        c.mem_write8(vertex + kVertexColour + 3u, q.a);
    };
    if (f.ghosts) {
        c.mem_write8(flare + kFlareGhostsHidden, 0u);
        ++stats_.flare_ghosts_shown;
        for (s32 i = 1; i < count; ++i) {
            for (u32 k = 0; k < 4; ++k) {
                const effects::FlareVertex& q = flare_ghosts_[4u * static_cast<std::size_t>(i - 1) + k];
                const u32 vertex = vertices + kQuadStride * static_cast<u32>(i) + kVertexStride * k;
                c.mem_write32(vertex + 0u, as_bits(q.x));
                c.mem_write32(vertex + 4u, as_bits(q.y));
                put_colour(vertex, q);
            }
        }
        for (u32 k = 0; k < 4; ++k) {
            const u32 vertex = flare + kFlareFlashQuad + kVertexStride * k;
            c.mem_write8(vertex + kVertexColour + 0u, f.flash_level);
            c.mem_write8(vertex + kVertexColour + 1u, f.flash_level);
            c.mem_write8(vertex + kVertexColour + 2u, f.flash_level);
            c.mem_write8(vertex + kVertexColour + 3u, 0xFFu);
        }
    }
    if (f.glow && count > 0) {
        c.mem_write8(flare + kFlareGlowHidden, 0u);
        ++stats_.flare_glow_shown;
        for (u32 k = 0; k < 4; ++k) {
            const u32 vertex = vertices + kVertexStride * k;
            c.mem_write32(vertex + 0u, as_bits(f.glow_quad[k].x));
            c.mem_write32(vertex + 4u, as_bits(f.glow_quad[k].y));
            c.mem_write32(vertex + kVertexZ, as_bits(f.glow_depth));
            put_colour(vertex, f.glow_quad[k]);
        }
    }
    // The flash covers the raster: its corners (0, 0), (0, H), (W, 0), (W, H).
    const float corner_x[4] = {0.0f, 0.0f, view.raster_width, view.raster_width};
    const float corner_y[4] = {0.0f, view.raster_height, 0.0f, view.raster_height};
    for (u32 k = 0; k < 4; ++k) {
        const u32 vertex = flare + kFlareFlashQuad + kVertexStride * k;
        c.mem_write32(vertex + 0u, as_bits(corner_x[k]));
        c.mem_write32(vertex + 4u, as_bits(corner_y[k]));
    }

    // Shown where the console's raster test would have hidden them: the sun past the raster's own square.
    const float sx = view.raster_width * (sun[0] / sun[2]) - 0.5f * view.raster_width;
    const float sy = view.raster_height * (sun[1] / sun[2]) - 0.5f * view.raster_height;
    const float raster_radius = 0.5f * (view.raster_width > view.raster_height ? view.raster_width : view.raster_height);
    if (f.ghosts && (sx > raster_radius || sx < -raster_radius || sy > raster_radius || sy < -raster_radius))
        ++stats_.flare_beyond_raster;
    if (trace_flare_ > 0) {
        --trace_flare_;
        std::fprintf(stderr,
                     "[flare] frame %llu sun (%.2f %.2f) from the raster centre; raster %.0fx%.0f shown %.2fx%.2f "
                     "(output aspect %.6f); ghosts %d fade %.4f flash %u; glow %d depth %.6f\n",
                     static_cast<unsigned long long>(rw_.frame_number()), sx, sy, view.raster_width,
                     view.raster_height, view.shown_width, view.shown_height, rw_.presented_aspect(), f.ghosts ? 1 : 0,
                     f.fade, static_cast<unsigned>(f.flash_level), f.glow ? 1 : 0, f.glow_depth);
    }
    return true;
}

bool lockon_mark_draw(Cpu& c) {
    GameOwners* o = owners(c, "lockon_mark_draw");
    return o == nullptr || o->lockon_mark_draw(c);
}

bool lens_flare_build(Cpu& c) {
    GameOwners* o = owners(c, "lens_flare_build");
    return o == nullptr || o->lens_flare_build(c);
}

}  // namespace gekko::game
