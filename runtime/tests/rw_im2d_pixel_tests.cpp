// A pixel test for what is drawn through RwDriver (the lifted renderer's RenderWare driver scaffold), on the WARP
// (software D3D12) adapter: no GPU of the machine is used. Built only in a tree with SHADOW_RENDER_LIFTED.
//
// The scene lives in test guest memory the way the game's RenderWare objects do: a camera whose frame buffer is a
// camera raster (640 x 480, the main screen's), the camera's frame with an identity local-to-world matrix, and a vertex
// array of 24-byte RwIm2DVertex records. A frame is: camera_begin, the draw, camera_end, submit_frame; the exact frame the
// driver hands over is rendered by the native renderer (Renderer::render_to_image, RGBA8) and read back.
//
// Checks (all deterministic: WARP images are compared with each other, never with a file):
//  1. an empty frame is uniformly the background (so a difference below is a drawing);
//  2. the line emitter owner (RwIm2DRenderLine 0x80491D0C: vertices v1, v2 of the array) draws the SAME image as the
//     plain primitive entry called with a line list of exactly those two vertices (no indices): different code in the
//     driver (index resolution), same geometry;
//  3. the same for the triangle emitter and a triangle list of its three vertices;
//  4. the images are not empty (some pixels differ from the background) and the owner draws change when v1/v2 change
//     (a negative control inside the test: vertex indices swapped to a different pair give a different image).
// usage: rw_im2d_pixel_tests
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "render/frame.h"
#include "render/packet_queue.h"
#include "render/renderer.h"
#include "rw_driver/im2d_prims.h"
#include "rw_driver/raster_render.h"
#include "rw_driver/rw_driver.h"

#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace gekko::scaffold {
u32 rw_im2d_render_primitive(Cpu& c, u32 type, u32 vertices, u32 count);
u32 rw_im2d_render_indexed_primitive(Cpu& c, u32 type, u32 vertices, u32 count, u32 indices, u32 index_count);
}  // namespace gekko::scaffold

