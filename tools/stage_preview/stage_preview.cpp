// stage_preview - draws a stage's land with the new renderer, straight from the disc, into a PNG
// (docs/lift-20261006/R_DESIGN.md section 6: the importers and the renderer, before any game code reaches them).
//
//   stage_preview <files directory> <stage, e.g. stg0100> <out.png> [--warp] [--width W] [--height H]
//                 [--skip-far] [--side S] [--rise R] [--no-cull]

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "render/frame.h"
#include "render/renderer.h"
#include "stage_scene.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace shadow;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: stage_preview <files dir> <stage> <out.png> [--warp] [--width W] [--height H] "
                             "[--skip-far] [--side S] [--rise R] [--no-cull]\n");
        return 2;
    }
    const std::filesystem::path files = std::filesystem::u8path(argv[1]);
    const std::string stage_name = argv[2];
    const std::string out_png = argv[3];
    bool warp = false, skip_far = false;
    stage::LoadOptions load_options;
    std::uint32_t width = 1920, height = 1080;
    float camera_side = 0.45f, camera_rise = 0.35f;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--warp") warp = true;
        else if (a == "--skip-far") skip_far = true;
        else if (a == "--no-cull") load_options.cull = false;
        else if (a == "--side" && i + 1 < argc) camera_side = std::stof(argv[++i]);
        else if (a == "--rise" && i + 1 < argc) camera_rise = std::stof(argv[++i]);
        else if (a == "--width" && i + 1 < argc) width = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        else if (a == "--height" && i + 1 < argc) height = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    }

    render::Renderer renderer;
    render::RendererOptions options;
    options.device.adapter = warp ? render::DeviceOptions::Adapter::Warp : render::DeviceOptions::Adapter::HighPerformance;
    options.device.debug_layer = warp;
    std::string error;
    if (!renderer.create(options, &error)) {
        std::fprintf(stderr, "renderer: %s\n", error.c_str());
        return 1;
    }
    std::printf("device: %s\n", renderer.device().caps().adapter_name.c_str());
    stage::Scene scene;
    if (!stage::load(renderer, files, stage_name, load_options, &scene, &error)) {
        std::fprintf(stderr, "stage: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s: %zu land worlds, %zu textures\n", stage_name.c_str(), scene.worlds, scene.textures);

    const float* lo = scene.lo;
    const float* hi = scene.hi;
    const render::Float3 centre{(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    const float extent = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                   (hi[2] - lo[2]) * (hi[2] - lo[2]));
    render::Camera camera;
    camera.frame = render::look_at(
        render::Float3{centre.x + (hi[0] - lo[0]) * camera_side, hi[1] + (hi[1] - lo[1]) * camera_rise,
                       centre.z + (hi[2] - lo[2]) * camera_side},
        render::Float3{centre.x, lo[1] + (hi[1] - lo[1]) * 0.2f, centre.z}, render::Float3{0, 1, 0});
    camera.view_window_y = std::tan(0.45f);
    camera.near_plane = extent * 0.002f;
    camera.far_plane = extent * 6.0f;

    render::FrameBuilder builder;
    builder.begin_frame(1, render::Float4{0.55f, 0.65f, 0.78f, 1.0f});
    const render::ViewId view = builder.view(camera, render::ViewRect{});
    render::Clear clear;
    clear.colour = true;
    clear.depth = true;
    builder.begin_pass(view, render::PassKind::Scene, clear);
    stage::submit(scene, builder, !skip_far);
    builder.end_pass();
    std::vector<std::uint8_t> pixels;
    if (!renderer.render_to_image(builder.finish(), width, height, &pixels, &error)) {
        std::fprintf(stderr, "render: %s\n", error.c_str());
        return 1;
    }
    const render::Renderer::Stats stats = renderer.stats();
    std::printf("%zu meshes, %llu draws, %zu triangles, %zu draws without their texture\n", stats.meshes,
                static_cast<unsigned long long>(stats.draws), scene.triangles, scene.missing_textures);
    for (const std::string& message : renderer.take_debug_errors())
        std::printf("debug layer: %s\n", message.c_str());
    if (stbi_write_png(out_png.c_str(), static_cast<int>(width), static_cast<int>(height), 4, pixels.data(),
                       static_cast<int>(width * 4)) == 0) {
        std::fprintf(stderr, "could not write %s\n", out_png.c_str());
        return 1;
    }
    std::printf("wrote %s (%u x %u)\n", out_png.c_str(), width, height);
    return 0;
}
