#include "gekko/native_renderer.h"
#include "gekko/thread_name.h"  // ES-T1

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <exception>
#include <mutex>
#include <sstream>
#include <thread>

#include "gekko/profiling.h"
#include "gekko/native_failure_publication.h"
#include "native_memory.h"

namespace gekko {
namespace {

using native::Op;

void dump_command(std::ostream& out, const native::FrameSegment& segment,
                  const native::CommandHeader& header, const u8* record) {
    using native::FrameSegment;
    char line[512];
    switch (header.op) {
        case Op::Viewport: {
            const auto v = FrameSegment::read<native::Viewport>(record);
            std::snprintf(line, sizeof(line), "%.3f %.3f %.3f %.3f z %.4f..%.4f field %d", v.x,
                          v.y, v.width, v.height, v.near_z, v.far_z, v.field);
            break;
        }
        case Op::Scissor: {
            const auto s = FrameSegment::read<native::Scissor>(record);
            std::snprintf(line, sizeof(line), "%d %d %d %d", s.x, s.y, s.width, s.height);
            break;
        }
        case Op::Projection: {
            const auto p = FrameSegment::read<native::Projection>(record);
            std::snprintf(line, sizeof(line), "type %u {%g %g %g %g %g %g}", p.type, p.p[0],
                          p.p[1], p.p[2], p.p[3], p.p[4], p.p[5]);
            break;
        }
        case Op::PositionMatrix:
        case Op::NormalMatrix:
        case Op::TextureMatrix: {
            const auto m = FrameSegment::read<native::MatrixLoad>(record);
            std::snprintf(line, sizeof(line), "id %u type %u [%g %g %g %g | %g %g %g %g | %g %g %g %g]",
                          m.id, m.type, m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5], m.m[6],
                          m.m[7], m.m[8], m.m[9], m.m[10], m.m[11]);
            break;
        }
        case Op::Blend: {
            const auto b = FrameSegment::read<native::Blend>(record);
            std::snprintf(line, sizeof(line), "type %u src %u dst %u logic %u", b.type, b.source,
                          b.destination, b.logic);
            break;
        }
        case Op::DepthMode: {
            const auto d = FrameSegment::read<native::DepthMode>(record);
            std::snprintf(line, sizeof(line), "enable %u func %u update %u", d.enable, d.function,
                          d.update);
            break;
        }
        case Op::AlphaCompare: {
            const auto a = FrameSegment::read<native::AlphaCompare>(record);
            std::snprintf(line, sizeof(line), "(%u %u) op %u (%u %u)", a.comp0, a.ref0, a.op,
                          a.comp1, a.ref1);
            break;
        }
        case Op::GlareWindow: {
            const auto g = FrameSegment::read<native::GlareWindow>(record);
            std::snprintf(line, sizeof(line),
                          "%s object 0x%08X gate %u | enable %u A %u B %u sep %u bsrc %u"
                          " intensity %u/%u spread %g/%g taps %d/%d %d/%d",
                          g.phase == native::kGlarePreGlare ? "PRE" : "POST", g.object, g.gate,
                          g.enable, g.enable_a, g.enable_b, g.separate, g.b_from_source,
                          g.intensity_a, g.intensity_b, g.spread_a, g.spread_b, g.taps_ah,
                          g.taps_av, g.taps_bh, g.taps_bv);
            break;
        }
        case Op::Fog: {
            const auto f = FrameSegment::read<native::Fog>(record);
            std::snprintf(line, sizeof(line), "type %u start %g end %g near %g far %g rgba %u %u %u %u",
                          f.type, f.start, f.end, f.near_z, f.far_z, f.rgba[0], f.rgba[1],
                          f.rgba[2], f.rgba[3]);
            break;
        }
        case Op::ChannelControl: {
            const auto c = FrameSegment::read<native::ChannelControl>(record);
            std::snprintf(line, sizeof(line), "chan %u enable %u amb %u mat %u lights 0x%02X diff %u attn %u",
                          c.channel, c.enable, c.ambient_source, c.material_source, c.light_mask,
                          c.diffuse, c.attenuation);
            break;
        }
        case Op::MaterialColor:
        case Op::AmbientColor: {
            const auto c = FrameSegment::read<native::ChannelColor>(record);
            std::snprintf(line, sizeof(line), "chan %u rgba %u %u %u %u", c.channel, c.rgba[0],
                          c.rgba[1], c.rgba[2], c.rgba[3]);
            break;
        }
        case Op::Light: {
            const auto l = FrameSegment::read<native::Light>(record);
            std::snprintf(line, sizeof(line), "mask 0x%02X rgba %u %u %u %u pos %g %g %g dir %g %g %g",
                          l.mask, l.rgba[0], l.rgba[1], l.rgba[2], l.rgba[3], l.position[0],
                          l.position[1], l.position[2], l.direction[0], l.direction[1],
                          l.direction[2]);
            break;
        }
        case Op::TexGen: {
            const auto t = FrameSegment::read<native::TexGen>(record);
            std::snprintf(line, sizeof(line), "coord %u func %u src %u mtx %u norm %u post %u",
                          t.coord, t.function, t.source, t.matrix, t.normalize, t.post_matrix);
            break;
        }
        case Op::TevRegisterColor:
        case Op::TevKonstColor: {
            const auto t = FrameSegment::read<native::TevColor>(record);
            std::snprintf(line, sizeof(line), "index %u rgba %d %d %d %d", t.index, t.rgba[0],
                          t.rgba[1], t.rgba[2], t.rgba[3]);
            break;
        }
        case Op::BindTexture: {
            const auto b = FrameSegment::read<native::BindTexture>(record);
            std::snprintf(line, sizeof(line),
                          "map %u raster 0x%08X gen %u %ux%u wrap %u/%u filter %u/%u flags 0x%X off %d,%d",
                          b.map, b.texture.address, b.texture.generation, b.width, b.height,
                          b.wrap_s, b.wrap_t, b.min_filter, b.mag_filter, b.flags, b.offset_x,
                          b.offset_y);
            break;
        }
        case Op::TextureContent: {
            const auto t = FrameSegment::read<native::TextureContent>(record);
            std::snprintf(line, sizeof(line),
                          "raster 0x%08X gen %u %ux%u fmt %u tlut %u x%u flags 0x%X texels %zu B palette %zu B",
                          t.texture.address, t.texture.generation, t.width, t.height, t.gx_format,
                          t.tlut_format, t.tlut_entries, t.flags, t.texels.size, t.palette.size);
            break;
        }
        case Op::GeometryContent: {
            const auto g = FrameSegment::read<native::GeometryContent>(record);
            std::snprintf(line, sizeof(line), "entry 0x%08X gen %u flags 0x%X sector %u blob %zu B",
                          g.geometry.address, g.geometry.generation, g.geometry_flags,
                          g.world_sector, g.blob.size);
            break;
        }
        case Op::GeometryDynamic: {
            const auto g = FrameSegment::read<native::GeometryDynamic>(record);
            std::snprintf(line, sizeof(line), "entry 0x%08X gen %u verts %u pos %zu B nrm %zu B",
                          g.geometry.address, g.geometry.generation, g.vertex_count,
                          g.positions.size, g.normals.size);
            break;
        }
        case Op::ReleaseTexture:
        case Op::ReleaseGeometry: {
            const auto k = FrameSegment::read<native::ResourceKey>(record);
            std::snprintf(line, sizeof(line), "0x%08X gen %u", k.address, k.generation);
            break;
        }
        case Op::DrawImmediate: {
            const auto d = FrameSegment::read<native::DrawImmediate>(record);
            const u8* at = segment.payload_at(d.vertices);
            shadow::render::RenderVertex first{};
            if (d.vertex_count != 0u) std::memcpy(&first, at, sizeof(first));
            std::snprintf(line, sizeof(line),
                          "producer %u program %u prim %u attrs 0x%X verts %u lr 0x%08X v0 (%g %g %g) rgba %u %u %u %u uv (%g %g)",
                          static_cast<unsigned>(d.producer), static_cast<unsigned>(d.program),
                          d.primitive, d.attributes, d.vertex_count, d.return_address,
                          first.position[0], first.position[1], first.position[2],
                          first.color0[0], first.color0[1], first.color0[2], first.color0[3],
                          first.uv[0][0], first.uv[0][1]);
            break;
        }
        case Op::DrawMesh: {
            const auto d = FrameSegment::read<native::DrawMesh>(record);
            std::snprintf(line, sizeof(line),
                          "producer %u program %u entry 0x%08X gen %u mesh %u object 0x%08X flags 0x%X lr 0x%08X",
                          static_cast<unsigned>(d.producer), static_cast<unsigned>(d.program),
                          d.geometry.address, d.geometry.generation, d.mesh_index, d.object,
                          d.geometry_flags, d.return_address);
            break;
        }
        case Op::Clear: {
            const auto c = FrameSegment::read<native::Clear>(record);
            std::snprintf(line, sizeof(line), "%d %d %d %d rgba %u %u %u %u mode %u", c.x, c.y,
                          c.width, c.height, c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3], c.mode);
            break;
        }
        case Op::CopyTexture: {
            const auto c = FrameSegment::read<native::CopyTexture>(record);
            std::snprintf(line, sizeof(line),
                          "src %d %d %d %d -> raster 0x%08X gen %u (%ux%u) at %d,%d half %u",
                          c.source_x, c.source_y, c.source_width, c.source_height,
                          c.target.address, c.target.generation, c.target_width, c.target_height,
                          c.destination_x, c.destination_y, c.half_resolution);
            break;
        }
        case Op::CameraBegin:
        case Op::CameraEnd: {
            const auto c = FrameSegment::read<native::Camera>(record);
            std::snprintf(line, sizeof(line), "camera 0x%08X fb 0x%08X %d,%d %ux%u type %u",
                          c.camera, c.frame_buffer, c.offset_x, c.offset_y, c.width, c.height,
                          c.raster_type);
            break;
        }
        case Op::Present: {
            const auto p = FrameSegment::read<native::Present>(record);
            std::snprintf(line, sizeof(line), "frame %u xfb 0x%08X %ux%u efb h %u game ticks %llu",
                          p.frame, p.xfb, p.xfb_width, p.xfb_height, p.efb_height,
                          static_cast<unsigned long long>(p.game_ticks));
            break;
        }
        default: {
            const auto s = FrameSegment::read<native::Scalar>(record);
            std::snprintf(line, sizeof(line), "%u %u", s.value, s.value2);
            break;
        }
    }
    out << header.sequence << " " << native::op_name(header.op) << " " << line << "\n";
}

}  // namespace