namespace {
using namespace gekko;
using namespace gekko::scaffold;

constexpr u32 kCamera = 0x80800000u, kFrame = 0x80801000u, kRaster = 0x80802000u;
constexpr u32 kVertices = 0x80810000u, kPair = 0x80811000u, kTriple = 0x80812000u;
constexpr u32 kWidth = 640u, kHeight = 480u;
// the raster-render scene: the game's Im texture, a source raster (type 4) with 8 x 8 GX RGBA8 texels, the small-data base
constexpr u32 kR13 = 0x805EC500u, kImTex = 0x80820000u, kImTexRaster = 0x80821000u, kSource = 0x80830000u,
              kTexels = 0x80840000u;
constexpr u32 kRasterExt = 0x34u, kTextureExt = 0x58u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

u32 bits(float f) {
    u32 v;
    std::memcpy(&v, &f, sizeof(v));
    return v;
}

struct Vertex {
    float x, y, z;
    u32 rgba;
};

void write_vertex(Cpu& cpu, u32 address, const Vertex& v) {
    cpu.mem_write32(address + 0x00u, bits(v.x));
    cpu.mem_write32(address + 0x04u, bits(v.y));
    cpu.mem_write32(address + 0x08u, bits(v.z));
    cpu.mem_write32(address + 0x0Cu, v.rgba);  // R, G, B, A bytes
    cpu.mem_write32(address + 0x10u, bits(0.0f));
    cpu.mem_write32(address + 0x14u, bits(0.0f));
}

void build_scene(Cpu& cpu) {
    // the camera raster: a top-level camera raster (type 2), the whole 640 x 480 screen
    cpu.mem_write32(kRaster + layout::kRasterParent, 0u);
    cpu.mem_write32(kRaster + layout::kRasterWidth, kWidth);
    cpu.mem_write32(kRaster + layout::kRasterHeight, kHeight);
    cpu.mem_write16(kRaster + layout::kRasterOffsetX, 0u);
    cpu.mem_write16(kRaster + layout::kRasterOffsetY, 0u);
    cpu.mem_write8(kRaster + layout::kRasterType, 2u);
    // the camera's frame: identity local-to-world
    for (u32 i = 0; i < 16u; ++i) cpu.mem_write32(kFrame + layout::kFrameLtm + 4u * i, 0u);
    cpu.mem_write32(kFrame + layout::kFrameLtm + 0x00u, bits(1.0f));  // right
    cpu.mem_write32(kFrame + layout::kFrameLtm + 0x14u, bits(1.0f));  // up
    cpu.mem_write32(kFrame + layout::kFrameLtm + 0x28u, bits(1.0f));  // at
    cpu.mem_write32(kCamera + layout::kCameraFrame, kFrame);
    cpu.mem_write32(kCamera + layout::kCameraProjection, 1u);
    cpu.mem_write32(kCamera + layout::kCameraFrameBuffer, kRaster);
    cpu.mem_write32(kCamera + layout::kCameraViewWindow, bits(0.5f));
    cpu.mem_write32(kCamera + layout::kCameraViewWindow + 4u, bits(0.375f));
    cpu.mem_write32(kCamera + layout::kCameraNear, bits(0.5f));
    cpu.mem_write32(kCamera + layout::kCameraFar, bits(100.0f));
    // five vertices of the array, well inside the canvas (RGBA: red, green, blue, white, yellow)
    const Vertex v[5] = {{100.0f, 80.0f, 0.0f, 0xFF0000FFu},
                         {500.0f, 120.0f, 0.0f, 0x00FF00FFu},
                         {320.0f, 400.0f, 0.0f, 0x0000FFFFu},
                         {60.0f, 440.0f, 0.0f, 0xFFFFFFFFu},
                         {580.0f, 60.0f, 0.0f, 0xFFFF00FFu}};
    for (u32 i = 0; i < 5u; ++i) write_vertex(cpu, kVertices + 0x18u * i, v[i]);
}

// 8 x 8 GX RGBA8: 4 x 4 blocks of 64 bytes (16 AR pairs, then 16 GB pairs); block order left to right, top to bottom:
// red, green (alpha 128), blue, yellow.
void build_raster_scene(Cpu& cpu) {
    cpu.gpr[13] = kR13;
    cpu.mem_write32(kR13 + kRasterTargetWord, kRaster);  // the current target: the camera raster (type 2)
    cpu.mem_write32(layout::kRasterExtOffset, kRasterExt);
    cpu.mem_write32(layout::kTextureExtOffset, kTextureExt);
    cpu.mem_write32(layout::kImTexture, kImTex);
    cpu.mem_write32(kImTex + layout::kTextureRaster, kImTexRaster);
    cpu.mem_write32(kImTex + layout::kTextureFilterAddressing, 0x1102u);  // linear, wrap, wrap
    cpu.mem_write32(kSource + layout::kRasterParent, kSource);
    cpu.mem_write32(kSource + layout::kRasterWidth, 8u);
    cpu.mem_write32(kSource + layout::kRasterHeight, 8u);
    cpu.mem_write32(kSource + layout::kRasterDepth, 32u);
    cpu.mem_write16(kSource + layout::kRasterOffsetX, 0u);
    cpu.mem_write16(kSource + layout::kRasterOffsetY, 0u);
    cpu.mem_write8(kSource + layout::kRasterType, 4u);
    const u32 ext = kSource + kRasterExt;
    cpu.mem_write32(ext + layout::kRasterExtGxFormat, 6u);
    cpu.mem_write32(ext + layout::kRasterExtTlutFormat, 0u);
    cpu.mem_write32(ext + layout::kRasterExtFlags, 1u);
    cpu.mem_write32(ext + layout::kRasterExtPixels, kTexels);
    cpu.mem_write32(ext + layout::kRasterExtTexture, 0u);
    const struct { u8 r, g, b, a; } colour[4] = {{255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 255, 255}, {255, 255, 0, 255}};
    for (u32 block = 0; block < 4u; ++block)
        for (u32 pixel = 0; pixel < 16u; ++pixel) {
            const u32 at = kTexels + block * 64u;
            cpu.mem_write8(at + pixel * 2u, colour[block].a);
            cpu.mem_write8(at + pixel * 2u + 1u, colour[block].r);
            cpu.mem_write8(at + 32u + pixel * 2u, colour[block].g);
            cpu.mem_write8(at + 32u + pixel * 2u + 1u, colour[block].b);
        }
}

struct Harness {
    GuestMemory memory;
    Cpu cpu{memory};
    shadow::render::Renderer renderer;
    shadow::render::PacketQueue queue;
    std::unique_ptr<RwDriver> driver;
    std::vector<std::uint8_t> last_image;
    std::string error;

