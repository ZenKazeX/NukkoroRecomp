// The Vulkan renderer's headless check on the desktop (tests/smoke_checks.h: split-screen packets rendered, read back,
// compared with the values the chain may give and rendered a second time to the same bytes).
//
// Then the draw checks (tests/draw_checks.h: meshes, culling, depth, particles, canvas quads, a texture, at known
// pixels) and the effect checks (tests/effect_checks.h: snapshots, histories, skinning, morphs, glare with the
// folded composite) and the frame graph's checks (tests/graph_checks.h: culling, aliasing, barriers, load and store
// ops, pooling, refused graphs); `--png <file>` saves the draw checks' picture, `--effects-png <file>` the effect checks'.
//
//   shadow_render_vk_smoke [--validation] [--samples N] [--png <file>] [--effects-png <file>] [--v2-plan]
//                          [--no-local-read]
//
// Exit code 0: every check passed and the validation layer (when on) reported nothing.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "render_vk/renderer.h"
#include "render_vk/report.h"
#include "render_vk/tests/draw_checks.h"
#include "render_vk/tests/effect_checks.h"
#include "render_vk/tests/graph_checks.h"
#include "render_vk/tests/smoke_checks.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

using namespace shadow;

int main(int argc, char** argv) {
    render_vk::RendererOptions options;
    const char* png = nullptr;
    const char* effects_png = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--validation") == 0)
            options.device.validation = true;
        else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc)
            options.msaa_samples = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc)
            png = argv[++i];
        else if (std::strcmp(argv[i], "--effects-png") == 0 && i + 1 < argc)
            effects_png = argv[++i];
        else if (std::strcmp(argv[i], "--v2-plan") == 0)
            options.transient_targets = options.merge_passes = options.keep_depth_on_tile = false;
        else if (std::strcmp(argv[i], "--no-local-read") == 0)
            options.device.local_read = false;
    }
    render_vk::Renderer renderer;
    std::string error;
    if (!renderer.create(options, &error)) {
        std::fprintf(stderr, "create: %s\n", error.c_str());
        return 2;
    }
    for (const std::string& l : render_vk::capability_report(renderer.device()))
        std::printf("%s\n", l.c_str());
    std::printf("scene: format %d, %u samples\n", static_cast<int>(renderer.scene_format()),
                static_cast<unsigned>(renderer.scene_samples()));
    const auto print = [](const std::string& l) { std::printf("%s\n", l.c_str()); };
    int failures = render_vk::run_smoke_checks(renderer, print);
    std::vector<std::uint8_t> picture;
    failures += render_vk::run_draw_checks(renderer, print, &picture);
    if (png != nullptr && !picture.empty() &&
        stbi_write_png(png, render_vk::kDrawCheckWidth, render_vk::kDrawCheckHeight, 4, picture.data(),
                       render_vk::kDrawCheckWidth * 4) == 0)
        std::printf("could not write %s\n", png);
    std::vector<std::uint8_t> effects;
    failures += render_vk::run_effect_checks(renderer, print, &effects);
    if (effects_png != nullptr && !effects.empty() &&
        stbi_write_png(effects_png, render_vk::kEffectCheckWidth, render_vk::kEffectCheckHeight, 4, effects.data(),
                       render_vk::kEffectCheckWidth * 4) == 0)
        std::printf("could not write %s\n", effects_png);
    failures += render_vk::run_graph_checks(renderer, print);
    const std::vector<std::string> messages = renderer.take_debug_errors();
    for (const std::string& m : messages)
        std::printf("validation %s\n", m.c_str());
    if (!messages.empty())
        ++failures;
    renderer.destroy();
    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