struct NativeRenderer::Impl {
    NativeRendererConfig config;
    std::unique_ptr<native::SegmentQueue> queue;
    std::unique_ptr<NativeGpu> gpu;
    std::string error;
    std::thread thread;
    std::atomic<bool> running{false};
    mutable std::mutex report_mutex;
    Report report;
    std::exception_ptr failure;
    std::atomic<bool> has_failure{false};
    u32 frame = 0;
    // Edge state for the mailbox-overflow message below. It exists only to turn
    // a cumulative counter into one event per NEW loss, and it must not exist in
    // the shipped binary at all, so the member itself is conditional.
    GEKKO_PROFILE_ONLY(u64 last_mailbox_overflow = 0;)

    void process(const native::FrameSegment& segment) {
        // The pair bounds this thread's busy period so it can be read against
        // the present thread's frame set. It is named for the SEGMENT, not the
        // frame: the two are 1:1 only while close_segment() is the sole caller
        // and always presents - report.frames below counts ends_frame() instead.
        GEKKO_PROFILE_FRAME_START("gpu/render segment");
        const auto begin = std::chrono::steady_clock::now();
        GEKKO_PROFILE_ZONE("gpu/segment process");
        GEKKO_PROFILE_VALUE(segment.command_count());
        const bool dump = !config.dump_directory.empty() && frame + 1u >= config.dump_first_frame &&
                          frame + 1u <= config.dump_last_frame;
        std::ofstream out;
        if (dump) {
            char name[64];
            std::snprintf(name, sizeof(name), "segment_%06u.txt", frame + 1u);
            out.open(config.dump_directory / name, std::ios::app);
        }
        if (dump && out) {
            // The whole walk, never the lambda: at ~1,632 commands per segment a
            // per-command zone would sit at the event-rate ceiling and would be
            // measuring snprintf.
            GEKKO_PROFILE_ZONE("gpu/segment dump");
            segment.for_each([&](const native::CommandHeader& header, const u8* record) {
                dump_command(out, segment, header, record);
            });
        }
        if (gpu != nullptr) gpu->process(segment);
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - begin)
                                            .count());
        // Explicitly scoped so nothing below runs while report_mutex is held.
        // The guard used to run to the end of the function; in the shipped
        // build everything after this brace compiles away, so the release point
        // is where it always was.
        {
            std::lock_guard lock(report_mutex);
            ++report.segments;
            report.commands += segment.command_count();
            report.payload_bytes += segment.payload().size();
            report.process_ns += ns;
            report.process_ns_max = std::max(report.process_ns_max, ns);
            if (dump) ++report.dumped_frames;
            if (segment.ends_frame()) {
                ++frame;
                ++report.frames;
            }
        }

        // The report keeps only session totals, so a segment carrying five
        // times the usual work is indistinguishable from an average one. These
        // four are O(1) reads of the segment already in hand; every expression
        // vanishes with the macro in the shipped build.
        GEKKO_PROFILE_PLOT("gpu/segment commands", static_cast<double>(segment.command_count()));
        GEKKO_PROFILE_PLOT("gpu/segment payload", static_cast<double>(segment.payload().size()));
        // size() is this frame's traffic; capacity() is residency. FrameSegment
        // reserves 64 KiB + 1 MiB and clear() never gives capacity back, so a
        // pooled segment holds its high-water arena for the whole session.
        GEKKO_PROFILE_PLOT(
            "gpu/segment arena bytes",
            static_cast<double>(segment.commands().capacity() + segment.payload().capacity()));
        // How STALE the work in flight is: close_ns is stamped by the capture
        // from the same steady_clock epoch as `begin`, so this is the time the
        // segment spent queued. Queue depth says how many are waiting; this
        // says how old the one being drawn is.
        GEKKO_PROFILE_PLOT("gpu/segment age ns",
                           static_cast<double>(static_cast<s64>(begin.time_since_epoch().count()) -
                                               static_cast<s64>(segment.close_ns())));

        // Everything below asks another object for a snapshot, and each of
        // those snapshots takes a lock some other thread also uses (the mailbox
        // mutex the guest drains at ~10.5k/s, SegmentQueue::mutex_, the GPU
        // stats mutex plus a D3D12MA budget query, the memory system mutex).
        // None of it may cost the shipped binary anything, and none of it may
        // run when nobody is watching - hence both guards.
        GEKKO_PROFILE_ONLY(if (GEKKO_PROFILE_CONNECTED()) {
            // One snapshot for all four mailbox sites: stats() returns the four
            // counters under a single lock, so pending derives exactly.
            const auto mailbox = queue->rejections().stats();
            GEKKO_PROFILE_PLOT("gpu/mailbox pending",
                               static_cast<double>(mailbox.posted - mailbox.drained -
                                                   mailbox.coalesced - mailbox.overflow));
            GEKKO_PROFILE_PLOT("gpu/rejections posted", static_cast<double>(mailbox.posted));
            // Any slope here means the mailbox is saturated and the next
            // DISTINCT resource will be lost outright.
            GEKKO_PROFILE_PLOT("gpu/rejections coalesced", static_cast<double>(mailbox.coalesced));
            // Edge-triggered: each increment is a rejection DROPPED, which is an
            // object that cannot recover for the rest of the stage.
            if (mailbox.overflow != last_mailbox_overflow) {
                last_mailbox_overflow = mailbox.overflow;
                GEKKO_PROFILE_MESSAGE_LITERAL("gpu/mailbox overflow: a rejection was LOST");
            }
            // Flat means the segment pool is doing its job; a step is a new
            // segment and roughly another MiB of arena held for the session.
            const auto q = queue->stats();
            GEKKO_PROFILE_PLOT("gpu/segments allocated",
                               static_cast<double>(q.segments_allocated));
            if (gpu != nullptr) {
                const NativeGpu::Stats g = gpu->stats();
                GEKKO_PROFILE_PLOT("gpu/vram usage", static_cast<double>(g.vram_usage_bytes));
                // Usage alone cannot say whether the device is near eviction.
                GEKKO_PROFILE_PLOT("gpu/vram budget", static_cast<double>(g.vram_budget_bytes));
                GEKKO_PROFILE_PLOT("gpu/textures live", static_cast<double>(g.textures_live));
                GEKKO_PROFILE_PLOT("gpu/texture bytes", static_cast<double>(g.texture_bytes));
                // Read next to gpu/rejections posted: did refused objects come
                // back, or did live geometry step down and stay down?
                GEKKO_PROFILE_PLOT("gpu/geometry live", static_cast<double>(g.geometry_live));
                GEKKO_PROFILE_PLOT("gpu/copy queue bytes", static_cast<double>(g.copy_queue_bytes));
                GEKKO_PROFILE_PLOT("gpu/copy queue batch waits",
                                   static_cast<double>(g.copy_queue_batch_waits));
                GEKKO_PROFILE_PLOT("gpu/copy queue refused",
                                   static_cast<double>(g.copy_queue_refused_oversize +
                                                       g.copy_queue_refused_arguments));
            }
            // FrameSegment's arenas are Domain::Renderer vectors, so the domain
            // is this subsystem's exact host residency - the whole pool, not
            // just the segment gpu/segment arena bytes happens to sample.
            const auto host = shadow::memory::system().snapshot();
            const auto& renderer_domain =
                host.domains[static_cast<std::size_t>(shadow::memory::Domain::Renderer)];
            GEKKO_PROFILE_PLOT("gpu/renderer host memory",
                               static_cast<double>(renderer_domain.active));
            // A host allocation the renderer domain was REFUSED. Normally flat
            // at zero, which is the point; it reaches no report today.
            GEKKO_PROFILE_PLOT("gpu/renderer host memory failures",
                               static_cast<double>(renderer_domain.failures));
        })

        GEKKO_PROFILE_FRAME_END("gpu/render segment");
    }

    void record_failure(std::exception_ptr caught) {
        std::exception_ptr first;
        bool newly_failed = false;
        {
            std::lock_guard lock(report_mutex);
            if (!failure) {
                failure = caught;
                newly_failed = true;
            }
            first = failure;
            has_failure.store(true, std::memory_order_release);
        }
        native::publishFailureBeforeDiagnostics([&] { queue->fail(first); }, [&] {
            if (!newly_failed) return;
            const auto diagnose = [&](const char* message) {
                std::fprintf(stderr, "[native-renderer] terminal failure: %s\n", message);
                std::fflush(stderr);
                std::lock_guard lock(report_mutex);
                error = message;
            };
            try { std::rethrow_exception(first); }
            catch (const std::exception& e) { diagnose(e.what()); }
            catch (...) { diagnose("unknown native renderer failure"); }
        });
    }

    void run() {
        GEKKO_PROFILE_THREAD("native-render");
        gekko::name_this_thread("legacy-native-render");  // ES-T1
        try {
        while (true) {
            auto segment = queue->pop(50);
            if (!segment) {
                if (!running.load(std::memory_order_relaxed) || queue->closed()) {
                    // Drain whatever arrived before the close.
                    while (auto rest = queue->pop(0)) {
                        process(*rest);
                        queue->recycle(std::move(rest));
                    }
                    return;
                }
                continue;
            }
            process(*segment);
            queue->recycle(std::move(segment));
        }
        } catch (...) {
            record_failure(std::current_exception());
        }
    }
};