    Harness() {
        cpu.reset();
        build_scene(cpu);
        build_raster_scene(cpu);
        shadow::render::RendererOptions options;
        options.device.adapter = shadow::render::DeviceOptions::Adapter::Warp;
        options.msaa_samples = 1;
        require(renderer.create(options, &error), "renderer on WARP: " + error);
        driver = std::make_unique<RwDriver>(renderer, queue);
        driver->install();
        driver->set_frame_observer([this](const shadow::render::FramePacket& packet) {
            std::string render_error;
            if (!renderer.render_to_image(packet, kWidth, kHeight, &last_image, &render_error)) {
                last_image.clear();
                error = render_error;
            }
        });
    }
    ~Harness() {
        driver->uninstall();
        driver.reset();
    }
    // One frame: the bracket, the draw (a callable), the end, the hand-over. Returns the rendered RGBA8 image.
    template <class Draw>
    std::vector<std::uint8_t> frame(Draw&& draw) {
        last_image.clear();
        driver->camera_begin(cpu, kCamera);
        draw();
        driver->camera_end(cpu, kCamera);
        driver->submit_frame(cpu);
        require(!last_image.empty(), "the frame was not rendered: " + error);
        require(last_image.size() == std::size_t{kWidth} * kHeight * 4u, "image size");
        return last_image;
    }
};

std::size_t differing_pixels(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4)
        if (std::memcmp(&a[i], &b[i], 4) != 0) ++n;
    return n;
}

void set_regs(Cpu& cpu, u32 vertices, u32 count, u32 a, u32 b, u32 c) {
    cpu.gpr[1] = 0x817E0000u;
    cpu.gpr[3] = vertices;
    cpu.gpr[4] = count;
    cpu.gpr[5] = a;
    cpu.gpr[6] = b;
    cpu.gpr[7] = c;
}
}  // namespace

