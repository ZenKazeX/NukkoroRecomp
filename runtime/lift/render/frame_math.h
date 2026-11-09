#pragma once

// The arithmetic every renderer backend applies to a FramePacket, and the constant blocks it hands the shaders
// (render/shaders/*.hlsl) - ONE definition, so the D3D12 renderer and the Vulkan renderer (render_vk, the Android port)
// feed the same shaders the same numbers (docs/VULKAN_RENDERER.md 8, hook H2). Plain values, no graphics API.
//
// H2 status (2026-10-26): render_vk uses this header; render/renderer.cpp still has its own copy of these definitions in
// its anonymous namespace (taken from it verbatim) - the D3D12 owner deletes that copy and includes this file.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "render/frame.h"
#include "render/view.h"

namespace shadow::render {

// ---- constant blocks (layouts shared with the HLSL; D3D packing, -fvk-use-dx-layout on Vulkan) --------------------

// cbuffer Draw (b2) in shaders/mesh.hlsl; one 256-byte block per draw.
struct DrawConstants {
    float world_view_projection[16];  // column-major: the HLSL default packing
    float world_row[3][4];
    float view_row[3][4];
    float colour[4];
    float uv_row[2][4];
    std::uint32_t texture;
    std::uint32_t sampler;
    std::uint32_t flags;
    std::uint32_t uv_set;
    float cutoff;
    float ka;
    float coefficient;
    float fog_start;
    float fog_end;
    std::uint32_t fog_colour;
    std::uint32_t fog_enabled;
    float dissolve;  // the fade-in (gekko/appear_fade.h): share of pixels the ordered dither hides (0 = all drawn)
};
static_assert(sizeof(DrawConstants) == 256, "one draw block");

// cbuffer Particles (b2) in shaders/particles.hlsl; one 256-byte block per batch.
struct ParticleConstants {
    float view_projection[16];  // column-major
    float right[4];
    float up[4];
    float colour[4];
    std::uint32_t texture;
    std::uint32_t sampler;
    std::uint32_t flags;
    std::uint32_t fog_colour;
    float cutoff;
    float fog_start;
    float fog_end;
    std::uint32_t fog_enabled;
};
static_assert(sizeof(ParticleConstants) <= 256, "one batch block");
inline constexpr std::uint32_t kParticleColours = 1;

// A fragment whose final alpha is exactly 0 is not drawn (every draw shader; particle, mesh and immediate flags). Only
// set when such a fragment cannot change the target: no depth write, and a blend whose result at source alpha 0 is the
// destination bit for bit (clear_fragment_keeps_target). The scene's alpha channel is ZERO x src + ONE x dst, always.
// A measured fill saving, not a picture change: GPU_FILL.md.
inline constexpr std::uint32_t kSkipClear = 64;
// Diagnostics (SHADOW_R2_OPAQUE_CHECK=1): a draw on the exact opaque write outputs magenta where its alpha is not 1.
inline constexpr std::uint32_t kOpaqueCheck = 128;
// Every vertex and texel alpha of the draw is 255: the fragment alpha is the material constant's (shaders/mesh.hlsl).
inline constexpr std::uint32_t kExactAlpha = 1024;
// MatFX BUMPENVMAP (0x804A4E00): the base pass is the GameCube's four-stage bump + environment combine
// (shaders/mesh.hlsl PSBase). The block's spare view_row .w lanes carry the bump scale and the environment map.
inline constexpr std::uint32_t kBumpEnvironment = 2048;
inline constexpr std::uint32_t kBumpFramebufferAlpha = 4096;  // the environment slot's useFrameBufferAlpha

// cbuffer Immediate (b2) in shaders/immediate.hlsl; one 256-byte block per draw.
struct ImmediateConstants {
    float world_view_projection[16];  // column-major
    std::uint32_t texture;
    std::uint32_t sampler;
    std::uint32_t fog_colour;
    std::uint32_t fog_enabled;
    float cutoff;
    float fog_start;
    float fog_end;
    std::uint32_t flags;  // kSkipClear
    float uv_row[2][4];  // u' = r0 . (u, v, 1), v' = r1 . (u, v, 1)
};
static_assert(sizeof(ImmediateConstants) <= 256, "one immediate block");

// cbuffer Lights (b1) in shaders/mesh.hlsl.
struct LightConstants {
    float ambient[4];
    std::uint32_t counts[4];
    float directional_colour[4][4];
    float directional_vector[4][4];
    float point_position[8][4];
    float point_colour[8][4];
};

// Mesh vertex streams (shaders/mesh.hlsl MeshVertex). Stream 0: what skinning replaces. Stream 1: what it does not.
struct PositionVertex {
    float position[3];
    float normal[3];
};
static_assert(sizeof(PositionVertex) == 24, "PositionVertex layout");
struct AttributeVertex {
    std::uint32_t colour;
    float uv0[2];
    float uv1[2];
};
static_assert(sizeof(AttributeVertex) == 20, "AttributeVertex layout");

// Material switches (shaders/mesh.hlsl).
inline constexpr std::uint32_t kPrelit = 1, kModulate = 2, kLit = 4, kAmbient = 8, kCombined = 16, kUnbaked = 32;

inline constexpr std::uint32_t kNoShaderResource = 0xFFFFFFFFu;  // a block's "no texture"

// ---- matrices -------------------------------------------------------------------------------------------------------

// 4x4, column-vector convention, stored m[row][column].
struct Mat4 {
    float m[4][4] = {};
};

// Accumulated in double, rounded once: a world matrix far from the origin (stages sit tens of thousands of units out,
// where a float step is 0.002) times a view whose translation cancels it would otherwise lose the small difference
// that places the object in front of the camera, and two touching objects (a character's pupils on its head) land a
// different rounding apart every frame and fight in depth.
inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += static_cast<double>(a.m[i][k]) * static_cast<double>(b.m[k][j]);
            r.m[i][j] = static_cast<float>(sum);
        }
    return r;
}