NativeRenderer::NativeRenderer() : impl_(std::make_unique<Impl>()) {
    impl_->queue = std::make_unique<native::SegmentQueue>(2);
}

NativeRenderer::~NativeRenderer() { stop(); }

bool NativeRenderer::start(const NativeRendererConfig& config) {
    {
        std::lock_guard lock(impl_->report_mutex);
        if (impl_->failure) return false; // terminal owner, no implicit device restart
    }
    if (impl_->running.load()) return true;
    impl_->config = config;
    impl_->queue = std::make_unique<native::SegmentQueue>(config.frames_in_flight);
    // Every quantity this subsystem publishes is a step function. Without
    // step=true Tracy interpolates between samples and a reader infers queue
    // depths and resource counts that never existed.
    //
    // NOT behind GEKKO_PROFILE_CONNECTED(): under on-demand capture nothing is
    // connected at startup, and ConfigurePlot defers itself so a configuration
    // emitted now survives a later attach. A CONNECTED() guard here would
    // silence the configuration permanently.
    GEKKO_PROFILE_PLOT_CONFIG("gpu/segment commands", GEKKO_PLOT_NUMBER, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/segment payload", GEKKO_PLOT_MEMORY, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/segment arena bytes", GEKKO_PLOT_MEMORY, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/segment age ns", GEKKO_PLOT_NUMBER, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/segment queue depth", GEKKO_PLOT_NUMBER, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/producer wait ns", GEKKO_PLOT_NUMBER, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/segments allocated", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/mailbox pending", GEKKO_PLOT_NUMBER, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/rejections posted", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/rejections coalesced", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/vram usage", GEKKO_PLOT_MEMORY, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/vram budget", GEKKO_PLOT_MEMORY, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/textures live", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/texture bytes", GEKKO_PLOT_MEMORY, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/geometry live", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/device resources tracked", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/device upload bytes", GEKKO_PLOT_MEMORY, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/device upload stalls", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/device upload refused", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/renderer host memory", GEKKO_PLOT_MEMORY, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/renderer host memory failures", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/hud laid out", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/hud off canvas", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/hud group memory", GEKKO_PLOT_NUMBER, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/hud cast memory", GEKKO_PLOT_NUMBER, true, true, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/hud group memory capped", GEKKO_PLOT_NUMBER, true, false, 0);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/hud groups open at frame end", GEKKO_PLOT_NUMBER, true, false, 0);
    // The depth plot has no reference line otherwise: its cap is whatever the
    // queue was built with, so "depth 2" only means the cap was TOUCHED if the
    // reader knows the cap is 2.
    GEKKO_PROFILE_ONLY({
        char info[96];
        const int written = std::snprintf(info, sizeof(info),
                                          "native renderer: frames in flight %u",
                                          static_cast<unsigned>(config.frames_in_flight));
        if (written > 0) GEKKO_PROFILE_APP_INFO(info, static_cast<std::size_t>(written));
    })
    if (config.gpu) {
        // Device, swapchain and the first pipelines: hundreds of ms of one-time
        // cost that otherwise shows up as an unexplained gap before frame one.
        GEKKO_PROFILE_ZONE("gpu/renderer create");
        impl_->gpu = std::make_unique<NativeGpu>();
        if (!impl_->gpu->create(config.gpu_settings, impl_->error)) {
            impl_->gpu.reset();
            return false;
        }
        // Close the resource contract: the renderer can now tell the capture
        // what it refused. The mailbox lives on the queue because that is the
        // one object both sides already hold.
        impl_->gpu->set_rejections(&impl_->queue->rejections());
    }
    if (!config.dump_directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(config.dump_directory, error);
    }
    impl_->running.store(true);
    impl_->thread = std::thread([this] { impl_->run(); });
    return true;
}

