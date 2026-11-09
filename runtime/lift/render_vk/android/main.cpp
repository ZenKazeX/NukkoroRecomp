// The Vulkan renderer on Android (docs/VULKAN_RENDERER.md, stage V5): a NativeActivity that drives
// render_vk::Renderer with FramePackets - the same packet contract (render/frame.h) the game hands the D3D12 renderer.
// Log tag "shadow_vk" (adb logcat -s shadow_vk).
//
//   1. On the first surface: the renderer is created, the device's capabilities are logged, and the headless checks
//      run (tests/smoke_checks.h: split-screen packets rendered off-screen, read back, compared, rendered twice to the
//      same bytes; tests/draw_checks.h: meshes, culling, depth, particles, canvas quads, a texture at known pixels;
//      tests/effect_checks.h: snapshots, histories, skinning, morphs, glare and its folded composite;
//      tests/graph_checks.h: the frame graph's culling, aliasing, barriers, load and store ops, pooling) -
//      "CHECKS PASS" / "CHECKS FAIL".
//   2. Every frame: a split-screen packet (two views and an overlay, pillar-boxed to 4:3 as an in-engine cutscene is),
//      its background cycling, rendered and presented through the pre-rotated swapchain - "PRESENT PASS" after 300.
//      Replay mode: when the app's files folder holds packets.stream (render/packet_stream.h, recorded from the game
//      on Windows: adb push <stream> /sdcard/Android/data/com.shadow.vk/files/packets.stream), its game frames are
//      presented instead, in a loop - "REPLAY LOOP <n>: <frames> frames".
// The surface follows Android's lifecycle: APP_CMD_TERM_WINDOW detaches it (the device and resources stay),
// APP_CMD_INIT_WINDOW attaches the next one.
//
// SCAFFOLDING (V5): the packets are made here until the game itself runs on Android and hands its own; deleted then.

#include <android/log.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>

#include "render/frame.h"
#include "render/packet_stream.h"
#include "render_vk/renderer.h"
#include "render_vk/report.h"
#include "render_vk/tests/draw_checks.h"
#include "render_vk/tests/effect_checks.h"
#include "render_vk/tests/graph_checks.h"
#include "render_vk/tests/smoke_checks.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "shadow_vk", __VA_ARGS__)
#define LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "shadow_vk", __VA_ARGS__)

using namespace shadow;

