#include "gekko/lifted_render.h"
#include "gekko/native_rw_world.h"
#include "gekko/native_rw_device_state.h"
#include "gekko/native_rw_registry.h"
#include "gekko/native_rw_raster.h"

#include "gekko/display_geometry.h"

#include "game/game_owners.h"
#include "gekko/native_gx_state.h"
#include "gekko/native_gx_display.h"
#include "gekko/native_gx_texobj.h"
#include "gekko/native_rw_node_stops.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gekko/cpu.h"
#include "render/effect_layers.h"
#include "src/gekko/effect_layer_overlay.h"
#include "src/gekko/options_display_overlay.h"
#include "render/packet_queue.h"
#include "render/present_trace.h"
#include "gekko/phase_lock.h"
#include "render/profile.h"
#include "render/renderer.h"
#include "render/view.h"
#include "rw_driver/im2d_prims.h"
#include "rw_driver/raster_render.h"
#include "rw_driver/rw_driver.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace gekko::scaffold {
u32 rw_pipeline_execute_im3d_transform(Cpu& c, u32 pipeline, u32 pool, u32);  // rw_driver.cpp
u32 rw_native_texture_loaded(Cpu& c, u32 texture, u32 bias_clamp, u32 edge_lod, u32 max_aniso, double lod_bias);  // rw_driver.cpp
u32 rw_raster_clear(Cpu& c, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint32_t rgba, bool colour, bool depth);
u32 rw_pipeline_execute_im3d_render(Cpu& c, u32 pipeline, u32 stash, u32);
}  // namespace gekko::scaffold

namespace gekko {

namespace render = shadow::render;

struct LiftedRender::Impl {
    LiftedRenderOptions options;
    render::Renderer renderer;
    render::PacketQueue frames{2};
    std::unique_ptr<scaffold::RwDriver> driver;
    std::unique_ptr<game::GameOwners> owners;  // NE-RW1: the game-level owners, reaching RenderWare through the driver
    EffectLayerOverlay effect_overlay;          // the effect layer list (UI thread)
    OptionsDisplayOverlay options_overlay;      // the Options menu's Display page (UI thread)
    bool started = false;
    bool attached = false;
    std::uint32_t width = 0, height = 0;
    std::uint32_t render_width = 0, render_height = 0;  // G10: the render size last set (0: the window's)
    std::mutex render_note_mutex;
    std::string render_note;  // apply_render_size: why the render size in use is not the one asked
    std::uint32_t output_width = 0, output_height = 0;  // the game-visible output last set (set_output_aspect)
    void apply_render_size();
    std::uint64_t presented = 0;
    mutable std::mutex telemetry_mutex;
    LiftedRender::Telemetry timing;
    std::uint64_t pending_packet_wait_ns = 0;  // UI thread, includes empty dequeue waits
    std::int64_t last_credit_ns = 0;  // eager start: when the last frame's swap-chain credit came (steady ns)
    // Bumped when V-Sync changes: the phase lock's filters forget their samples (gekko/phase_lock.h display_paced).
    std::uint32_t pacing_epoch = 0;
    double present_sum_ms = 0.0;
    // Frame pacing as presented (ES 2026-10-19; measurement only, nothing paces by it): the wall time between
    // consecutive presents, in 0.1 ms bins up to 100 ms (the last bin holds everything longer), and the longest.
    std::array<std::uint64_t, 1001> present_bins{};
    std::uint64_t present_intervals = 0;
    double present_max_ms = 0.0;
    std::chrono::steady_clock::time_point last_present{};
    bool have_present = false;
    void note_present(std::chrono::steady_clock::time_point now) {
        if (have_present) {
            const double ms = std::chrono::duration<double, std::milli>(now - last_present).count();
            const std::size_t bin = std::min<std::size_t>(static_cast<std::size_t>(ms * 10.0), present_bins.size() - 1);
            ++present_bins[bin];
            ++present_intervals;
            present_sum_ms += ms;
            present_max_ms = std::max(present_max_ms, ms);
        }
        last_present = now;
        have_present = true;
    }
    // The interval below which a share `q` of the presents came (bin upper edge, ms).
    double present_quantile(double q) const {
        if (present_intervals == 0u)
            return 0.0;
        const double want = q * static_cast<double>(present_intervals);
        std::uint64_t seen = 0;
        for (std::size_t b = 0; b < present_bins.size(); ++b) {
            seen += present_bins[b];
            if (static_cast<double>(seen) >= want)
                return static_cast<double>(b + 1) / 10.0;
        }
        return present_max_ms;
    }
    std::uint64_t presents_longer_than(double ms) const {
        std::uint64_t n = 0;
        for (std::size_t b = static_cast<std::size_t>(ms * 10.0); b < present_bins.size(); ++b)
            n += present_bins[b];
        return n;
    }
    std::uint64_t render_failures = 0;
    // Captures (the UI thread or the headless thread; armed from any thread by the development link).
    mutable std::mutex capture_mutex;
    LiftedCapture capture;
    std::uint64_t capture_next = 0;
    std::uint64_t capture_from_game_tick = 0;  // non-zero: the next capture is the first frame finished at or after it
    std::uint64_t captures_left = 0;
    std::uint64_t captures_written = 0;
    std::uint64_t last_capture_frame = 0;
    std::uint64_t last_capture_game_ticks = 0;
    std::string last_capture_path;
    std::string last_capture_error;
    // Headless: the frames the game finished (seen on the game thread).
    std::uint64_t headless_taken = 0;
    bool headless = false;

    // The D3D12 debug layer's messages (when it is on), to stderr with the frame that produced them.
    std::uint64_t debug_messages = 0;
    void report_debug_layer(std::uint64_t tick) {
        for (const std::string& message : renderer.take_debug_errors()) {
            if (++debug_messages <= 50u)
                std::cerr << "[lifted-render] debug layer (frame " << tick << "): " << message << '\n';
        }
    }

    static bool saves_history(const render::FramePacket& packet) {
        for (const render::PassRecord& pass : packet.passes) {
            if (pass.kind == render::PassKind::SaveHistory)
                return true;
        }
        return false;
    }

    // A packet as the game made it: captured with the effect layer switches applied (render/effect_layers.h).
    void maybe_capture(const render::FramePacket& packet) {
        const render::EffectLayerState layers = render::effect_layer_switches().state();
        if (layers.everything())
            return capture_filtered(packet);
        render::FramePacket filtered = packet;
        render::apply_effect_layers(filtered, layers);
        capture_filtered(filtered);
    }