void NativeRenderer::stop() {
    if (!impl_->running.exchange(false)) return;
    // Successful join drains every segment; terminal worker failure instead
    // retains GPU ownership and wakes the producer with that error. Shutdown is
    // then an explicit final retirement attempt, so a hitch lands here and is
    // invisible without a zone.
    GEKKO_PROFILE_ZONE("gpu/renderer stop");
    impl_->queue->close();
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->gpu != nullptr) {
        try { impl_->gpu->finish(); }
        catch (...) { impl_->record_failure(std::current_exception()); }
    }
}

std::string NativeRenderer::last_error() const {
    std::lock_guard lock(impl_->report_mutex);
    return impl_->error;
}

bool NativeRenderer::failed() const noexcept {
    return impl_->has_failure.load(std::memory_order_acquire);
}
NativeGpu* NativeRenderer::gpu() noexcept { return impl_->gpu.get(); }

native::SegmentQueue& NativeRenderer::queue() { return *impl_->queue; }

NativeRenderer::Report NativeRenderer::report() const {
    std::lock_guard lock(impl_->report_mutex);
    Report report = impl_->report;
    report.queue = impl_->queue->stats();
    return report;
}

std::string NativeRenderer::report_text() const {
    const Report r = report();
    std::ostringstream out;
    const auto error = last_error();
    if (!error.empty()) out << "[native-renderer] terminal error: " << error << "\n";
    out << "[native-renderer] segments " << r.segments << " frames " << r.frames << " commands "
        << r.commands << " payload " << r.payload_bytes << " B dumped " << r.dumped_frames
        << " process " << static_cast<double>(r.process_ns) / 1e6 << " ms (max "
        << static_cast<double>(r.process_ns_max) / 1e6 << " ms)\n";
    out << "[native-renderer] queue pushed " << r.queue.pushed << " popped " << r.queue.popped
        << " producer waits " << r.queue.producer_waits << " ("
        << static_cast<double>(r.queue.producer_wait_ns) / 1e6 << " ms, max "
        << static_cast<double>(r.queue.producer_wait_ns_max) / 1e6 << " ms) depth max "
        << r.queue.depth_max << " segments allocated " << r.queue.segments_allocated << "\n";
    if (impl_->gpu != nullptr) {
        const NativeGpu::Stats g = impl_->gpu->stats();
        out << "[native-gpu] frames " << g.frames << " draws " << g.draws << " (immediate "
            << g.immediate_draws << ", mesh " << g.mesh_draws << ", skipped " << g.skipped_draws
            << " [cull-all " << g.skipped_cull_all << ", refused pipeline " << g.skipped_null_pipeline
            << "]"
            << ", unsupported program " << g.unsupported_program_draws << ") clears " << g.clears
            << " copies " << g.copies << " half-res copies unsupported "
            << g.half_resolution_copies_unsupported << " lines " << g.line_draws << " points "
            << g.point_draws << "\n";
        // Health, not cosmetics: a non-zero count here means the renderer ran
        // degraded - a fence that never retired (the slot is still in flight
        // and its resources were NOT recycled) or a render target the device
        // refused. Either one precedes the crashes this instrumentation exists
        // to make visible, so it is printed unconditionally.
        // `already complete` used to be one clobbered field and printed a
        // number larger than the wait total, which is impossible for a subset
        // of those waits. It was two unrelated quantities sharing storage.
        out << "[native-gpu] fence waits " << g.fence_waits << " (slot already complete "
            << g.slot_fence_already_complete << ", FAILED " << g.fence_wait_failures
            << ") target allocation failures " << g.target_allocation_failures << "\n";
        out << "[native-gpu] transfer queue: submits " << g.copy_queue_submits << " (early "
            << g.copy_queue_early_submits << ") uploads " << g.copy_queue_uploads << " ("
            << static_cast<double>(g.copy_queue_bytes) / 1048576.0 << " MiB, staging "
            << static_cast<double>(g.copy_queue_staging_bytes) / 1048576.0
            << " MiB) retained " << static_cast<double>(g.copy_queue_retained_bytes) / 1048576.0
            << " MiB peak-batch " << static_cast<double>(g.copy_queue_peak_batch_bytes) / 1048576.0
            << " MiB largest-request "
            << static_cast<double>(g.copy_queue_largest_request_bytes) / 1048576.0
            << " MiB growths " << g.copy_queue_growths << " allocation-failures "
            << g.copy_queue_allocation_failures << " current "
            << static_cast<double>(g.copy_queue_current_bytes) / 1048576.0
            << " MiB resets " << g.copy_queue_resets << " chunk-switches "
            << g.copy_queue_chunk_switches << " batch reuse waits "
            << g.copy_queue_batch_waits << " ("
            << static_cast<double>(g.copy_queue_batch_wait_ns) / 1e6 << " ms) refused oversize "
            << g.copy_queue_refused_oversize
            << " zero-byte " << g.copy_queue_refused_arguments << " semaphores "
            << g.copy_queue_semaphores << " (failures " << g.copy_queue_semaphore_failures << ")\n";
        out << "[native-gpu] transfer direct-fill " << g.copy_queue_generated_uploads << "/"
            << static_cast<double>(g.copy_queue_generated_bytes) / 1048576.0
            << " MiB (generated directly in mapped staging)\n";
        {
            static constexpr const char* names[4] = {"texture", "geometry-vertex",
                                                     "geometry-index", "immediate-index"};
            out << "[native-gpu] transfer owners:";
            for (std::size_t index = 0; index < 4u; ++index) {
                out << ' ' << names[index] << '=' << g.copy_queue_owner_uploads[index] << '/'
                    << static_cast<double>(g.copy_queue_owner_bytes[index]) / 1048576.0 << '/'
                    << static_cast<double>(g.copy_queue_owner_staging_bytes[index]) / 1048576.0
                    << "MiB";
            }
            out << " (uploads/payload/staging)\n";
        }
        out << "[native-gpu] geometry store: assets created " << g.geometry_created
            << " instance hits " << g.geometry_asset_hits << " resident "
            << g.geometry_assets_resident << " (unreferenced " << g.geometry_assets_unreferenced
            << ", " << static_cast<double>(g.geometry_asset_bytes) / 1048576.0 << " MiB GPU, "
            << static_cast<double>(g.geometry_source_bytes) / 1048576.0
            << " MiB identity source) evicted " << g.geometry_assets_evicted
            << " IDENTITY CONFLICTS " << g.geometry_identity_conflicts << "\n";
        if (g.present_output_texture_failures != 0u || g.present_output_semaphore_failures != 0u ||
            g.efb_resize_refused != 0u) {
            out << "[native-gpu] PRESENT PATH REFUSALS: output texture "
                << g.present_output_texture_failures << " semaphore "
                << g.present_output_semaphore_failures << " EFB resize refused "
                << g.efb_resize_refused << "\n";
        }
        if (g.glare_windows != 0u) {
            const native::GlareWindow& p = g.glare_parameters;
            char line[320];
            std::snprintf(line, sizeof(line),
                          "[native-gpu] glare windows %llu (gate open %llu, parameter changes %llu)"
                          "\n[native-gpu] glare parameters: enable %u A %u B %u separate %u"
                          " b-from-source %u intensity %u/%u spread %.4f/%.4f taps %d/%d %d/%d\n",
                          static_cast<unsigned long long>(g.glare_windows),
                          static_cast<unsigned long long>(g.glare_windows_gated_on),
                          static_cast<unsigned long long>(g.glare_parameter_changes),
                          p.enable, p.enable_a, p.enable_b, p.separate, p.b_from_source,
                          p.intensity_a, p.intensity_b, p.spread_a, p.spread_b,
                          p.taps_ah, p.taps_av, p.taps_bh, p.taps_bv);
            out << line;
        }
        out << "[native-gpu] guest-texel footprint draws " << g.footprint_draws
            << "; raster-border snapped draws " << g.border_snap_draws << " (refused "
            << g.border_snap_refused << ")\n";
        out << impl_->gpu->target_inventory();
        out << "[native-gpu] missing textures " << g.missing_textures << " missing geometry "
            << g.missing_geometry << " decode failures " << g.texture_decode_failures
            << " convert failures " << g.geometry_convert_failures << " foreign DL commands "
            << g.geometry_foreign_commands << " logic-op draws " << g.logic_op_draws
            << " early-z+alpha-test " << g.depth_before_texture_with_alpha_test << "\n";
        // A disappeared element is a correctness failure, so it is attributed
        // rather than totalled. These five sum to missing geometry.
        if (g.missing_geometry != 0u || g.geometry_drops_seen != 0u) {
            out << "[native-gpu] DROPPED DRAWS by cause: unknown " << g.missing_geometry_unknown
                << " generation " << g.missing_geometry_generation
                << " mesh-index " << g.missing_geometry_mesh_index
                << " dynamic-stale " << g.missing_geometry_dynamic_stale << "\n";
            out << "[native-gpu] DROPPED SKINNED UPDATES by cause: unknown "
                << g.dynamic_drop_unknown << " no-cpu " << g.dynamic_drop_no_cpu
                << " expand " << g.dynamic_drop_expand << " staging " << g.dynamic_drop_staging
                << "\n";
            out << "[native-gpu] first " << std::min<gekko::u64>(g.geometry_drops_seen,
                                                                 g.kGeometryDropRecords)
                << " of " << g.geometry_drops_seen << " drops:\n";
            for (unsigned i = 0; i < g.kGeometryDropRecords && i < g.geometry_drops_seen; ++i) {
                const auto& d = g.geometry_drops[i];
                if (d.cause == nullptr) break;
                char row[160];
                std::snprintf(row, sizeof(row),
                              "    frame %-6u %-20s object 0x%08X gen wanted %u held %u detail %u\n",
                              d.frame, d.cause, d.address, d.wanted, d.held, d.detail);
                out << row;
            }
        }
        // THE SILENT LANE. Every one of these was a bare `return` on a path
        // that had already decided a renderable element existed. Printed
        // UNCONDITIONALLY and with an explicit statement when the total is
        // zero, because a counter that always reads zero without saying why is
        // exactly the failure mode this instrumentation exists to end.
        {
            const gekko::u64 immediate_lost =
                g.immediate_drop_zero_count + g.immediate_drop_degenerate +
                g.immediate_drop_no_pipeline + g.immediate_drop_vertex_ring +
                g.immediate_drop_index_ring;
            const gekko::u64 mesh_lost = g.mesh_drop_zero_indices + g.mesh_drop_no_pipeline;
            const gekko::u64 content_lost = g.geometry_drop_buffer_create +
                                            g.geometry_drop_vertex_staging +
                                            g.geometry_drop_index_staging;
            const gekko::u64 total = immediate_lost + mesh_lost + content_lost;
            if (total == 0u) {
                out << "[native-gpu] SILENT DROPS: none on this route (immediate, mesh and "
                       "geometry-content lanes all zero)\n";
            } else {
                out << "[native-gpu] SILENT DROPS " << total << ": immediate " << immediate_lost
                    << " (zero-count " << g.immediate_drop_zero_count << ", degenerate "
                    << g.immediate_drop_degenerate << ", no-pipeline " << g.immediate_drop_no_pipeline
                    << ", vertex-ring " << g.immediate_drop_vertex_ring << ", index-ring "
                    << g.immediate_drop_index_ring << ") mesh " << mesh_lost << " (zero-indices "
                    << g.mesh_drop_zero_indices << ", no-pipeline " << g.mesh_drop_no_pipeline
                    << ") content " << content_lost << " (buffer-create "
                    << g.geometry_drop_buffer_create << ", vertex-staging "
                    << g.geometry_drop_vertex_staging << ", index-staging "
                    << g.geometry_drop_index_staging << ")\n";
            }
        }
        // WHICH OBJECTS, over WHICH FRAME SPAN. A permanent loss is a few
        // addresses whose last frame is the final frame; a transient is many
        // addresses with small counts. The 24-entry prefix ring above cannot
        // tell those apart, and that ambiguity cost a whole investigation.
        if (g.geometry_drop_objects_used != 0u) {
            out << "[native-gpu] DROPPED OBJECTS " << g.geometry_drop_objects_used
                << " distinct (final frame " << g.frames << ")";
            if (g.geometry_drop_objects_overflow != 0u) {
                out << " + " << g.geometry_drop_objects_overflow
                    << " NOT RECORDED (table full)";
            }
            out << ":\n";
            for (gekko::u32 i = 0; i < g.geometry_drop_objects_used; ++i) {
                const auto& o = g.geometry_drop_objects[i];
                char row[192];
                std::snprintf(row, sizeof(row),
                              "    object 0x%08X  x%-7llu frames %u..%-6u  %-20s -> %-20s"
                              "  gen wanted %u held %u\n",
                              o.address, static_cast<unsigned long long>(o.count), o.first_frame,
                              o.last_frame, o.first_cause != nullptr ? o.first_cause : "?",
                              o.last_cause != nullptr ? o.last_cause : "?", o.wanted, o.held);
                out << row;
            }
        }
        // THE RESOURCE CONTRACT. Printed unconditionally: "the renderer never
        // refused anything" and "the renderer refused things and could not say
        // so" used to be the same output, which is what let one refusal delete
        // an object for a whole session.
        if (g.rejections_posted == 0u) {
            out << "[native-gpu] RESOURCE REJECTIONS: none (every geometry and texture the "
                   "capture sent was accepted)\n";
        } else {
            out << "[native-gpu] RESOURCE REJECTIONS " << g.rejections_posted << ": geometry "
                << g.rejections_geometry << " texture " << g.rejections_texture;
            if (g.rejections_undeliverable != 0u) {
                out << " -- " << g.rejections_undeliverable
                    << " UNDELIVERABLE (no mailbox wired; those objects cannot recover)";
            }
            out << "\n";
        }
        if (g.state_ops_unconsumed != 0u) {
            out << "[native-gpu] GX state records emitted and consumed by nothing: "
                << g.state_ops_unconsumed << " (first op id " << g.state_op_unconsumed_first
                << "). Op::DestinationAlpha and Op::TevKonstColor have no handler; "
                   "Op::TevRegisterColor does.\n";
        }
        if (g.publish_latency_samples != 0u) {
            const auto percentile = [&](double p) {
                const u64 rank = static_cast<u64>(p * static_cast<double>(g.publish_latency_samples - 1u));
                u64 seen = 0;
                for (std::size_t bucket = 0; bucket < g.publish_latency_histogram.size(); ++bucket) {
                    seen += g.publish_latency_histogram[bucket];
                    if (seen > rank) return 0.5 * static_cast<double>(bucket + 1u);
                }
                return 32.0;
            };
            out << "[native-gpu] guest present -> output published: mean "
                << static_cast<double>(g.publish_latency_ns) / static_cast<double>(g.publish_latency_samples) / 1e6
                << " ms, p50 <= " << percentile(0.50) << " ms, p99 <= " << percentile(0.99) << " ms, max "
                << static_cast<double>(g.publish_latency_ns_max) / 1e6 << " ms\n";
        }
        out << "[native-gpu] hud laid out " << g.hud_laid_out << " remapped " << g.hud_remapped
            << " off canvas " << g.hud_off_canvas << " skipped (before last copy "
            << g.hud_skip_before_copy << ", effect texture " << g.hud_skip_effect_texture
            << ", small scissor " << g.hud_skip_small_scissor << ") groups " << g.hud_groups
            << " scenes " << g.hud_scenes << " output " << g.output_width << "x"
            << g.output_height << " resizes " << g.output_resizes << "\n";
        out << "[native-gpu] output pool slots " << g.output_slots << " created "
            << g.output_slots_created << " pins " << g.output_pins << " (observed peak "
            << g.output_pins_peak << ") retained "
            << static_cast<double>(g.output_pool_bytes) / 1048576.0 << " MiB\n";
        // pipeline_create_failures is the ONLY honest measurement of a refused
        // PSO on this backend: plume returns a live wrapper around a null
        // handle, so a failure is otherwise indistinguishable from a success
        // and every later draw of that key is booked as a cache hit.
        out << "[native-gpu] pipelines " << g.pipelines << " ("
            << static_cast<double>(g.pipeline_create_ns) / 1e6 << " ms, CREATE FAILURES "
            << g.pipeline_create_failures << ") textures live "
            << g.textures_live << " created " << g.textures_created << " ("
            << static_cast<double>(g.texture_bytes) / 1048576.0 << " MiB decoded) camera targets "
            << g.camera_targets << " geometry live " << g.geometry_live << " created "
            << g.geometry_created << " dynamic updates " << g.dynamic_updates
            << " bindless high water " << g.bindless_high_water << "\n";
        out << "[native-gpu] constants uploaded " << g.constant_uploads << " reused "
            << g.constant_reuses << " (identity " << g.constant_identity_reuses
            << ", rebuilt-identical " << g.constant_rebuild_reuses << ", materialized "
            << g.constant_materializations << ") upload "
            << static_cast<double>(g.upload_bytes) / 1048576.0
            << " MiB ring retained " << static_cast<double>(g.ring_retained_bytes) / 1048576.0
            << " MiB peak " << static_cast<double>(g.ring_peak_bytes) / 1048576.0
            << " MiB largest-request "
            << static_cast<double>(g.ring_largest_request_bytes) / 1048576.0
            << " MiB growths " << g.ring_growths << " allocation-failures "
            << g.ring_allocation_failures << " current "
            << static_cast<double>(g.ring_current_bytes) / 1048576.0
            << " MiB resets " << g.ring_resets << " chunk-switches "
            << g.ring_chunk_switches << " ring splits " << g.ring_flushes
            << " ring bound failures "
            << g.ring_bound_failures << " deferred releases " << g.deferred_releases
            << " fence waits " << g.fence_waits << " ("
            << static_cast<double>(g.fence_wait_ns) / 1e6 << " ms, already complete "
            << g.fence_already_complete << ")\n";
        out << "[native-gpu] rebuilt-identical observed state ops: no-state="
            << g.constant_rebuild_no_state;
        for (u32 op = 0; op < static_cast<u32>(native::Op::Count); ++op) {
            if (g.constant_rebuild_dirty_ops[op] == 0u) continue;
            out << ' ' << native::op_name(static_cast<native::Op>(op)) << '='
                << g.constant_rebuild_dirty_ops[op];
        }
        out << " (seen ops may not change generation)\n";
        out << "[native-gpu] constant generation changes:";
        for (u32 op = 0; op < static_cast<u32>(native::Op::Count); ++op) {
            if (g.constant_generation_changes[op] == 0u) continue;
            out << ' ' << native::op_name(static_cast<native::Op>(op)) << '='
                << g.constant_generation_changes[op];
        }
        out << "\n[native-gpu] rebuilt-identical changed state ops: no-changed-state="
            << g.constant_rebuild_no_changed_state << " non-state-input-change="
            << g.constant_rebuild_non_state_changed;
        for (u32 op = 0; op < static_cast<u32>(native::Op::Count); ++op) {
            if (g.constant_rebuild_changed_ops[op] == 0u) continue;
            out << ' ' << native::op_name(static_cast<native::Op>(op)) << '='
                << g.constant_rebuild_changed_ops[op];
        }
        out << " (changed owners may co-occur)\n";
        out << "[native-gpu] rebuilt-identical exclusive generation owner:";
        for (u32 op = 0; op < static_cast<u32>(native::Op::Count); ++op) {
            if (g.constant_rebuild_single_changed_ops[op] == 0u) continue;
            out << ' ' << native::op_name(static_cast<native::Op>(op)) << '='
                << g.constant_rebuild_single_changed_ops[op];
        }
        out << "\n[native-gpu] rebuilt-identical projection source restored="
            << g.constant_rebuild_projection_source_restored << " distinct="
            << g.constant_rebuild_projection_source_distinct
            << "; fog source restored=" << g.constant_rebuild_fog_source_restored
            << " distinct=" << g.constant_rebuild_fog_source_distinct << "\n";
        out << "[native-gpu] restored constant sources reused="
            << g.constant_restored_reuses << " byte-checks=" << g.constant_restore_checks
            << " mismatches=" << g.constant_restore_mismatches << "\n";
        out << "[native-gpu] CPU staging copies avoided "
            << static_cast<double>(g.cpu_staging_copy_bytes_avoided) / 1048576.0
            << " MiB; dynamic decode scratch avoided "
            << static_cast<double>(g.cpu_dynamic_scratch_bytes_avoided) / 1048576.0
            << " MiB; immediate index copies avoided "
            << static_cast<double>(g.cpu_index_staging_copy_bytes_avoided) / 1048576.0
            << " MiB; static geometry copies avoided "
            << static_cast<double>(g.cpu_static_geometry_copy_bytes_avoided) / 1048576.0
            << " MiB (generated directly in fence-owned upload memory)\n";
        out << "[native-gpu] native-linear texture payloads "
            << g.native_linear_texture_payloads << '/'
            << static_cast<double>(g.native_linear_texture_bytes) / 1048576.0
            << " MiB; intermediate CPU scratch copies "
            << g.native_linear_texture_scratch_copies << '/'
            << static_cast<double>(g.native_linear_texture_scratch_bytes) / 1048576.0
            << " MiB; immutable payload direct sources "
            << g.native_linear_texture_direct_payloads << '/'
            << static_cast<double>(g.native_linear_texture_direct_bytes) / 1048576.0
            << " MiB\n";
        out << "[native-gpu] immediate topology (draws/indexed/index-MiB/max-vertices):";
        for (std::size_t primitive = 0; primitive < g.immediate_primitive_draws.size();
             ++primitive) {
            if (g.immediate_primitive_draws[primitive] == 0u) continue;
            out << ' ' << primitive << '=' << g.immediate_primitive_draws[primitive] << '/'
                << g.immediate_indexed_draws[primitive] << '/'
                << static_cast<double>(g.immediate_index_bytes[primitive]) / 1048576.0 << '/'
                << g.immediate_max_vertices[primitive];
        }
        out << "; resident-index hits " << g.immediate_index_pattern_hits << " growths "
            << g.immediate_index_pattern_growths << " retained "
            << static_cast<double>(g.immediate_index_pattern_bytes) / 1048576.0
            << " MiB upload/generation avoided "
            << static_cast<double>(g.immediate_index_upload_bytes_avoided) / 1048576.0 << '/'
            << static_cast<double>(g.immediate_index_generation_bytes_avoided) / 1048576.0
            << " MiB failures " << g.immediate_index_pattern_create_failures << '/'
            << g.immediate_index_pattern_upload_failures << "\n";
        {
            static constexpr const char* names[] = {
                "texture", "geometry-vertex", "geometry-index", "dynamic-vertex",
                "light-constants", "draw-constants", "immediate-vertex",
                "immediate-index"};
            out << "[native-gpu] graphics upload owners:";
            for (std::size_t index = 0; index < std::size(names); ++index) {
                out << ' ' << names[index] << '=' << g.ring_owner_allocations[index] << '/'
                    << static_cast<double>(g.ring_owner_bytes[index]) / 1048576.0 << "MiB";
            }
            out << " (allocations/payload)\n";
        }
        out << "[native-gpu] copy queue submits " << g.copy_queue_submits << " uploads "
            << g.copy_queue_uploads << " (" << static_cast<double>(g.copy_queue_bytes) / 1048576.0
            << " MiB) graphics waits on copy " << g.graphics_waits_on_copy << " barriers "
            << g.barriers << " (requested " << g.barrier_requests << ") pso hits " << g.pso_hits
            << " misses " << g.pso_misses << "\n";
        const double gpu_ms = g.gpu_frames_timed != 0u
                                  ? static_cast<double>(g.gpu_frame_ns) / 1e6 /
                                        static_cast<double>(g.gpu_frames_timed)
                                  : 0.0;
        out << "[native-gpu] record " << static_cast<double>(g.record_ns) / 1e6 << " ms total (max "
            << static_cast<double>(g.record_ns_max) / 1e6 << " ms/segment); GPU " << gpu_ms
            << " ms/frame mean over " << g.gpu_frames_timed << " (max "
            << static_cast<double>(g.gpu_frame_ns_max) / 1e6 << " ms); VRAM usage "
            << static_cast<double>(g.vram_usage_bytes) / 1048576.0 << " MiB of "
            << static_cast<double>(g.vram_budget_bytes) / 1048576.0 << " MiB budget; screenshots "
            << g.screenshots << "\n";
        if (g.cpu_profile_enabled) {
            static constexpr std::array<const char*, static_cast<std::size_t>(NativeGpu::CpuPhase::Count)>
                phase_names{"state-apply", "prepare-draw", "constants-materialize-upload",
                            "projection-policy", "pipeline-lookup-create", "texture-descriptor-lookup",
                            "immediate-draw", "immediate-layout-snap", "immediate-pack-upload",
                            "mesh-draw", "geometry-lookup-validate", "geometry-convert-upload",
                            "geometry-dynamic", "vertex-index-bind", "texture-upload", "copy",
                            "present", "list-begin", "list-submit", "fence-slot-wait", "resource-upload-flush",
                            "list-close", "transfer-submit", "queue-submit",
                            "sample-constants-transform-projection", "sample-constants-texgen",
                            "sample-constants-color-conversion", "sample-constants-fog-coefficients",
                            "sample-constants-resolved-textures-alpha",
                            "sample-constants-compare-upload-attribution",
                            "upload-reserve", "vertex-bind-emit", "index-bind-emit",
                            "draw-emit", "light-upload"};
            out << "[native-cpu-profile] inclusive render-thread timings; nested phases overlap, "
                   "do not sum or subtract clock overhead; clock-pair "
                << g.cpu_profile_clock_pair_ns << " ns/" << g.cpu_profile_clock_pair_calls
                << " pairs; phase-count=" << phase_names.size() << "\n";
            out << "[native-cpu-profile] constants-sampling stride="
                << NativeGpu::kCpuConstantSampleStride << " materializations="
                << g.constant_materializations << " samples="
                << g.cpu_phases[static_cast<std::size_t>(
                       NativeGpu::CpuPhase::SampleConstantsTransformProjection)].calls
                << "; sample-constants phases measure only every stride-th materialization, "
                   "no extrapolation; timer floor may dominate short fragments; "
                   "transform includes zero-init/projection, color includes channels, "
                   "resolved-textures-alpha includes program/HUD fields\n";
            for (std::size_t phase = 0; phase < phase_names.size(); ++phase) {
                const auto& counter = g.cpu_phases[phase];
                out << "[native-cpu-profile] " << phase_names[phase] << " calls=" << counter.calls
                    << " total-ms=" << static_cast<double>(counter.ns) / 1e6
                    << " max-us=" << static_cast<double>(counter.max_ns) / 1e3 << "\n";
            }
            out << "[native-cpu-profile] perspective raw-key reuse hits=" << g.cpu_projection_key_hits
                << " misses=" << g.cpu_projection_key_misses << " non-neutral-policy="
                << g.cpu_projection_policy_non_neutral << " (census only; no cache enabled)\n";
            out << "[native-cpu-profile] binding-census vertex-attempts=" << g.cpu_bind_attempts[0]
                << " vertex-reuses=" << g.cpu_bind_reuses[0]
                << " index-attempts=" << g.cpu_bind_attempts[1]
                << " index-reuses=" << g.cpu_bind_reuses[1]
                << " pipeline-emits=" << g.cpu_draw_state_emits[0]
                << " viewport-emits=" << g.cpu_draw_state_emits[1]
                << " scissor-emits=" << g.cpu_draw_state_emits[2]
                << "; draw preparation only; emitted phases exclude reuse\n";
        }
        if (g.queue_cpu_profile_enabled) {
            static constexpr std::array<const char*, 7> queue_names{
                "total", "queue-wait", "list-packing", "execute", "signal-semaphore",
                "signal-fence", "event-registration"};
            out << "[native-queue-cpu] graphics queue inclusive timings; total overlaps stages\n";
            for (std::size_t stage = 0; stage < queue_names.size(); ++stage) {
                const auto& c = g.queue_cpu_phases[stage];
                out << "[native-queue-cpu] " << queue_names[stage] << " calls=" << c.calls
                    << " total-ms=" << static_cast<double>(c.ns) / 1e6
                    << " max-us=" << static_cast<double>(c.max_ns) / 1e3
                    << " failures=" << c.failures << " last-hresult=" << c.last_failure_hresult << "\n";
            }
        }
        if (!g.last_error.empty()) out << "[native-gpu] last error: " << g.last_error << "\n";
    }
    return out.str();
}

}  // namespace gekko
