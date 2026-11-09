#include "render_vk/tests/smoke_checks.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace shadow::render_vk {

namespace {

template <class... Args>
std::string line(const char* format, Args... args) {
    char text[512];
    std::snprintf(text, sizeof(text), format, args...);
    return text;
}

// The values the chain may give a channel. UNORM16 scene: the clear's UNORM16 storage, the mean of identical samples
// (exact), then the UNORM8 output - and Vulkan lets each float -> UNORM conversion return EITHER of the two nearest
// representable values ("Fixed-Point Data Conversions": implementations should, not must, round to nearest), so a value
// whose scaled form sits near .5 has two valid results per conversion. Another scene format (the fallback,
// docs/VULKAN_RENDERER.md 5.3): within one step of the value. Which result a driver gives is fixed for that driver; the
// checks verify that separately (every frame rendered twice, byte for byte).
bool allowed_channel(VkFormat scene, float value, std::uint8_t got) {
    if (scene != VK_FORMAT_R16G16B16A16_UNORM)
        return std::fabs(static_cast<double>(got) - static_cast<double>(value) * 255.0) <= 1.0;
    const double scaled16 = static_cast<double>(value) * 65535.0;
    for (const double u16 : {std::floor(scaled16), std::ceil(scaled16)}) {
        const double scaled8 = u16 / 65535.0 * 255.0;
        if (got == static_cast<std::uint8_t>(std::floor(scaled8)) || got == static_cast<std::uint8_t>(std::ceil(scaled8)))
            return true;
    }
    return false;
}

}  // namespace

render::FramePacket split_screen_packet(render::FrameBuilder& builder, std::uint64_t tick,
                                        const render::Float4& background, float content_aspect) {
    builder.begin_frame(tick, background);
    render::Camera camera;
    camera.frame = render::look_at({0, 0, -10}, {0, 0, 0}, {0, 1, 0});
    const render::ViewId left = builder.view(camera, {0.0f, 0.0f, 0.5f, 1.0f});
    const render::ViewId right = builder.view(camera, {0.5f, 0.0f, 0.5f, 1.0f});
    const render::ViewId overlay = builder.view(camera, {0.0f, 0.0f, 1.0f, 1.0f}, render::Canvas{640, 480, 0}, true);
    for (const render::ViewId v : {left, right, overlay}) {
        builder.begin_pass(v, render::PassKind::Scene, render::Clear{});
        // A draw of a mesh the renderer does not have: counted as skipped, never drawn.
        builder.draw(render::MeshHandle{}, 0, render::Material{}, render::Float3x4{}, render::Recipe{});
        builder.end_pass();
    }
    builder.save_history(overlay);
    render::FramePacket packet = builder.finish();
    packet.content_aspect = content_aspect;
    return packet;
}

int run_smoke_checks(Renderer& renderer, const std::function<void(const std::string&)>& log) {
    render::FrameBuilder builder;
    int failures = 0;
    std::string error;
    const struct {
        std::uint32_t width, height;
        render::Float4 background;
    } frames[] = {
        {64, 32, {0.25f, 0.5f, 0.75f, 1.0f}},
        {64, 32, {1.0f, 0.0f, 0.2f, 1.0f}},   // the pooled targets reused
        {33, 17, {0.1f, 0.9f, 0.4f, 1.0f}},   // odd sizes: new targets, a new capture image
        {640, 360, {0.6f, 0.3f, 0.05f, 1.0f}},
    };
    std::uint64_t tick = 1;
    for (const auto& f : frames) {
        render::FramePacket packet = split_screen_packet(builder, tick++, f.background, 1.0f);
        std::vector<std::uint8_t> rgba;
        if (!renderer.render_to_image(packet, f.width, f.height, &rgba, &error)) {
            log(line("render_to_image %ux%u: %s", f.width, f.height, error.c_str()));
            return failures + 1;
        }
        // Every pixel is the first pixel's value (the views, the bars and the overlay all show the background), and
        // that value is one the conversions allow.
        const std::uint8_t* first = rgba.data();
        const VkFormat scene = renderer.scene_format();
        const bool value_ok = allowed_channel(scene, f.background.x, first[0]) &&
                              allowed_channel(scene, f.background.y, first[1]) &&
                              allowed_channel(scene, f.background.z, first[2]) && first[3] == 255;
        std::size_t wrong = 0;
        std::size_t first_wrong = 0;
        for (std::size_t p = 0; p < std::size_t{f.width} * f.height; ++p)
            if (std::memcmp(&rgba[p * 4], first, 4) != 0 && wrong++ == 0)
                first_wrong = p;
        // Self-determinism: the same packet again gives the same bytes.
        std::vector<std::uint8_t> again;
        if (!renderer.render_to_image(packet, f.width, f.height, &again, &error)) {
            log(line("render_to_image (again) %ux%u: %s", f.width, f.height, error.c_str()));
            return failures + 1;
        }
        const bool same = again == rgba;
        if (!value_ok || !same)
            ++failures;
        const Renderer::Stats stats = renderer.stats();
        log(line("frame %ux%u: background %.3f %.3f %.3f -> %u %u %u %u (%s), %zu pixels differ from it, re-render "
                 "%s; skipped draws %llu, history saves %llu, pooled images %zu, pipelines %zu",
                 f.width, f.height, static_cast<double>(f.background.x), static_cast<double>(f.background.y),
                 static_cast<double>(f.background.z), first[0], first[1], first[2], first[3],
                 value_ok ? "allowed" : "NOT ALLOWED", wrong, same ? "identical" : "DIFFERENT",
                 static_cast<unsigned long long>(stats.skipped_draws),
                 static_cast<unsigned long long>(stats.history_saves), stats.pool_images, stats.pipelines));
        if (wrong != 0) {
            const std::uint8_t* got = &rgba[first_wrong * 4];
            log(line("  first wrong pixel (%zu, %zu): %u %u %u %u", first_wrong % f.width, first_wrong / f.width,
                     got[0], got[1], got[2], got[3]));
            ++failures;
        }
        if (stats.skipped_draws != 3 || stats.history_saves != 1) {
            log("  unexpected counts (3 skipped draws and 1 history save expected)");
            ++failures;
        }
    }
    return failures;
}

}  // namespace shadow::render_vk
