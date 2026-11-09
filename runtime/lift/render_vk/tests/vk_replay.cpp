// Replays a recorded packet stream (render/packet_stream.h: recorded from the D3D12 renderer with
// GEKKO_PACKET_RECORD=<file>) through render_vk: every resource the game asked for is made (recorded textures decoded
// by the game's own importer), the recorded handles are mapped to render_vk's, and every recorded frame is rendered at
// its recorded size in the recorded order - the frames rendered only for their history too, so trails build up as in
// the game. Each frame the game read back (a capture) is written as <out>/vk-<tick>.png and, given the D3D12 renderer's
// capture of the same frame (--reference <pattern with %llu for the tick>), compared: the share of pixels that differ
// at all, by more than 2 and by more than 16 steps in any channel, the mean difference, and a difference image
// <out>/diff-<tick>.png (the per-channel difference x 8).
//
// Every frame's GPU time per rendering group (timestamps, V6) is summed by group name: the summary lists the groups
// by their total, with the mean per frame. `--v2-plan` turns off V6's transient images, pass merging and keeping the
// depth on tile (the A/B); `--no-tile-depth` only the last (scene + glare sources pairs, depth-free passes);
// `--no-local-read` leaves the device's local read off (depth-free passes without pairs: the fallback).
//
//   shadow_render_vk_replay <stream> --out <dir> [--reference <pattern>] [--validation] [--samples N]
//                           [--anisotropy N] [--v2-plan] [--no-tile-depth] [--no-local-read]
//                           [--groups]
//
// `--groups`: each capture's rendering groups in recorded order, with their GPU times.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "assets/texture_import.h"
#include "render/packet_stream.h"
#include "render_vk/renderer.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

using namespace shadow;