int main() {
    try {
        Harness h;
        Cpu& cpu = h.cpu;
        const auto line_owner = [&](u32 a, u32 b) {
            set_regs(cpu, kVertices, 5u, a, b, 0u);
            serve_im2d_line(cpu, rw_im2d_render_indexed_primitive);
        };
        const auto triangle_owner = [&](u32 a, u32 b, u32 c) {
            set_regs(cpu, kVertices, 5u, a, b, c);
            serve_im2d_triangle(cpu, rw_im2d_render_indexed_primitive);
        };
        // the reference: the primitive entry (no indices) over a copy of the chosen vertices in order
        const auto copy_vertices = [&](u32 to, std::initializer_list<u32> which) {
            u32 k = 0;
            for (u32 index : which) {
                for (u32 off = 0; off < 0x18u; off += 4u)
                    cpu.mem_write32(to + 0x18u * k + off, cpu.mem_read32(kVertices + 0x18u * index + off));
                ++k;
            }
        };

        const std::vector<std::uint8_t> empty = h.frame([] {});
        const std::size_t empty_vs_empty = differing_pixels(empty, h.frame([] {}));
        require(empty_vs_empty == 0u, "two empty frames differ (the renderer is not deterministic here)");

        // line v1 = 0, v2 = 2
        const std::vector<std::uint8_t> line_a = h.frame([&] { line_owner(0u, 2u); });
        copy_vertices(kPair, {0u, 2u});
        const std::vector<std::uint8_t> line_b = h.frame([&] {
            set_regs(cpu, kPair, 2u, 0u, 0u, 0u);
            rw_im2d_render_primitive(cpu, 1u, kPair, 2u);
        });
        const std::size_t line_drawn = differing_pixels(line_a, empty);
        require(line_drawn > 0u, "the line owner drew nothing");
        require(differing_pixels(line_a, line_b) == 0u, "the line owner and the primitive entry draw different images");
        const std::vector<std::uint8_t> line_c = h.frame([&] { line_owner(1u, 3u); });
        require(differing_pixels(line_a, line_c) > 0u, "a different vertex pair draws the same image (the test is blind)");
        const std::vector<std::uint8_t> line_d = h.frame([&] { line_owner(2u, 0u); });
        // a line has no direction: reversed endpoints may draw a different image only through interpolation order; the
        // colours here run from blue to red instead of red to blue
        const std::size_t reversed = differing_pixels(line_a, line_d);

        // triangles: both windings of (0, 1, 2) - the render states in force may cull one of them (the default cull mode);
        // each must equal the triangle-list primitive over the same three vertices, and at least one must draw
        std::size_t tri_drawn = 0;
        std::vector<std::uint8_t> tri_first;
        std::array<u32, 3> tri_order = {0u, 1u, 2u};
        for (const auto& order : {std::array<u32, 3>{0u, 1u, 2u}, std::array<u32, 3>{0u, 2u, 1u}}) {
            const std::vector<std::uint8_t> owner = h.frame([&] { triangle_owner(order[0], order[1], order[2]); });
            copy_vertices(kTriple, {order[0], order[1], order[2]});
            const std::vector<std::uint8_t> primitive = h.frame([&] {
                set_regs(cpu, kTriple, 3u, 0u, 0u, 0u);
                rw_im2d_render_primitive(cpu, 3u, kTriple, 3u);
            });
            require(differing_pixels(owner, primitive) == 0u,
                    "the triangle owner and the primitive entry draw different images");
            const std::size_t drawn = differing_pixels(owner, empty);
            if (drawn > tri_drawn) {
                tri_drawn = drawn;
                tri_first = owner;
                tri_order = order;
            }
        }
        require(tri_drawn > 0u, "neither winding of the triangle draws anything");
        const std::vector<std::uint8_t> tri_c = h.frame([&] { triangle_owner(0u, 3u, 4u); });
        const std::vector<std::uint8_t> tri_d = h.frame([&] { triangle_owner(0u, 4u, 3u); });
        require(differing_pixels(tri_first, tri_c) > 0u || differing_pixels(tri_first, tri_d) > 0u,
                "different triples draw the same image (the test is blind)");

        // ---- RwRasterRender: the textured quad ------------------------------------------------------------------------
        const auto pixel = [&](const std::vector<std::uint8_t>& image, u32 x, u32 y) {
            const std::size_t at = (std::size_t{y} * kWidth + x) * 4u;
            return std::array<int, 4>{image[at], image[at + 1], image[at + 2], image[at + 3]};
        };
        const auto near_colour = [&](const std::array<int, 4>& got, int r, int g, int b) {
            return std::abs(got[0] - r) <= 12 && std::abs(got[1] - g) <= 12 && std::abs(got[2] - b) <= 12;
        };
        const auto describe = [&](const std::array<int, 4>& p) {
            return "(" + std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "," +
                   std::to_string(p[3]) + ")";
        };
        const auto quad_sink = [](Cpu& c, u32 raster, const u32 rect[4], bool scaled, bool fast) {
            RwDriver::installed()->raster_quad(c, raster, rect, scaled, fast);
        };
        const auto error_sink = [](Cpu&, RasterErrorSite) -> bool { throw std::runtime_error("unexpected error path"); };
        const auto render_raster = [&](u32 x, u32 y, u32 w, u32 hh, bool scaled, bool fast) {
            cpu.gpr[1] = 0x817E0000u;
            cpu.gpr[3] = kSource;
            cpu.gpr[4] = 0x80813000u;
            cpu.mem_write32(0x80813000u, x);
            cpu.mem_write32(0x80813004u, y);
            cpu.mem_write32(0x80813008u, w);
            cpu.mem_write32(0x8081300Cu, hh);
            cpu.gpr[5] = scaled ? 1u : 0u;
            cpu.gpr[6] = fast ? 1u : 0u;
            serve_raster_render(cpu, quad_sink, error_sink);
            require(cpu.gpr[3] == 1u, "the core returns 1 after a draw");
        };
        const auto snapshot = [&]() {
            std::vector<u32> s;
            for (u32 state : {1u, 3u, 4u, 9u, 10u, 11u, 14u}) {
                cpu.mem_write32(0x80813100u, 0xDEADBEEFu);
                h.driver->render_state_get(cpu, state, 0x80813100u);
                s.push_back(cpu.mem_read32(0x80813100u));
            }
            s.push_back(cpu.mem_read32(layout::kImTexture));
            s.push_back(cpu.mem_read32(kImTex + layout::kTextureRaster));
            s.push_back(cpu.mem_read32(kImTex + layout::kTextureFilterAddressing));
            return s;
        };
        const std::vector<u32> before = snapshot();
        // scaled onto (50, 50) 200 x 200: each quadrant of the source is 100 x 100 pixels; not blended (ONE, ZERO)
        const std::vector<std::uint8_t> scaled_opaque = h.frame([&] { render_raster(50u, 50u, 200u, 200u, true, false); });
        require(snapshot() == before, "the render states / Im texture words are not as the original leaves them");
        const auto red = pixel(scaled_opaque, 100u, 100u), green = pixel(scaled_opaque, 200u, 100u),
                   blue = pixel(scaled_opaque, 100u, 200u), yellow = pixel(scaled_opaque, 200u, 200u);
        require(near_colour(red, 255, 0, 0), "scaled quad: top-left quadrant is not red " + describe(red));
        require(near_colour(green, 0, 255, 0), "scaled quad: top-right quadrant is not green " + describe(green));
        require(near_colour(blue, 0, 0, 255), "scaled quad: bottom-left quadrant is not blue " + describe(blue));
        require(near_colour(yellow, 255, 255, 0), "scaled quad: bottom-right quadrant is not yellow " + describe(yellow));
        require(pixel(scaled_opaque, 400u, 300u) == pixel(empty, 400u, 300u) &&
                    pixel(scaled_opaque, 40u, 40u) == pixel(empty, 40u, 40u) &&
                    pixel(scaled_opaque, 260u, 260u) == pixel(empty, 260u, 260u),
                "the scaled quad drew outside its rect");
        // the same with the alpha blend: the half-transparent green quadrant mixes with the background, the opaque ones do not
        const std::vector<std::uint8_t> scaled_blend = h.frame([&] { render_raster(50u, 50u, 200u, 200u, true, true); });
        require(snapshot() == before, "the render states changed after a blended quad");
        require(pixel(scaled_blend, 100u, 100u) == red, "alpha-blended opaque texel differs from the replaced one");
        const auto green_blend = pixel(scaled_blend, 200u, 100u), background = pixel(empty, 200u, 100u);
        require(green_blend != green && green_blend[1] < green[1] && green_blend[1] > background[1],
                "the half-transparent texel is not mixed with the background when fast " + describe(green_blend));
        // unscaled: the source's own 8 x 8 at (300, 60), nearest-filtered: the rect's size words are not read
        const std::vector<std::uint8_t> unscaled = h.frame([&] { render_raster(300u, 60u, 999u, 999u, false, false); });
        require(snapshot() == before, "the render states changed after an unscaled quad");
        const auto ur = pixel(unscaled, 302u, 62u), ug = pixel(unscaled, 306u, 62u), ub = pixel(unscaled, 302u, 66u),
                   uy = pixel(unscaled, 306u, 66u);
        require(near_colour(ur, 255, 0, 0) && near_colour(ug, 0, 255, 0) && near_colour(ub, 0, 0, 255) &&
                    near_colour(uy, 255, 255, 0),
                "unscaled quad quadrants: " + describe(ur) + describe(ug) + describe(ub) + describe(uy));
        require(pixel(unscaled, 312u, 62u) == pixel(empty, 312u, 62u) &&
                    pixel(unscaled, 302u, 70u) == pixel(empty, 302u, 70u) &&
                    pixel(unscaled, 296u, 62u) == pixel(empty, 296u, 62u),
                "the unscaled quad is not 8 x 8 pixels at the rect origin");
        // ---- the raster-level clear (RwRasterClearRect on a camera / z-buffer raster -> ClearQuad 0x80499E58 ->
        // RwDriver::raster_clear): drawn-then-cleared frames against the drawn-only frame --------------------------------
        constexpr u32 kRed = 0xFF0000FFu;   // RGBA, R in the low byte: opaque red
        const std::vector<std::uint8_t> drawn = h.frame([&] { triangle_owner(tri_order[0], tri_order[1], tri_order[2]); });
        require(differing_pixels(drawn, empty) > 0u, "the reference triangle drew nothing");
        const auto pixel_at = [&](const std::vector<std::uint8_t>& image, u32 x, u32 y) {
            return std::array<std::uint8_t, 4>{image[4u * (y * kWidth + x)], image[4u * (y * kWidth + x) + 1u],
                                               image[4u * (y * kWidth + x) + 2u], image[4u * (y * kWidth + x) + 3u]};
        };
        // 1. a colour clear of the whole raster replaces everything the triangle drew: one uniform red image
        const std::vector<std::uint8_t> full = h.frame([&] {
            triangle_owner(tri_order[0], tri_order[1], tri_order[2]);
            h.driver->raster_clear(cpu, 0, 0, static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight), kRed, true, false);
        });
        std::size_t not_red = 0;
        for (u32 y = 0; y < kHeight; ++y)
            for (u32 x = 0; x < kWidth; ++x) {
                const auto p = pixel_at(full, x, y);
                if (p[0] != 255u || p[1] != 0u || p[2] != 0u) ++not_red;
            }
        require(not_red == 0u, "a full colour clear left " + std::to_string(not_red) + " pixels that are not the clear colour");
        // 2. a rectangle-limited colour clear: inside the rectangle the clear colour, outside exactly the drawn-only frame
        const u32 rx = 200u, ry = 100u, rw = 240u, rh = 200u;
        const std::vector<std::uint8_t> part = h.frame([&] {
            triangle_owner(tri_order[0], tri_order[1], tri_order[2]);
            h.driver->raster_clear(cpu, static_cast<std::int32_t>(rx), static_cast<std::int32_t>(ry), static_cast<std::int32_t>(rw),
                                   static_cast<std::int32_t>(rh), kRed, true, false);
        });
        std::size_t inside_wrong = 0, outside_wrong = 0, inside_changed = 0;
        for (u32 y = 0; y < kHeight; ++y)
            for (u32 x = 0; x < kWidth; ++x) {
                const bool inside = x >= rx && x < rx + rw && y >= ry && y < ry + rh;
                const auto p = pixel_at(part, x, y);
                if (inside) {
                    // one pixel of slack at the rectangle's edge (the rasterisation rule)
                    const bool edge = x == rx || x == rx + rw - 1u || y == ry || y == ry + rh - 1u;
                    if (!edge && (p[0] != 255u || p[1] != 0u || p[2] != 0u)) ++inside_wrong;
                    if (p != pixel_at(drawn, x, y)) ++inside_changed;
                } else if (p != pixel_at(drawn, x, y)) {
                    ++outside_wrong;
                }
            }
        require(inside_wrong == 0u, "a rectangle clear left " + std::to_string(inside_wrong) + " inside pixels not the clear colour");
        require(outside_wrong == 0u, "a rectangle clear changed " + std::to_string(outside_wrong) + " pixels outside the rectangle");
        require(inside_changed > 0u, "the rectangle clear changed nothing inside (the test is blind)");
        // 3. a depth-only clear keeps the colour: the image is the drawn-only frame
        const std::vector<std::uint8_t> depth_only = h.frame([&] {
            triangle_owner(tri_order[0], tri_order[1], tri_order[2]);
            h.driver->raster_clear(cpu, 0, 0, static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight), kRed, false, true);
        });
        require(differing_pixels(depth_only, drawn) == 0u, "a depth-only clear changed the colour");
        // 4. nothing to clear, and an empty rectangle: the drawn-only frame
        const std::vector<std::uint8_t> nothing = h.frame([&] {
            triangle_owner(tri_order[0], tri_order[1], tri_order[2]);
            h.driver->raster_clear(cpu, 0, 0, static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight), kRed, false, false);
            h.driver->raster_clear(cpu, 10, 10, 0, 50, kRed, true, true);
        });
        require(differing_pixels(nothing, drawn) == 0u, "a clear with nothing to clear changed the picture");
        // 5. negative control: the clear colour matters (green instead of red gives a different image than red)
        const std::vector<std::uint8_t> green_clear = h.frame([&] {
            triangle_owner(tri_order[0], tri_order[1], tri_order[2]);
            h.driver->raster_clear(cpu, 0, 0, static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight), 0xFF00FF00u, true, false);
        });
        require(differing_pixels(green_clear, full) > 0u, "a different clear colour draws the same image (the test is blind)");
        std::printf("PASS rw raster clear (WARP): full colour clear uniform, rectangle clear exact outside (%zu inside pixels changed), "
                    "depth-only keeps the colour, no-op clears change nothing\n", inside_changed);

        std::printf("PASS rw im2d pixels (WARP, %ux%u): line owner == line-list primitive (%zu pixels drawn), triangle owner == "
                    "triangle-list primitive (%zu pixels drawn), different vertices draw different images; reversed line "
                    "differs in %zu pixels; raster quad: scaled and unscaled quadrant colours, blend, outside untouched, "
                    "render states restored\n",
                    kWidth, kHeight, line_drawn, tri_drawn, reversed);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw im2d pixels: %s\n", error.what());
        return 1;
    }
}