    void capture_filtered(const render::FramePacket& packet) {
        std::unique_lock<std::mutex> capture_lock(capture_mutex);
        if (captures_left == 0u)
            return;
        const bool due = capture_from_game_tick != 0u ? packet.game_ticks >= capture_from_game_tick
                                                       : packet.tick >= capture_next;
        if (!due) {
            // Headless, a frame that saves a view's history is rendered all the same (nothing read back): the trail
            // a later capture shows feeds back from the frames before it, as on screen, not from the last capture.
            if (headless && saves_history(packet)) {
                std::string error;
                if (!renderer.render_to_image(packet, capture.width, capture.height, nullptr, &error))
                    std::cerr << "[lifted-render] frame " << packet.tick << " (history) failed: " << error << '\n';
                report_debug_layer(packet.tick);
            }
            return;
        }
        std::vector<std::uint8_t> rgba;
        std::string error;
        // Diagnostics: SHADOW_R2_STRIP=<glare|glaresources|particles|immediate|meshes> renders the packet without that
        // kind of pass or draw (bisecting the GPU path).
        render::FramePacket stripped;
        const render::FramePacket* rendering = &packet;
        if (const char* strip = std::getenv("SHADOW_R2_STRIP")) {
            stripped = packet;
            const std::string what = strip;
            if (what == "rotation") {
                for (render::Particle& particle : stripped.particles)
                    particle.rotation = 0.0f;
            }
            if (what == "sourcedepth" || what == "sourceblend") {
                for (const render::PassRecord& pass : stripped.passes) {
                    if (pass.kind != render::PassKind::GlareSources)
                        continue;
                    for (std::uint32_t d = pass.first_draw; d < pass.first_draw + pass.draw_count; ++d) {
                        if (what == "sourcedepth")
                            stripped.draws[d].recipe.depth_test = false;
                        else
                            stripped.draws[d].recipe.dst = render::Blend::Zero;  // no blending onto the sources
                    }
                }
            }
            for (render::PassRecord& pass : stripped.passes) {
                if (what == "glare" && pass.kind == render::PassKind::Glare)
                    pass.view = 0xFFFFFFFFu;  // no view draws it
                if (what == "glaresources" && pass.kind == render::PassKind::GlareSources)
                    pass.view = 0xFFFFFFFFu;
                if (what == "particles" || what == "immediate" || what == "meshes" || what == "skinned" ||
                    what == "static" || what == "sourceparticles" || what == "sceneparticles") {
                    std::uint32_t kept = 0;
                    const bool sources = pass.kind == render::PassKind::GlareSources;
                    for (std::uint32_t d = pass.first_draw; d < pass.first_draw + pass.draw_count; ++d) {
                        const render::DrawRecord& draw = stripped.draws[d];
                        const bool is_particles = draw.particles != render::kNoParticles;
                        const bool is_immediate = draw.immediate != render::kNoImmediate;
                        const bool is_mesh = !is_particles && !is_immediate;
                        const bool drop = (what == "particles" && is_particles) ||
                                          (what == "immediate" && is_immediate) ||
                                          (what == "meshes" && is_mesh) ||
                                          (what == "skinned" && is_mesh && draw.bones.count != 0u) ||
                                          (what == "static" && is_mesh && draw.bones.count == 0u) ||
                                          (what == "sourceparticles" && is_particles && sources) ||
                                          (what == "sceneparticles" && is_particles && !sources);
                        if (!drop)
                            stripped.draws[pass.first_draw + kept++] = draw;
                    }
                    pass.draw_count = kept;
                }
            }
            rendering = &stripped;
        }
#if !defined(GEKKO_SHIPPING)
        // Diagnostics: SHADOW_R2_FOLD_CHECK=1 renders the captured packet with the effect-layer overlay over it (the
        // view the fold applies to; captures otherwise never carry it) twice - the view under it folded into it, and
        // resolved and copied as before - and reports whether the two pictures are byte-identical.
        if (const char* check = std::getenv("SHADOW_R2_FOLD_CHECK");
            check != nullptr && (check[0] == '1' || check[0] == '2')) {
            render::FramePacket overlaid = *rendering;
            effect_overlay.append(overlaid, renderer, capture.width, capture.height);
            std::vector<std::uint8_t> folded_rgba, copied_rgba;
            std::string check_error;
            // SHADOW_R2_FOLD_CHECK=2: the copy rendered first (which render a first-use difference follows).
            const bool copy_first = check[0] == '2';
            bool a = false, b = false;
            if (copy_first) {
                render::set_fold_under_override(0);
                b = renderer.render_to_image(overlaid, capture.width, capture.height, &copied_rgba, &check_error, false);
            }
            render::set_fold_under_override(1);
            a = renderer.render_to_image(overlaid, capture.width, capture.height, &folded_rgba, &check_error, false);
            if (!copy_first) {
                render::set_fold_under_override(0);
                b = renderer.render_to_image(overlaid, capture.width, capture.height, &copied_rgba, &check_error, false);
            }
            // The folded picture once more: a difference between the first two that this one also shows against the
            // first is the first render's own (a resource made during it), not the fold's.
            std::vector<std::uint8_t> again_rgba;
            render::set_fold_under_override(1);
            const bool c = renderer.render_to_image(overlaid, capture.width, capture.height, &again_rgba, &check_error,
                                                    false);
            render::set_fold_under_override(-1);
            std::size_t differing = 0, differing_again = 0, first_at = folded_rgba.size();
            for (std::size_t i = 0; a && b && i < folded_rgba.size() && i < copied_rgba.size(); ++i) {
                if (folded_rgba[i] != copied_rgba[i]) {
                    ++differing;
                    first_at = std::min(first_at, i);
                }
            }
            for (std::size_t i = 0; b && c && i < again_rgba.size() && i < copied_rgba.size(); ++i)
                differing_again += again_rgba[i] != copied_rgba[i] ? 1u : 0u;
            std::cerr << "[fold-check] frame " << packet.tick << " views " << overlaid.views.size() << ": "
                      << (a && b && c ? std::to_string(differing) + " bytes differ, again " +
                                            std::to_string(differing_again)
                                      : "render failed: " + check_error);
            if (differing != 0u)
                std::cerr << " (first at pixel " << (first_at / 4) % capture.width << ", "
                          << (first_at / 4) / capture.width << ", byte " << first_at % 4 << ": "
                          << int(folded_rgba[first_at]) << " vs " << int(copied_rgba[first_at]) << ")";
            std::cerr << '\n';
        }
#endif
        // Windowed, this frame is presented right after: render it without committing the views' histories.
        const bool rendered =
            renderer.render_to_image(*rendering, capture.width, capture.height, &rgba, &error, headless);
        report_debug_layer(packet.tick);
        // Diagnostics: SHADOW_R2_CAPTURE_REPEAT=<n> renders each captured packet n more times and reports whether
        // every rendering is byte-identical to the first (determinism of the GPU path itself, within one process).
        if (const char* repeat = std::getenv("SHADOW_R2_CAPTURE_REPEAT"); repeat != nullptr && rendered) {
            const unsigned long times = std::strtoul(repeat, nullptr, 10);
            // SHADOW_R2_CAPTURE_REPEAT_DELAY_MS: a pause before each repeat (a driver's background recompilation).
            const char* delay = std::getenv("SHADOW_R2_CAPTURE_REPEAT_DELAY_MS");
            const unsigned long pause = delay != nullptr ? std::strtoul(delay, nullptr, 10) : 0ul;
            for (unsigned long r = 0; r < times; ++r) {
                if (pause != 0ul)
                    std::this_thread::sleep_for(std::chrono::milliseconds(pause));
                std::vector<std::uint8_t> again;
                std::string again_error;
                if (!renderer.render_to_image(*rendering, capture.width, capture.height, &again, &again_error, false))
                    continue;
                std::size_t differing = 0, first = again.size();
                for (std::size_t i = 0; i < again.size(); ++i) {
                    if (again[i] != rgba[i]) {
                        ++differing;
                        if (first == again.size())
                            first = i;
                    }
                }
                std::cerr << "[lifted-render] repeat " << r + 1 << " of frame " << packet.tick << ": " << differing
                          << " bytes differ";
                if (differing != 0u)
                    std::cerr << " (first at pixel " << (first / 4) % capture.width << ", "
                              << (first / 4) / capture.width << ")";
                std::cerr << '\n';
            }
        }
        if (!rendered) {
            std::cerr << "[lifted-render] capture of frame " << packet.tick << " failed: " << error << '\n';
            last_capture_error = error;
        } else {
            std::filesystem::path path(capture.path);
            if (capture.count > 1u) {
                path.replace_filename(path.stem().string() + "-" + std::to_string(packet.tick) +
                                      path.extension().string());
            }
            if (stbi_write_png(path.string().c_str(), static_cast<int>(capture.width),
                               static_cast<int>(capture.height), 4, rgba.data(),
                               static_cast<int>(capture.width * 4u)) == 0) {
                std::cerr << "[lifted-render] could not write " << path.string() << '\n';
            } else {
                ++captures_written;
                last_capture_frame = packet.tick;
                last_capture_game_ticks = packet.game_ticks;
                last_capture_path = path.string();
                last_capture_error.clear();
                std::cout << "[lifted-render] frame " << packet.tick << " -> " << path.string() << '\n';
            }
        }
        --captures_left;
        capture_next = packet.tick + (capture.stride != 0u ? capture.stride : 1u);
        capture_from_game_tick = 0u;
    }
};

LiftedRender::LiftedRender() : impl_(std::make_unique<Impl>()) {}

LiftedRender::~LiftedRender() {
    stop();
}

bool LiftedRender::start(const LiftedRenderOptions& options, std::string* error) {
    impl_->options = options;
    gekko::phase_lock::display_paced().store(options.vsync, std::memory_order_relaxed);
    // Diagnostics: SHADOW_R2_WARP=1 renders on the software rasterizer, SHADOW_R2_DEBUG_LAYER=1 enables the D3D12
    // debug layer (its messages go to stderr as frames are rendered).
    if (const char* warp = std::getenv("SHADOW_R2_WARP"))
        impl_->options.warp = impl_->options.warp || warp[0] == '1';
    if (const char* debug = std::getenv("SHADOW_R2_DEBUG_LAYER"))
        impl_->options.debug_layer = impl_->options.debug_layer || debug[0] == '1';
    const LiftedRenderOptions& chosen = impl_->options;
    render::RendererOptions renderer_options;
    renderer_options.msaa_samples = std::max<std::uint32_t>(chosen.msaa_samples, 1u);
    renderer_options.max_anisotropy = std::clamp<std::uint32_t>(chosen.anisotropy, 1u, 16u);
    renderer_options.device.adapter =
        chosen.warp ? render::DeviceOptions::Adapter::Warp : render::DeviceOptions::Adapter::HighPerformance;
    renderer_options.device.debug_layer = chosen.debug_layer;
    // Diagnostics: SHADOW_R2_LOW_POWER_GPU=1 picks the integrated GPU (comparisons across GPUs).
    if (const char* low = std::getenv("SHADOW_R2_LOW_POWER_GPU"); low != nullptr && low[0] == '1' && !chosen.warp)
        renderer_options.device.adapter = render::DeviceOptions::Adapter::LowPower;
    // Diagnostics: SHADOW_R2_GBV=1 adds GPU-based validation to the debug layer (slow).
    if (const char* gbv = std::getenv("SHADOW_R2_GBV"); gbv != nullptr && gbv[0] == '1') {
        renderer_options.device.debug_layer = true;
        renderer_options.device.gpu_validation = true;
    }
    // Diagnostics: SHADOW_R2_GLARE_ONLY=1 - the glare composite replaces the scene (the picture is the glare alone).
    if (const char* glare_only = std::getenv("SHADOW_R2_GLARE_ONLY")) {
        renderer_options.debug_glare_only = glare_only[0] >= '1' && glare_only[0] <= '3';
        renderer_options.debug_glare_parts = glare_only[0] == '2' ? 1u : glare_only[0] == '3' ? 2u : 0u;
    }
    // Diagnostics: SHADOW_R2_SOURCE_DEPTH_RW=1 - the glare sources bind the scene depth through its writable view.
    if (const char* rw = std::getenv("SHADOW_R2_SOURCE_DEPTH_RW"))
        renderer_options.debug_writable_source_depth = rw[0] == '1';
    // Diagnostics: SHADOW_R2_MSAA=<samples> (1: no multisampling) for bisecting the GPU path.
    if (const char* msaa = std::getenv("SHADOW_R2_MSAA"))
        renderer_options.msaa_samples = static_cast<std::uint32_t>(std::max(1ul, std::strtoul(msaa, nullptr, 10)));
    if (!impl_->renderer.create(renderer_options, error))
        return false;
    // G10's proof hook: SHADOW_R2_CAPTURE_AT_RENDER_SIZE=1 renders captures at the render size and scales them into
    // their own size with the presented path's scale pass (the scaled picture read back; HW_REPORT N.2).
    if (const char* scaled = std::getenv("SHADOW_R2_CAPTURE_AT_RENDER_SIZE"); scaled != nullptr && scaled[0] == '1')
        impl_->renderer.set_capture_at_render_size(true);
    std::cout << "[lifted-render] adapter: " << impl_->renderer.device().caps().adapter_name << ", "
              << renderer_options.msaa_samples << "x multisampling\n";
    std::cout << "[lifted-render] render size: "
              << (chosen.render_width != 0u || chosen.render_height != 0u
                      ? std::to_string(chosen.render_width) + "x" + std::to_string(chosen.render_height) +
                            " asked (exact when both are given, else aspect-locked to the game's output)"
                      : std::string("the window's size"))
              << "\n";
    impl_->driver = std::make_unique<scaffold::RwDriver>(impl_->renderer, impl_->frames);
    impl_->driver->set_framing(options.game_framing ? render::Framing::GameWindow : render::Framing::SquarePixels);
    impl_->driver->install();
    impl_->owners = std::make_unique<game::GameOwners>(*impl_->driver);
    impl_->owners->install();
    impl_->started = true;
    return true;
}

HudListener* LiftedRender::hud_listener() noexcept {
    return impl_->driver.get();
}

void LiftedRender::set_display_blank(std::function<bool()> source) {
    if (impl_->driver)
        impl_->driver->set_display_blank(std::move(source));
}

void LiftedRender::set_fade_in_ticks(u64 ticks) noexcept {
    if (impl_->driver)
        impl_->driver->set_fade_in_ticks(ticks);
}

bool LiftedRender::set_effect_layers(const std::string& off, const std::string& solo, bool isolate,
                                     std::string* error) {
    render::EffectLayerSwitches& switches = render::effect_layer_switches();
    if (!switches.parse_off_list(off, error) || (!solo.empty() && !switches.parse_solo(solo, error)))
        return false;
    if (isolate)
        switches.toggle_isolate();
    return true;
}

std::string LiftedRender::effect_layers() const { return render::effect_layer_switches().describe(); }

std::string LiftedRender::effect_layer_names() {
    std::string out;
    for (std::uint32_t i = 1; i < render::kEffectLayerCount; ++i)
        out += std::string(i > 1 ? "," : "") + render::effect_layer_name(static_cast<render::EffectLayer>(i));
    return out;
}

void LiftedRender::set_fps_counter(bool on) { impl_->effect_overlay.set_fps_counter(on); }

void LiftedRender::set_vsync(bool on) noexcept {
    const bool changed = impl_->options.vsync != on;
    impl_->options.vsync = on;
    gekko::phase_lock::display_paced().store(on, std::memory_order_relaxed);
    if (changed) {
        // V-Sync off: nothing steers the game clock's phase (no vblank to lock to); it keeps the shift it has. On again:
        // the lock starts from new holds only - samples from before the switch measured another reference.
        gekko::phase_lock::published_hold_ns().store(gekko::phase_lock::kNothingMeasured, std::memory_order_relaxed);
        gekko::phase_lock::published_target_shift_ns().store(gekko::phase_lock::kNothingMeasured,
                                                             std::memory_order_relaxed);
        ++impl_->pacing_epoch;
    }
}

void LiftedRender::set_msaa(std::uint32_t samples) noexcept { impl_->renderer.set_msaa_samples(samples); }
void LiftedRender::set_anisotropy(std::uint32_t level) noexcept { impl_->renderer.set_anisotropy(level); }
std::uint32_t LiftedRender::msaa_active() const noexcept { return impl_->renderer.msaa_samples(); }
std::uint32_t LiftedRender::anisotropy_active() const noexcept { return impl_->renderer.anisotropy(); }
std::uint32_t LiftedRender::msaa_supported_mask() const noexcept { return impl_->renderer.msaa_supported_mask(); }
std::string LiftedRender::msaa_note() const { return impl_->renderer.msaa_note(); }

void LiftedRender::effect_layer_keys(unsigned keys) {
    if (keys != 0u)
        impl_->effect_overlay.keys(keys);
}

void LiftedRender::set_draw_distance(float range_scale) noexcept {
    if (!impl_->driver)
        return;
    // 1: the original (no proxies); above 1, proxies to that many times each entry's range; 8 and up: no limit
    // beyond the projected-size rule.
    impl_->driver->far_objects().set_enabled(range_scale > 1.0f);
    impl_->driver->far_objects().set_range_scale(range_scale >= 8.0f ? 0.0f : range_scale);
    // Measuring: the classes not yet verified are drawn too, and every handover compares their pose with the game's.
    if (const char* validate = std::getenv("SHADOW_FAR_VALIDATE"))
        impl_->driver->far_objects().set_validate(validate[0] == '1');
    if (const char* distance = std::getenv("SHADOW_DISTANCE_TRACE"))
        impl_->driver->far_objects().set_distance_trace(distance[0] == '1');
    // Measuring: SHADOW_FAR_CENSUS=<frame>[:<last>:<step>] lists the layout's classes at those frames.
    if (const char* census = std::getenv("SHADOW_FAR_CENSUS")) {
        unsigned long long first = 0, last = 0, step = 1;
        if (std::sscanf(census, "%llu:%llu:%llu", &first, &last, &step) >= 1)
            impl_->driver->far_objects().set_census(first, last, step);
    }
    // Measuring: SHADOW_FAR_TRACE=<class>:<first frame>:<last frame> logs that class's entries near the eye per frame.
    if (const char* trace = std::getenv("SHADOW_FAR_TRACE")) {
        static std::string trace_class;
        unsigned long long first = 0, last = 0;
        const char* colon = std::strchr(trace, ':');
        if (colon != nullptr && std::sscanf(colon + 1, "%llu:%llu", &first, &last) == 2) {
            trace_class.assign(trace, colon);
            impl_->driver->far_objects().set_trace(trace_class.c_str(), first, last);
        }
    }
}

std::uint64_t LiftedRender::far_proxy_draws() const noexcept {
    return impl_->driver ? impl_->driver->stats().far_proxy_draws : 0u;
}

u64 LiftedRender::fade_ins() const noexcept {
    return impl_->driver ? impl_->driver->fade_ins() : 0u;
}

// extract/native-owners-r2.txt: Weapon::LockonMark's draw and the lens flare's builder - functions of the game, owned
// by runtime/game/game_owners.cpp (NE-RW1).
void LiftedRender::install_owners(Cpu& cpu) {
    if (impl_->driver)
        impl_->driver->material_probe().install(cpu);
    scaffold::install_device_surface_owners(cpu);
    scaffold::install_display_owners(cpu);  // the display swap (docs/agents/vi.md, step 12)
    scaffold::install_raster_render_owner(cpu);  // RwRasterRender core 0x80499924 (docs/agents/os-other.md, step 13)
    scaffold::install_im2d_prim_owners(cpu);  // the device table's Im2D line / triangle emitters (docs/agents/os-other.md, step 12)
    // rwR3: the Im3D entry points; the pipeline execution is the scaffold's (the sites the lowered bodies called).
    install_native_rw_im3d_owners(
        cpu, [](Cpu& c, u32 pipeline, u32 data, u32 option) { return scaffold::rw_pipeline_execute_im3d_transform(c, pipeline, data, option); },
        [](Cpu& c, u32 pipeline, u32 data, u32 option) { return scaffold::rw_pipeline_execute_im3d_render(c, pipeline, data, option); });
    install_native_rw_world_owners(cpu);
    rw_node_stops::install_lifted(cpu);  // entries nothing enters (gekko/native_rw_node_stops.h)
    rw_raster::install_lifted(cpu, [](Cpu& c, u32 texture, u32 bias_clamp, u32 edge_lod, u32 max_aniso, double lod_bias) {
        return scaffold::rw_native_texture_loaded(c, texture, bias_clamp, edge_lod, max_aniso, lod_bias);
    });   // GcnNativeTextureRead (extract/native-owners-r2.txt)
    rw_raster::set_raster_clear(&scaffold::rw_raster_clear);   // the raster-level clear (ClearQuad 0x80499E58) is the driver's
    rw_registry::install(cpu);  // rwR5 plugin-registry entries (docs/agents/native-boot.md Step 8)
    rw_device_state::install(cpu);  // rwR1 device render-state entries (docs/agents/native-boot.md Step 7)
    cpu.install_hle(0x800620D4u, [](Cpu& c) { return game::lockon_mark_draw(c); },
                    "Weapon::LockonMark draw (game owner)");
    cpu.install_hle(0x80432360u, [](Cpu& c) { return game::lens_flare_build(c); },
                    "lens flare build (game owner)");
    gx_state::install_lifted(cpu);  // the GX vertex path: dead in this build (docs/agents/card-saves.md, gxC)
    gx_display::install_lifted(cpu);  // GXCopyDisp: its copy trigger is a dead site in this build (Step H)
    gx_texobj::install_lifted(cpu);  // the texture loads: dead pipe sites, unread words (Step J)
}

void LiftedRender::set_logic_aspect(float aspect) {
    if (impl_->driver)
        impl_->driver->set_logic_aspect(aspect);
}

void LiftedRender::set_output_aspect(std::uint32_t width, std::uint32_t height) {
    if (impl_->driver && width != 0u && height != 0u)
        impl_->driver->set_output_aspect(static_cast<float>(width) / static_cast<float>(height));
    impl_->output_width = width;
    impl_->output_height = height;
    // The game output is the picture (the viewport, gekko/display_geometry.h): a scaled render size is placed at its
    // exact aspect.
    if (width != 0u && height != 0u)
        impl_->renderer.set_present_aspect(static_cast<double>(width) / static_cast<double>(height));
    impl_->apply_render_size();
}

void LiftedRender::set_render_size(std::uint32_t width, std::uint32_t height) {
    impl_->options.render_width = width;
    impl_->options.render_height = height;
    impl_->apply_render_size();
}

void LiftedRender::set_present_aspect(double aspect) {
    if (aspect > 0.0) impl_->renderer.set_present_aspect(aspect);
}

std::string LiftedRender::render_note() const {
    std::lock_guard<std::mutex> lock(impl_->render_note_mutex);
    return impl_->render_note;
}

// G10: the render size. gekko/display_geometry.h is its one owner: both sides asked are taken exactly as asked (the
// picture's shape already; before 2026-11-05 render_size_for re-fitted them to the game output, so an asked 720x540
// rendered 720x405 while the settings said 720x540); none asked: the window's size. The only change made here is the
// GPU's memory: a size whose main targets would not fit in 3/4 of the adapter's dedicated memory is lowered by one
// factor on both sides (the shape kept), and said so (render_note).
void LiftedRender::Impl::apply_render_size() {
    const std::uint32_t rw = options.render_width, rh = options.render_height;
    std::uint32_t w = rw != 0u && rh != 0u ? rw : 0u, h = rw != 0u && rh != 0u ? rh : 0u;
    std::string note;
    if (w != 0u) {
        const std::uint64_t budget = renderer.device().caps().dedicated_video_memory / 4u * 3u;
        const std::uint64_t bytes = gekko::render_target_bytes({w, h}, std::max(1u, renderer.msaa_samples()));
        if (budget != 0u && bytes > budget) {
            const double factor = std::sqrt(static_cast<double>(budget) / static_cast<double>(bytes));
            const std::uint32_t fw = std::max(1u, static_cast<std::uint32_t>(std::floor(w * factor)));
            const std::uint32_t fh = std::max(1u, static_cast<std::uint32_t>(std::floor(h * factor)));
            note = "render size " + std::to_string(w) + "x" + std::to_string(h) + " needs ~" +
                   std::to_string(bytes >> 20) + " MiB of scene targets, more than 3/4 of the GPU's " +
                   std::to_string(renderer.device().caps().dedicated_video_memory >> 20) + " MiB: " +
                   std::to_string(fw) + "x" + std::to_string(fh) + " is used (the same shape)";
            w = fw;
            h = fh;
        }
    }
    {
        std::lock_guard<std::mutex> lock(render_note_mutex);
        if (note != render_note && !note.empty()) std::cerr << "WARNING: [lifted-render] " << note << "\n";
        render_note = note;
    }
    if (w == 0u && render_width == 0u)
        return;
    renderer.set_render_size(w, h);
    if (w != render_width || h != render_height) {
        render_width = w;
        render_height = h;
        if (w == 0u) {
            std::cout << "[lifted-render] render size: the window's\n";
            return;
        }
        std::cout << "[lifted-render] render size " << w << "x" << h << " (asked " << rw << "x" << rh
                  << ", game output " << output_width << "x" << output_height
                  << "); windowed it is fitted into the back buffer (1:1 when the sizes match, a 2x2 box at exactly "
                  << "2:1, a bicubic otherwise, bars when the shapes differ), captures keep their own size\n";
    }
}

std::uint32_t LiftedRender::render_width() const noexcept {
    return impl_->render_width;
}

std::uint32_t LiftedRender::render_height() const noexcept {
    return impl_->render_height;
}

void LiftedRender::request_capture(const LiftedCapture& capture) {
    // The game's output is not the capture's (set_output_aspect): a headless run whose captures define its output
    // sets that output from the same size before the game runs (shadow_main), so a capture asked for during a run
    // never changes what the game sees.
    std::lock_guard<std::mutex> lock(impl_->capture_mutex);
    impl_->capture = capture;
    impl_->capture_next = capture.first_frame;
    impl_->capture_from_game_tick = 0u;
    impl_->captures_left = capture.path.empty() ? 0u : (capture.count == 0u ? 1u : capture.count);
}

void LiftedRender::arm_capture(const std::string& path, std::uint64_t frame, std::uint32_t width,
                               std::uint32_t height) {
    LiftedCapture capture;
    capture.path = path;
    capture.first_frame = frame;
    capture.count = 1;
    capture.width = width != 0u ? width : 1280u;
    capture.height = height != 0u ? height : 720u;
    request_capture(capture);
}

void LiftedRender::arm_capture_at_tick(const std::string& path, std::uint64_t game_tick, std::uint32_t width,
                                       std::uint32_t height) {
    arm_capture(path, 0u, width, height);
    std::lock_guard<std::mutex> lock(impl_->capture_mutex);
    impl_->capture_from_game_tick = game_tick != 0u ? game_tick : 1u;
}

LiftedRender::CaptureResult LiftedRender::capture_result() const {
    std::lock_guard<std::mutex> lock(impl_->capture_mutex);
    CaptureResult result;
    result.written = impl_->captures_written;
    result.frame = impl_->last_capture_frame;
    result.game_ticks = impl_->last_capture_game_ticks;
    result.path = impl_->last_capture_path;
    result.error = impl_->last_capture_error;
    result.pending = impl_->captures_left != 0u;
    return result;
}

std::uint64_t LiftedRender::next_frame() const {
    return impl_->driver ? impl_->driver->frames() : 0u;
}

void LiftedRender::save_state(std::vector<std::uint8_t>& out) const {
    if (impl_->driver)
        impl_->driver->save_state(out);
}

bool LiftedRender::load_state(const std::uint8_t*& cursor, const std::uint8_t* end) {
    return impl_->driver && impl_->driver->load_state(cursor, end);
}

void LiftedRender::on_state_loaded() {
    if (!impl_->driver)
        return;
    impl_->driver->on_state_loaded();
    // Frames of the replaced timeline that were not presented yet.
    render::FramePacket stale;
    impl_->frames.pop_newest(&stale, std::chrono::milliseconds(0));
    impl_->renderer.invalidate_histories();
}

void LiftedRender::own_words(std::vector<std::uint32_t>& out) const {
    if (impl_->driver)
        impl_->driver->own_words(out);
}

bool LiftedRender::frame_open() const {
    return impl_->driver && impl_->driver->frame_open();
}

// Without a window nothing presents: the requested frames are rendered on the game thread when the game finishes
// them (outside game time - it advances only at wait points), so a capture is always the exact frame asked for.
void LiftedRender::start_headless() {
    if (!impl_->started)
        return;
    impl_->headless = true;
    impl_->driver->set_frame_observer([impl = impl_.get()](const render::FramePacket& packet) {
        ++impl->headless_taken;
        impl->maybe_capture(packet);
        // Headless, nothing else ever records a packet: once seen here it is finished, and the handles its building
        // released can go.
        impl->renderer.finish_packets(packet.tick);
    });
}

void LiftedRender::stop() {
    if (!impl_->started)
        return;
    impl_->frames.close();
    // Flush after close so the final queued-packet cancellations are included.
    render::present_trace::flush();
    impl_->driver->set_frame_observer({});
    if (impl_->owners)
        impl_->owners->uninstall();
    impl_->driver->uninstall();
    impl_->renderer.destroy();
    impl_->started = false;
    impl_->attached = false;
}

bool LiftedRender::attach(void* hwnd, std::uint32_t width, std::uint32_t height, std::string* error) {
    if (!impl_->renderer.attach_window(static_cast<HWND>(hwnd), width, height, error))
        return false;
    impl_->attached = true;
    impl_->width = width;
    impl_->height = height;
    return true;
}

bool LiftedRender::resize(std::uint32_t width, std::uint32_t height, std::string* error) {
    if (!impl_->attached || width == 0u || height == 0u || (width == impl_->width && height == impl_->height))
        return true;
    if (!impl_->renderer.resize_window(width, height, error))
        return false;
    impl_->width = width;
    impl_->height = height;
    return true;
}

bool LiftedRender::present_newest(std::chrono::milliseconds wait, std::string* error,
                                  PresentationSample* sample) {
    using Clock = std::chrono::steady_clock;
    const auto ns = [](Clock::time_point a, Clock::time_point b) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    };
    if (sample) *sample = {};
    const bool timed = impl_->options.collect_timing;
    const auto wait_begin = timed ? Clock::now() : Clock::time_point{};
    // SHADOW_R2_CREDIT_FIRST=0 (A/B): take the packet first and wait for the swap chain's credit holding it, as before
    // 2026-10-31. Default: once a packet exists, take the credit, then the newest packet - the frame recorded at a
    // vblank is the newest one the game had pushed by then (docs/TIMING_ARCHITECTURE.md finding 3). Same GPU start,
    // same work; up to one refresh less input-to-display latency, depending on where the game's push falls.
    static const bool credit_first = [] {
        const char* v = std::getenv("SHADOW_R2_CREDIT_FIRST");
        return !(v != nullptr && v[0] == '0');
    }();
    render::FramePacket packet;
    const std::int64_t packet_wait_start = render::present_trace::enabled() ? render::present_trace::now() : 0;
    // Eager start (render/burst_ahead.h; SHADOW_R2_EAGER=1 or the burst lead, not with the JIT pacer): the newest packet
    // now, recorded at once; the credit is taken inside render() before the present.
    const bool eager_mode = impl_->attached && impl_->options.vsync && render::BurstAhead::eager() &&
                            !render::JitPacer::requested();
    bool credit_then_packet = credit_first && impl_->attached && !eager_mode;
    bool eager = false;
    bool dequeued = false;
    if (eager_mode) {
        // Eager only for a packet pushed after the last credit: its own credit is then the next one, as with the credit
        // first. A packet pushed before it would be recorded and queued behind the frame still waiting for its flip - a
        // frame deeper than the credit-first pipeline, where a newer packet arriving before the credit replaces it
        // (measured 2026-11-05: such a start settled one frame deeper and the phase lock chased a hold over a period,
        // 41 ms of shift, until the audio queue ran dry). Such a packet goes the credit-first way.
        dequeued = impl_->frames.wait_for_packet(wait);
        if (dequeued) {
            std::int64_t newest_push = 0;
            impl_->frames.with_newest([&](const render::FramePacket& p) { newest_push = p.pushed_ns; });
            eager = newest_push != 0 && newest_push >= impl_->last_credit_ns;
            if (eager)
                dequeued = impl_->frames.pop_newest(&packet, std::chrono::milliseconds(0));
            else
                credit_then_packet = true;
        }
    } else {
        dequeued = credit_then_packet ? impl_->frames.wait_for_packet(wait) : impl_->frames.pop_newest(&packet, wait);
    }
    // The packet wait alone (the credit wait below is traced by the swap chain's WaitBegin / WaitEnd).
    render::present_trace::record(render::present_trace::Ev::PacketWait, dequeued ? 1u : 0u,
                                  static_cast<std::uint64_t>(packet_wait_start));
    if (timed) {
        const auto waited = ns(wait_begin, Clock::now());
        impl_->pending_packet_wait_ns += waited;
        std::lock_guard<std::mutex> lock(impl_->telemetry_mutex);
        impl_->timing.packet_wait_ns += waited;
    }
    if (dequeued && credit_then_packet) {
        // A wait that ended without a credit (timed out / failed) has no credit time: it is neither published as the
        // last credit nor sampled as a hold (Codex review 2026-11-05; the renderer owes the credit instead).
        const bool credit = impl_->renderer.acquire_frame();
        if (eager_mode && credit)
            impl_->last_credit_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        // SHADOW_R2_JIT (render/jit_pacer.h): start the frame shortly before the vblank it is shown at, not right after
        // the one before; the newest packet is taken after the wait.
        // SHADOW_R2_JIT_ASSIST=predict: the newest packet's particle fill, estimated on the CPU before its start is chosen.
        double fill_mpx = 0.0;
        bool estimated = false;
        std::uint64_t estimated_tick = 0;
        if (render::JitPacer::assist() == render::JitPacer::Assist::Predict)
            impl_->frames.with_newest([&](const render::FramePacket& newest) {
                fill_mpx = impl_->renderer.particle_fill_mpx(newest);
                estimated_tick = newest.tick;
                estimated = true;
            });
        const std::int64_t planned_start = impl_->renderer.jit_wait(impl_->options.vsync, fill_mpx);
        dequeued = impl_->frames.pop_newest(&packet, std::chrono::milliseconds(0));
        // The start was chosen from the packet that was newest before the wait; a newer one pushed during it is the one
        // recorded, and the pacer's model must learn from that frame's own fill, not the other's (Codex review).
        if (dequeued && estimated && packet.tick != estimated_tick)
            impl_->renderer.set_jit_fill_mpx(impl_->renderer.particle_fill_mpx(packet));
        // The hold the phase lock steers: push -> recording begins (now: credit and packet both held). With a planned
        // start (SHADOW_R2_JIT) push -> that start, negative for a packet pushed after it.
        // With the JIT pacer requested, a frame it did not plan (no grid yet: the first second, a resize, a refresh
        // re-base) is not sampled: its hold is measured from the credit, a different reference from the planned frames'
        // (vblank - lead), and enough such samples flipped the median target by the difference (~8-12 ms) and slewed
        // the phase the wrong way until the planned frames outvoted them again.
        const bool unplanned_jit = planned_start == 0 && render::JitPacer::requested();
        // A frame right after the burst mode switched (render/burst_ahead.h) holds for the switch, not for the phase.
        // V-Sync off: the credit is not a vblank (an immediate present returns it at once), so a hold measures nothing
        // the game clock could lock to - it read ~0 against the 2 ms target and the controller ran the game 1000 ppm
        // fast for ever (gekko/phase_lock.h display_paced, 2026-11-06).
        if (dequeued && credit && impl_->options.vsync && gekko::phase_lock::enabled() && packet.pushed_ns != 0 &&
            !unplanned_jit && !impl_->renderer.burst_settling()) {
            static gekko::phase_lock::HoldFilter hold_filter;
            static gekko::phase_lock::TargetFilter target_filter;
            static std::uint32_t epoch = 0;
            if (epoch != impl_->pacing_epoch) {
                epoch = impl_->pacing_epoch;
                hold_filter.reset();
                target_filter.reset();
            }
            const std::int64_t begins =
                planned_start != 0
                    ? planned_start
                    : std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            std::int64_t hold = begins - packet.pushed_ns;
            if (planned_start == 0)
                hold -= impl_->renderer.hold_bias_ns();  // burst lead: the packet came early by design
            if (planned_start != 0) {
                // The reference is the vblank after the pop - minus the committed lead; a frame that starts after its
                // vblank names the next one and its hold reads a whole period off. Kept within half a period of the
                // target, so one late frame cannot pull the phase by a frame (the measured wrap of the first JIT A/B).
                const std::int64_t period = impl_->renderer.jit_period_ns();
                const std::int64_t target = gekko::phase_lock::kTargetHold.count();
                if (period > 0) {
                    while (hold - target > period / 2) hold -= period;
                    while (hold - target < -period / 2) hold += period;
                }
            }
            hold_filter.add(hold);  // the legacy law's input, and the trace's
            target_filter.add(hold, packet.anchor_shift_ns);
        }
    }
    if (!dequeued)
        return true;
    GEKKO_PROFILE_ZONE("present/newest");
    render::present_trace::record(render::present_trace::Ev::Pop, packet.tick, packet.game_ticks);
    // Every packet up to this one is finished once it is recorded (the queue dropped the older ones): the handles
    // released while they were built go only now - a mesh rebuilt by the game's next frame stays drawable here.
    if (!impl_->attached) {
        impl_->renderer.finish_packets(packet.tick);
        return true;
    }
    const auto capture_begin = timed ? Clock::now() : Clock::time_point{};
    // The effect layer switches (render/effect_layers.h): what the player asked to see, for the picture and its captures.
    render::apply_effect_layers(packet, render::effect_layer_switches().state());
    impl_->capture_filtered(packet);
    impl_->options_overlay.append(packet, impl_->renderer);
    impl_->effect_overlay.append(packet, impl_->renderer, impl_->width, impl_->height);
    const auto render_begin = timed ? Clock::now() : Clock::time_point{};
    const std::int64_t pushed_ns = packet.pushed_ns, anchor_shift_ns = packet.anchor_shift_ns;
    std::int64_t credit_ns = 0;
    const bool ok = impl_->renderer.render(
        packet, impl_->options.vsync ? render::PresentMode::VSync : render::PresentMode::Immediate, error,
        eager ? &credit_ns : nullptr);
    const auto render_end = Clock::now();  // also the existing presentation histogram observation
    // Eager start: the phase lock's hold is still push -> the credit (the same reference as with the credit first),
    // less the burst lead the packet came early by (renderer.hold_bias_ns); a frame right after a mode switch is left out.
    if (eager && credit_ns != 0)
        impl_->last_credit_ns = credit_ns;
    if (eager && ok && credit_ns != 0 && pushed_ns != 0 && gekko::phase_lock::enabled() &&
        !impl_->renderer.burst_settling()) {
        static gekko::phase_lock::HoldFilter eager_hold_filter;
        static gekko::phase_lock::TargetFilter eager_target_filter;
        static std::uint32_t eager_epoch = 0;
        if (eager_epoch != impl_->pacing_epoch) {
            eager_epoch = impl_->pacing_epoch;
            eager_hold_filter.reset();
            eager_target_filter.reset();
        }
        const std::int64_t hold = credit_ns - pushed_ns - impl_->renderer.hold_bias_ns();
        eager_hold_filter.add(hold);
        eager_target_filter.add(hold, anchor_shift_ns);
    }
    if (timed) {
        const auto capture_ns = ns(capture_begin, render_begin);
        const auto render_ns = ns(render_begin, render_end);
        if (sample) {
            sample->attempted = true;
            sample->success = ok;
            sample->index = impl_->presented + 1u;
            sample->packet_tick = packet.tick;
            sample->game_ticks = packet.game_ticks;
            sample->packet_wait_ns = impl_->pending_packet_wait_ns;
            sample->capture_ns = capture_ns;
            sample->render_present_ns = render_ns;
            sample->finished_at = render_end;
            sample->has_interval = impl_->have_present;
            if (sample->has_interval)
                sample->interval_ms = std::chrono::duration<double, std::milli>(render_end - impl_->last_present).count();
        }
        impl_->pending_packet_wait_ns = 0;
        std::lock_guard<std::mutex> lock(impl_->telemetry_mutex);
        ++impl_->timing.attempts;
        impl_->timing.presents += ok ? 1u : 0u;
        impl_->timing.failures += ok ? 0u : 1u;
        impl_->timing.capture_ns += capture_ns;
        impl_->timing.render_present_ns += render_ns;
    }
    impl_->renderer.finish_packets(packet.tick);
    render::present_trace::record(render::present_trace::Ev::RenderEnd, packet.tick, ok ? 1u : 0u);
    // C3: its vectors' capacity serves the next frame - handed back only once the renderer no longer reads it
    // (render and finish_packets above are synchronous; the check keeps the contract should either ever not be).
    if (impl_->renderer.reading(packet))
        assert(!"C3: a packet handed back while the renderer still reads it");
    else
        impl_->driver->recycle_packet(std::move(packet));
    if (!ok) {
        ++impl_->render_failures;
        return false;
    }
    ++impl_->presented;
    impl_->note_present(render_end);
    return true;
}