namespace {

constexpr std::uint64_t kPassFrames = 300;
// A recorded frame stays on screen for its tick distance to the previous one, at most this many vsyncs (the
// recordings keep some ticks, not all).
constexpr std::uint64_t kMaxHold = 4;

// The packet stream replay (render_vk/tests/vk_replay.cpp's loop, presented to the window).
struct Replay {
    std::string path;
    std::unique_ptr<render::PacketReader> reader;  // open while a loop plays
    std::map<std::uint32_t, std::uint32_t> textures, meshes;  // recorded handle -> render_vk's
    std::uint64_t tick_offset = 0;  // added on every loop so the renderer sees ticks only grow
    std::uint64_t last_tick = 0;
    std::uint64_t previous_recorded = 0;  // the last frame's recorded tick, this loop
    std::uint64_t loop = 0, frames = 0, failures = 0;
    render::FramePacket current;
    bool has_current = false;
    std::uint64_t hold = 0;
};

struct App {
    std::unique_ptr<render_vk::Renderer> renderer;
    render::FrameBuilder builder;
    Replay replay;
    bool failed = false;
    std::uint64_t presented = 0;
    bool passed = false;
};

std::uint32_t mapped(const std::map<std::uint32_t, std::uint32_t>& map, std::uint32_t recorded) {
    if (recorded == 0)
        return 0;
    const auto found = map.find(recorded);
    return found != map.end() ? found->second : 0u;
}

void remap(render::FramePacket& packet, const Replay& replay) {
    for (render::DrawRecord& draw : packet.draws) {
        draw.mesh.value = mapped(replay.meshes, draw.mesh.value);
        draw.material.texture.value = mapped(replay.textures, draw.material.texture.value);
        draw.material.extra.texture.value = mapped(replay.textures, draw.material.extra.texture.value);
    }
    for (render::ParticleBatch& batch : packet.particle_batches)
        batch.texture.value = mapped(replay.textures, batch.texture.value);
    for (render::ImmediateDraw& im : packet.immediates)
        im.texture.value = mapped(replay.textures, im.texture.value);
}

// Reads events up to the next frame, creating and releasing resources on the way. False: no frame (end of stream or
// an error; the stream is closed and every replay resource released, so the next call starts the next loop).
bool next_replay_frame(render_vk::Renderer& renderer, Replay& replay) {
    std::string error;
    if (!replay.reader) {
        replay.reader = std::make_unique<render::PacketReader>();
        if (!replay.reader->open(replay.path, &error)) {
            LOG_ERROR("replay: %s", error.c_str());
            replay.reader.reset();
            replay.path.clear();
            return false;
        }
        replay.tick_offset = replay.last_tick + 1;
        renderer.invalidate_histories();
    }
    render::StreamRecord r;
    while (replay.reader->next(&r, &error)) {
        switch (r.event) {
        case render::StreamEvent::TextureSource: {
            assets::TextureImage image;
            std::string why;
            render::TextureHandle made;
            if (assets::import_texture(r.record, r.options, &image, &why))
                made = renderer.create_texture(r.scope, image, &why);
            if (!made && ++replay.failures <= 10)
                LOG_ERROR("replay texture %s: %s", r.record.name.c_str(), why.c_str());
            replay.textures[r.handle] = made.value;
            break;
        }
        case render::StreamEvent::TextureImage: {
            std::string why;
            const render::TextureHandle made = renderer.create_texture(r.scope, r.image, &why);
            if (!made && ++replay.failures <= 10)
                LOG_ERROR("replay texture image: %s", why.c_str());
            replay.textures[r.handle] = made.value;
            break;
        }
        case render::StreamEvent::Mesh: {
            std::string why;
            const render::MeshHandle made = renderer.create_mesh(r.scope, r.mesh, &why);
            if (!made && ++replay.failures <= 10)
                LOG_ERROR("replay mesh: %s", why.c_str());
            replay.meshes[r.handle] = made.value;
            break;
        }
        case render::StreamEvent::Release:
            if (r.texture)
                renderer.release(render::TextureHandle{mapped(replay.textures, r.handle)}, r.tick + replay.tick_offset);
            else
                renderer.release(render::MeshHandle{mapped(replay.meshes, r.handle)}, r.tick + replay.tick_offset);
            break;
        case render::StreamEvent::FinishPackets:
            renderer.finish_packets(r.tick + replay.tick_offset);
            break;
        case render::StreamEvent::ReleaseScope:
            renderer.release_scope(r.scope);
            break;
        case render::StreamEvent::InvalidateHistories:
            renderer.invalidate_histories();
            break;
        case render::StreamEvent::Frame: {
            remap(r.packet, replay);
            const std::uint64_t recorded = r.packet.tick;
            r.packet.tick += replay.tick_offset;
            replay.hold = replay.has_current && recorded > replay.previous_recorded
                              ? std::min<std::uint64_t>(recorded - replay.previous_recorded, kMaxHold)
                              : 1;
            replay.previous_recorded = recorded;
            replay.last_tick = std::max(replay.last_tick, r.packet.tick);
            replay.current = std::move(r.packet);
            replay.has_current = true;
            ++replay.frames;
            return true;
        }
        }
    }
    if (!error.empty())
        LOG_ERROR("replay: %s", error.c_str());
    LOG("REPLAY LOOP %llu: %llu frames, %llu resource failures", static_cast<unsigned long long>(replay.loop),
        static_cast<unsigned long long>(replay.frames), static_cast<unsigned long long>(replay.failures));
    replay.reader.reset();
    replay.has_current = false;
    replay.frames = 0;
    ++replay.loop;
    renderer.finish_packets(replay.last_tick);
    for (render::Scope scope : {render::Scope::Frame, render::Scope::Event, render::Scope::Stage, render::Scope::Session})
        renderer.release_scope(scope);
    replay.textures.clear();
    replay.meshes.clear();
    return false;
}

void log_window(App& app, const char* why) {
    LOG("window (%s): picture %ux%u, %u quarter turns", why, app.renderer->window_width(),
        app.renderer->window_height(), app.renderer->window_quarter_turns());
}

bool create_renderer(App& app, std::string* error) {
    app.renderer = std::make_unique<render_vk::Renderer>();
    render_vk::RendererOptions options;
    if (!app.renderer->create(options, error))
        return false;
    for (const std::string& line : render_vk::capability_report(app.renderer->device()))
        LOG("%s", line.c_str());
    LOG("scene: format %d, %u samples", static_cast<int>(app.renderer->scene_format()),
        static_cast<unsigned>(app.renderer->scene_samples()));
    const auto log = [](const std::string& line) { LOG("%s", line.c_str()); };
    const int failures = render_vk::run_smoke_checks(*app.renderer, log) + render_vk::run_draw_checks(*app.renderer, log) +
                         render_vk::run_effect_checks(*app.renderer, log) +
                         render_vk::run_graph_checks(*app.renderer, log);
    if (failures == 0)
        LOG("CHECKS PASS");
    else
        LOG_ERROR("CHECKS FAIL: %d", failures);
    return true;
}

void handle_command(android_app* android, std::int32_t command) {
    App& app = *static_cast<App*>(android->userData);
    std::string error;
    switch (command) {
    case APP_CMD_INIT_WINDOW:
        if (android->window == nullptr || app.failed)
            break;
        if (!app.renderer && !create_renderer(app, &error)) {
            LOG_ERROR("FAIL: renderer: %s", error.c_str());
            app.failed = true;
            break;
        }
        if (!app.renderer->attach_window(render_vk::NativeWindow{android->window, nullptr},
                                         static_cast<std::uint32_t>(ANativeWindow_getWidth(android->window)),
                                         static_cast<std::uint32_t>(ANativeWindow_getHeight(android->window)),
                                         &error)) {
            LOG_ERROR("FAIL: window: %s", error.c_str());
            app.failed = true;
            break;
        }
        log_window(app, "new surface");
        break;
    case APP_CMD_TERM_WINDOW:
        if (app.renderer) {
            app.renderer->detach_window();
            LOG("surface released after %llu frames", static_cast<unsigned long long>(app.presented));
        }
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        if (app.renderer && app.renderer->has_window()) {
            if (app.renderer->resize_window(0, 0, &error))
                log_window(app, "surface changed");
            else
                LOG_ERROR("resize: %s", error.c_str());
        }
        break;
    default:
        break;
    }
}

}  // namespace