namespace {

std::uint32_t mapped(const std::map<std::uint32_t, std::uint32_t>& map, std::uint32_t recorded) {
    if (recorded == 0)
        return 0;
    const auto found = map.find(recorded);
    return found != map.end() ? found->second : 0u;
}

// The recorded handles of a packet replaced by render_vk's (an unknown one becomes "none").
void remap(render::FramePacket& packet, const std::map<std::uint32_t, std::uint32_t>& textures,
           const std::map<std::uint32_t, std::uint32_t>& meshes) {
    for (render::DrawRecord& draw : packet.draws) {
        draw.mesh.value = mapped(meshes, draw.mesh.value);
        draw.material.texture.value = mapped(textures, draw.material.texture.value);
        draw.material.extra.texture.value = mapped(textures, draw.material.extra.texture.value);
    }
    for (render::ParticleBatch& batch : packet.particle_batches)
        batch.texture.value = mapped(textures, batch.texture.value);
    for (render::ImmediateDraw& im : packet.immediates)
        im.texture.value = mapped(textures, im.texture.value);
}

struct Comparison {
    std::size_t pixels = 0, any = 0, over2 = 0, over16 = 0;
    double sum = 0.0;  // of the largest channel difference per pixel
    int largest = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: shadow_render_vk_replay <stream> --out <dir> [--reference <pattern>] "
                             "[--validation] [--samples N]\n");
        return 2;
    }
    const std::string stream = argv[1];
    std::string out_dir = ".";
    std::string reference;
    render_vk::RendererOptions options;
    bool print_groups = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out_dir = argv[++i];
        else if (std::strcmp(argv[i], "--reference") == 0 && i + 1 < argc)
            reference = argv[++i];
        else if (std::strcmp(argv[i], "--validation") == 0)
            options.device.validation = true;
        else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc)
            options.msaa_samples = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--anisotropy") == 0 && i + 1 < argc)
            options.max_anisotropy = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--v2-plan") == 0)
            options.transient_targets = options.merge_passes = options.keep_depth_on_tile = false;
        else if (std::strcmp(argv[i], "--no-tile-depth") == 0)
            options.keep_depth_on_tile = false;
        else if (std::strcmp(argv[i], "--no-local-read") == 0)
            options.device.local_read = false;
        else if (std::strcmp(argv[i], "--groups") == 0)
            print_groups = true;
    }
    std::filesystem::create_directories(out_dir);
    render::PacketReader reader;
    std::string error;
    if (!reader.open(stream, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    render_vk::Renderer renderer;
    if (!renderer.create(options, &error)) {
        std::fprintf(stderr, "create: %s\n", error.c_str());
        return 2;
    }
    std::map<std::uint32_t, std::uint32_t> textures, meshes;
    std::uint64_t events = 0, frames = 0, captures = 0, resource_failures = 0, compared = 0;
    double frame_ms = 0.0;
    Comparison total;
    render_vk::FrameGraph::Stats graph_total;  // summed over the rendered frames
    struct GpuTotal {
        double ms = 0.0;
        std::uint64_t count = 0;
    };
    std::map<std::string, GpuTotal> gpu_groups;  // GPU time by rendering group name, all frames
    double gpu_ms = 0.0, gpu_max = 0.0;
    std::uint64_t pairs_total = 0;
    std::uint64_t gpu_frames = 0;
    render::StreamRecord r;
    while (reader.next(&r, &error)) {
        ++events;
        switch (r.event) {
        case render::StreamEvent::TextureSource: {
            assets::TextureImage image;
            std::string why;
            render::TextureHandle made;
            if (assets::import_texture(r.record, r.options, &image, &why))
                made = renderer.create_texture(r.scope, image, &why);
            if (!made && ++resource_failures <= 10)
                std::fprintf(stderr, "texture %s: %s\n", r.record.name.c_str(), why.c_str());
            textures[r.handle] = made.value;
            break;
        }
        case render::StreamEvent::TextureImage: {
            std::string why;
            const render::TextureHandle made = renderer.create_texture(r.scope, r.image, &why);
            if (!made && ++resource_failures <= 10)
                std::fprintf(stderr, "texture image: %s\n", why.c_str());
            textures[r.handle] = made.value;
            break;
        }
        case render::StreamEvent::Mesh: {
            std::string why;
            const render::MeshHandle made = renderer.create_mesh(r.scope, r.mesh, &why);
            if (!made && ++resource_failures <= 10)
                std::fprintf(stderr, "mesh: %s\n", why.c_str());
            meshes[r.handle] = made.value;
            break;
        }
        case render::StreamEvent::Release:
            if (r.texture)
                renderer.release(render::TextureHandle{mapped(textures, r.handle)}, r.tick);
            else
                renderer.release(render::MeshHandle{mapped(meshes, r.handle)}, r.tick);
            break;
        case render::StreamEvent::FinishPackets:
            renderer.finish_packets(r.tick);
            break;
        case render::StreamEvent::ReleaseScope:
            renderer.release_scope(r.scope);
            break;
        case render::StreamEvent::InvalidateHistories:
            renderer.invalidate_histories();
            break;
        case render::StreamEvent::Frame: {
            if (r.width == 0 || r.height == 0)
                break;  // presented to the window: replayed only as images
            remap(r.packet, textures, meshes);
            std::vector<std::uint8_t> rgba;
            const auto start = std::chrono::steady_clock::now();
            if (!renderer.render_to_image(r.packet, r.width, r.height, r.read_back ? &rgba : nullptr, &error,
                                          r.commit_history)) {
                std::fprintf(stderr, "frame %llu: %s\n", static_cast<unsigned long long>(r.packet.tick),
                             error.c_str());
                return 3;
            }
            frame_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            ++frames;
            {
                const render_vk::FrameGraph::Stats& g = renderer.stats().graph;
                graph_total.passes += g.passes;
                graph_total.kept += g.kept;
                graph_total.culled += g.culled;
                graph_total.textures += g.textures;
                graph_total.images += g.images;
                graph_total.aliased += g.aliased;
                graph_total.barriers += g.barriers;
                graph_total.barrier_calls += g.barrier_calls;
                graph_total.transient += g.transient;
                graph_total.merged += g.merged;
                graph_total.stores += g.stores;
                graph_total.depth_stores += g.depth_stores;
                pairs_total += renderer.stats().pairs;
                const render_vk::Renderer::Stats now = renderer.stats();
                if (now.gpu_frames > gpu_frames) {  // this frame's times were read
                    gpu_frames = now.gpu_frames;
                    gpu_ms += now.gpu_ms;
                    gpu_max = std::max(gpu_max, now.gpu_ms);
                    for (const auto& span : now.gpu_passes) {
                        GpuTotal& t = gpu_groups[span.name];
                        t.ms += span.ms;
                        ++t.count;
                    }
                }
            }
            if (!r.read_back)
                break;
            ++captures;
            const unsigned long long tick = static_cast<unsigned long long>(r.packet.tick);
            char name[64];
            std::snprintf(name, sizeof(name), "vk-%llu.png", tick);
            const std::string path = (std::filesystem::path(out_dir) / name).string();
            stbi_write_png(path.c_str(), static_cast<int>(r.width), static_cast<int>(r.height), 4, rgba.data(),
                           static_cast<int>(r.width * 4));
            const render_vk::Renderer::Stats stats = renderer.stats();
            std::printf("frame %llu (%ux%u): draws %llu, skipped %llu, glares %llu, snapshots %llu, history saves %llu, "
                        "skin jobs %llu",
                        tick, r.width, r.height, static_cast<unsigned long long>(stats.draws),
                        static_cast<unsigned long long>(stats.skipped_draws),
                        static_cast<unsigned long long>(stats.glares),
                        static_cast<unsigned long long>(stats.snapshots),
                        static_cast<unsigned long long>(stats.history_saves),
                        static_cast<unsigned long long>(stats.skin_jobs));
            std::printf("; graph %u passes (%u culled, %u merged), %u textures (%u transient) in %u images, %u barriers "
                        "in %u calls; GPU %.3f ms",
                        stats.graph.passes, stats.graph.culled, stats.graph.merged, stats.graph.textures,
                        stats.graph.transient, stats.graph.images, stats.graph.barriers, stats.graph.barrier_calls,
                        stats.gpu_ms);
            std::printf("; pairs %llu, attachment stores %u (depth %u)", static_cast<unsigned long long>(stats.pairs),
                        stats.graph.stores, stats.graph.depth_stores);
            if (!reference.empty()) {
                char ref_path[1024];
                std::snprintf(ref_path, sizeof(ref_path), reference.c_str(), tick);
                int w = 0, h = 0, channels = 0;
                stbi_uc* ref = stbi_load(ref_path, &w, &h, &channels, 4);
                if (ref == nullptr || w != static_cast<int>(r.width) || h != static_cast<int>(r.height)) {
                    std::printf("; no reference %s", ref_path);
                } else {
                    Comparison c;
                    std::vector<std::uint8_t> diff(rgba.size(), 255);
                    for (std::size_t p = 0; p < std::size_t{r.width} * r.height; ++p) {
                        int d = 0;
                        for (int k = 0; k < 3; ++k) {
                            const int e = std::abs(static_cast<int>(rgba[p * 4 + k]) - static_cast<int>(ref[p * 4 + k]));
                            d = std::max(d, e);
                            diff[p * 4 + k] = static_cast<std::uint8_t>(std::min(255, e * 8));
                        }
                        ++c.pixels;
                        c.sum += d;
                        c.largest = std::max(c.largest, d);
                        c.any += d > 0 ? 1u : 0u;
                        c.over2 += d > 2 ? 1u : 0u;
                        c.over16 += d > 16 ? 1u : 0u;
                    }
                    stbi_image_free(ref);
                    std::snprintf(name, sizeof(name), "diff-%llu.png", tick);
                    stbi_write_png((std::filesystem::path(out_dir) / name).string().c_str(), static_cast<int>(r.width),
                                   static_cast<int>(r.height), 4, diff.data(), static_cast<int>(r.width * 4));
                    std::printf("; vs D3D12: differ %.3f%%, >2 %.3f%%, >16 %.3f%%, mean %.4f, largest %d",
                                100.0 * c.any / c.pixels, 100.0 * c.over2 / c.pixels, 100.0 * c.over16 / c.pixels,
                                c.sum / c.pixels, c.largest);
                    ++compared;
                    total.pixels += c.pixels;
                    total.any += c.any;
                    total.over2 += c.over2;
                    total.over16 += c.over16;
                    total.sum += c.sum;
                    total.largest = std::max(total.largest, c.largest);
                }
            }
            std::printf("\n");
            if (print_groups)
                for (const auto& span : stats.gpu_passes)
                    std::printf("    %8.4f ms  %s\n", span.ms, span.name.c_str());
            break;
        }
        }
    }
    if (!error.empty()) {
        std::fprintf(stderr, "stream: %s\n", error.c_str());
        return 3;
    }
    const render_vk::Renderer::Stats stats = renderer.stats();
    std::printf("replayed %llu events: %llu frames (%.2f ms each, recording + GPU, serial), %llu captures, %zu textures "
                "and %zu meshes live, %llu resources not made\n",
                static_cast<unsigned long long>(events), static_cast<unsigned long long>(frames),
                frames != 0 ? frame_ms / frames : 0.0, static_cast<unsigned long long>(captures), stats.textures,
                stats.meshes, static_cast<unsigned long long>(resource_failures));
    std::printf("frame graph, all frames: %u passes declared, %u kept, %u culled, %u merged; %u textures (%u "
                "transient) in %u images (%u aliased); %u barriers in %u calls; target pool %zu images, %.1f MiB, %llu "
                "made\n",
                graph_total.passes, graph_total.kept, graph_total.culled, graph_total.merged, graph_total.textures,
                graph_total.transient, graph_total.images, graph_total.aliased, graph_total.barriers,
                graph_total.barrier_calls, stats.pool_images, static_cast<double>(stats.pool_bytes) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(stats.pool_created));
    std::printf("rendering instances, all frames: %llu scene + glare sources pairs; %u attachment stores, %u of them "
                "depth\n",
                static_cast<unsigned long long>(pairs_total), graph_total.stores, graph_total.depth_stores);
    if (gpu_frames != 0) {
        // GPU time by rendering group, largest total first (a group's mean is over its runs: a frame may run a group
        // name several times - a view's scene passes between its glare passes).
        std::printf("GPU time, %llu frames: mean %.3f ms, largest %.3f ms (one frame at a time: no overlap)\n",
                    static_cast<unsigned long long>(gpu_frames), gpu_ms / static_cast<double>(gpu_frames), gpu_max);
        std::vector<std::pair<std::string, GpuTotal>> by_total(gpu_groups.begin(), gpu_groups.end());
        std::sort(by_total.begin(), by_total.end(),
                  [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
        for (const auto& [name, t] : by_total)
            std::printf("  %-40s %8.3f ms mean over %5llu runs (%5.1f%% of all)\n", name.c_str(),
                        t.ms / static_cast<double>(t.count), static_cast<unsigned long long>(t.count),
                        100.0 * t.ms / gpu_ms);
    }
    std::printf("skipped draws, all frames:");
    for (std::size_t i = 0; i < render_vk::Renderer::kSkipReasons; ++i)
        if (stats.skipped_by_reason[i] != 0)
            std::printf(" %s %llu", render_vk::Renderer::skip_reason_name(static_cast<render_vk::Renderer::SkipReason>(i)),
                        static_cast<unsigned long long>(stats.skipped_by_reason[i]));
    std::printf("\n");
    if (compared != 0)
        std::printf("vs D3D12 over %llu captures: differ %.3f%%, >2 %.3f%%, >16 %.3f%%, mean %.4f, largest %d\n",
                    static_cast<unsigned long long>(compared), 100.0 * total.any / total.pixels,
                    100.0 * total.over2 / total.pixels, 100.0 * total.over16 / total.pixels,
                    total.sum / total.pixels, total.largest);
    const auto messages = renderer.take_debug_errors();
    for (std::size_t i = 0; i < messages.size() && i < 20; ++i)
        std::printf("validation %s\n", messages[i].c_str());
    if (!messages.empty())
        std::printf("%zu validation messages\n", messages.size());
    renderer.destroy();
    return 0;
}