LiftedRender::Telemetry LiftedRender::telemetry() const {
    Telemetry out;
    {
        std::lock_guard<std::mutex> lock(impl_->telemetry_mutex);
        out = impl_->timing;
    }
    const auto q = impl_->frames.stats();
    const auto r = impl_->renderer.stats();
    out.packets = q.pushed;
    out.dropped = q.dropped;
    out.pending = q.pending;
    out.draws_last = r.draws;
    out.textures = r.textures;
    out.meshes = r.meshes;
    out.skipped_draws = r.skipped_draws_total;
    out.resource_failures = r.resources_failed;
    return out;
}

std::string LiftedRender::presentation_json() const {
    const auto q = impl_->frames.stats();
    const auto t = telemetry();
    std::ostringstream out;
    out << std::fixed << std::setprecision(6)
        << "{\"available\": " << (impl_->presented ? "true" : "false")
        << ", \"scope\": \"successful CPU-side render/present returns, not GPU execution or display scanout\""
        << ", \"rendered_frames\": " << impl_->presented
        << ", \"render_failures\": " << impl_->render_failures
        << ", \"intervals\": " << impl_->present_intervals
        << ", \"average_fps\": ";
    if (impl_->present_sum_ms > 0.0) out << impl_->present_intervals * 1000.0 / impl_->present_sum_ms;
    else out << "null";
    out << ", \"interval_ms\": {\"sum\": " << impl_->present_sum_ms
        << ", \"max\": " << impl_->present_max_ms;
    const auto quantile = [&](const char* name, double share) {
        const double upper = impl_->present_quantile(share);
        out << ", \"" << name << "\": ";
        if (impl_->present_intervals == 0u) { out << "null"; return; }
        out << "{\"lower_inclusive\": " << std::max(0.0, upper - 0.1) << ", \"upper_exclusive\": ";
        if (upper > 100.0) out << "null"; else out << upper;
        out << '}';
    };
    quantile("p50", 0.50); quantile("p95", 0.95); quantile("p99", 0.99);
    out << "}, \"queue\": {\"pushed\": " << q.pushed << ", \"popped\": " << q.popped
        << ", \"dropped\": " << q.dropped << ", \"shutdown_discards\": " << q.discarded_on_close
        << ", \"pending\": " << q.pending << "}, \"timing_enabled\": "
        << (impl_->options.collect_timing ? "true" : "false") << ", \"cpu_wall_ms\": ";
    if (impl_->options.collect_timing)
        out << "{\"packet_wait\": " << t.packet_wait_ns / 1.0e6 << ", \"capture\": " << t.capture_ns / 1.0e6
            << ", \"render_present\": " << t.render_present_ns / 1.0e6 << '}';
    else out << "null";
    out << ", \"gpu_frame_ms\": null, \"render_rate_applied\": false}";
    return out.str();
}