void android_main(android_app* android) {
    App app;
    android->userData = &app;
    android->onAppCmd = handle_command;
    LOG("start");
    if (android->activity->externalDataPath != nullptr) {
        const std::string stream = std::string(android->activity->externalDataPath) + "/packets.stream";
        struct stat info {};
        if (stat(stream.c_str(), &info) == 0) {
            app.replay.path = stream;
            LOG("replay: %s (%lld bytes)", stream.c_str(), static_cast<long long>(info.st_size));
        } else {
            LOG("no %s: split-screen packets", stream.c_str());
        }
    }
    while (!android->destroyRequested) {
        const bool drawing = app.renderer && app.renderer->has_window() && !app.failed;
        int events = 0;
        android_poll_source* source = nullptr;
        // Wait for events only when there is nothing to draw.
        while (ALooper_pollOnce(drawing ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr)
                source->process(android, source);
            if (android->destroyRequested || (app.renderer && app.renderer->has_window() && !app.failed))
                break;
        }
        if (android->destroyRequested)
            break;
        if (!app.renderer || !app.renderer->has_window() || app.failed)
            continue;
        std::string error;
        Replay& replay = app.replay;
        if (!replay.path.empty()) {
            if (replay.hold == 0 || !replay.has_current) {
                // At the end of a loop the next call opens the stream again.
                if (!next_replay_frame(*app.renderer, replay) && !next_replay_frame(*app.renderer, replay)) {
                    LOG_ERROR("FAIL: replay has no frames");
                    app.failed = true;
                    continue;
                }
            }
            --replay.hold;
            if (!app.renderer->render(replay.current, render_vk::PresentMode::VSync, &error)) {
                LOG_ERROR("FAIL: replay frame %llu: %s", static_cast<unsigned long long>(replay.current.tick),
                          error.c_str());
                app.failed = true;
                continue;
            }
            ++app.presented;
            continue;
        }
        const float t = static_cast<float>(app.presented) / 60.0f;
        const render::Float4 background{0.5f + 0.5f * std::sin(t), 0.5f + 0.5f * std::sin(t + 2.1f),
                                        0.5f + 0.5f * std::sin(t + 4.2f), 1.0f};
        render::FramePacket packet =
            render_vk::split_screen_packet(app.builder, app.presented + 1, background, 4.0f / 3.0f);
        if (!app.renderer->render(packet, render_vk::PresentMode::VSync, &error)) {
            LOG_ERROR("FAIL: frame %llu: %s", static_cast<unsigned long long>(app.presented), error.c_str());
            app.failed = true;
            continue;
        }
        app.builder.recycle(std::move(packet));
        ++app.presented;
        if (!app.passed && app.presented >= kPassFrames) {
            app.passed = true;
            const render_vk::Renderer::Stats stats = app.renderer->stats();
            LOG("PRESENT PASS: %llu frames (renderer frames %llu, skipped draws total %llu, views over views %llu, "
                "pooled images %zu, pipelines %zu)",
                static_cast<unsigned long long>(app.presented), static_cast<unsigned long long>(stats.frames),
                static_cast<unsigned long long>(stats.skipped_draws_total),
                static_cast<unsigned long long>(stats.views_over_views), stats.pool_images, stats.pipelines);
        }
    }
    if (app.renderer)
        app.renderer->destroy();
    LOG("end");
}
