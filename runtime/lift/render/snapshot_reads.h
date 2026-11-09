#pragma once

// The texels of a view image (a snapshot of the scene colour) that a canvas draw can read - so a snapshot resolves only
// the part of the scene its distortion draws sample instead of the whole view (2026-10-26: a full 1440p 4x snapshot per
// distortion draw was ~1 ms of GPU time each, up to 2.5 ms, and the frames with several of them missed the refresh).
//
// The immediate shader (shaders/immediate.hlsl) places a vertex's texture coordinate with the rows uv_row (u' = r0 .
// (u, v, 1), v' = r1 . (u, v, 1), fractions of the view), interpolates it linearly over the triangle (a canvas draw's
// w is 1) and samples the image bilinearly. So every coordinate a covered sample evaluates lies in the triangle's
// coordinates' bounding box - except at a pixel the triangle only partly covers: there the coordinate is evaluated at
// the pixel's centre, up to half a pixel outside the triangle on each axis, which moves it by at most half the
// triangle's texture-coordinate change per pixel. The bilinear footprint adds the texel on each side, and one texel of
// slack covers the shader's float evaluation of the same rows. Anything this cannot bound (lines, a non-affine w, a
// degenerate placement) answers false: the caller then resolves the whole view, as before.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "render/frame.h"

namespace shadow::render {

struct TexelRect {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;  // right and bottom exclusive
    bool empty() const { return right <= left || bottom <= top; }
};

// Grows `rect` (in texels of a width x height image covering the view) by what one draw can read. `position` holds
// the clip-space rows x, y, w of placement x world (the draw's vertices map to clip = position . (x, y, z, 1));
// `uv` the rows the shader maps texture coordinates with. False: not bounded here, read the whole image.
inline bool grow_sampled_texels(Topology topology, const ImmediateVertex* vertices, std::uint32_t vertex_count,
                                const std::uint32_t* indices, std::uint32_t index_count, const float position[3][4],
                                const float uv[2][4], std::uint32_t width, std::uint32_t height, TexelRect* rect) {
    if (topology != Topology::Triangles && topology != Topology::TriangleStrip)
        return false;
    const std::uint32_t count = index_count != 0u ? index_count : vertex_count;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    struct Corner {
        double px, py;  // pixels on the view
        double tx, ty;  // texels of the image
    };
    const auto corner = [&](std::uint32_t n, Corner* out) {
        const std::uint32_t i = index_count != 0u ? indices[n] : n;
        if (i >= vertex_count)
            return false;
        const ImmediateVertex& v = vertices[i];
        const double p[4] = {v.position.x, v.position.y, v.position.z, 1.0};
        double clip[3];
        for (int r = 0; r < 3; ++r)
            clip[r] = position[r][0] * p[0] + position[r][1] * p[1] + position[r][2] * p[2] + position[r][3] * p[3];
        if (std::fabs(clip[2] - 1.0) > 1.0e-6)  // only an affine (canvas) placement interpolates linearly here
            return false;
        out->px = (clip[0] + 1.0) * 0.5 * w;
        out->py = (1.0 - clip[1]) * 0.5 * h;
        out->tx = (uv[0][0] * v.uv[0] + uv[0][1] * v.uv[1] + uv[0][2]) * w;
        out->ty = (uv[1][0] * v.uv[0] + uv[1][1] * v.uv[1] + uv[1][2]) * h;
        return std::isfinite(out->px) && std::isfinite(out->py) && std::isfinite(out->tx) && std::isfinite(out->ty);
    };
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    const std::uint32_t triangles = topology == Topology::Triangles ? count / 3u : (count >= 3u ? count - 2u : 0u);
    for (std::uint32_t t = 0; t < triangles; ++t) {
        const std::uint32_t base = topology == Topology::Triangles ? 3u * t : t;
        Corner c[3];
        for (std::uint32_t k = 0; k < 3; ++k)
            if (!corner(base + k, &c[k]))
                return false;
        const double ax = c[1].px - c[0].px, ay = c[1].py - c[0].py;
        const double bx = c[2].px - c[0].px, by = c[2].py - c[0].py;
        const double det = ax * by - bx * ay;
        // A triangle without area covers no sample and reads nothing.
        if (det == 0.0)
            continue;
        // The texel change per pixel: J = [dT1 dT2] [dP1 dP2]^-1.
        const double ux = c[1].tx - c[0].tx, uy = c[1].ty - c[0].ty;
        const double vx = c[2].tx - c[0].tx, vy = c[2].ty - c[0].ty;
        const double j00 = (ux * by - vx * ay) / det, j01 = (vx * ax - ux * bx) / det;
        const double j10 = (uy * by - vy * ay) / det, j11 = (vy * ax - uy * bx) / det;
        const double ex = 0.5 * (std::fabs(j00) + std::fabs(j01));
        const double ey = 0.5 * (std::fabs(j10) + std::fabs(j11));
        if (!std::isfinite(ex) || !std::isfinite(ey))
            return false;
        for (const Corner& k : c) {
            x0 = std::min(x0, k.tx - ex);
            x1 = std::max(x1, k.tx + ex);
            y0 = std::min(y0, k.ty - ey);
            y1 = std::max(y1, k.ty + ey);
        }
    }
    if (x1 < x0)
        return true;  // nothing covered
    // A coordinate t reads texels floor(t - 1/2) and the next, clamped to the image's edge texels (the sampler clamps);
    // one more texel each side for the float evaluation.
    const auto index = [](double v, double last) {
        return static_cast<std::int32_t>(std::min(std::max(v, 0.0), last));
    };
    TexelRect r;
    r.left = index(std::floor(x0 - 0.5) - 1.0, w - 1.0);
    r.top = index(std::floor(y0 - 0.5) - 1.0, h - 1.0);
    r.right = index(std::floor(x1 - 0.5) + 2.0, w - 1.0) + 1;
    r.bottom = index(std::floor(y1 - 0.5) + 2.0, h - 1.0) + 1;
    if (rect->empty()) {
        *rect = r;
    } else {
        rect->left = std::min(rect->left, r.left);
        rect->top = std::min(rect->top, r.top);
        rect->right = std::max(rect->right, r.right);
        rect->bottom = std::max(rect->bottom, r.bottom);
    }
    return true;
}

}  // namespace shadow::render
