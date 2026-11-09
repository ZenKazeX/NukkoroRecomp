#pragma once

// A CPU-side estimate of a packet's particle fill (docs/PERF_60FPS_2026-11-04.md, burst prediction; experimental).
// Measured 2026-11-04: the stage-start GPU bursts (9-18 ms frames at the GPU's 210 MHz idle clock) are one particle
// effect's batches in the scene pass - ~6 ms of an 8 ms scene at 1080p, ~2,000 camera-near particles. Their cost is
// fill: covered pixels, overdraw included, blended into the 4x MSAA scene. This estimates that fill from what the
// packet already holds, before anything is submitted: no GPU work, no readback, no wait.
//
// Per particle: depth z along the camera's forward axis, the quad's half extents projected through the camera's view
// window (half extents of the view at distance 1), the covered part of the view (each axis clipped to the view), times
// the view's pixels at the render size. Particles behind the near plane or off the view count nothing. The view window
// is the game's own (Hor+ widening is a constant factor per view; the cost model's coefficient absorbs it).

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "render/frame.h"

namespace shadow::render {

struct ParticleFill {
    std::uint32_t particles = 0;   // drawn particles (count)
    double pixels = 0.0;           // covered pixels summed over particles (overdraw included), at the render size
};

inline ParticleFill estimate_particle_fill(const FramePacket& packet, std::uint32_t render_w, std::uint32_t render_h) {
    ParticleFill out;
    for (const PassRecord& pass : packet.passes) {
        if (pass.view >= packet.views.size()) continue;
        const ViewRecord& view = packet.views[pass.view];
        const Camera& camera = pass.has_camera ? pass.camera : view.camera;
        const auto& m = camera.frame.m;
        const float fx = m[0][2], fy = m[1][2], fz = m[2][2];       // forward
        const float px = m[0][3], py = m[1][3], pz = m[2][3];       // position
        const float rx = m[0][0], ry = m[1][0], rz = m[2][0];       // right
        const float ux = m[0][1], uy = m[1][1], uz = m[2][1];       // up
        const double view_px = static_cast<double>(render_w) * view.rect.width * render_h * view.rect.height;
        const float wx = std::max(camera.view_window_x, 1e-4f), wy = std::max(camera.view_window_y, 1e-4f);
        const float near_plane = std::max(camera.near_plane, 1e-3f);
        const std::uint32_t end = std::min<std::uint32_t>(pass.first_draw + pass.draw_count,
                                                          static_cast<std::uint32_t>(packet.draws.size()));
        for (std::uint32_t d = pass.first_draw; d < end; ++d) {
            const DrawRecord& draw = packet.draws[d];
            if (draw.particles == kNoParticles || draw.particles >= packet.particle_batches.size()) continue;
            const ParticleBatch& batch = packet.particle_batches[draw.particles];
            const std::uint32_t last = std::min<std::uint32_t>(batch.first + batch.count,
                                                               static_cast<std::uint32_t>(packet.particles.size()));
            for (std::uint32_t i = batch.first; i < last; ++i) {
                const Particle& p = packet.particles[i];
                const float dx = p.position.x - px, dy = p.position.y - py, dz = p.position.z - pz;
                const float z = dx * fx + dy * fy + dz * fz;
                ++out.particles;
                if (z <= near_plane) continue;
                // The quad's centre and half extents in units of the view's half size (the view spans -1..1).
                const float cx = (dx * rx + dy * ry + dz * rz) / (z * wx);
                const float cy = (dx * ux + dy * uy + dz * uz) / (z * wy);
                const float hx = 0.5f * std::abs(p.size[0]) / (z * wx);
                const float hy = 0.5f * std::abs(p.size[1]) / (z * wy);
                const float x0 = std::max(cx - hx, -1.0f), x1 = std::min(cx + hx, 1.0f);
                const float y0 = std::max(cy - hy, -1.0f), y1 = std::min(cy + hy, 1.0f);
                if (x1 <= x0 || y1 <= y0) continue;
                out.pixels += 0.25 * static_cast<double>(x1 - x0) * static_cast<double>(y1 - y0) * view_px;
            }
        }
    }
    return out;
}

}  // namespace shadow::render