inline Mat4 from3x4(const Float3x4& f) {
    Mat4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = f.m[i][j];
    r.m[3][3] = 1;
    return r;
}

// The inverse of an orthonormal camera frame: rows right, up, forward; translation -R^T * position.
inline Mat4 view_of(const Camera& camera) {
    Mat4 v;
    const Float3x4& f = camera.frame;
    for (int axis = 0; axis < 3; ++axis) {
        float t = 0;
        for (int k = 0; k < 3; ++k) {
            v.m[axis][k] = f.m[k][axis];
            t += f.m[k][axis] * f.m[k][3];
        }
        v.m[axis][3] = -t;
    }
    v.m[3][3] = 1;
    return v;
}

// Depth is reversed: the near plane maps to 1 and the far plane to 0, depth buffers clear to kFarDepth and a draw passes
// where its depth is GREATER (or equal) than the stored one. A 32-bit float depth buffer holds n / z then, nearly
// uniform in relative precision over the whole range (1e-5 units at 150 units from a near plane of 1); the
// conventional mapping crowds 1 - n / z next to 1.0, where a float step is 0.004 units at that distance. The levels
// lay glow quads 0.005 units over their walls (GUN Fortress's corridor lights, glare sources tested against the
// scene's depth): the conventional mapping could not tell them apart from the wall, and they lost their triangles
// frame by frame as the camera moved (docs/RENDER_FLICKER.md).
inline constexpr float kFarDepth = 0.0f;

// Clip space of D3D (x, y in [-1, 1], z in [0, 1], reversed: near 1, far 0) of the picture a view of `view_aspect`
// displays of the camera (view.h displayed_window: the authored picture on the view's canvas, fitted to the view).
// Vulkan draws the same clip space through negative-height viewports (render_vk/pipelines.h).
inline Mat4 projection_of(const Camera& camera, float view_aspect, float canvas_aspect) {
    const ViewWindow window = displayed_window(ViewWindow{camera.view_window_x, camera.view_window_y}, canvas_aspect,
                                               view_aspect, camera.framing);
    const float n = camera.near_plane;
    const float f = camera.far_plane;
    Mat4 p;
    p.m[0][0] = 1.0f / window.x;
    p.m[1][1] = 1.0f / window.y;
    if (camera.parallel) {
        p.m[2][2] = -1.0f / (f - n);
        p.m[2][3] = f / (f - n);
        p.m[3][3] = 1.0f;
    } else {
        p.m[2][2] = -n / (f - n);
        p.m[2][3] = n * f / (f - n);
        p.m[3][2] = 1.0f;
    }
    return p;
}

// The aspect a view's canvas is authored to be seen at (a zero-size canvas is the view's own pixels).
inline float canvas_aspect_of(const Canvas& canvas, float view_width, float view_height) {
    const float w = canvas.width > 0.0f ? canvas.width : view_width;
    const float h = canvas.height > 0.0f ? canvas.height : view_height;
    return canvas.aspect > 0.0f ? canvas.aspect : w / h;
}