std::string LiftedRender::report_text() const {
    std::ostringstream out;
    if (!impl_->driver)
        return "[lifted-render] not started\n";
    const scaffold::RwDriver::Stats s = impl_->driver->stats();
    const game::GameOwners::Stats g = impl_->owners ? impl_->owners->stats() : game::GameOwners::Stats{};
    const render::PacketQueue::Stats q = impl_->frames.stats();
    const render::Renderer::Stats r = impl_->renderer.stats();
    out << "[lifted-render] frames " << s.frames << " presented " << impl_->presented << " headless "
        << impl_->headless_taken << " dropped " << q.dropped << " captures " << impl_->captures_written
        << " render failures " << impl_->render_failures << "\n";
    out << "[lifted-render] queue: pushed " << q.pushed << " popped " << q.popped
        << " dropped " << q.dropped << " shutdown discards " << q.discarded_on_close
        << " pending " << q.pending << "\n";
    out << "[lifted-render] renderer: frames " << r.frames << " last draws " << r.draws << " (skipped "
        << r.skipped_draws << ", all frames " << r.skipped_draws_total << ") pipelines " << r.pipelines << " textures "
        << r.textures << " meshes " << r.meshes << " (skinned " << r.skinned_meshes << ", delta-morphed "
        << r.morphed_meshes << "); releases waiting for queued packets " << r.deferred_releases << " (still waiting "
        << r.pending_releases << ")\n";
    out << "[lifted-render] skipped draws by reason (all frames):";
    for (std::size_t i = 0; i < render::Renderer::kSkipReasons; ++i)
        out << ' ' << render::Renderer::skip_reason_name(static_cast<render::Renderer::SkipReason>(i)) << ' '
            << r.skipped_by_reason[i];
    out << "\n";
    out << "[lifted-render] fade-in: " << impl_->driver->fade_ins() << " models appeared inside the view and faded in ("
        << s.dissolving_draws << " draws mid fade)\n";
    {
        const scaffold::FarObjects::Stats f = impl_->driver->far_objects().stats();
        out << "[lifted-render] far-object proxies: " << f.frames << " frames, candidates " << f.candidates
            << " resolved " << f.resolved << " drawn entries " << f.drawn_entries << " atomics " << f.drawn_atomics
            << ", meshes drawn " << s.far_proxy_draws << "; handovers " << f.handovers << " (" << f.handover_frames
            << " frames, " << f.handover_timeouts << " timed out)\n";
        for (std::size_t i = 0; i < f.class_count; ++i) {
            const scaffold::FarObjects::ClassStats& k = f.classes[i];
            out << "[lifted-render]   " << k.name << (k.verified ? "" : " (unverified)") << ": candidates "
                << k.candidates << " resolved " << k.resolved << " drawn " << k.drawn_entries << "; handovers compared "
                << k.compared << " (" << k.exact << " exact), largest matrix difference " << k.max_error
                << ", doubled " << k.doubled << ", stood in for the game's fade " << k.faded
                << ", hidden until the player is near " << k.hidden << "\n";
        }
    }
    out << "[lifted-render] texture pool: " << r.pool_textures << " render targets, created " << r.pool_created
        << ", released after going unused " << r.pool_released << "; shared memory " << r.pool_memories
        << " blocks, " << (r.pool_memory_bytes >> 20) << " MiB for " << (r.pool_placed_bytes >> 20)
        << " MiB of placed textures\n";
    {
        std::size_t begin = 0;
        while (begin < r.pool_memory_map.size()) {
            const std::size_t end = r.pool_memory_map.find('\n', begin);
            const std::size_t stop = end == std::string::npos ? r.pool_memory_map.size() : end;
            out << "[lifted-render]   shared memory " << r.pool_memory_map.substr(begin, stop - begin) << "\n";
            begin = stop + 1;
        }
    }
    out << "[lifted-render] states: sets " << s.state_sets << " gets " << s.state_gets << "; cameras " << s.cameras
        << " (camera moved within a view " << s.camera_mismatches << ", passes " << s.passes << ", view offsets "
        << s.view_offsets
        << ", handedness " << s.handedness << "); clears " << s.clears << " (" << s.clears_ignored
        << " for the screen passes)\n";
    out << "[lifted-render] sectors " << s.sectors << " drawn meshes " << s.sector_draws << " built "
        << s.sector_meshes_built << " skipped " << s.sectors_skipped << " import failures "
        << s.sector_import_failures << " draws without a view " << s.draws_without_view << " without a texture "
        << s.draws_without_texture << "; corridor lights onto their slots: " << s.light_slot_vertices_moved
        << " vertices moved, " << s.light_slots_added << " lights added\n";
    out << "[lifted-render] models: draws " << s.model_draws << " built " << s.models_built << " rebuilt "
        << s.models_rebuilt << " failed " << s.model_failures << " extra passes " << s.extra_passes
        << " released " << s.geometry_releases << "; skinned draws " << s.skinned_draws << " (unsupported skins "
        << s.skins_unsupported << ")\n";
    out << "[lifted-render] im3d: draws " << s.im3d_draws << " skipped " << s.im3d_skipped << " (last frame "
        << r.immediates << " immediate draws in all)\n";
    out << "[lifted-render] boundary: framing "
        << (impl_->driver->framing() == render::Framing::GameWindow ? "game window" : "square pixels")
        << "; world-to-screen projections " << s.projections << " (no camera " << s.projections_without_camera
        << ", on the eye's plane " << s.projections_degenerate << ", window differs from the renderer camera's "
        << s.projection_window_mismatches << "); screen tests on the displayed screen " << s.screen_edge_reads
        << ", on-screen points " << s.displayed_screen_points << " (on the displayed screen "
        << s.displayed_screen_points_on << "; verdict differs from the game's 4:3 window: on only when displayed "
        << s.displayed_screen_points_gained << ", only in the game's window " << s.displayed_screen_points_lost
        << "); lock-on marks " << g.lockon_marks << " (drawn " << g.lockon_marks_drawn << ", hidden "
        << g.lockon_marks_hidden << "); renderer camera windows set " << s.camera_windows << "\n";
    out << "[lifted-render] lens flare: builds " << g.flare_builds << " (sun in front " << g.flare_in_front
        << "; ghosts shown " << g.flare_ghosts_shown << ", of them past the raster's own square " << g.flare_beyond_raster
        << "; glow shown " << g.flare_glow_shown << "; declined: unknown system / table " << g.flare_unknown_system
        << ")\n";
    out << "[lifted-render] im2d: canvas draws " << s.im2d_draws << " (left " << s.im2d_anchors[0] << " centre "
        << s.im2d_anchors[1] << " right " << s.im2d_anchors[2] << " fill " << s.im2d_anchors[3] << " projected "
        << s.im2d_anchors[4] << "; off canvas "
        << s.im2d_off_canvas << "; fog armed " << s.im2d_fog_armed << ") outside views " << s.im2d_outside_views
        << " effect composites " << s.im2d_effect_composites << " refused " << s.im2d_refused << " unreadable "
        << s.im2d_unreadable << "\n";
    out << "[lifted-render] screen passes: snapshots " << s.screen_snapshots << " distortion draws "
        << s.distortion_draws << "; history saves " << s.trail_saves << " trail draws " << s.trail_draws
        << "; screen multiplies " << s.screen_multiplies << "; screen covers widened " << s.screen_covers_widened
        << "; not described " << s.screen_pass_skipped
        << "; renderer last frame: snapshots " << r.snapshots << " history saves " << r.history_saves
        << ", glare composites folded into the resolve (all frames) " << r.glare_composites_folded << " (tile rows still blended "
        << r.folded_glare_spans << ")"
        << ", views under an overlay resolved into it (all frames) " << r.views_folded << " of " << r.views_over_views
        << ", image misses (all frames) " << r.image_misses << "\n";
    const auto span = [](const std::uint64_t (&frames)[2]) {
        return frames[0] > frames[1] ? std::string("none")
                                     : std::to_string(frames[0]) + ".." + std::to_string(frames[1]);
    };
    out << "[lifted-render] screen passes drawn in frames: distortion " << span(s.distortion_frames) << ", trail "
        << span(s.trail_frames) << ", multiply " << span(s.multiply_frames) << "\n";
    out << "[lifted-render] screen copies outside a camera bracket " << s.screen_copies_unbracketed
        << "; screen passes not described: no screen bracket " << s.screen_skips[0] << ", no snapshot of the view "
        << s.screen_skips[1] << ", unreadable " << s.screen_skips[2] << ", no pass " << s.screen_skips[3]
        << ", switched off " << s.screen_skips[4] << ", a copy covering no view " << s.screen_skips[5]
        << "; in frames " << span(s.screen_skip_frames) << "; by site:";
    for (const auto& [key, count] : impl_->driver->screen_skip_sites())
        out << ' ' << std::hex << (static_cast<std::uint32_t>(key) - 4u) << std::dec << '/' << (key >> 32) << " x"
            << count;
    out << "\n";
    out << "[lifted-render] movies: frame blits " << s.movie_draws << " pictures " << s.movie_pictures
        << " in frames " << span(s.movie_frames) << "; video-mode switches shown black " << s.mode_switch_blanks
        << "; in-engine event frames shown 4:3 " << s.event_framed << "\n";
    const auto sites = [&](const char* what, const std::map<std::uint32_t, std::uint64_t>& counts) {
        out << "[lifted-render] " << what << " not drawn yet, by site:";
        for (const auto& [lr, count] : counts)
            out << ' ' << std::hex << (lr - 4u) << std::dec << " x" << count;
        out << "\n";
    };
    sites("screen-pass composites", impl_->driver->effect_composite_sites());
    sites("camera-texture copies", impl_->driver->copy_sites());
    out << "[lifted-render] glare: windows " << s.glare_windows << " composites " << s.glares << " (last frame "
        << r.glares << ") mismatches " << s.glare_mismatches << " unclosed " << s.glares_unclosed << "\n";
    out << "[lifted-render] particles: batches " << s.particle_batches << " particles " << s.particles
        << " unsupported " << s.particles_unsupported << " skipped " << s.particles_skipped << "; last frame "
        << r.particle_batches << " batches, " << r.particles << " particles\n";
    out << "[lifted-render] skinning: last frame " << r.skin_jobs << " jobs, " << r.skinned_vertices
        << " vertices; skinned meshes " << r.skinned_meshes << "; cache overflows " << r.skin_overflows
        << "; not drawn: no skin " << s.skins_without_skin << " no hierarchy " << s.skins_without_hierarchy
        << " no matrices " << s.skins_without_matrices << " bad bones " << s.skins_bad_bones
        << " (last: hierarchy flags 0x" << std::hex << s.last_hierarchy_flags << std::dec << " nodes "
        << s.last_hierarchy_nodes << " skin bones " << s.last_skin_bones << "); bone frames " << s.bone_frames
        << " (" << s.dirty_bone_frames << " in a dirty hierarchy)\n";
    out << "[lifted-render] deforming meshes: skins with weights not summing to 1 " << s.skins_unnormalised << " ("
        << s.skin_vertices_unnormalised << " vertices); meshes rebuilt after a draw of the same frame "
        << s.meshes_replaced_in_frame << "; draws with several morph targets " << s.morph_target_draws
        << "; skins posed from hierarchy matrices " << s.skins_from_matrices << "\n";
    out << "[lifted-render] textures built " << s.textures_built << " (" << s.textures_overridden
        << " drawn from texture overrides) failed " << s.texture_failures << " released "
        << s.textures_released << "; meshes released " << s.meshes_released << "; atomics " << s.atomics
        << " im2d " << s.im2d_primitives << " im3d " << s.im3d_primitives << " copies " << s.copies << "\n";
    // The resource worker (render/resource_worker.h): what the game thread no longer does itself.
    const render::ResourceWorker::Stats w = impl_->renderer.worker_stats();
    out << "[lifted-render] resource worker: jobs " << w.submitted << " (worker " << w.run_by_worker
        << ", run by waiting threads " << w.run_by_waiters << "), peak queued " << w.peak_queued << ", busy "
        << static_cast<double>(w.busy_ns) / 1.0e6 << " ms; waits " << w.waits << " ("
        << static_cast<double>(w.wait_ns) / 1.0e6 << " ms); asked textures " << r.textures_requested << " meshes "
        << r.meshes_requested << ", failed " << r.resources_failed << "; draws recorded while their resource was made "
        << r.resolve_waits << "\n";
    {
        const Impl& i = *impl_;
        char row[320];
        std::snprintf(row, sizeof(row),
                      "[lifted-render] present intervals: %llu, p50 %.1f p95 %.1f p99 %.1f max %.1f ms; longer than "
                      "20 ms %llu, 34 ms %llu, 50 ms %llu\n",
                      static_cast<unsigned long long>(i.present_intervals), i.present_quantile(0.50),
                      i.present_quantile(0.95), i.present_quantile(0.99), i.present_max_ms,
                      static_cast<unsigned long long>(i.presents_longer_than(20.0)),
                      static_cast<unsigned long long>(i.presents_longer_than(34.0)),
                      static_cast<unsigned long long>(i.presents_longer_than(50.0)));
        out << row;
    }
    return out.str();
}

}  // namespace gekko