// A view's canvas in clip space for draws anchored at `anchor` (R2_DRIVER.md 4.8, view.h canvas_fit): the box of the
// canvas's authored aspect inside the view, its anchor point kept on the view's; Fill stretches the canvas over the
// view. The canvas z is the depth, reversed (depth 1 - z: see kFarDepth).
inline Mat4 canvas_projection(const Canvas& canvas, Anchor anchor, float view_width, float view_height) {
    const float w = canvas.width > 0.0f ? canvas.width : view_width;
    const float h = canvas.height > 0.0f ? canvas.height : view_height;
    const CanvasFit fit = canvas_fit(w, h, canvas_aspect_of(canvas, view_width, view_height), anchor,
                                     view_width / view_height);
    Mat4 p;
    p.m[0][0] = fit.sx;
    p.m[0][3] = fit.ox;
    p.m[1][1] = fit.sy;
    p.m[1][3] = fit.oy;
    p.m[2][2] = -1.0f;  // reversed depth: canvas z 0 is the near end (1), z 1 the far end (0)
    p.m[2][3] = 1.0f;
    p.m[3][3] = 1.0f;
    return p;
}

// A rectangle of view pixels: [left, right) x [top, bottom).
struct PixelBox {
    long left = 0, top = 0, right = 0, bottom = 0;
};

// The clip of canvas draws at `anchor` (whose placement is `p`, canvas_projection): the canvas's own rectangle, as the
// console's raster edge clipped the 2D, so art parked outside the canvas stays hidden on a wider view - except a
// Projected one, which marks the world, and a Continued one, a pattern the layout continued to the view's edges: those
// are clipped to the view.
inline PixelBox canvas_scissor(const Canvas& canvas, Anchor anchor, const Mat4& p, float view_width,
                               float view_height) {
    if (clipped_to_view(anchor))
        return PixelBox{0, 0, static_cast<long>(view_width), static_cast<long>(view_height)};
    const float canvas_width = canvas.width > 0.0f ? canvas.width : view_width;
    const float canvas_height = canvas.height > 0.0f ? canvas.height : view_height;
    const float left = (p.m[0][3] + 1.0f) * 0.5f * view_width;
    const float right = (p.m[0][0] * canvas_width + p.m[0][3] + 1.0f) * 0.5f * view_width;
    const float top = (1.0f - p.m[1][3]) * 0.5f * view_height;
    const float bottom = (1.0f - (p.m[1][1] * canvas_height + p.m[1][3])) * 0.5f * view_height;
    return PixelBox{std::max(0L, std::lround(left)), std::max(0L, std::lround(top)),
                    std::min(static_cast<long>(view_width), std::lround(right)),
                    std::min(static_cast<long>(view_height), std::lround(bottom))};
}

// A canvas draw laid out in a region of the view's canvas (a player's viewport in split screen, view.h region_fit):
// its placement, and its clip - the region's anchored box (or, for a draw clipped to the view, the region's share of
// the view), within the view.
inline void region_projection(const Canvas& canvas, const CanvasRegion& region, Anchor anchor, float view_width,
                              float view_height, Mat4* placement, PixelBox* scissor) {
    const float w = canvas.width > 0.0f ? canvas.width : view_width;
    const float h = canvas.height > 0.0f ? canvas.height : view_height;
    const CanvasFit fit = region_fit(w, h, canvas_aspect_of(canvas, view_width, view_height), region, anchor,
                                     view_width / view_height);
    Mat4 p;
    p.m[0][0] = fit.sx;
    p.m[0][3] = fit.ox;
    p.m[1][1] = fit.sy;
    p.m[1][3] = fit.oy;
    p.m[2][2] = -1.0f;  // reversed depth: canvas z 0 is the near end (1), z 1 the far end (0)
    p.m[2][3] = 1.0f;
    p.m[3][3] = 1.0f;
    *placement = p;
    // The region's share of the view, in view pixels.
    float left = region.x / w * view_width, right = (region.x + region.width) / w * view_width;
    float top = region.y / h * view_height, bottom = (region.y + region.height) / h * view_height;
    if (!clipped_to_view(anchor)) {
        left = std::max(left, (fit.sx * region.x + fit.ox + 1.0f) * 0.5f * view_width);
        right = std::min(right, (fit.sx * (region.x + region.width) + fit.ox + 1.0f) * 0.5f * view_width);
        top = std::max(top, (1.0f - (fit.sy * region.y + fit.oy)) * 0.5f * view_height);
        bottom = std::min(bottom, (1.0f - (fit.sy * (region.y + region.height) + fit.oy)) * 0.5f * view_height);
    }
    *scissor = PixelBox{std::max(0L, std::lround(left)), std::max(0L, std::lround(top)),
                        std::min(static_cast<long>(view_width), std::lround(right)),
                        std::min(static_cast<long>(view_height), std::lround(bottom))};
}

// The same placement for a canvas point a canvas draw samples a snapshot or the history at: the point's place on
// the view, as fractions of the view (rows of u' = r0 . (u, v, 1), v' = r1 . (u, v, 1)).
inline std::array<std::array<float, 4>, 2> canvas_image_rows(const Mat4& placement) {
    // Clip x = m00 x + m03 -> fraction (clip + 1) / 2; clip y = m11 y + m13 -> fraction (1 - clip) / 2.
    return {{{0.5f * placement.m[0][0], 0.0f, 0.5f * (placement.m[0][3] + 1.0f), 0.0f},
             {0.0f, -0.5f * placement.m[1][1], 0.5f * (1.0f - placement.m[1][3]), 0.0f}}};
}

// RGBA8 with R in the lowest byte, as the shaders unpack it.
inline std::uint32_t packed(const Float4& c) {
    const auto byte = [](float v) {
        return static_cast<std::uint32_t>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f));
    };
    return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (byte(c.w) << 24);
}

// Column-major, the HLSL default packing.
inline void store_column_major(const Mat4& m, float* out) {
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            out[col * 4 + row] = m.m[row][col];
}

inline std::uint32_t flags_of(const Material& m) {
    return (m.vertex_colours ? kPrelit : 0u) | (m.modulate ? kModulate : 0u) | (m.lit ? kLit : 0u) |
           (m.ambient ? kAmbient : 0u) | (m.combine == Combine::TwoStage ? kCombined : 0u) |
           (m.baked_light ? 0u : kUnbaked);
}

// The bump-environment base pass's extra inputs (ExtraPass::Kind::BumpEnvironment): the environment map's descriptor and
// sampler, the bump scale and coefficient. view_row .w is otherwise 0 and unread (the view normal uses .xyz).
inline void fill_bump_environment(const ExtraPass& extra, std::uint32_t environment_srv,
                                  std::uint32_t environment_sampler, DrawConstants* out) {
    out->flags |= kBumpEnvironment | (extra.framebuffer_alpha ? kBumpFramebufferAlpha : 0u);
    out->coefficient = extra.coefficient;
    out->cutoff = -1.0f;  // 0x804987C8(1): the alpha compare always passes
    out->view_row[0][3] = extra.bump_shift;
    std::memcpy(&out->view_row[1][3], &environment_srv, sizeof(float));
    std::memcpy(&out->view_row[2][3], &environment_sampler, sizeof(float));
}

inline void fill_light_constants(const LightSet& set, LightConstants* out) {
    *out = LightConstants{};
    out->ambient[0] = std::min(set.ambient.x, 1.0f);
    out->ambient[1] = std::min(set.ambient.y, 1.0f);
    out->ambient[2] = std::min(set.ambient.z, 1.0f);
    out->counts[0] = std::min<std::uint32_t>(set.directional_count, 4);
    out->counts[1] = std::min<std::uint32_t>(set.point_count, 8);
    for (std::uint32_t i = 0; i < out->counts[0]; ++i) {
        const Float3& c = set.directional_colour[i];
        const Float3& d = set.directional_vector[i];
        const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const float scale = length > 0 ? 1.0f / length : 0.0f;
        out->directional_colour[i][0] = c.x;
        out->directional_colour[i][1] = c.y;
        out->directional_colour[i][2] = c.z;
        out->directional_vector[i][0] = d.x * scale;
        out->directional_vector[i][1] = d.y * scale;
        out->directional_vector[i][2] = d.z * scale;
    }
    for (std::uint32_t i = 0; i < out->counts[1]; ++i) {
        const LightSet::Point& p = set.points[i];
        out->point_position[i][0] = p.position.x;
        out->point_position[i][1] = p.position.y;
        out->point_position[i][2] = p.position.z;
        out->point_position[i][3] = p.radius;
        out->point_colour[i][0] = p.colour.x;
        out->point_colour[i][1] = p.colour.y;
        out->point_colour[i][2] = p.colour.z;
    }
}

}  // namespace shadow::render
